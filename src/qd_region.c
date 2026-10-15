/*
 * qd_region.c - Regions.
 *
 * A Region is an arbitrary set of pixels, stored as a per-scanline list of
 * [x0,x1) spans. Atkinson's 1984 format packed runs into a dense RLE because
 * a Region had to fit in a few KB of a 128 KB machine and be manipulated at
 * 8 MHz. We have 16 MB of PSRAM and 320 MHz, so the straightforward form
 * wins: it is cache-friendly for the scanline walk that actually drives
 * BitBlt, and it is possible to write it so that it is obviously correct.
 *
 * A rectangle is the overwhelmingly common case (window clips, menu bar,
 * dialog boxes), so it is stored in place with offsets == NULL and never
 * touches the heap. Set operations promote to the general form.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "qd_internal.h"

#include <stdlib.h>

#define QD_REGION_MAX_SPANS_PER_LINE 1024
#define QD_REGION_MAX_LINES 4096

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

void qd_region_init_rect(qd_region_rect_t *rr, qd_rect_t r)
{
    memset(&rr->reg, 0, sizeof(rr->reg));
    rr->reg.offsets = NULL; /* marks it as a rectangle */
    rr->reg.bounds = r;
    /* Keep the span-range fields consistent with bounds even though a rect
     * region never consults them, so a copy that promotes to a general region
     * later does not start from a zeroed top/bottom. */
    rr->reg.top = r.top;
    rr->reg.bottom = r.bottom;
    rr->reg.span_cap = 0;
    rr->reg.owns_storage = false;
    rr->rect = r;
}

/*
 * An empty region is represented as a degenerate rectangle: offsets == NULL
 * and a zero-area bounds. qd_region_spans() then naturally yields nothing,
 * and qd_region_is_rect() correctly reports true because it *is* expressible
 * as a rect. No sentinel pointer required.
 */
void qd_region_init_empty(qd_region_t *r)
{
    memset(r, 0, sizeof(*r));
    r->offsets = NULL;
    r->bounds = qd_rect_make(0, 0, 0, 0);
    r->spans = NULL;
    r->span_cap = 0;
    r->owns_storage = false;
}

void qd_region_dispose(qd_region_t *r)
{
    if (r == NULL) return;
    if (r->owns_storage) {
        free(r->offsets);
        free(r->spans);
    }
    r->offsets = NULL;
    r->spans = NULL;
    r->owns_storage = false;
    r->span_cap = 0;
    r->bounds = qd_rect_make(0, 0, 0, 0);
}

void qd_region_copy(qd_region_t *dst, const qd_region_t *src)
{
    qd_region_dispose(dst);
    if (qd_region_is_rect(src)) {
        dst->offsets = NULL;
        dst->spans = NULL;
        dst->span_cap = 0;
        dst->bounds = src->bounds;
        dst->top = src->top;
        dst->bottom = src->bottom;
        dst->owns_storage = false;
        return;
    }
    const int nlines = src->bottom - src->top;
    if (nlines <= 0 || nlines > QD_REGION_MAX_LINES) {
        qd_region_init_empty(dst);
        return;
    }
    const int32_t total = src->offsets[nlines]; /* prefix-sum sentinel */
    if (total <= 0) {
        qd_region_init_empty(dst);
        return;
    }

    dst->offsets = (int32_t *)calloc((size_t)nlines + 1, sizeof(int32_t));
    dst->spans = (qd_span_t *)calloc((size_t)total, sizeof(qd_span_t));
    if (dst->offsets == NULL || dst->spans == NULL) {
        qd_region_dispose(dst);
        qd_region_init_empty(dst);
        return;
    }
    memcpy(dst->offsets, src->offsets, (size_t)(nlines + 1) * sizeof(int32_t));
    memcpy(dst->spans, src->spans, (size_t)total * sizeof(qd_span_t));
    dst->bottom = src->bottom;
    dst->top = src->top;
    dst->bounds = src->bounds;
    dst->span_cap = total;
    dst->offsets[nlines] = total; /* sentinel: closes the last line */
    dst->owns_storage = true;
}

/* ------------------------------------------------------------------ *
 * Query
 * ------------------------------------------------------------------ */

