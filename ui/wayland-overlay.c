/*
 * QEMU Wayland overlay support
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "ui/wayland-overlay.h"
#include "ui/console.h"
#include "qemu/queue.h"

#include "ui/sdl2.h"
#include "ui/input.h"

#include "standard-headers/drm/drm_fourcc.h"

#define ALPHA_MAX_UINT16_D 65535.0

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t id, const char *interface,
                            uint32_t version)
{
    struct wayland_console *out = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        out->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 5);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        out->subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        out->dmabuf_manager = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 4);
    } else if (strcmp(interface, "wl_seat") == 0) {
        uint32_t seat_ver = MIN((uint32_t)version, 5u);
        out->seat = wl_registry_bind(registry, id, &wl_seat_interface, seat_ver);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
         out->viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, "zwp_alpha_blend_control_manager_v1") == 0) {
         out->abc_manager = wl_registry_bind(registry, id, &zwp_alpha_blend_control_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t id)
{
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wayland_pointer_send_motion(struct wayland_console *console,
                                        double sx, double sy)
{
    struct sdl2_console *sdlc = console->parent_console;
    if (!sdlc || !sdlc->dcl.con) {
        return;
    }

    int x = (int)lrint(sx);
    int y = (int)lrint(sy);

    if (qemu_input_is_absolute()) {
        qemu_input_queue_abs(sdlc->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(sdlc->surface));
        qemu_input_queue_abs(sdlc->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(sdlc->surface));
        qemu_input_event_sync();
    } else {
        int dx = x - console->pointer_last_x;
        int dy = y - console->pointer_last_y;
        console->pointer_last_x = x;
        console->pointer_last_y = y;
        qemu_input_queue_rel(sdlc->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(sdlc->dcl.con, INPUT_AXIS_Y, dy);
        qemu_input_event_sync();
    }
}

static InputButton wayland_button_to_qemu(uint32_t btn)
{
    switch (btn) {
    case 272: /* BTN_LEFT */
        return INPUT_BUTTON_LEFT;
    case 273: /* BTN_RIGHT */
        return INPUT_BUTTON_RIGHT;
    case 274: /* BTN_MIDDLE */
        return INPUT_BUTTON_MIDDLE;
    case 275: /* BTN_SIDE */
        return INPUT_BUTTON_SIDE;
    case 276: /* BTN_EXTRA */
        return INPUT_BUTTON_EXTRA;
    default:
        return INPUT_BUTTON__MAX; /* unsupported */
    }
}

static void wayland_pointer_send_button(struct wayland_console *console,
                                        uint32_t button, bool down)
{
    struct sdl2_console *sdlc = console->parent_console;
    if (!sdlc || !sdlc->dcl.con) {
        return;
    }
    InputButton qbtn = wayland_button_to_qemu(button);
    if (qbtn == INPUT_BUTTON__MAX) {
        return;
    }
    if( qbtn == INPUT_BUTTON_LEFT ) {
        console->pointer_grab = down;
    }
    qemu_input_queue_btn(sdlc->dcl.con, qbtn, down);
    qemu_input_event_sync();
}

static void wayland_pointer_send_axis(struct wayland_console *console,
                                      uint32_t axis, wl_fixed_t value)
{
    struct sdl2_console *sdlc = console->parent_console;
    if (!sdlc || !sdlc->dcl.con) {
        return;
    }
    double v = wl_fixed_to_double(value);
    InputButton btn;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        btn = (v < 0) ? INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN;
    } else {
        btn = (v < 0) ? INPUT_BUTTON_WHEEL_LEFT : INPUT_BUTTON_WHEEL_RIGHT;
    }
    qemu_input_queue_btn(sdlc->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(sdlc->dcl.con, btn, false);
    qemu_input_event_sync();
}

