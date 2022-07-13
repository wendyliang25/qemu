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
#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/virtio/virtio.h"

#define TYPE_VIRTIO_TEE "virtio-tee-device"
OBJECT_DECLARE_TYPE(VirtIOTEE, VirtIOTEEClass, VIRTIO_TEE)

struct VirtIOTEE {
    VirtIODevice parent_obj;

    VirtQueue *cmd_vq;

    QEMUBH *cmd_bh;

    /* TODO: add further fields */
};

struct VirtIOTEEClass {
    VirtioDeviceClass parent;

    /* TODO: add further methods */
};

void virtio_tee_reset(VirtIODevice *vdev);

#endif /* HW_VIRTIO_TEE_H */
