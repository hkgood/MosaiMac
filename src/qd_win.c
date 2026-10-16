/*
 * qd_win.c - window manager. See qd_win.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd_win.h"

#include <string.h>

void qd_wm_set_painters(qd_wm_t *wm, qd_wm_paint_desktop_fn desktop,
                        qd_wm_paint_window_fn window, void *ctx)
{
    wm->paint_desktop = desktop;
    wm->paint_window = window;
    wm->paint_ctx = ctx;
}

qd_rect_t qd_win_visual(qd_rect_t frame)
{
    return qd_rect_make(frame.left, frame.top,
                       (int16_t)(frame.right + QD_WIN_SHADOW),
                       (int16_t)(frame.bottom + QD_WIN_SHADOW));
}

void qd_wm_init(qd_wm_t *wm, const qd_bitmap_t *bm, qd_rect_t menu_bar)
{
    memset(wm, 0, sizeof(*wm));
    wm->bitmap = *bm;
    wm->screen = qd_rect_make(0, 0, bm->width, bm->height);
    wm->menu_bar = menu_bar;
    wm->win.frame = qd_rect_make(0, 0, 0, 0);
    wm->win.visible = false;
    qd_region_init_empty(&wm->damage);
}

void qd_wm_dispose(qd_wm_t *wm)
{
    qd_region_dispose(&wm->damage);
}

/* Repaint with the caller's current clip left alone; the painter draws
 * everything and trusts the clip. */
static void wm_paint_desktop(qd_wm_t *wm, qd_grafport_t *port)
{
    if (wm->paint_desktop) wm->paint_desktop(wm->paint_ctx, port);
}

static void wm_paint_window(qd_wm_t *wm, qd_grafport_t *port, qd_rect_t frame)
{
    if (wm->paint_window) wm->paint_window(wm->paint_ctx, port, frame);
}

static void paint_window_frame(qd_wm_t *wm, qd_rect_t frame)
{
    qd_grafport_t port;
    qd_port_init(&port, &wm->bitmap);
    qd_port_set_bounds(&port, wm->screen);

    const int s = QD_WIN_SHADOW;
    for (int i = s; i >= 1; i--) {
        qd_port_set_black(&port);
        qd_fill_rect(&port, qd_rect_make((int16_t)(frame.left + i), (int16_t)(frame.top + i),
                                         frame.right, frame.bottom), QD_ROP_SRC_COPY);
    }
    for (int i = s; i >= 1; i--) {
        qd_port_set_black(&port);
        qd_fill_rect(&port, qd_rect_make(frame.left, (int16_t)(frame.top + i),
                                         (int16_t)(frame.right + i), frame.bottom),
                     QD_ROP_SRC_COPY);
    }

    qd_port_set_white(&port);
    qd_fill_rect(&port, frame, QD_ROP_SRC_COPY);
    qd_port_set_black(&port);
    qd_frame_rect(&port, frame, 1);
    qd_fill_rect(&port, qd_rect_make(frame.left + 1, frame.top + 1, frame.right,
                                     (int16_t)(frame.top + QD_WIN_TITLE_H)),
                 QD_ROP_SRC_COPY);

    /* Close box and zoom box, as the original drew them. */
    for (int k = 0; k < 2; k++) {
        const int bx = frame.left + 9 + k * 20;
        const int by = frame.top + 4;
        qd_port_set_white(&port);
        qd_fill_rect(&port, qd_rect_make((int16_t)bx, (int16_t)by,
                                         (int16_t)(bx + 11), (int16_t)(by + 11)),
                     QD_ROP_SRC_COPY);
        qd_port_set_black(&port);
        qd_frame_rect(&port, qd_rect_make((int16_t)bx, (int16_t)by,
                                          (int16_t)(bx + 10), (int16_t)(by + 10)), 1);
        if (k == 1) {
            qd_frame_rect(&port, qd_rect_make((int16_t)(bx + 3), (int16_t)(by + 3),
                                              (int16_t)(bx + 7), (int16_t)(by + 7)), 1);
        }
    }
}

void qd_wm_open(qd_wm_t *wm, qd_rect_t frame)
{
    qd_grafport_t port;
    qd_port_init(&port, &wm->bitmap);
    qd_port_set_bounds(&port, wm->screen);
    wm_paint_desktop(wm, &port);
    wm->win.frame = frame;
    wm->win.visible = true;
    paint_window_frame(wm, frame);
    wm_paint_window(wm, &port, frame);
}

