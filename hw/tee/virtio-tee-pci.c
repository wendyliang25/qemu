/*
 * Virtio TEE PCI Device
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
#include "hw/virtio/virtio-tee-pci.h"
#include "hw/pci/pci.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-tee.h"

static void virtio_tee_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOTEEPCI *vtee = VIRTIO_TEE_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&vtee->vdev);

    virtio_pci_force_virtio_1(vpci_dev);

    if (!qdev_realize(vdev, BUS(&vpci_dev->bus), errp)) {
        return;
    }
}

static void virtio_tee_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->hotpluggable = false;
    k->realize = virtio_tee_pci_realize;
    pcidev_k->class_id = PCI_CLASS_OTHERS;
}

static void virtio_tee_initfn(Object *obj)
{
    VirtIOTEEPCI *dev = VIRTIO_TEE_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_TEE);
}

static const VirtioPCIDeviceTypeInfo virtio_tee_pci_info = {
    .generic_name = TYPE_VIRTIO_TEE_PCI,
    .instance_size = sizeof(VirtIOTEEPCI),
    .instance_init = virtio_tee_initfn,
    .class_init = virtio_tee_pci_class_init,
};
module_obj(TYPE_VIRTIO_TEE_PCI);

static void virtio_tee_pci_register_types(void)
{
    virtio_pci_types_register(&virtio_tee_pci_info);
}

type_init(virtio_tee_pci_register_types)