int32_t qd_region_spans(const qd_region_t *r, int16_t y,
                        qd_span_t *out, int32_t max_spans)
{
    if (r == NULL || y < r->bounds.top || y >= r->bounds.bottom) return 0;

    if (qd_region_is_rect(r)) {
        if (max_spans < 1) return 0;
        out[0].x0 = r->bounds.left;
        out[0].x1 = r->bounds.right;
        return 1;
    }
    if (r->offsets == NULL) return 0;

    const int line = y - r->top;
    int32_t n = r->offsets[line + 1] - r->offsets[line];
    if (n > max_spans) n = max_spans;
    const qd_span_t *base = r->spans + r->offsets[line];
    for (int32_t i = 0; i < n; i++) out[i] = base[i];
    return n;
}

/* ------------------------------------------------------------------ *
 * Span list primitives. Inputs are sorted and non-overlapping.
 * ------------------------------------------------------------------ */

static int32_t spans_union(const qd_span_t *a, int32_t na,
                           const qd_span_t *b, int32_t nb,
                           qd_span_t *o, int32_t max)
{
    int32_t i = 0, j = 0, k = 0;
    while ((i < na || j < nb) && k < max) {
        qd_span_t s;
        if (j >= nb || (i < na && a[i].x0 <= b[j].x0)) {
            s = a[i++];
        } else {
            s = b[j++];
        }
        if (k > 0 && s.x0 <= o[k - 1].x1) {
            if (s.x1 > o[k - 1].x1) o[k - 1].x1 = s.x1;
        } else {
            o[k++] = s;
        }
    }
    return k;
}

static int32_t spans_intersect(const qd_span_t *a, int32_t na,
                               const qd_span_t *b, int32_t nb,
                               qd_span_t *o, int32_t max)
{
    int32_t i = 0, j = 0, k = 0;
    while (i < na && j < nb && k < max) {
        const int x0 = a[i].x0 > b[j].x0 ? a[i].x0 : b[j].x0;
        const int x1 = a[i].x1 < b[j].x1 ? a[i].x1 : b[j].x1;
        if (x1 > x0) { o[k].x0 = (int16_t)x0; o[k].x1 = (int16_t)x1; k++; }
        if (a[i].x1 < b[j].x1) i++; else j++;
    }
    return k;
}

static void xor_emit(qd_span_t *o, int32_t *k, int32_t max, int x0, int x1)
{
    if (*k < max && x1 > x0) {
        o[*k].x0 = (int16_t)x0;
        o[*k].x1 = (int16_t)x1;
        (*k)++;
    }
}

/* Smallest span endpoint strictly greater than x, across both operand lists.
 * Starts matter as much as ends: a = [0,10)+[30,50) against b = [5,35) has to
 * break at x=5, not run [0,10) as one solid segment. */
static int xor_next_boundary(int x, const qd_span_t *a, int32_t na, int32_t i,
                             const qd_span_t *b, int32_t nb, int32_t j)
{
    int nxt = INT32_MAX;
    if (i < na) {
        if (a[i].x0 > x && a[i].x0 < nxt) nxt = a[i].x0;
        if (a[i].x1 > x && a[i].x1 < nxt) nxt = a[i].x1;
    }
    if (j < nb) {
        if (b[j].x0 > x && b[j].x0 < nxt) nxt = b[j].x0;
        if (b[j].x1 > x && b[j].x1 < nxt) nxt = b[j].x1;
    }
    return nxt;
}

/* Symmetric difference, emitted in ascending order.
 *
 * Implemented as an event sweep over the union of both boundary sets rather
 * than a two-cursor merge. A merge is subtly wrong: emitting the head of the
 * earlier span and then consuming a whole operand loses any overlap between
 * the later span and the *next* span of the other list. a = [0,10)+[30,50)
 * against b = [5,35) must give [0,5)+[10,30)+[35,50), and no amount of
 * careful cursor juggling gets there without tracking coverage state. */
