/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Virtio Accelerator Device
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"

#include <virglrenderer.h>

struct accel_virgl_resource {
    struct virtio_gpu_simple_resource base;
    MemoryRegion mr;
};

static inline struct accel_virgl_resource *
to_accel_res(struct virtio_gpu_simple_resource *res)
{
    return container_of(res, struct accel_virgl_resource, base);
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

    BQL_LOCK_GUARD();
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
}

static struct virgl_renderer_callbacks virgl_cbs = {
    .version     = VIRGL_RENDERER_CALLBACKS_VERSION,
    .get_drm_fd  = accel_get_drm_fd,
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

    ret = virgl_renderer_init(g,
                              VIRGL_RENDERER_DRM |
                              VIRGL_RENDERER_ASYNC_FENCE_CB |
                              VIRGL_RENDERER_NO_VIRGL,
                              &virgl_cbs);
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

    resp.capset_id = VIRTIO_GPU_CAPSET_DRM;
    virgl_renderer_get_cap_set(resp.capset_id, &resp.capset_max_version,
                               &resp.capset_max_size);
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

    virgl_renderer_get_cap_set(gc.capset_id, &max_ver,
                               &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(gc.capset_id, gc.capset_version,
                             (void *)resp->capset_data);
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

static void
accel_cmd_ctx_create(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;
    VirtIOAccel *accel= VIRTIO_ACCEL(g);

    VIRTIO_GPU_FILL_CMD(cc);

    info_report("create ctx, id %d, len %d, debug_name %s, node %s", cc.hdr.ctx_id, cc.nlen, cc.debug_name, accel->accel_node);

    virgl_renderer_context_create_with_flags(cc.hdr.ctx_id,
                                             6,
                                             cc.nlen,
                                             cc.debug_name);
}

static void
accel_cmd_ctx_destroy(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);

    virgl_renderer_context_destroy(cd.hdr.ctx_id);
}

static void
accel_cmd_resource_create_blob(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    struct virgl_renderer_resource_create_blob_args virgl_args = { 0 };
    struct virtio_gpu_resource_create_blob cblob;
    struct virtio_gpu_simple_resource *res;
    int ret;

    if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);

    res = virtio_gpu_find_resource(g, cblob.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = &((struct accel_virgl_resource *)g_new0(struct accel_virgl_resource, 1))->base;
    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                        cmd, &res->addrs,
                                        &res->iov, &res->iov_cnt);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    virgl_args.res_handle = cblob.resource_id;
    virgl_args.ctx_id = cblob.hdr.ctx_id;
    virgl_args.blob_mem = VIRGL_RENDERER_BLOB_MEM_GUEST;
    virgl_args.blob_id = cblob.blob_id;
    virgl_args.blob_flags = cblob.blob_flags;
    virgl_args.size = cblob.size;
    virgl_args.iovecs = res->iov;
    virgl_args.num_iovs = res->iov_cnt;

    ret = virgl_renderer_resource_create_blob(&virgl_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        goto cleanup_mapping;
    }

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);
    res = NULL;

    return;

cleanup_mapping:
    virtio_gpu_cleanup_mapping(g, res);
    return;
}

static void accel_cmd_resource_unref(VirtIOGPU *g,
                   struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_simple_resource *res;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(unref);


    res = virtio_gpu_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virgl_renderer_resource_detach_iov(unref.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }

    virgl_renderer_resource_unref(unref.resource_id);
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

    ret = virgl_renderer_submit_cmd(buf, cs.hdr.ctx_id, cs.size / 4);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }

out:
    g_free(buf);
}

static void
virtio_accel_process_cmd(VirtIOGPU *g,
                         struct virtio_gpu_ctrl_command *cmd)
{
    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

    //info_report("received command %x", cmd->cmd_hdr.type);
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

    if (cmd->finished) {
        return;
    }
    if (cmd->error) {
        error_report("ctrl 0x%x, error 0x%x",
                     cmd->cmd_hdr.type, cmd->error);
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

        if ((cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) && !cmd->error) {
            virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id,
                                                VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                                cmd->cmd_hdr.ring_idx,
                                                cmd->cmd_hdr.fence_id);
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
