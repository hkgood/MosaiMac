/*
 * qd.h - A clean-room QuickDraw 1-bit core for the ESP32-S31.
 *
 * Scope: the 1984 Macintosh QuickDraw drawing primitives that matter for a
 * bitmap UI. Bitmaps are 1 bit per pixel, MSB-first, matching the original
 * 68000 bit order so that Chicago bitmap font data stays compatible.
 *
 * This is an independent implementation written from the documented behaviour
 * of the Toolbox (Inside Macintosh) and from public descriptions of the
 * algorithms. No Apple code is copied.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QD_H
#define QD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * Screen geometry
 *
 * We render natively at 480x320 rather than the original 512x342.
 * 480/320 = 1.500 vs 512/342 = 1.497 -- visually the same aspect ratio --
 * and it removes an entire resampling stage from the display path.
 * 60 bytes per row.
 * ------------------------------------------------------------------ */

#define QD_SCREEN_W 480
#define QD_SCREEN_H 320
/*
 * Payload bytes per row: one bit per pixel, rounded up to a byte. 480 px is
 * exactly 60 bytes, which happens to leave every row 16-byte misaligned.
 * The trade-off is accepted to avoid a resampling stage.
 */
#define QD_SCREEN_ROWBYTES ((QD_SCREEN_W + 7) / 8)

/*
 * Readable slack required at the end of every row.
 *
 * qd_rop_span_1bpp() slides a misaligned source through a 64-bit window and
 * reads up to 8 bytes past the bits it consumes; the partial-byte helpers read
 * one byte past the end. Both stay inside the row's own stride, so this costs
 * 8 B * height and nothing else. QD_BSP_ROW_SLACK in the platform layer is
 * the same number.
 */
/*
 * Which bit is which colour. 1 = the 1984 look (bit 1 black on white); 0 = the
 * dark theme, which on an AMOLED lights far fewer pixels.
 *
 * Public, because anything that reads back pixels -- the tests, the render
 * tools, the simulator's own verification -- has to interpret them the same way
 * the panel does.
 * They assert pixel values, and a test that hard-codes one mapping breaks the
 * moment the other is chosen -- which is exactly what happened: flipping the
 * default produced 1368 failures in a file that does not even test colour
 * choice.
 */
#ifndef QD_INVERT
#define QD_INVERT 1
#endif

#define QD_ROW_SLACK 8

/*
 * The stride a bitmap for this screen must actually have, i.e. what goes in
 * qd_bitmap_t::rowBytes.
 *
 * This is the distinction that bites: rowBytes is the *stride*, used to find
 * the next row, so it has to include the slack. QD_SCREEN_ROWBYTES is the
 * payload width and is what bit arithmetic is based on. Setting rowBytes to
 * the payload width while allocating stride bytes silently shears the image,
 * because the writer and any reader then disagree about where row N+1 starts.
 */
#define QD_SCREEN_STRIDE (QD_SCREEN_ROWBYTES + QD_ROW_SLACK) /* 60 */

/* ------------------------------------------------------------------ *
 * Geometry
 * ------------------------------------------------------------------ */

typedef struct {
    int16_t top, left, bottom, right; /* half-open: [top,bottom) [left,right) */
} qd_rect_t;

typedef struct {
    int16_t h, v; /* fixed point, 16.16, QuickDraw style */
} qd_point_t;

static inline qd_rect_t qd_rect_make(int16_t l, int16_t t, int16_t r, int16_t b)
{
    qd_rect_t r_ = { t, l, b, r };
    return r_;
}

static inline int16_t qd_rect_width(qd_rect_t r) { return r.right - r.left; }
static inline int16_t qd_rect_height(qd_rect_t r) { return r.bottom - r.top; }

qd_rect_t qd_rect_intersect(qd_rect_t a, qd_rect_t b);
bool qd_rect_is_empty(qd_rect_t r);

