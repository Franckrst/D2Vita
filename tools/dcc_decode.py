#!/usr/bin/env python3
# tools/dcc_decode.py — standalone DCC decoder -> chain of "DC6 in memory" cel
# blocks, exactly as Game.exe 1.14d builds them (reference oracle for Halt 1420).
#
# The goal is NOT the image: it's the CORRECT REFERENCE for the game decoder's
# output, to diff field by field against save2/oracle_<sprite>.out (dynarec
# output on console, frozen at the faulting block) — see tools/oracle_diff.py.
#
# Output format (= what 0x5fec90 validates):
#   block = 0x20 header + RLE data [+0x1c] bytes + 3 bytes 0xEE
#   [+0x00] flip (0)     [+0x04] width        [+0x08] height
#   [+0x0c] xoff         [+0x10] yoff         [+0x14] unknown (0)
#   [+0x18] next block   [+0x1c] RLE length
#   stride = 0x23 + [+0x14] + [+0x1c]
# This is exactly a DC6 frame (32-byte header + RLE + 0xEE 0xEE 0xEE): the DCC
# itself carries, per direction, "OutSizeCoded" = sum of (0x23 + length) over
# its frames. This field is the decoder's INTERNAL ORACLE: if our RLE encoder
# doesn't reproduce the sum bit for bit, the format is wrong (--check).
#
# RLE encoding (DC6) of one row, rows from BOTTOM to TOP:
#   0x80        end of row (remaining pixels are transparent)
#   0x80|n      n transparent pixels (1..0x7f)
#   n, p[n]     n literal pixels (1..0x7f)
#   Rule (validated with --check across the 8/16 directions of several DCCs):
#   trailing transparent pixels at END of row are not emitted; a fully
#   "empty" row is just 0x80.
#
# DCC decoding: faithful port of Paul Siramy's algorithm (dcc.c) as adopted by
# OpenDiablo2 (d2dcc): header, 4x4 direction cells (last one 1..4), frame
# cells (first 4-r, last 2..5), PixelBuffer (stage 1, EqualCells / PixelMask /
# EncodingType / RawPixel / PixelCodeDisplacement streams), then frame
# reconstruction (stage 2). DCC "bottom-up" frames (FrameBottomUp bit) are not
# handled (never seen in D2 sprites).
#
# Usage:
#   dcc_decode.py <file.dcc>                   # summary + OutSizeCoded check
#   dcc_decode.py <file.dcc> --dir 4 --out ref.bin   # chain for direction 4
#   dcc_decode.py <file.dcc> --dir 4 --list    # list blocks (i, L, H, x, y, len, stride)
#   dcc_decode.py <file.dcc> --all --out ref.bin     # all directions concatenated
#   dcc_decode.py <file.dcc> --dir 4 --pgm d.pgm     # frame sheet (visual check)
import struct, sys, argparse

WIDTH_TABLE = [0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 26, 28, 30, 32]
POPCOUNT = [bin(i).count('1') for i in range(16)]


class Bits:
    """LSB-first bit reader (the one DCC uses)."""
    __slots__ = ('d', 'p', 'end')

    def __init__(self, data, bitpos, nbits=None):
        self.d = data
        self.p = bitpos
        self.end = bitpos + nbits if nbits is not None else len(data) * 8

    def read(self, n):
        v = 0
        p = self.p
        d = self.d
        for i in range(n):
            v |= ((d[p >> 3] >> (p & 7)) & 1) << i
            p += 1
        self.p = p
        return v

    def read_signed(self, n):
        if n == 0:
            return 0
        v = self.read(n)
        if v & (1 << (n - 1)):
            v -= 1 << n
        return v

    def align(self):
        self.p = (self.p + 7) & ~7


class Frame:
    __slots__ = ('var0', 'w', 'h', 'xoff', 'yoff', 'optbytes', 'coded', 'bottomup',
                 'xmin', 'xmax', 'ymin', 'ymax', 'cells', 'ncw', 'nch', 'pixels', 'optdata')


