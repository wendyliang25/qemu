/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/iov.h"
#include "ui/console.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "trace.h"
#include "exec/ramblock.h"
#include "sysemu/hostmem.h"
#include <sys/ioctl.h>
#include <linux/memfd.h>
#include "qemu/memfd.h"
#include "standard-headers/linux/udmabuf.h"

static int virtio_gpu_find_overlay_by_id(VirtIOGPU *g, uint32_t scanout_id,
                                         uint32_t overlay_id)
{
    VGPUDMABuf *tmp;
    int i;

    for (i = 0; i < VIRTIO_GPU_MAX_OVERLAYS_PER_SCANOUT; i++) {
        tmp = g->dmabuf.overlay[scanout_id][i];
        if (!tmp)
            return i;

        if (tmp->overlay_id == overlay_id)
            return i;
    }

    return -EINVAL;
}

static void virtio_gpu_create_udmabuf(struct virtio_gpu_simple_resource *res)
{
    struct udmabuf_create_list *list;
    RAMBlock *rb;
    ram_addr_t offset;
    int udmabuf, i;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return;
    }

    list = g_malloc0(sizeof(struct udmabuf_create_list) +
                     sizeof(struct udmabuf_create_item) * res->iov_cnt);

    for (i = 0; i < res->iov_cnt; i++) {
        rcu_read_lock();
        rb = qemu_ram_block_from_host(res->iov[i].iov_base, false, &offset);
        rcu_read_unlock();

        if (!rb || rb->fd < 0) {
            g_free(list);
            return;
        }

        list->list[i].memfd  = rb->fd;
        list->list[i].offset = offset;
        list->list[i].size   = res->iov[i].iov_len;
    }

    list->count = res->iov_cnt;
    list->flags = UDMABUF_FLAGS_CLOEXEC;

    res->dmabuf_fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);
    if (res->dmabuf_fd < 0) {
        warn_report("%s: UDMABUF_CREATE_LIST: %s", __func__,
                    strerror(errno));
    }
    g_free(list);
}

static void virtio_gpu_remap_udmabuf(struct virtio_gpu_simple_resource *res)
{
    res->remapped = mmap(NULL, res->blob_size, PROT_READ,
                         MAP_SHARED, res->dmabuf_fd, 0);
    if (res->remapped == MAP_FAILED) {
        warn_report("%s: dmabuf mmap failed: %s", __func__,
                    strerror(errno));
        res->remapped = NULL;
    }
}

static void virtio_gpu_destroy_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->remapped) {
        munmap(res->remapped, res->blob_size);
        res->remapped = NULL;
    }
    if (res->dmabuf_fd >= 0) {
        close(res->dmabuf_fd);
        res->dmabuf_fd = -1;
    }
}

static int find_memory_backend_type(Object *obj, void *opaque)
{
    bool *memfd_backend = opaque;
    int ret;

    if (object_dynamic_cast(obj, TYPE_MEMORY_BACKEND)) {
        HostMemoryBackend *backend = MEMORY_BACKEND(obj);
        RAMBlock *rb = backend->mr.ram_block;

        if (rb && rb->fd > 0) {
            ret = fcntl(rb->fd, F_GET_SEALS);
            if (ret > 0) {
                *memfd_backend = true;
            }
        }
    }

    return 0;
}

bool virtio_gpu_have_udmabuf(void)
{
    Object *memdev_root;
    int udmabuf;
    bool memfd_backend = false;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return false;
    }

    memdev_root = object_resolve_path("/objects", NULL);
    object_child_foreach(memdev_root, find_memory_backend_type, &memfd_backend);

    return memfd_backend;
}

void virtio_gpu_init_udmabuf(struct virtio_gpu_simple_resource *res)
{
    void *pdata = NULL;

    res->dmabuf_fd = -1;
    if (res->iov_cnt == 1) {
        pdata = res->iov[0].iov_base;
    } else {
        virtio_gpu_create_udmabuf(res);
        if (res->dmabuf_fd < 0) {
            return;
        }
        virtio_gpu_remap_udmabuf(res);
        if (!res->remapped) {
            return;
        }
        pdata = res->remapped;
    }

    res->blob = pdata;
}

void virtio_gpu_fini_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->remapped) {
        virtio_gpu_destroy_udmabuf(res);
    }
}

static void virtio_gpu_free_dmabuf(VirtIOGPU *g, VGPUDMABuf *dmabuf)
{
    struct virtio_gpu_scanout *scanout;

    scanout = &g->parent_obj.scanout[dmabuf->scanout_id];
    dpy_gl_release_dmabuf(scanout->con, &dmabuf->buf);
    QTAILQ_REMOVE(&g->dmabuf.bufs, dmabuf, next);
    close(dmabuf->buf.fd);
    g_free(dmabuf);
}

