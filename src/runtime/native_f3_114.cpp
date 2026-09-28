// native_f3_114.cpp — NATIVE F3: Game+0x10cf70 (VA 0x50cf70), the subdivided
// floor-tile draw of the Perspective renderer (1.14d monolith).
//
// WHY THIS FUNCTION. Timed on console (Act V patrol, D2_PHASEPROF_X, one
// probe per pass, 26/09/2026): 74 calls/frame, ~11.4 ms/frame inclusive,
// ~150 us per call. Per cell it makes ~30 small calls (15 projections
// 0x50dd60, 15 colour LUTs 0x50dc30) in the middle of x87 code, and each call
// ends a translated block and purges the x87 cache. Porting those leaves one
// by one was measured NEGATIVE (D2_INTRIN, +4.3 ms/frame): the translated
// integer bodies are already near-native, the win is in removing the calls.
// So the port absorbs the whole CALLER, like the native cell loop did.
//
// WHAT F3 DOES (disassembly 0x50cf70..0x50d404, ret 0x10). Inputs: EAX = mode
// (0/1/2, horizontal half-screen shift), stack: ctx, X, Y, colour table.
//   frame: rect L,T,R,B and shift from 0x50c690 (screen size, mode);
//   for each of ctx->count cells (20-byte records at ctx->cells):
//     corners: 4 projections -> vertices v0, v12, v14, v2 (float x,y);
//     visible if ANY corner has L <= x < R and T <= y < B;
//     if visible: centre, three colours (A, per-channel average, B),
//       0x50fbd0(cell) = texture bind (GAME code: runs as guest),
//       grid: 5 rows x 3 vertices, 11 more projections, x += shift,
//       colour LUT per vertex; two grDrawVertexArray(4, 10, ptrs).
//   returns EAX = 1 if a cell was drawn, else 0.
//
// HOW. An `alternate` hook on the entry. The engine cannot nest the emulator
// inside a shim, so the texture call is a CONTINUATION: the shim runs cells
// natively until one needs 0x50fbd0, points the return address at a
// continuation trap, lets the guest run 0x50fbd0, and resumes the cell when
// it returns. The two draws are written into the Glide ring by the host with
// gr_draw_native — the record the DLL's own grDrawVertexArray writes, proved
// byte-identical (tools/oracle_natdraw.sh) — with the stride the DLL
// publishes in the ring header. Anything unexpected (cell or tile type out
// of the asserted range, Glide table not pointing at our DLL, ring not armed)
// runs the ORIGINAL function: the hook replays `push ebp` and resumes at +1.
//
// EXACTNESS. Integer arithmetic is replicated operation by operation
// (arithmetic shifts spelled out). x87: every float written is either
// (float)int (fild/fstp) or ONE float addition (fld dword; fadd dword; fstp),
// which round once whether the dynarec keeps the slot in F32 or F64, so C
// float arithmetic reproduces it bit for bit. The visibility tests compare
// exact values (ints and floats of ints) and are done in double.
//
// ORACLE: D2_F3NATIF=2 computes every cell natively WITHOUT writing
// anything, lets the original F3 run behind a return trap, then compares the
// draw records the guest appended to the ring (vertex bytes included) and the
// return value. D2_F3NATIF=1 serves natively. Default 0 (not armed).
#include "glide_ring/gx_host.h"
#include "glide_ring/glide_ring.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
using namespace d2rt;

extern "C" uintptr_t dyn86_membase;   // guest VA -> host: membase + va
extern "C" void wx86_intrinline_counts(int* on, unsigned long long* stubs, unsigned long long* skip);   // engine, cpu_box86.cpp
// Block-sampler flavour (D2VPK_BLKSAMP): the sampled word takes a PHASE value
// (0xFFFE0000 | phase) inside this port, so the sampler splits the shim's
// time by phase without a clock read (rt_now_us costs ~1.3 us on Vita, and
// D2_F3PROF's per-cell reads inflated what they measured). A plain store,
// dead weight outside the flavour (dyn86_blksamp_built is 0).
extern "C" { extern volatile uint32_t dyn86_blksamp_ip; extern const int dyn86_blksamp_built; }
static inline void PH(uint32_t ph){ if(dyn86_blksamp_built) dyn86_blksamp_ip = 0xFFFE0000u | ph; }
enum { PH_ACCEPT=1, PH_FRONT=2, PH_CORNERS=3, PH_MID=4, PH_GRID=5, PH_WVTX=6, PH_DRAW=7, PH_STEP=8 };

