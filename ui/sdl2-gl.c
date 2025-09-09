/*
 * QEMU SDL display driver -- opengl support
 *
 * Copyright (c) 2014 Red Hat
 *
 * Authors:
 *     Gerd Hoffmann <kraxel@redhat.com>
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
#include "qemu/main-loop.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/sdl2.h"

#ifdef CONFIG_VIRGL
#include <virglrenderer.h>
#endif

#include "ui/wayland-overlay.h"

static int sdl2_gl_recovery(struct sdl2_console *scon, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h);
static int sdl2_gl_check_reset_status(struct sdl2_console *scon);

static int sdl2_gl_find_overlay_by_id(struct sdl2_console *scon, uint32_t id)
{
    egl_overlay_fb *ov;
    int i = 0;

    for (i = 0; i < SDL2_GL_MAX_OVERLAY_NUM; i++) {
        ov = &scon->guest_overlay_fbs[i];

        if (ov->valid && (ov->id == id))
            return i;
    }

    for (i = 0; i < SDL2_GL_MAX_OVERLAY_NUM; i++)
        if (!scon->guest_overlay_fbs[i].valid)
            return i;

    return -EINVAL;
}

static void sdl2_set_scanout_mode(struct sdl2_console *scon, bool scanout)
{
    if (scon->scanout_mode == scanout) {
        return;
    }

    scon->scanout_mode = scanout;
    if (!scon->scanout_mode) {
        egl_fb_destroy(&scon->guest_fb);
        if (scon->surface) {
            surface_gl_destroy_texture(scon->gls, scon->surface);
            surface_gl_create_texture(scon->gls, scon->surface);
        }
    }
}

static void sdl2_gl_render_surface(struct sdl2_console *scon)
{
    int ww, wh;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    sdl2_set_scanout_mode(scon, false);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    surface_gl_setup_viewport(scon->gls, scon->surface, ww, wh);

    surface_gl_render_texture(scon->gls, scon->surface);
    SDL_GL_SwapWindow(scon->real_window);
}

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    if (!scon->real_window) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_update_texture(scon->gls, scon->surface, x, y, w, h);
    scon->updates++;
}

void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    DisplaySurface *old_surface = scon->surface;

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_destroy_texture(scon->gls, scon->surface);

    scon->surface = new_surface;


    if (!scon->real_window) {
        sdl2_window_create(scon);
        scon->gls = qemu_gl_init_shader();
    } else if (old_surface &&
               ((surface_width(old_surface)  != surface_width(new_surface)) ||
                (surface_height(old_surface) != surface_height(new_surface)))) {
        sdl2_window_resize(scon);
    }

    surface_gl_create_texture(scon->gls, scon->surface);
    if (is_placeholder(new_surface) && (qemu_console_get_index(dcl->con))) {
        sdl2_window_hide(scon);
    } else {
        sdl2_window_show(scon);
    }
}

void sdl2_gl_refresh(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);

    graphic_hw_update(dcl->con);
    if (scon->updates && scon->real_window) {
        scon->updates = 0;
        sdl2_gl_render_surface(scon);
    }
    sdl2_clean_invalid_sub_windows(scon);
    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND)
        wayland_clean_invalid_sub_windows(scon->wayland_console);

    sdl2_poll_events(scon);
    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND)
        wayland_poll_events(scon->wayland_console);
}

void sdl2_gl_redraw(struct sdl2_console *scon)
{
    assert(scon->opengl);

    if (scon->scanout_mode) {
        /* sdl2_gl_scanout_flush actually only care about
         * the first argument. */
        return sdl2_gl_scanout_flush(&scon->dcl, 0, 0, 0, 0);
    }
    if (scon->surface) {
        sdl2_gl_render_surface(scon);
    }
}

QEMUGLContext sdl2_gl_create_context(DisplayGLCtx *dgc,
                                     QEMUGLParams *params)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext ctx;

    assert(scon->opengl);

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    if (scon->opts->gl == DISPLAYGL_MODE_ON ||
        scon->opts->gl == DISPLAYGL_MODE_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
    } else if (scon->opts->gl == DISPLAYGL_MODE_ES) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, params->major_ver);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, params->minor_ver);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_RESET_NOTIFICATION,
                        SDL_GL_CONTEXT_RESET_LOSE_CONTEXT);

    ctx = SDL_GL_CreateContext(scon->real_window);

    /* If SDL fail to create a GL context and we use the "on" flag,
     * then try to fallback to GLES.
     */
    if (!ctx && scon->opts->gl == DISPLAYGL_MODE_ON) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_ES);
        ctx = SDL_GL_CreateContext(scon->real_window);
    }
    return (QEMUGLContext)ctx;
}

