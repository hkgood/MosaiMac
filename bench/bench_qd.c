/*
 * bench_qd.c - throughput measurements for the pieces that decide whether
 * this port is viable.
 *
 * Run it on a PC first. It tells you the order of magnitude of each stage and
 * therefore which stage is worth optimising, before anyone touches hardware.
 * The absolute numbers will not transfer to the ESP32-S31; the ratios will,
 * and that is the point.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd/qd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_REALTIME
#endif

#define W QD_SCREEN_W
#define H QD_SCREEN_H
#define RB ((W + 7) / 8)
#define PAD 16

static uint8_t g_fb[H * (RB + PAD) + 64];
static uint8_t g_src[H * (RB + PAD) + 64];
static uint16_t g_rgb[W * H];
static uint16_t g_scratch[W * H];

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

static void fill_pseudorandom(uint8_t *buf, size_t n, unsigned seed)
{
    unsigned s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(s >> 16);
    }
}

static qd_bitmap_t mk(uint8_t *buf)
{
    qd_bitmap_t bm;
    bm.base = buf;
    bm.width = W;
    bm.height = H;
    bm.rowBytes = RB;
    bm.seed = 0;
    return bm;
}

/* Run `iters` of `body`, return best-of-`reps` milliseconds per iteration. */
#define BENCH(label, iters, reps, body)                                        \
    do {                                                                       \
        double best = 1e30;                                                    \
        for (int r = 0; r < (reps); r++) {                                     \
            const double t0 = now_ms();                                        \
            for (int i = 0; i < (iters); i++) { body; }                        \
            const double dt = (now_ms() - t0) / (iters);                       \
            if (dt < best) best = dt;                                          \
        }                                                                      \
        printf("  %-46s %9.3f ms\n", label, best);                             \
        last = best;                                                           \
    } while (0)