static bool wm_hit(const qd_wm_t *wm, int16_t x, int16_t y)
{
    const qd_rect_t f = wm->win.frame;
    return x >= f.left && x < f.right && y >= f.top && y < f.bottom;
}

static void wm_clamp(const qd_wm_t *wm, int *nx, int *ny)
{
    const int w = qd_rect_width(wm->win.frame);
    const int h = qd_rect_height(wm->win.frame);
    int lo = 0;
    /* Never let the title bar slide under the menu bar. */
    if (wm->menu_bar.bottom > lo) lo = wm->menu_bar.bottom;
    if (*nx < 0) *nx = 0;
    if (*ny < lo) *ny = lo;
    if (*nx > wm->screen.right  - w) *nx = wm->screen.right  - w;
    if (*ny > wm->screen.bottom - h) *ny = wm->screen.bottom - h;
}

void qd_wm_pointer(qd_wm_t *wm, int16_t x, int16_t y, bool down)
{
    if (!wm->win.visible) return;

    if (down && !wm->dragging) {
        if (!wm_hit(wm, x, y)) return;   /* press landed on the desktop */
        wm->dragging = true;
        wm->grab_dx = (int16_t)(x - wm->win.frame.left);
        wm->grab_dy = (int16_t)(y - wm->win.frame.top);
        return;
    }

    if (!down) {
        wm->dragging = false;
        return;
    }

    if (wm->dragging) {
        int nx = x - wm->grab_dx;
        int ny = y - wm->grab_dy;
        wm_clamp(wm, &nx, &ny);
        qd_wm_drag(wm, (int16_t)nx, (int16_t)ny);
    }
}

bool qd_wm_is_dragging(const qd_wm_t *wm) { return wm->dragging; }

void qd_wm_drag(qd_wm_t *wm, int16_t nx, int16_t ny)
{
    if (!wm->win.visible) return;

    const int w = qd_rect_width(wm->win.frame);
    const int h = qd_rect_height(wm->win.frame);
    const qd_rect_t nf = qd_rect_make(nx, ny, (int16_t)(nx + w), (int16_t)(ny + h));
    if (nf.left == wm->win.frame.left && nf.top == wm->win.frame.top) return;

    /*
     * Damage is the UNION of the old and new visual bounds, not their
     * symmetric difference.
     *
     * XOR is the obvious answer and it is wrong: it names only the pixels
     * that changed *ownership*, so it excludes the area the window newly
     * covers. Those pixels were just drawn into the framebuffer, but nothing
     * marks them for transfer, so the panel keeps showing the old ones. The
     * window appears to land one frame behind, and the intersection -- which
     * is fully repainted -- goes stale too.
     *
     * Union is the honest answer: everything the drag touched, whether it was
     * uncovered or covered, and whether or not the window moved.
     *
     * The visual bounds rather than the frame, because the shadow sticks out
     * past the frame on the right and bottom.
     */
    qd_region_rect_t a, b, d = { 0 };   /* set ops dispose their destination */
    qd_region_init_rect(&a, qd_win_visual(wm->win.frame));
    qd_region_init_rect(&b, qd_win_visual(nf));
    qd_region_union(&d.reg, &a.reg, &b.reg);
    qd_region_union(&wm->damage, &wm->damage, &d.reg);

    /* Restore the ground -- desktop only, the window's old contents must not
     * be painted back over it -- then draw the window afresh where it now
     * belongs, contents included. */
    qd_grafport_t port;
    qd_port_init(&port, &wm->bitmap);
    qd_port_set_bounds(&port, wm->screen);
    qd_port_save(&port);
    qd_port_set_clip_region(&port, &d.reg);
    wm_paint_desktop(wm, &port);
    qd_port_restore(&port);

    wm->win.frame = nf;
    paint_window_frame(wm, nf);
    wm_paint_window(wm, &port, nf);

    qd_region_dispose(&a.reg);
    qd_region_dispose(&b.reg);
    qd_region_dispose(&d.reg);
}

void qd_wm_take_damage(qd_wm_t *wm, qd_region_t *out)
{
    qd_region_copy(out, &wm->damage);
    qd_region_init_empty(&wm->damage);
}

bool qd_wm_dirty(const qd_wm_t *wm)
{
    return qd_region_area(&wm->damage) > 0;
}
