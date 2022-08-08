/*
 * Virtio TEE Device
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Author: Rijo Thomas <Rijo-john.Thomas@amd.com>
 *
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-tee.h"
#include "hw/virtio/virtio.h"
#include "qemu/main-loop.h"
#include "qemu/bswap.h"
#include "sysemu/dma.h"
#include "virtio-tee-client.h"

static inline void
virtio_tee_cmd_hdr_bswap(struct virtio_tee_hdr *hdr)
{
    le32_to_cpus(&hdr->type);
}

static
void virtio_tee_cmd_response(VirtIOTEE *t,
                             struct virtio_tee_command *cmd,
                             struct virtio_tee_hdr *resp,
                             size_t resp_len)
{
    size_t s;

    s = iov_from_buf(cmd->elem.in_sg, cmd->elem.in_num, 0, resp, resp_len);
    if (s != resp_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: response size incorrect %zu vs %zu\n",
                      __func__, s, resp_len);
    }

    virtqueue_push(cmd->vq, &cmd->elem, s);
    virtio_notify(VIRTIO_DEVICE(t), cmd->vq);
    cmd->finished = true;
}

static
void virtio_tee_cmd_response_nodata(VirtIOTEE *t,
                                    struct virtio_tee_command *cmd,
                                    enum virtio_tee_cmd_type type)
{
    struct virtio_tee_hdr resp;

    memset(&resp, 0, sizeof(resp));
    resp.type = cpu_to_le32(type);
    virtio_tee_cmd_response(t, cmd, &resp, sizeof(resp));
}

static void virtio_tee_open_device(VirtIOTEE *t,
                                   struct virtio_tee_command *cmd)
{
    struct virtio_tee_resp_open_device open_dev;
    uint32_t gen_caps;
    int fd;

    memset(&open_dev, 0, sizeof(open_dev));

    fd = teec_open_device(&gen_caps);
    if (fd < 0) {
        open_dev.hdr.type = cpu_to_le32(VIRTIO_TEE_RESP_ERR_OPEN_DEVICE);
    } else {
        open_dev.hdr.type = cpu_to_le32(VIRTIO_TEE_RESP_OK_OPEN_DEVICE);
        open_dev.fd = cpu_to_le32(fd);
        open_dev.gen_caps = cpu_to_le32(gen_caps);
    }

    virtio_tee_cmd_response(t, cmd, &open_dev.hdr, sizeof(open_dev));
}

static void virtio_tee_close_device(VirtIOTEE *t,
                                    struct virtio_tee_command *cmd)
{
    struct virtio_tee_cmd_close_device close_dev;
    int fd;

    VIRTIO_TEE_FILL_CMD(close_dev);

    fd = le32_to_cpu(close_dev.fd);

    teec_close_device(fd);
}

static void virtio_tee_register_mem(VirtIOTEE *t,
                                    struct virtio_tee_command *cmd)
{
    struct virtio_tee_cmd_register_mem reg_mem;
    TEEC_SharedMemory shm;
    dma_addr_t addr, len;
    TEEC_Context ctx;
    uint32_t size;
    int ret;

    VIRTIO_TEE_FILL_CMD(reg_mem);

    addr = le64_to_cpu(reg_mem.addr);
    size = le32_to_cpu(reg_mem.size);

    /*
     * Virtio TEE driver allocates extra page. Hence, size is expected
     * to be greater than PAGE_SIZE
     */
    if (size < PAGE_SIZE) {
        fprintf(stderr, "register_mem: error unexpected size %u\n", size);
        goto err;
    }

    len = size;

    shm.buffer = dma_memory_map(VIRTIO_DEVICE(t)->dma_as,
                                addr, &len, DMA_DIRECTION_FROM_DEVICE,
				MEMTXATTRS_UNSPECIFIED);
    if (!shm.buffer || len < size) {
        fprintf(stderr, "register_mem: error dma_memory_map failed\n");
        if (shm.buffer) {
            dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as,
                             shm.buffer, len, DMA_DIRECTION_FROM_DEVICE, 0);
        }
        goto err;
    }

    shm.size = size - PAGE_SIZE;

    /*
     * teec_register_shared_memory() does not make use of flags.
     * So, set a dummy value.
     */
    shm.flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;

    ctx.fd = le32_to_cpu(reg_mem.fd);
    ctx.reg_mem = false; /* Use allocate shm */

    ret = teec_register_shared_memory(&ctx, &shm);
    if (ret) {
        fprintf(stderr,
                "register_mem: error teec_register_shared_memory failed %d\n",
                ret);
        dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as,
                         shm.buffer, len, DMA_DIRECTION_FROM_DEVICE, 0);
        goto err;
    }

    /* Save the registered TEEC_SharedMemory context */
    memcpy((void *)((uintptr_t) shm.buffer + size - PAGE_SIZE),
           &shm, sizeof(shm));

    dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as,
                     shm.buffer, len, DMA_DIRECTION_FROM_DEVICE, 0);
    return;