void sdl2_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx)
{
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    SDL_GL_DeleteContext(sdlctx);
}

int sdl2_gl_make_context_current(DisplayGLCtx *dgc,
                                 QEMUGLContext ctx)
{
    struct sdl2_console *scon = container_of(dgc, struct sdl2_console, dgc);
    SDL_GLContext sdlctx = (SDL_GLContext)ctx;

    assert(scon->opengl);

    return SDL_GL_MakeCurrent(scon->real_window, sdlctx);
}

void sdl2_set_dpms(DisplayChangeListener *dcl, uint32_t level)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
#ifdef HAVE_SDL_SETDPMS
    if (scon->real_window)
        SDL_SetDpms(scon->real_window, level);
#endif
}

void sdl2_set_suspend_state(DisplayChangeListener *dcl, bool suspend)
{
    if (suspend) {
        sdl2_subwin_suspend_handler(NULL, NULL);
    } else {
        sdl2_subwin_resume_handler(NULL, NULL);
    }
}

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->w = 0;
    scon->h = 0;
    sdl2_set_scanout_mode(scon, false);
}

void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    scon->x = x;
    scon->y = y;
    scon->w = w;
    scon->h = h;
    scon->y0_top = backing_y_0_top;

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    sdl2_set_scanout_mode(scon, true);
    egl_fb_setup_for_tex(&scon->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

static void sdl2_init_wayland_overlay(struct sdl2_console *scon)
{
    SDL_SysWMinfo wm_info;
    struct wl_display *wl_display = NULL;
    struct wl_surface *wl_surface = NULL;

    if (!scon || !scon->real_window || scon->wayland_console) {
        return;
    }

    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(scon->real_window, &wm_info)) {
        fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return;
    }

    if (wm_info.subsystem != SDL_SYSWM_WAYLAND) {
        /* Not using Wayland, ignore */
        return;
    }

    wl_display = wm_info.info.wl.display;
    wl_surface = wm_info.info.wl.surface;

    if (!wl_display || !wl_surface) {
        fprintf(stderr, "SDL_GetWindowWMInfo: Wayland display or surface not available\n");
        return;
    }

    scon->wayland_console = wayland_console_init(scon, wl_display, wl_surface);

    if (!scon->wayland_console) {
        fprintf(stderr, "Failed to initialize Wayland console\n");
        return;
    }

    fprintf(stdout, "Wayland overlay initialized successfully\n");
}

