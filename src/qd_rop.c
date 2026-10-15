/*
 * qd_rop.c - the transfer engine. This is the hot path.
 *
 * Layout contract: every bitmap row must have at least 2 bytes of readable
 * padding past its last byte. All the bit-extraction helpers below peek two
 * bytes ahead, and on a 60-byte row that peek lands in the padding rather
 * than in the next row (or off the end of the buffer). Callers allocating
 * framebuffers must honour this; see qd_bitmap_alloc_padded().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd_internal.h"

/* ------------------------------------------------------------------ *
 * Byte-granular core
 * ------------------------------------------------------------------ */

/*
 * Apply `mode` to the pixels occupying [dp, dp+chunk) of destination byte
 * `dbyte`, sourcing them from pixel index `sbit` of `srow`.
 * dp and the source phase may differ; the mask is defined purely in
 * destination terms, and `sv` is already presented with its first pixel in
 * the top bit, so the same mask selects the correct source bits.
 */
static inline void rop_byte_chunk(uint8_t *drow, int dbyte, int dp,
                                  const uint8_t *srow, int sbit, int chunk,
                                  qd_rop_mode_t mode)
{
    const int sb = sbit >> 3;
    const int sh = sbit & 7;
    const uint16_t w = (uint16_t)(((uint16_t)srow[sb] << 8) | (uint16_t)srow[sb + 1]);

    /*
     * The chunk's source bits arrive packed against the top of a byte. The
     * destination wants them sitting at bit (8 - dp - chunk), so they have to
     * be shifted down by dp before the mask is applied. Masking `sv` directly
     * is only correct when dp == 0, which is why the whole-byte path hid this
     * and the unaligned tail did not.
     */
    const uint8_t m = (uint8_t)(((1u << chunk) - 1u) << (8 - dp - chunk));
    const uint8_t sv = (uint8_t)((w >> (8 - sh)) >> dp);

    const uint8_t dv = drow[dbyte];
    uint8_t r;
    switch (mode) {
    case QD_ROP_SRC_COPY: r = sv;                         break;
    case QD_ROP_SRC_OR:   r = (uint8_t)(dv | sv);         break;
    case QD_ROP_SRC_XOR:  r = (uint8_t)(dv ^ sv);         break;
    case QD_ROP_SRC_BIC:  r = (uint8_t)(dv & (uint8_t)~sv); break;
    case QD_ROP_SRC_NOT:  r = (uint8_t)(~sv);             break;
    default:              r = 0u;                         break; /* CLEAR */
    }
    drow[dbyte] = (uint8_t)((uint8_t)(dv & (uint8_t)~m) | (uint8_t)(r & m));
}

/* Whole-byte ROP. Source and destination share the same bit phase. */
static inline void rop_bytes(uint8_t *d, const uint8_t *s, int nbytes,
                             qd_rop_mode_t mode)
{
    int i = 0;

    if (nbytes >= 8) {
        /* 64-bit chunks. memcpy keeps us endian-clean: we never inspect the
         * integer, we only push the same bit pattern back out. */
        for (; i + 8 <= nbytes; i += 8) {
            uint64_t dv, sv, r;
            memcpy(&dv, d + i, 8);
            memcpy(&sv, s + i, 8);
            switch (mode) {
            case QD_ROP_SRC_COPY: r = sv;          break;
            case QD_ROP_SRC_OR:   r = dv | sv;     break;
            case QD_ROP_SRC_XOR:  r = dv ^ sv;     break;
            case QD_ROP_SRC_BIC:  r = dv & ~sv;    break;
            default:              r = ~sv;         break;
            }
            memcpy(d + i, &r, 8);
        }
    }
    for (; i < nbytes; i++) {
        const uint8_t dv = d[i], sv = s[i];
        uint8_t out;
        switch (mode) {
        case QD_ROP_SRC_COPY: out = sv;                    break;
        case QD_ROP_SRC_OR:   out = (uint8_t)(dv | sv);    break;
        case QD_ROP_SRC_XOR:  out = (uint8_t)(dv ^ sv);    break;
        case QD_ROP_SRC_BIC:  out = (uint8_t)(dv & ~sv);   break;
        case QD_ROP_SRC_NOT:  out = (uint8_t)(~sv);        break;
        default:              out = 0;                     break;
        }
        d[i] = out;
    }
}

