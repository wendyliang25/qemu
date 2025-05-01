/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Virtio Accelerator Device
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-gpu-pci.h"
#include "qom/object.h"

#define TYPE_VIRTIO_ACCEL_PCI "virtio-accel-pci"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOAccelPCI, VIRTIO_ACCEL_PCI)

struct VirtIOAccelPCI {
    VirtIOGPUPCIBase parent_obj;
    VirtIOAccel vdev;
};

static void virtio_accel_initfn(Object *obj)
{
    VirtIOAccelPCI *dev = VIRTIO_ACCEL_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_ACCEL);
    VIRTIO_GPU_PCI_BASE(obj)->vgpu = VIRTIO_GPU_BASE(&dev->vdev);
}

static const TypeInfo virtio_accel_pci_info[] = {
    {
        .name = TYPE_VIRTIO_ACCEL_PCI,
        .parent = TYPE_VIRTIO_GPU_PCI_BASE,
        .instance_size = sizeof(VirtIOAccelPCI),
        .instance_init = virtio_accel_initfn,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        }
    },
};

DEFINE_TYPES(virtio_accel_pci_info)

module_obj(TYPE_VIRTIO_ACCEL_PCI);
module_kconfig(VIRTIO_PCI);
module_dep("hw-display-virtio-gpu-pci");