void sdl2_gl_overlay_dmabuf(DisplayChangeListener *dcl, QemuDmaBuf *dmabuf,
                            uint32_t id)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    assert(scon->opengl);

    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_BLIT){
        egl_overlay_fb *ov;
        int ov_slot;

        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

        egl_dmabuf_import_texture(dmabuf);
        if (!dmabuf->texture) {
            fprintf(stderr, "sdl2_gl_overlay_dmabuf failed fd=%d\n", dmabuf->fd);
            return;
        }

        ov_slot = sdl2_gl_find_overlay_by_id(scon, id);
        if (ov_slot < 0) {
            fprintf(stderr, "invalid overlay id=%d\n", id);
            return;
        }
        ov = &scon->guest_overlay_fbs[ov_slot];
        if (!ov->valid) {
            ov->valid = true;
            ov->id = id;
        }
        ov->width = dmabuf->width;
        ov->height = dmabuf->height;
        ov->alpha = dmabuf->alpha;
        ov->zpos = dmabuf->zpos;
        ov->x_coord = dmabuf->x_coord;
        ov->y_coord = dmabuf->y_coord;

        egl_fb_setup_for_tex(&ov->fb, ov->width, ov->height, dmabuf->texture, false);

        if (dmabuf->allow_fences) {
            ov->fb.dmabuf = dmabuf;
        }
    } else if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_SUBWIN) {
        sdl2_update_sub_window(scon, id, dmabuf->x_coord, dmabuf->y_coord, dmabuf->width, dmabuf->height, 0, 0, dmabuf->width, dmabuf->height, dmabuf->zpos, dmabuf->alpha);

        struct sdl2_sub_window *sub=sdl2_find_sub_window(scon, id);
        if (!sub || !sub->window) {
            fprintf(stderr, "sdl2_gl_overlay_dmabuf: sub-window not found for plane %u , id invalid or not created\n", id);
            return;
        }

        SDL_GL_MakeCurrent(sub->window, sub->gl_context);

        egl_dmabuf_import_texture(dmabuf);
        if (!dmabuf->texture) {
            fprintf(stderr, "sdl2_gl_overlay_dmabuf failed import fd=%d\n", dmabuf->fd);
            return;
        }
        egl_fb_setup_for_tex(&sub->guest_fb,
                            dmabuf->width, dmabuf->height,
                            dmabuf->texture,
                            false);

        if (dmabuf->allow_fences) {
            sub->guest_fb.dmabuf = dmabuf;
        }
        sub->y0_top = dmabuf->y0_top;

        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    } else if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND) {
        if (!scon->wayland_console) {
            sdl2_init_wayland_overlay(scon);
            if (!scon->wayland_console) {
                fprintf(stderr, "sdl2_gl_overlay_dmabuf: wayland console not available\n");
                return;
            }
        }

        struct wayland_sub_window *sub = wayland_find_sub_window(scon->wayland_console, id);
        if (!sub) {
            wayland_update_sub_window(scon->wayland_console, id,
                                     dmabuf->x_coord, dmabuf->y_coord,
                                     dmabuf->width, dmabuf->height,
                                     0, 0, dmabuf->width, dmabuf->height,
                                     dmabuf->zpos, dmabuf->alpha);
            sub = wayland_find_sub_window(scon->wayland_console, id);
            if (!sub) {
                fprintf(stderr, "sdl2_gl_overlay_dmabuf: wayland sub-window creation failed\n");
                return;
            }
        }

        wayland_update_dmabuf(sub, dmabuf);

        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    } else {
        fprintf(stderr, "sdl2_gl_overlay_dmabuf type %d not supported\n", scon->present_type);
    }
}

void sdl2_gl_scanout_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->opengl);
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    if (sdl2_gl_check_reset_status(scon)) {
        if(sdl2_gl_recovery(scon, dmabuf->x, dmabuf->y, dmabuf->width, dmabuf->height)) {
            fprintf(stderr, "sdl2_gl_scanout_dmabuf: GPU recovery failed!\n");
            return;
        } else {
            fprintf(stderr, "sdl2_gl_scanout_dmabuf: GPU recovery succeeded\n");
        }
    }

    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        fprintf(stderr, "sdl2_gl_scanout_dmabuf failed fd=%d\n", dmabuf->fd);
    }

    sdl2_gl_scanout_texture(dcl, dmabuf->texture,
                            false, dmabuf->width, dmabuf->height,
                            0, 0, dmabuf->width, dmabuf->height);

    if (dmabuf->allow_fences) {
        scon->guest_fb.dmabuf = dmabuf;
    }
}

void sdl2_gl_release_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf)
{
    egl_dmabuf_release_texture(dmabuf);
}

bool sdl2_gl_has_dmabuf(DisplayChangeListener *dcl)
{
    return qemu_egl_has_dmabuf();
}

/**
 * Synchronizes and manages flush states of SDL2 sub-windows
 *
 * This function maintains a flush synchronization system for all sub-windows
 * attached to the main console window. It works by:
 *
 * 1. Incrementing the flush counter for each sub-window on every main window flush
 * 2. Checking if any valid sub-window has exceeded the allowed timeout period
 *    without being refreshed (SDL2_GL_OVERLAY_TIMEOUT cycles)
 * 3. Marking timed-out sub-windows as invalid for later cleanup
 * 4. Releasing window grab state to prevent input capture by stale windows
 *
 * Under normal operation, scanout flush and overlay flush are called together,
 * keeping the flush_count at or near zero for active sub-windows. When a
 * sub-window's flush_count exceeds the timeout threshold, it indicates the
 * overlay has likely become invalid (closed by guest, obscured by mouse cursor,
 * or hidden behind transparent layers).
 *
 * During cleanup, it's essential to forcibly release window grab state to
 * prevent the Wayland compositor (like Weston) from losing 'pointer->focus',
 * which could cause input handling issues in the overall desktop environment.
 *
 * @param scon Pointer to the main SDL2 console structure
 */
