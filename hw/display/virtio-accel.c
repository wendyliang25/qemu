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
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"

#include <vaccel.h>

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

static void accel_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t ring_idx, uint64_t fence_id)
{
    struct virtio_gpu_ctrl_command *cmd, *tmp;
    VirtIOGPU *g = opaque;

    bool locked_iothread = false;

    /* Only lock if we're not in the iothread and iothread mutex isn't already locked */
    if (!qemu_in_main_thread() && !bql_locked()) {
        bql_lock();
        locked_iothread = true;
    }

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        if (cmd->cmd_hdr.fence_id != fence_id) {
            continue;
        }
        if (cmd->cmd_hdr.ctx_id != ctx_id) {
            continue;
        }

        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
    }

    if (locked_iothread) {
        bql_unlock();
    }
}

static struct vaccel_callbacks vaccel_cbs = {
    .get_device_fd  = accel_get_drm_fd,
    .write_context_fence = accel_write_context_fence,
};

static void virtio_accel_realize(DeviceState *qdev, Error **errp)
{
    VirtIOGPUBase *bdev = VIRTIO_GPU_BASE(qdev);
    VirtIOGPU *g = VIRTIO_GPU(qdev);
    int ret;

    bdev->virtio_config.num_capsets = 1;
    bdev->conf.max_outputs = 0;
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_ACCEL_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_BLOB_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_DMABUF_ENABLED);
    bdev->conf.flags |= (1 << VIRTIO_GPU_FLAG_CONTEXT_INIT_ENABLED);

    ret = vaccel_create(g, VIRACCEL_CAPSET_ID_AMDXDNA, &vaccel_cbs);
    if (ret) {
        error_setg(errp, "Init specified accel device failed");
        return;
    }

    virtio_gpu_device_realize(qdev, errp);
}

static void virtio_accel_unrealize(DeviceState *qdev)
{
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

    vres->region = MEMORY_REGION(object_new(TYPE_MEMORY_REGION));
    if (!vres->region) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to create memory region\n", __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
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
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr;
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
    ret = vaccel_resource_unmap(g, vres->base.resource_id);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource unmap error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    mr = vres->region;
    memory_region_set_enabled(mr, false);
    memory_region_del_subregion(&b->hostmem, mr);
    object_unparent(OBJECT(mr));

    vres->region = NULL;
}

static void accel_cmd_resource_unref(VirtIOGPU *g,
                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_simple_resource *res;
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
    int ret;

    VIRTIO_GPU_FILL_CMD(cs);

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
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        accel_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        accel_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        accel_cmd_resource_unmap_blob(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        accel_cmd_resource_unref(g, cmd);
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

    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }
}

static void virtio_accel_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    struct virtio_gpu_ctrl_command *cmd;

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

        if (!cmd->finished && (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
            (void)vaccel_submit_fence(g, cmd->cmd_hdr.ctx_id, 0, cmd->cmd_hdr.ring_idx, cmd->cmd_hdr.fence_id);
        }
        cmd = virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command));
    }
}

static const Property virtio_accel_properties[] = {
    DEFINE_PROP_STRING("accel-node", VirtIOAccel,
                       accel_node),
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
