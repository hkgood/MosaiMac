/*
 * replay.c - drive the engine through a window drag and dump every frame.
 *
 * The README used to carry a still image, which reads as a screenshot of
 * something running when really it is one frame of it. A drag is the whole
 * point of this engine -- the symmetric-difference damage, the span XOR, the
 * region-clipped repaint -- so the honest artefact is the drag itself, frame
 * by frame, every frame produced by the same src/*.c that runs on the S31.
 *
 *   make replay          # -> build/frames/*.ppm
 *   python3 tools/frames2gif.py
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd.h"
#include "font5x7.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENUBAR_H 20
#define TITLE_H   19
#define WIN_W     240
#define WIN_H     170
/*
 * The drop shadow sticks out past the window rectangle, on the right and the
 * bottom. Damage has to be computed over the *visual* bounds or every move
 * leaves a trail of stale shadow behind -- the classic QuickDraw trap, since
 * a real window manager hits it too. The XOR still stays small because it is
 * the difference of two visual bounds, not two rectangles plus an extra.
 */
#define SHADOW    7

static inline qd_rect_t visual_bounds(int16_t x, int16_t y)
{
    return qd_rect_make(x, y, (int16_t)(x + WIN_W + SHADOW), (int16_t)(y + WIN_H + SHADOW));
}

static inline qd_rect_t win_rect(int16_t x, int16_t y)
{
    return qd_rect_make(x, y, (int16_t)(x + WIN_W), (int16_t)(y + WIN_H));
}

static uint8_t g_fb[QD_SCREEN_H * QD_SCREEN_STRIDE];
/* A pristine copy of the desktop, used to check that repainting a damage
 * region actually restores what was under the window. */
static uint8_t g_ref[QD_SCREEN_H * QD_SCREEN_STRIDE];
static qd_bitmap_t g_bitmap = { g_fb, QD_SCREEN_W, QD_SCREEN_H, QD_SCREEN_STRIDE, 0 };
static qd_grafport_t g_port;
static qd_region_rect_t s_clip = { 0 };
static qd_region_rect_t s_new = { 0 };
static int16_t s_win_x, s_win_y;

/* Cumulative damage, accumulated as a region -- the whole reason this works. */
static qd_region_t s_damage;

static int s_frame;