static struct wayland_sub_window *wayland_sub_from_surface(struct wayland_console *console,
                                                           struct wl_surface *surface)
{
    struct wayland_sub_window *sub;
    QLIST_FOREACH(sub, &console->sub_windows, next) {
        if (sub->surface == surface) {
            return sub;
        }
    }
    return NULL;
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_console *console = data;
    struct wayland_sub_window *sub = wayland_sub_from_surface(console, surface);
    console->pointer_focus_sub = sub;
    int x = wl_fixed_to_int(sx);
    int y = wl_fixed_to_int(sy);
    int gx = x, gy = y;
    if (sub) {
        gx += sub->x;
        gy += sub->y;
    }
    console->pointer_last_x = gx;
    console->pointer_last_y = gy;
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland_console *console = data;
    console->pointer_focus_sub = NULL;
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_console *console = data;
    double x = wl_fixed_to_double(sx);
    double y = wl_fixed_to_double(sy);
    if (console->pointer_grab && x < 10.0) {
        fprintf(stderr, "pointer motion ignored due to grab\n");
        return;
    }
    if( console->pointer_grab && !console->pointer_focus_sub ){
        fprintf(stderr, "pointer motion ignored due to grab without focus\n");
        return;
    }
    double gx = x, gy = y;
    if (console->pointer_focus_sub) {
        gx += console->pointer_focus_sub->x;
        gy += console->pointer_focus_sub->y;
    }
    wayland_pointer_send_motion(console, gx, gy);
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, uint32_t time,
                                  uint32_t button, uint32_t state)
{
    struct wayland_console *console = data;
    wayland_pointer_send_button(console, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct wayland_console *console = data;
    wayland_pointer_send_axis(console, axis, value);
}

static void pointer_handle_frame(void *data, struct wl_pointer *pointer)
{}

static void pointer_handle_axis_source(void *data, struct wl_pointer *pointer,
                                       uint32_t axis_source)
{}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *pointer,
                                     uint32_t time, uint32_t axis)
{}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer,
                                         uint32_t axis, int32_t discrete)
{
    // TODO: support whell_discrete events if needed
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
};

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    if (fd >= 0) {
        close(fd);
    }
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface)
{}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time,
                                uint32_t key, uint32_t state)
{
    struct wayland_console *console = data;
    struct sdl2_console *sdlc = console->parent_console;
    if (!sdlc || !sdlc->kbd) {
        return;
    }
    int qcode = qemu_input_linux_to_qcode(key);
    if (qcode <= 0) {
        return;
    }
    qkbd_state_key_event(sdlc->kbd, qcode, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
                                        int32_t rate, int32_t delay)
{}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     uint32_t caps)
{
    struct wayland_console *console = data;
    bool want_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    bool want_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;

    if (want_pointer && !console->pointer) {
        console->pointer = wl_seat_get_pointer(seat);
        if (console->pointer) {
            wl_pointer_add_listener(console->pointer, &pointer_listener, console);
        }
    } else if (!want_pointer && console->pointer) {
        wl_pointer_release(console->pointer);
        console->pointer = NULL;
    }

    if (want_keyboard && !console->keyboard) {
        console->keyboard = wl_seat_get_keyboard(seat);
        if (console->keyboard) {
            wl_keyboard_add_listener(console->keyboard, &keyboard_listener, console);
        }
    } else if (!want_keyboard && console->keyboard) {
        wl_keyboard_release(console->keyboard);
        console->keyboard = NULL;
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void sub_window_flush_done(struct wayland_sub_window *sub)
{
    if (sub->valid) {
        struct sdl2_console *sdlc = sub->wl_console->parent_console;
        graphic_hw_gl_flush_done(sdlc->dcl.con, sub->fence);
        sub->fence = 0;
    }
}

static void commit_buffer(struct wayland_sub_window *sub, uint64_t fence_id);

static void frame_handle_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct wayland_sub_window *sub = data;
    if (sub->valid) {
        wl_callback_destroy(sub->frame_callback);
        sub->frame_callback = NULL;
        sub->framing = false;

        sub_window_flush_done(sub);
    }
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_handle_done,
};

static void buffer_release_handler(void *data, struct wl_buffer *wl_buffer)
{
    struct wayland_buffer *buf = data;
    if (buf->wl_buffer == wl_buffer) {
        wl_buffer_destroy(wl_buffer);
    }
    g_free(buf);

}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release_handler
};

struct wayland_console *wayland_console_init(void *parent_console,
                                             struct wl_display *display,
                                             struct wl_surface *main_surface)
{
    struct wayland_console *console;

    console = g_new0(struct wayland_console, 1);
    if (!console) {
        fprintf(stderr, "Failed to allocate memory for wayland_console\n");
        return NULL;
    }

