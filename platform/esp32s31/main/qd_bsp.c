/*
 * qd_bsp.c - display bring-up and the 1bpp -> RGB565 push path.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NOT YET COMPILED. No ESP-IDF toolchain was available where this was written,
 * so treat it as a reviewed skeleton rather than a verified build. The first
 * thing to do with real hardware is `idf.py build` and reconcile whatever the
 * BSP's actual API disagrees about.
 *
 * The design decision worth knowing: the panel is opened through
 * esp-mosaico-bsp without the LVGL adapter, so this file does the rendering
 * and the transmission itself. Nothing here uses LVGL. QuickDraw's whole
 * reason to exist is a 1-bit framebuffer with dirty rectangles, and a
 * retained-mode widget toolkit on top of that would be throwing away the
 * thing that makes it fast.
 */

#include "qd_bsp.h"

#include <string.h>

#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"

static const char *TAG = "qd_bsp";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

static uint8_t *s_fb_mem;
static size_t s_fb_size;
static qd_bitmap_t s_fb;
static bool s_ready;

/* Staging for the 1bpp -> RGB565 expansion, one band of full width. Keeping
 * this a band rather than a full frame costs 30 KB instead of 300 KB, and a
 * dirty rect taller than the band is simply sent in slices. */
#define QD_BSP_BAND_H 32

/* Bound on spans per scanline when splitting damage into runs. Matches
 * the engine's internal per-scanline capacity; a window move never
 * produces anywhere near this many spans per line. */
#define QD_BSP_MAX_SPANS 64
static uint16_t s_band[QD_BSP_PANEL_W * QD_BSP_BAND_H] __attribute__((aligned(4)));

/* Accumulated damage, kept as a region so a diagonal window move does not
 * degrade into a full-window rectangle push. */
static qd_region_t s_damage;

/* ------------------------------------------------------------------ *
 * Damage accumulation
 * ------------------------------------------------------------------ */

void qd_bsp_take_dirty(qd_region_t *out)
{
    qd_region_copy(out, &s_damage);
    qd_region_init_empty(&s_damage);
}

void qd_bsp_mark_dirty(const qd_region_t *r)
{
    if (r == NULL || qd_region_area(r) == 0) return;
    qd_region_union(&s_damage, &s_damage, r);
}

/* ------------------------------------------------------------------ *
 * Bring-up
 * ------------------------------------------------------------------ */

static bool bsp_alloc_framebuffer(void)
{
    const size_t bytes = (size_t)QD_SCREEN_STRIDE * QD_SCREEN_H;

    s_fb_mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_fb_mem == NULL) {
        /* The board has PSRAM, but it may be uninitialised or this may be a
         * variant without it. The framebuffer is only ~21 KB, so internal RAM
         * is an acceptable fallback -- just slower to fill and to feed. */
        ESP_LOGW(TAG, "PSRAM alloc failed (%u B), falling back to internal RAM",
                 (unsigned)bytes);
        s_fb_mem = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_fb_mem == NULL) {
        ESP_LOGE(TAG, "no memory for a %u B framebuffer", (unsigned)bytes);
        return false;
    }
    memset(s_fb_mem, 0, bytes);

    s_fb_size = bytes;
    s_fb.base = s_fb_mem;
    s_fb.width = QD_SCREEN_W;
    s_fb.height = QD_SCREEN_H;
    s_fb.rowBytes = QD_SCREEN_STRIDE; /* stride includes the per-row slack */
    s_fb.seed = 0;
    return true;
}

qd_bitmap_t *qd_bsp_bitmap(void)
{
    return s_ready ? &s_fb : NULL;
}

bool qd_bsp_init(void)
{
    if (!bsp_alloc_framebuffer()) return false;

    const bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    if (bsp_display_new(&cfg, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed");
        return false;
    }

    /*
     * Touch is optional. A board without the digitiser, or one where the I2C
     * bus is not routed the way this BSP expects, should still be able to
     * draw -- the demo degrades to animation without a finger.
     */
    if (bsp_touch_new(cfg.rotation, &s_touch) != ESP_OK) {
        ESP_LOGW(TAG, "no touch digitiser; continuing with pointer input disabled");
        s_touch = NULL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "qd_bsp ready: %dx%d 1bpp (stride=%d, fb=%u B) inside %dx%d",
             QD_SCREEN_W, QD_SCREEN_H, (int)QD_SCREEN_STRIDE,
             (unsigned)s_fb_size, QD_BSP_PANEL_W, QD_BSP_PANEL_H);
    return true;
}

/* ------------------------------------------------------------------ *
 * Transmission
 * ------------------------------------------------------------------ */

static inline int16_t even_floor(int16_t v) { return (int16_t)(v & ~1); }
static inline int16_t even_ceil(int16_t v) { return (int16_t)((v + 1) & ~1); }

static bool push_rect(qd_rect_t r)
{
    r = qd_rect_intersect(r, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));
    if (qd_rect_is_empty(r)) return false;

    /*
     * The CO5300 wants an even-aligned window. Rounding outward is what makes
     * the extra column or row get pushed too; rounding inward would leave
     * stale pixels on screen, which is the classic dirty-rect bug.
     */
    r.left = even_floor(r.left);
    r.top = even_floor(r.top);
    r.right = even_ceil(r.right);
    r.bottom = even_ceil(r.bottom);
    r = qd_rect_intersect(r, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));
    if (qd_rect_is_empty(r)) return false;

    /* Lift out of the top bezel, then clamp so the panel never sees a window
     * past its own edge -- the DDIC treats that as a fault, not a no-op. */
    int py = r.top + QD_BSP_SCREEN_YOFF;
    int ph = r.bottom - r.top;
    if (py < 0) { ph += py; py = 0; }
    if (py + ph > QD_BSP_PANEL_H) ph = QD_BSP_PANEL_H - py;
    if (ph <= 0) return false;

    const int w = r.right - r.left;
    for (int y = r.top; y < r.top + ph; y += QD_BSP_BAND_H) {
        int h = QD_BSP_BAND_H;
        if (y + h > r.top + ph) h = r.top + ph - y;

        qd_expand_dirty_rect(&s_fb, qd_rect_make(r.left, y, r.right, y + h), s_band);
        if (esp_lcd_panel_draw_bitmap(s_panel, r.left, y + QD_BSP_SCREEN_YOFF, w, h,
                                      s_band) != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap failed at %d,%d %dx%d", r.left,
                     y + QD_BSP_SCREEN_YOFF, w, h);
            return false;
        }
    }
    return true;
}

