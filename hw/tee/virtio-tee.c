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
int virtio_tee_memory_map(VirtIOTEE *t,
                          TEEC_RegisteredMemoryReference *mref,
                          dma_addr_t addr, dma_addr_t len,
                          uint32_t size, uint32_t offset)
{
    void *buffer;
    dma_addr_t requested_len = len;

    buffer = dma_memory_map(VIRTIO_DEVICE(t)->dma_as,
                            addr, &len, DMA_DIRECTION_TO_DEVICE,
			    MEMTXATTRS_UNSPECIFIED);
    if (!buffer || len != requested_len) {
        fprintf(stderr, "error: dma_memory_map failed\n");
        if (buffer) {
            dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as, buffer, len,
                             DMA_DIRECTION_TO_DEVICE, 0);
        }
        return TEEC_ERROR_GENERIC;
    }

    mref->parent =
        (TEEC_SharedMemory *)((uintptr_t) buffer + len - PAGE_SIZE);

    /*
     * Refer virtio_tee_register_mem(). In host address space, we map extra
     * PAGE_SIZE memory as compared to actual shared memory that gets
     * registered.
     */
    if (len != mref->parent->size + PAGE_SIZE) {
        fprintf(stderr,
                "warning: mapped length(%lu) - shm size(%lu) != PAGE_SIZE\n",
                len, mref->parent->size);
    }

    mref->parent->buffer = buffer;
    mref->size = size;
    mref->offset = offset;

    return TEEC_SUCCESS;
}

static
void virtio_tee_memory_unmap(VirtIOTEE *t,
                             TEEC_RegisteredMemoryReference *mref)
{
    if (!t || !mref || !mref->parent || !mref->parent->buffer) {
        return;
    }

    dma_memory_unmap(VIRTIO_DEVICE(t)->dma_as, mref->parent->buffer,
                     mref->parent->size + PAGE_SIZE,
                     DMA_DIRECTION_TO_DEVICE, 0);
}

static
void virtio_tee_unmap_params(VirtIOTEE *t, TEEC_Operation *tee, uint32_t count)
{
    uint32_t type;
    int i;

    if (!count || !t || !tee) {
        return;
    }

    if (count > TEE_MAX_PARAMS) {
        fprintf(stderr, "virtio_tee_unmap_params: invalid count %u\n", count);
        return;
    }

    for (i = 0; i < count; ++i) {
        type = TEEC_PARAM_TYPE_GET(tee->paramTypes, i);

        if (type < TEE_OP_PARAM_TYPE_MEMREF_INPUT ||
            type > TEE_OP_PARAM_TYPE_MEMREF_INOUT) {
            continue;
        }

        virtio_tee_memory_unmap(t, &tee->params[i].memref);
    }
}

static
void set_teec_param_type(uint32_t *teec_param_types, uint32_t param_types)
{
    uint32_t temp = 0, type, teec_type;
    int i;

    for (i = 0; i < TEE_MAX_PARAMS; ++i) {
        type = TEEC_PARAM_TYPE_GET(param_types, i);

        switch (type) {
        case TEE_OP_PARAM_TYPE_NONE:
            teec_type = TEEC_NONE;
            break;
        case TEE_OP_PARAM_TYPE_VALUE_INPUT:
            teec_type = TEEC_VALUE_INPUT;
            break;
        case TEE_OP_PARAM_TYPE_VALUE_OUTPUT:
            teec_type = TEEC_VALUE_OUTPUT;
            break;
        case TEE_OP_PARAM_TYPE_VALUE_INOUT:
            teec_type = TEEC_VALUE_INOUT;
            break;
        case TEE_OP_PARAM_TYPE_MEMREF_INPUT:
            teec_type = TEEC_MEMREF_PARTIAL_INPUT;
            break;
        case TEE_OP_PARAM_TYPE_MEMREF_OUTPUT:
            teec_type = TEEC_MEMREF_PARTIAL_OUTPUT;
            break;
        case TEE_OP_PARAM_TYPE_MEMREF_INOUT:
            teec_type = TEEC_MEMREF_PARTIAL_INOUT;
            break;
        default:
            /*
             * Set invalid param type, so that driver code fails for this
             * parameter
             */
            teec_type = TEE_OP_PARAM_TYPE_INVALID;
            break;
        }

        if (teec_type == TEE_OP_PARAM_TYPE_INVALID) {
            fprintf(stderr,
                    "error: received invalid virtio tee param type %u\n",
                    type);
        }

        temp |= ((teec_type & 0xF) << i * 4);
    }

    *teec_param_types = temp;
}

static
int virtio_params_to_teec_params(VirtIOTEE *t,
                                 TEEC_Operation *tee, uint32_t count,
                                 struct virtio_tee_operation *op)
{
    uint32_t param_types = le32_to_cpu(op->param_types), type;
    int ret = TEEC_SUCCESS, i;

    if (!count) {
        return TEEC_SUCCESS;
    }

    if (!tee || !op || count > TEE_MAX_PARAMS) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    for (i = 0; i < TEE_MAX_PARAMS; ++i) {
        type = TEEC_PARAM_TYPE_GET(param_types, i);

        if (type == TEE_OP_PARAM_TYPE_INVALID) {
            return TEEC_ERROR_BAD_PARAMETERS;
        }

        if (type == TEE_OP_PARAM_TYPE_NONE ||
            type == TEE_OP_PARAM_TYPE_VALUE_OUTPUT) {
            continue;
        }

        if (type > TEE_OP_PARAM_TYPE_VALUE_INOUT) {
            dma_addr_t addr, len;
            uint32_t size, offset;

            addr = le64_to_cpu(op->params[i].mref.shm.addr);
            len = le32_to_cpu(op->params[i].mref.shm.size);
            size = le32_to_cpu(op->params[i].mref.size);
            offset = le32_to_cpu(op->params[i].mref.shm.offset);

            ret = virtio_tee_memory_map(t, &tee->params[i].memref,
                                        addr, len, size, offset);
            if (ret) {
                fprintf(stderr, "error: memory map failed %d\n", ret);
                goto err_unmap;
            }
        } else {
            if (op->params[i].val.c) {
                fprintf(stderr, "warning: discarding value c %lu\n",
                        le64_to_cpu(op->params[i].val.c));
            }

            tee->params[i].value.a = le64_to_cpu(op->params[i].val.a);
            tee->params[i].value.b = le64_to_cpu(op->params[i].val.b);
        }
    }

