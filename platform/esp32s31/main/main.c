/*
 * main.c - bring up the panel, run a draggable window, keep the frame rate up
 * by only pushing what changed.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The window logic is not written here. It lives in qd_win.c, shared with the
 * desktop simulator, because a window move is the part of this most likely to
 * be wrong and the part most worth iterating on without reflashing: drag it on
 * a Mac, see exactly what the panel would show.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "qd/qd.h"
#include "qd/qd_win.h"
#include "qd_bsp.h"

static const char *TAG = "qd";

#define MENUBAR_H 20

static qd_grafport_t s_port;
static qd_wm_t s_wm;

/* ------------------------------------------------------------------ *
 * The desktop, which only this application knows what looks like
 * ------------------------------------------------------------------ */

static void fill_black(qd_rect_t r)
{
    qd_port_set_black(&s_port);
    qd_fill_rect(&s_port, r, QD_ROP_SRC_COPY);
}
static void fill_white(qd_rect_t r)
{
    qd_port_set_white(&s_port);
    qd_fill_rect(&s_port, r, QD_ROP_SRC_COPY);
}

static void paint_desktop(void *ctx, qd_grafport_t *port)
{
    (void)ctx;
    fill_white(qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));

    fill_black(qd_rect_make(0, 0, QD_SCREEN_W, MENUBAR_H));
}

static void paint_window(void *ctx, qd_grafport_t *port, qd_rect_t f)
{
    (void)ctx;
    qd_region_rect_t w = { 0 };
    qd_region_init_rect(&w, f);
    qd_port_save(port);
    qd_port_set_clip_region(port, &w.reg);

    /* A few rules in the content area, because a blank window makes damage
     * bugs invisible: there has to be something to smear. */
    qd_port_set_black(port);
    for (int i = 1; i < 6; i++) {
        const int y = f.top + QD_WIN_TITLE_H + i * 20;
        if (y + 4 >= f.bottom) break;
        qd_fill_rect(port, qd_rect_make((int16_t)(f.left + 14), (int16_t)y,
                                        (int16_t)(f.right - 14 - (i * 22)),
                                        (int16_t)(y + 2)),
                     QD_ROP_SRC_COPY);
    }
    /* An inverted band, so dragging across it is obvious. */
    if (f.bottom - f.top > 120) {
        qd_port_set_white(port);
        qd_fill_rect(port, qd_rect_make((int16_t)(f.left + 12),
                                        (int16_t)(f.top + QD_WIN_TITLE_H + 6),
                                        (int16_t)(f.left + 150),
                                        (int16_t)(f.top + QD_WIN_TITLE_H + 22)),
                      QD_ROP_SRC_COPY);
    }

    qd_port_restore(port);
    qd_region_dispose(&w.reg);
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    if (!qd_bsp_init()) {
        ESP_LOGE(TAG, "bsp init failed");
        return;
    }
    qd_bitmap_t *bm = qd_bsp_bitmap();

    qd_port_init(&s_port, bm);
    qd_port_set_bounds(&s_port, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));

    qd_wm_init(&s_wm, bm, qd_rect_make(0, 0, QD_SCREEN_W, MENUBAR_H));
    qd_wm_set_painters(&s_wm, paint_desktop, paint_window, NULL);
    qd_wm_open(&s_wm, qd_rect_make(40, MENUBAR_H + 6, 280, MENUBAR_H + 176));

    ESP_LOGI(TAG, "screen is %dx%d 1bpp inside a %dx%d panel",
             QD_SCREEN_W, QD_SCREEN_H, QD_BSP_PANEL_W, QD_BSP_PANEL_H);

    qd_region_t dmg = { 0 };
    uint32_t frames = 0, flushes = 0;
    int64_t t_next = esp_timer_get_time();

    /* Idle animation, so there is something to see before anyone touches it. */
    int16_t ax = 40, ay = MENUBAR_H + 6, adx = 2, ady = 1;

    for (;;) {
        /*
         * One call, whole pointer state machine. qd_wm_pointer() works out
         * whether this is a grab, a move of an existing grab, or the release,
         * and applies the same clamping the simulator does -- so a drag cannot
         * behave one way on the panel and another way on the Mac.
         */
        const qd_bsp_event_t ev = qd_bsp_poll_touch();
        if (ev.kind == QD_BSP_EVENT_DOWN || ev.kind == QD_BSP_EVENT_DRAG) {
            qd_wm_pointer(&s_wm, ev.x, ev.y, true);
        } else if (ev.kind == QD_BSP_EVENT_UP) {
            qd_wm_pointer(&s_wm, ev.x, ev.y, false);
        } else {
            /* Idle animation, so there is something to see before anyone
             * touches the screen. */
            ax = (int16_t)(ax + adx);
            ay = (int16_t)(ay + ady);
            if (ax <= 0 || ax >= QD_SCREEN_W - 240) adx = (int16_t)-adx;
            if (ay <= MENUBAR_H || ay >= QD_SCREEN_H - 170) ady = (int16_t)-ady;
            qd_wm_drag(&s_wm, ax, ay);
        }

        qd_wm_take_damage(&s_wm, &dmg);
        if (qd_region_area(&dmg) > 0) {
            qd_bsp_mark_dirty(&dmg);
            if (qd_bsp_flush(&dmg) > 0) flushes++;
            frames++;
        }
        qd_region_dispose(&dmg);

        const int64_t now = esp_timer_get_time();
        if (now - t_next >= 5000000) {
            ESP_LOGI(TAG, "%u frames, %u flushes in 5 s",
                     (unsigned)frames, (unsigned)flushes);
            frames = 0;
            flushes = 0;
            t_next = now;
        }

        vTaskDelay(pdMS_TO_TICKS(16)); /* ~60 Hz cap */
    }
}