    console->parent_console = parent_console;
    console->display = display;
    console->main_surface = main_surface;
    QLIST_INIT(&console->sub_windows);
    console->num_sub_windows = 0;
    console->num_flushed = 0;
    console->pointer_grab = false;
    console->pointer_last_x = 0;
    console->pointer_last_y = 0;

    struct wl_registry *registry = wl_display_get_registry(display);
    if(!registry) {
        fprintf(stderr, "[wayland] Failed to get Wayland registry\n");
        g_free(console);
        return NULL;
    }
    wl_registry_add_listener(registry, &registry_listener, console);
    wl_display_roundtrip(display);

    if (!console->compositor) {
        fprintf(stderr, "[wayland] Global compositor not initialized\n");
        g_free(console);
        return NULL;
    }
    if (!console->subcompositor) {
        fprintf(stderr, "[wayland] Wayland subcompositor not initialized\n");
        g_free(console);
        return NULL;
    }
    if (!console->dmabuf_manager) {
        fprintf(stderr, "[wayland] Wayland dmabuf manager not initialized\n");
        g_free(console);
        return NULL;
    }

    if (console->seat) {
        wl_seat_add_listener(console->seat, &seat_listener, console);
        wl_display_roundtrip(display);
    } else {
        fprintf(stderr, "[wayland] Wayland input no wl_seat bound");
    }

    return console;
}

static void wayland_release_sub_window_resources(struct wayland_sub_window *sub);

void wayland_console_destroy(struct wayland_console *console)
{
    struct wayland_sub_window *sub, *next;

    if (!console) {
        return;
    }

    if (console->pointer) {
        wl_pointer_release(console->pointer);
        console->pointer = NULL;
    }
    if (console->keyboard) {
        wl_keyboard_release(console->keyboard);
        console->keyboard = NULL;
    }
    if (console->seat) {
        wl_seat_release(console->seat);
        console->seat = NULL;
    }

    QLIST_FOREACH_SAFE(sub, &console->sub_windows, next, next) {
        QLIST_REMOVE(sub, next);
        wayland_release_sub_window_resources(sub);
        g_free(sub);
    }

    g_free(console);
}

struct wayland_sub_window *wayland_find_sub_window(struct wayland_console *parent,
                                                   uint32_t plane_id)
{
    struct wayland_sub_window *sub;

    if (!parent) {
        return NULL;
    }

    QLIST_FOREACH(sub, &parent->sub_windows, next) {
        if (sub->id == plane_id) {
            return sub;
        }
    }
    return NULL;
}

static inline bool wayland_is_valid_subwindow(struct wayland_sub_window *sub)
{
    if (!sub || !sub->wl_console || !sub->surface || !sub->subsurface) {
        fprintf(stderr, "Invalid parameters for wayland_is_valid_subwindow\n");
        return false;
    }
    return true;
}

static struct wayland_buffer * wayland_dmabuf_to_buffer(struct wayland_sub_window *sub)
{
    int i;

    if (!sub->dmabuf || !sub->wl_console->dmabuf_manager) {
        fprintf(stderr, "Invalid parameters for wayland_dmabuf_to_buffer\n");
        return NULL;
    }

    QemuDmaBuf *dmabuf = sub->dmabuf;

    struct wayland_console *console = sub->wl_console;
    struct wayland_buffer *newbuf = g_new0(struct wayland_buffer, 1);
    if (!newbuf || !console) {
        fprintf(stderr, "Failed to allocate memory for wayland_buffer\n");
        g_free(newbuf);
        return NULL;
    }
    newbuf->width = dmabuf->width;
    newbuf->height = dmabuf->height;
    newbuf->format = dmabuf->fourcc;
    newbuf->fd = dmabuf->fd;
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(console->dmabuf_manager);
    if (!params) {
        fprintf(stderr, "Failed to create Wayland buffer params\n");
        g_free(newbuf);
        return NULL;
    }

