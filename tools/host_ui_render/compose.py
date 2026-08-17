#!/usr/bin/env python3
"""Tile rendered PPM screens into one PNG (2x) for review: compose.py out.png a.ppm b.ppm ..."""
import struct, sys, zlib
def readppm(p):
    d=open(p,'rb').read(); parts=d.split(b'\n',3); w,h=map(int,parts[1].split()); return w,h,parts[3]
out=sys.argv[1]; imgs=[readppm(p) for p in sys.argv[2:]]
W,H=172,320; S=2; gap=8
ow=(W*S+gap)*len(imgs)+gap; oh=H*S+2*gap
rows=[]
for y in range(oh):
    row=bytearray(b'\x00')
    for x in range(ow):
        r=g=b=0x30
        i=(x-gap)//(W*S+gap); xi=(x-gap)%(W*S+gap); yi=y-gap
        if 0<=i<len(imgs) and xi<W*S and 0<=yi<H*S:
            px=imgs[i][2]; o=((yi//S)*W+xi//S)*3; r,g,b=px[o],px[o+1],px[o+2]
        row+=bytes((r,g,b))
    rows.append(bytes(row))
def chunk(k,b): return struct.pack('>I',len(b))+k+b+struct.pack('>I',zlib.crc32(k+b)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',ow,oh,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),6))+chunk(b'IEND',b'')
open(out,'wb').write(png)
