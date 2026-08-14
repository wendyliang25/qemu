/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Virtio Accelerator Device
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/qdev-properties.h"
#include "system/iothread.h"
#include "block/aio-wait.h"

#include <vaccel.h>

/*
 * Control + cursor queues, as created by virtio_gpu_base_device_realize(). Must
 * match the number of virtqueues the base device creates; start/stop_ioeventfd
 * iterate [0, VIRTIO_ACCEL_NVQS).
 */
#define VIRTIO_ACCEL_NVQS 2

struct accel_resource {
    struct virtio_gpu_simple_resource base;
    MemoryRegion *region;  /* For host memory mapping */
};

static inline struct accel_resource *
to_accel_res(struct virtio_gpu_simple_resource *res)
{
    return container_of(res, struct accel_resource, base);
}

static struct accel_resource *
accel_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return NULL;
    }

    return to_accel_res(res);
}

static int accel_get_drm_fd(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    char name[64];

    snprintf(name, sizeof(name), "/dev/accel/%s", accel->accel_node);
    return open(name, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
}

/*
 * Drain fence completions queued by accel_write_context_fence(). Runs on
 * accel->ctx (the iothread when configured, else the main loop), which is the
 * single owner of g->fenceq / g->inflight; the ctrl path runs on the same
 * context, so no lock is needed. virtqueue_push()/virtio_notify() (via
 * virtio_gpu_ctrl_response_nodata) are RCU/atomic-safe off the BQL.
 */
static void accel_context_fence_bh(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    QSLIST_HEAD(, accel_context_fence) fenceq;
    struct accel_context_fence *f;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QSLIST_MOVE_ATOMIC(&fenceq, &accel->async_fenceq);

    while (!QSLIST_EMPTY(&fenceq)) {
        f = QSLIST_FIRST(&fenceq);
        QSLIST_REMOVE_HEAD(&fenceq, next);

        QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
            if (cmd->cmd_hdr.fence_id != f->fence_id) {
                continue;
            }
            if (cmd->cmd_hdr.ctx_id != f->ctx_id) {
                continue;
            }

            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
        }

        g_free(f);
    }
}

/*
 * Invoked on the vaccel fence-polling thread (not a QEMU thread). Never take the
 * BQL here: ctx teardown joins this thread while holding the BQL, so doing so
 * deadlocks. Enqueue lock-free and kick the BH, which does the real work on the
 * main loop under the BQL.
 */
static void accel_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t ring_idx, uint64_t fence_id)
{
    VirtIOGPU *g = opaque;
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    struct accel_context_fence *f = g_new(struct accel_context_fence, 1);

    f->ctx_id = ctx_id;
    f->ring_idx = ring_idx;
    f->fence_id = fence_id;

    QSLIST_INSERT_HEAD_ATOMIC(&accel->async_fenceq, f, next);
    qemu_bh_schedule(accel->fence_bh);
}

static struct vaccel_callbacks vaccel_cbs = {
    .get_device_fd  = accel_get_drm_fd,
    .write_context_fence = accel_write_context_fence,
};

/*
 * Control-queue bottom half, bound to accel->ctx (the iothread when configured).
 * Replaces the base virtio_gpu_ctrl_bh. Command processing runs without the BQL;
 * the handlers that manipulate memory regions / DMA / g->reslist bounce those
 * sections to the main loop (see accel_run_on_main).
 */
static void virtio_accel_ctrl_bh(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(g);

    vgc->handle_ctrl(VIRTIO_DEVICE(g), g->ctrl_vq);
}

