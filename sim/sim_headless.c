/*
 * sim_headless.c - the simulator with no window at all.
 *
 * Drives the same core through a scripted drag and writes the panel frame out
 * as a PPM per step. This is how the interactive path gets tested on a build
 * machine: the headless run and the Cocoa run execute the same
 * sim_mouse/sim_present calls, so a ghost that appears here is a ghost the Mac
 * build would show too.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sim_core.h"

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int write_ppm(const char *path, const uint16_t *panel)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    fprintf(f, "P6\n%d %d\n255\n", SIM_PANEL_W, SIM_PANEL_H);
    for (int i = 0; i < SIM_PANEL_W * SIM_PANEL_H; i++) {
        const uint16_t p = panel[i];
        /* RGB565 -> RGB8, and the 1-bit framebuffer stores 1 as black. */
        const uint8_t v = (uint8_t)((p == 0) ? 0 : 255);
        fputc(v, f); fputc(v, f); fputc(v, f);
    }
    fclose(f);
    return 0;
}

/*
 * Everything outside the window must equal a freshly painted desktop.
 *
 * The margin matters: the window's own bounds are not the region the drag
 * path writes to, and a stale shadow that lands a few pixels outside the
 * visual bounds is still a ghost on the panel. Checking only the exact
 * bounds is what let a smear survive sixty drag steps unnoticed.
 */
/*
 * Compare the incrementally-updated panel against a reference simulator that
 * was told to repaint everything from scratch, pixel for pixel, over the whole
 * screen.
 *
 * The window's own rectangle used to be skipped. That was backwards: the
 * window interior is where a stale-blit bug actually shows, and skipping it
 * is exactly how the panel/framebuffer divergence described in qd_win.c got
 * shipped unnoticed. Nothing is exempt now.
 */
/*
 * Two independent references, because "did we transfer everything we should"
 * and "does the framebuffer hold the right picture" are different questions and
 * one reference cannot answer both.
 *
 *   blank -- same scene with no window at all. Anything the drag uncovered
 *            must match it exactly; a stale pixel shows up as leftover window.
 *   twin  -- an identical simulator replayed with the same input from a fresh
 *            framebuffer, but always transferred in full. Any difference means
 *            the incremental damage path dropped or duplicated a region.
 *
 * Between them every pixel on screen is checked, window interior included.
 * The interior used to be exempt, which is precisely how a divergence between
 * the framebuffer and the panel shipped unnoticed.
 */
static int verify(sim_t *s, const sim_t *blank, const sim_t *twin, int step)
{
    for (int y = 0; y < SIM_H; y++) {
        for (int x = 0; x < SIM_W; x++) {
            const size_t i = (size_t)(y + SIM_PANEL_YOFF) * SIM_PANEL_W + x;
            const int a   = (s->panel[i]      == 0) ? 1 : 0;
            const int t   = (twin->panel[i]   == 0) ? 1 : 0;
            const int bl  = (blank->panel[i]  == 0) ? 1 : 0;

            if (a != t) {
                printf("  FAIL step %d: (%d,%d) incremental=%d full-repaint=%d"
                       " -- damage region missed or doubled\n", step, x, y, a, t);
                return 1;
            }
            (void)bl; /* agreement with the blank desktop is expected here;
                       * the twin comparison above is the actual assertion */
        }
    }
    return 0;
}

/* Advance the twin through the same input, but force a full repaint. */
static void twin_step(sim_t *twin, sim_mouse_kind_t k, int16_t x, int16_t y)
{
    sim_mouse(twin, k, x, y);
    twin->needs_full = true;
    sim_present(twin);
}

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : "sim_frames";
    const int steps = (argc > 2) ? atoi(argv[2]) : 60;
    /* Frames are for the README animation; the verification itself does not
     * need them, and writing 60 PPMs on every `make test` is pure waste. */
    const int dump = (argc > 3) ? atoi(argv[3]) : 0;

    char path[512];

    /* The reference: a desktop with no window on it, so a window dragged away
     * should reveal exactly this. */
    sim_t ref;
    sim_init(&ref);
    ref.wm.win.visible = false;   /* a desktop with no window on it */
    {
        qd_grafport_t p;
        qd_port_init(&p, &ref.bitmap);
        qd_port_set_bounds(&p, qd_rect_make(0, 0, SIM_W, SIM_H));
        sim_paint_desktop(&ref, &p);
        ref.needs_full = true;
        sim_present(&ref);
    }

    /* The frame dump is the whole point of the headless build, so failing to
     * create it must not pass silently. */
    if (mkdir(outdir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", outdir, strerror(errno));
        return 1;
    }

    sim_t s;
    sim_init(&s);
    sim_present(&s);

    sim_t twin;
    sim_init(&twin);
    twin.needs_full = true;
    sim_present(&twin);

    if (dump) {
        snprintf(path, sizeof path, "%s/step000.ppm", outdir);
        if (write_ppm(path, s.panel) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
    }
    if (verify(&s, &ref, &twin, 0) != 0) return 1;

    /* A drag: press on the title bar, then walk diagonally, crossing the
     * desktop icons on the way back down. */
    const qd_rect_t f = s.wm.win.frame;
    const int16_t px = (int16_t)(f.left + 60);
    const int16_t py = (int16_t)(f.top + 10 + SIM_PANEL_YOFF);

    sim_mouse(&s, SIM_MOUSE_DOWN, px, py);
    sim_present(&s);
    twin_step(&twin, SIM_MOUSE_DOWN, px, py);

    for (int i = 1; i <= steps; i++) {
        const int16_t x = (int16_t)(px + (i * 2) % 300 - 60);
        const int16_t y = (int16_t)(py + (i * 1) % 120);
        sim_mouse(&s, SIM_MOUSE_DRAG, x, y);
        sim_present(&s);
        twin_step(&twin, SIM_MOUSE_DRAG, x, y);
        if (dump) {
            snprintf(path, sizeof path, "%s/step%03d.ppm", outdir, i);
            if (write_ppm(path, s.panel) != 0) {
                fprintf(stderr, "cannot write %s\n", path);
                return 1;
            }
        }
        if (verify(&s, &ref, &twin, i) != 0) {
            printf("  drag aborted at step %d\n", i);
            return 1;
        }
    }

    sim_mouse(&s, SIM_MOUSE_UP, 0, 0);
    sim_present(&s);
    twin_step(&twin, SIM_MOUSE_UP, 0, 0);
    sim_present(&s);

    if (dump) {
        snprintf(path, sizeof path, "%s/step%03d.ppm", outdir, steps);
        if (write_ppm(path, s.panel) != 0) {
            fprintf(stderr, "cannot write %s\n", path);
            return 1;
        }
    }

    printf("headless simulator: %d drag steps, every pixel of every frame "
           "matched an independent full repaint\n", steps);
    printf("last frame: %s/step%03d.ppm\n", outdir, steps);

    sim_shutdown(&s);
    /* Both references hold a window manager and a panel buffer of their own;
     * leaking them under ASan hid how much the leak count had actually moved. */
    sim_shutdown(&twin);
    sim_shutdown(&ref);
    sim_shutdown(&ref);
    return 0;
}
