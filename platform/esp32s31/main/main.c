/*
 * main.c - bring up the panel, run a small demo, and keep the frame rate up
 * by only pushing what changed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "qd/qd.h"
#include "qd_bsp.h"

static const char *TAG = "qd";

static qd_grafport_t s_port;
static qd_region_rect_t s_clip;   /* the window's current dirty region */
static qd_region_rect_t s_new;    /* the next window's dirty region */
static qd_region_t s_damage = { 0 }; /* what actually gets pushed */
static int16_t s_win_x = 40, s_win_y = 40;

/*
 * Classic QuickDraw dirty-rectangle idiom: keep the region of the screen the
 * window currently covers, XOR the incoming and outgoing regions on every
 * move, and that difference is exactly the part that has to be repainted.
 */
static void window_moves_to(int16_t nx, int16_t ny)
{
    const int16_t w = 240, h = 170;

    qd_region_init_rect(&s_new, qd_rect_make(nx, ny, (int16_t)(nx + w), (int16_t)(ny + h)));

    qd_region_xor(&s_clip.reg, &s_clip.reg, &s_new.reg);

    /* The old window is wherever we are leaving, painted back to white. */
    qd_port_save(&s_port);
    qd_port_set_clip_region(&s_port, &s_clip.reg);
    qd_fill_rect(&s_port, s_clip.rect, QD_ROP_CLEAR);
    qd_port_restore(&s_port);

    /* The new window is drawn on top. */
    qd_port_save(&s_port);
    qd_port_set_clip_region(&s_port, &s_new.reg);
    qd_frame_rect(&s_port, s_new.rect, 1);
    qd_fill_rect(&s_port, qd_rect_make(s_new.rect.left + 1, s_new.rect.top + 1,
                                       s_new.rect.right - 1, s_new.rect.top + 18),
                 QD_ROP_SRC_COPY);
    qd_port_restore(&s_port);

    qd_bsp_mark_dirty(&s_clip.reg);

    /* s_new becomes s_clip, and s_new is re-armed as an empty rect region so
     * the next call can build into it. Handing the storage over rather than
     * copying keeps a window move allocation-free after the first frame. */
    qd_region_dispose(&s_clip.reg);
    s_clip = s_new;
    qd_region_init_rect(&s_new, s_clip.rect);
}

static void draw_menu_bar(void)
{
    qd_port_save(&s_port);
    qd_fill_rect(&s_port, qd_rect_make(0, 0, QD_SCREEN_W, 20), QD_ROP_SRC_COPY);
    /* A handful of fake title-bar blocks, enough to show the ROP working. */
    qd_fill_rect(&s_port, qd_rect_make(6, 6, 46, 14), QD_ROP_CLEAR);
    qd_fill_rect(&s_port, qd_rect_make(54, 6, 74, 14), QD_ROP_CLEAR);
    qd_frame_rect(&s_port, qd_rect_make(82, 5, 120, 15), 1);
    qd_port_restore(&s_port);
    {
        static qd_region_rect_t bar;
        qd_region_init_rect(&bar, qd_rect_make(0, 0, QD_SCREEN_W, 20));
        qd_bsp_mark_dirty(&bar.reg);
    }
}

void app_main(void)
{
    if (!qd_bsp_init()) {
        ESP_LOGE(TAG, "bsp init failed");
        return;
    }
    qd_bitmap_t *bm = qd_bsp_bitmap();

    qd_port_init(&s_port, bm);
    qd_port_set_bounds(&s_port, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H));
    qd_port_set_origin(&s_port, 0, 0);

    qd_fill_rect(&s_port, qd_rect_make(0, 0, QD_SCREEN_W, QD_SCREEN_H), QD_ROP_CLEAR);
    draw_menu_bar();

    /* Prime the window's dirty region before the first move. */
    qd_region_init_rect(&s_new, qd_rect_make(s_win_x, s_win_y,
                                             (int16_t)(s_win_x + 240),
                                             (int16_t)(s_win_y + 170)));
    qd_region_copy(&s_clip.reg, &s_new.reg);
    s_clip.rect = s_new.rect;

    ESP_LOGI(TAG, "screen is %dx%d 1bpp inside a %dx%d panel",
             QD_SCREEN_W, QD_SCREEN_H, QD_BSP_PANEL_W, QD_BSP_PANEL_H);

    int16_t dx = 2, dy = 1;
    uint32_t frames = 0, flushes = 0;
    int64_t t_next = esp_timer_get_time();

    for (;;) {
        /* --- input --- */
        const qd_bsp_event_t ev = qd_bsp_poll_touch();
        if (ev.kind == QD_BSP_EVENT_DOWN) {
            /* Drag the window with the finger. */
            static int16_t grab_dx, grab_dy;
            static bool grabbing;
            if (ev.kind == QD_BSP_EVENT_DOWN) {
                grabbing = (ev.x >= s_win_x && ev.x < s_win_x + 240 &&
                            ev.y >= s_win_y && ev.y < s_win_y + 170);
                grab_dx = (int16_t)(ev.x - s_win_x);
                grab_dy = (int16_t)(ev.y - s_win_y);
            }
            if (grabbing) {
                int16_t nx = (int16_t)(ev.x - grab_dx);
                int16_t ny = (int16_t)(ev.y - grab_dy);
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx > QD_SCREEN_W - 240) nx = QD_SCREEN_W - 240;
                if (ny > QD_SCREEN_H - 170) ny = QD_SCREEN_H - 170;
                s_win_x = nx;
                s_win_y = ny;
                window_moves_to(nx, ny);
            }
        }

        /* --- animation when nobody is touching --- */
        if (ev.kind == QD_BSP_EVENT_NONE) {
            s_win_x = (int16_t)(s_win_x + dx);
            s_win_y = (int16_t)(s_win_y + dy);
            if (s_win_x <= 0 || s_win_x >= QD_SCREEN_W - 240) dx = (int16_t)-dx;
            if (s_win_y <= 20 || s_win_y >= QD_SCREEN_H - 170) dy = (int16_t)-dy;
            window_moves_to(s_win_x, s_win_y);
        }

        /* --- present --- */
        qd_bsp_take_dirty(&s_damage);
        if (qd_region_area(&s_damage) > 0) {
            if (qd_bsp_flush(&s_damage) > 0) flushes++;
            frames++;
        }
        qd_region_dispose(&s_damage);

        const int64_t now = esp_timer_get_time();
        if (now - t_next >= 5000000) {
            ESP_LOGI(TAG, "%u frames, %u flushes in 5 s (%u flushes/s)",
                     (unsigned)frames, (unsigned)flushes,
                     (unsigned)(flushes / 5));
            frames = 0;
            flushes = 0;
            t_next = now;
        }

        vTaskDelay(pdMS_TO_TICKS(16)); /* ~60 Hz cap */
    }
}