namespace {

// ---- addresses (VA at image base 0x400000; relocated by g_d2base) --------
constexpr uint32_t RVA_F3      = 0x10cf70;   // the function
constexpr uint32_t RVA_TEXCELL = 0x10fbd0;   // texture bind per cell (fastcall ecx)
constexpr uint32_t RVA_GLDRAWVA= 0x47eb18;   // [ ] = grDrawVertexArray (Glide table)
constexpr uint32_t RVA_VTX     = 0x480458;   // 15 vertices x 0x1c
constexpr uint32_t RVA_STRIP0  = 0x32f350;   // 10 vertex pointers
constexpr uint32_t RVA_STRIP1  = 0x32f378;   // 10 vertex pointers
constexpr uint32_t RVA_CORNERS = 0x32f3a0;   // corner offset table (12-byte stride)
constexpr uint32_t RVA_W       = 0x3c9138;   // render width
constexpr uint32_t RVA_H       = 0x3c913c;   // render height
constexpr uint32_t RVA_CAMX    = 0x480b60;
constexpr uint32_t RVA_CAMY    = 0x480b68;
constexpr uint32_t RVA_TBL     = 0x480b6c;   // [ ] = 8192-entry table
constexpr uint32_t RVA_LUT     = 0x480b5c;   // [ ] = 256x256 colour table
constexpr uint32_t VTX_STRIDE  = 0x1c;

static int g_mode = 0;                       // D2_F3NATIF: 0 off, 1 serve, 2 oracle
static Cpu* g_cpu = nullptr;
static uint32_t g_trapCont = 0, g_trapVerify = 0;

// counters (published by f3_line)
static uint64_t n_calls=0, n_served=0, n_cells=0, n_drawn=0;
static uint64_t n_fb_type=0, n_fb_sub=0, n_fb_glide=0, n_fb_ring=0, n_fb_busy=0;
// D2_F3PROF=1: where the native time goes, by phase (one clock read per
// phase boundary, i.e. per cell, so ~1 us/cell of its own — diagnostic only).
static bool g_prof=false;
static uint64_t us_front=0, us_grid=0, us_draw=0, us_accept=0;
static inline uint64_t now_us(){ return rt_now_us(); }
static uint64_t n_ver=0, n_ver_bad=0, n_ver_skip=0, n_ver_cells=0, n_ver_bytes=0;

static inline uint32_t VA(uint32_t rva){ return g_d2base + rva; }
// DIRECT guest memory access, as in d2_intrin_114.cpp. Cpu::read/write cost a
// virtual call plus arena and code-page (isprotectedDB) checks on EVERY
// access; with ~40 accesses per cell and ~2 300 cells per frame that made the
// first version of this port 30 ms/frame SLOWER than the game (console,
// 26/09/2026). Everything touched here is game DATA (globals, tables, cells,
// the vertex array), never translated code, and the original reads and
// writes exactly the same addresses.
static inline uint8_t* G(uint32_t va){ return (uint8_t*)(dyn86_membase + va); }
static inline uint32_t ld(uint32_t va){ uint32_t v; std::memcpy(&v,G(va),4); return v; }
static inline void     st(uint32_t va, uint32_t v){ std::memcpy(G(va),&v,4); }
static inline int32_t  rd32(Cpu&, uint32_t rva){ return (int32_t)ld(VA(rva)); }
static inline int32_t  sar(int32_t v, int n){ return (int32_t)(v < 0 ? ~((~(uint32_t)v) >> n) : ((uint32_t)v >> n)); }

// 0x50dd60 — world (x, y, a3) -> screen (outX, outY). Same body as the
// verified intrinsic of d2_intrin_114.cpp (8.5-9.2 M comparisons, 0
// divergence), split in two so that a batch of projections can PREFETCH its
// table entries first: each one reads a 32 KiB table at a pseudo-random
// index, and on console those cache misses — not the arithmetic — were the
// cost (~1.3 us per cell with four sequential projections). Issued together,
// the misses overlap.
struct Glob { int32_t camX, camY, halfX, halfY; uint32_t tbl, lut; };
static inline void load_glob(Glob& g){
    g.camX=(int32_t)ld(VA(RVA_CAMX)); g.camY=(int32_t)ld(VA(RVA_CAMY));
    g.halfX=(int32_t)ld(VA(RVA_W))/2; g.halfY=(int32_t)ld(VA(RVA_H))/2;
    g.tbl=ld(VA(RVA_TBL)); g.lut=ld(VA(RVA_LUT));
}
struct PJ { int32_t A, C, edi, esi; uint32_t addr; };
static inline void pj_start(const Glob& g, int32_t x, int32_t y, int32_t a3, PJ& p){
    const int32_t ecx = x - g.camX, edx = y - g.camY;
    p.A = sar((int32_t)((uint32_t)ecx * 24u), 16);
    p.C = sar((int32_t)((uint32_t)edx * 24u), 16);
    p.edi = (int32_t)(((uint32_t)(p.A + p.C)) * 0x2d4u);
    p.esi = (int32_t)((uint32_t)a3 * 0x4ccu);
    const int32_t t = (int32_t)((uint32_t)p.edi * 0x376u + ((uint32_t)p.esi << 9));
    const uint32_t idx = (uint32_t)(sar(t, 20) - 0x1000) & 0x1fffu;
    p.addr = g.tbl + idx * 4u;
    __builtin_prefetch(G(p.addr));
}
static inline void pj_end(const Glob& g, const PJ& p, int32_t* ox, int32_t* oy){
    const int32_t k = (int32_t)ld(p.addr);
    int32_t cx = (int32_t)((uint32_t)(p.C - p.A) * (uint32_t)k);
    cx = (int32_t)((uint32_t)cx * 0x2d4u);
    *ox = sar(cx, 20) + g.halfX;
    int32_t sy = (int32_t)((uint32_t)p.esi * 0x376u - ((uint32_t)p.edi << 9));
    sy = sar(sy, 10);
    sy = (int32_t)((uint32_t)sy * (uint32_t)k);
    *oy = sar(sy, 20) + g.halfY - 0x20;
}
// 0x50dc30 — colour LUT: [vtx+8] = 0xFF000000 | T[r+b1]<<16 | T[r+b2]<<8 | T[r+b3].
static uint32_t lut(const Glob& g, uint32_t colour){
    const uint32_t r = g.lut + ((colour & 0xffu) << 8);
    const uint8_t b1=*G(r + ((colour >> 8) & 0xffu));
    const uint8_t b2=*G(r + ((colour >> 16) & 0xffu));
    const uint8_t b3=*G(r + (colour >> 24));
    return 0xFF000000u | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
}

struct Vtx { float x, y; uint32_t col; };

// One activation of F3 (not reentrant: 0x50fbd0 never calls F3).
struct State {
    bool     active=false, verify=false;
    uint32_t E=0, ret=0, ctx=0, ctab=0, type=0, count=0, cells=0, i=0;
    int32_t  Xs=0, Ys=0, L=0, T=0, R=0, B=0, shift=0;
    bool     drew=false;
    // current cell
    uint32_t cellptr=0, sub=0;
    int32_t  x0w=0,y0w=0,a0=0,x2w=0,y2w=0,a2=0,cx=0,cy=0,ca=0;
    uint32_t col[3]={0,0,0};
    Glob     g;
    Vtx      v[15];
    // oracle
    uint32_t headAtEntry=0;
    std::vector<uint8_t> expect;   // concatenated expected 0x31 record payloads
    uint32_t expectRet=0;
} S;

static bool visible(float x, float y){
    const double X=x, Y=y;
    return X >= (double)(float)S.L && X < (double)S.R && Y >= (double)S.T && Y < (double)S.B;
}
static void set_vtx(int k, int32_t ox, int32_t oy){ S.v[k].x=(float)ox; S.v[k].y=(float)oy; }

// Corners and visibility of cell S.i; fills the cell part of S. False = culled.
static bool cell_front(Cpu& c){
    S.cellptr = S.cells + S.i * 0x14u;
    const uint32_t w0 = ld(S.cellptr);
    const int16_t s0 = (int16_t)(w0 & 0xffff);
    const int16_t dy = (int16_t)(w0 >> 16);
    S.sub = (uint32_t)sar((int32_t)s0, 5);                  // checked <= 4 at entry
    const uint32_t b = VA(RVA_CORNERS) + 12u * (S.sub + 6u * S.type);
    const int32_t T0=(int32_t)ld(b), T1=(int32_t)ld(b+4), T2=(int32_t)ld(b+8);
    const int32_t T3=(int32_t)ld(b+12), T4=(int32_t)ld(b+16), T5=(int32_t)ld(b+20);
    S.x0w=T0+S.Xs; S.y0w=T1+S.Ys; S.a0=T2-dy;
    S.x2w=T3+S.Xs; S.y2w=T4+S.Ys; S.a2=T5-dy;
    load_glob(S.g);
    PJ p[4]; int32_t ox[4],oy[4];
    pj_start(S.g,S.x0w,S.y0w,S.a0,p[0]);
    pj_start(S.g,S.x0w,S.y0w,S.a0-0x20,p[1]);
    pj_start(S.g,S.x2w,S.y2w,S.a2-0x20,p[2]);
    pj_start(S.g,S.x2w,S.y2w,S.a2,p[3]);
    static const int K[4]={0,12,14,2};
    for(int i=0;i<4;i++){ pj_end(S.g,p[i],&ox[i],&oy[i]); set_vtx(K[i],ox[i],oy[i]); }
    // Integer test when every value converts to float EXACTLY (|v| < 2^24):
    // then (double)(float)v == v and visible()'s comparisons are the integer
    // ones. Outside that range, the original float/double test.
    auto small=[](int32_t v){ return v > -(1<<24) && v < (1<<24); };
    if(small(S.L)&&small(S.T)&&small(S.R)&&small(S.B)
       &&small(ox[0])&&small(ox[1])&&small(ox[2])&&small(ox[3])&&small(oy[0])&&small(oy[1])&&small(oy[2])&&small(oy[3])){
        for(int i=0;i<4;i++) if(ox[i]>=S.L && ox[i]<S.R && oy[i]>=S.T && oy[i]<S.B) return true;
        return false;
    }
    return visible(S.v[0].x,S.v[0].y) || visible(S.v[12].x,S.v[12].y)
        || visible(S.v[14].x,S.v[14].y) || visible(S.v[2].x,S.v[2].y);
}
// Centre and colours (0x50d136..0x50d1d5).
static void cell_mid(Cpu& c){
    S.cx=sar(S.x0w+S.x2w,1); S.cy=sar(S.y0w+S.y2w,1); S.ca=sar(S.a0+S.a2,1);
    const uint32_t A=ld(S.ctab+S.sub*4u), Bc=ld(S.ctab+S.sub*4u+4u);
    auto avg=[&](int sh)->uint32_t{ return (uint32_t)(sar((int32_t)(((A>>sh)&0xffu)+((Bc>>sh)&0xffu)),1)&0xff); };
    S.col[0]=A; S.col[2]=Bc;
    S.col[1]=(avg(24)<<24)|(avg(16)<<16)|(avg(8)<<8)|avg(0);
}
// Grid (0x50d1dd..0x50d38a): the remaining 11 projections, x += shift, LUT.
// The colour LUT depends on the colour only: three distinct colours per
// cell, so three lookups serve the fifteen calls of the original.
static void cell_grid(Cpu& c){
    const float off=(float)S.shift;
    load_glob(S.g);
    // The 11 projections of the grid, all started before any is finished.
    PJ p[11]; int k[11]; int n=0;
    for(int r=0;r<5;r++){
        const int32_t e=r*8;
        if(r!=0 && r!=4){
            pj_start(S.g,S.x0w,S.y0w,S.a0-e,p[n]); k[n++]=3*r;
            pj_start(S.g,S.x2w,S.y2w,S.a2-e,p[n]); k[n++]=3*r+2;
        }
        pj_start(S.g,S.cx,S.cy,S.ca-e,p[n]); k[n++]=3*r+1;
    }
    const uint32_t lc[3]={ lut(S.g,S.col[0]), lut(S.g,S.col[1]), lut(S.g,S.col[2]) };
    for(int i=0;i<n;i++){ int32_t ox,oy; pj_end(S.g,p[i],&ox,&oy); set_vtx(k[i],ox,oy); }
    for(int r=0;r<5;r++)
        for(int j=0;j<3;j++){ Vtx& v=S.v[3*r+j]; v.x = off + v.x; v.col = lc[j]; }
}
// Guest-memory writes of a cell's 15 vertices (x, y, colour).
static void write_vtx(Cpu& c){
    for(int k=0;k<15;k++){
        uint8_t* a=G(VA(RVA_VTX)+k*VTX_STRIDE);
        std::memcpy(a,&S.v[k].x,4); std::memcpy(a+4,&S.v[k].y,4); std::memcpy(a+8,&S.v[k].col,4);
    }
}
// Expected payload of one grDrawVertexArray(4,10,ptrs) record, from guest
// memory with this cell's x/y/colour overlaid (oracle: nothing was written).
static void expect_strip(Cpu& c, uint32_t rvaPtrs, uint32_t stride){
    const uint32_t hdr[3]={4u,10u,stride};
    const uint8_t* h=(const uint8_t*)hdr; S.expect.insert(S.expect.end(),h,h+12);
    for(int i=0;i<10;i++){
        const uint32_t p=c.read_u32(VA(rvaPtrs)+4u*i);
        std::vector<uint8_t> b(stride,0); c.read(p,b.data(),stride);
        const uint32_t k=(p-VA(RVA_VTX))/VTX_STRIDE;
        if(p>=VA(RVA_VTX) && k<15 && (p-VA(RVA_VTX))%VTX_STRIDE==0 && stride>=12){
            std::memcpy(&b[0],&S.v[k].x,4); std::memcpy(&b[4],&S.v[k].y,4); std::memcpy(&b[8],&S.v[k].col,4); }
        S.expect.insert(S.expect.end(),b.begin(),b.end());
    }
}

// Entry checks that decide native vs original. Fills S's call part.
static bool accept(Cpu& c){
    const uint32_t E=c.reg(R_ESP);
    S.E=E; S.ret=ld(E); S.ctx=ld(E+4);
    const int32_t X=(int32_t)ld(E+8), Y=(int32_t)ld(E+12);
    S.ctab=ld(E+16);
    S.Xs=(int32_t)((uint32_t)X<<13); S.Ys=(int32_t)((uint32_t)Y<<13);
    const uint32_t mode=c.reg(R_EAX);
    if(!S.ctx){ ++n_fb_type; return false; }
    S.type=ld(S.ctx);
    if(S.type>9){ ++n_fb_type; return false; }                  // the original asserts
    S.count=ld(S.ctx+0x50); S.cells=ld(S.ctx+0x54);
    if((int32_t)S.count>0){
        for(uint32_t i=0;i<S.count;i++){                         // every sub index <= 4, or the original asserts
            const int16_t s0=(int16_t)(ld(S.cells+i*0x14u)&0xffff);
            if((uint32_t)sar((int32_t)s0,5)>4u){ ++n_fb_sub; return false; } }
    }
    volatile uint32_t* H=nullptr; const uint8_t* R=nullptr; uint32_t sz=0;
    if(!gr_ring_view(&R,&sz,&H) || !H[12]){ ++n_fb_ring; return false; }
    if(ld(VA(RVA_GLDRAWVA))!=H[13]){ ++n_fb_glide; return false; }   // Glide table must point at OUR grDrawVertexArray
    // 0x50c690(frame, esi=mode, edi=&shift, push 1)
    const int32_t W=rd32(c,RVA_W), Hh=rd32(c,RVA_H);
    S.L=0; S.T=0; S.R=W; S.B=Hh-0x2f; S.shift=0;
    const int32_t q=(W + (int32_t)((uint32_t)(W>>31) & 3u)) >> 2;   // cdq; and edx,3; add; sar 2
    if(mode==1){ S.shift=-q; S.R-=q; S.L+=q; }
    else if(mode==2){ S.shift=q; S.R-=q; S.L+=q; }
    S.i=0; S.drew=false;
    return true;
}

// Replays the original entry: `push ebp`, resume at +1, EAX unchanged.
static uint32_t fallback(Cpu& c, Bridge& br){
    const uint32_t E=c.reg(R_ESP);
    c.write_u32(E-4,c.reg(R_EBP)); c.set_reg(R_ESP,E-8);
    br.redirect_next(VA(RVA_F3)+1);
    return c.reg(R_EAX);
}

// Runs cells from S.i until one needs the guest texture call (returns after
// arming it) or all are done (returns to F3's caller).
// The original writes the four corners' x,y for EVERY cell, culled ones
// included (0x50d059..0x50d0da): the vertex memory it leaves behind must match.
static void write_corners(Cpu& c){
    static const int K[4]={0,12,14,2};
    for(int k: K){
        uint8_t* a=G(VA(RVA_VTX)+k*VTX_STRIDE);
        std::memcpy(a,&S.v[k].x,4); std::memcpy(a+4,&S.v[k].y,4);
    }
}
// One grDrawVertexArray(4, 10, strip) record. Fast path: every pointer of the
// strip lies in the game's vertex array (the case F3 is written for), reached
// directly in host memory. Anything else: the generic gr_draw_native.
// D2_F3CHECK=1 re-reads the record just written against the generic path's
// bytes (the vertex memory it would have read), and counts differences.
static bool g_f3check=false; static uint64_t n_chk=0, n_chk_bad=0, n_draw_fast=0, n_draw_slow=0;
static void draw_strip(Cpu& c, uint32_t rvaPtrs, uint32_t stride){
    const uint32_t lo=VA(RVA_VTX), hi=lo+15u*VTX_STRIDE;
    const uint8_t* hv[10]; bool fast=true;
    for(int i=0;i<10;i++){ const uint32_t p=ld(VA(rvaPtrs)+4u*i);
        if(p<lo || p>=hi || p+stride>hi+VTX_STRIDE){ fast=false; break; }
        hv[i]=G(p); }
    if(!fast){ ++n_draw_slow; gr_draw_native(c,D2GR_OP_DRAWVERTEXARRAY,4,10,VA(rvaPtrs),stride); return; }
    ++n_draw_fast;
    const uint32_t rec=gr_draw_native_hv(4,10,hv,stride);
    if(g_f3check && rec){
        ++n_chk;
        const uint8_t* got=(const uint8_t*)c.hostptr(rec+16u,10u*stride);
        std::vector<uint8_t> exp(10u*stride);
        for(int i=0;i<10;i++){ const uint32_t p=c.read_u32(VA(rvaPtrs)+4u*i); c.read(p,&exp[(size_t)i*stride],stride); }
        if(!got || std::memcmp(got,exp.data(),exp.size())!=0){
            ++n_chk_bad;
            if(n_chk_bad<=4) jpline("F3 natif CONTROLE: enregistrement de bande different (#%llu)",(unsigned long long)n_chk_bad); }
    }
}
static uint32_t step(Cpu& c, Bridge& br){
    while((int32_t)S.i < (int32_t)S.count){
        ++n_cells;
        const uint64_t t0 = g_prof ? now_us() : 0;
        PH(PH_FRONT);
        const bool vis=cell_front(c);
        PH(PH_STEP);
        if(g_prof) us_front += now_us()-t0;
        // The original writes the four corners of EVERY cell. A culled cell's
        // corners are overwritten by the next cell before any guest code runs
        // (culled cells call nothing): only the visible cell's (before the
        // guest texture call) and the LAST cell's are observable.
        if(!vis){ ++S.i; if((int32_t)S.i >= (int32_t)S.count){ PH(PH_CORNERS); write_corners(c); PH(PH_STEP); } continue; }
        PH(PH_CORNERS);
        write_corners(c);
        PH(PH_STEP);
        PH(PH_MID);
        cell_mid(c);
        PH(PH_STEP);
        // 0x50fbd0(ecx = cell): guest code, returns into the continuation trap.
        st(S.E-4,g_trapCont);
        c.set_reg(R_ECX,S.cellptr);
        c.set_reg(R_ESP,S.E-8);                                 // the bridge pops 4 on the way out
        br.redirect_next(VA(RVA_TEXCELL));
        return c.reg(R_EAX);
    }
    S.active=false; ++n_served;
    c.set_reg(R_ESP,S.E+0x10);                                  // ret 0x10 (+4 popped by the bridge)
    br.redirect_next(S.ret);
    return S.drew?1u:0u;
}
static uint32_t cont(Cpu& c, Bridge& br){
    if(!S.active){ d2_crashlog("F3 natif: continuation sans activation"); return c.reg(R_EAX); }
    const uint64_t t0 = g_prof ? now_us() : 0;
    PH(PH_GRID);
    cell_grid(c);
    PH(PH_WVTX);
    write_vtx(c);
    PH(PH_DRAW);
    const uint64_t t1 = g_prof ? now_us() : 0;
    volatile uint32_t* H=nullptr; const uint8_t* R=nullptr; uint32_t sz=0;
    gr_ring_view(&R,&sz,&H);
    const uint32_t stride=H[12];
    draw_strip(c,RVA_STRIP0,stride);
    draw_strip(c,RVA_STRIP1,stride);
    if(g_prof){ const uint64_t t2=now_us(); us_grid+=t1-t0; us_draw+=t2-t1; }
    PH(PH_STEP);
    S.drew=true; ++n_drawn; ++S.i;
    return step(c,br);
}

// ---- oracle ---------------------------------------------------------------
static uint32_t verify_enter(Cpu& c, Bridge& br){
    // Simulate the whole call natively, writing nothing: the vertex memory
    // the strips read is overlaid in the expected bytes instead.
    volatile uint32_t* H=nullptr; const uint8_t* R=nullptr; uint32_t sz=0;
    gr_ring_view(&R,&sz,&H);
    const uint32_t stride=H[12];
    S.expect.clear(); S.drew=false;
    for(S.i=0;(int32_t)S.i<(int32_t)S.count;S.i++){
        if(!cell_front(c)) continue;
        cell_mid(c); cell_grid(c);
        expect_strip(c,RVA_STRIP0,stride); expect_strip(c,RVA_STRIP1,stride);
        S.drew=true;
        ++n_ver_cells;
    }
    S.expectRet=S.drew?1u:0u;
    S.headAtEntry=H[4];
    S.verify=true;
    c.write_u32(S.E,g_trapVerify);                              // return into the verify trap
    return fallback(c,br);                                       // and let the original run
}
static uint32_t verify_exit(Cpu& c, Bridge& br){
    const uint32_t eax=c.reg(R_EAX);
    if(!S.verify){ d2_crashlog("F3 natif: retour d'oracle sans armement"); return eax; }
    S.verify=false;
    volatile uint32_t* H=nullptr; const uint8_t* R=nullptr; uint32_t sz=0;
    gr_ring_view(&R,&sz,&H);
    // Collect the guest's 0x31 records appended since entry.
    std::vector<uint8_t> got;
    const uint32_t mask=sz-1u; uint32_t p=S.headAtEntry; const uint32_t end=H[4];
    bool bad=false;
    while(p!=end && (int32_t)(end-p)>0){
        uint32_t w0; std::memcpy(&w0,R+(p&mask),4);
        const uint32_t op=w0>>24, len=w0&0xffffffu;
        if(!len){ bad=true; break; }
        if(op==D2GR_OP_DRAWVERTEXARRAY){
            const uint32_t count=*(const uint32_t*)(R+((p+8)&mask)), stride=*(const uint32_t*)(R+((p+12)&mask));
            const uint32_t n=12u+count*stride;
            for(uint32_t k=0;k<n;k++) got.push_back(R[(p+4+k)&mask]);
        }
        p+=len*4u;
    }
    ++n_ver; n_ver_bytes+=got.size();
    if(bad || got!=S.expect || eax!=S.expectRet){
        ++n_ver_bad;
        if(n_ver_bad<=4) jpline("F3 natif ORACLE: divergence #%llu (attendu %zu o ret=%u, obtenu %zu o ret=%u)",
            (unsigned long long)n_ver_bad,S.expect.size(),S.expectRet,got.size(),eax);
    }
    c.set_reg(R_ESP,c.reg(R_ESP)-4);                            // undo the bridge's pop
    br.redirect_next(S.ret);
    return eax;
}

} // namespace

