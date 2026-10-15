/*
 * render.c - draw a classic Macintosh desktop with the QuickDraw engine and
 * write it out as a PPM.
 *
 * Why this exists: the README and the GitHub Page need a picture of the thing
 * working, and a mock-up drawn in another tool would be a picture of a
 * picture. This renders through exactly the same ROP, region and grafport code
 * that runs on the ESP32-S31, so the output is evidence rather than
 * decoration. If a span XOR is wrong, the ghosting shows up in the PNG.
 *
 *   make render          # -> build/render.ppm
 *   python3 tools/ppm2png.py build/render.ppm docs/assets/img/shot.png
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd.h"
#include "font5x7.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static uint8_t g_fb[QD_SCREEN_H * QD_SCREEN_STRIDE];
static qd_bitmap_t g_bitmap = { g_fb, QD_SCREEN_W, QD_SCREEN_H, QD_SCREEN_STRIDE, 0 };
static qd_grafport_t g_port;

#define MENUBAR_H 20
#define TITLE_H  19

/* ------------------------------------------------------------------ *
 * Classic chrome
 * ------------------------------------------------------------------ */

static void draw_apple_mark(int x, int y, int s)
{
    /* Not a logo, just the silhouette the menu bar has always used. */
    qd_frame_rect(&g_port, qd_rect_make(x, y, (int16_t)(x + s - 1), (int16_t)(y + s - 1)), 1);
    qd_fill_rect(&g_port, qd_rect_make(x + 1, (int16_t)(y + s / 3),
                                      (int16_t)(x + s - 2), (int16_t)(y + s - 1)),
                 QD_ROP_SRC_COPY);
    qd_fill_rect(&g_port, qd_rect_make((int16_t)(x + s / 2), (int16_t)(y + 1),
                                      (int16_t)(x + s - 2), (int16_t)(y + s / 3)),
                 QD_ROP_SRC_COPY);
    qd_fill_rect(&g_port, qd_rect_make(x + 2, y, (int16_t)(x + s / 3 - 1), (int16_t)(y + s / 3)),
                 QD_ROP_SRC_COPY);
}

/*
 * Chrome, drawn with an explicit pen choice at every call site.
 *
 * The pen has to be stated rather than implied: qd_port_set_white() followed
 * by a fill erases, which is what QuickDraw meant, but it means a stray
 * white pen silently blanks things rather than drawing them black. Saying it
 * out loud is the point.
 */

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

static void draw_window(qd_region_rect_t *clip, qd_rect_t r, const char *title)
{
    /*
     * A hard black drop shadow, the way the original drew one. There is no
     * alpha in a 1-bit system, so a soft shadow is not available; the sharp
     * offset band is both the authentic look and the only honest option.
     */
    const int S = 7, GAP = 9;
    qd_region_rect_t sh = { 0 };
    for (int i = S; i >= 1; i--) {
        qd_region_init_rect(&sh, qd_rect_make((int16_t)(r.left + i), (int16_t)(r.top + i),
                                             (int16_t)(r.right), (int16_t)(r.bottom)));
        qd_port_save(&g_port);
        qd_port_set_clip_region(&g_port, &sh.reg);
        fill_black(&g_port, sh.rect);
        qd_port_restore(&g_port);
        qd_region_dispose(&sh.reg);
    }
    for (int i = S; i >= 1; i--) {
        qd_region_init_rect(&sh, qd_rect_make((int16_t)r.left, (int16_t)(r.top + i),
                                             (int16_t)(r.right + i), (int16_t)(r.bottom)));
        qd_port_save(&g_port);
        qd_port_set_clip_region(&g_port, &sh.reg);
        fill_black(&g_port, sh.rect);
        qd_port_restore(&g_port);
        qd_region_dispose(&sh.reg);
    }
    (void)GAP;
    qd_region_init_rect(&sh, qd_rect_make((int16_t)(r.right + 2), (int16_t)(r.top + 2),
                                         (int16_t)(r.right + GAP), (int16_t)(r.bottom + 2)));
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &sh.reg);
    fill_black(&g_port, sh.rect);
    qd_port_restore(&g_port);
    qd_region_dispose(&sh.reg);

    qd_region_dispose(&clip->reg);
    qd_region_init_rect(clip, r);
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &clip->reg);

    fill_white(&g_port, r);
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, r, 1);
    fill_black(&g_port, qd_rect_make(r.left + 1, r.top + 1, r.right, (int16_t)(r.top + TITLE_H)));

    /* Close box and zoom box. */
    for (int k = 0; k < 2; k++) {
        const int bx = r.left + 9 + k * 20;
        const int by = r.top + 4;
        fill_white(&g_port, qd_rect_make((int16_t)bx, (int16_t)by,
                                         (int16_t)(bx + 11), (int16_t)(by + 11)));
        qd_port_set_black(&g_port);
        qd_frame_rect(&g_port, qd_rect_make((int16_t)bx, (int16_t)by,
                                           (int16_t)(bx + 10), (int16_t)(by + 10)), 1);
        if (k == 1) {
            qd_frame_rect(&g_port, qd_rect_make((int16_t)(bx + 3), (int16_t)(by + 3),
                                               (int16_t)(bx + 7), (int16_t)(by + 7)), 1);
        }
    }

    /* Title, knocked out of the black band. */
    const int tw = (int)strlen(title) * 6;
    fill_white(&g_port, qd_rect_make((int16_t)((r.left + r.right) / 2 - tw / 2 - 4),
                                     (int16_t)(r.top + 3),
                                     (int16_t)((r.left + r.right) / 2 + tw / 2 + 4),
                                     (int16_t)(r.top + 16)));
    qd_port_set_black(&g_port);
    draw_text(&g_port, (r.left + r.right) / 2 - tw / 2, r.top + 5, title, 1);
    qd_port_restore(&g_port);
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

