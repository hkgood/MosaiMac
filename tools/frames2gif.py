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

FRAMES = "build/frames"
W, H = 480, 320
BEZEL = 480
FPS = 24

# Bayer 2x2 threshold matrix, normalised to 0..1.
BAYER = [[0 / 4, 3 / 4], [2 / 4, 1 / 4]]


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    i = data.index(b"255\n") + 4
    return Image.frombytes("L", (W, H), data[i : i + W * H])


def panel_frame(im):
    """
    Place the 480x320 screen inside the 480x480 panel the S31 actually drives.

    Deliberately no resampling and no dithering. Downsampling 1-bit content
    with a filter or an ordered dither turns legible 5x7 type into mush --
    there is no grey in the source to dither between, and a resample that
    averages it just deletes the glyphs. The panel runs 480x320 content on a
    480x480 surface, so the mapping is 1:1 with black bezel top and bottom,
    which is exactly what the hardware does.
    """
    assert im.size == (W, H), f"frame is {im.size}, expected {(W, H)}"
    out = Image.new("L", (BEZEL, BEZEL), 0)  # black bezel
    yoff = (BEZEL - H) // 2
    out.paste(im, (0, yoff))
    return out


def main():
    files = sorted(f for f in os.listdir(FRAMES) if f.endswith(".ppm"))
    if not files:
        sys.exit(f"no frames in {FRAMES}/ -- run `make build/replay` first")

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
    panel_frame(srcs[len(srcs) // 2]).save(png, optimize=True)
    print(f"{png}: {os.path.getsize(png) / 1024:.0f} KB")


if __name__ == "__main__":
    main()