void native_f3_install(Cpu* cpu, Bridge& br){
    const char* e=getenv("D2_F3NATIF");
    g_mode = e ? atoi(e) : 0;
    g_prof = getenv("D2_F3PROF") != nullptr;
    g_f3check = getenv("D2_F3CHECK") != nullptr;
    if(g_mode<1 || g_mode>2 || !g_114 || !g_d2base) return;
    g_cpu=cpu;
    // The original must start with `push ebp; mov ebp,esp; sub esp,0x60`.
    uint8_t b[6]={0}; cpu->read(VA(RVA_F3),b,6);
    if(!(b[0]==0x55 && b[1]==0x8b && b[2]==0xec && b[3]==0x83 && b[4]==0xec && b[5]==0x60)){
        jpline("F3 natif: REFUS — prologue inattendu a Game+0x%x (%02x %02x %02x)",RVA_F3,b[0],b[1],b[2]); return; }
    Shim sc; sc.argc=0; sc.stdcall_cleanup=false; sc.tag="native!f3_cont";
    sc.fn=[&br](Cpu&c)->uint32_t{ return cont(c,br); };
    br.register_shim("native.hook","f3_cont",sc);
    g_trapCont=br.shim_trap("native.hook","f3_cont");
    Shim sv; sv.argc=0; sv.stdcall_cleanup=false; sv.tag="native!f3_verify";
    sv.fn=[&br](Cpu&c)->uint32_t{ return verify_exit(c,br); };
    br.register_shim("native.hook","f3_verify",sv);
    g_trapVerify=br.shim_trap("native.hook","f3_verify");
    Shim se; se.argc=0; se.stdcall_cleanup=false; se.tag="native!f3";
    se.fn=[&br](Cpu&c)->uint32_t{
        ++n_calls;
        if(S.active || S.verify){ ++n_fb_busy; return fallback(c,br); }   // never nested
        const uint64_t ta = g_prof ? now_us() : 0;
        PH(PH_ACCEPT);
        const bool ok = accept(c);
        PH(PH_STEP);
        if(g_prof) us_accept += now_us()-ta;
        if(!ok) return fallback(c,br);
        if(g_mode==2) return verify_enter(c,br);
        S.active=true;
        return step(c,br); };
    br.register_shim("native.hook","f3",se);
    cpu->set_alternate(VA(RVA_F3),br.shim_trap("native.hook","f3"));
    // D2_INTRINLINE: the entry and the continuation never block — served in
    // line from translated code (~300-480 round trips per frame in Act V).
    if(g_mode==1){ cpu->set_inline_shim(br.shim_trap("native.hook","f3")); cpu->set_inline_shim(g_trapCont);
        int on=0; unsigned long long st=0, sk=0; wx86_intrinline_counts(&on,&st,&sk);
        if(on) jpline("F3 natif: entree et continuation servies EN LIGNE (D2_INTRINLINE, %llu stubs en ligne au total, %llu laisses en sortie)",st,sk); }
    jpline("F3 natif: ARME (%s) — Game+0x%x, texture en continuation, dessins ecrits par l'hote",
           g_mode==2?"ORACLE, le jeu dessine":"sert",RVA_F3);
}

