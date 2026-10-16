/*
 * sim_cocoa.m - macOS backend for the simulator.
 *
 * AppKit, no third-party dependencies. SDL2 would be one line of build
 * system but it is a brew install away from not working, and Cocoa is what
 * every Mac already has.
 *
 * The whole point of this file is to be boring: it opens a window, converts
 * mouse events into sim_mouse() calls, and blits the RGB565 panel. All the
 * interesting decisions live in sim_core.c and src/qd_*.c, where the firmware
 * runs exactly the same code.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#import <Cocoa/Cocoa.h>

#include "sim_core.h"

/* The panel is 480x480; scale it to something comfortable on a Retina display.
 * 2x lands on a 960pt-wide window, which fits any modern Mac, and stays
 * crisp because the source is only 1 bit per pixel in the first place. */
#define SIM_SCALE 2.0

@interface SimView : NSView
@property(nonatomic, strong) NSBitmapImageRep *rep;
@property(nonatomic, assign) sim_t *sim;
@end

@implementation SimView

- (BOOL)isFlipped { return NO; }

- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const int px = (int)(p.x / SIM_SCALE);
    const int py = (int)(p.y / SIM_SCALE);
    sim_mouse(self.sim, SIM_MOUSE_DOWN, (int16_t)px, (int16_t)py);
    sim_present(self.sim);
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    const int px = (int)(p.x / SIM_SCALE);
    const int py = (int)(p.y / SIM_SCALE);
    sim_mouse(self.sim, SIM_MOUSE_DRAG, (int16_t)px, (int16_t)py);
    sim_present(self.sim);
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e
{
    const NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    sim_mouse(self.sim, SIM_MOUSE_UP, (int16_t)(int)(p.x / SIM_SCALE),
              (int16_t)(int)(p.y / SIM_SCALE));
    sim_present(self.sim);
    [self setNeedsDisplay:YES];
}

- (void)keyDown:(NSEvent *)e
{
    if (e.keyCode == 0x1b /* Escape */ || e.keyCode == 0x71 /* q */) {
        [NSApp terminate:nil];
    }
}

/*
 * Draw the panel by making a bitmap rep and handing it straight to Core
 * Graphics, letting AppKit do the scaling. The source is 1bpp expanded to
 * RGB565, so there is nothing to smooth and no reason to interpolate.
 */
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const size_t w = SIM_PANEL_W, h = SIM_PANEL_H;
    if (self.rep == nil ||
        self.rep.pixelsWide != (NSInteger)w ||
        self.rep.pixelsHigh != (NSInteger)h) {
        self.rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:(NSInteger)w
                          pixelsHigh:(NSInteger)h
                       bitsPerSample:16
                     samplesPerPixel:3
                            hasAlpha:NO
                            isPlanar:NO
                      colorSpaceName:NSCalibratedRGBColorSpace
                         bytesPerRow:0
                        bitsPerPixel:0];
        self.rep.size = NSMakeSize(SIM_PANEL_W * SIM_SCALE, SIM_PANEL_H * SIM_SCALE);
    }

    /*
     * RGB565 -> RGB888 in one pass. Only two colours are possible, so this is
     * a compare and a store, not real colour conversion. Done here rather
     * than with a rep format of RGB565 because AppKit has no 565 support on
     * every deployment target we care about.
     */
    uint8_t *dst = self.rep.bitmapData;
    const uint16_t *src = self.sim->panel;
    for (size_t i = 0, n = w * h; i < n; i++) {
        const uint8_t v = (src[i] == 0) ? 0 : 0xFF;
        dst[i * 3 + 0] = v;
        dst[i * 3 + 1] = v;
        dst[i * 3 + 2] = v;
    }

    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    CGContextTranslateCTM(gc.CGContext, 0, SIM_PANEL_H * SIM_SCALE);
    CGContextScaleCTM(gc.CGContext, 1.0, -1.0);
    CGContextScaleCTM(gc.CGContext, SIM_SCALE, SIM_SCALE);
    CGContextSetInterpolationQuality(gc.CGContext, kCGInterpolationNone);

    CGImageRef img = [self.rep CGImage];
    CGContextDrawImage(gc.CGContext,
                       CGRectMake(0, 0, SIM_PANEL_W, SIM_PANEL_H), img);
}

@end

int main(void)
{
    @autoreleasepool {
        sim_t sim;
        sim_init(&sim);
        sim_present(&sim);

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const NSRect frame = NSMakeRect(0, 0, SIM_PANEL_W * SIM_SCALE,
                                        SIM_PANEL_H * SIM_SCALE);
        NSWindowStyleMask style = NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskResizable |
                                  NSWindowStyleMaskMiniaturizable;

        NSWindow *win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:style
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [win setTitle:@"MosaiMac - 480x480 CO5300"];
        [win center];

        SimView *view = [[SimView alloc] initWithFrame:frame];
        view.sim = &sim;
        view.wantsLayer = YES;
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];

        [NSApp activateIgnoringOtherApps:YES];

        /*
         * A slow idle animation so the window is alive before anyone touches
         * it, and so the damage path keeps getting exercised. Same call the
         * firmware's idle loop makes.
         */
        __block int16_t ax = 40, ay = 40, adx = 2, ady = 1;
        NSTimer *t = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                                      repeats:YES
                                                        block:^(NSTimer *timer) {
            (void)timer;
            if (!qd_wm_is_dragging(&sim.wm)) {
                ax = (int16_t)(ax + adx);
                ay = (int16_t)(ay + ady);
                if (ax <= 0 || ax >= SIM_W - 240) adx = (int16_t)-adx;
                if (ay <= 0 || ay >= SIM_H - 170) ady = (int16_t)-ady;
                /*
                 * Through qd_wm_drag, not qd_wm_pointer: there is no button
                 * held here, and pointer() would drop the move. The firmware's
                 * idle loop does exactly this too.
                 */
                qd_wm_drag(&sim.wm, ax, ay);
                sim_present(&sim);
                [view setNeedsDisplay:YES];
            }
        }];
        (void)t;

        [NSApp run];
        sim_shutdown(&sim);
    }
    return 0;
}
