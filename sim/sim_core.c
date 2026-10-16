/*
 * sim_core.c - portable simulator core.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sim_core.h"

#include <stdlib.h>
#include <string.h>

#include "font5x7.h"

#define MENUBAR_H 20
#define WIN_W     240
#define WIN_H     170

static uint8_t g_fb[SIM_H * QD_SCREEN_STRIDE];
static sim_t *g_sim;   /* only the painter needs it, and there is one screen */

static void fill_black(qd_grafport_t *p, qd_rect_t r)
{
    qd_port_set_black(p);
    qd_fill_rect(p, r, QD_ROP_SRC_COPY);
}
static void fill_white(qd_grafport_t *p, qd_rect_t r)
{
    qd_port_set_white(p);
    qd_fill_rect(p, r, QD_ROP_SRC_COPY);
}

static void draw_apple_mark(qd_grafport_t *p, int x, int y, int sz)
{
    qd_port_set_white(p);
    qd_frame_rect(p, qd_rect_make((int16_t)x, (int16_t)y, (int16_t)(x + sz - 1),
                                  (int16_t)(y + sz - 1)), 1);
    fill_white(p, qd_rect_make((int16_t)(x + 1), (int16_t)(y + sz / 3),
                               (int16_t)(x + sz - 2), (int16_t)(y + sz - 1)));
    fill_white(p, qd_rect_make((int16_t)(x + sz / 2), (int16_t)(y + 1),
                               (int16_t)(x + sz - 2), (int16_t)(y + sz / 3)));
    fill_white(p, qd_rect_make((int16_t)(x + 2), (int16_t)y,
                               (int16_t)(x + sz / 3 - 1), (int16_t)(y + sz / 3)));
}

void sim_paint_desktop(void *ctx, qd_grafport_t *port)
{
    (void)ctx;
    sim_t *s = g_sim;
    if (s == NULL) return;

    fill_white(port, qd_rect_make(0, 0, SIM_W, SIM_H));

    /* Menu bar */
    fill_black(port, qd_rect_make(0, 0, SIM_W, MENUBAR_H));
    draw_apple_mark(port, 9, 4, 12);
    qd_port_set_white(port);
    draw_text(port, 27, 7, "File", 1);
    draw_text(port, 57, 7, "Edit", 1);
    draw_text(port, 91, 7, "Special", 1);

    /* Desktop icons, bottom-left. These are here to earn their keep: a window
     * dragged across them has to restore them, and that is the uncover half
     * of the damage repaint that the GIF deliberately left out. */
    static const char *names[] = { "Disk", "Docs", "System" };
    for (int i = 0; i < 3; i++) {
        const int x = 18 + i * 66;
        const int y = SIM_H - 56;
        qd_port_set_black(port);
        qd_frame_rect(port, qd_rect_make((int16_t)x, (int16_t)y,
                                         (int16_t)(x + 52), (int16_t)(y + 34)), 1);
        qd_frame_rect(port, qd_rect_make((int16_t)(x + 4), (int16_t)(y + 4),
                                         (int16_t)(x + 48), (int16_t)(y + 20)), 1);
        const int tw = (int)strlen(names[i]) * 6;
        draw_text(port, x + (52 - tw) / 2, y + 25, names[i], 1);
    }

    (void)s;
}

void sim_paint_window(void *ctx, qd_grafport_t *port, qd_rect_t f)
{
    (void)ctx;
    {
        (void)f;
        qd_region_rect_t w = { 0 };
        qd_region_init_rect(&w, f);
        qd_port_save(port);
        qd_port_set_clip_region(port, &w.reg);

        static const char *words[] = { "The", "quick", "brown", "fox", "jumps",
                                       "over", "a", "lazy", "dog", "and", "then",
                                       "runs", "away", "fast" };
        const int tx = f.left + 16;
        const int ty = f.top + QD_WIN_TITLE_H + 8;
        qd_port_set_black(port);
        for (int i = 0; i < 9; i++) {
            int x = tx;
            for (int k = 0; k < 4; k++) {
                const char *w2 = words[(i * 5 + k * 3) % 14];
                if (x + (int)strlen(w2) * 6 + 8 > f.right - 22) break;
                draw_text(port, x, ty + i * 13, w2, 1);
                x += (int)strlen(w2) * 6 + 7;
            }
        }
        /* An inverted selection, so drag damage is visible on real content. */
        fill_white(port, qd_rect_make((int16_t)(tx - 4), (int16_t)(ty + 13),
                                      (int16_t)(tx + 108), (int16_t)(ty + 39)));
        qd_port_set_black(port);
        draw_text(port, tx, ty + 16, "quick brown", 1);
        draw_text(port, tx, ty + 29, "fox jumps over", 1);

        /* Scroll bar down the right edge. */
        const int sbx = f.right - 16;
        fill_black(port, qd_rect_make((int16_t)sbx, (int16_t)(f.top + QD_WIN_TITLE_H),
                                      (int16_t)(f.right - 1), (int16_t)(f.bottom - 1)));
        const int ty0 = f.top + QD_WIN_TITLE_H + 1;
        const int th = (f.bottom - f.top - QD_WIN_TITLE_H) - 18;
        fill_white(port, qd_rect_make((int16_t)(sbx + 1), (int16_t)ty0,
                                      (int16_t)(sbx + 14), (int16_t)(ty0 + th)));
        qd_port_set_black(port);
        qd_frame_rect(port, qd_rect_make((int16_t)(sbx + 1), (int16_t)ty0,
                                         (int16_t)(sbx + 14), (int16_t)(ty0 + th)), 1);
        fill_white(port, qd_rect_make((int16_t)sbx, (int16_t)(f.bottom - 16),
                                      (int16_t)(f.right - 1), f.bottom));
        qd_port_set_black(port);
        qd_frame_rect(port, qd_rect_make((int16_t)sbx, (int16_t)(f.bottom - 16),
                                         (int16_t)(f.right - 1), f.bottom), 1);

        qd_port_restore(port);
        qd_region_dispose(&w.reg);
    }
}

