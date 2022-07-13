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

static void virtio_tee_handle_cmd_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOTEE *t = VIRTIO_TEE(vdev);
    qemu_bh_schedule(t->cmd_bh);
}

static void virtio_tee_cmd_bh(void *opaque)
{
}

void virtio_tee_reset(VirtIODevice *vdev)
{
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