static void virtio_accel_realize(DeviceState *qdev, Error **errp)
{
    ERRP_GUARD();
    VirtIOGPUBase *bdev = VIRTIO_GPU_BASE(qdev);
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    int ret;

    bdev->virtio_config.num_capsets = 1;
    bdev->conf.max_outputs = 0;
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_ACCEL_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_BLOB_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_DMABUF_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED);

    /*
     * Resolve the AioContext used for command and fence processing. With a
     * dedicated iothread this is off the main loop; otherwise it is the main
     * loop and behaviour is unchanged.
     */
    if (accel->iothread) {
        accel->ctx = iothread_get_aio_context(accel->iothread);
        object_ref(OBJECT(accel->iothread));
    } else {
        accel->ctx = qemu_get_aio_context();
    }

    /* Set up the fence-completion BH before any context (hence polling thread)
     * can exist and call accel_write_context_fence(). */
    QSLIST_INIT(&accel->async_fenceq);
    accel->fence_bh = aio_bh_new(accel->ctx, accel_context_fence_bh, g);

    ret = vaccel_create(g, VIRACCEL_CAPSET_ID_AMDXDNA, &vaccel_cbs);
    if (ret) {
        error_setg(errp, "Init specified accel device failed");
        goto err;
    }

    virtio_gpu_device_realize(qdev, errp);
    if (*errp) {
        vaccel_destroy(g);
        goto err;
    }

    /*
     * With an iothread, move the base control BH (created on the main loop by
     * virtio_gpu_device_realize()) onto the iothread's AioContext, so the BH and
     * command processing run off the main loop.
     *
     * The base BH is guarded by the transport's mem_reentrancy_guard, but that
     * guard is a per-device flag shared with MMIO dispatch and is not
     * thread-aware: while a guarded BH runs it sets engaged_in_io, and a
     * concurrent guest notify MMIO to the same device (on a vCPU/main thread)
     * would then be blocked and dropped, wedging the queue. Since this BH runs
     * on a dedicated iothread concurrently with guest MMIO, it must be
     * unguarded (as virtio dataplane devices are on their iothread path).
     */
    if (accel->iothread) {
        qemu_bh_delete(g->ctrl_bh);
        g->ctrl_bh = aio_bh_new(accel->ctx, virtio_accel_ctrl_bh, g);
    }
    return;

err:
    qemu_bh_delete(accel->fence_bh);
    accel->fence_bh = NULL;
    if (accel->iothread) {
        object_unref(OBJECT(accel->iothread));
    }
}

static void virtio_accel_unrealize(DeviceState *qdev)
{
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    struct accel_context_fence *f;

    /*
     * Do not assume the guest tore its contexts down first (hot-unplug, abrupt
     * VM shutdown and guest crashes all reach here with contexts still live).
     * vaccel_destroy() destroys every context and joins their fence-polling
     * threads, so once it returns no producer of async_fenceq remains and the
     * BH can be deleted safely. This is deadlock-free under the BQL only because
     * the fence callback no longer takes the BQL (see accel_write_context_fence).
     */
    vaccel_destroy(g);

    if (accel->fence_bh) {
        qemu_bh_delete(accel->fence_bh);
        accel->fence_bh = NULL;
    }
    while ((f = QSLIST_FIRST(&accel->async_fenceq))) {
        QSLIST_REMOVE_HEAD(&accel->async_fenceq, next);
        g_free(f);
    }

    if (accel->iothread) {
        object_unref(OBJECT(accel->iothread));
    }
}

static void
accel_cmd_get_capset_info(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VIRTIO_GPU_FILL_CMD(info);

    memset(&resp, 0, sizeof(resp));

    /*
     * Since NPU accelerator is GPU DRM device, we use VIRTIO_GPU_CAPSET_DRM
     * capset ID for NPU eventhough NPU doesn't have rendering capability.
     */
    resp.capset_id = VIRTIO_GPU_CAPSET_DRM;
    int ret = vaccel_get_capset_info(g, &resp.capset_max_version,
                                 &resp.capset_max_size);
    if (ret) {
        error_report("virtio-accel: Failed to get capset info: %s", strerror(-ret));
    }
    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void
accel_cmd_get_capset(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t max_ver, max_size;

    VIRTIO_GPU_FILL_CMD(gc);

    vaccel_get_capset_info(g, &max_ver, &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    vaccel_fill_capset(g, max_size, (void *)resp->capset_data);
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

static void
accel_cmd_ctx_create(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;
    VirtIOAccel *accel= VIRTIO_ACCEL(g);

    VIRTIO_GPU_FILL_CMD(cc);

    trace_virtio_accel_cmd_ctx_create(cc.hdr.ctx_id, cc.nlen, cc.debug_name, accel->accel_node);

    vaccel_create_ctx_with_flags(g, cc.hdr.ctx_id, 0, cc.nlen, cc.debug_name);
}

static void
accel_cmd_ctx_destroy(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);

    trace_virtio_accel_cmd_ctx_destroy(cd.hdr.ctx_id);

    vaccel_destroy_ctx(g, cd.hdr.ctx_id);
}

static void
accel_cmd_resource_create_blob(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct vaccel_create_resource_blob_args vaccel_args = { 0 };
    struct virtio_gpu_resource_create_blob cblob;
    struct virtio_gpu_simple_resource *res;
    int ret;

    if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: blob not enabled\n", __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);

    trace_virtio_accel_create_blob(cblob.resource_id, cblob.blob_mem,
                                   cblob.nr_entries, cblob.size,
                                   cblob.hdr.ctx_id);

    res = virtio_gpu_find_resource(g, cblob.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = &((struct accel_resource *)g_new0(struct accel_resource, 1))->base;
    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    if (cblob.blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST) {
         ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                             cmd, &res->addrs,
                                            &res->iov, &res->iov_cnt);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: create_mapping_iov failed: res=%d, "
                    "nr_entries=%d, size=%ld, ret=%d\n",
                    __func__, cblob.resource_id, cblob.nr_entries,
                    (long)cblob.size, ret);
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            g_free(res);
            return;
        }
    }

    vaccel_args.res_handle = cblob.resource_id;
    vaccel_args.ctx_id = cblob.hdr.ctx_id;
    vaccel_args.blob_mem = cblob.blob_mem;
    vaccel_args.blob_id = cblob.blob_id;
    vaccel_args.blob_flags = cblob.blob_flags;
    vaccel_args.size = cblob.size;
    vaccel_args.iovecs = res->iov;
    vaccel_args.num_iovs = res->iov_cnt;
    trace_virtio_accel_cmd_resource_create_blob(cblob.resource_id, cblob.hdr.ctx_id, cblob.blob_id, cblob.blob_flags, cblob.blob_mem, cblob.size);


    ret = vaccel_create_resource_blob(g, &vaccel_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        goto cleanup_mapping;
    }

    trace_virtio_accel_cmd_resource_create_blob(cblob.resource_id, cblob.hdr.ctx_id, cblob.blob_id, cblob.blob_flags, cblob.blob_mem, cblob.size);

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
    res = NULL;

    return;

cleanup_mapping:
    virtio_gpu_cleanup_mapping(g, res);
    g_free(res);
    return;
}

