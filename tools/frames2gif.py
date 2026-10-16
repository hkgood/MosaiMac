#!/usr/bin/env python3
"""
Turn the engine's frame dump into the artefacts the README and the page use.

Two outputs:

  drag.gif     the drag itself, as an animation
  panel.png    one frame, placed inside a 480x480 bezel the way the S31 shows it

Note there is no resampling or dithering anywhere in here. Averaging or
dithering 1-bit content destroys 5x7 glyphs: there is no intermediate grey to
dither between, so a filter just deletes the type. The framebuffer is already
the right size for the panel.

    python3 tools/frames2gif.py
"""

import os
import sys

from PIL import Image

# Written by `make sim-frames`. This used to come from tools/replay.c, which
# carried its own copy of the window-drawing code; the simulator now runs the
# shared qd_win.c instead, so the animation and the firmware cannot disagree.
FRAMES = "build/sim_frames"
# The simulator already dumps the full 480x480 panel, black bezel included --
# it is the thing the S31 actually drives. This script used to wrap a
# 480x320 dump in a bezel itself, which meant the animation and the device
# could disagree about what the picture looks like.
W, H = 480, 480
FPS = 24

# Bayer 2x2 threshold matrix, normalised to 0..1.
BAYER = [[0 / 4, 3 / 4], [2 / 4, 1 / 4]]


def load(path):
    """
    Read a P6 PPM. The dump is RGB triplets, so the payload is three times
    the pixel count and one cannot simply take the first W*H bytes: that
    silently reads the red channel of the first W/3 rows and then walks off
    into the middle of the image, producing a scrambled, mostly-plausible
    picture. It looked like a rendering bug, and it was a stride bug.
    """
    with open(path, "rb") as f:
        data = f.read()
    i = data.index(b"255\n") + 4
    n = W * H
    if len(data) - i < n * 3:
        sys.exit(f"{path}: truncated, {len(data) - i} bytes for {n} RGB pixels")
    # All three channels carry the same 1-bit value, so take the red one.
    grey = data[i : i + n * 3 : 3]
    return Image.frombytes("L", (W, H), grey)


def panel_frame(im):
    """Identity: the dump is already the panel. Kept as a named step so the
    pipeline reads the same whichever backend produced the frames."""
    assert im.size == (W, H), f"frame is {im.size}, expected {(W, H)}"
    return im




def main():
    files = sorted(f for f in os.listdir(FRAMES) if f.endswith(".ppm"))
    if not files:
        sys.exit(f"no frames in {FRAMES}/ -- run `make sim-frames` first")

    srcs = [load(os.path.join(FRAMES, f)) for f in files]
    print(f"{len(srcs)} frames at {W}x{H}")

    gif = os.path.join("docs", "assets", "img", "drag.gif")
    os.makedirs(os.path.dirname(gif), exist_ok=True)
    srcs[0].save(
        gif,
        save_all=True,
        append_images=srcs[1:],
        duration=int(1000 / FPS),
        loop=0,
        optimize=True,
    )
    print(f"{gif}: {os.path.getsize(gif) / 1024:.0f} KB")

    png = os.path.join("docs", "assets", "img", "panel.png")
    # The last frame, not the middle one: mid-drag the window can cover most
    # of the desktop, and a still that shows nothing but a title bar is a poor
    # thing to put in a README.
    panel_frame(srcs[-1]).save(png, optimize=True)
    print(f"{png}: {os.path.getsize(png) / 1024:.0f} KB")


if __name__ == "__main__":
    main()
