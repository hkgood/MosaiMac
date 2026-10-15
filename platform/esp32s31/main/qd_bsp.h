/*
 * qd_bsp.h - the only file that knows this project runs on an ESP-Mosaico.
 *
 * Everything above this header is portable C and builds on a host. Everything
 * below is board-specific. Keeping the seam here means the QuickDraw core can
 * be tested without hardware and the hardware can be swapped without touching
 * the graphics engine.
 *
 * Board bring-up is delegated to esp-mosaico-bsp rather than reimplemented.
 * That BSP knows the real CO5300 and CST9217 pin mapping, the glass mounting
 * orientation, and the panel's power sequence; hard-coding those here would
 * only be a slower way to get it wrong, and it would be wrong on the next
 * board revision.
 */

#ifndef QD_BSP_H
#define QD_BSP_H

#include <stdbool.h>
#include <stdint.h>

#include "qd/qd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The panel is 480x480. The QuickDraw screen is 480x320, laid out vertically
 * centred with an 80 px band above and below. */
#define QD_BSP_PANEL_W     480
#define QD_BSP_PANEL_H     480
#define QD_BSP_SCREEN_YOFF ((QD_BSP_PANEL_H - QD_SCREEN_H) / 2)

/*
 * Readable slack required at the end of every row of the framebuffer.
 *
 * The ROP engine reads up to 8 bytes past the bits it consumes when it slides
 * a misaligned source through a 64-bit window, and the partial-byte helpers
 * read one byte past the end. Both stay inside the row's own stride, so this
 * costs 8 B x height. Matches QD_ROW_SLACK in the portable header.
 */
#define QD_BSP_ROW_SLACK QD_ROW_SLACK

/*
 * Bring up the display and touch. Blocking.
 *
 * The panel is opened without the LVGL adapter, so the caller owns the
 * rendering serialisation: every esp_lcd_panel_* call has to be serialised
 * against the framebuffer writes that feed it. This file does that with a
 * single task doing both, which is what the demo does.
 */
bool qd_bsp_init(void);

/* The 1bpp QuickDraw screen bitmap, backed by PSRAM. */
qd_bitmap_t *qd_bsp_bitmap(void);

/*
 * Accumulate damage, then transmit it.
 *
 * Damage is a region, not a rectangle. The XOR of a window's old and new
 * position has a bounding box equal to the whole window even for a one-pixel
 * move, so accumulating damage as a rectangle and pushing that rectangle
 * would put 82 KB on a link that can carry a few KB per frame. Keeping it as
 * a region and splitting it into runs is what makes interactive dragging
 * viable: measured, a 1 px move is 818 px of damage instead of 40,800.
 */
void qd_bsp_mark_dirty(const qd_region_t *r);

/* Move the accumulated damage out into `out`, leaving the accumulator empty.
 * `out` must be a valid region; it is overwritten. */
void qd_bsp_take_dirty(qd_region_t *out);

/*
 * Transmit a damage region, scanline by scanline, coalescing vertically
 * identical runs into maximal rectangles. Coordinates are QuickDraw screen
 * coordinates; this offsets into the panel and rounds each rectangle outward
 * to even pixel bounds, which the CO5300 requires.
 *
 * Returns the number of rectangles transmitted, or negative on driver error.
 */
int qd_bsp_flush(const qd_region_t *r);

typedef enum {
    QD_BSP_EVENT_NONE = 0,
    QD_BSP_EVENT_DOWN,
    QD_BSP_EVENT_DRAG,
    QD_BSP_EVENT_UP
} qd_bsp_event_kind_t;

typedef struct {
    qd_bsp_event_kind_t kind;
    int16_t x;   /* screen coordinates, already lifted out of the bezel */
    int16_t y;
    int16_t pressure;
} qd_bsp_event_t;

/*
 * Poll the touch digitiser. Fills `out` with one pending event or
 * QD_BSP_EVENT_NONE when the finger is up or nothing has changed.
 *
 * Single-touch by design: the CST9217 reports one contact, and QuickDraw's
 * original event model has no notion of a second pointer. Multi-touch would
 * need a reworked event layer, not just a bigger touch struct.
 */
qd_bsp_event_t qd_bsp_poll_touch(void);

#ifdef __cplusplus
}
#endif

#endif /* QD_BSP_H */