static int32_t spans_xor(const qd_span_t *a, int32_t na,
                         const qd_span_t *b, int32_t nb,
                         qd_span_t *o, int32_t max)
{
    int32_t i = 0, j = 0, k = 0;
    int x = 0;

    if (na > 0) x = a[0].x0;
    else if (nb > 0) x = b[0].x0;

    while (i < na || j < nb) {
        /* Skip whatever has already ended by x, so the current spans are the
         * ones that could still cover it. */
        while (i < na && a[i].x1 <= x) i++;
        while (j < nb && b[j].x1 <= x) j++;
        if (i >= na && j >= nb) break;

        const int in_a = (i < na) && (a[i].x0 <= x && x < a[i].x1);
        const int in_b = (j < nb) && (b[j].x0 <= x && x < b[j].x1);

        const int next = xor_next_boundary(x, a, na, i, b, nb, j);
        if (next <= x) break; /* no forward progress; bail out rather than spin */

        if (in_a != in_b) xor_emit(o, &k, max, x, next);
        x = next;
        if (k >= max) break;
    }
    return k;
}

/* ------------------------------------------------------------------ *
 * Set operations
 * ------------------------------------------------------------------ */

typedef enum { OP_UNION, OP_INTERSECT, OP_XOR } set_op_t;

static void region_binop(qd_region_t *dst, const qd_region_t *a,
                         const qd_region_t *b, set_op_t op)
{
    if (dst == a || dst == b) {
        /* Caller contract says no aliasing; guard anyway rather than corrupt.
         * The copies have to be zero-initialised: qd_region_copy() disposes
         * its destination first, and dispose() reads owns_storage to decide
         * whether to free, so handing it stack garbage is a wild free. */
        qd_region_t ta = { 0 }, tb = { 0 };
        qd_region_copy(&ta, a);
        qd_region_copy(&tb, b);
        region_binop(dst, &ta, &tb, op);
        qd_region_dispose(&ta);
        qd_region_dispose(&tb);
        return;
    }

    /*
     * The scanline window must contain the *result*, not the overlap of the
     * inputs. Only INTERSECT is bounded by the intersection; XOR and UNION
     * both reach outside it. Using the intersection for all three silently
     * truncates the result -- (A xor B) where B is a hole punched in the top
     * corner of A loses the entire lower part of A.
     */
    qd_rect_t box;
    if (op == OP_INTERSECT) {
        box = qd_rect_intersect(a->bounds, b->bounds);
        if (qd_rect_is_empty(box)) {
            qd_region_init_empty(dst);
            return;
        }
    } else {
        box = qd_rect_make(a->bounds.left < b->bounds.left ? a->bounds.left : b->bounds.left,
                           a->bounds.top < b->bounds.top ? a->bounds.top : b->bounds.top,
                           a->bounds.right > b->bounds.right ? a->bounds.right : b->bounds.right,
                           a->bounds.bottom > b->bounds.bottom ? a->bounds.bottom : b->bounds.bottom);
        if (qd_rect_is_empty(box)) {
            qd_region_init_empty(dst);
            return;
        }
    }

    qd_region_dispose(dst);
    dst->top = box.top;
    dst->bottom = box.bottom;
    dst->bounds = box;
    const int nlines = box.bottom - box.top;
    dst->offsets = (int32_t *)calloc((size_t)nlines + 1, sizeof(int32_t));
    if (dst->offsets == NULL) { qd_region_init_empty(dst); return; }
    dst->owns_storage = true;

    /* Two passes: size the span arena, then fill it. */
    qd_span_t sa[QD_REGION_MAX_SPANS_PER_LINE];
    qd_span_t sb[QD_REGION_MAX_SPANS_PER_LINE];
    qd_span_t so[QD_REGION_MAX_SPANS_PER_LINE];
    int32_t total = 0;

    for (int y = box.top; y < box.bottom; y++) {
        const int32_t na = qd_region_spans(a, (int16_t)y, sa, QD_REGION_MAX_SPANS_PER_LINE);
        const int32_t nb = qd_region_spans(b, (int16_t)y, sb, QD_REGION_MAX_SPANS_PER_LINE);
        int32_t n = 0;
        switch (op) {
        case OP_UNION:    n = spans_union(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        case OP_INTERSECT:n = spans_intersect(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        default:          n = spans_xor(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        }
        total += n;
    }

    if (total == 0) {
        qd_region_dispose(dst);
        qd_region_init_empty(dst);
        return;
    }

    dst->spans = (qd_span_t *)calloc((size_t)total, sizeof(qd_span_t));
    if (dst->spans == NULL) {
        qd_region_dispose(dst);
        qd_region_init_empty(dst);
        return;
    }
    dst->span_cap = total;
    dst->offsets[nlines] = total; /* sentinel: closes the last line */

    int32_t off = 0;
    int16_t x0 = 32767, x1 = -32768, y0 = 32767, y1 = -32768;

    for (int y = box.top; y < box.bottom; y++) {
        const int32_t na = qd_region_spans(a, (int16_t)y, sa, QD_REGION_MAX_SPANS_PER_LINE);
        const int32_t nb = qd_region_spans(b, (int16_t)y, sb, QD_REGION_MAX_SPANS_PER_LINE);
        int32_t n = 0;
        switch (op) {
        case OP_UNION:    n = spans_union(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        case OP_INTERSECT:n = spans_intersect(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        default:          n = spans_xor(sa, na, sb, nb, so, QD_REGION_MAX_SPANS_PER_LINE); break;
        }
        dst->offsets[y - box.top] = off; /* prefix sum; sentinel set after */

        /* Track the true bounds as we go rather than walking the arena twice. */
        for (int32_t i = 0; i < n; i++) {
            const qd_span_t s = so[i];
            dst->spans[off + i] = s;
            if (s.x0 < x0) x0 = s.x0;
            if (s.x1 > x1) x1 = s.x1;
        }
        if (n > 0) {
            if (y < y0) y0 = (int16_t)y;
            y1 = (int16_t)(y + 1);
        }
        off += n;
    }

    if (x1 < x0) { qd_region_init_empty(dst); return; }
    dst->bounds = qd_rect_make(x0, y0, x1, y1);
}

void qd_region_union(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b)
{
    region_binop(dst, a, b, OP_UNION);
}

void qd_region_intersect(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b)
{
    region_binop(dst, a, b, OP_INTERSECT);
}

void qd_region_xor(qd_region_t *dst, const qd_region_t *a, const qd_region_t *b)
{
    region_binop(dst, a, b, OP_XOR);
}

int32_t qd_region_area(const qd_region_t *r)
{
    if (r == NULL) return 0;
    if (qd_region_is_rect(r)) {
        if (qd_rect_is_empty(r->bounds)) return 0;
        return (int32_t)qd_rect_width(r->bounds) * qd_rect_height(r->bounds);
    }
    if (r->offsets == NULL) return 0;

    const int nlines = r->bottom - r->top;
    int32_t area = 0;
    for (int line = 0; line < nlines; line++) {
        const qd_span_t *base = r->spans + r->offsets[line];
        const int32_t n = r->offsets[line + 1] - r->offsets[line];
        for (int32_t i = 0; i < n; i++) area += base[i].x1 - base[i].x0;
    }
    return area;
}

void qd_region_offset(qd_region_t *r, int16_t dh, int16_t dv)
{
    if (qd_region_is_rect(r)) {
        r->bounds = qd_rect_make((int16_t)(r->bounds.left + dh),
                                 (int16_t)(r->bounds.top + dv),
                                 (int16_t)(r->bounds.right + dh),
                                 (int16_t)(r->bounds.bottom + dv));
        return;
    }
    if (r->spans == NULL) return;
    for (int32_t i = 0; i < r->span_cap; i++)
        r->spans[i].x0 = (int16_t)(r->spans[i].x0 + dh);
    for (int32_t i = 0; i < r->span_cap; i++)
        r->spans[i].x1 = (int16_t)(r->spans[i].x1 + dh);
    r->bounds.left = (int16_t)(r->bounds.left + dh);
    r->bounds.right = (int16_t)(r->bounds.right + dh);
    r->bounds.top = (int16_t)(r->bounds.top + dv);
    r->bounds.bottom = (int16_t)(r->bounds.bottom + dv);
    r->top = (int16_t)(r->top + dv);
    r->bottom = (int16_t)(r->bottom + dv);
}

void qd_region_invert(qd_region_t *r, qd_rect_t bounds)
{
    qd_region_rect_t full, res;
    qd_region_init_rect(&full, bounds);
    qd_region_xor(&res.reg, r, &full.reg);
    qd_region_dispose(r);
    *r = res.reg;
}
