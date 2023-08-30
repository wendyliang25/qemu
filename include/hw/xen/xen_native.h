#ifndef QEMU_HW_XEN_NATIVE_H
#define QEMU_HW_XEN_NATIVE_H

#ifdef __XEN_INTERFACE_VERSION__
#error In Xen native files, include xen_native.h before other Xen headers
#endif

/*
 * If we have new enough libxenctrl then we do not want/need these compat
 * interfaces, despite what the user supplied cflags might say. They
 * must be undefined before including xenctrl.h
 */
#undef XC_WANT_COMPAT_EVTCHN_API
#undef XC_WANT_COMPAT_GNTTAB_API
#undef XC_WANT_COMPAT_MAP_FOREIGN_API

#include <xenctrl.h>
#include <xenstore.h>

#include "hw/xen/xen.h"
#include "hw/pci/pci_device.h"
#include "hw/xen/trace.h"

#include "exec/ramblock.h"

extern xc_interface *xen_xc;

/*
 * We don't support Xen prior to 4.7.1.
 */

#include <xenforeignmemory.h>

extern xenforeignmemory_handle *xen_fmem;

#if CONFIG_XEN_CTRL_INTERFACE_VERSION < 40900

typedef xc_interface xendevicemodel_handle;

#else /* CONFIG_XEN_CTRL_INTERFACE_VERSION >= 40900 */

#undef XC_WANT_COMPAT_DEVICEMODEL_API
#include <xendevicemodel.h>

#endif

#if CONFIG_XEN_CTRL_INTERFACE_VERSION < 41100

static inline int xendevicemodel_relocate_memory(
    xendevicemodel_handle *dmod, domid_t domid, uint32_t size, uint64_t src_gfn,
    uint64_t dst_gfn)
{
    uint32_t i;
    int rc;

    for (i = 0; i < size; i++) {
        unsigned long idx = src_gfn + i;
        xen_pfn_t gpfn = dst_gfn + i;

        rc = xc_domain_add_to_physmap(xen_xc, domid, XENMAPSPACE_gmfn, idx,
                                      gpfn);
        if (rc) {
            return rc;
        }
    }

    return 0;
}

static inline int xendevicemodel_pin_memory_cacheattr(
    xendevicemodel_handle *dmod, domid_t domid, uint64_t start, uint64_t end,
    uint32_t type)
{
    return xc_domain_pin_memory_cacheattr(xen_xc, domid, start, end, type);
}

typedef void xenforeignmemory_resource_handle;

#define XENMEM_resource_ioreq_server 0

#define XENMEM_resource_ioreq_server_frame_bufioreq 0
#define XENMEM_resource_ioreq_server_frame_ioreq(n) (1 + (n))

static inline xenforeignmemory_resource_handle *xenforeignmemory_map_resource(
    xenforeignmemory_handle *fmem, domid_t domid, unsigned int type,
    unsigned int id, unsigned long frame, unsigned long nr_frames,
    void **paddr, int prot, int flags)
{
    errno = EOPNOTSUPP;
    return NULL;
}

static inline int xenforeignmemory_unmap_resource(
    xenforeignmemory_handle *fmem, xenforeignmemory_resource_handle *fres)
{
    return 0;
}

#endif /* CONFIG_XEN_CTRL_INTERFACE_VERSION < 41100 */

#if CONFIG_XEN_CTRL_INTERFACE_VERSION < 41000

#define XEN_COMPAT_PHYSMAP
static inline void *xenforeignmemory_map2(xenforeignmemory_handle *h,
                                          uint32_t dom, void *addr,
                                          int prot, int flags, size_t pages,
                                          const xen_pfn_t arr[/*pages*/],
                                          int err[/*pages*/])
{
    assert(addr == NULL && flags == 0);
    return xenforeignmemory_map(h, dom, prot, pages, arr, err);
}

static inline int xentoolcore_restrict_all(domid_t domid)
{
    errno = ENOTTY;
    return -1;
}

static inline int xendevicemodel_shutdown(xendevicemodel_handle *dmod,
                                          domid_t domid, unsigned int reason)
{
    errno = ENOTTY;
    return -1;
}

