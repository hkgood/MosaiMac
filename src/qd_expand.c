/*
 * qd_expand.c - 1bpp -> RGB565, the last stage before the panel.
 *
 * The 1-bit engine writes black and white. The CO5300 wants RGB565. This is
 * a pure bit-unpack, and it is memory-bound rather than ALU-bound: it writes
 * two bytes per input bit, so the arithmetic is free and the only thing that
 * matters is not doing anything silly per pixel.
 *
 * Bit 1 renders as white, bit 0 as black, which gives the 1984 look. Flip
 * QD_INVERT for a dark theme -- on an AMOLED that is the difference between
 * the screen being the power hog and the screen being nearly free.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd_internal.h"

#ifndef QD_INVERT
#define QD_INVERT 0
#endif

/* Reverse the bit order of a byte. Our bitmaps are MSB-first (bit 7 is the
 * leftmost pixel) but the spread trick below consumes bit 0 first. */
static inline uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

/* Spread the 8 bits of b so that bit k lands at bit 8k of the result. */
static inline uint64_t spread8(uint8_t b)
{
    uint64_t x = b;
    x = (x | (x << 28)) & 0x0000000F0000000FULL;
    x = (x | (x << 14)) & 0x0003000300030003ULL;
    x = (x | (x << 7)) & 0x0101010101010101ULL;
    return x;
}

static inline uint16_t qd_pixel(bool on)
{
#if QD_INVERT
    return on ? QD_BLACK : QD_WHITE;
#else
    return on ? QD_WHITE : QD_BLACK;
#endif
}

/* Expand eight pixels given as a source byte, MSB-first. */
static inline void expand_byte(uint8_t b, uint16_t *out)
{
    const uint64_t x = spread8(reverse8(b));
    out[0] = qd_pixel(x & 1);
    out[1] = qd_pixel((x >> 8) & 1);
    out[2] = qd_pixel((x >> 16) & 1);
    out[3] = qd_pixel((x >> 24) & 1);
    out[4] = qd_pixel((x >> 32) & 1);
    out[5] = qd_pixel((x >> 40) & 1);
    out[6] = qd_pixel((x >> 48) & 1);
    out[7] = qd_pixel((x >> 56) & 1);
}

void qd_expand_1bpp_to_rgb565(const uint8_t *src_row, int src_x0,
                              int n_pixels, uint16_t *out)
{
    int i = 0;

    /* Head: up to the next source byte boundary. */
    const int sb0 = src_x0 >> 3;
    int head = ((sb0 + 1) << 3) - src_x0;
    if (head > n_pixels) head = n_pixels;
    for (; i < head; i++) {
        const bool on = (src_row[sb0] >> (7 - ((src_x0 + i) & 7))) & 1u;
        out[i] = qd_pixel(on);
    }

    /* Body: whole source bytes. */
    {
        const int nbytes = (n_pixels - i) >> 3;
        for (int k = 0; k < nbytes; k++) {
            expand_byte(src_row[sb0 + 1 + k], out + i + (k << 3));
        }
        i += nbytes << 3;
    }

    /* Tail. */
    for (; i < n_pixels; i++) {
        const int b = src_x0 + i;
        const bool on = (src_row[b >> 3] >> (7 - (b & 7))) & 1u;
        out[i] = qd_pixel(on);
    }
}

int32_t qd_expand_dirty_rect(const qd_bitmap_t *bm, qd_rect_t r, uint16_t *out)
{
    const qd_rect_t c = qd_rect_intersect(
        r, qd_rect_make(0, 0, bm->width, bm->height));
    if (qd_rect_is_empty(c)) return 0;

    const int w = c.right - c.left;
    const int h = c.bottom - c.top;

    for (int y = c.top; y < c.bottom; y++) {
        qd_expand_1bpp_to_rgb565(qd_bitmap_row_c(bm, (int16_t)y), c.left, w,
                                 out + (int32_t)(y - c.top) * w);
    }
    return (int32_t)w * h * 2;
}
