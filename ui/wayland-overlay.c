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
#include "qemu/main-loop.h"

#include "ui/sdl2.h"
#include "ui/input.h"

#include "standard-headers/drm/drm_fourcc.h"

#include <sys/eventfd.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define ALPHA_MAX_UINT16_D 65535.0

/*
 * Fence signal timing configuration.
 *
 * FENCE_SIGNAL_BEFORE_VBLANK_US: How many microseconds before vblank to signal fence.
 *   - Lower value = lower latency but risk missing vblank
 *   - Higher value = safer but higher latency
 *   - Recommended: 500-2000 us (0.5-2 ms)
 */
#define FENCE_SIGNAL_BEFORE_VBLANK_US  500

/* Default vblank period if not yet calibrated (60Hz) */
#define DEFAULT_VBLANK_PERIOD_NS  16666666

static inline uint64_t get_time_ns(void) {
  return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

/**
 * Calculate next vblank time based on calibrated timing.
 * Returns 0 if not calibrated.
 */
static uint64_t predict_next_vblank(struct wayland_console *console, uint64_t now_ns)
{
    if (!console->vblank_calibrated || console->vblank_period_ns == 0) {
        return 0;
    }

    uint64_t elapsed = now_ns - console->last_vblank_ns;
    uint64_t periods = elapsed / console->vblank_period_ns;

    return console->last_vblank_ns + (periods + 1) * console->vblank_period_ns;
}

static void signal_fence(struct wayland_console *console)
{
    if (!console || console->pending_fence_id == 0) {
        return;
    }

    struct sdl2_console *sdlc = console->parent_console;
    uint64_t fence_id = console->pending_fence_id;
    console->pending_fence_id = 0;

    if (sdlc && sdlc->dcl.con) {
        graphic_hw_gl_flush_done(sdlc->dcl.con, fence_id);
    }
}

static void fence_timer_callback(void *opaque)
{
    struct wayland_console *console = opaque;
    signal_fence(console);
}

/**
 * Schedule fence signal at vblank - FENCE_SIGNAL_BEFORE_VBLANK_US.
 * Called when frame callback fires.
 */
static void schedule_fence_for_vblank(struct wayland_console *console)
{
    uint64_t now_ns = get_time_ns();
    uint64_t next_vblank = predict_next_vblank(console, now_ns);

    if (next_vblank == 0) {
        signal_fence(console);
        return;
    }

    uint64_t before_vblank_ns = FENCE_SIGNAL_BEFORE_VBLANK_US * 1000ULL;
    uint64_t signal_time_ns = next_vblank - before_vblank_ns;

    if (signal_time_ns <= now_ns) {
        signal_fence(console);
        return;
    }

    int64_t delay_ns = signal_time_ns - now_ns;
    int64_t delay_ms = delay_ns / 1000000;
    if (delay_ms < 1) delay_ms = 1;  /* Minimum 1ms */

    timer_mod(console->fence_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + delay_ms);

}

static void calibration_feedback_sync_output(void *data,
                                             struct wp_presentation_feedback *feedback,
                                             struct wl_output *output)
{
    /* No action needed for sync_output in this implementation */
}

static void calibration_feedback_discarded(void *data,
                                           struct wp_presentation_feedback *feedback)
{
    struct wayland_console *console = data;

    if (console->calibration_feedback == feedback) {
        wp_presentation_feedback_destroy(feedback);
        console->calibration_feedback = NULL;
    }
}

/**
 * Presentation feedback: presented event
 * Used only for vblank calibration - does NOT trigger fence directly.
 */
static void calibration_feedback_presented(void *data,
                                           struct wp_presentation_feedback *feedback,
                                           uint32_t tv_sec_hi,
                                           uint32_t tv_sec_lo,
                                           uint32_t tv_nsec,
                                           uint32_t refresh,
                                           uint32_t seq_hi,
                                           uint32_t seq_lo,
                                           uint32_t flags)
{
    struct wayland_console *console = data;
    uint64_t now_ns = get_time_ns();

    if (refresh > 0) {
        console->vblank_period_ns = refresh;
        console->last_vblank_ns = now_ns;
        console->vblank_calibrated = true;
    }

    if (console->calibration_feedback == feedback) {
        wp_presentation_feedback_destroy(feedback);
        console->calibration_feedback = NULL;
    }
}

static const struct wp_presentation_feedback_listener calibration_feedback_listener = {
    .sync_output = calibration_feedback_sync_output,
    .presented = calibration_feedback_presented,
    .discarded = calibration_feedback_discarded,
};

static void register_calibration_feedback(struct wayland_console *console)
{
    if (!console->presentation || !console->main_surface) {
        return;
    }

    if (console->calibration_feedback) {
        wp_presentation_feedback_destroy(console->calibration_feedback);
    }

    console->calibration_feedback = wp_presentation_feedback(console->presentation,
                                                             console->main_surface);
    if (console->calibration_feedback) {
        wp_presentation_feedback_add_listener(console->calibration_feedback,
                                              &calibration_feedback_listener,
                                              console);
    }
}

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
    } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
         out->presentation = wl_registry_bind(registry, id, &wp_presentation_interface, 1);
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

