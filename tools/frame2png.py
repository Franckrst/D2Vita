#!/usr/bin/env python3
# tools/frame2png.py — convert a dumped D2 frame (raw 8bpp indexed or 32bpp)
# to PNG. 8bpp uses the palette dumped alongside (d2_palette.raw, 256*RGBQUAD)
# or grayscale fallback.
#   frame2png.py /tmp/d2_frame_1_640x480x8.raw [out.png]
import sys, re, struct

path = sys.argv[1]
m = re.search(r'(\d+)x(\d+)x(\d+)\.raw$', path)
w, h, bpp = int(m.group(1)), int(m.group(2)), int(m.group(3))
data = open(path, 'rb').read()

def png_write(out, w, h, rgb_rows):
    import zlib
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = b''.join(b'\x00' + r for r in rgb_rows)
    open(out, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw))
        + chunk(b'IEND', b''))

rows = []
if bpp == 8:
    pal = None
    # prefer the palette dumped WITH this frame (menu vs act palettes differ),
    # then the latest-palette file, then grayscale
    for palpath in (path.replace('.raw', '.pal'), '/tmp/d2_palette.raw'):
        try:
            pr = open(palpath, 'rb').read()
            pal = [(pr[i*4+2], pr[i*4+1], pr[i*4+0]) for i in range(256)]  # RGBQUAD = BGRX
            break
        except OSError:
            continue
    if pal is None:
        pal = [(i, i, i) for i in range(256)]                              # grayscale
    for y in range(h):
        row = bytearray()
        for x in range(w):
            p = data[y*w+x] if y*w+x < len(data) else 0
            r, g, b = pal[p]
            row += bytes((r, g, b))
        rows.append(bytes(row))
else:  # 32bpp BGRX
    for y in range(h):
        row = bytearray()
        for x in range(w):
            o = (y*w+x)*4
            row += bytes((data[o+2], data[o+1], data[o+0])) if o+3 < len(data) else b'\x00\x00\x00'
        rows.append(bytes(row))

out = sys.argv[2] if len(sys.argv) > 2 else path.replace('.raw', '.png')
png_write(out, w, h, rows)
print('wrote', out)