static void draw_pull_menu(void)
{
    qd_region_rect_t m = { 0 };
    qd_region_init_rect(&m, qd_rect_make(18, MENUBAR_H, 162, MENUBAR_H + 108));
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &m.reg);

    fill_black(&g_port, m.rect);
    fill_white(&g_port, qd_rect_make(m.rect.left + 1, m.rect.top + 1,
                                     m.rect.right - 1, m.rect.bottom - 1));
    fill_black(&g_port, qd_rect_make(m.rect.left + 1, m.rect.top + 1,
                                     m.rect.right - 1, m.rect.top + 17));
    qd_port_set_white(&g_port);
    draw_text(&g_port, m.rect.left + 9, m.rect.top + 6, "File", 1);

    qd_port_set_black(&g_port);
    static const char *items[] = { "New Folder", "Open", "Print", "Close", NULL, "Quit" };
    for (int i = 0; items[i]; i++) {
        draw_text(&g_port, m.rect.left + 20, m.rect.top + 22 + i * 14, items[i], 1);
    }
    /* The separator is drawn as a row of hyphens because there is no dashed
     * line primitive; this is what the original did too. */
    draw_text(&g_port, m.rect.left + 14, m.rect.top + 78, "------", 1);

    /* Check mark, disabled-looking item, and the shortcut column. */
    qd_port_set_white(&g_port);
    draw_text(&g_port, m.rect.left + 8, m.rect.top + 36, "v", 1);
    qd_port_set_black(&g_port);
    draw_text(&g_port, m.rect.right - 40, m.rect.top + 22, "Cmd-N", 1);
    draw_text(&g_port, m.rect.right - 40, m.rect.top + 36, "Cmd-O", 1);
    draw_text(&g_port, m.rect.right - 40, m.rect.top + 64, "Cmd-W", 1);

    qd_port_restore(&g_port);
    qd_region_dispose(&m.reg);
}

static void draw_icons(void)
{
    static const char *names[] = { "Disk", "Docs", "System" };
    for (int i = 0; i < 3; i++) {
        const int x = 18 + i * 66;
        const int y = QD_SCREEN_H - 56;
        qd_port_set_black(&g_port);
        qd_frame_rect(&g_port, qd_rect_make((int16_t)x, (int16_t)y, (int16_t)(x + 52), (int16_t)(y + 34)), 1);
        qd_frame_rect(&g_port, qd_rect_make((int16_t)(x + 4), (int16_t)(y + 4), (int16_t)(x + 48), (int16_t)(y + 20)), 1);
        const int tw = (int)strlen(names[i]) * 6;
        draw_text(&g_port, x + (52 - tw) / 2, y + 25, names[i], 1);
    }
}

