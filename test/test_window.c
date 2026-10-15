/*
 * test_window.c - the dirty-rectangle window-move loop, verified on the host.
 *
 * This is the piece that decides whether the panel link is ever the
 * bottleneck. Painting a whole 240x170 window every frame costs 82 KB per
 * frame, which the QSPI link cannot absorb; repainting only the symmetric
 * difference of the old and new window regions costs a few KB. The arithmetic
 * has to be exactly right or stale pixels survive the move, and it is the one
 * piece of the port's behaviour that can be tested with no hardware at all.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd.h"

#include <stdio.h>
#include <string.h>

#define BM_W 480
#define BM_H 320
#define BM_RB ((BM_W + 7) / 8)
#define PAD 16

static uint8_t g_fb[BM_H * (BM_RB + PAD)];
static int g_fail;
static int g_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define WIN_W 240
#define WIN_H 170
#define WIN_TITLE_H 18

static qd_bitmap_t g_bitmap = { g_fb, BM_W, BM_H, BM_RB, 0 };
static qd_rect_t g_allow = { 0, 0, 0, 0 };
static bool g_have_allow;
static qd_grafport_t g_port;
static qd_region_rect_t g_clip = { 0 };
static qd_region_rect_t g_new = { 0 };
static int16_t g_win_x, g_win_y;

/* Draws the window contents. Only valid to call with the clip set to the
 * window region, which is how main.c drives it. */
static void paint_window(void)
{
    qd_frame_rect(&g_port, g_new.rect, 1);
    qd_fill_rect(&g_port,
                 qd_rect_make(g_new.rect.left + 1, g_new.rect.top + 1,
                              g_new.rect.right - 1,
                              (int16_t)(g_new.rect.top + WIN_TITLE_H)),
                 QD_ROP_SRC_COPY);
    /* A little content, so the move is not just a flat rectangle. */
    for (int i = 0; i < 6; i++) {
        const int16_t x = (int16_t)(g_new.rect.left + 8 + i * 34);
        const int16_t y = (int16_t)(g_new.rect.top + WIN_TITLE_H + 10);
        if (x + 28 < g_new.rect.right) {
            qd_frame_rect(&g_port, qd_rect_make(x, y, (int16_t)(x + 28), (int16_t)(y + 60)), 1);
        }
    }
}

static void window_moves_to(int16_t nx, int16_t ny, int32_t *out_area)
{
    qd_region_init_rect(&g_new, qd_rect_make(nx, ny, (int16_t)(nx + WIN_W), (int16_t)(ny + WIN_H)));

    /* damage = old XOR new */
    qd_region_xor(&g_clip.reg, &g_clip.reg, &g_new.reg);

    /* Repaint the area we are leaving. */
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &g_clip.reg);
    qd_fill_rect(&g_port, g_clip.rect, QD_ROP_CLEAR);
    qd_port_restore(&g_port);

    /* Paint the window in its new place. */
    qd_port_save(&g_port);
    qd_port_set_clip_region(&g_port, &g_new.reg);
    paint_window();
    qd_port_restore(&g_port);

    *out_area = qd_region_area(&g_clip.reg);

    qd_region_dispose(&g_clip.reg);
    g_clip = g_new;
    qd_region_init_rect(&g_new, g_clip.rect);
}

static void reset(int16_t x, int16_t y)
{
    g_have_allow = false;
    memset(g_fb, 0, sizeof(g_fb));
    qd_port_init(&g_port, &g_bitmap);
    g_win_x = x;
    g_win_y = y;
    qd_region_init_rect(&g_new, qd_rect_make(x, y, (int16_t)(x + WIN_W), (int16_t)(y + WIN_H)));
    qd_region_copy(&g_clip.reg, &g_new.reg);
    g_clip.rect = g_new.rect;
    qd_region_init_rect(&g_new, g_clip.rect);
    /* Initial full paint. */
    qd_port_set_clip_region(&g_port, &g_new.reg);
    paint_window();
    qd_port_set_clip(&g_port, qd_rect_make(0, 0, BM_W, BM_H));
}

/*
 * The whole screen should read as: the moving window, the static window if
 * there is one, or background. Anything else is a pixel the damage region
 * failed to cover, i.e. a stale remnant of the window's previous position.
 */
static void check_no_stale_pixels(const char *what, int16_t wx, int16_t wy)
{
    for (int y = 0; y < BM_H; y++) {
        for (int x = 0; x < BM_W; x++) {
            if (x >= wx && x < wx + WIN_W && y >= wy && y < wy + WIN_H) continue;
            if (g_have_allow && x >= g_allow.left && x < g_allow.right &&
                y >= g_allow.top && y < g_allow.bottom) continue;
            const int got = (g_fb[y * BM_RB + (x >> 3)] >> (7 - (x & 7))) & 1;
            if (got) {
                CHECK(0, "%s: stray set pixel at (%d,%d) outside the window", what, x, y);
                return;
            }
        }
    }
}

/* The window's own content must actually be drawn: the title bar is a solid
 * black band, so it must be set. */
