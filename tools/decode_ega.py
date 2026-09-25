#!/usr/bin/env python3
"""Decode a 4-plane EGA SCREEN 9 (640x350x16) dump to PNG."""
import sys
from PIL import Image

W, H = 640, 350
BPR = W // 8                      # 80 bytes per row per plane
EGA = [(0,0,0),(0,0,170),(0,170,0),(0,170,170),(170,0,0),(170,0,170),
       (170,85,0),(170,170,170),(85,85,85),(85,85,255),(85,255,85),
       (85,255,255),(255,85,85),(255,85,255),(255,255,85),(255,255,255)]

src = sys.argv[1] if len(sys.argv) > 1 else "work/ega_planes.bin"
out = sys.argv[2] if len(sys.argv) > 2 else "work/screen.png"
start = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0   # page offset (CRTC start)
d = open(src, "rb").read()
PSZ = len(d) // 4                 # 28000 (one page) or 65536 (whole plane)
planes = [d[p*PSZ + start:(p+1)*PSZ] for p in range(4)]
img = Image.new("RGB", (W, H))
px = img.load()
nonzero = 0
for y in range(H):
    for x in range(W):
        byte = y * BPR + x // 8
        bit = 7 - (x & 7)
        c = 0
        for p in range(4):
            if byte < len(planes[p]) and (planes[p][byte] >> bit) & 1:
                c |= (1 << p)
        if c:
            nonzero += 1
        px[x, y] = EGA[c]
img.save(out)
print(f"wrote {out}  ({W}x{H}, {nonzero} non-black pixels of {W*H})")
