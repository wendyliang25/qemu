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

#ifndef HW_VIRTIO_TEE_PCI_H
#define HW_VIRTIO_TEE_PCI_H

#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-tee.h"
#include "qom/object.h"

/*
 * virtio-tee-pci: This extends VirtioPCIProxy.
 */
#define TYPE_VIRTIO_TEE_PCI "virtio-tee-pci"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOTEEPCI, VIRTIO_TEE_PCI)

struct VirtIOTEEPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOTEE vdev;
};

#endif /* HW_VIRTIO_TEE_PCI_H */