void native_f3_line(){
    if(!g_mode) return;
    if(g_prof) jpline("f3prof: accept=%llu us coins+visibilite=%llu us grille=%llu us dessins=%llu us (cumules)",
        (unsigned long long)us_accept,(unsigned long long)us_front,(unsigned long long)us_grid,(unsigned long long)us_draw);
    jpline("f3natif/bandes: directes=%llu generiques=%llu | controle D2_F3CHECK=%llu ecarts=%llu",
        (unsigned long long)n_draw_fast,(unsigned long long)n_draw_slow,(unsigned long long)n_chk,(unsigned long long)n_chk_bad);
    jpline("f3natif: appels=%llu servis=%llu cellules=%llu dessinees=%llu | replis type=%llu sub=%llu glide=%llu ring=%llu imbrique=%llu | oracle=%llu divergences=%llu cellules-comparees=%llu octets=%llu",
        (unsigned long long)n_calls,(unsigned long long)n_served,(unsigned long long)n_cells,(unsigned long long)n_drawn,
        (unsigned long long)n_fb_type,(unsigned long long)n_fb_sub,(unsigned long long)n_fb_glide,(unsigned long long)n_fb_ring,
        (unsigned long long)n_fb_busy,(unsigned long long)n_ver,(unsigned long long)n_ver_bad,
        (unsigned long long)n_ver_cells,(unsigned long long)n_ver_bytes);
}