#else /* CONFIG_XEN_CTRL_INTERFACE_VERSION >= 41000 */

#include <xentoolcore.h>

#endif

#if CONFIG_XEN_CTRL_INTERFACE_VERSION < 40900

static inline xendevicemodel_handle *xendevicemodel_open(
    struct xentoollog_logger *logger, unsigned int open_flags)
{
    return xen_xc;
}

static inline int xendevicemodel_create_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, int handle_bufioreq,
    ioservid_t *id)
{
    return xc_hvm_create_ioreq_server(dmod, domid, handle_bufioreq,
                                      id);
}

static inline int xendevicemodel_get_ioreq_server_info(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id,
    xen_pfn_t *ioreq_pfn, xen_pfn_t *bufioreq_pfn,
    evtchn_port_t *bufioreq_port)
{
    return xc_hvm_get_ioreq_server_info(dmod, domid, id, ioreq_pfn,
                                        bufioreq_pfn, bufioreq_port);
}

static inline int xendevicemodel_map_io_range_to_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id, int is_mmio,
    uint64_t start, uint64_t end)
{
    return xc_hvm_map_io_range_to_ioreq_server(dmod, domid, id, is_mmio,
                                               start, end);
}

static inline int xendevicemodel_unmap_io_range_from_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id, int is_mmio,
    uint64_t start, uint64_t end)
{
    return xc_hvm_unmap_io_range_from_ioreq_server(dmod, domid, id, is_mmio,
                                                   start, end);
}

static inline int xendevicemodel_map_pcidev_to_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id,
    uint16_t segment, uint8_t bus, uint8_t device, uint8_t function)
{
    return xc_hvm_map_pcidev_to_ioreq_server(dmod, domid, id, segment,
                                             bus, device, function);
}

static inline int xendevicemodel_unmap_pcidev_from_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id,
    uint16_t segment, uint8_t bus, uint8_t device, uint8_t function)
{
    return xc_hvm_unmap_pcidev_from_ioreq_server(dmod, domid, id, segment,
                                                 bus, device, function);
}

static inline int xendevicemodel_destroy_ioreq_server(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id)
{
    return xc_hvm_destroy_ioreq_server(dmod, domid, id);
}

static inline int xendevicemodel_set_ioreq_server_state(
    xendevicemodel_handle *dmod, domid_t domid, ioservid_t id, int enabled)
{
    return xc_hvm_set_ioreq_server_state(dmod, domid, id, enabled);
}

static inline int xendevicemodel_set_pci_intx_level(
    xendevicemodel_handle *dmod, domid_t domid, uint16_t segment,
    uint8_t bus, uint8_t device, uint8_t intx, unsigned int level)
{
    return xc_hvm_set_pci_intx_level(dmod, domid, segment, bus, device,
                                     intx, level);
}

static inline int xendevicemodel_set_isa_irq_level(
    xendevicemodel_handle *dmod, domid_t domid, uint8_t irq,
    unsigned int level)
{
    return xc_hvm_set_isa_irq_level(dmod, domid, irq, level);
}

static inline int xendevicemodel_set_pci_link_route(
    xendevicemodel_handle *dmod, domid_t domid, uint8_t link, uint8_t irq)
{
    return xc_hvm_set_pci_link_route(dmod, domid, link, irq);
}

static inline int xendevicemodel_inject_msi(
    xendevicemodel_handle *dmod, domid_t domid, uint64_t msi_addr,
    uint32_t msi_data)
{
    return xc_hvm_inject_msi(dmod, domid, msi_addr, msi_data);
}

static inline int xendevicemodel_track_dirty_vram(
    xendevicemodel_handle *dmod, domid_t domid, uint64_t first_pfn,
    uint32_t nr, unsigned long *dirty_bitmap)
{
    return xc_hvm_track_dirty_vram(dmod, domid, first_pfn, nr,
                                   dirty_bitmap);
}

static inline int xendevicemodel_modified_memory(
    xendevicemodel_handle *dmod, domid_t domid, uint64_t first_pfn,
    uint32_t nr)
{
    return xc_hvm_modified_memory(dmod, domid, first_pfn, nr);
}

