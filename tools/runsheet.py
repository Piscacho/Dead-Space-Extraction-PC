"""Tile the interpolated runs of one game frame (logs/runs/fNNNNN_rRR.bmp) into one PNG.

Usage: python tools/runsheet.py logs/runs/f09000 out.png [scale]
Runs go left to right, top to bottom (4 per row), each scaled down by `scale` (default 3).
Standard library only.
"""
import glob
import struct
import sys
import zlib

prefix, out = sys.argv[1], sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3
files = sorted(glob.glob(prefix + "_r*.bmp"))
images = []
for path in files:
    d = open(path, "rb").read()
    off, = struct.unpack_from("<I", d, 10)
    w, h = struct.unpack_from("<ii", d, 18)
    h = abs(h)
    sw, sh = w // scale, h // scale
    rows = []
    for y in range(sh):
        base = off + (y * scale) * w * 4
        row = bytearray()
        for x in range(sw):
            p = base + x * scale * 4
            row += bytes((d[p + 2], d[p + 1], d[p]))
        rows.append(row)
    images.append((sw, sh, rows))
cols = 4
sw, sh = images[0][0], images[0][1]
nrows = (len(images) + cols - 1) // cols
W, H = cols * sw, nrows * sh
canvas = [bytearray(W * 3) for _ in range(H)]
for i, (w, h, rows) in enumerate(images):
    cx, cy = (i % cols) * sw, (i // cols) * sh
    for y in range(h):
        canvas[cy + y][cx * 3:(cx + w) * 3] = rows[y]
raw = b"".join(b"\0" + bytes(r) for r in canvas)


def chunk(t, b):
    return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print(f"{len(images)} runs -> {out} ({W}x{H})")
