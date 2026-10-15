#!/usr/bin/env python3
"""
PPM (P5) -> PNG, so the renderer's output can be committed as an image.

Pure standard library: PNG allows zlib "stored" (uncompressed) deflate blocks,
so no third-party encoder is needed and the output is byte-for-byte
reproducible.

    python3 tools/ppm2png.py build/render.ppm docs/assets/img/shot.png
"""

import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P5"):
        raise SystemExit(f"{path}: not a binary PPM (P5)")

    # Header: P5 <w> <h> <maxval>, whitespace separated, '#' comments allowed.
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, maxval = fields
    if maxval != 255:
        raise SystemExit(f"{path}: maxval {maxval} not supported")
    return w, h, data[i : i + w * h]


def chunk(tag, payload):
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def write_png(path, w, h, gray):
    raw = b"".join(b"\x00" + gray[y * w : (y + 1) * w] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, px = read_ppm(src)
    write_png(dst, w, h, px)
    print(f"{dst}: {w}x{h}, {len(px)} px")


if __name__ == "__main__":
    main()