    for (i = 0; i < dmabuf->num_planes; i++) {
        zwp_linux_buffer_params_v1_add(params, dmabuf->fd,
                                       i, /* plane_idx */
                                       dmabuf->offsets[i], /* plane offset */
                                       dmabuf->strides[i], /* plane stride */
                                       (uint32_t)(dmabuf->modifier >> 32),
                                       (uint32_t)(dmabuf->modifier & 0xFFFFFFFF));
    }
    newbuf->wl_buffer = zwp_linux_buffer_params_v1_create_immed(
                                params, dmabuf->width, dmabuf->height, dmabuf->fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!newbuf->wl_buffer) {
        fprintf(stderr, "Failed to create Wayland buffer\n");
        g_free(newbuf);
        return NULL;
    }
    wl_buffer_add_listener(newbuf->wl_buffer, &buffer_listener, newbuf);

    return newbuf;
}

/* We just cache the dmabuf and commit it in wayland frame callback */
void wayland_update_dmabuf(struct wayland_sub_window *sub,
                           QemuDmaBuf *dmabuf)
{
    if (!wayland_is_valid_subwindow(sub) || !dmabuf) {
        return;
    }

    sub->dmabuf = dmabuf;
    sub->buffer_queued = true;
}

static void commit_buffer(struct wayland_sub_window *sub, uint64_t fence_id)
{
    if (sub->buffer_queued) {
        struct wayland_buffer *buf = wayland_dmabuf_to_buffer(sub);
        if (buf != NULL) {
            if (buf->wl_buffer) {
                wl_surface_attach(sub->surface, buf->wl_buffer, 0, 0);
                wl_surface_damage_buffer(sub->surface, 0, 0, buf->width, buf->height);

                sub->fence = fence_id;
                sub->frame_callback = wl_surface_frame(sub->surface);
                wl_callback_add_listener(sub->frame_callback, &frame_listener, sub);

                sub->framing = true;
            }
        }
        sub->buffer_queued = false;
    } else {
        fprintf(stderr, "[wayland] commit_buffer: buffer not queued for fd %d\n", sub->dmabuf->fd);
    }
}


void wayland_flush_sub_window(struct wayland_sub_window *sub,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint64_t fence_id)
{
    if (!wayland_is_valid_subwindow(sub)) {
        return;
    }

    if (sub->valid) {
        wl_subsurface_set_position(sub->subsurface_proxy, sub->x, sub->y);

        if (sub->framing)
        {
            if (wl_display_dispatch_queue_pending(sub->wl_console->display, sub->event_queue) < 0) {
                fprintf(stderr, "Failed to dispatch Wayland display queue\n");
                return;
            }
            if (sub->framing)
                return;
        }

        if (!sub->framing) {
            commit_buffer(sub, fence_id);
        } else {
            sub_window_flush_done(sub);
            fprintf(stderr, "[wayland] flush_sub_window %d already framing\n",
                    sub->dmabuf->fd);
        }
    }

    // wl_display_flush(sub->wl_console->display);
    sub->flush_count = 0;
}

void wayland_update_sub_window(struct wayland_console *parent,
                               uint32_t plane_id,
                               uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t src_width, uint32_t src_height,
                               uint32_t zpos, uint32_t alpha, uint32_t pixel_blend_mode,
                               uint32_t scale_width, uint32_t scale_height)
{
    struct wayland_sub_window *sub = wayland_find_sub_window(parent, plane_id);

    if (!parent || !parent->main_surface) {
        return;
    }

    if (!sub) {
        sub = wayland_create_sub_window(parent, plane_id, width, height);
        if (!sub) {
            fprintf(stderr, "Failed to create sub window for plane %u\n", plane_id);
            return;
        }
    }

    if (x == 0) sub->x = -src_x;
    else sub->x = x;
    if (y == 0) sub->y = -src_y;
    else sub->y = y;

    sub->width = width;
    sub->height = height;
    sub->scale_width = scale_width;
    sub->scale_height = scale_height;
    sub->src_x = src_x;
    sub->src_y = src_y;
    sub->src_width = src_width;
    sub->src_height = src_height;
    sub->alpha = alpha;
    sub->pixel_blend_mode = pixel_blend_mode;
    sub->zpos = zpos;
    sub->valid = true;
    sub->flush_count = 0;

    if (sub->viewport)
        wp_viewport_set_destination(sub->viewport, scale_width, scale_height);

    if (sub->subsurface) {
        wl_subsurface_set_position(sub->subsurface, x, y);
    }
    if (sub->abc) {
        zwp_alpha_blend_control_v1_set_alpha(
            sub->abc, wl_fixed_from_double(alpha / ALPHA_MAX_UINT16_D));
        zwp_alpha_blend_control_v1_set_blend_mode(sub->abc, pixel_blend_mode);
    }
}