static VGPUDMABuf
*virtio_gpu_create_dmabuf(VirtIOGPU *g, enum virgl_gpu_resource_type type,
                          uint32_t scanout_id, uint32_t overlay_id,
                          struct virtio_gpu_simple_resource *res,
                          struct virtio_gpu_framebuffer *fb,
                          struct virtio_gpu_rect *r)
{
    VGPUDMABuf *dmabuf;
    int i;

    if (res->dmabuf_fd < 0) {
        return NULL;
    }

    dmabuf = g_new0(VGPUDMABuf, 1);
    dmabuf->buf.width = fb->width;
    dmabuf->buf.height = fb->height;
    dmabuf->buf.stride = fb->stride;
    dmabuf->buf.x = r->x;
    dmabuf->buf.y = r->y;
    dmabuf->buf.scanout_width = r->width;
    dmabuf->buf.scanout_height = r->height;
    dmabuf->buf.fourcc = qemu_pixman_to_drm_format(fb->format);
    dmabuf->buf.fd = qemu_dup(res->dmabuf_fd);
    dmabuf->buf.allow_fences = true;
    dmabuf->buf.draw_submitted = false;
    dmabuf->scanout_id = scanout_id;
    dmabuf->buf.protected = res->protected;

    dmabuf->buf.num_planes = res->num_planes;
    for (i = 0; i < res->num_planes; i++) {
        dmabuf->buf.strides[i] = res->strides[i];
        dmabuf->buf.offsets[i] = res->offsets[i];
    }

    /* Extra props for overlay buffer */
    if (type == VIRGL_GPU_RESOURCE_TYPE_OVERLAY) {
        dmabuf->overlay_id = overlay_id;
        dmabuf->buf.zpos = res->zpos;
        dmabuf->buf.alpha = res->alpha;
        dmabuf->buf.pixel_blend_mode = res->pixel_blend_mode;
        dmabuf->buf.x_coord = res->x_coord;
        dmabuf->buf.y_coord = res->y_coord;
        dmabuf->buf.scale_width = res->scale_width;
        dmabuf->buf.scale_height = res->scale_height;
    }

    QTAILQ_INSERT_HEAD(&g->dmabuf.bufs, dmabuf, next);

    return dmabuf;
}

int virtio_gpu_update_dmabuf(VirtIOGPU *g,
                             uint32_t scanout_id,
                             struct virtio_gpu_simple_resource *res,
                             struct virtio_gpu_framebuffer *fb,
                             struct virtio_gpu_rect *r)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];
    VGPUDMABuf *new_primary, *old_primary = NULL;

    new_primary = virtio_gpu_create_dmabuf(g, VIRGL_GPU_RESOURCE_TYPE_SCANOUT,
                                           scanout_id, UINT32_MAX, res, fb, r);
    if (!new_primary) {
        return -EINVAL;
    }

    if (g->dmabuf.primary[scanout_id]) {
        old_primary = g->dmabuf.primary[scanout_id];
    }

    g->dmabuf.primary[scanout_id] = new_primary;
    qemu_console_resize(scanout->con,
                        new_primary->buf.scanout_width,
                        new_primary->buf.scanout_height);
    dpy_gl_scanout_dmabuf(scanout->con, &new_primary->buf);

    if (old_primary) {
        virtio_gpu_free_dmabuf(g, old_primary);
    }

    return 0;
}
int virtio_gpu_update_dmabuf_overlay(VirtIOGPU *g,
                                     uint32_t scanout_id, uint32_t overlay_id,
                                     struct virtio_gpu_simple_resource *res,
                                     struct virtio_gpu_framebuffer *fb,
                                     struct virtio_gpu_rect *r)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];
    VGPUDMABuf *new_overlay, *old_overlay = NULL;
    int ov_slot;

    new_overlay = virtio_gpu_create_dmabuf(g, VIRGL_GPU_RESOURCE_TYPE_OVERLAY,
                                           scanout_id, overlay_id, res, fb, r);
    if (!new_overlay)
        return -EINVAL;

    ov_slot = virtio_gpu_find_overlay_by_id(g, scanout_id, overlay_id);
    if (ov_slot < 0) {
        virtio_gpu_free_dmabuf(g, new_overlay);
        return -EINVAL;
    }

    if (g->dmabuf.overlay[scanout_id][ov_slot])
        old_overlay = g->dmabuf.overlay[scanout_id][ov_slot];

    g->dmabuf.overlay[scanout_id][ov_slot] = new_overlay;

    /* Present this overlay by console or other method, like weston surface */
    dpy_gl_overlay_dmabuf(scanout->con, &new_overlay->buf, overlay_id);

    if (old_overlay)
        virtio_gpu_free_dmabuf(g, old_overlay);

    return 0;
}
