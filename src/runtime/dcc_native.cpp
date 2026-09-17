// src/runtime/dcc_native.cpp — see dcc_native.h. Line-for-line port of
// tools/dcc_decode.py (stages 1 and 2, DC6 RLE encoding), with strict bounds:
// any read outside the input buffer is an error, never an access.
#include "runtime/dcc_native.h"
#include <cstring>
#include <algorithm>

namespace d2rt {
namespace {

const int WIDTH_TABLE[16] = {0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 26, 28, 30, 32};
const int POPCOUNT[16] = {0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4};

enum { E_OK=0, E_BOUNDS=1, E_DIM=2, E_SIZE=3, E_NFRAMES=4, E_CELLS=5, E_PALETTE=6, E_RLE=7, E_BOTTOMUP=8 };

struct Bits {
    const uint8_t* d; uint64_t p, end; bool bad;
    Bits(const uint8_t* dd, uint64_t bitpos, uint64_t endbit) : d(dd), p(bitpos), end(endbit), bad(false) {}
    uint32_t read(int n) {
        if (n <= 0) return 0;
        if (p + (uint64_t)n > end) { bad = true; p = end; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < n; i++) { v |= (uint32_t)((d[p >> 3] >> (p & 7)) & 1) << i; ++p; }
        return v;
    }
    int32_t read_signed(int n) {
        if (n == 0) return 0;
        uint32_t v = read(n);
        if (v & (1u << (n - 1))) v -= (1u << n);   // n < 32 per the table
        return (int32_t)v;
    }
    void align() { p = (p + 7) & ~(uint64_t)7; }
};

struct Frame {
    uint32_t var0, w, h, optbytes, coded, bottomup; int32_t xoff, yoff;
    int32_t xmin, xmax, ymin, ymax;
    int ncw, nch;
    std::vector<int> cw, ch;                  // cell widths / heights
    std::vector<uint8_t> pixels;              // h rows of w
};

struct PbEntry { uint8_t val[4]; int frame, cell; };

void split(int first, int total, std::vector<int>& out) {
    out.clear();
    if ((total - 1) <= first) { out.push_back(total); return; }
    int tmp = total - first - 1;
    int n = 2 + tmp / 4;
    if (tmp % 4 == 0) n -= 1;
    out.push_back(first);
    for (int i = 0; i < n - 2; i++) out.push_back(4);
    out.push_back(total - first - 4 * (n - 2));
}

void rle_row(const uint8_t* row, int n, std::vector<uint8_t>& out) {
    int end = n;
    while (end > 0 && row[end - 1] == 0) --end;
    int i = 0;
    while (i < end) {
        if (row[i] == 0) {
            int j = i; while (j < end && row[j] == 0) ++j;
            int run = j - i;
            while (run > 0) { int k = run < 0x7F ? run : 0x7F; out.push_back((uint8_t)(0x80 | k)); run -= k; }
            i = j;
        } else {
            int j = i; while (j < end && row[j] != 0) ++j;
            int k0 = i;
            while (k0 < j) { int k = (j - k0) < 0x7F ? (j - k0) : 0x7F; out.push_back((uint8_t)k);
                             out.insert(out.end(), row + k0, row + k0 + k); k0 += k; }
            i = j;
        }
    }
    out.push_back(0x80);
}

// One direction. `br` is the shared sequential reader (entry position = start
// of the direction); on exit it sits in the PCD stream after the last bit
// read (the game resumes from there, byte-aligned).
int decode_direction(Bits& br, const uint8_t* data, uint64_t endbit, uint32_t nframes,
                     DccResult& out) {
    const uint32_t outsize = br.read(32);
    const uint32_t flags = br.read(2);
    int var0bits = WIDTH_TABLE[br.read(4)], wbits = WIDTH_TABLE[br.read(4)], hbits = WIDTH_TABLE[br.read(4)];
    int xbits = WIDTH_TABLE[br.read(4)], ybits = WIDTH_TABLE[br.read(4)];
    int optbits = WIDTH_TABLE[br.read(4)], codedbits = WIDTH_TABLE[br.read(4)];
    if (br.bad) return E_BOUNDS;
    std::vector<Frame> frames(nframes);
    uint32_t anyopt = 0;
    for (uint32_t f = 0; f < nframes; f++) {
        Frame& fr = frames[f];
        fr.var0 = br.read(var0bits); fr.w = br.read(wbits); fr.h = br.read(hbits);
        fr.xoff = br.read_signed(xbits); fr.yoff = br.read_signed(ybits);
        fr.optbytes = br.read(optbits); fr.coded = br.read(codedbits); fr.bottomup = br.read(1);
        if (br.bad) return E_BOUNDS;
        if (fr.w > 4096 || fr.h > 4096) return E_DIM;
        if (fr.bottomup) return E_BOTTOMUP;          // never seen in D2; not ported (same as dcc_decode.py)
        anyopt += fr.optbytes;
        fr.xmin = fr.xoff; fr.xmax = fr.xoff + (int32_t)fr.w - 1;
        fr.ymin = fr.yoff - (int32_t)fr.h + 1; fr.ymax = fr.yoff;
    }
    if (anyopt) {
        br.align();
        for (uint32_t f = 0; f < nframes; f++) br.p += (uint64_t)frames[f].optbytes * 8;
        if (br.p > endbit) return E_BOUNDS;
    }
    uint32_t eq_size = (flags & 2) ? br.read(20) : 0;
    uint32_t pm_size = br.read(20);
    uint32_t et_size = 0, rp_size = 0;
    if (flags & 1) { et_size = br.read(20); rp_size = br.read(20); }
    uint8_t pal[256]; int npal = 0;
    for (int i = 0; i < 256; i++) if (br.read(1)) pal[npal++] = (uint8_t)i;
    if (br.bad) return E_BOUNDS;
    uint64_t eq_start = br.p, pm_start = eq_start + eq_size, et_start = pm_start + pm_size;
    uint64_t rp_start = et_start + et_size, pcd_start = rp_start + rp_size;
    if (pcd_start > endbit) return E_BOUNDS;
    // direction's bounding box
    if (nframes == 0) return E_NFRAMES;
    int32_t dxmin = frames[0].xmin, dxmax = frames[0].xmax, dymin = frames[0].ymin, dymax = frames[0].ymax;
    for (auto& fr : frames) { dxmin = std::min(dxmin, fr.xmin); dxmax = std::max(dxmax, fr.xmax);
                              dymin = std::min(dymin, fr.ymin); dymax = std::max(dymax, fr.ymax); }
    const int W = dxmax - dxmin + 1, H = dymax - dymin + 1;
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return E_DIM;
    const int ncw = 1 + (W - 1) / 4, nch = 1 + (H - 1) / 4;
    const int ncells = ncw * nch;
    // per-frame cells
    std::vector<int> cws, chs;
    for (auto& fr : frames) {
        int w0 = 4 - (((fr.xmin - dxmin) % 4 + 4) % 4);
        int h0 = 4 - (((fr.ymin - dymin) % 4 + 4) % 4);
        split(w0, (int)fr.w, fr.cw); split(h0, (int)fr.h, fr.ch);
        fr.ncw = (int)fr.cw.size(); fr.nch = (int)fr.ch.size();
    }
    Bits eq(data, eq_start, pm_start), pm(data, pm_start, et_start), et(data, et_start, rp_start),
         rp(data, rp_start, pcd_start), pcd(data, pcd_start, endbit);
    // ---- stage 1: PixelBuffer ----
    std::vector<int> cell_buffer(ncells, -1);   // index into pb, -1 = empty
    std::vector<PbEntry> pb; pb.reserve(4096);
    for (uint32_t fi = 0; fi < nframes; fi++) {
        Frame& fr = frames[fi];
        int ocx = (fr.xmin - dxmin) / 4, ocy = (fr.ymin - dymin) / 4;
        for (int cy = 0; cy < fr.nch; cy++) for (int cx = 0; cx < fr.ncw; cx++) {
            int cur = ocx + cx + (ocy + cy) * ncw;
            if (cur < 0 || cur >= ncells) return E_CELLS;
            uint32_t pixel_mask = 0x0F;
            if (cell_buffer[cur] >= 0) {
                uint32_t tmp = eq_size > 0 ? eq.read(1) : 0;
                if (tmp == 0) pixel_mask = pm.read(4); else continue;   // next_cell
            }
            uint8_t read_px[4] = {0, 0, 0, 0};
            uint32_t last = 0;
            int npix = POPCOUNT[pixel_mask];
            uint32_t enc = (npix && et_size > 0) ? et.read(1) : 0;
            int decoded = 0;
            for (int i = 0; i < npix; i++) {
                uint32_t v;
                if (enc) v = rp.read(8);
                else { v = last; uint32_t disp = pcd.read(4); v += disp;
                       while (disp == 15) { disp = pcd.read(4); v += disp; if (pcd.bad) break; } }
                if (v == last) { read_px[i] = 0; break; }
                read_px[i] = (uint8_t)v; last = v; ++decoded;
            }
            if (eq.bad || pm.bad || et.bad || rp.bad || pcd.bad) return E_BOUNDS;
            PbEntry e; e.frame = (int)fi; e.cell = cx + cy * fr.ncw;
            int ci = decoded - 1;
            const int old = cell_buffer[cur];
            for (int i = 0; i < 4; i++) {
                if (pixel_mask & (1u << i)) { if (ci >= 0) { e.val[i] = read_px[ci]; --ci; } else e.val[i] = 0; }
                else e.val[i] = old >= 0 ? pb[old].val[i] : 0;
            }
            cell_buffer[cur] = (int)pb.size();
            pb.push_back(e);
        }
    }
    for (auto& e : pb) for (int i = 0; i < 4; i++) e.val[i] = e.val[i] < npal ? pal[e.val[i]] : 0;
    // ---- stage 2: frames ----
    std::vector<uint8_t> buf((size_t)W * H, 0), img((size_t)W * H, 0);
    std::vector<int> last_w(ncells, -1), last_h(ncells, -1), last_x(ncells, 0), last_y(ncells, 0);
    size_t pbi = 0;
    for (uint32_t fi = 0; fi < nframes; fi++) {
        Frame& fr = frames[fi];
        std::fill(img.begin(), img.end(), 0);
        int c = 0;
        int oy = fr.ymin - dymin;
        for (int cy = 0; cy < fr.nch; cy++) {
            int ox = fr.xmin - dxmin;
            const int chh = fr.ch[cy];
            for (int cx = 0; cx < fr.ncw; cx++, c++) {
                const int cww = fr.cw[cx];
                const int x0 = ox, y0 = oy;
                int cur = (x0 / 4) + (y0 / 4) * ncw;
                if (cur < 0 || cur >= ncells || x0 + cww > W || y0 + chh > H) return E_CELLS;
                const PbEntry* e = pbi < pb.size() ? &pb[pbi] : nullptr;
                if (!e || e->frame != (int)fi || e->cell != c) {
                    if (cww != last_w[cur] || chh != last_h[cur]) {
                        for (int y = 0; y < chh; y++) std::memset(&buf[(size_t)(y0 + y) * W + x0], 0, (size_t)cww);
                    } else {
                        int lx = last_x[cur], ly = last_y[cur];
                        if (lx != x0 || ly != y0)
                            for (int y = 0; y < chh; y++)
                                std::memmove(&buf[(size_t)(y0 + y) * W + x0], &buf[(size_t)(ly + y) * W + lx], (size_t)cww);
                        for (int y = 0; y < chh; y++) std::memcpy(&img[(size_t)(y0 + y) * W + x0], &buf[(size_t)(y0 + y) * W + x0], (size_t)cww);
                    }
                } else {
                    const uint8_t* v = e->val;
                    if (v[0] == v[1]) {
                        for (int y = 0; y < chh; y++) std::memset(&buf[(size_t)(y0 + y) * W + x0], v[0], (size_t)cww);
                    } else {
                        int nb = (v[1] == v[2]) ? 1 : 2;
                        for (int y = 0; y < chh; y++) {
                            uint8_t* o = &buf[(size_t)(y0 + y) * W + x0];
                            for (int x = 0; x < cww; x++) o[x] = v[pcd.read(nb)];
                        }
                        if (pcd.bad) return E_BOUNDS;
                    }
                    for (int y = 0; y < chh; y++) std::memcpy(&img[(size_t)(y0 + y) * W + x0], &buf[(size_t)(y0 + y) * W + x0], (size_t)cww);
                    ++pbi;
                }
                last_w[cur] = cww; last_h[cur] = chh; last_x[cur] = x0; last_y[cur] = y0;
                ox += cww;
            }
            oy += chh;
        }
        // crop the frame into its own box
        const int fx = fr.xmin - dxmin, fy = fr.ymin - dymin;
        fr.pixels.resize((size_t)fr.w * fr.h);
        for (uint32_t y = 0; y < fr.h; y++)
            std::memcpy(&fr.pixels[(size_t)y * fr.w], &img[(size_t)(fy + (int)y) * W + fx], fr.w);
        out.cells += (uint32_t)c;
    }
    out.frames += nframes;
    // ---- cel block chain (DC6 in memory) ----
    const size_t base = out.chain.size();
    std::vector<uint8_t> rle;
    for (auto& fr : frames) {
        rle.clear();
        for (int y = (int)fr.h - 1; y >= 0; --y) rle_row(&fr.pixels[(size_t)y * fr.w], (int)fr.w, rle);
        if (rle.size() != fr.coded) return E_RLE;      // DCC's CodedBytes = DC6 length: the internal check
        uint32_t hdr[8] = { fr.var0, fr.w, fr.h, (uint32_t)fr.xoff, (uint32_t)fr.yoff, 0, 0, fr.coded };
        const uint8_t* hp = (const uint8_t*)hdr;
        out.blocks.push_back({(uint32_t)out.chain.size(), 32u + fr.coded});
        out.chain.insert(out.chain.end(), hp, hp + 32);
        out.chain.insert(out.chain.end(), rle.begin(), rle.end());
        out.chain.push_back(0xEE); out.chain.push_back(0xEE); out.chain.push_back(0xEE);
    }
    if (out.chain.size() - base != outsize) return E_SIZE;
    out.dir_off.push_back((uint32_t)base);
    out.dir_size.push_back(outsize);
    // the game resumes from the PCD stream, byte-aligned
    br.p = pcd.p; br.align();
    return E_OK;
}

} // namespace

int dcc_decode_native(const uint8_t* data, uint32_t len, uint32_t nframes, uint32_t ndirs, DccResult& out) {
    out.chain.clear(); out.dir_off.clear(); out.dir_size.clear(); out.blocks.clear(); out.frames = out.cells = 0;
    if (!data || !len || !nframes || nframes > 4096 || !ndirs || ndirs > 64) return E_NFRAMES;
    const uint64_t endbit = (uint64_t)len * 8;
    Bits br(data, 0, endbit);
    for (uint32_t d = 0; d < ndirs; d++) {
        int r = decode_direction(br, data, endbit, nframes, out);
        if (r) return r;
    }
    return E_OK;
}

const char* dcc_error_name(int code) {
    switch (code) {
        case E_OK: return "ok"; case E_BOUNDS: return "hors-bornes"; case E_DIM: return "dimensions";
        case E_SIZE: return "somme!=OutSizeCoded"; case E_NFRAMES: return "nframes/ndirs";
        case E_CELLS: return "cellules"; case E_PALETTE: return "palette"; case E_RLE: return "rle!=CodedBytes";
        case E_BOTTOMUP: return "bottom-up"; default: return "?";
    }
}

} // namespace d2rt