static int commit_buffer(struct wayland_sub_window *sub);

/**
 * Main surface frame callback handler.
 * Triggers delayed fence signal based on calibrated vblank timing.
 */
static void main_frame_handle_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct wayland_console *console = data;

    if (!console) {
        return;
    }

    if (console->main_frame_callback) {
        wl_callback_destroy(console->main_frame_callback);
        console->main_frame_callback = NULL;
    }

    if (console->pending_fence_id != 0) {
        schedule_fence_for_vblank(console);
    }
}

const struct wl_callback_listener main_frame_listener = {
    .done = main_frame_handle_done,
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

/*
 * Wayland event handler - called by QEMU main loop when Wayland fd is readable
 * This runs in the main QEMU thread, so all operations are thread-safe.
 * Replaces the previous pthread-based event loop.
 */
static void wayland_fd_read_handler(void *opaque)
{
    struct wayland_console *console = opaque;
    int ret;

    if (!console || !console->display) {
        return;
    }

    /* Dispatch any pending events first (without reading) */
    ret = wl_display_dispatch_pending(console->display);
    if (ret < 0) {
        fprintf(stderr, "wayland_fd_read: dispatch_pending failed: %s\n", strerror(errno));
        return;
    }

    /* Prepare to read from the Wayland fd */
    ret = wl_display_prepare_read(console->display);
    if (ret != 0) {
        /* Events were queued, dispatch them and retry */
        wl_display_dispatch_pending(console->display);
        return;
    }

    /* Read events from the fd (QEMU main loop already determined fd is readable) */
    ret = wl_display_read_events(console->display);
    if (ret < 0) {
        fprintf(stderr, "wayland_fd_read: read_events failed: %s\n", strerror(errno));
        return;
    }

    /* Dispatch the newly read events (frame callbacks will be triggered here) */
    ret = wl_display_dispatch_pending(console->display);
    if (ret < 0) {
        fprintf(stderr, "wayland_fd_read: final dispatch failed: %s\n", strerror(errno));
    }

    /* Flush any pending requests to the compositor */
    wl_display_flush(console->display);
}

/*
 * Optional: Write handler for when we have data to send to Wayland
 * This is called when the Wayland fd becomes writable and we have pending data
 */
static void wayland_fd_write_handler(void *opaque)
{
    struct wayland_console *console = opaque;

    if (!console || !console->display) {
        return;
    }

    /* Flush pending requests */
    int ret = wl_display_flush(console->display);
    if (ret < 0 && errno != EAGAIN) {
        fprintf(stderr, "wayland_fd_write: flush failed: %s\n", strerror(errno));
        return;
    }

    /* If all data was sent, we can stop monitoring for write */
    if (ret >= 0) {
        qemu_set_fd_handler(console->wl_fd, wayland_fd_read_handler, NULL, console);
    }
}

/**
 * Register fence for delayed signaling.
 * Sets up frame callback and presentation feedback for vblank-aligned fence.
 */
void wayland_register_fence(struct wayland_console *console, uint64_t fence_id)
{
    if (!console || !console->main_surface) {
        return;
    }

    console->pending_fence_id = fence_id;

    if (console->main_frame_callback) {
        wl_callback_destroy(console->main_frame_callback);
    }

    console->main_frame_callback = wl_surface_frame(console->main_surface);
    if (console->main_frame_callback) {
        wl_callback_add_listener(console->main_frame_callback,
                                 &main_frame_listener, console);
    }

    register_calibration_feedback(console);
}

/*
 * Public helper to flush Wayland requests from any context
 * This ensures requests are sent to the compositor.
 * If flush would block (EAGAIN), enables write monitoring to retry later.
 */
void wayland_display_flush(struct wayland_console *console)
{
    int ret;

    if (!console || !console->display) {
        return;
    }

    ret = wl_display_flush(console->display);

    /* If flush would block, enable write monitoring */
    if (ret < 0 && errno == EAGAIN) {
        /* Register write handler to flush when fd becomes writable */
        qemu_set_fd_handler(console->wl_fd, wayland_fd_read_handler,
                           wayland_fd_write_handler, console);
    } else if (ret < 0) {
        fprintf(stderr, "wayland_display_flush: failed: %s\n", strerror(errno));
    }
}

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

    /* Initialize fence synchronization */
    console->main_frame_callback = NULL;
    console->pending_fence_id = 0;
    console->presentation = NULL;
    console->calibration_feedback = NULL;

    /* Initialize vblank calibration */
    console->vblank_period_ns = DEFAULT_VBLANK_PERIOD_NS;
    console->last_vblank_ns = 0;
    console->vblank_calibrated = false;

    /* Initialize fence timer */
    console->fence_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                        fence_timer_callback, console);

    /* Initialize Wayland event handling via QEMU main loop fd handler */
    console->wl_fd = wl_display_get_fd(display);
    console->wl_fd_registered = false;
    if (console->wl_fd < 0) {
        fprintf(stderr, "[wayland] Failed to get Wayland display fd\n");
        g_free(console);
        return NULL;
    }

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
        fprintf(stderr, "[wayland] Wayland input no wl_seat bound\n");
    }

    /* Register Wayland fd with QEMU main loop for event-driven processing
     * This replaces the pthread-based event loop for thread safety.
     * The read handler will be called automatically when Wayland events arrive. */
    qemu_set_fd_handler(console->wl_fd, wayland_fd_read_handler, NULL, console);
    console->wl_fd_registered = true;

    fprintf(stderr, "[wayland] console initialized with fd handler (fd=%d)\n", console->wl_fd);

    return console;
}