int qd_bsp_flush(const qd_region_t *r)
{
    if (!s_ready || r == NULL) return -1;
    if (qd_region_area(r) == 0) return 0;

    /* A rectangular damage region is one rectangle, no walk needed. */
    if (qd_region_is_rect(r)) return push_rect(r->bounds) ? 1 : 0;

    /*
     * Decompose into maximal rectangles: a run started on one scanline is
     * extended downward for as long as the identical run keeps appearing. A
     * window move's damage collapses from two rectangles per scanline per edge
     * down to roughly four rectangles total, which is the difference between
     * 2 KB and 16 KB on the wire.
     */
    enum { QD_MAX_ACTIVE = 64 };
    int sent = 0;
    qd_rect_t active[QD_MAX_ACTIVE];
    int nactive = 0;
    qd_span_t spans[QD_BSP_MAX_SPANS];

    for (int y = r->top; y < r->bottom; y++) {
        const int32_t ns = qd_region_spans(r, (int16_t)y, spans, QD_BSP_MAX_SPANS);
        if (ns <= 0) continue;

        int nnext = 0;
        for (int32_t i = 0; i < ns; i++) {
            bool extended = false;
            for (int a = 0; a < nactive; a++) {
                if (active[a].left == spans[i].x0 && active[a].right == spans[i].x1 &&
                    active[a].bottom == y) {
                    active[a].bottom = (int16_t)(y + 1);
                    extended = true;
                    break;
                }
            }
            if (!extended && nnext < QD_MAX_ACTIVE) {
                active[nnext].left = spans[i].x0;
                active[nnext].top = (int16_t)y;
                active[nnext].right = spans[i].x1;
                active[nnext].bottom = (int16_t)(y + 1);
                nnext++;
            }
        }
        /* Anything not extended on this scanline is finished. */
        for (int a = 0; a < nactive; a++) {
            if (active[a].bottom == y) {
                if (push_rect(active[a])) sent++;
            } else if (nnext < QD_MAX_ACTIVE) {
                active[nnext++] = active[a];
            }
        }
        nactive = nnext;
    }
    for (int a = 0; a < nactive; a++) {
        if (push_rect(active[a])) sent++;
    }
    return sent;
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

qd_bsp_event_t qd_bsp_poll_touch(void)
{
    qd_bsp_event_t ev = { QD_BSP_EVENT_NONE, 0, 0, 0 };
    if (!s_ready || s_touch == NULL) return ev;

    /*
     * esp_lcd_touch_get_coordinates() is deprecated as of this component
     * version and returns "no data" rather than "released", so contact has to
     * come from a point count instead. The whole struct is handed back by
     * value, which is why this cannot be a pointer to caller storage.
     */
    esp_lcd_touch_point_data_t pt = { 0 };
    uint8_t n = 0;

    const esp_err_t err = esp_lcd_touch_get_data(s_touch, &pt, &n, 1);

    static bool was_down;
    const bool pressed = (err == ESP_OK) && (n > 0);

    if (!pressed) {
        if (was_down) ev.kind = QD_BSP_EVENT_UP;
        was_down = false;
        return ev;
    }

    ev.kind = was_down ? QD_BSP_EVENT_DRAG : QD_BSP_EVENT_DOWN;
    was_down = true;

    /* The screen is the full panel width, so X passes straight through. Y is
     * lifted out of the top bezel; a touch in the bottom bezel clamps to the
     * bottom scanline rather than wrapping to a negative row. */
    const int16_t x = pt.x > (uint16_t)(QD_SCREEN_W - 1) ? (int16_t)(QD_SCREEN_W - 1)
                                                        : (int16_t)pt.x;
    int16_t y = (int16_t)(pt.y - QD_BSP_SCREEN_YOFF);
    if (y < 0) y = 0;
    if (y > QD_SCREEN_H - 1) y = QD_SCREEN_H - 1;
    ev.x = x;
    ev.y = y;
    ev.pressure = (int16_t)(pt.strength > 0 ? pt.strength : 1);
    return ev;
}
