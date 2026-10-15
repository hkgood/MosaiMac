/*
 * test_qd.c - correctness tests for the ROP engine, regions and the expand pass.
 *
 * The reference is a naive per-pixel model. It is far too slow to ship and
 * far too slow to ever be a bottleneck, which is exactly what a reference
 * implementation should be.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fail++;                                                          \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ *
 * Reference model
 * ------------------------------------------------------------------ */

static int ref_get(const uint8_t *buf, int rowbytes, int x, int y)
{
    return (buf[(size_t)y * rowbytes + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static void ref_put(uint8_t *buf, int rowbytes, int x, int y, int v)
{
    uint8_t *p = &buf[(size_t)y * rowbytes + (x >> 3)];
    uint8_t m = (uint8_t)(1u << (7 - (x & 7)));
    *p = (uint8_t)(v ? (*p | m) : (*p & (uint8_t)~m));
}

static int rop_apply(int dst, int src, qd_rop_mode_t mode)
{
    switch (mode) {
    case QD_ROP_SRC_COPY: return src;
    case QD_ROP_SRC_OR:   return dst | src;
    case QD_ROP_SRC_XOR:  return dst ^ src;
    case QD_ROP_SRC_BIC:  return dst & ~src;
    case QD_ROP_SRC_NOT:  return !src;
    default:              return 0;
    }
}

static void ref_copy_bits(uint8_t *db, int drb, qd_rect_t dr,
                          const uint8_t *sb, int srb, qd_rect_t sr,
                          qd_rop_mode_t mode)
{
    for (int y = dr.top; y < dr.bottom; y++) {
        for (int x = dr.left; x < dr.right; x++) {
            const int sx = sr.left + (x - dr.left);
            const int sy = sr.top + (y - dr.top);
            const int s = ref_get(sb, srb, sx, sy);
            const int d = ref_get(db, drb, x, y);
            ref_put(db, drb, x, y, rop_apply(d, s, mode));
        }
    }
}

/* ------------------------------------------------------------------ *
 * Harness
 * ------------------------------------------------------------------ */

#define PAD 8
#define BM_W 128
#define BM_H 40
#define BM_RB ((BM_W + 7) / 8)

static uint8_t g_dst[(BM_H) * (BM_RB + PAD) + 64];
static uint8_t g_src[(BM_H) * (BM_RB + PAD) + 64];
static uint8_t g_dst_ref[(BM_H) * (BM_RB + PAD) + 64];
static uint8_t g_src_ref[(BM_H) * (BM_RB + PAD) + 64];

static void make_bitmap(qd_bitmap_t *bm, uint8_t *buf)
{
    bm->base = buf;
    bm->width = BM_W;
    bm->height = BM_H;
    bm->rowBytes = BM_RB;
    bm->seed = 0;
}

static void fill_pseudorandom(uint8_t *buf, unsigned seed)
{
    unsigned s = seed;
    for (int i = 0; i < BM_H * BM_RB; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(s >> 16);
    }
}

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

static void test_bitblt_modes(void)
{
    printf("test: CopyBits transfer modes\n");
    const qd_rop_mode_t modes[] = { QD_ROP_SRC_COPY, QD_ROP_SRC_OR, QD_ROP_SRC_XOR,
                                   QD_ROP_SRC_BIC, QD_ROP_SRC_NOT };
    const char *names[] = { "copy", "or", "xor", "bic", "not" };

    /*
     * Sweep every phase combination. Bits land wrong whenever the two bit
     * cursors disagree, so the source x offset ranges over a full byte plus
     * one and the destination likewise. Three window widths keep some
     * combinations aligned and some straddling. The first real bug in this
     * engine was a missing phase shift in rop_byte_chunk() that only showed up
     * once the two phases differed, so this sweep stays deliberately wide.
     */
    for (int dx = 0; dx <= 15; dx++) {
        for (int sx = 0; sx <= 15; sx++) {
            for (int wi = 0; wi < 3; wi++) {
                const int w = 8 + wi * 17; /* 8, 25, 42 */
                if (dx + w > BM_W || sx + w > BM_W) continue;
                const int dy = wi * 3, sy = 9 - wi * 2;
                if (dy + 8 > BM_H || sy + 8 > BM_H) continue;

                const qd_rect_t dr = qd_rect_make((int16_t)dx, (int16_t)dy,
                                                  (int16_t)(dx + w), (int16_t)(dy + 8));
                const qd_rect_t sr = qd_rect_make((int16_t)sx, (int16_t)sy,
                                                  (int16_t)(sx + w), (int16_t)(sy + 8));

                for (int m = 0; m < 5; m++) {
                    fill_pseudorandom(g_src, 0xC0FFEEu + (unsigned)(dx * 31 + sx * 7 + m));
                    fill_pseudorandom(g_dst, 0xBEEFu + (unsigned)(dx * 17 + sx * 3 + m));
                    memcpy(g_src_ref, g_src, sizeof(g_src_ref));
                    memcpy(g_dst_ref, g_dst, sizeof(g_dst_ref));

                    qd_bitmap_t db, sb;
                    make_bitmap(&db, g_dst);
                    make_bitmap(&sb, g_src);
                    qd_copy_bits(&db, dr, &sb, sr, NULL, modes[m]);
                    ref_copy_bits(g_dst_ref, BM_RB, dr, g_src_ref, BM_RB, sr, modes[m]);

                    for (int y = 0; y < BM_H; y++) {
                        for (int x = 0; x < BM_W; x++) {
                            const int got = ref_get(g_dst, BM_RB, x, y);
                            const int want = ref_get(g_dst_ref, BM_RB, x, y);
                            if (got != want) {
                                CHECK(0,
                                      "%s dx=%d sx=%d w=%d dy=%d sy=%d: "
                                      "pixel (%d,%d) = %d, want %d",
                                      names[m], dx, sx, w, dy, sy, x, y, got, want);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
    printf("  ok (16x16 phase combinations x 3 widths x 5 modes)\n");
}

static void test_bitblt_region_clip(void)
{
    printf("test: CopyBits with a non-rectangular region clip\n");

    /* An L-shaped region built by XOR of two rects:
     *   A = [0,70) x [0,40)
     *   B = [30,96) x [0,20)
     *   L = A xor B  ->  [0,30)+[70,96) for y<20, and just [0,70) for y>=20.
     * Membership is computed analytically below so the expectation does not
     * depend on the region code under test. */
    qd_region_rect_t ra, rb, rr = { 0 };
    qd_region_init_rect(&ra, qd_rect_make(0, 0, 70, 40));
    qd_region_init_rect(&rb, qd_rect_make(30, 0, 96, 20));
    qd_region_xor(&rr.reg, &ra.reg, &rb.reg);

    CHECK(!qd_region_is_rect(&rr.reg), "L region should not be a plain rect");

    qd_span_t spans[64];
    const int32_t n = qd_region_spans(&rr.reg, 5, spans, 64);
    CHECK(n == 2, "L region scanline 5 should have 2 spans, got %d", (int)n);
    if (n == 2) {
        CHECK(spans[0].x0 == 0 && spans[0].x1 == 30,
              "scanline 5 span 0 = [%d,%d), want [0,30)", spans[0].x0, spans[0].x1);
        CHECK(spans[1].x0 == 70 && spans[1].x1 == 96,
              "scanline 5 span 1 = [%d,%d), want [70,96)", spans[1].x0, spans[1].x1);
    }
    /* B stops at y=20, so above that L is just A -- one span [0,70), not
     * the full width. */
    const int32_t n2 = qd_region_spans(&rr.reg, 30, spans, 64);
    CHECK(n2 == 1 && spans[0].x0 == 0 && spans[0].x1 == 70,
          "scanline 30 should be one span [0,70), got %d spans", (int)n2);

    const qd_rop_mode_t modes[] = { QD_ROP_SRC_COPY, QD_ROP_SRC_OR,
                                    QD_ROP_SRC_XOR, QD_ROP_SRC_BIC, QD_ROP_SRC_NOT };
    const qd_rect_t dr = qd_rect_make(0, 0, BM_W, BM_H);
    const qd_rect_t sr = qd_rect_make(0, 0, BM_W, BM_H);

    for (int m = 0; m < 5; m++) {
        fill_pseudorandom(g_src, 0xABCDu + (unsigned)m);
        fill_pseudorandom(g_dst, 0x1111u + (unsigned)m);
        memcpy(g_src_ref, g_src, sizeof(g_src_ref));
        memcpy(g_dst_ref, g_dst, sizeof(g_dst_ref));

        qd_bitmap_t db, sb;
        make_bitmap(&db, g_dst);
        make_bitmap(&sb, g_src);
        qd_copy_bits(&db, dr, &sb, sr, &rr.reg, modes[m]);

        for (int y = 0; y < BM_H; y++) {
            for (int x = 0; x < BM_W; x++) {
                const int in_a = (x >= 0 && x < 70 && y >= 0 && y < 40);
                const int in_b = (x >= 30 && x < 96 && y >= 0 && y < 20);
                const int in_l = in_a ^ in_b;

                const int d_orig = ref_get(g_dst_ref, BM_RB, x, y);
                const int s = ref_get(g_src_ref, BM_RB, x, y);
                const int want = in_l ? rop_apply(d_orig, s, modes[m]) : d_orig;
                const int got = ref_get(g_dst, BM_RB, x, y);

                if (got != want) {
                    CHECK(0, "mode %d pixel (%d,%d) = %d, want %d (in_L=%d)",
                          (int)modes[m], x, y, got, want, in_l);
                    qd_region_dispose(&rr.reg);
                    return;
                }
            }
        }
    }
    printf("  ok\n");
    qd_region_dispose(&rr.reg);
}

static void test_region_ops(void)
{
    printf("test: region union / intersect / xor / offset\n");
    qd_region_rect_t a, b, out = { 0 };

    qd_region_init_rect(&a, qd_rect_make(0, 0, 40, 40));
    qd_region_init_rect(&b, qd_rect_make(20, 0, 60, 40));

    qd_region_union(&out.reg, &a.reg, &b.reg);
    qd_span_t sp[8];
    int32_t n = qd_region_spans(&out.reg, 10, sp, 8);
    CHECK(n == 1 && sp[0].x0 == 0 && sp[0].x1 == 60, "union should be [0,60)");
    qd_region_dispose(&out.reg);

    qd_region_intersect(&out.reg, &a.reg, &b.reg);
    n = qd_region_spans(&out.reg, 10, sp, 8);
    CHECK(n == 1 && sp[0].x0 == 20 && sp[0].x1 == 40, "intersect should be [20,40)");
    qd_region_dispose(&out.reg);

    qd_region_xor(&out.reg, &a.reg, &b.reg);
    n = qd_region_spans(&out.reg, 10, sp, 8);
    CHECK(n == 2 && sp[0].x0 == 0 && sp[0].x1 == 20 && sp[1].x0 == 40 && sp[1].x1 == 60,
          "xor should be [0,20)+[40,60), got %d spans", (int)n);
    qd_region_dispose(&out.reg);

    /* Offset a general region and confirm the spans moved. */
    qd_region_rect_t c = { 0 };
    qd_region_xor(&c.reg, &a.reg, &b.reg);
    qd_region_offset(&c.reg, 5, 3);
    n = qd_region_spans(&c.reg, 13, sp, 8);
    CHECK(n == 2 && sp[0].x0 == 5 && sp[0].x1 == 25,
          "offset region scanline 13 span0 = [%d,%d), want [5,25)", n ? sp[0].x0 : -1,
          n ? sp[0].x1 : -1);
    qd_region_dispose(&c.reg);

    printf("  ok\n");
}

static void test_fill_span_exhaustive(void)
{
    printf("test: fill_span_1bpp, exhaustive offset x length sweep\n");

    /* Regression guard. The mask used to ignore the bit phase, which was
     * invisible whenever a span ended on a byte boundary and wrong the moment
     * the head or tail got clipped. Sweep every offset and length so there is
     * no phase combination left untested. */
    for (int x0 = 0; x0 < BM_W; x0++) {
        for (int n = 1; x0 + n <= BM_W; n++) {
            memset(g_dst, 0x00, sizeof(g_dst));
            qd_fill_span_1bpp(g_dst, x0, n, true);

            for (int x = 0; x < BM_W; x++) {
                const int want = (x >= x0 && x < x0 + n) ? 1 : 0;
                const int got = ref_get(g_dst, BM_RB, x, 0);
                if (got != want) {
                    CHECK(0, "fill(%d, %d, true): pixel %d = %d, want %d", x0, n, x, got, want);
                    return;
                }
            }
        }
    }

    /* Same sweep, clearing. */
    for (int x0 = 0; x0 < BM_W; x0++) {
        for (int n = 1; x0 + n <= BM_W; n++) {
            memset(g_dst, 0xFF, sizeof(g_dst));
            qd_fill_span_1bpp(g_dst, x0, n, false);
            for (int x = 0; x < BM_W; x++) {
                const int want = (x >= x0 && x < x0 + n) ? 0 : 1;
                const int got = ref_get(g_dst, BM_RB, x, 0);
                if (got != want) {
                    CHECK(0, "clear(%d, %d): pixel %d = %d, want %d", x0, n, x, got, want);
                    return;
                }
            }
        }
    }
    printf("  ok (%d spans verified against the reference)\n", BM_W * (BM_W + 1));
}

static void test_pen_and_fill(void)
{
    printf("test: the pen actually controls fill_rect\n");
    /* A port that exposes a pen and then ignores it is unusable: the caller
     * sets white expecting to erase, gets black, and has no way to tell. */
    qd_bitmap_t bm;
    make_bitmap(&bm, g_dst);
    qd_grafport_t p;
    qd_port_init(&p, &bm);
    const qd_rect_t r = qd_rect_make(10, 5, 50, 20);

    memset(g_dst, 0xFF, sizeof(g_dst));        /* all black */
    qd_port_set_white(&p);
    qd_fill_rect(&p, r, QD_ROP_SRC_COPY);
    for (int y = 5; y < 20; y++)
        for (int x = 10; x < 50; x++)
            if (ref_get(g_dst, BM_RB, x, y) != 0) {
                CHECK(0, "white pen + srcCopy set pixel (%d,%d)", x, y);
                y = 20; break;
            }

    memset(g_dst, 0x00, sizeof(g_dst));        /* all white */
    qd_port_set_black(&p);
    qd_fill_rect(&p, r, QD_ROP_SRC_COPY);
    for (int y = 5; y < 20; y++)
        for (int x = 10; x < 50; x++)
            if (ref_get(g_dst, BM_RB, x, y) != 1) {
                CHECK(0, "black pen + srcCopy left pixel (%d,%d) clear", x, y);
                y = 20; break;
            }

    /* Erase modes ignore the pen, as they should: they paint the background. */
    memset(g_dst, 0xFF, sizeof(g_dst));
    qd_port_set_black(&p);
    qd_fill_rect(&p, r, QD_ROP_CLEAR);
    for (int y = 5; y < 20; y++)
        for (int x = 10; x < 50; x++)
            if (ref_get(g_dst, BM_RB, x, y) != 0) {
                CHECK(0, "clear did not erase pixel (%d,%d) with a black pen", x, y);
                y = 20; break;
            }

    printf("  ok\n");
}

static void test_fill_and_frame(void)
{
    printf("test: fill_span and frame_rect\n");
    memset(g_dst, 0x00, sizeof(g_dst));

    qd_fill_span_1bpp(g_dst, 3, 10, true);
    for (int x = 3; x < 13; x++)
        CHECK(ref_get(g_dst, BM_RB, x, 0) == 1, "fill set bit %d", x);
    for (int x = 0; x < 3; x++)
        CHECK(ref_get(g_dst, BM_RB, x, 0) == 0, "fill leaked left of x=3 at %d", x);
    for (int x = 13; x < BM_W; x++)
        CHECK(ref_get(g_dst, BM_RB, x, 0) == 0, "fill leaked right of x=13 at %d", x);

    /* Frame: only the border should be set. */
    qd_bitmap_t bm;
    make_bitmap(&bm, g_dst);
    qd_grafport_t port;
    qd_port_init(&port, &bm);
    memset(g_dst, 0x00, sizeof(g_dst));
    qd_frame_rect(&port, qd_rect_make(4, 4, 40, 28), 3);
    for (int y = 4; y < 28; y++) {
        for (int x = 4; x < 40; x++) {
            const int on_border = (x < 7 || x >= 37 || y < 7 || y >= 25);
            const int got = ref_get(g_dst, BM_RB, x, y);
            if (got != (on_border ? 1 : 0)) {
                CHECK(0, "frame pixel (%d,%d) = %d, want %d", x, y, got, on_border ? 1 : 0);
                y = 28; break;
            }
        }
    }
    printf("  ok\n");
}

static void test_expand(void)
{
    printf("test: 1bpp -> RGB565 expansion\n");

    /* A known byte: 0b10110010 */
    uint8_t row[2] = { 0xB2, 0x00 };
    uint16_t out[8];
    qd_expand_1bpp_to_rgb565(row, 0, 8, out);

    const uint8_t expect[8] = { 1, 0, 1, 1, 0, 0, 1, 0 };
    for (int i = 0; i < 8; i++) {
        const uint16_t want = expect[i] ? QD_WHITE : QD_BLACK;
        CHECK(out[i] == want, "expand bit %d = %04x, want %04x", i, out[i], want);
    }

    /* Unaligned start: pixels 3..18 of a 3-byte row. */
    uint8_t row2[4] = { 0xFF, 0x00, 0xFF, 0x00 };
    uint16_t out2[16];
    qd_expand_1bpp_to_rgb565(row2, 3, 16, out2);
    for (int i = 0; i < 16; i++) {
        const int px = 3 + i;
        const int want_on = (row2[px >> 3] >> (7 - (px & 7))) & 1;
        const uint16_t want = want_on ? QD_WHITE : QD_BLACK;
        CHECK(out2[i] == want, "unaligned expand bit %d = %04x, want %04x", i, out2[i], want);
    }

    /* Full rectangle from a real bitmap. */
    qd_bitmap_t bm;
    make_bitmap(&bm, g_src);
    fill_pseudorandom(g_src, 0x55AAu);
    static uint16_t rect_out[BM_W * BM_H];
    const qd_rect_t r = qd_rect_make(5, 3, 61, 27);
    const int32_t bytes = qd_expand_dirty_rect(&bm, r, rect_out);
    CHECK(bytes == (r.right - r.left) * (r.bottom - r.top) * 2,
          "expand_dirty_rect returned %d bytes", (int)bytes);
    for (int y = r.top; y < r.bottom; y++) {
        for (int x = r.left; x < r.right; x++) {
            const int want_on = ref_get(g_src, BM_RB, x, y);
            const uint16_t want = want_on ? QD_WHITE : QD_BLACK;
            const uint16_t got = rect_out[(y - r.top) * (r.right - r.left) + (x - r.left)];
            CHECK(got == want, "rect expand (%d,%d) = %04x, want %04x", x, y, got, want);
        }
    }
    printf("  ok\n");
}

static void test_screen_size_constants(void)
{
    printf("test: screen geometry\n");
    CHECK(QD_SCREEN_W == 480 && QD_SCREEN_H == 320, "screen is 480x320");
    CHECK(QD_SCREEN_ROWBYTES == 60, "480 px -> 60 bytes per row, got %d",
          (int)QD_SCREEN_ROWBYTES);
    CHECK((int)(512 / 8) == 64, "the original 512 px row was 64 bytes = 16B aligned");
    printf("  note: 480 px -> 60 B rows, which are 16 B misaligned. Trade-off\n");
    printf("        accepted to avoid a resampling stage. See README.\n");
    printf("  ok\n");
}

int main(void)
{
    printf("qd core tests\n\n");
    test_screen_size_constants();
    test_bitblt_modes();
    test_bitblt_region_clip();
    test_region_ops();
    test_pen_and_fill();
    test_fill_span_exhaustive();
    test_fill_and_frame();
    test_expand();

    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