static void wayland_release_sub_window_resources(struct wayland_sub_window *sub);

void wayland_console_destroy(struct wayland_console *console)
{
    struct wayland_sub_window *sub, *next;

    if (!console) {
        return;
    }

    /* Unregister Wayland fd from QEMU main loop */
    if (console->wl_fd_registered) {
        qemu_set_fd_handler(console->wl_fd, NULL, NULL, NULL);
        console->wl_fd_registered = false;
    }

    /* Clean up frame callback */
    if (console->main_frame_callback) {
        wl_callback_destroy(console->main_frame_callback);
        console->main_frame_callback = NULL;
    }

    /* Clean up calibration feedback */
    if (console->calibration_feedback) {
        wp_presentation_feedback_destroy(console->calibration_feedback);
        console->calibration_feedback = NULL;
    }

    /* Clean up presentation protocol */
    if (console->presentation) {
        wp_presentation_destroy(console->presentation);
        console->presentation = NULL;
    }

    /* Clean up fence timer */
    if (console->fence_timer) {
        timer_free(console->fence_timer);
        console->fence_timer = NULL;
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

static int commit_buffer(struct wayland_sub_window *sub)
{
    if (!sub->buffer_queued) {
        fprintf(stderr, "[wayland] commit_buffer: buffer not queued for plane %u\n", sub->id);
        return -EINVAL;
    }

    struct wayland_buffer *buf = wayland_dmabuf_to_buffer(sub);
    if (buf == NULL) {
        fprintf(stderr, "[wayland] commit_buffer: failed to convert dmabuf to buffer for plane %u\n", sub->id);
        sub->buffer_queued = false;
        return -EINVAL;
    }

    if (!buf->wl_buffer) {
        fprintf(stderr, "[wayland] commit_buffer: no wl_buffer for plane %u\n", sub->id);
        sub->buffer_queued = false;
        return -EINVAL;
    }

    wl_surface_attach(sub->surface, buf->wl_buffer, 0, 0);
    wl_surface_damage_buffer(sub->surface, 0, 0, buf->width, buf->height);

    wl_surface_commit(sub->surface_proxy);
    sub->buffer_queued = false;

    return 0;
}


int wayland_flush_sub_window(struct wayland_sub_window *sub,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    int ret;

    if (!sub) {
        return -ENOENT;
    }

    if (!wayland_is_valid_subwindow(sub) || !sub->valid) {
        return -EINVAL;
    }

    sub->flush_count = 0;
    wl_subsurface_set_position(sub->subsurface_proxy, sub->x, sub->y);

    ret = commit_buffer(sub);
    if (ret < 0) {
        return ret;
    }
    // wl_display_flush(sub->wl_console->display);
    return 0;
}

/*
 * Reorder all subsurfaces according to their zpos values
 * Lower zpos values should be placed above higher zpos values (closer to viewer)
 */
static void wayland_reorder_subsurfaces_by_zpos(struct wayland_console *console)
{
    struct wayland_sub_window *sub, *prev_sub;
    struct wayland_sub_window **sorted_array;
    int i, j, count;

    if (!console || !console->num_sub_windows) {
        return;
    }

    /* Allocate temporary array to hold all subsurface pointers */
    sorted_array = g_new0(struct wayland_sub_window *, console->num_sub_windows);
    if (!sorted_array) {
        fprintf(stderr, "Failed to allocate memory for sorting subsurfaces\n");
        return;
    }

    /* Collect all subsurfaces into the array */
    count = 0;
    QLIST_FOREACH(sub, &console->sub_windows, next) {
        if (count < console->num_sub_windows) {
            sorted_array[count++] = sub;
        }
    }

    /* Simple insertion sort by zpos (lower zpos = higher in z-order, closer to viewer) */
    for (i = 1; i < count; i++) {
        struct wayland_sub_window *key = sorted_array[i];
        j = i - 1;
        while (j >= 0 && sorted_array[j]->zpos > key->zpos) {
            sorted_array[j + 1] = sorted_array[j];
            j--;
        }
        sorted_array[j + 1] = key;
    }

    /* Apply the z-order using wl_subsurface_place_above
     * Start from the bottom (highest zpos) and place each one above the previous
     * The first one (lowest zpos) will be on top */
    for (i = 0; i < count; i++) {
        sub = sorted_array[i];
        if (i == 0) {
            /* First subsurface (lowest zpos) - place it above main surface
             * so it's at the bottom of all subsurfaces */
            if (sub->subsurface_proxy) {
                wl_subsurface_place_above(sub->subsurface_proxy, console->main_surface);
            }
        } else {
            /* Place this subsurface above the previous one (lower zpos) */
            prev_sub = sorted_array[i - 1];
            if (sub->subsurface_proxy && prev_sub->surface_proxy) {
                wl_subsurface_place_above(sub->subsurface_proxy, prev_sub->surface_proxy);
            }
        }
    }

    g_free(sorted_array);

    /* Commit the main surface to apply the z-order changes */
    wl_surface_commit(console->main_surface);
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
    bool is_new_window = false;

    if (!parent || !parent->main_surface) {
        return;
    }

    if (!sub) {
        sub = wayland_create_sub_window(parent, plane_id, width, height);
        if (!sub) {
            fprintf(stderr, "Failed to create sub window for plane %u\n", plane_id);
            return;
        }
        is_new_window = true;
    }

    if (x == 0) sub->x = -src_x;
    else sub->x = x;
    if (y == 0) sub->y = -src_y;
    else sub->y = y;

    bool zpos_changed = (sub->zpos != zpos);

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
    
    /* Reorder subsurfaces if zpos changed or it's a new window
     * Note: new windows are already reordered in wayland_create_sub_window,
     * but we need to reorder again here with the correct zpos value */
    if (zpos_changed || is_new_window) {
        wayland_reorder_subsurfaces_by_zpos(parent);
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
    sub->in_batch = false;
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

    /* Set initial z-order for the new subsurface */
    wayland_reorder_subsurfaces_by_zpos(parent);

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
            sub->buffer_queued = false;
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