/*
 * Tear down a blob's host mapping. Remove the MemoryRegion from the guest
 * address space first: del_subregion commits the memory transaction and drops
 * the KVM memslot, so a concurrent vCPU can no longer reach the backing before
 * vaccel_resource_unmap() munmaps it. Returns the vaccel_resource_unmap() result.
 */
static int accel_resource_unmap_region(VirtIOGPU *g, struct accel_resource *vres)
{
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr = vres->region;

    memory_region_set_enabled(mr, false);
    memory_region_del_subregion(&b->hostmem, mr);
    object_unparent(OBJECT(mr));
    vres->region = NULL;

    return vaccel_resource_unmap(g, vres->base.resource_id);
}

static void accel_cmd_resource_map_blob(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_map_blob mblob;
    struct accel_resource *vres;
    MemoryRegion *mr;
    int ret;
    void *data;
    uint64_t size;
    struct virtio_gpu_resp_map_info resp;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    if (mblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n", __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (!virtio_gpu_hostmem_enabled(b->conf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: hostmem disabled\n", __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    vres = accel_find_resource(g, mblob.resource_id);
    if (!vres) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    if (vres->region) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already mapped %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = vaccel_resource_map(g, vres->base.resource_id, &data, &size);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource map error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    /*
     * mblob.offset is guest-controlled: reject a mapping that would fall outside
     * the hostmem window (overflow-safe) before installing the subregion.
     */
    if (size > b->conf.hostmem || mblob.offset > b->conf.hostmem - size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: map offset 0x%" PRIx64 " size 0x%" PRIx64
                      " exceeds hostmem window 0x%" PRIx64 "\n",
                      __func__, mblob.offset, size, b->conf.hostmem);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        vaccel_resource_unmap(g, vres->base.resource_id);
        return;
    }

    vres->region = MEMORY_REGION(object_new(TYPE_MEMORY_REGION));
    mr = vres->region;

    memory_region_init_ram_ptr(mr, OBJECT(mr), "blob", size, data);
    memory_region_add_subregion(&b->hostmem, mblob.offset, mr);
    memory_region_set_enabled(mr, true);

    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    vaccel_resource_get_map_info(g, mblob.resource_id, &resp.map_info);
    trace_virtio_accel_cmd_resource_map_blob(mblob.resource_id, mblob.offset, size);
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void accel_cmd_resource_unmap_blob(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd,
                                          bool *cmd_suspended)
{
    struct accel_resource *vres;
    struct virtio_gpu_resource_unmap_blob ublob;
    int ret;

    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    if (ublob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n", __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    vres = accel_find_resource(g, ublob.resource_id);
    if (!vres) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n", __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (!vres->region) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already unmapped %d\n", __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    trace_virtio_accel_cmd_resource_unmap_blob(vres->base.resource_id);
    ret = accel_resource_unmap_region(g, vres);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource unmap error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
}

static void accel_cmd_resource_unref(VirtIOGPU *g,
                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_simple_resource *res;
    struct accel_resource *vres;
    struct iovec *res_iovs = NULL;
    uint32_t num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(unref);


    res = virtio_gpu_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    /*
     * Do not trust the guest to unmap before unref: a still-mapped blob leaves
     * a MemoryRegion in the hostmem window backed by memory that the destroy
     * below frees, which the guest could then reach through its BAR. Tear the
     * mapping down first.
     */
    vres = to_accel_res(res);
    if (vres->region) {
        accel_resource_unmap_region(g, vres);
    }

    vaccel_detach_resource_blob(g, unref.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }

    vaccel_destroy_resource_blob(g, unref.resource_id);
    QTAILQ_REMOVE(&g->reslist, res, next);
    g_free(res);
}

static void accel_cmd_submit(VirtIOGPU *g,
                             struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_submit cs;
    void *buf;
    size_t s;
    size_t available;
    int ret;

    VIRTIO_GPU_FILL_CMD(cs);

    /*
     * cs.size is guest-controlled; bound it by the bytes actually present in the
     * command buffer (after the header) so we don't attempt a huge g_malloc().
     */
    available = iov_size(cmd->elem.out_sg, cmd->elem.out_num);
    if (available < sizeof(cs) || cs.size > available - sizeof(cs)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: submit size %u exceeds buffer\n",
                      __func__, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    buf = g_malloc(cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    if (s != cs.size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: size mismatch (%zd/%d)",
                      __func__, s, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        goto out;
    }

    ret = vaccel_submit_ccmd(g, cs.hdr.ctx_id, buf, cs.size);
    if (ret)
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;

out:
    g_free(buf);
}

typedef void (*accel_cmd_fn)(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd);

struct accel_bounce {
    VirtIOGPU *g;
    struct virtio_gpu_ctrl_command *cmd;
    accel_cmd_fn fn;
    QemuEvent done;
};

static void accel_bounce_bh(void *opaque)
{
    struct accel_bounce *b = opaque;

    /* Main-loop BHs run with the BQL already held; do not re-acquire it. */
    b->fn(b->g, b->cmd);
    qemu_event_set(&b->done);
}

/*
 * Run a command handler that manipulates memory regions / DMA / g->reslist on
 * the main loop under the BQL (those operations assert bql_locked()). When
 * already on the main-loop AioContext (no iothread configured) call directly.
 * From the iothread, schedule the body on the main loop and wait: the BH runs
 * under the BQL that the main loop already holds. This is the sanctioned
 * iothread->main-loop wait direction (block/aio-wait.h), and it is deadlock-free
 * against stop_ioeventfd because the main thread services this BH from within
 * its own aio_poll() while it drains the iothread. cmd->error / cmd->finished
 * written in the BH are published by qemu_event_wait().
 */
static void accel_run_on_main(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd,
                              accel_cmd_fn fn)
{
    struct accel_bounce b;

    if (qemu_get_current_aio_context() == qemu_get_aio_context()) {
        fn(g, cmd);
        return;
    }

    b.g = g;
    b.cmd = cmd;
    b.fn = fn;
    qemu_event_init(&b.done, false);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), accel_bounce_bh, &b);
    qemu_event_wait(&b.done);
    qemu_event_destroy(&b.done);
}

/* accel_cmd_fn adapter: accel never sets cmd_suspended, so ignore it. */
static void accel_cmd_resource_unmap_blob_fn(VirtIOGPU *g,
                                             struct virtio_gpu_ctrl_command *cmd)
{
    bool cmd_suspended = false;

    accel_cmd_resource_unmap_blob(g, cmd, &cmd_suspended);
}

static void
virtio_accel_process_cmd(VirtIOGPU *g,
                         struct virtio_gpu_ctrl_command *cmd)
{
    bool cmd_suspended = false;

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

    trace_virtio_accel_cmd(cmd->cmd_hdr.type, cmd->cmd_hdr.ctx_id, cmd->cmd_hdr.flags);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        accel_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        accel_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_CREATE:
        accel_cmd_ctx_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        accel_cmd_ctx_destroy(g, cmd);
        break;
    /*
     * The RESOURCE_* commands manipulate memory regions / DMA mappings /
     * g->reslist, which require the BQL and main-loop ownership. Bounce their
     * bodies to the main loop; the surrounding cmdq/fenceq bookkeeping stays on
     * accel->ctx.
     */
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        accel_run_on_main(g, cmd, accel_cmd_resource_create_blob);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        accel_run_on_main(g, cmd, accel_cmd_resource_map_blob);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        accel_run_on_main(g, cmd, accel_cmd_resource_unmap_blob_fn);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        accel_run_on_main(g, cmd, accel_cmd_resource_unref);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        accel_cmd_submit(g, cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd->finished || cmd_suspended) {
        return;
    }

    if (cmd->error) {
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }

    if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        int ret;

        ret = vaccel_submit_fence(g, cmd->cmd_hdr.ctx_id, 0,
                                  cmd->cmd_hdr.ring_idx, cmd->cmd_hdr.fence_id);
        if (ret) {
            /*
             * On success the command is left unfinished so the base cmdq loop
             * queues it on g->fenceq and the fence-polling thread completes it
             * later. If submission fails no completion is ever produced, so
             * respond now (which sets cmd->finished) to avoid stranding the
             * command on fenceq and leaking g->inflight.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: fence submit failed (ctx %u, ring %u, fence %"
                          PRIu64 "): %s\n", __func__, cmd->cmd_hdr.ctx_id,
                          cmd->cmd_hdr.ring_idx, cmd->cmd_hdr.fence_id,
                          strerror(-ret));
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_ERR_UNSPEC);
        }
        return;
    }
    virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
}

/*
 * Max commands processed per ctrl_bh run before yielding, when running on the
 * shared main-loop AioContext (no iothread configured). There, draining the
 * whole queue in one BH can starve other devices' BHs (notably virtio-gpu
 * rendering), so after this many commands we reschedule ctrl_bh and return,
 * letting the main loop service other work before we resume draining. With a
 * dedicated iothread the ctrl BH owns its own AioContext and there is nothing to
 * yield to, so no budget is applied.
 */
#define VIRTIO_ACCEL_CTRL_BUDGET 16

static void virtio_accel_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    struct virtio_gpu_ctrl_command *cmd;
    unsigned int processed = 0;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        virtio_gpu_process_cmdq(g);

        if (!accel->iothread && ++processed >= VIRTIO_ACCEL_CTRL_BUDGET) {
            /*
             * Main-loop only: reschedule ourselves so pending BHs (e.g.
             * virtio-gpu's ctrl_bh) get to run, then resume draining the queue
             * on the next ctrl_bh invocation.
             */
            qemu_bh_schedule(g->ctrl_bh);
            break;
        }
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }
}

/* Detach a queue's host notifier; runs in that queue's AioContext. */
static void virtio_accel_ioeventfd_stop_vq_bh(void *opaque)
{
    VirtQueue *vq = opaque;
    EventNotifier *host_notifier = virtio_queue_get_host_notifier(vq);

    virtio_queue_aio_detach_host_notifier(vq, qemu_get_current_aio_context());
    /* Drain any pending notification the poll callback may have missed. */
    virtio_queue_host_notifier_read(host_notifier);
}

/*
 * Start ioeventfd-based virtqueue processing. The control queue's host notifier
 * is attached to accel->ctx (the iothread when configured, else the main loop),
 * so its processing runs off the main loop; the cursor queue stays on the main
 * loop. With no iothread accel->ctx is the main loop, so this matches the
 * default host-notifier behaviour. Context: BQL held.
 */
static int virtio_accel_start_ioeventfd(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    BusState *qbus = qdev_get_parent_bus(DEVICE(vdev));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int r, i, j;

    if (accel->ioeventfd_started) {
        return 0;
    }

    r = k->set_guest_notifiers(qbus->parent, VIRTIO_ACCEL_NVQS, true);
    if (r != 0) {
        error_report("virtio-accel: failed to set guest notifiers (%d), "
                     "ensure the accelerator supports irqfd", r);
        return r;
    }

    /* Batch host-notifier changes into one memory transaction. */
    memory_region_transaction_begin();
    for (i = 0; i < VIRTIO_ACCEL_NVQS; i++) {
        r = virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), i, true);
        if (r != 0) {
            j = i;
            while (i--) {
                virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), i, false);
            }
            memory_region_transaction_commit();
            while (j--) {
                virtio_bus_cleanup_host_notifier(VIRTIO_BUS(qbus), j);
            }
            k->set_guest_notifiers(qbus->parent, VIRTIO_ACCEL_NVQS, false);
            return r;
        }
    }
    memory_region_transaction_commit();

    accel->ioeventfd_started = true;

    virtio_queue_aio_attach_host_notifier(g->ctrl_vq, accel->ctx);
    virtio_queue_aio_attach_host_notifier(g->cursor_vq, qemu_get_aio_context());
    return 0;
}

