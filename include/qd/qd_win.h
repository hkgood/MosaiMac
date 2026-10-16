/*
 * qd_win.h - a minimal window manager in the QuickDraw idiom.
 *
 * Why this lives in the core rather than in the application: the window move
 * loop is the part of a Macintosh-style UI most likely to be wrong, and it
 * is exactly the part a developer wants to iterate on without reflashing
 * firmware. Firmware and the desktop simulator drive the same code, so a
 * window that drags cleanly on the Mac drags cleanly on the panel.
 *
 * The whole thing is plain C with no platform dependency. The simulator
 * feeds it mouse events; the firmware feeds it touch events. Nothing here
 * knows which.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QD_WIN_H
#define QD_WIN_H

#include "qd/qd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Title bar height, in the 1-bit font's terms. */
#define QD_WIN_TITLE_H 19
/* Drop shadow offset. The shadow is drawn OUTSIDE the window rectangle, so
 * every damage computation has to use the visual bounds, not the frame. */
#define QD_WIN_SHADOW   7
#define QD_WIN_TITLE_W  80

typedef struct {
    qd_rect_t frame;    /* the window rectangle, shadow excluded */
    bool     visible;
} qd_window_t;

/*
 * A window manager owns one window and one damage region. More windows is a
 * compositor problem; for the 1-bit case a single window plus a desktop is
 * what a working session actually looks like, and stacking is what QuickDraw
 * did not do either.
 */
typedef void (*qd_wm_paint_desktop_fn)(void *ctx, qd_grafport_t *port);
typedef void (*qd_wm_paint_window_fn)(void *ctx, qd_grafport_t *port, qd_rect_t frame);

typedef struct {
    qd_bitmap_t bitmap;
    qd_rect_t   screen;     /* the whole framebuffer, 0,0,w,h */
    qd_rect_t   menu_bar;   /* reserved strip at the top */
    qd_region_t damage;     /* accumulated, in bitmap coordinates */
    qd_window_t win;
    bool        dragging;
    int16_t     grab_dx, grab_dy; /* finger offset inside the window */
    qd_wm_paint_desktop_fn paint_desktop;
    qd_wm_paint_window_fn  paint_window;
    void       *paint_ctx;
} qd_wm_t;

/* --- lifecycle ----------------------------------------------------- */

/* wm->bitmap must outlive the wm. */
void qd_wm_init(qd_wm_t *wm, const qd_bitmap_t *bm, qd_rect_t menu_bar);
void qd_wm_dispose(qd_wm_t *wm);

/*
 * Place the window and paint it. Any damage from a previous window is
 * repainted first, so this is safe to call directly.
 */
void qd_wm_open(qd_wm_t *wm, qd_rect_t frame);

/* --- input --------------------------------------------------------- */

/*
 * A complete pointer state machine, so the firmware's touch handler and the
 * simulator's mouse handler do not each reimplement "grabbed, moved, let go"
 * and, worse, get the clamping subtly different from each other.
 *
 * down/move/up report the button state. Coordinates are in the same space as
 * the framebuffer, so the caller has already removed any bezel offset.
 */
void qd_wm_pointer(qd_wm_t *wm, int16_t x, int16_t y, bool down);

/* The window manager clamps moves to the screen; expose it for tests. */
void qd_wm_drag(qd_wm_t *wm, int16_t nx, int16_t ny);

/* True while a drag is in progress. */
bool qd_wm_is_dragging(const qd_wm_t *wm);

/* --- output -------------------------------------------------------- */

/* The window's visual bounds: frame plus the shadow that sticks out of it. */
qd_rect_t qd_win_visual(qd_rect_t frame);

/* Hands over the accumulated damage and resets it. */
void qd_wm_take_damage(qd_wm_t *wm, qd_region_t *out);

/* True when there is anything to push. */
bool qd_wm_dirty(const qd_wm_t *wm);

/* --- what is on screen --------------------------------------------- */

/* --- what is on screen --------------------------------------------- */

/*
 * Two painters, and they have to be two.
 *
 * Restoring the ground and drawing the window are separate steps: the drag
 * path clears everything the window used to cover, then redraws it in its new
 * place. If the window's contents were painted by the desktop painter, that
 * second step would put the old contents back in the old spot, because the
 * window rectangle has not moved yet when the ground is being restored.
 */
typedef void (*qd_wm_paint_desktop_fn)(void *ctx, qd_grafport_t *port);

/* Draws the window's contents. The port arrives clipped to `frame`. */
typedef void (*qd_wm_paint_window_fn)(void *ctx, qd_grafport_t *port, qd_rect_t frame);

void qd_wm_set_painters(qd_wm_t *wm, qd_wm_paint_desktop_fn desktop,
                        qd_wm_paint_window_fn window, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* QD_WIN_H */
