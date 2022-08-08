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

#ifndef HW_VIRTIO_TEE_H
#define HW_VIRTIO_TEE_H

#include "qemu/queue.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_tee.h"
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/iov.h"
#include "qom/object.h"
#include "hw/virtio/virtio.h"

#define TYPE_VIRTIO_TEE "virtio-tee-device"
OBJECT_DECLARE_TYPE(VirtIOTEE, VirtIOTEEClass, VIRTIO_TEE)

struct VirtIOTEE {
    VirtIODevice parent_obj;

    VirtQueue *cmd_vq;

    QEMUBH *cmd_bh;

    QTAILQ_HEAD(, virtio_tee_command) cmdq;

    bool processing_cmdq;
};

struct VirtIOTEEClass {
    VirtioDeviceClass parent;

    void (*handle_cmd)(VirtIODevice *vdev, VirtQueue *vq);
    void (*process_cmd)(VirtIOTEE *t, struct virtio_tee_command *cmd);
};

struct virtio_tee_command {
    VirtQueueElement elem;
    VirtQueue *vq;
    struct virtio_tee_hdr cmd_hdr;
    uint32_t error;
    bool finished;
    QTAILQ_ENTRY(virtio_tee_command) next;
};

#define PAGE_SIZE       4096

#define VIRTIO_TEE_FILL_CMD(out) do {                                   \
        size_t s;                                                       \
        s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num, 0,          \
                       &out, sizeof(out));                              \
        if (s != sizeof(out)) {                                         \
            qemu_log_mask(LOG_GUEST_ERROR,                              \
                          "%s: command size incorrect %zu vs %zu\n",    \
                          __func__, s, sizeof(out));                    \
            return;                                                     \
        }                                                               \
    } while (0)

void virtio_tee_reset(VirtIODevice *vdev);

#endif /* HW_VIRTIO_TEE_H */