static void sdl2_gl_subwin_flush_sync(struct sdl2_console *scon){
    if(scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_SUBWIN){
        struct sdl2_sub_window *sub;
        for(sub = scon->sub_windows; sub != NULL; sub=sub->next){
            sub->flush_count++;
            if (sub->valid){
                if (sub->flush_count >= SDL2_GL_OVERLAY_TIMEOUT){
                    sub->valid = false;
                    SDL_SetWindowGrab(scon->real_window, SDL_FALSE);
                }
            }
        }
    } else if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND) {
        wayland_flush_sync_sub_window(scon->wayland_console);
    }
}

static void sdl2_gl_real_scanout_flush(DisplayChangeListener *dcl)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    struct SDL_WindowEglSurfaceData *data = SDL_GetWindowData(scon->real_window,
                                                              SDL_WINDOWEGLSURFACEDATA);
    int ww, wh;

    assert(scon->opengl);
    if (!scon->scanout_mode) {
        return;
    }
    if (!scon->guest_fb.framebuffer) {
        return;
    }

    if (data && scon->guest_fb.dmabuf && data->current_frame_state != scon->guest_fb.dmabuf->protected)
        data->frame_need_protected = scon->guest_fb.dmabuf->protected;

reflush:
    /* Drawing is synchronous here, so no need to use graphic_hw_gl_block. */
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    SDL_GetWindowSize(scon->real_window, &ww, &wh);
    egl_fb_setup_default(&scon->win_fb, ww, wh);
    egl_fb_blit(&scon->win_fb, &scon->guest_fb, !scon->y0_top);

    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND && scon->wayland_console){
        struct wayland_sub_window *sub;
        QLIST_FOREACH(sub, &scon->wayland_console->sub_windows, next) {
            wl_surface_commit(sub->surface_proxy);
        }

        wl_surface_commit(scon->wayland_console->main_surface);
        wl_display_flush(scon->wayland_console->display);
    }

    SDL_GL_SwapWindow(scon->real_window);

    if (sdl2_gl_check_reset_status(scon)) {
        if(!sdl2_gl_recovery(scon, scon->x, scon->y, scon->w, scon->h)) {
            fprintf(stderr, "sdl2_gl_scanout_flush: GPU recovery succeeded\n");
            goto reflush;
        }
        else {
            fprintf(stderr, "sdl2_gl_scanout_flush: GPU recovery failed\n");
            return;
        }
    }

    // sdl2_gl_subwin_flush_sync(scon);
}

void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    scon->x = x;
    scon->y = y;
    scon->w = w;
    scon->h = h;

    if ( scon->wayland_console == NULL || scon->wayland_console->num_sub_windows == 0) {
        sdl2_gl_real_scanout_flush(dcl);
    }

    sdl2_gl_subwin_flush_sync(scon);

}

void sdl2_gl_overlay_flush(DisplayChangeListener *dcl, uint32_t id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint64_t fence_id)
{
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);
    assert(scon->opengl);
    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_BLIT){

        egl_overlay_fb *ov;
        int ov_slot;
        int ww, wh;

        if (!scon->scanout_mode) {
            return;
        }

        ov_slot = sdl2_gl_find_overlay_by_id(scon, id);
        if (ov_slot < 0) {
            fprintf(stderr, "invalid overlay id=%d\n", id);
            return;
        }

        ov = &scon->guest_overlay_fbs[ov_slot];
        assert(ov->valid);

        if (!ov->fb.framebuffer) {
            return;
        }

        /* TODO, only process x/y/w/h, while not the whole overlay FB */

        /* Drawing is synchronous here, so no need to use graphic_hw_gl_block. */
        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

        SDL_GetWindowSize(scon->real_window, &ww, &wh);
        egl_fb_setup_default(&scon->win_fb, ww, wh);
        egl_fb_blit_overlay(&scon->win_fb, &ov->fb, !scon->y0_top, ov->x_coord,
                            ov->y_coord);

        SDL_GL_SwapWindow(scon->real_window);
    } else if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_SUBWIN) {
        struct sdl2_sub_window *sub = sdl2_find_sub_window(scon, id);
        if (!sub || !sub->window) {
            fprintf(stderr,"sdl2_gl_overlay_flush: sub-window not found for plane %u\n",
                    id);
            return;
        }
        if (!sub->guest_fb.framebuffer) {
            return;
        }

        sub->flush_count=0;
        SDL_SetWindowPosition(sub->window, sub->x, sub->y);

        SDL_GL_MakeCurrent(sub->window, sub->gl_context);
        egl_fb_setup_default(&sub->win_fb, sub->src_width, sub->src_height);
        egl_fb_blit(&sub->win_fb, &sub->guest_fb, !sub->y0_top);
        SDL_GL_SwapWindow(sub->window);

        SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    } else if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND) {
        if (!scon->wayland_console) {
            fprintf(stderr, "sdl2_gl_overlay_flush: wayland console not initialized\n");
            return;
        }

        struct wayland_sub_window *sub = wayland_find_sub_window(scon->wayland_console, id);
        if (!sub) {
            fprintf(stderr, "sdl2_gl_overlay_flush: wayland sub-window not found for plane %u\n", id);
            return;
        }

        sub->flush_count = 0;
        wayland_flush_sub_window(sub, x, y, w, h, fence_id);
    } else {
        fprintf(stderr, "sdl2_gl_overlay_flush: type %d not supported\n", scon->present_type);
        return;
    }

    scon->wayland_console->num_flushed++;
    if( scon->wayland_console->num_flushed >= scon->wayland_console->num_sub_windows ) {
        sdl2_gl_real_scanout_flush(dcl);
        scon->wayland_console->num_flushed = 0;
    }
}