void sim_init(sim_t *s)
{
    memset(s, 0, sizeof(*s));
    s->bitmap.base = g_fb;
    s->bitmap.width = SIM_W;
    s->bitmap.height = SIM_H;
    s->bitmap.rowBytes = QD_SCREEN_STRIDE;
    s->bitmap.seed = 0;

    s->panel_bytes = (size_t)SIM_PANEL_W * SIM_PANEL_H * sizeof(uint16_t);
    s->panel = (uint16_t *)calloc(1, s->panel_bytes);
    if (s->panel == NULL) return;

    g_sim = s;
    qd_wm_init(&s->wm, &s->bitmap, qd_rect_make(0, 0, SIM_W, MENUBAR_H));
    qd_wm_set_painters(&s->wm, sim_paint_desktop, sim_paint_window, s);
    s->needs_full = true;   /* the panel buffer is calloc'd, i.e. all black */
    qd_wm_open(&s->wm, qd_rect_make(40, MENUBAR_H + 6, 40 + WIN_W,
                                     MENUBAR_H + 6 + WIN_H));
}

void sim_shutdown(sim_t *s)
{
    qd_wm_dispose(&s->wm);
    free(s->panel);
    s->panel = NULL;
    if (g_sim == s) g_sim = NULL;
}

void sim_mouse(sim_t *s, sim_mouse_kind_t kind, int16_t x, int16_t y)
{
    /* Panel coordinates from the caller; the window manager speaks framebuffer
     * coordinates, which differ by the letterbox height. */
    const int16_t fx = x;
    const int16_t fy = (int16_t)(y - SIM_PANEL_YOFF);

    qd_wm_pointer(&s->wm, fx, fy, kind != SIM_MOUSE_UP);
}

/*
 * Expand a rectangle and scatter it into the panel buffer.
 *
 * qd_expand_dirty_rect() writes a *tightly packed* block -- stride w, not the
 * panel stride -- so it cannot be pointed straight at the panel. The firmware
 * gets away with passing its band straight to the panel driver because the
 * driver applies the stride; here we have to do the scatter ourselves.
 */
static void expand_into_panel(const sim_t *s, qd_rect_t r)
{
    r = qd_rect_intersect(r, qd_rect_make(0, 0, SIM_W, SIM_H));
    if (qd_rect_is_empty(r)) return;
    const int w = r.right - r.left;

    uint16_t row[SIM_W];
    for (int y = r.top; y < r.bottom; y++) {
        qd_expand_dirty_rect(&s->bitmap, qd_rect_make(r.left, (int16_t)y, r.right, (int16_t)(y + 1)), row);
        memcpy(s->panel + (size_t)(y + SIM_PANEL_YOFF) * SIM_PANEL_W + r.left,
               row, (size_t)w * sizeof(uint16_t));
    }
}

void sim_present(sim_t *s)
{
    if (s->panel == NULL) return;

    /* First frame: there is no damage yet, but the panel buffer has never
     * been written and is all black. Without this the window would appear
     * against nothing. */
    if (s->needs_full) {
        expand_into_panel(s, qd_rect_make(0, 0, SIM_W, SIM_H));
        s->needs_full = false;
        return;
    }

    if (qd_wm_dirty(&s->wm)) {
        qd_region_t dmg = { 0 };
        qd_wm_take_damage(&s->wm, &dmg);

        /*
         * Expand into the panel, expanding each scanline's runs and widening
         * to even bounds the way qd_bsp_flush() does on the device. Doing the
         * same maths here is the point: the simulator should fail the same way
         * the hardware would, not paper over it.
         */
        if (qd_region_is_rect(&dmg)) {
            qd_rect_t r = dmg.bounds;
            r.left &= ~1; r.top &= ~1;
            r.right = (int16_t)((r.right + 1) & ~1);
            r.bottom = (int16_t)((r.bottom + 1) & ~1);
            expand_into_panel(s, r);
        } else {
            qd_span_t spans[64];
            for (int y = dmg.bounds.top; y < dmg.bounds.bottom; y++) {
                const int32_t n = qd_region_spans(&dmg, (int16_t)y, spans, 64);
                for (int32_t i = 0; i < n; i++) {
                    qd_rect_t r = qd_rect_make(spans[i].x0, (int16_t)y, spans[i].x1, (int16_t)(y + 1));
                    r.left &= ~1;
                    r.right = (int16_t)((r.right + 1) & ~1);
                    expand_into_panel(s, r);
                }
            }
        }
        qd_region_dispose(&dmg);
    }
}
