#ifndef WAYLAND_OVERLAY_H
#define WAYLAND_OVERLAY_H

#include <wayland-client.h>
#include "ui/xdg-shell-client.h"
#include "ui/linux-dmabuf-v1-client.h"
#include "ui/viewporter-client.h"
#include "ui/zwp_alpha_blend_control_manager_v1-client.h"

#include "ui/console.h"
#include "qemu/queue.h"

#define WAYLAND_OVERLAY_MAX_NUM 8
#define WAYLAND_OVERLAY_MAX_IDLE_FRAMES 3

/* Wayland buffer cache structure */
struct wayland_buffer {
    struct wl_buffer *wl_buffer;
    uint32_t fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
};


struct wayland_console;

struct wayland_sub_window {
    struct wl_surface *surface;
    struct wl_surface *surface_proxy;
    struct wl_subsurface *subsurface;
    struct wl_subsurface *subsurface_proxy;
    struct wl_event_queue *event_queue;

    uint32_t id;
    /* Position relative to parent window */
    uint32_t x;
    uint32_t y;
    /* Buffer size */
    uint32_t width;
    uint32_t height;
    /* Source rectangle position in buffer */
    uint32_t src_x;
    uint32_t src_y;
    /* For window resizing */
    uint32_t src_width;
    uint32_t src_height;
    /* Scale the resource from width x height to scale_width x scale_height */
    uint32_t scale_width;
    uint32_t scale_height;
    /* Alpha value */
    uint32_t alpha;
    uint32_t pixel_blend_mode;
    /* Z-order */
    uint32_t zpos;
    bool valid;
    /* Main window refresh count since last flush */
    uint32_t flush_count;

    QemuDmaBuf *dmabuf;
    bool buffer_queued;

    struct wl_callback *frame_callback;
    struct wl_callback *frame_callback_proxy;
    bool framing;
    uint64_t fence;

    /* Batch flush tracking - marks if this plane is part of a batch operation */
    bool in_batch;

    QLIST_ENTRY(wayland_sub_window) next;

    struct wayland_console *wl_console;
    struct wp_viewport *viewport;
	struct zwp_alpha_blend_control_v1 *abc;
};

/* Sub-window list head using QLIST */
typedef QLIST_HEAD(, wayland_sub_window) wayland_sub_window_list;

/* Main console structure extension - needs to be integrated with existing code */
struct wayland_console {
    void *parent_console;
    struct wl_surface *main_surface;
    struct wl_display *display;
    wayland_sub_window_list sub_windows;
    int num_sub_windows;
    struct zwp_linux_dmabuf_v1 *dmabuf_manager;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    int num_flushed;

    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    struct wayland_sub_window *pointer_focus_sub;
    bool pointer_grab;
    int32_t pointer_last_x;
    int32_t pointer_last_y;

    struct wp_viewporter *viewporter;
    struct zwp_alpha_blend_control_manager_v1 *abc_manager;

    /* Wayland event handling via QEMU main loop fd handler (no pthread) */
    int wl_fd;                          /* Wayland display file descriptor */
    bool wl_fd_registered;              /* Whether fd handler is registered */

    /* Main surface frame callback for scanout fence tracking */
    struct wl_callback *main_frame_callback;
    uint64_t main_fence_id;
    bool main_framing;

    /* Batch flush tracking for synchronized fence signaling */
    uint64_t batch_fence_id;          /* Fence ID for current batch flush */
    uint32_t batch_total_planes;      /* Total planes in current batch */
    uint32_t batch_completed_planes;  /* Completed planes (frame callbacks received) */
    bool batch_in_progress;           /* Whether a batch flush is in progress */
};

/* API function declarations */
struct wayland_sub_window *wayland_find_sub_window(struct wayland_console *parent,
                                  uint32_t plane_id);

void wayland_update_sub_window(struct wayland_console *parent,
                               uint32_t plane_id,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t src_width, uint32_t src_height,
                               uint32_t zpos, uint32_t alpha, uint32_t pixel_blend_mode,
                               uint32_t scale_width, uint32_t scale_height);

struct wayland_sub_window *wayland_create_sub_window(struct wayland_console *parent,
                                     uint32_t plane_id,
                                     uint32_t width, uint32_t height);

void wayland_destroy_sub_window(struct wayland_console *parent,
                                uint32_t plane_id);

/* Used by sdl2_gl_subwin_flush_sync */
void wayland_flush_sync_sub_window(struct wayland_console *parent);

void wayland_clean_invalid_sub_windows(struct wayland_console *parent);

/* Used by sdl2_gl_overlay_dmabuf */
void wayland_update_dmabuf(struct wayland_sub_window *sub,
                           QemuDmaBuf *dmabuf);
/* Used by sdl2_gl_overlay_flush */
int wayland_flush_sub_window(struct wayland_sub_window *sub,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint64_t fence_id);

void wayland_release_buffer(struct wayland_sub_window *sub,
                            struct wayland_buffer *buffer);

void wayland_poll_events(struct wayland_console *console);

/* Suspend/Resume support functions */
void wayland_suspend_sub_windows(struct wayland_console *console);
void wayland_resume_sub_windows(struct wayland_console *console);

/* Initialization and cleanup functions */
struct wayland_console *wayland_console_init(void *parent_console, struct wl_display *display, struct wl_surface *main_surface);
void wayland_console_destroy(struct wayland_console *console);

bool wayland_is_alive(struct wayland_console *console);

/* Flush Wayland display requests to compositor */
void wayland_display_flush(struct wayland_console *console);

/* Frame callback listener for main surface fence tracking */
extern const struct wl_callback_listener main_frame_listener;

#endif /* WAYLAND_OVERLAY_H */
