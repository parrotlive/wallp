#!/usr/bin/env python3
"""Generates resources/wallp.ico: a rounded gradient tile with two waves.

Sizes 16-64 are stored as 32-bit BMP entries, 256 as PNG. Standard library only.
"""
import math
import os
import struct
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
C0 = (0x4B, 0x2B, 0xD6)   # violet
C1 = (0x19, 0xC3, 0xD8)   # cyan
WAVES = ((0.42, 0.070, 0.0), (0.63, 0.055, 1.9))  # (center y, amplitude, phase)


def sample(u, v):
    """Returns (r, g, b, a) in 0..1 for a point in the unit square."""
    margin, radius = 0.05, 0.22
    qx = max(abs(u - 0.5) - (0.5 - margin - radius), 0.0)
    qy = max(abs(v - 0.5) - (0.5 - margin - radius), 0.0)
    if math.hypot(qx, qy) > radius:
        return (0.0, 0.0, 0.0, 0.0)
    t = min(max((u + v) * 0.5, 0.0), 1.0)
    col = [(C0[i] + (C1[i] - C0[i]) * t) / 255.0 for i in range(3)]
    for cy, amp, phase in WAVES:
        wy = cy + amp * math.sin(u * 2.0 * math.pi * 1.15 + phase)
        a = min(max(1.0 - abs(v - wy) / 0.05, 0.0), 1.0) * 0.9
        col = [c + (1.0 - c) * a for c in col]
    return (col[0], col[1], col[2], 1.0)


def render(size):
    ss = 4 if size <= 64 else 2
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    r, g, b, a = sample((x + (sx + 0.5) / ss) / size, (y + (sy + 0.5) / ss) / size)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = ss * ss
            alpha = acc[3] / n
            if alpha > 0:
                rgb = [acc[i] / acc[3] for i in range(3)]
            else:
                rgb = [0.0, 0.0, 0.0]
            row.append(tuple(int(round(c * 255)) for c in rgb) + (int(round(alpha * 255)),))
        rows.append(row)
    return rows  # top-down RGBA


def bmp_entry(rows):
    size = len(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for row in reversed(rows):
        for r, g, b, a in row:
            xor += bytes((b, g, r, a))
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(stride)
        for x, (_, _, _, a) in enumerate(row):
            if a == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + bytes(xor) + bytes(mask)


def png_entry(rows):
    size = len(rows)
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw += bytes(px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def main():
    images = []
    for s in SIZES:
        rows = render(s)
        images.append((s, png_entry(rows) if s >= 256 else bmp_entry(rows)))
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for s, data in images:
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "wallp.ico")
    with open(path, "wb") as f:
        f.write(out)
    print(f"wrote {os.path.normpath(path)} ({len(out)} bytes)")


if __name__ == "__main__":
    main()