/* Context: BQL held. */
static void virtio_accel_stop_ioeventfd(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);
    BusState *qbus = qdev_get_parent_bus(DEVICE(vdev));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int i;

    if (!accel->ioeventfd_started) {
        return;
    }

    /* Detach each queue's host notifier from within its own AioContext. */
    aio_wait_bh_oneshot(accel->ctx, virtio_accel_ioeventfd_stop_vq_bh,
                        g->ctrl_vq);
    aio_wait_bh_oneshot(qemu_get_aio_context(), virtio_accel_ioeventfd_stop_vq_bh,
                        g->cursor_vq);

    memory_region_transaction_begin();
    for (i = 0; i < VIRTIO_ACCEL_NVQS; i++) {
        virtio_bus_set_host_notifier(VIRTIO_BUS(qbus), i, false);
    }
    memory_region_transaction_commit();

    for (i = 0; i < VIRTIO_ACCEL_NVQS; i++) {
        virtio_bus_cleanup_host_notifier(VIRTIO_BUS(qbus), i);
    }

    accel->ioeventfd_started = false;
    k->set_guest_notifiers(qbus->parent, VIRTIO_ACCEL_NVQS, false);
}

/*
 * Drain g->cmdq / g->fenceq / g->inflight. Mirrors the inline drain in base
 * virtio_gpu_reset(); run on accel->ctx (see virtio_accel_reset) so the queues,
 * which are owned by the iothread, are not touched concurrently.
 */