static inline int xendevicemodel_set_mem_type(
    xendevicemodel_handle *dmod, domid_t domid, hvmmem_type_t mem_type,
    uint64_t first_pfn, uint32_t nr)
{
    return xc_hvm_set_mem_type(dmod, domid, mem_type, first_pfn, nr);
}

#endif

extern xendevicemodel_handle *xen_dmod;

static inline int xen_set_mem_type(domid_t domid, hvmmem_type_t type,
                                   uint64_t first_pfn, uint32_t nr)
{
    return xendevicemodel_set_mem_type(xen_dmod, domid, type, first_pfn,
                                       nr);
}

static inline int xen_set_pci_intx_level(domid_t domid, uint16_t segment,
                                         uint8_t bus, uint8_t device,
                                         uint8_t intx, unsigned int level)
{
    return xendevicemodel_set_pci_intx_level(xen_dmod, domid, segment, bus,
                                             device, intx, level);
}

static inline int xen_inject_msi(domid_t domid, uint64_t msi_addr,
                                 uint32_t msi_data)
{
    return xendevicemodel_inject_msi(xen_dmod, domid, msi_addr, msi_data);
}

static inline int xen_set_isa_irq_level(domid_t domid, uint8_t irq,
                                        unsigned int level)
{
    return xendevicemodel_set_isa_irq_level(xen_dmod, domid, irq, level);
}

static inline int xen_track_dirty_vram(domid_t domid, uint64_t first_pfn,
                                       uint32_t nr, unsigned long *bitmap)
{
    return xendevicemodel_track_dirty_vram(xen_dmod, domid, first_pfn, nr,
                                           bitmap);
}

static inline int xen_modified_memory(domid_t domid, uint64_t first_pfn,
                                      uint32_t nr)
{
    return xendevicemodel_modified_memory(xen_dmod, domid, first_pfn, nr);
}

static inline int xen_restrict(domid_t domid)
{
    int rc;
    rc = xentoolcore_restrict_all(domid);
    trace_xen_domid_restrict(rc ? errno : 0);
    return rc;
}

void destroy_hvm_domain(bool reboot);

/* shutdown/destroy current domain because of an error */
void xen_shutdown_fatal_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

#ifdef HVM_PARAM_VMPORT_REGS_PFN
static inline int xen_get_vmport_regs_pfn(xc_interface *xc, domid_t dom,
                                          xen_pfn_t *vmport_regs_pfn)
{
    int rc;
    uint64_t value;
    rc = xc_hvm_param_get(xc, dom, HVM_PARAM_VMPORT_REGS_PFN, &value);
    if (rc >= 0) {
        *vmport_regs_pfn = (xen_pfn_t) value;
    }
    return rc;
}
#else
static inline int xen_get_vmport_regs_pfn(xc_interface *xc, domid_t dom,
                                          xen_pfn_t *vmport_regs_pfn)
{
    return -ENOSYS;
}
#endif

static inline int xen_get_default_ioreq_server_info(domid_t dom,
                                                    xen_pfn_t *ioreq_pfn,
                                                    xen_pfn_t *bufioreq_pfn,
                                                    evtchn_port_t
                                                        *bufioreq_evtchn)
{
    unsigned long param;
    int rc;

    rc = xc_get_hvm_param(xen_xc, dom, HVM_PARAM_IOREQ_PFN, &param);
    if (rc < 0) {
        fprintf(stderr, "failed to get HVM_PARAM_IOREQ_PFN\n");
        return -1;
    }

    *ioreq_pfn = param;

    rc = xc_get_hvm_param(xen_xc, dom, HVM_PARAM_BUFIOREQ_PFN, &param);
    if (rc < 0) {
        fprintf(stderr, "failed to get HVM_PARAM_BUFIOREQ_PFN\n");
        return -1;
    }

    *bufioreq_pfn = param;

    rc = xc_get_hvm_param(xen_xc, dom, HVM_PARAM_BUFIOREQ_EVTCHN,
                          &param);
    if (rc < 0) {
        fprintf(stderr, "failed to get HVM_PARAM_BUFIOREQ_EVTCHN\n");
        return -1;
    }

    *bufioreq_evtchn = param;

    return 0;
}