/* ------------------------------------------------------------------ *
 * Bitmaps (1 bit per pixel, MSB = leftmost)
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *base;     /* pointer to row 0, column 0 */
    int16_t width;     /* pixels */
    int16_t height;    /* pixels */
    int16_t rowBytes;  /* bytes per row, (width+7)/8, caller aligned */
    int32_t seed;      /* QuickDraw's baseAddr seed, unused here */
} qd_bitmap_t;

static inline int16_t qd_bitmap_rowbytes(int16_t width)
{
    return (int16_t)(((uint32_t)width + 7u) >> 3);
}

static inline uint8_t *qd_bitmap_row(qd_bitmap_t *bm, int16_t y)
{
    return bm->base + (int32_t)y * bm->rowBytes;
}

static inline const uint8_t *qd_bitmap_row_c(const qd_bitmap_t *bm, int16_t y)
{
    return bm->base + (int32_t)y * bm->rowBytes;
}

/* Single pixel access, MSB-first. */
static inline bool qd_bitmap_get(const qd_bitmap_t *bm, int16_t x, int16_t y)
{
    return (bm->base[(int32_t)y * bm->rowBytes + (x >> 3)] >> (7 - (x & 7))) & 1u;
}

static inline void qd_bitmap_set(qd_bitmap_t *bm, int16_t x, int16_t y, bool v)
{
    uint8_t *p = &bm->base[(int32_t)y * bm->rowBytes + (x >> 3)];
    uint8_t m = (uint8_t)(1u << (7 - (x & 7)));
    *p = (uint8_t)(v ? (*p | m) : (*p & (uint8_t)~m));
}

/* ------------------------------------------------------------------ *
 * Regions
 *
 * A region is a per-scanline run-length span list. This is deliberately
 * not Atkinson's packed RLE: the 1984 format existed to fit a Region in
 * a few KB of a 128 KB machine, and we have 16 MB of PSRAM. The span
 * form is cache-friendly for scanline rasterisation and far easier to
 * get provably correct, which matters more here than bytes on the wire.
 * ------------------------------------------------------------------ */

#define QD_SPAN_EMPTY ((int16_t)0x7FFF)

typedef struct {
    int16_t x0, x1; /* [x0, x1) */
} qd_span_t;

typedef struct qd_region qd_region_t;

/*
 * Region storage.
 *
 * `spans` is one flat array holding every scanline's spans back to back, and
 * `offsets` is a prefix-sum table: line L occupies
 * spans[offsets[L] .. offsets[L+1]), with (bottom - top) + 1 entries so the
 * last line needs no special case.
 *
 * Storing offsets rather than per-line span counts is what keeps
 * qd_region_spans() O(1) per scanline. Summing the counts up to line L on
 * every call is O(y), which turns a full-screen region-clipped blit quadratic
 * and showed up as a 5x slowdown against the untrapped path.
 */
struct qd_region {
    int16_t top, bottom;
    int32_t *offsets; /* (bottom-top) + 1 entries, the +1 is a sentinel */
    qd_span_t *spans;
    int32_t span_cap;
    qd_rect_t bounds;
    bool owns_storage;
};

/* A rectangular region: the overwhelmingly common case, built in O(1)
 * with no heap traffic. Set ops promote to a general region on demand. */
typedef struct {
    qd_region_t reg;
    qd_rect_t rect;
} qd_region_rect_t;

/*
 * Destination contract for the set operations: `dst` must be a valid region
 * before the call. Zeroing is enough:  qd_region_t r = { 0 };
 * or call qd_region_init_empty(). Passing uninitialised stack memory is
 * undefined -- dispose() inspects owns_storage to decide whether to free.
 * Aliasing `dst` with either operand is handled but costs an extra copy.
 */
void qd_region_init_rect(qd_region_rect_t *rr, qd_rect_t r);