int main(void)
{
    qd_bitmap_t dst = mk(g_fb);
    qd_bitmap_t src = mk(g_src);
    const qd_rect_t full = qd_rect_make(0, 0, W, H);
    double last = 0.0;

    fill_pseudorandom(g_fb, sizeof(g_fb), 0x1234u);
    fill_pseudorandom(g_src, sizeof(g_src), 0xABCDu);

    const double full_mpix = (double)(W * H) / 1e6;   /* 1bpp packs 8 px/byte */
    const double fb_bytes = (double)W * H / 8.0;

    printf("qd benchmark -- %dx%d 1bpp, rowBytes=%d, framebuffer %.1f KB\n\n",
           W, H, (int)RB, fb_bytes / 1024.0);
    printf("(desktop x86-64; the S31 is slower, the ratios are the signal)\n\n");

    printf("CopyBits, full screen, no clip\n");
    BENCH("srcXor  aligned   (60B rows)", 200, 7, {
        qd_copy_bits(&dst, full, &src, full, NULL, QD_ROP_SRC_XOR);
    });
    printf("      -> %.1f Mpixel/s\n", full_mpix / (last / 1e3));

    BENCH("srcOr   aligned", 200, 7, {
        qd_copy_bits(&dst, full, &src, full, NULL, QD_ROP_SRC_OR);
    });
    printf("      -> %.1f Mpixel/s\n", full_mpix / (last / 1e3));

    BENCH("srcCopy aligned", 200, 7, {
        qd_copy_bits(&dst, full, &src, full, NULL, QD_ROP_SRC_COPY);
    });
    printf("      -> %.1f Mpixel/s\n", full_mpix / (last / 1e3));

    printf("\nCopyBits, full screen, misaligned by 3 px (source phase shift)\n");
    const qd_rect_t shifted = qd_rect_make(3, 0, W, H);
    BENCH("srcXor  misaligned", 200, 7, {
        qd_copy_bits(&dst, full, &src, shifted, NULL, QD_ROP_SRC_XOR);
    });
    printf("      -> %.1f Mpixel/s\n", full_mpix / (last / 1e3));

    printf("\nCopyBits with an L-shaped region clip (2 spans/scanline)\n");
    {
        qd_region_rect_t ra, rb, rr;
        qd_region_init_rect(&ra, qd_rect_make(0, 0, W - 100, H));
        qd_region_init_rect(&rb, qd_rect_make(W - 200, 0, W, H / 2));
        qd_region_xor(&rr.reg, &ra.reg, &rb.reg);
        BENCH("srcXor  L-region clipped", 200, 7, {
            qd_copy_bits(&dst, full, &src, full, &rr.reg, QD_ROP_SRC_XOR);
        });
        printf("      -> %6.1f Mpixel/s   %7.0f MB/s of 1bpp\n",
               full_mpix / (last / 1e3), (fb_bytes / 1e6) / (last / 1e3));
        qd_region_dispose(&rr.reg);
    }

    printf("\nTypical window move: 200x150 srcXor, aligned\n");
    {
        const qd_rect_t w1 = qd_rect_make(40, 30, 240, 180);
        const qd_rect_t w2 = qd_rect_make(60, 50, 260, 200);
        BENCH("srcXor  200x150 window move", 2000, 7, {
            qd_copy_bits(&dst, w2, &src, w1, NULL, QD_ROP_SRC_XOR);
        });
        printf("      -> %.0f kops/s\n", 1000.0 / last);
    }

    printf("\nDisplay output path\n");
    BENCH("1bpp -> RGB565, full frame", 200, 7, {
        qd_expand_dirty_rect(&dst, full, g_rgb);
    });
    printf("      -> %6.1f Mpixel/s, %d KB out per frame\n",
           full_mpix / (last / 1e3), (W * H * 2) / 1024);

    BENCH("1bpp -> RGB565, typical 120x90 dirty rect", 2000, 7, {
        qd_expand_dirty_rect(&dst, qd_rect_make(100, 80, 220, 170), g_scratch);
    });

    printf("\nEstimated end-to-end frame cost (QSPI push modelled, not measured)\n");
    {
        /* The CO5300 is a Quad-SPI AMOLED DDIC. At a 40 MHz SPI clock with four
         * data lines the wire rate is 20 MB/s before overhead; assume 70%
         * efficiency. This is an assumption, not a measurement -- it is the
         * number to go and measure on real hardware. */
        const double kb = (W * H * 2.0) / 1024.0;
        const double dirty_px = 120.0 * 90.0;
        for (int q = 0; q < 2; q++) {
            const double spi_mhz = q ? 80.0 : 40.0;
            const double wire = spi_mhz * 4.0 * 0.70 / 8.0; /* MB/s */
            const double push_full = (W * H * 2.0) / (wire * 1e6) * 1e3;
            const double push_dirty = (dirty_px * 2.0) / (wire * 1e6) * 1e3;
            printf("  @ %.0f MHz QSPI: wire %5.1f MB/s | full frame %.1f ms (%.0f fps)"
                   " | 120x90 dirty %.2f ms (%.0f fps)\n",
                   spi_mhz, wire, push_full, 1000.0 / push_full,
                   push_dirty, 1000.0 / push_dirty);
        }
        printf("  full frame is %.0f KB of RGB565 -- this is the real budget, not the ROP\n", kb);
    }

    printf("\nMemory footprint at %dx%d (PSRAM is 16 MB, so this is not tight)\n", W, H);
    printf("  1bpp framebuffer                 %8.1f KB\n",
           (W * H / 8.0) / 1024.0);
    printf("  + %d B/row slack for the ROP window reads%4.1f KB\n", QD_ROW_SLACK,
           (W * H / 8.0 + (double)QD_ROW_SLACK * H) / 1024.0 - (W * H / 8.0) / 1024.0);
    printf("  RGB565 band buffer (32 lines)    %8.1f KB\n",
           (W * 32 * 2.0) / 1024.0);
    printf("  a full RGB565 frame, if you did  %8.1f KB\n", (W * H * 2.0) / 1024.0);
    printf("  full-screen region, typical rect %8.1f KB\n", (H * 4.0) / 1024.0);
    printf("  full-screen region, worst case   %8.1f KB   (a 60-span-per-line\n"
           "                                          checkerboard, which no\n"
           "                                          real drawing produces)\n",
           (H * (W / 16) * 4.0) / 1024.0);
    printf("  for comparison, the 68000 had     %8.1f KB of total RAM\n", 128.0);

    return 0;
}