#define DYN_HVA_MAPPING

#ifdef DYN_HVA_MAPPING
static inline void xen_update_hva(unsigned long hva, size_t npages)
{
    privcmd_update_hva(xen_fmem, hva, npages);
}
#else
static bool reuse_hva_hpfn_mapping = true;
#endif

static bool use_default_ioreq_server;

static inline void xen_map_memory_section(domid_t dom,
                                          ioservid_t ioservid,
                                          MemoryRegionSection *section)
{
    hwaddr start_addr = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    hwaddr end_addr = start_addr + size - 1;

    if (use_default_ioreq_server) {
        return;
    }

#ifdef DYN_HVA_MAPPING
    if (section->mr->is_hostmem) {
        void *hva = section->mr->ram_block->host + section->offset_within_region;
        xen_pfn_t gfn = start_addr >> XC_PAGE_SHIFT;
        unsigned int npages = DIV_ROUND_UP(size, XC_PAGE_SIZE);
        int rc;

        fprintf(stderr, "New Hostmem Map (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
                section->mr->name, dom, start_addr, end_addr, size);
        rc = privcmd_map_hva(xen_fmem, xen_domid, (unsigned long)hva, gfn, npages, true);
        if (rc) {
            fprintf(stderr, "%s: New Hostmem Map rc=%d\n", __func__, rc);
        }
    }
#else
    if (section->mr->is_hostmem) {
        domid_t gdom = dom;
        domid_t hdom = 0;
        xen_pfn_t start_gpfn, *gpfns, *hpfns;
        unsigned int nr_pfns = size >> XC_PAGE_SHIFT;
        int i, rc, *errs;
        void *hva = section->mr->ram_block->host + section->offset_within_region;
        section->mr->is_mmio = false;

        fprintf(stderr, "Hostmem Map (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
                section->mr->name, dom, start_addr, end_addr, size);

        section->mr->hpfns = g_malloc(nr_pfns * sizeof(*hpfns));
        hpfns = section->mr->hpfns;
        gpfns = g_malloc(nr_pfns * sizeof(*gpfns));
        errs = g_malloc(nr_pfns * sizeof(*errs));
        if (!hpfns || !gpfns || !errs) {
            fprintf(stderr, "%s: mem alloc failed\n", __FUNCTION__);
            return;
        }

        start_gpfn = start_addr >> XC_PAGE_SHIFT;
        for (i = 0; i < nr_pfns; i++)
            gpfns[i] = start_gpfn + i;

        rc = map_hva_to_gpfns(xen_fmem, hdom, gdom, nr_pfns,
                              hva, gpfns, hpfns, 1);
        if (rc) {
            fprintf(stderr, "%s: map_hva_to_gpfns failed rc=%d\n", __func__, rc);
            goto out;
        }

        /* Note: this function uses xen_add_to_physmap_batch, which can only
         * store the number of pages to process in a uint16_t without checking
         * if size is larger.
         */
        int page_done = 0;
        for (int p = 0; p < DIV_ROUND_UP(size, (uint16_t)0xffff) && rc == 0; p++) {
            int n = MIN(nr_pfns - page_done, (uint16_t)0xffff);
            rc = xc_domain_add_to_physmap_batch(xen_xc, gdom, hdom,
                                                XENMAPSPACE_gmfn_foreign,
                                                n, &hpfns[page_done], &gpfns[page_done], &errs[page_done]);
            for (i = 0; i < n; i++) {
                if (errs[page_done + i]) {
                    rc = errs[page_done + i];
                    break;
                }
            }
            page_done += n;
        }

        if (rc == 0) /* Success */
          goto out;

        fprintf(stderr, "%s: WE SHOULD NOT BE HERE\n", __func__);
        assert(false);

        /* TODO: undo the successful xc_domain_add_to_physmap_batch part */
        rc = 0;

        for (int i = 0; i < nr_pfns && rc == 0; i++) {
          rc = xc_domain_iomem_permission(xen_xc, gdom, hpfns[i], 1, 1);
          if (rc)
            printf("xc_domain_iomem_permission failed\n");
          else
            rc = xc_domain_memory_mapping(xen_xc, gdom, gpfns[i], hpfns[i], 1, 1);
          if (rc)
            printf("xc_domain_memory_mapping failed\n");
        }
        section->mr->is_mmio = true;

out:
        if (rc)
            printf("%s: hva=%p gpfn=[0x%lx, 0x%lx] hpfn=[0x%lx, 0x%lx] is_mmio=%d rc=%d\n",
                    __func__, hva,
                    gpfns[0], gpfns[nr_pfns - 1],
                    hpfns[0], hpfns[nr_pfns - 1],
                    section->mr->is_mmio, rc);

        if (reuse_hva_hpfn_mapping)
            section->mr->hpfns = hpfns;
        else
            g_free(hpfns);

        g_free(gpfns);
        g_free(errs);

        if (rc == 0)
            return;
        else
            section->mr->is_hostmem = false;
    }
#endif /* DYN_HVA_MAPPING */

    fprintf(stderr, "Map (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
            section->mr->name, dom, start_addr, end_addr, size);
    trace_xen_map_mmio_range(ioservid, start_addr, end_addr);
    xendevicemodel_map_io_range_to_ioreq_server(xen_dmod, dom, ioservid, 1,
                                                start_addr, end_addr);
}