static void check_window_drawn(const char *what, int16_t wx, int16_t wy)
{
    for (int x = wx + 2; x < wx + WIN_W - 2; x += 7) {
        const int y = wy + WIN_TITLE_H / 2;
        const int got = (g_fb[y * BM_RB + (x >> 3)] >> (7 - (x & 7))) & 1;
        if (!got) {
            CHECK(0, "%s: title bar not drawn at (%d,%d)", what, x, y);
            return;
        }
    }
    /* The border itself. */
    for (int x = wx; x < wx + WIN_W; x++) {
        const int got = (g_fb[wy * BM_RB + (x >> 3)] >> (7 - (x & 7))) & 1;
        if (!got) { CHECK(0, "%s: top border missing at x=%d", what, x); return; }
    }
}

int main(void)
{
    printf("qd window-move tests\n");

    /* 1. A long drag, verifying the screen after every step. */
    {
        reset(40, 40);
        const int16_t path[][2] = {
            { 44, 42 }, { 60, 55 }, { 90, 90 }, { 120, 30 }, { 30, 60 },
            { 0, 0 }, { 5, 5 }, { 239, 0 }, { 0, 149 }, { 240, 150 },
            { 100, 100 }, { 41, 41 }
        };
        for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
            int32_t area;
            window_moves_to(path[i][0], path[i][1], &area);
            check_no_stale_pixels("drag", path[i][0], path[i][1]);
            check_window_drawn("drag", path[i][0], path[i][1]);
            /* The damage is the symmetric difference of two window positions,
             * so it can never exceed twice the window area no matter how far
             * the window travelled. */
            CHECK(area <= 2 * WIN_W * WIN_H,
                  "step %zu damage is %d px, larger than 2 windows (%d)",
                  i, (int)area, 2 * WIN_W * WIN_H);
        }
        printf("  ok (drag path, %zu steps)\n", sizeof(path) / sizeof(path[0]));
    }

    /* 2. The damage area must shrink as the move gets smaller. A 1 px move
     *    should touch ~2 window edges, not the whole window. */
    {
        reset(100, 80);
        int32_t ab, as;
        window_moves_to(140, 110, &ab);
        window_moves_to(141, 111, &as);
        CHECK(as < ab, "1 px move damage (%d px) is not smaller than a 40 px move (%d px)",
              (int)as, (int)ab);
        /* A 1 px move of a 240x170 window touches two vertical strips of
         * 240 and two horizontal strips of 170. */
        CHECK(as <= 2 * WIN_H + 2 * WIN_W,
              "1 px move damage %d px, expected <= %d", (int)as, 2 * (WIN_H + WIN_W));
        printf("  ok (40 px move %d px damage, 1 px move %d px)\n", (int)ab, (int)as);
    }

    /* 3. A zero move must repaint nothing outside the window. */
    {
        reset(60, 60);
        int32_t area;
        window_moves_to(60, 60, &area);
        check_no_stale_pixels("no-op move", 60, 60);
        check_window_drawn("no-op move", 60, 60);
        /* Damage for a no-op move is empty, so the caller pushes nothing. */
        CHECK(area == 0, "no-op move produced %d px of damage", (int)area);
        printf("  ok (no-op move produces an empty damage region)\n");
    }

    /* 4. Overlapping windows of different content: drag one over the other and
     *    the static one must come back intact. */
    {
        memset(g_fb, 0, sizeof(g_fb));
        qd_port_init(&g_port, &g_bitmap);
        qd_port_set_clip(&g_port, qd_rect_make(0, 0, BM_W, BM_H));
        qd_fill_rect(&g_port, qd_rect_make(0, 0, BM_W, BM_H), QD_ROP_CLEAR);

        g_allow = qd_rect_make(200, 100, 400, 260);
        g_have_allow = true;

        /* Static window on the right. */
        qd_frame_rect(&g_port, g_allow, 1);
        qd_fill_rect(&g_port, qd_rect_make(201, 101, 399, 118), QD_ROP_SRC_COPY);

        /* Moving window starts left, walks right across it. */
        g_win_x = 20;
        g_win_y = 100;
        qd_region_init_rect(&g_new, qd_rect_make(20, 100, 20 + WIN_W, 100 + WIN_H));
        qd_region_copy(&g_clip.reg, &g_new.reg);
        g_clip.rect = g_new.rect;
        qd_region_init_rect(&g_new, g_clip.rect);
        qd_port_set_clip_region(&g_port, &g_new.reg);
        paint_window();
        qd_port_set_clip(&g_port, qd_rect_make(0, 0, BM_W, BM_H));

        for (int16_t x = 24; x <= 180; x += 4) {
            int32_t area;
            window_moves_to(x, 100, &area);
            check_no_stale_pixels("overlap", x, 100);
            check_window_drawn("overlap", x, 100);
            /* Whatever is left of the static window must still have its title
             * bar and its right border. */
            for (int sx = 360; sx < 400; sx += 5) {
                const int got = (g_fb[109 * BM_RB + (sx >> 3)] >> (7 - (sx & 7))) & 1;
                CHECK(got == 1, "static window title bar lost at x=%d", sx);
                if (!got) break;
            }
            for (int sy = 100; sy < 260; sy += 5) {
                const int got = (g_fb[sy * BM_RB + (399 >> 3)] >> (7 - (399 & 7))) & 1;
                CHECK(got == 1, "static window right border lost at y=%d", sy);
                if (!got) break;
            }
        }
        printf("  ok (moving window crosses a static one, 40 steps)\n");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