class Direction:
    __slots__ = ('index', 'offset', 'outsize', 'flags', 'frames', 'xmin', 'xmax', 'ymin', 'ymax',
                 'w', 'h', 'ncw', 'nch', 'cell_w', 'cell_h', 'palette', 'sizes', 'nframes')


class DCC:
    def __init__(self, data, name='?'):
        self.data = data
        self.name = name
        if len(data) < 15 or data[0] != 0x74:
            raise ValueError('%s: pas un DCC (signature %02x)' % (name, data[0] if data else -1))
        self.version = data[1]
        self.ndirs = data[2]
        self.nframes, self.tag, self.final_dc6_size = struct.unpack_from('<III', data, 3)
        self.dir_offsets = list(struct.unpack_from('<%dI' % self.ndirs, data, 15))

    # ---- reading one direction ------------------------------------------------
    def read_direction(self, di):
        data = self.data
        d = Direction()
        d.index = di
        d.offset = self.dir_offsets[di]
        d.nframes = self.nframes
        br = Bits(data, d.offset * 8)
        d.outsize = br.read(32)
        d.flags = br.read(2)
        var0bits = WIDTH_TABLE[br.read(4)]
        wbits = WIDTH_TABLE[br.read(4)]
        hbits = WIDTH_TABLE[br.read(4)]
        xbits = WIDTH_TABLE[br.read(4)]
        ybits = WIDTH_TABLE[br.read(4)]
        optbits = WIDTH_TABLE[br.read(4)]
        codedbits = WIDTH_TABLE[br.read(4)]
        d.frames = []
        anyopt = 0
        for f in range(self.nframes):
            fr = Frame()
            fr.var0 = br.read(var0bits)
            fr.w = br.read(wbits)
            fr.h = br.read(hbits)
            fr.xoff = br.read_signed(xbits)
            fr.yoff = br.read_signed(ybits)
            fr.optbytes = br.read(optbits)
            fr.coded = br.read(codedbits)
            fr.bottomup = br.read(1)
            fr.optdata = b''
            anyopt += fr.optbytes
            fr.xmin = fr.xoff
            fr.xmax = fr.xoff + fr.w - 1
            if fr.bottomup:
                fr.ymin = fr.yoff
                fr.ymax = fr.yoff + fr.h - 1
            else:
                fr.ymin = fr.yoff - fr.h + 1
                fr.ymax = fr.yoff
            d.frames.append(fr)
        if anyopt:
            br.align()
            for fr in d.frames:
                if fr.optbytes:
                    o = br.p >> 3
                    fr.optdata = bytes(data[o:o + fr.optbytes])
                    br.p += fr.optbytes * 8
        eq_size = br.read(20) if (d.flags & 2) else 0
        pm_size = br.read(20)
        et_size = rp_size = 0
        if d.flags & 1:
            et_size = br.read(20)
            rp_size = br.read(20)
        d.palette = []
        for i in range(256):
            if br.read(1):
                d.palette.append(i)
        eq_start = br.p
        pm_start = eq_start + eq_size
        et_start = pm_start + pm_size
        rp_start = et_start + et_size
        pcd_start = rp_start + rp_size
        d.sizes = (eq_size, pm_size, et_size, rp_size)
        # direction bounding box
        d.xmin = min(fr.xmin for fr in d.frames)
        d.xmax = max(fr.xmax for fr in d.frames)
        d.ymin = min(fr.ymin for fr in d.frames)
        d.ymax = max(fr.ymax for fr in d.frames)
        d.w = d.xmax - d.xmin + 1
        d.h = d.ymax - d.ymin + 1
        # direction cells: 4, ..., 4, last one 1..4
        d.ncw = 1 + (d.w - 1) // 4
        d.nch = 1 + (d.h - 1) // 4
        d.cell_w = [4] * (d.ncw - 1) + [d.w - 4 * (d.ncw - 1)] if d.ncw > 1 else [d.w]
        d.cell_h = [4] * (d.nch - 1) + [d.h - 4 * (d.nch - 1)] if d.nch > 1 else [d.h]
        # frame cells
        for fr in d.frames:
            self._frame_cells(d, fr)
        streams = (Bits(data, eq_start, eq_size), Bits(data, pm_start, pm_size),
                   Bits(data, et_start, et_size), Bits(data, rp_start, rp_size),
                   Bits(data, pcd_start))
        self._decode(d, streams)
        return d

    @staticmethod
    def _split(first, total):
        """Splits one frame dimension into cells aligned to the 4-pixel grid."""
        if (total - 1) <= first:
            return [total]
        tmp = total - first - 1
        n = 2 + tmp // 4
        if tmp % 4 == 0:
            n -= 1
        return [first] + [4] * (n - 2) + [total - first - 4 * (n - 2)]

    def _frame_cells(self, d, fr):
        w0 = 4 - ((fr.xmin - d.xmin) % 4)
        h0 = 4 - ((fr.ymin - d.ymin) % 4)
        cws = self._split(w0, fr.w)
        chs = self._split(h0, fr.h)
        fr.ncw = len(cws)
        fr.nch = len(chs)
        fr.cells = []           # (x0, y0, w, h) relative to the direction bounding box
        oy = fr.ymin - d.ymin
        for ch in chs:
            ox = fr.xmin - d.xmin
            for cw in cws:
                fr.cells.append((ox, oy, cw, ch))
                ox += cw
            oy += ch

    def _decode(self, d, streams):
        eq, pm, et, rp, pcd = streams
        eq_size, pm_size, et_size, rp_size = d.sizes
        ncells = d.ncw * d.nch
        cell_buffer = [None] * ncells
        pb = []                 # entries: [val0..3], frame, frame_cell_index
        # ---- stage 1: PixelBuffer ----
        for fi, fr in enumerate(d.frames):
            ocx = (fr.xmin - d.xmin) // 4
            ocy = (fr.ymin - d.ymin) // 4
            for cy in range(fr.nch):
                for cx in range(fr.ncw):
                    cur = ocx + cx + (ocy + cy) * d.ncw
                    next_cell = False
                    if cell_buffer[cur] is not None:
                        tmp = eq.read(1) if eq_size > 0 else 0
                        if tmp == 0:
                            pixel_mask = pm.read(4)
                        else:
                            next_cell = True
                    else:
                        pixel_mask = 0x0F
                    if next_cell:
                        continue
                    read_px = [0, 0, 0, 0]
                    last = 0
                    npix = POPCOUNT[pixel_mask]
                    enc = et.read(1) if (npix and et_size > 0) else 0
                    decoded = 0
                    for i in range(npix):
                        if enc:
                            v = rp.read(8)
                        else:
                            v = last
                            disp = pcd.read(4)
                            v += disp
                            while disp == 15:
                                disp = pcd.read(4)
                                v += disp
                        if v == last:
                            read_px[i] = 0
                            break
                        read_px[i] = v
                        last = v
                        decoded += 1
                    old = cell_buffer[cur]
                    new = [0, 0, 0, 0]
                    ci = decoded - 1
                    for i in range(4):
                        if pixel_mask & (1 << i):
                            if ci >= 0:
                                new[i] = read_px[ci]
                                ci -= 1
                            else:
                                new[i] = 0
                        else:
                            new[i] = old[0][i]
                    entry = (new, fi, cx + cy * fr.ncw)
                    cell_buffer[cur] = entry
                    pb.append(entry)
        pal = d.palette
        for e in pb:
            v = e[0]
            for i in range(4):
                v[i] = pal[v[i]] if v[i] < len(pal) else 0
        # ---- stage 2: frames ----
        W = d.w
        buf = bytearray(W * d.h)
        last_w = [-1] * ncells
        last_h = [-1] * ncells
        last_x = [0] * ncells
        last_y = [0] * ncells
        pbi = 0
        for fi, fr in enumerate(d.frames):
            img = bytearray(W * d.h)
            for c, (x0, y0, cw, ch) in enumerate(fr.cells):
                cur = (x0 // 4) + (y0 // 4) * d.ncw
                e = pb[pbi] if pbi < len(pb) else None
                if e is None or e[1] != fi or e[2] != c:
                    if cw != last_w[cur] or ch != last_h[cur]:
                        for y in range(ch):
                            o = (y0 + y) * W + x0
                            buf[o:o + cw] = bytes(cw)
                    else:
                        lx, ly = last_x[cur], last_y[cur]
                        if (lx, ly) != (x0, y0):
                            for y in range(ch):
                                buf[(y0 + y) * W + x0:(y0 + y) * W + x0 + cw] = buf[(ly + y) * W + lx:(ly + y) * W + lx + cw]
                        for y in range(ch):
                            o = (y0 + y) * W + x0
                            img[o:o + cw] = buf[o:o + cw]
                else:
                    vals = e[0]
                    if vals[0] == vals[1]:
                        row = bytes([vals[0]]) * cw
                        for y in range(ch):
                            o = (y0 + y) * W + x0
                            buf[o:o + cw] = row
                    else:
                        nb = 1 if vals[1] == vals[2] else 2
                        for y in range(ch):
                            o = (y0 + y) * W + x0
                            for x in range(cw):
                                buf[o + x] = vals[pcd.read(nb)]
                    for y in range(ch):
                        o = (y0 + y) * W + x0
                        img[o:o + cw] = buf[o:o + cw]
                    pbi += 1
                last_w[cur] = cw
                last_h[cur] = ch
                last_x[cur] = x0
                last_y[cur] = y0
            # splits the frame (rows top to bottom, within its own box)
            fx = fr.xmin - d.xmin
            fy = fr.ymin - d.ymin
            fr.pixels = [bytes(img[(fy + y) * W + fx:(fy + y) * W + fx + fr.w]) for y in range(fr.h)]


# ---- DC6 encoding --------------------------------------------------------------
def dc6_encode_row(row, keep_trailing=False):
    """One pixel row -> DC6 RLE (0x80 = end of row)."""
    out = bytearray()
    n = len(row)
    end = n
    if not keep_trailing:
        while end > 0 and row[end - 1] == 0:
            end -= 1
    i = 0
    while i < end:
        if row[i] == 0:
            j = i
            while j < end and row[j] == 0:
                j += 1
            run = j - i
            while run > 0:
                k = min(run, 0x7F)
                out.append(0x80 | k)
                run -= k
            i = j
        else:
            j = i
            while j < end and row[j] != 0:
                j += 1
            k0 = i
            while k0 < j:
                k = min(j - k0, 0x7F)
                out.append(k)
                out += row[k0:k0 + k]
                k0 += k
            i = j
    out.append(0x80)
    return bytes(out)


def dc6_frame_data(fr, keep_trailing=False):
    """Frame RLE data, rows bottom to top (DC6)."""
    out = bytearray()
    for y in range(fr.h - 1, -1, -1):
        out += dc6_encode_row(fr.pixels[y], keep_trailing)
    return bytes(out)


def build_chain(d, base_offset=0, next_field='offset', keep_trailing=False):
    """Block chain for direction d, exactly as the game lays it out in memory.
    next_field: 'offset' -> [+0x18] = offset of the next block from the start
    of the chain (as in a DC6 file); 'zero' -> 0. This field is REWRITTEN by
    0x5fec90 (pointer to a 0x2c-byte record): it can never be compared
    strictly."""
    blocks = []
    off = base_offset
    for fr in d.frames:
        rle = dc6_frame_data(fr, keep_trailing)
        nxt = off + 0x23 + len(rle) if next_field == 'offset' else 0
        hdr = struct.pack('<IIIiiIII', 0, fr.w, fr.h, fr.xoff, fr.yoff, 0, nxt, len(rle))
        blk = hdr + rle + b'\xee\xee\xee'
        blocks.append(blk)
        off += len(blk)
    return blocks


def chain_total(blocks):
    return sum(len(b) for b in blocks)


def check_direction(d, keep_trailing=False):
    blocks = build_chain(d, keep_trailing=keep_trailing)
    tot = chain_total(blocks)
    return tot, d.outsize, tot == d.outsize


def write_pgm(d, path):
    """Frame sheet (visual check, raw palette indices)."""
    n = len(d.frames)
    W = d.w * n
    H = d.h
    img = bytearray(W * H)
    for fi, fr in enumerate(d.frames):
        fx = fr.xmin - d.xmin
        fy = fr.ymin - d.ymin
        for y in range(fr.h):
            row = fr.pixels[y]
            o = (fy + y) * W + fi * d.w + fx
            img[o:o + fr.w] = row
    with open(path, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (W, H))
        f.write(img)


def main(argv=None):
    ap = argparse.ArgumentParser(description='DCC -> chaine de blocs de cel (reference oracle 1420)')
    ap.add_argument('dcc')
    ap.add_argument('--dir', type=int, default=None, help='direction a produire (defaut : verification de toutes)')
    ap.add_argument('--all', action='store_true', help='toutes les directions, concatenees dans --out')
    ap.add_argument('--out', help='fichier binaire de la chaine de blocs')
    ap.add_argument('--list', action='store_true', help='lister les blocs')
    ap.add_argument('--pgm', help='planche PGM de la direction (--dir)')
    ap.add_argument('--keep-trailing', action='store_true', help='variante RLE : emettre les transparents de fin de ligne')
    a = ap.parse_args(argv)
    data = open(a.dcc, 'rb').read()
    dcc = DCC(data, a.dcc)
    print('%s : %d o, version %d, %d directions x %d cadres, final_dc6_size=%d' % (
        a.dcc, len(data), dcc.version, dcc.ndirs, dcc.nframes, dcc.final_dc6_size))
    dirs = range(dcc.ndirs) if (a.dir is None or a.all) else [a.dir]
    allblocks = []
    ok_all = True
    tot_all = 0
    for di in dirs:
        d = dcc.read_direction(di)
        tot, exp, ok = check_direction(d, a.keep_trailing)
        ok_all &= ok
        tot_all += tot
        print('  dir %2d @%-6d boite %dx%d (x %d..%d, y %d..%d) cellules %dx%d flux eq/pm/et/rp=%s  chaine=%d OutSizeCoded=%d %s' % (
            di, d.offset, d.w, d.h, d.xmin, d.xmax, d.ymin, d.ymax, d.ncw, d.nch, d.sizes, tot, exp,
            'OK' if ok else 'ECART %+d' % (tot - exp)))
        blocks = build_chain(d, base_offset=sum(len(b) for b in allblocks), keep_trailing=a.keep_trailing)
        if a.list:
            off = 0
            for i, (fr, b) in enumerate(zip(d.frames, blocks)):
                ln = len(b) - 0x23
                print('    i=%-3d @+0x%05x L=%-3d H=%-3d x=%-4d y=%-4d len=%-5d pas=%d' % (
                    i, off, fr.w, fr.h, fr.xoff, fr.yoff, ln, 0x23 + ln))
                off += len(b)
        if a.pgm and a.dir is not None:
            write_pgm(d, a.pgm)
            print('  planche -> %s' % a.pgm)
        allblocks += blocks
    if a.dir is None and not a.all:
        # final_dc6_size = 24 (DC6 header) + 4*dirs*frames (table) + sum of directions? (informational)
        print('  somme des chaines = %d ; final_dc6_size = %d (delta %+d)' % (tot_all, dcc.final_dc6_size, tot_all - dcc.final_dc6_size))
    print('VERIFICATION OutSizeCoded : %s' % ('PASS (toutes les directions)' if ok_all else 'FAIL'))
    if a.out:
        with open(a.out, 'wb') as f:
            for b in allblocks:
                f.write(b)
        print('chaine -> %s (%d o, %d blocs)' % (a.out, chain_total(allblocks), len(allblocks)))
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main())