    tee->started = 1;
    set_teec_param_type(&tee->paramTypes, param_types);

    return TEEC_SUCCESS;

err_unmap:
    while (i >= 0) {
        virtio_tee_memory_unmap(t, &tee->params[i].memref);
        i--;
    }

    return ret;
}

static
int teec_params_to_virtio_params(TEEC_Operation *tee, uint32_t count,
                                 struct virtio_tee_operation *op)
{
    uint32_t type;
    int i;

    if (!count) {
        return TEEC_SUCCESS;
    }

    if (!tee || !op || count > TEE_MAX_PARAMS) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    for (i = 0; i < count; ++i) {
        type = TEEC_PARAM_TYPE_GET(op->param_types, i);

        if (type == TEE_OP_PARAM_TYPE_INVALID ||
            type > TEE_OP_PARAM_TYPE_MEMREF_INOUT) {
            return TEEC_ERROR_BAD_PARAMETERS;
        }

        if (type == TEE_OP_PARAM_TYPE_NONE ||
            type == TEE_OP_PARAM_TYPE_VALUE_INPUT ||
            type == TEE_OP_PARAM_TYPE_MEMREF_INPUT) {
            continue;
        }

        if (type > TEE_OP_PARAM_TYPE_MEMREF_INPUT) {
            op->params[i].mref.shm.offset =
                cpu_to_le32(tee->params[i].memref.offset);
            op->params[i].mref.size =
                cpu_to_le32(tee->params[i].memref.size);
        } else {
            uint64_t a = tee->params[i].value.a;
            uint64_t b = tee->params[i].value.b;

            /* Virtio TEE client does not support 'c' */
            op->params[i].val.a = cpu_to_le64(a);
            op->params[i].val.b = cpu_to_le64(b);
            op->params[i].val.c = 0;
        }
    }

    return TEEC_SUCCESS;
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

static void virtio_tee_open_session(VirtIOTEE *t,
                                    struct virtio_tee_command *cmd)
{
    struct virtio_tee_cmd_open_session os_cmd;
    struct virtio_tee_resp_open_session resp;
    uint32_t ret_origin = TEEC_ORIGIN_COMMS;
    TEEC_Operation op, *op_ptr = NULL;
    TEEC_Result ret, status;
    TEEC_Session session;
    uint32_t clnt_login;
    uint32_t num_params;
    TEEC_Context ctx;
    int fd;

    VIRTIO_TEE_FILL_CMD(os_cmd);

    resp.hdr.type = cpu_to_le32(VIRTIO_TEE_RESP_ERR_OPEN_SESSION);

    fd = le32_to_cpu(os_cmd.fd);
    clnt_login = le32_to_cpu(os_cmd.clnt_login);
    num_params = le32_to_cpu(os_cmd.num_params);

    if (num_params != 0) {
        ret = virtio_params_to_teec_params(t, &op, num_params, &os_cmd.op);
        if (ret != TEEC_SUCCESS) {
            goto end;
        }
        op_ptr = &op;
    }

    ctx.reg_mem = false;
    ctx.fd = fd;

    ret = teec_open_session(&ctx, &session, &os_cmd.uuid[0], clnt_login,
                            &os_cmd.clnt_uuid[0], op_ptr, &ret_origin);
    if (ret == TEEC_SUCCESS) {
        resp.hdr.type = cpu_to_le32(VIRTIO_TEE_RESP_OK_OPEN_SESSION);
        resp.session = cpu_to_le32(session.session_id);
    }

    if (num_params != 0) {
        resp.op.param_types = os_cmd.op.param_types;
        status = teec_params_to_virtio_params(&op, num_params, &resp.op);
        if (ret == TEEC_SUCCESS && status != TEEC_SUCCESS) {
            resp.hdr.type = cpu_to_le32(VIRTIO_TEE_RESP_ERR_OPEN_SESSION);
            ret = status;
            ret_origin = TEEC_ORIGIN_COMMS;
        }

        virtio_tee_unmap_params(t, &op, num_params);
    }

end:
    resp.ret = cpu_to_le32(ret);
    resp.ret_origin = cpu_to_le32(ret_origin);
    virtio_tee_cmd_response(t, cmd, &resp.hdr, sizeof(resp));
}

static void virtio_tee_close_session(VirtIOTEE *t,
                                     struct virtio_tee_command *cmd)
{
    struct virtio_tee_cmd_close_session close_session;
    TEEC_Session session;
    TEEC_Context ctx;

    VIRTIO_TEE_FILL_CMD(close_session);

    ctx.reg_mem = false;
    ctx.fd = le32_to_cpu(close_session.fd);

    session.ctx = &ctx;
    session.session_id = le32_to_cpu(close_session.session);

    teec_close_session(&session);
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
    case VIRTIO_TEE_CMD_OPEN_SESSION:
        virtio_tee_open_session(t, cmd);
        break;
    case VIRTIO_TEE_CMD_CLOSE_SESSION:
        virtio_tee_close_session(t, cmd);
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