err:
    cmd->error = VIRTIO_TEE_RESP_ERR_REGISTER_MEM;
}

static void virtio_tee_unregister_mem(VirtIOTEE *t,
                                      struct virtio_tee_command *cmd)
{
    struct virtio_tee_cmd_unregister_mem unreg_mem;
    TEEC_SharedMemory *shm;
    dma_addr_t addr, len;
    uint32_t size;
    void *buffer;

    VIRTIO_TEE_FILL_CMD(unreg_mem);

    addr = le64_to_cpu(unreg_mem.addr);
    size = le32_to_cpu(unreg_mem.size);

    len = size;

    buffer = dma_memory_map(VIRTIO_DEVICE(t)->dma_as,
                            addr, &len, DMA_DIRECTION_TO_DEVICE,
			    MEMTXATTRS_UNSPECIFIED);
    if (!buffer || len < size) {
        fprintf(stderr, "unregister_mem: error dma_memory_map failed\n");
        if (buffer) {
            dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as, buffer, len,
                             DMA_DIRECTION_TO_DEVICE, 0);
        }
        goto err;
    }

    shm = (TEEC_SharedMemory *)((uintptr_t) buffer + size - PAGE_SIZE);

    shm->buffer = buffer;

    teec_release_shared_memory(shm);

    dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as,
                     buffer, len, DMA_DIRECTION_TO_DEVICE, 0);
    return;
err:
    cmd->error = VIRTIO_TEE_RESP_ERR_UNREGISTER_MEM;
}

static void virtio_tee_process_cmd(VirtIOTEE *t,
                                   struct virtio_tee_command *cmd)
{
    VIRTIO_TEE_FILL_CMD(cmd->cmd_hdr);
    virtio_tee_cmd_hdr_bswap(&cmd->cmd_hdr);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_TEE_CMD_OPEN_DEVICE:
        virtio_tee_open_device(t, cmd);
        break;
    case VIRTIO_TEE_CMD_CLOSE_DEVICE:
        virtio_tee_close_device(t, cmd);
        break;
    case VIRTIO_TEE_CMD_REGISTER_MEM:
        virtio_tee_register_mem(t, cmd);
        break;
    case VIRTIO_TEE_CMD_UNREGISTER_MEM:
        virtio_tee_unregister_mem(t, cmd);
        break;
    default:
        cmd->error = VIRTIO_TEE_RESP_ERR_UNSPECIFIED;
        break;
    }

    if (!cmd->finished) {
        virtio_tee_cmd_response_nodata(t, cmd, cmd->error ? cmd->error :
                                       VIRTIO_TEE_RESP_OK_NODATA);
    }
}