static bool wayland_create_sub_window_resources(struct wayland_console *parent,
                                                struct wayland_sub_window *sub)
{
    if (!parent->compositor || !parent->subcompositor) {
        fprintf(stderr, "Wayland compositor or subcompositor not available\n");
        return false;
    }

    sub->surface = wl_compositor_create_surface(parent->compositor);
    if (!sub->surface) {
        fprintf(stderr, "Failed to create Wayland surface\n");
        return false;
    }
    sub->surface_proxy = wl_proxy_create_wrapper(sub->surface);
    if (!sub->surface_proxy) {
        fprintf(stderr, "Failed to create Wayland surface proxy\n");
        return false;
    }
    wl_proxy_set_queue((struct wl_proxy *)sub->surface_proxy, sub->event_queue);


    sub->subsurface = wl_subcompositor_get_subsurface(
        parent->subcompositor, sub->surface_proxy, parent->main_surface);
    if (!sub->subsurface) {
        fprintf(stderr, "Failed to create Wayland subsurface\n");
        return false;
    }
    sub->subsurface_proxy = wl_proxy_create_wrapper(sub->subsurface);
    if (!sub->subsurface_proxy) {
        fprintf(stderr, "Failed to create Wayland subsurface proxy\n");
        return false;
    }
    wl_proxy_set_queue((struct wl_proxy *)sub->subsurface_proxy, sub->event_queue);

    if (parent->viewporter)
        sub->viewport = wp_viewporter_get_viewport(parent->viewporter, sub->surface);
    if (!sub->viewport)
        fprintf(stderr, "Warning: Overlay scaling is not supported!\n");
    if (parent->abc_manager)
		sub->abc = zwp_alpha_blend_control_manager_v1_get_alpha_blend_control(parent->abc_manager, sub->surface);


    wl_subsurface_set_position(sub->subsurface_proxy, sub->x, sub->y);
    wl_subsurface_set_sync(sub->subsurface_proxy);

    return true;
}

static void wayland_release_sub_window_resources(struct wayland_sub_window *sub)
{
    if (!sub) {
        return;
    }

    if (sub->frame_callback) {
        if (sub->fence) {
            sub_window_flush_done(sub);
        }
        wl_callback_destroy(sub->frame_callback);
        sub->frame_callback = NULL;
    }

    if (sub->viewport)
        wp_viewport_destroy(sub->viewport);

    if (sub->abc)
        zwp_alpha_blend_control_v1_destroy(sub->abc);

    if (sub->subsurface) {
        wl_subsurface_destroy(sub->subsurface);
        sub->subsurface = NULL;
    }
    if (sub->surface) {
        wl_surface_destroy(sub->surface);
        sub->surface = NULL;
    }

    if (sub->subsurface_proxy) {
        wl_proxy_wrapper_destroy(sub->subsurface_proxy);
        sub->subsurface_proxy = NULL;
    }
    if (sub->surface_proxy) {
        wl_proxy_wrapper_destroy(sub->surface_proxy);
        sub->surface_proxy = NULL;
    }

    if (sub->event_queue) {
        wl_event_queue_destroy(sub->event_queue);
        sub->event_queue = NULL;
    }
}

struct wayland_sub_window *wayland_create_sub_window(struct wayland_console *parent,
                                                     uint32_t plane_id,
                                                     uint32_t width, uint32_t height)
{
    struct wayland_sub_window *sub;

    if (!parent || !parent->main_surface) {
        fprintf(stderr, "Invalid parent console or main surface\n");
        return NULL;
    }

    sub = g_new0(struct wayland_sub_window, 1);
    if (!sub) {
        fprintf(stderr, "Failed to allocate memory for wayland_sub_window\n");
        return NULL;
    }

    sub->id = plane_id;
    sub->width = width;
    sub->height = height;
    sub->src_width = width;
    sub->src_height = height;
    sub->valid = true;
    sub->flush_count = 0;
    sub->framing = false;
    sub->wl_console = parent;
    sub->event_queue =
        wl_display_create_queue_with_name(parent->display,
                                          g_strdup_printf("sub_window_%u", plane_id));

    if (!wayland_create_sub_window_resources(parent, sub)) {
        wayland_release_sub_window_resources(sub);
        g_free(sub);
        return NULL;
    }

    QLIST_INSERT_HEAD(&parent->sub_windows, sub, next);
    parent->num_sub_windows++;

    wl_surface_commit(sub->surface_proxy);

    return sub;
}


