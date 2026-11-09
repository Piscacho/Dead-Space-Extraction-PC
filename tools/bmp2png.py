"""Convert a 32-bit top-down BMP (as written by the runtime) to PNG, standard library only.

Usage: python tools/bmp2png.py in.bmp out.png
"""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
off, = struct.unpack_from("<I", data, 10)
w, h = struct.unpack_from("<ii", data, 18)
top_down = h < 0
h = abs(h)
rows = []
for y in range(h):
    sy = y if top_down else h - 1 - y
    row = data[off + sy * w * 4: off + (sy + 1) * w * 4]
    rgb = bytearray()
    for x in range(w):
        b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
        rgb += bytes((r, g, b))
    rows.append(b"\0" + bytes(rgb))


def chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
print(f"{w}x{h} -> {dst}")