/* True when the region is a plain rectangle. Rect regions are stored in
 * place with no heap traffic, so the ROP engine can skip the per-scanline
 * walk entirely. Callers may rely on this for fast paths. */
static inline bool qd_region_is_rect(const qd_region_t *r)
{
    return r->offsets == NULL;
}

void qd_region_init_empty(qd_region_t *r);
void qd_region_dispose(qd_region_t *r);
void qd_region_copy(qd_region_t *dst, const qd_region_t *src);

/* Return the spans covering scanline y. `out` must hold >= max_spans. */
int32_t qd_region_spans(const qd_region_t *r, int16_t y,
                        qd_span_t *out, int32_t max_spans);

/* Set operations. `dst` is overwritten and may alias neither operand. */
void qd_region_union(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b);
void qd_region_intersect(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b);
void qd_region_xor(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b);
void qd_region_offset(qd_region_t *r, int16_t dh, int16_t dv);
void qd_region_invert(qd_region_t *r, qd_rect_t bounds);

/*
 * Total pixel area of the region, summed over spans.
 *
 * This is the number that matters when deciding what to push to the panel.
 * The bounding box of a window-move region is always the whole window, so
 * bounding-box area says nothing useful; span area is what actually gets
 * transmitted.
 */
int32_t qd_region_area(const qd_region_t *r);

/* ------------------------------------------------------------------ *
 * Transfer modes
 * ------------------------------------------------------------------ */

typedef enum {
    QD_ROP_SRC_COPY = 0, /* dst = src                                    */
    QD_ROP_SRC_OR,       /* dst |= src                                   */
    QD_ROP_SRC_XOR,      /* dst ^= src                                   */
    QD_ROP_SRC_BIC,      /* dst &= ~src                                  */
    QD_ROP_SRC_NOT,      /* dst = ~src                                   */
    QD_ROP_CLEAR,        /* dst = 0                                      */
} qd_rop_mode_t;

/* ------------------------------------------------------------------ *
 * Graphics port
 * ------------------------------------------------------------------ */

typedef qd_span_t qd_pattern_t; /* 8x8 pattern, 8 spans of x0/x1 or 8-bit rows */
typedef uint8_t qd_patrow_t[8]; /* 8 bytes, one per row, 8 bits per byte */

/*
 * Graphics state, pushed by qd_port_save() and popped by qd_port_restore().
 * The classic QuickDraw idiom for window drawing: clip, move the clip, draw,
 * restore, without having to remember what the clip was.
 */
typedef struct {
    qd_rect_t bounds;
    qd_rect_t clip;
    const qd_region_t *clip_region;
    qd_patrow_t pat;
    bool pat_is_white;
} qd_gstate_t;

#define QD_GSTATE_STACK_DEPTH 8

typedef struct {
    qd_bitmap_t bitmap;  /* destination pixels */
    qd_rect_t bounds;    /* valid extent, in bitmap coordinates */
    qd_rect_t clip;      /* clip rectangle, in bitmap coordinates */
    const qd_region_t *clip_region; /* non-NULL: general clip, in bitmap coords */
    int16_t origin_x;    /* port coordinate (x) lands on bitmap (x - origin_x) */
    int16_t origin_y;
    qd_patrow_t pat;     /* current pattern */
    bool pat_is_white;   /* true => all-ones fill, the common case */
    qd_gstate_t stack[QD_GSTATE_STACK_DEPTH];
    int stack_depth;
} qd_grafport_t;

void qd_port_init(qd_grafport_t *p, qd_bitmap_t *bm);
void qd_port_set_bounds(qd_grafport_t *p, qd_rect_t r);

/* Sets a rectangular clip and clears any general-region clip. */
void qd_port_set_clip(qd_grafport_t *p, qd_rect_t r);

/*
 * Sets a non-rectangular clip. `r` is in bitmap coordinates and must outlive
 * the clip -- QuickDraw regions are value types and this port does not copy,
 * so hand it the region you already own rather than a temporary.
 * Pass NULL to go back to the rectangle in `clip`.
 */