void sdl2_gl_set_hdcp(DisplayChangeListener *dcl, uint32_t type, uint32_t mode)
{
#ifdef HAVE_SDL_SETHDCP
    struct sdl2_console *scon = container_of(dcl, struct sdl2_console, dcl);

    assert(scon->real_window);
    SDL_SetWindowProtectedMode(scon->real_window, SDL_RELAXED);
    if (mode == 0)
        SDL_SetWindowProtectedType(scon->real_window, SDL_HDCP_UNPROTECTED);
    else
        SDL_SetWindowProtectedType(scon->real_window, type);
#endif
}

static int sdl2_gl_check_reset_status(struct sdl2_console *scon)
{
    bool has_reset = false;
    int i = 0;

    if (!scon->glGetGraphicsResetStatus)
        return 0;

    /* Assume GPU reset should be finished within 50s */
    for (i = 0; i < 1000000; i++) {
        unsigned status;

        status = scon->glGetGraphicsResetStatus();
        if (status == GL_NO_ERROR)
            break;

        has_reset = true;
        usleep(50);
    }
    if (!has_reset)
        return 0;

    /* if GPU reset is failed, nothing we can do */
    assert(i < 1000000);

    return -EAGAIN;
}

static int sdl2_gl_recovery(struct sdl2_console *scon, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{
    /* 1, Destroy the current contexts and related resources */
#ifdef CONFIG_VIRGL
    virgl_renderer_destroy_ctx0();
#endif
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
    surface_gl_destroy_texture(scon->gls, scon->surface);
    qemu_gl_fini_shader(scon->gls);

    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND && scon->wayland_console) {
        fprintf(stderr, "sdl2_gl_recovery: destroying wayland console before SDL window destruction\n");
        wl_display_roundtrip(scon->wayland_console->display);
        wayland_console_destroy(scon->wayland_console);
        scon->wayland_console = NULL;
    }

    sdl2_window_destroy(scon);

    /* 2, Recreate the contexs and the resources */
    sdl2_window_create(scon);
    scon->gls = qemu_gl_init_shader();
    if (!scon->gls)
        return -ENOMEM;
    surface_gl_create_texture(scon->gls, scon->surface);
    sdl2_window_show(scon);
    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    if (scon->present_type == SDL2_OVERLAY_PRESENT_TYPE_WAYLAND && !scon->wayland_console) {
        sdl2_init_wayland_overlay(scon);

        if (scon->wayland_console) {
            fprintf(stderr, "sdl2_gl_recovery: wayland console recreated successfully\n");
        } else {
            fprintf(stderr, "sdl2_gl_recovery: failed to recreate wayland console\n");
            return -ENOMEM;
        }
    }

#ifdef CONFIG_VIRGL
    if (virgl_renderer_restore_ctx0())
        return -EINVAL;
#endif
    surface_gl_update_texture(scon->gls, scon->surface, x, y, w, h);

    return 0;
}