void wayland_destroy_sub_window(struct wayland_console *parent, uint32_t plane_id)
{
    struct wayland_sub_window *sub;

    if (!parent) {
        return;
    }

    QLIST_FOREACH(sub, &parent->sub_windows, next) {
        if (sub->id == plane_id) {

            QLIST_REMOVE(sub, next);
            parent->num_sub_windows--;

            wayland_release_sub_window_resources(sub);

            g_free(sub);
            break;
        }
    }
}

/**
 * Track activity of subsurfaces and mark as invalid if idle too long
 * This is part of the subsurface lifecycle management to automatically
 * clean up unused overlay planes when they're destroyed in the guestVM.
 *
 * See also: sdl2_gl_subwin_flush_sync
 */
void wayland_flush_sync_sub_window(struct wayland_console *parent)
{
    struct wayland_sub_window *sub;

    if (!parent) {
        return;
    }

    QLIST_FOREACH(sub, &parent->sub_windows, next) {
        if (sub->valid) {
            sub->flush_count++;
            if (sub->flush_count >= WAYLAND_OVERLAY_MAX_IDLE_FRAMES) {
                sub->valid = false;
            }
        }
    }
}

void wayland_clean_invalid_sub_windows(struct wayland_console *parent)
{
    struct wayland_sub_window *sub, *next;

    if (!parent) {
        return;
    }

    QLIST_FOREACH_SAFE(sub, &parent->sub_windows, next, next) {
        if (!sub->valid) {
            wayland_destroy_sub_window(parent, sub->id);
        }
    }
}

void wayland_poll_events(struct wayland_console *parent)
{
    if (!parent || !parent->display) {
        return;
    }
    struct wayland_sub_window *sub;
    QLIST_FOREACH(sub, &parent->sub_windows, next) {
        if (sub->event_queue) {
            if (wl_display_dispatch_queue_pending(parent->display, sub->event_queue) < 0) {
                fprintf(stderr, "Failed to dispatch Wayland display queue\n");
                return;
            }
        }
    }
}

void wayland_suspend_sub_windows(struct wayland_console *console)
{
    struct wayland_sub_window *sub, *next;
    int count = 0;

    if (!console) {
        fprintf(stderr, "Console is NULL, nothing to suspend\n");
        return;
    }

    QLIST_FOREACH_SAFE(sub, &console->sub_windows, next, next) {
        count++;
        fprintf(stdout, "WAYLAND: Suspending sub-window %d (ID: %u) - Valid: %s, Surface: %s\n",
               count, sub->id,
               sub->valid ? "yes" : "no",
               sub->surface ? "present" : "none");

        if (sub->valid && sub->surface) {

            if (sub->frame_callback) {
                wl_callback_destroy(sub->frame_callback);
                sub->frame_callback = NULL;
                sub->framing = false;
            }

            if (sub->buffer_queued) {
                sub->buffer_queued = false;
            }
            wayland_destroy_sub_window(console, sub->id);
        }
    }

    if (console->display) {
        wl_display_flush(console->display);
    }

}

void wayland_resume_sub_windows(struct wayland_console *console)
{
    if (!console) {
        fprintf(stderr, "Console is NULL, nothing to resume\n");
        return;
    }

    if (console->display) {
        wl_display_flush(console->display);
    }
}

bool wayland_is_alive(struct wayland_console *console)
{
    if (!console || !console->display) {
        return false;
    }
    /* Prefer explicit error query */
    if (wl_display_get_error(console->display) != 0) {
        return false;
    }
    /* Try a non-blocking flush to detect EPIPE */
    int rc = wl_display_flush(console->display);
    if (rc < 0) {
        return false;
    }
    return true;
}