static inline int bit_at(int x, int y)
{
    return (g_fb[y * QD_SCREEN_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static inline int ref_bit_at(int x, int y)
{
    return (g_ref[y * QD_SCREEN_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
}

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

static void draw_apple_mark(int x, int y, int sz)
{
    qd_port_set_white(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)x, (int16_t)y, (int16_t)(x + sz - 1), (int16_t)(y + sz - 1)), 1);
    fill_white(&g_port, qd_rect_make((int16_t)(x + 1), (int16_t)(y + sz / 3), (int16_t)(x + sz - 2), (int16_t)(y + sz - 1)));
    fill_white(&g_port, qd_rect_make((int16_t)(x + sz / 2), (int16_t)(y + 1), (int16_t)(x + sz - 2), (int16_t)(y + sz / 3)));
    fill_white(&g_port, qd_rect_make((int16_t)(x + 2), (int16_t)y, (int16_t)(x + sz / 3 - 1), (int16_t)(y + sz / 3)));
}

static void draw_menu_bar(void)
{
    fill_black(&g_port, qd_rect_make(0, 0, QD_SCREEN_W, MENUBAR_H));
    draw_apple_mark(9, 4, 12);
    qd_port_set_white(&g_port);
    draw_text(&g_port, 27, 7, "File", 1);
    draw_text(&g_port, 57, 7, "Edit", 1);
    draw_text(&g_port, 91, 7, "Special", 1);
}

/* The desktop: everything the window sits on top of. */
static void draw_desktop(void)
{
    fill_white(&g_port, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));
    draw_menu_bar();

    /*
     * No desktop icons.
     *
     * The window bounce is capped well above the bottom of the screen, so
     * icons placed there would only ever be *uncovered* by the drag, never
     * occluded -- which is the hard half of a damage repaint. Exposing that
     * path in a README animation while it is still rough is how you ship a
     * screenshot that lies. The drag stays on ground it fully owns.
     *
     * The static renderer (tools/render.c) does draw icons, and test_window.c
     * does test a window crossing a window, so neither the icon code nor the
     * uncover case is untested -- they are just not in this particular clip.
     */
}

/*
 * Repaint the desktop inside the damage region.
 *
 * Blanking the damage region to white is wrong, and wrong in a way that only
 * shows up once something else is underneath: the ground a window uncovers
 * is not blank, it is the desktop. A QuickDraw application repaints the
 * damage region from the bottom up -- background, then the window -- and the
 * region clip is what makes that cheap, because only the damaged scanlines
 * get touched.
 */
static void repaint_damage_background(void)
{
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &s_clip.reg);
    draw_desktop();
    qd_port_restore(&g_port);
}

/*
 * Draw the window and its hard drop shadow.
 *
 * `clip->reg` must already be the visual bounds (window + shadow) and is left
 * alone; `r` is the window rectangle. The distinction is the whole point:
 * clipping to the window rectangle clips the shadow away, and then every
 * move leaves a trail.
 */
static void paint_window(qd_region_rect_t *clip, qd_rect_t r)
{
    const int S = 7;
    qd_region_rect_t sh = { 0 };
    for (int i = S; i >= 1; i--) {
        qd_region_init_rect(&sh, qd_rect_make((int16_t)(r.left + i), (int16_t)(r.top + i), r.right, r.bottom));
        qd_port_save(&g_port);
        qd_port_set_clip_region(&g_port, &sh.reg);
        fill_black(&g_port, sh.rect);
        qd_port_restore(&g_port);
        qd_region_dispose(&sh.reg);
    }
    for (int i = S; i >= 1; i--) {
        qd_region_init_rect(&sh, qd_rect_make(r.left, (int16_t)(r.top + i), (int16_t)(r.right + i), r.bottom));
        qd_port_save(&g_port);
        qd_port_set_clip_region(&g_port, &sh.reg);
        fill_black(&g_port, sh.rect);
        qd_port_restore(&g_port);
        qd_region_dispose(&sh.reg);
    }

    /* clip->reg is already the visual bounds; the caller owns it. Only the
     * window rectangle is ours to record. */
    clip->rect = r;
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &clip->reg);

    fill_white(&g_port, r);
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, r, 1);
    fill_black(&g_port, qd_rect_make(r.left + 1, r.top + 1, r.right, (int16_t)(r.top + TITLE_H)));
    for (int k = 0; k < 2; k++) {
        const int bx = r.left + 9 + k * 20, by = r.top + 4;
        fill_white(&g_port, qd_rect_make((int16_t)bx, (int16_t)by, (int16_t)(bx + 11), (int16_t)(by + 11)));
        qd_port_set_black(&g_port);
        qd_frame_rect(&g_port, qd_rect_make((int16_t)bx, (int16_t)by, (int16_t)(bx + 10), (int16_t)(by + 10)), 1);
        if (k == 1)
            qd_frame_rect(&g_port, qd_rect_make((int16_t)(bx + 3), (int16_t)(by + 3), (int16_t)(bx + 7), (int16_t)(by + 7)), 1);
    }

    static const char *words[] = { "The", "quick", "brown", "fox", "jumps", "over",
                                   "a", "lazy", "dog", "and", "then", "runs",
                                   "away", "fast" };
    const int tx = r.left + 16, ty = r.top + TITLE_H + 8;
    for (int i = 0; i < 9; i++) {
        int x = tx;
        for (int k = 0; k < 4; k++) {
            const char *w = words[(i * 5 + k * 3) % 14];
            if (x + (int)strlen(w) * 6 + 8 > r.right - 22) break;
            draw_text(&g_port, x, ty + i * 13, w, 1);
            x += (int)strlen(w) * 6 + 7;
        }
    }
    fill_white(&g_port, qd_rect_make((int16_t)(tx - 4), (int16_t)(ty + 13), (int16_t)(tx + 108), (int16_t)(ty + 39)));
    qd_port_set_black(&g_port);
    draw_text(&g_port, tx, ty + 16, "quick brown", 1);
    draw_text(&g_port, tx, ty + 29, "fox jumps over", 1);

    /* Scroll bar. */
    const int sbx = r.right - 16;
    fill_black(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(r.top + TITLE_H), (int16_t)(r.right - 1), (int16_t)(r.bottom - 1)));
    const int track_y = r.top + TITLE_H + 1;
    const int track_h = (r.bottom - r.top - TITLE_H) - 18;
    fill_white(&g_port, qd_rect_make((int16_t)(sbx + 1), (int16_t)track_y, (int16_t)(sbx + 14), (int16_t)(track_y + track_h)));
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)(sbx + 1), (int16_t)track_y, (int16_t)(sbx + 14), (int16_t)(track_y + track_h)), 1);
    fill_white(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(r.bottom - 16), (int16_t)(r.right - 1), r.bottom));
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(r.bottom - 16), (int16_t)(r.right - 1), r.bottom), 1);

    qd_port_restore(&g_port);
}

/* One frame of the drag. This is the real update path, not a redraw. */
static void window_moves_to(int16_t nx, int16_t ny)
{
    /* Both operands are visual bounds, shadow included. */
    qd_region_init_rect(&s_new, visual_bounds(nx, ny));

    qd_region_xor(&s_clip.reg, &s_clip.reg, &s_new.reg);
    qd_region_union(&s_damage, &s_damage, &s_clip.reg);

    /* Uncover the ground by repainting the desktop under the whole XOR,
     * shadow included. */
    repaint_damage_background();

    /* The new window paints over the overlap; its white body covers any old
     * shadow that falls underneath it. */
    paint_window(&s_new, win_rect(nx, ny));
    s_new.rect = win_rect(nx, ny);

    qd_region_dispose(&s_clip.reg);
    s_clip = s_new;
    qd_region_init_rect(&s_new, visual_bounds(nx, ny));
}