static void virtio_tee_process_cmdq(VirtIOTEE *t)
{
    struct virtio_tee_command *cmd;
    VirtIOTEEClass *vtc = VIRTIO_TEE_GET_CLASS(t);

    if (t->processing_cmdq) {
        return;
    }

    t->processing_cmdq = true;

    while (!QTAILQ_EMPTY(&t->cmdq)) {
        cmd = QTAILQ_FIRST(&t->cmdq);

        /* process command */
        vtc->process_cmd(t, cmd);

        QTAILQ_REMOVE(&t->cmdq, cmd, next);

        if (!cmd->finished) {
            fprintf(stderr, "cmd (%x) not finished!\n", cmd->cmd_hdr.type);
        }

        g_free(cmd);
    }

    t->processing_cmdq = false;
}

static void virtio_tee_handle_cmd(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOTEE *t = VIRTIO_TEE(vdev);
    struct virtio_tee_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    cmd = virtqueue_pop(vq, sizeof(struct virtio_tee_command));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&t->cmdq, cmd, next);
        cmd = virtqueue_pop(vq, sizeof(struct virtio_tee_command));
    }

    virtio_tee_process_cmdq(t);
}

static void virtio_tee_handle_cmd_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOTEE *t = VIRTIO_TEE(vdev);
    qemu_bh_schedule(t->cmd_bh);
}

static void virtio_tee_cmd_bh(void *opaque)
{
    VirtIOTEE *t = opaque;
    VirtIOTEEClass *vtc = VIRTIO_TEE_GET_CLASS(t);

    vtc->handle_cmd(&t->parent_obj, t->cmd_vq);
}

void virtio_tee_reset(VirtIODevice *vdev)
{
    VirtIOTEE *t = VIRTIO_TEE(vdev);
    struct virtio_tee_command *cmd;

    while (!QTAILQ_EMPTY(&t->cmdq)) {
        cmd = QTAILQ_FIRST(&t->cmdq);
        QTAILQ_REMOVE(&t->cmdq, cmd, next);
        g_free(cmd);
    }

    t->processing_cmdq = false;
}

static void virtio_tee_device_realize(DeviceState *qdev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(qdev);
    VirtIOTEE *t = VIRTIO_TEE(qdev);

    /*
     * TODO: Do we need to check any config flags here that qemu user
     * might have set for the virtio tee device?
     */

    /*
     * There is no config structure defined for TEE. Hence its
     * size is 0.
     */
    virtio_init(VIRTIO_DEVICE(t), VIRTIO_ID_TEE, 0);

    virtio_add_queue(vdev, 64, virtio_tee_handle_cmd_cb);

    t->cmd_vq = virtio_get_queue(vdev, 0);
    t->cmd_bh = qemu_bh_new(virtio_tee_cmd_bh, t);

    QTAILQ_INIT(&t->cmdq);
}

static void virtio_tee_device_unrealize(DeviceState *qdev)
{
}

static uint64_t virtio_tee_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    return features;
}

static void virtio_tee_set_features(VirtIODevice *vdev, uint64_t features)
{
}

static const VMStateDescription vmstate_virtio_tee = {
    .name = "virtio-tee",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static void virtio_tee_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    VirtIOTEEClass *vtc = VIRTIO_TEE_CLASS(klass);

    vtc->handle_cmd = virtio_tee_handle_cmd;
    vtc->process_cmd = virtio_tee_process_cmd;

    vdc->realize = virtio_tee_device_realize;
    vdc->unrealize = virtio_tee_device_unrealize;
    vdc->reset = virtio_tee_reset;
    vdc->get_features = virtio_tee_get_features;
    vdc->set_features = virtio_tee_set_features;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->hotpluggable = false;

    dc->vmsd = &vmstate_virtio_tee; /* TODO: Check this */
}

static const TypeInfo virtio_tee_info = {
    .name = TYPE_VIRTIO_TEE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOTEE),
    .class_size = sizeof(VirtIOTEEClass),
    .class_init = virtio_tee_class_init,
};
module_obj(TYPE_VIRTIO_TEE);

static void virtio_register_types(void)
{
    type_register_static(&virtio_tee_info);
}

type_init(virtio_register_types)
