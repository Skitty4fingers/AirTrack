#!/usr/bin/env python3
"""Convert binary PPM files to PNG (no third-party dependencies)."""
import struct, sys, zlib

def convert(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    assert parts[0] == b'P6'
    width, height = map(int, parts[1].split())
    pixels = parts[3]
    rows = b''.join(b'\x00' + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body +
                struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))
    out = path[:-4] + '.png'
    with open(out, 'wb') as f:
        f.write(png)

for p in sys.argv[1:]:
    convert(p)
