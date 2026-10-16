/*
 * sim_core.h - the portable half of the desktop simulator.
 *
 * Same QuickDraw engine, same window manager, same damage logic as the
 * firmware. The only thing the platform backends do is open a window, turn
 * mouse events into calls here, and blit whatever this hands back.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SIM_CORE_H
#define SIM_CORE_H

#include "qd/qd.h"
#include "qd/qd_win.h"

#define SIM_W QD_SCREEN_W
#define SIM_H QD_SCREEN_H
/* The firmware puts the 480x320 screen in a 480x480 panel with black bands.
 * The simulator reproduces that so what you see is the panel, not just the
 * logical screen. */
#define SIM_PANEL_W QD_SCREEN_W
#define SIM_PANEL_H 480
#define SIM_PANEL_YOFF ((SIM_PANEL_H - SIM_H) / 2)

typedef enum {
    SIM_MOUSE_DOWN = 0,
    SIM_MOUSE_DRAG,
    SIM_MOUSE_UP,
    SIM_MOUSE_MOVE
} sim_mouse_kind_t;

/* Panel-resolution RGB565 frame the backends blit. */
typedef struct {
    qd_bitmap_t bitmap;
    qd_wm_t     wm;
    uint16_t   *panel;        /* SIM_PANEL_W * SIM_PANEL_H */
    size_t      panel_bytes;
    bool        dragging;
    bool        needs_full;   /* the panel has never been painted */
    int16_t     grab_dx, grab_dy;
    bool        quit;
} sim_t;

void sim_init(sim_t *s);
void sim_shutdown(sim_t *s);

/*
 * Feed one event. Coordinates are in *panel* space, i.e. the same space the
 * firmware sees after the CST9220's own scaling, so the simulator's bezel
 * maths is exercised the same way the driver's is.
 */
void sim_mouse(sim_t *s, sim_mouse_kind_t kind, int16_t x, int16_t y);

/* Present whatever accumulated damage since the last call. */
void sim_present(sim_t *s);

/* Paint the desktop. Exposed so a backend can force a full redraw. */
void sim_paint_desktop(void *ctx, qd_grafport_t *port);
void sim_paint_window(void *ctx, qd_grafport_t *port, qd_rect_t frame);

#endif /* SIM_CORE_H */