/* ------------------------------------------------------------------ *
 * Span-level ROP
 * ------------------------------------------------------------------ */

/*
 * ROP `n` pixels: destination starting at pixel `dx` of `drow`, source
 * starting at pixel `sx` of `srow`. Bits outside the span are never
 * written, in any mode.
 */
/* Single byte, full byte, no partial mask. */
static inline uint8_t rop1(uint8_t dv, uint8_t sv, qd_rop_mode_t mode)
{
    switch (mode) {
    case QD_ROP_SRC_COPY: return sv;
    case QD_ROP_SRC_OR:   return (uint8_t)(dv | sv);
    case QD_ROP_SRC_XOR:  return (uint8_t)(dv ^ sv);
    case QD_ROP_SRC_BIC:  return (uint8_t)(dv & (uint8_t)~sv);
    case QD_ROP_SRC_NOT: return (uint8_t)(~sv);
    default:              return 0; /* CLEAR */
    }
}

/* Big-endian 64-bit load: the byte at p[0] lands in the top bits. Zero-filled
 * past `avail` so a partial window near the end of a row is harmless. */
static inline uint64_t load_be64(const uint8_t *p, int avail)
{
    uint64_t v = 0;
    const int n = avail < 8 ? avail : 8;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (56 - 8 * i);
    return v;
}

/*
 * Whole-byte ROP with the source offset by `sh` bits.
 *
 * This is the whole reason unaligned BitBlt is fast. The alternative -- one
 * rop_byte_chunk() per destination byte -- reloads a 16-bit source window,
 * shifts it and switches on the mode for every single byte, and measured
 * roughly 14x slower than the aligned path on a desktop. Dragging a window is
 * the most common BitBlt a Macintosh-style UI performs, and a dragged window
 * is misaligned by definition, so the 8x-bits-at-a-time path is not a nicety.
 */
static void rop_bytes_shifted(uint8_t *d, const uint8_t *s, int nbytes, int sh,
                              int avail, qd_rop_mode_t mode)
{
    if (nbytes <= 0) return;
    if (sh == 0) { rop_bytes(d, s, nbytes, mode); return; }

    uint64_t w = load_be64(s, avail);

    /*
     * The window holds the source stream big-endian with the next bit at the
     * top, so every output byte is a shifted top-of-window extract: `sh`
     * applies each time, not just to the first. Zeroing it after the first
     * iteration slides the stream one bit early from the second byte on.
     */
    /* Split so the refill guard is not in the hot loop. */
    const int guarded = avail - 8;
    int i = 0;
    for (; i < nbytes && i < guarded; i++) {
        d[i] = rop1(d[i], (uint8_t)((w << sh) >> 56), mode);
        w = (w << 8) | (uint64_t)s[i + 8];
    }
    /* Past that the top byte is still correct; only the refill is not. */
    for (; i < nbytes; i++) {
        d[i] = rop1(d[i], (uint8_t)((w << sh) >> 56), mode);
        w <<= 8;
    }}

void qd_rop_span_1bpp(uint8_t *drow, int dx, const uint8_t *srow, int sx, int n,
                      int src_row_bytes, qd_rop_mode_t mode)
{
    if (n <= 0) return;

    /* CLEAR has no source; it is a fill. */
    if (mode == QD_ROP_CLEAR) {
        qd_fill_span_1bpp(drow, dx, n, false);
        return;
    }

    const int phase_d = dx & 7;
    const int phase_s = sx & 7;
    const bool aligned = (phase_d == phase_s);
    int done = 0;

    if (aligned) {
        /* --- fast path: same phase, so we can move whole bytes --- */
        int head = (8 - phase_d) & 7;
        if (head > n) head = n;

        for (int i = 0; i < head; i++, done++) {
            rop_byte_chunk(drow, (dx + i) >> 3, (dx + i) & 7,
                           srow, sx + i, 1, mode);
        }
    } else {
        /* Bring the destination to a byte boundary, then let the whole-byte
         * shifted path below take over. */
        int lead = (8 - phase_d) & 7;
        if (lead > n) lead = n;
        rop_byte_chunk(drow, dx >> 3, phase_d, srow, sx, lead, mode);
        done = lead;
    }

    /* --- whole destination bytes --- */
    {
        const int nwhole = (n - done) >> 3;
        if (nwhole > 0) {
            const int dbyte = (dx + done) >> 3;
            const int sbyte = (sx + done) >> 3;
            const int avail = src_row_bytes - sbyte;
            if (aligned) {
                rop_bytes(drow + dbyte, srow + sbyte, nwhole, mode);
            } else {
                rop_bytes_shifted(drow + dbyte, srow + sbyte, nwhole,
                                  (sx + done) & 7, avail, mode);
            }
            done += nwhole << 3;
        }
    }

    /* --- tail --- */
    int ntail = n - done;
    if (ntail > 0) {
        int dpos = dx + done, spos = sx + done;
        while (ntail > 0) {
            const int room = 8 - (dpos & 7);
            const int chunk = room < ntail ? room : ntail;
            rop_byte_chunk(drow, dpos >> 3, dpos & 7, srow, spos, chunk, mode);
            dpos += chunk;
            spos += chunk;
            ntail -= chunk;
        }
    }
}