static inline void xen_unmap_memory_section(domid_t dom,
                                            ioservid_t ioservid,
                                            MemoryRegionSection *section)
{
    hwaddr start_addr = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    hwaddr end_addr = start_addr + size - 1;
    int rc = 0;

    if (use_default_ioreq_server) {
        return;
    }

#ifdef DYN_HVA_MAPPING
    if (section->mr->is_hostmem) {
        void *hva = section->mr->ram_block->host + section->offset_within_region;
        xen_pfn_t gfn = start_addr >> XC_PAGE_SHIFT;
        unsigned int npages = DIV_ROUND_UP(size, XC_PAGE_SIZE);

        fprintf(stderr, "New Hostmem Unmap (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
                section->mr->name, dom, start_addr, end_addr, size);

        rc = privcmd_map_hva(xen_fmem, xen_domid, (unsigned long)hva, gfn, npages, false);
        if (rc) {
            fprintf(stderr, "%s: New Hostmem Unmap failed rc=%d\n", __func__, rc);
        }
    }
#else
    if (section->mr->is_hostmem) {
        xen_pfn_t start_gpfn = start_addr >> XC_PAGE_SHIFT;
        unsigned int i, nr_pfns = size >> XC_PAGE_SHIFT;
        void *hva = section->mr->ram_block->host + section->offset_within_region;

        fprintf(stderr, "Hostmem Unmap (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
                section->mr->name, dom, start_addr, end_addr, size);

        if (!section->mr->is_mmio) {
            xen_pfn_t *gpfns, *mfns;
            domid_t gdom = dom;
            domid_t hdom = 0;
            int *errs;

            gpfns = g_malloc(nr_pfns * sizeof(*gpfns));
            mfns = g_malloc(nr_pfns * sizeof(*mfns));
            errs = g_malloc(nr_pfns * sizeof(*errs));
            if (!gpfns || !mfns || !errs)
                return;
            memset(mfns, 0xff, nr_pfns * sizeof(*mfns));
            memset(errs, 0, nr_pfns * sizeof(*errs));

            for (i = 0; i < nr_pfns; i++)
                gpfns[i] = start_gpfn + i;

            int page_done = 0;
            for (int p = 0; p < DIV_ROUND_UP(nr_pfns, (uint16_t)0xffff); p++) {
                int n = MIN(nr_pfns - page_done, (uint16_t)0xffff);
                rc = xc_domain_add_to_physmap_batch(
                    xen_xc, gdom, hdom, XENMAPSPACE_gmfn_foreign,
                    n, &mfns[page_done], &gpfns[page_done], &errs[page_done]);
                if (rc)
                    break;
                page_done += n;
            }

            if (rc) {
                printf("xc_domain_add_to_physmap_batch (unmap) %d/%d - rc=%d\n",
                       i, nr_pfns, rc);
                printf("    addr=0x%lx-0x%lx\n", start_addr, end_addr);
            }

            /* Pretend everything went fine. */
            if (reuse_hva_hpfn_mapping) {
               g_free(section->mr->hpfns);
               section->mr->hpfns = NULL;
            }
            g_free(gpfns);
            g_free(mfns);
            g_free(errs);

            if (rc)
                printf("%s: hva=%p gpfn=[0x%lx, 0x%lx] rc=%d %s\n",
                       __func__, hva,
                       start_gpfn, start_gpfn + nr_pfns - 1,
                       rc, rc == 0 ? "" : "(ignored)");
        } else {
            domid_t gdom = dom;
            domid_t hdom = 0;
            xen_pfn_t *gpfns, *hpfns;
            int *errs;

            fprintf(stderr, "%s: WE SHOULD NOT BE HERE\n", __func__);
            assert(false);

            hpfns =
               reuse_hva_hpfn_mapping ? section->mr->hpfns : g_malloc(nr_pfns * sizeof(*hpfns));
            gpfns = g_malloc(nr_pfns * sizeof(*gpfns));
            errs = g_malloc(nr_pfns * sizeof(*errs));
            if (!hpfns || !gpfns || !errs)
               return;

            for (i = 0; i < nr_pfns; i++)
               gpfns[i] = start_gpfn + i;

            if (reuse_hva_hpfn_mapping) {
               rc = 0;
            } else {
               rc = map_hva_to_gpfns(xen_fmem, hdom, gdom, nr_pfns, hva, gpfns, hpfns, 1);
            }

            for (int i = 0; i < nr_pfns && rc == 0; i++) {
               rc = xc_domain_memory_mapping(xen_xc, dom, gpfns[i], hpfns[i], 1, 0);
               if (rc == 0) {
                  rc = xc_domain_iomem_permission(xen_xc, dom, hpfns[i], 1, 0);
                  if (rc)
                     printf("xc_domain_iomem_permission [%d] gpfn=0x%lx hpfn=0x%lx failed\n", i,
                            gpfns[i], hpfns[i]);
               } else {
                  printf("xc_domain_memory_mapping [%d] gpfn=0x%lx hpfn=0x%lx failed\n", i,
                         gpfns[i], hpfns[i]);
               }
            }

            if (rc)
                printf("%s: hva=%p gpfn=[0x%lx, 0x%lx] hpfn=[0x%lx, 0x%lx] is_mmio=%d rc=%d\n",
                       __func__, hva,
                       gpfns[0], gpfns[nr_pfns - 1],
                       hpfns[0], hpfns[nr_pfns - 1],
                       section->mr->is_mmio, rc);

            g_free(section->mr->hpfns);
            section->mr->hpfns = NULL;
            g_free(gpfns);
            g_free(errs);
        }
        return;
    }
#endif /* DYN_HVA_MAPPING */

    fprintf(stderr, "Unmap (%s) via ioreqserver: dom=%d addr=0x%lx-0x%lx size=0x%lx\n",
            section->mr->name, dom, start_addr, end_addr, size);
    trace_xen_unmap_mmio_range(ioservid, start_addr, end_addr);
    xendevicemodel_unmap_io_range_from_ioreq_server(xen_dmod, dom, ioservid,
                                                    1, start_addr, end_addr);
}