static void draw_document(void)
{
    qd_region_rect_t w = { 0 };
    draw_window(&w, qd_rect_make(146, 70, 404, 254), "Untitled");

    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &w.reg);
    const int tx = w.rect.left + 16;
    const int ty = w.rect.top + TITLE_H + 8;

    /* Body text. */
    static const char *words[] = { "The", "quick", "brown", "fox", "jumps", "over",
                                   "a", "lazy", "dog", "and", "then", "runs",
                                   "away", "fast" };
    qd_port_set_black(&g_port);
    for (int i = 0; i < 10; i++) {
        int x = tx;
        for (int k = 0; k < 4; k++) {
            const char *word = words[(i * 5 + k * 3) % 14];
            if (x + (int)strlen(word) * 6 + 8 > w.rect.right - 22) break;
            draw_text(&g_port, x, ty + i * 13, word, 1);
            x += (int)strlen(word) * 6 + 7;
        }
    }

    /* Two inverted lines: the selection. */
    fill_white(&g_port, qd_rect_make((int16_t)(tx - 4), (int16_t)(ty + 26),
                                     (int16_t)(tx + 172), (int16_t)(ty + 52)));
    qd_port_set_black(&g_port);
    draw_text(&g_port, tx, ty + 29, "quick brown fox", 1);
    draw_text(&g_port, tx, ty + 42, "jumps over the", 1);

    /* Scroll bar: a black track, a raised thumb, and the drag box. */
    const int sbx = w.rect.right - 16;
    fill_black(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(w.rect.top + TITLE_H),
                                     (int16_t)(w.rect.right - 1),
                                     (int16_t)(w.rect.bottom - 1)));
    const int track_y = w.rect.top + TITLE_H + 1;
    const int track_h = (w.rect.bottom - w.rect.top - TITLE_H) - 18;
    fill_white(&g_port, qd_rect_make((int16_t)(sbx + 1), (int16_t)track_y,
                                     (int16_t)(sbx + 14), (int16_t)(track_y + track_h)));
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)(sbx + 1), (int16_t)track_y,
                                        (int16_t)(sbx + 14), (int16_t)(track_y + track_h)), 1);
    fill_white(&g_port, qd_rect_make((int16_t)(sbx + 2), (int16_t)(track_y + 2),
                                     (int16_t)(sbx + 13), (int16_t)(track_y + 2 + track_h / 3)));
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)(sbx + 2), (int16_t)(track_y + 2),
                                        (int16_t)(sbx + 13), (int16_t)(track_y + 2 + track_h / 3)), 1);
    fill_white(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(w.rect.bottom - 16),
                                     (int16_t)(w.rect.right - 1), w.rect.bottom));
    qd_port_set_black(&g_port);
    qd_frame_rect(&g_port, qd_rect_make((int16_t)sbx, (int16_t)(w.rect.bottom - 16),
                                        (int16_t)(w.rect.right - 1), w.rect.bottom), 1);

    qd_port_restore(&g_port);
    qd_region_dispose(&w.reg);
}

static void draw_desktop(void)
{
    fill_white(&g_port, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));
    draw_menu_bar();
    draw_icons();
    draw_document();
    draw_pull_menu();
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "render.ppm";

    qd_port_init(&g_port, &g_bitmap);
    draw_desktop();

    FILE *f = fopen(out, "wb");
    if (f == NULL) { perror("fopen"); return 1; }
    fprintf(f, "P5\n%d %d\n255\n", QD_SCREEN_W, QD_SCREEN_H);
    for (int y = 0; y < QD_SCREEN_H; y++) {
        for (int x = 0; x < QD_SCREEN_W; x++) {
            const int bit = (g_fb[y * QD_SCREEN_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1;
            fputc(bit ? 0x00 : 0xFF, f);   /* 1 = black */
        }
    }
    fclose(f);

    fprintf(stderr, "%s: %dx%d\n", out, QD_SCREEN_W, QD_SCREEN_H);
    return 0;
}