void qd_port_set_clip_region(qd_grafport_t *p, const qd_region_t *r);

/* Translate port coordinates into bitmap coordinates. */
void qd_port_set_origin(qd_grafport_t *p, int16_t x, int16_t y);
qd_rect_t qd_port_map(const qd_grafport_t *p, qd_rect_t r);

/* SaveGState / RestoreGState. qd_port_save returns false if the fixed-depth
 * stack is full, in which case the port is unchanged. */
bool qd_port_save(qd_grafport_t *p);
bool qd_port_restore(qd_grafport_t *p);

void qd_port_save_pattern(qd_grafport_t *p, const qd_patrow_t pat);
void qd_port_set_black(qd_grafport_t *p);
void qd_port_set_white(qd_grafport_t *p);

/* Effective clip = port clip AND port bounds AND bitmap extent. Does not
 * account for clip_region; that is applied per scanline. */
qd_rect_t qd_port_effective_clip(const qd_grafport_t *p);

/* ------------------------------------------------------------------ *
 * The hot path
 * ------------------------------------------------------------------ */

/*
 * CopyBits. Copies src_rect from src to dst_rect in dst, honouring the
 * clip region, using the given transfer mode. 1-bit only. Edge bits outside
 * dst_rect are never written, in any mode.
 *
 * This is the single hottest routine in the whole engine: every window
 * move, every menu redraw, every icon blit goes through it.
 */
void qd_copy_bits(qd_bitmap_t *dst, qd_rect_t dst_rect,
                  const qd_bitmap_t *src, qd_rect_t src_rect,
                  const qd_region_t *clip,
                  qd_rop_mode_t mode);

/* Same, restricted to a single destination scanline. Used by the bench. */
void qd_copy_bits_row(qd_bitmap_t *dst, qd_rect_t dst_rect,
                      const qd_bitmap_t *src, qd_rect_t src_rect,
                      qd_rop_mode_t mode);

/* Rectangle fill using the port's current pattern. */
void qd_fill_rect(qd_grafport_t *p, qd_rect_t r, qd_rop_mode_t mode);
void qd_frame_rect(qd_grafport_t *p, qd_rect_t r, int16_t line_width);

/* Raw span-level fill, no region machinery. */
void qd_fill_span_1bpp(uint8_t *row, int dst_x0, int n_pixels, bool value);

/* ------------------------------------------------------------------ *
 * Display output
 * ------------------------------------------------------------------ */

/*
 * Expand a 1bpp scanline run to RGB565, MSB-first in the source.
 * `out` must have room for n_pixels * 2 bytes.
 *
 * This is the 1 -> 16 bit unpacking step that feeds the CO5300 over QSPI.
 */
void qd_expand_1bpp_to_rgb565(const uint8_t *src_row, int src_x0,
                              int n_pixels, uint16_t *out);

/*
 * Expand a dirty rectangle of a 1bpp bitmap to RGB565, row by row.
 * `out` receives width*height*2 bytes, tightly packed.
 * Returns the number of bytes written.
 *
 * CO5300 requires update window bounds to be even-pixel aligned; callers
 * should round their dirty rects outward accordingly.
 */
int32_t qd_expand_dirty_rect(const qd_bitmap_t *bm, qd_rect_t r, uint16_t *out);

/* ------------------------------------------------------------------ *
 * Colour
 * ------------------------------------------------------------------ */

#define QD_RGB565(r, g, b)                                                    \
    ((uint16_t)((((r)&0xF8u) << 8) | (((g)&0xFCu) << 3) | (((b)&0xF8u) >> 3)))

#define QD_WHITE QD_RGB565(0xFF, 0xFF, 0xFF)
#define QD_BLACK QD_RGB565(0x00, 0x00, 0x00)

#ifdef __cplusplus
}
#endif

#endif /* QD_H */