static inline void xen_map_io_section(domid_t dom,
                                      ioservid_t ioservid,
                                      MemoryRegionSection *section)
{
    hwaddr start_addr = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    hwaddr end_addr = start_addr + size - 1;

    if (use_default_ioreq_server) {
        return;
    }

    trace_xen_map_portio_range(ioservid, start_addr, end_addr);
    xendevicemodel_map_io_range_to_ioreq_server(xen_dmod, dom, ioservid, 0,
                                                start_addr, end_addr);
}

static inline void xen_unmap_io_section(domid_t dom,
                                        ioservid_t ioservid,
                                        MemoryRegionSection *section)
{
    hwaddr start_addr = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    hwaddr end_addr = start_addr + size - 1;

    if (use_default_ioreq_server) {
        return;
    }

    trace_xen_unmap_portio_range(ioservid, start_addr, end_addr);
    xendevicemodel_unmap_io_range_from_ioreq_server(xen_dmod, dom, ioservid,
                                                    0, start_addr, end_addr);
}

static inline void xen_map_pcidev(domid_t dom,
                                  ioservid_t ioservid,
                                  PCIDevice *pci_dev)
{
    if (use_default_ioreq_server) {
        return;
    }

    trace_xen_map_pcidev(ioservid, pci_dev_bus_num(pci_dev),
                         PCI_SLOT(pci_dev->devfn), PCI_FUNC(pci_dev->devfn));
    xendevicemodel_map_pcidev_to_ioreq_server(xen_dmod, dom, ioservid, 0,
                                              pci_dev_bus_num(pci_dev),
                                              PCI_SLOT(pci_dev->devfn),
                                              PCI_FUNC(pci_dev->devfn));
}