static void virtio_accel_reset_drain_bh(void *opaque)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd;

    while (!QTAILQ_EMPTY(&g->cmdq)) {
        cmd = QTAILQ_FIRST(&g->cmdq);
        QTAILQ_REMOVE(&g->cmdq, cmd, next);
        g_free(cmd);
    }
    while (!QTAILQ_EMPTY(&g->fenceq)) {
        cmd = QTAILQ_FIRST(&g->fenceq);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g->inflight--;
        g_free(cmd);
    }
}

/*
 * Base virtio_gpu_reset() drains cmdq/fenceq inline on the calling (main/vCPU)
 * thread, which would race the iothread that owns them. The bus stops ioeventfd
 * before reset, so the ctrl notifier is already detached; drain the queues on
 * accel->ctx first, then let the base reset run (it now finds them empty and
 * still destroys resources on the main loop under the BQL).
 */
static void virtio_accel_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOAccel *accel = VIRTIO_ACCEL(g);

    if (accel->iothread) {
        aio_wait_bh_oneshot(accel->ctx, virtio_accel_reset_drain_bh, g);
    }
    virtio_gpu_reset(vdev);
}

static const Property virtio_accel_properties[] = {
    DEFINE_PROP_STRING("accel-node", VirtIOAccel,
                       accel_node),
    DEFINE_PROP_LINK("iothread", VirtIOAccel, iothread, TYPE_IOTHREAD,
                     IOThread *),
};

static void virtio_accel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOGPUClass *vgc = VIRTIO_GPU_CLASS(klass);

    vgc->handle_ctrl = virtio_accel_handle_ctrl;
    vgc->process_cmd = virtio_accel_process_cmd;
    vdc->realize = virtio_accel_realize;
    vdc->unrealize = virtio_accel_unrealize;
    vdc->reset = virtio_accel_reset;
    vdc->start_ioeventfd = virtio_accel_start_ioeventfd;
    vdc->stop_ioeventfd = virtio_accel_stop_ioeventfd;
    device_class_set_props(dc, virtio_accel_properties);
}

static const TypeInfo virtio_accel_info[] = {
    {
        .name = TYPE_VIRTIO_ACCEL,
        .parent = TYPE_VIRTIO_GPU,
        .instance_size = sizeof(VirtIOAccel),
        .class_init = virtio_accel_class_init,
    },
};

DEFINE_TYPES(virtio_accel_info)

module_obj(TYPE_VIRTIO_ACCEL);
module_kconfig(VIRTIO_GPU);
module_dep("hw-display-virtio-gpu");