void qd_fill_span_1bpp(uint8_t *row, int dst_x0, int n_pixels, bool value)
{
    if (n_pixels <= 0) return;

    const uint8_t fill = value ? 0xFFu : 0x00u;
    int pos = dst_x0;
    int left = n_pixels;

    /*
     * The partial-byte mask has to account for the bit phase, exactly as the
     * ROP mask does. Writing (0xFF << (8 - n)) here is only correct when the
     * span happens to end on a byte boundary, which is why this went unnoticed
     * until qd_frame_rect() clipped a 3 px band to 3 px.
     */
    const int ph0 = pos & 7;
    int head = (8 - ph0) & 7;
    if (head > left) head = left;
    if (head > 0) {
        const uint8_t m = (uint8_t)(((1u << head) - 1u) << (8 - ph0 - head));
        uint8_t *p = &row[pos >> 3];
        *p = (uint8_t)((*p & (uint8_t)~m) | (uint8_t)(fill & m));
        pos += head;
        left -= head;
    }

    const int nwhole = left >> 3;
    if (nwhole > 0) {
        memset(&row[pos >> 3], (int)fill, (size_t)nwhole);
        pos += nwhole << 3;
        left -= nwhole << 3;
    }

    if (left > 0) {
        const int ph = pos & 7;
        const uint8_t m = (uint8_t)(((1u << left) - 1u) << (8 - ph - left));
        uint8_t *p = &row[pos >> 3];
        *p = (uint8_t)((*p & (uint8_t)~m) | (uint8_t)(fill & m));
    }
}

/* ------------------------------------------------------------------ *
 * CopyBits
 * ------------------------------------------------------------------ */

void qd_copy_bits_row(qd_bitmap_t *dst, qd_rect_t dst_rect,
                      const qd_bitmap_t *src, qd_rect_t src_rect,
                      qd_rop_mode_t mode)
{
    const int n = qd_rect_width(dst_rect);
    if (n <= 0) return;

    uint8_t *drow = qd_bitmap_row(dst, dst_rect.top);
    const uint8_t *srow = qd_bitmap_row_c(src, src_rect.top);

    qd_rop_span_1bpp(drow, dst_rect.left, srow, src_rect.left, n,
                     src->rowBytes, mode);
}

#define QD_MAX_SPANS_PER_SCANLINE 256