static inline void xen_unmap_pcidev(domid_t dom,
                                    ioservid_t ioservid,
                                    PCIDevice *pci_dev)
{
    if (use_default_ioreq_server) {
        return;
    }

    trace_xen_unmap_pcidev(ioservid, pci_dev_bus_num(pci_dev),
                           PCI_SLOT(pci_dev->devfn), PCI_FUNC(pci_dev->devfn));
    xendevicemodel_unmap_pcidev_from_ioreq_server(xen_dmod, dom, ioservid, 0,
                                                  pci_dev_bus_num(pci_dev),
                                                  PCI_SLOT(pci_dev->devfn),
                                                  PCI_FUNC(pci_dev->devfn));
}

static inline int xen_create_ioreq_server(domid_t dom,
                                          ioservid_t *ioservid)
{
    int rc = xendevicemodel_create_ioreq_server(xen_dmod, dom,
                                                HVM_IOREQSRV_BUFIOREQ_ATOMIC,
                                                ioservid);

    if (rc == 0) {
        trace_xen_ioreq_server_create(*ioservid);
        return rc;
    }

    *ioservid = 0;
    use_default_ioreq_server = true;
    trace_xen_default_ioreq_server();

    return rc;
}

static inline void xen_destroy_ioreq_server(domid_t dom,
                                            ioservid_t ioservid)
{
    if (use_default_ioreq_server) {
        return;
    }

    trace_xen_ioreq_server_destroy(ioservid);
    xendevicemodel_destroy_ioreq_server(xen_dmod, dom, ioservid);
}

static inline int xen_get_ioreq_server_info(domid_t dom,
                                            ioservid_t ioservid,
                                            xen_pfn_t *ioreq_pfn,
                                            xen_pfn_t *bufioreq_pfn,
                                            evtchn_port_t *bufioreq_evtchn)
{
    if (use_default_ioreq_server) {
        return xen_get_default_ioreq_server_info(dom, ioreq_pfn,
                                                 bufioreq_pfn,
                                                 bufioreq_evtchn);
    }

    return xendevicemodel_get_ioreq_server_info(xen_dmod, dom, ioservid,
                                                ioreq_pfn, bufioreq_pfn,
                                                bufioreq_evtchn);
}

static inline int xen_set_ioreq_server_state(domid_t dom,
                                             ioservid_t ioservid,
                                             bool enable)
{
    if (use_default_ioreq_server) {
        return 0;
    }

    trace_xen_ioreq_server_state(ioservid, enable);
    return xendevicemodel_set_ioreq_server_state(xen_dmod, dom, ioservid,
                                                 enable);
}

#if CONFIG_XEN_CTRL_INTERFACE_VERSION <= 41500
static inline int xendevicemodel_set_irq_level(xendevicemodel_handle *dmod,
                                               domid_t domid, uint32_t irq,
                                               unsigned int level)
{
    return 0;
}
#endif

#if CONFIG_XEN_CTRL_INTERFACE_VERSION <= 41700
#define GUEST_VIRTIO_MMIO_BASE   xen_mk_ullong(0x02000000)
#define GUEST_VIRTIO_MMIO_SIZE   xen_mk_ullong(0x00100000)
#define GUEST_VIRTIO_MMIO_SPI_FIRST   33
#define GUEST_VIRTIO_MMIO_SPI_LAST    43
#endif

#endif /* QEMU_HW_XEN_NATIVE_H */
