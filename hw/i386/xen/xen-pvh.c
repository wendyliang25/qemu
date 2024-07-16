/*
 * QEMU Xen PVH Machine
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "hw/xen/arch_hvm.h"
#include "hw/xen/xen.h"
#include "hw/xen/xen-pvh-common.h"

#define TYPE_XEN_PVH_X86  MACHINE_TYPE_NAME("xenpvh")
OBJECT_DECLARE_SIMPLE_TYPE(XenPVHx86State, XEN_PVH_X86)

#define PVH_MAX_CPUS 128

#define PVH_LOWMEM_BASE "ram-low-base"
#define PVH_LOWMEM_SIZE "ram-low-size"
#define PVH_HIGHMEM_BASE "ram-high-base"
#define PVH_HIGHMEM_SIZE "ram-high-size"
#define PVH_PCIE_ECAM_BASE "pcie-ecam-base"
#define PVH_PCIE_ECAM_SIZE "pcie-ecam-size"
#define PVH_PCIE_MMIO_BASE "pcie-mmio-base"
#define PVH_PCIE_MMIO_SIZE "pcie-mmio-size"
#define PVH_PCIE_64BIT_MMIO_BASE "pcie-64bit-mmio-base"
#define PVH_PCIE_64BIT_MMIO_SIZE "pcie-64bit-mmio-size"

struct XenPVHx86State {
    /*< private >*/
    MachineState parent;

    DeviceState *cpu[PVH_MAX_CPUS];
    XenPVHCommonState pvh;

    uint64_t lowmem_base;
    uint64_t lowmem_size;
    uint64_t highmem_base;
    uint64_t highmem_size;

    uint64_t pcie_ecam_base;
    uint64_t pcie_ecam_size;
    uint64_t pcie_mmio_base;
    uint64_t pcie_mmio_size;
    uint64_t pcie_64bit_mmio_base;
    uint64_t pcie_64bit_mmio_size;
};

static void xenpvh_cpu_new(MachineState *ms,
                           XenPVHx86State *xp,
                           int cpu_idx,
                           int64_t apic_id)
{
    Object *cpu = object_new(ms->cpu_type);

    object_property_add_child(OBJECT(ms), "cpu[*]", cpu);
    object_property_set_uint(cpu, "apic-id", apic_id, &error_fatal);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    object_unref(cpu);

    xp->cpu[cpu_idx] = DEVICE(cpu);
}

static void xenpvh_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    XenPVHx86State *xp = XEN_PVH_X86(machine);
    int i;

    /* Describe the memory map.  */
    xp->pvh.cfg.ram_low.base = xp->lowmem_base;
    xp->pvh.cfg.ram_low.size = xp->lowmem_size;
    xp->pvh.cfg.ram_high.base = xp->highmem_base;
    xp->pvh.cfg.ram_high.size = xp->highmem_size;
    xp->pvh.cfg.pci.ecam.base = xp->pcie_ecam_base;
    xp->pvh.cfg.pci.ecam.size = xp->pcie_ecam_size;
    xp->pvh.cfg.pci.mmio.base = xp->pcie_mmio_base;
    xp->pvh.cfg.pci.mmio.size = xp->pcie_mmio_size;
    xp->pvh.cfg.pci.mmio_high.base = xp->pcie_64bit_mmio_base;
    xp->pvh.cfg.pci.mmio_high.size = xp->pcie_64bit_mmio_size;
    /* GSI's 16 - 20 are used for legacy PCIe INTX IRQs.  */
    xp->pvh.cfg.pci.intx_irq_base = 16;

    /* Create dummy cores. This will also create the APIC MSI window.  */
    for (i = 0; i < machine->smp.cpus; i++) {
        xenpvh_cpu_new(machine, xp, i, i);
    }

    xen_pvh_common_init(machine, &xp->pvh, sysmem);
}

static void pvh_set_lowmem_base(Object *obj, Visitor *v,
                                const char *name, void *opaque,
                                Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->lowmem_base = value;
}

static void pvh_set_lowmem_size(Object *obj, Visitor *v,
                                const char *name, void *opaque,
                                Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->lowmem_size = value;
}

static void pvh_set_highmem_base(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->highmem_base = value;
}

static void pvh_set_highmem_size(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->highmem_size = value;
}

static void pvh_set_pcie_ecam_base(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_ecam_base = value;
}

static void pvh_set_pcie_ecam_size(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_ecam_size = value;
}

static void pvh_set_pcie_mmio_base(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_mmio_base = value;
}

static void pvh_set_pcie_mmio_size(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_mmio_size = value;
}

static void pvh_set_pcie_64bit_mmio_base(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_64bit_mmio_base = value;
}

static void pvh_set_pcie_64bit_mmio_size(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    XenPVHx86State *xp = XEN_PVH_X86(obj);
    uint64_t value;

    if (!visit_type_size(v, name, &value, errp)) {
        return;
    }
    xp->pcie_64bit_mmio_size = value;
}

static void xenpvh_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Xen PVH x86 machine";
    mc->init = xenpvh_init;
    mc->max_cpus = PVH_MAX_CPUS;
    mc->default_cpu_type = TARGET_DEFAULT_CPU_TYPE;
    mc->default_machine_opts = "accel=xen";
    /* Set explicitly here to make sure that real ram_size is passed */
    mc->default_ram_size = 0;

    object_class_property_add(oc, PVH_LOWMEM_BASE, "uint64_t",
                              NULL, pvh_set_lowmem_base, NULL, NULL);
    object_class_property_add(oc, PVH_LOWMEM_SIZE, "uint64_t",
                              NULL, pvh_set_lowmem_size, NULL, NULL);
    object_class_property_add(oc, PVH_HIGHMEM_BASE, "uint64_t",
                              NULL, pvh_set_highmem_base, NULL, NULL);
    object_class_property_add(oc, PVH_HIGHMEM_SIZE, "uint64_t",
                              NULL, pvh_set_highmem_size, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_ECAM_BASE, "uint64_t",
                              NULL, pvh_set_pcie_ecam_base, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_ECAM_SIZE, "uint64_t",
                              NULL, pvh_set_pcie_ecam_size, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_MMIO_BASE, "uint64_t",
                              NULL, pvh_set_pcie_mmio_base, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_MMIO_SIZE, "uint64_t",
                              NULL, pvh_set_pcie_mmio_size, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_64BIT_MMIO_BASE, "uint64_t",
                              NULL, pvh_set_pcie_64bit_mmio_base, NULL, NULL);
    object_class_property_add(oc, PVH_PCIE_64BIT_MMIO_SIZE, "uint64_t",
                              NULL, pvh_set_pcie_64bit_mmio_size, NULL, NULL);
}

static const TypeInfo xenpvh_machine_type = {
    .name = TYPE_XEN_PVH_X86,
    .parent = TYPE_MACHINE,
    .class_init = xenpvh_machine_class_init,
    .instance_size = sizeof(XenPVHx86State),
};

static void xenpvh_machine_register_types(void)
{
    type_register_static(&xenpvh_machine_type);
}

type_init(xenpvh_machine_register_types)