void qd_copy_bits(qd_bitmap_t *dst, qd_rect_t dst_rect,
                  const qd_bitmap_t *src, qd_rect_t src_rect,
                  const qd_region_t *clip,
                  qd_rop_mode_t mode)
{
    if (qd_rect_is_empty(dst_rect)) return;

    /* Clip against the destination bitmap extent. */
    const qd_rect_t bm_rect = qd_rect_make(0, 0, dst->width, dst->height);
    const qd_rect_t d = qd_rect_intersect(dst_rect, bm_rect);
    if (qd_rect_is_empty(d)) return;

    const int srow0 = src_rect.top + (d.top - dst_rect.top);

    /* Rect clip: no per-scanline walk needed. */
    if (clip == NULL || qd_region_is_rect(clip)) {
        qd_rect_t c = d;
        if (clip != NULL) {
            const qd_rect_t cr = qd_rect_intersect(d, clip->bounds);
            if (qd_rect_is_empty(cr)) return;
            c = cr;
        }
        if (qd_rect_is_empty(c)) return;

        const int n = qd_rect_width(c);
        for (int y = c.top; y < c.bottom; y++) {
            uint8_t *drow = qd_bitmap_row(dst, (int16_t)y);
            const uint8_t *srow = qd_bitmap_row_c(src, (int16_t)(srow0 + y - d.top));
            const int sx = src_rect.left + (c.left - d.left);
            qd_rop_span_1bpp(drow, c.left, srow, sx, n, src->rowBytes, mode);
        }
        return;
    }

    /* General region: walk scanlines, intersect each with the dst rect. */
    qd_span_t spans[QD_MAX_SPANS_PER_SCANLINE];
    for (int y = d.top; y < d.bottom; y++) {
        const int32_t ns = qd_region_spans(clip, (int16_t)y, spans,
                                           QD_MAX_SPANS_PER_SCANLINE);
        if (ns <= 0) continue;

        uint8_t *drow = qd_bitmap_row(dst, (int16_t)y);
        const uint8_t *srow = qd_bitmap_row_c(src, (int16_t)(srow0 + y - d.top));

        for (int32_t i = 0; i < ns; i++) {
            const int x0 = spans[i].x0 > d.left ? spans[i].x0 : d.left;
            const int x1 = spans[i].x1 < d.right ? spans[i].x1 : d.right;
            if (x1 <= x0) continue;
            const int sx = src_rect.left + (x0 - d.left);
            qd_rop_span_1bpp(drow, x0, srow, sx, x1 - x0, src->rowBytes, mode);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Rect fill / frame
 * ------------------------------------------------------------------ */

void qd_fill_rect(qd_grafport_t *p, qd_rect_t r, qd_rop_mode_t mode)
{
    /* Port coordinates first, then the rectangular clip, then the region
     * clip per scanline. Doing it in that order means a caller only has to
     * get one of the three right for the result to be correct. */
    const qd_rect_t c = qd_rect_intersect(qd_port_map(p, r), qd_port_effective_clip(p));
    if (qd_rect_is_empty(c)) return;

    /*
     * In QuickDraw, srcCopy paints with the *foreground* pattern and srcBic
     * paints with the background, i.e. it erases. Honouring the pen is the
     * entire point of having one: qd_port_set_white() followed by a fill must
     * leave pixels alone, not paint them black. A port that exposes a pen and
     * then ignores it is worse than one with no pen at all, because the caller
     * has no way to notice.
     *
     * In this 1-bit engine the two colours are QD_WHITE (bit 0) and QD_BLACK
     * (bit 1), so the pen collapses to a single bool.
     */
    const bool erase = (mode == QD_ROP_CLEAR || mode == QD_ROP_SRC_BIC);
    const bool set = erase ? false : !p->pat_is_white;

    if (p->clip_region == NULL || qd_region_is_rect(p->clip_region)) {
        for (int y = c.top; y < c.bottom; y++) {
            qd_fill_span_1bpp(qd_bitmap_row(&p->bitmap, (int16_t)y), c.left,
                              c.right - c.left, set);
        }
        return;
    }

    qd_span_t spans[QD_MAX_SPANS_PER_SCANLINE];
    for (int y = c.top; y < c.bottom; y++) {
        const int32_t ns = qd_region_spans(p->clip_region, (int16_t)y, spans,
                                           QD_MAX_SPANS_PER_SCANLINE);
        for (int32_t i = 0; i < ns; i++) {
            const int x0 = spans[i].x0 > c.left ? spans[i].x0 : c.left;
            const int x1 = spans[i].x1 < c.right ? spans[i].x1 : c.right;
            if (x1 <= x0) continue;
            qd_fill_span_1bpp(qd_bitmap_row(&p->bitmap, (int16_t)y), x0, x1 - x0, set);
        }
    }
}

void qd_frame_rect(qd_grafport_t *p, qd_rect_t r, int16_t w)
{
    if (w <= 0) return;
    const qd_rect_t c = qd_rect_intersect(r, qd_port_effective_clip(p));
    if (qd_rect_is_empty(c)) return;

    if (w * 2 >= qd_rect_height(c) || w * 2 >= qd_rect_width(c)) {
        qd_fill_rect(p, c, QD_ROP_SRC_COPY);
        return;
    }
    qd_fill_rect(p, qd_rect_make(c.left, c.top, c.right, (int16_t)(c.top + w)), QD_ROP_SRC_COPY);
    qd_fill_rect(p, qd_rect_make(c.left, (int16_t)(c.bottom - w), c.right, c.bottom), QD_ROP_SRC_COPY);
    qd_fill_rect(p, qd_rect_make(c.left, (int16_t)(c.top + w), (int16_t)(c.left + w), (int16_t)(c.bottom - w)), QD_ROP_SRC_COPY);
    qd_fill_rect(p, qd_rect_make((int16_t)(c.right - w), (int16_t)(c.top + w), c.right, (int16_t)(c.bottom - w)), QD_ROP_SRC_COPY);
}

/* ------------------------------------------------------------------ *
 * Port
 * ------------------------------------------------------------------ */

void qd_port_init(qd_grafport_t *p, qd_bitmap_t *bm)
{
    memset(p, 0, sizeof(*p));
    p->bitmap = *bm;
    p->bounds = qd_rect_make(0, 0, bm->width, bm->height);
    p->clip = p->bounds;
    p->pat_is_white = false;
    memset(p->pat, 0xFF, sizeof(p->pat));
}

void qd_port_set_bounds(qd_grafport_t *p, qd_rect_t r) { p->bounds = r; }

void qd_port_set_clip(qd_grafport_t *p, qd_rect_t r)
{
    p->clip = r;
    p->clip_region = NULL;
}

void qd_port_set_clip_region(qd_grafport_t *p, const qd_region_t *r)
{
    p->clip_region = r;
    /* Keep the rectangular clip tight around the region so a caller that only
     * ever asks for effective_clip() still gets the right bounding box. */
    p->clip = (r != NULL) ? r->bounds : qd_port_effective_clip(p);
}

void qd_port_set_origin(qd_grafport_t *p, int16_t x, int16_t y)
{
    p->origin_x = x;
    p->origin_y = y;
}

qd_rect_t qd_port_map(const qd_grafport_t *p, qd_rect_t r)
{
    return qd_rect_make((int16_t)(r.left - p->origin_x),
                        (int16_t)(r.top - p->origin_y),
                        (int16_t)(r.right - p->origin_x),
                        (int16_t)(r.bottom - p->origin_y));
}

bool qd_port_save(qd_grafport_t *p)
{
    if (p->stack_depth >= QD_GSTATE_STACK_DEPTH) return false;
    qd_gstate_t *g = &p->stack[p->stack_depth++];
    g->bounds = p->bounds;
    g->clip = p->clip;
    g->clip_region = p->clip_region;
    g->pat_is_white = p->pat_is_white;
    memcpy(g->pat, p->pat, sizeof(p->pat));
    return true;
}

bool qd_port_restore(qd_grafport_t *p)
{
    if (p->stack_depth <= 0) return false;
    const qd_gstate_t *g = &p->stack[--p->stack_depth];
    p->bounds = g->bounds;
    p->clip = g->clip;
    p->clip_region = g->clip_region;
    p->pat_is_white = g->pat_is_white;
    memcpy(p->pat, g->pat, sizeof(p->pat));
    return true;
}

void qd_port_save_pattern(qd_grafport_t *p, const qd_patrow_t pat)
{
    memcpy(p->pat, pat, sizeof(p->pat));
    p->pat_is_white = (memcmp(pat, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0);
}

void qd_port_set_black(qd_grafport_t *p)
{
    memset(p->pat, 0x00, sizeof(p->pat));
    p->pat_is_white = false;
}

void qd_port_set_white(qd_grafport_t *p)
{
    memset(p->pat, 0xFF, sizeof(p->pat));
    p->pat_is_white = true;
}

qd_rect_t qd_port_effective_clip(const qd_grafport_t *p)
{
    qd_rect_t r = qd_rect_intersect(p->bounds, p->clip);
    return qd_rect_intersect(r, qd_rect_make(0, 0, p->bitmap.width, p->bitmap.height));
}

/* ------------------------------------------------------------------ *
 * Geometry helpers
 * ------------------------------------------------------------------ */

bool qd_rect_is_empty(qd_rect_t r) { return r.right <= r.left || r.bottom <= r.top; }

qd_rect_t qd_rect_intersect(qd_rect_t a, qd_rect_t b)
{
    qd_rect_t r;
    r.left = a.left > b.left ? a.left : b.left;
    r.top = a.top > b.top ? a.top : b.top;
    r.right = a.right < b.right ? a.right : b.right;
    r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}