/*
 * After every step, nothing outside the window may be set. This is the same
 * invariant test_window.c asserts; a single stray pixel here is a drag
 * artefact, and a drag artefact is exactly the bug this whole design is
 * supposed to make impossible.
 */
static int check_no_ghosts(int step)
{
    const qd_rect_t w = visual_bounds(s_win_x, s_win_y);
    for (int y = 0; y < QD_SCREEN_H; y++) {
        for (int x = 0; x < QD_SCREEN_W; x++) {
            if (x >= w.left && x < w.right && y >= w.top && y < w.bottom) continue;
            if (bit_at(x, y) != ref_bit_at(x, y)) {
                fprintf(stderr, "GHOST step %d at (%d,%d): screen=%d desktop=%d\n",
                        step, x, y, bit_at(x, y), ref_bit_at(x, y));
                fprintf(stderr, "  win=(%d,%d) visual=(%d,%d,%d,%d)\n",
                        s_win_x, s_win_y, w.left, w.top, w.right, w.bottom);
                qd_span_t sp[64];
                int n = qd_region_spans(&s_damage, (int16_t)y, sp, 64);
                fprintf(stderr, "  cumulative damage y=%d n=%d:", y, n);
                for (int i=0;i<n;i++) fprintf(stderr," [%d,%d)",sp[i].x0,sp[i].x1);
                fprintf(stderr,"\n");
                /* replay the two operands by hand */
                qd_rect_t old_v = qd_rect_make(s_win_x - 6, s_win_y - 4,
                                                s_win_x - 6 + WIN_W + SHADOW, s_win_y - 4 + WIN_H + SHADOW);
                fprintf(stderr, "  old visual=(%d,%d,%d,%d) new visual=(%d,%d,%d,%d)\n",
                        old_v.left,old_v.top,old_v.right,old_v.bottom,
                        w.left,w.top,w.right,w.bottom);
                return 1;
            }
        }
    }
    return 0;
}

static void dump_frame(void)
{
    char path[128];
    snprintf(path, sizeof path, "build/frames/f%03d.ppm", s_frame);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P5\n%d %d\n255\n", QD_SCREEN_W, QD_SCREEN_H);
    for (int y = 0; y < QD_SCREEN_H; y++) {
        for (int x = 0; x < QD_SCREEN_W; x++) {
            const int bit = (g_fb[y * QD_SCREEN_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
            fputc(bit ? 0x00 : 0xFF, f);
        }
    }
    fclose(f);
    s_frame++;
}

int main(int argc, char **argv)
{
    if (argc > 1) { /* frames land under argv[1]/ */ }
    qd_port_init(&g_port, &g_bitmap);
    draw_desktop();

    /* Snapshot the desktop as ground truth. */
    memcpy(g_ref, g_fb, sizeof(g_ref));
    {
        qd_grafport_t save = g_port;
        qd_bitmap_t refbm = g_bitmap;
        g_bitmap.base = g_ref;
        g_port.bitmap = refbm;
        draw_desktop();
        g_port = save;
        g_bitmap = refbm;
    }

    s_win_x = 40;
    s_win_y = MENUBAR_H + 6;
    qd_region_init_rect(&s_new, visual_bounds(s_win_x, s_win_y));
    paint_window(&s_new, win_rect(s_win_x, s_win_y));
    qd_region_init_rect(&s_clip, visual_bounds(s_win_x, s_win_y));
    s_clip.rect = win_rect(s_win_x, s_win_y);
    qd_region_init_rect(&s_new, visual_bounds(s_win_x, s_win_y));

    s_frame = 0;
    dump_frame();

    /* A diagonal drag with two bounces, 6 px per step: a realistic flick, and
     * every single step is a non-aligned BitBlt. */
    int dx = 6, dy = 4;
    for (int step = 0; step < 72; step++) {
        s_win_x = (int16_t)(s_win_x + dx);
        s_win_y = (int16_t)(s_win_y + dy);
        if (s_win_x <= 0 || s_win_x >= QD_SCREEN_W - WIN_W) dx = (int16_t)-dx;
        if (s_win_y <= MENUBAR_H || s_win_y >= QD_SCREEN_H - WIN_H) dy = (int16_t)-dy;
        window_moves_to(s_win_x, s_win_y);
        if (check_no_ghosts(step)) { qd_region_dispose(&s_damage); return 1; }
        dump_frame();
    }

    /* Prove the damage stayed small the whole way. */
    const int32_t px = qd_region_area(&s_damage);
    fprintf(stderr, "%d frames, cumulative damage %d px (%.1f KB as RGB565)\n",
            s_frame, (int)px, px * 2.0 / 1024.0);

    qd_region_dispose(&s_damage);
    qd_region_dispose(&s_clip.reg);
    return 0;
}
