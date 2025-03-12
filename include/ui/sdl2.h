#ifndef SDL2_H
#define SDL2_H

/* Avoid compiler warning because macro is redefined in SDL_syswm.h. */
#undef WIN32_LEAN_AND_MEAN

#include <SDL.h>

/* with Alpine / muslc SDL headers pull in directfb headers
 * which in turn trigger warning about redundant decls for
 * direct_waitqueue_deinit.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"

#include <SDL_syswm.h>

#pragma GCC diagnostic pop

#ifdef CONFIG_SDL_IMAGE
# include <SDL_image.h>
#endif

#include "ui/kbd-state.h"
#ifdef CONFIG_OPENGL
# include "ui/egl-helpers.h"
#endif

#define SDL2_GL_MAX_OVERLAY_NUM    8
typedef struct egl_overlay_fb {
    bool valid;
    egl_fb fb;
    uint32_t width;
    uint32_t height;
    uint32_t id;
    uint32_t alpha;
    uint32_t zpos;

    /* position on desktop */
    uint32_t x_coord;
    uint32_t y_coord;

} egl_overlay_fb;

enum sdl2_overlay_present_type {
    SDL2_OVERLAY_PRESENT_TYPE_NONE = 0,
    /* Blit to Main window */
    SDL2_OVERLAY_PRESENT_TYPE_BLIT,
    /* Present it by sub window */
    SDL2_OVERLAY_PRESENT_TYPE_SUBWIN,
    /* Present it by Wayland surface directly */
    SDL2_OVERLAY_PRESENT_TYPE_WAYLAND,
    SDL2_OVERLAY_PRESENT_TYPE_NUM,
};

/* Only support scanout mode */
struct sdl2_sub_window {
    struct sdl2_console *parent;
    SDL_Window *window;
    SDL_GLContext gl_context;        /* GL context for this window */
    uint32_t id;                     /* Associated Resource ID */
    uint32_t x;                      /* Position relative to parent */
    uint32_t y;
    uint32_t width;                  /* Buffer size */
    uint32_t height;
    uint32_t src_x;                  /* Source rectangle in buffer */
    uint32_t src_y;
    uint32_t src_width;              /* Use for window sizeing too */
    uint32_t src_height;
    uint32_t alpha;                  /* Plane alpha value */
    uint32_t zpos;
    bool valid;                     /* Window visibility state */
    struct sdl2_sub_window *next;   /* Next sub-window in list */
#ifdef CONFIG_OPENGL
    egl_fb guest_fb;
    egl_fb win_fb;
    bool y0_top;
#endif
};

struct sdl2_console {
    DisplayGLCtx dgc;
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Texture *texture;
    SDL_Window *real_window;
    SDL_Renderer *real_renderer;
    int idx;
    int last_vm_running; /* per console for caption reasons */
    int x, y, w, h;
    int hidden;
    int opengl;
    int updates;
    int idle_counter;
    int ignore_hotkeys;
    SDL_GLContext winctx;
    QKbdState *kbd;
#ifdef CONFIG_OPENGL
    QemuGLShader *gls;
    egl_fb guest_fb;
    egl_fb win_fb;
    egl_overlay_fb guest_overlay_fbs[SDL2_GL_MAX_OVERLAY_NUM];
    bool y0_top;
    bool scanout_mode;
#endif
    enum sdl2_overlay_present_type present_type;
    struct sdl2_sub_window *sub_windows;  /* List of sub-windows */
    int num_sub_windows;
};

void sdl2_window_create(struct sdl2_console *scon);
void sdl2_window_destroy(struct sdl2_console *scon);
void sdl2_window_hide(struct sdl2_console *scon);
void sdl2_window_show(struct sdl2_console *scon);
void sdl2_window_resize(struct sdl2_console *scon);
void sdl2_poll_events(struct sdl2_console *scon);

void sdl2_process_key(struct sdl2_console *scon,
                      SDL_KeyboardEvent *ev);

void sdl2_2d_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h);
void sdl2_2d_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface);
void sdl2_2d_refresh(DisplayChangeListener *dcl);
void sdl2_2d_redraw(struct sdl2_console *scon);
bool sdl2_2d_check_format(DisplayChangeListener *dcl,
                          pixman_format_code_t format);

void sdl2_gl_update(DisplayChangeListener *dcl,
                    int x, int y, int w, int h);
void sdl2_gl_switch(DisplayChangeListener *dcl,
                    DisplaySurface *new_surface);
void sdl2_gl_refresh(DisplayChangeListener *dcl);
void sdl2_gl_redraw(struct sdl2_console *scon);

QEMUGLContext sdl2_gl_create_context(DisplayGLCtx *dgc,
                                     QEMUGLParams *params);
void sdl2_gl_destroy_context(DisplayGLCtx *dgc, QEMUGLContext ctx);
int sdl2_gl_make_context_current(DisplayGLCtx *dgc,
                                 QEMUGLContext ctx);

void sdl2_gl_scanout_disable(DisplayChangeListener *dcl);
void sdl2_gl_scanout_texture(DisplayChangeListener *dcl,
                             uint32_t backing_id,
                             bool backing_y_0_top,
                             uint32_t backing_width,
                             uint32_t backing_height,
                             uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h);
void sdl2_gl_scanout_flush(DisplayChangeListener *dcl,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void sdl2_gl_overlay_flush(DisplayChangeListener *dcl, uint32_t id,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void sdl2_gl_scanout_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf);
void sdl2_gl_overlay_dmabuf(DisplayChangeListener *dcl, QemuDmaBuf *dmabuf,
                            uint32_t id);
void sdl2_gl_release_dmabuf(DisplayChangeListener *dcl,
                            QemuDmaBuf *dmabuf);
bool sdl2_gl_has_dmabuf(DisplayChangeListener *dcl);
void sdl2_set_dpms(DisplayChangeListener *dcl, uint32_t level);

void sdl2_gl_set_hdcp(DisplayChangeListener *dcl, uint32_t type, uint32_t mode);

/* Sub-window management functions */
struct sdl2_sub_window *sdl2_find_sub_window(struct sdl2_console *parent,
                            uint32_t plane_id);

void sdl2_update_sub_window(struct sdl2_console *parent,
                            uint32_t plane_id,
                            uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t src_width, uint32_t src_height,
                            uint32_t zpos, uint8_t alpha);


struct sdl2_sub_window * sdl2_create_sub_window(struct sdl2_console *parent,
                                                uint32_t plane_id,
                                                uint32_t width, uint32_t height);


void sdl2_destroy_sub_window(struct sdl2_console *parent,
                            uint32_t plane_id);

#endif /* SDL2_H */
