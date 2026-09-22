// gx_host.cpp — host-side GPU path: Glide ring reader, host-emitted records
// (D2_REPLAY60 / D2_RINGTAG), D2_GLIDEINV inventory, GPU state, atlas
// callbacks, gx_* (ring -> atlas -> batches), gr_replay, profiling lines
// gx_line / gr_line / gr_final_cumul. What rt_boot.cpp provides is declared in
// runtime/rt_host.h. See glide_ring/gx_host.h.
#include "glide_ring/gx_host.h"
#include "glide_ring/glide_atlas.h"
#include "glide_ring/replay60.h"
#include "platform/vita_gxm.h"
#include "platform/vita_present.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>
#include <algorithm>
#include <unistd.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <time.h>
#endif
using namespace d2rt;
extern "C" void r60_gxm_arm();
// Override de resolution de jeu (src/runtime/native_hooks_resolution.cpp).
// FAIBLES : l'anneau Glide se compile aussi seul (tools/build_glide_ring.sh),
// sans le runtime. Nuls dans ce cas, et grSstWinOpen garde la table Glide.
extern "C" { __attribute__((weak)) int d2res_active(void);
             __attribute__((weak)) int d2res_w(void);
             __attribute__((weak)) int d2res_h(void); }
#ifdef __vita__
extern "C" int d2vita_pin_self(int mask, unsigned* relu);
#endif

// Approximate size of a Glide texture upload. GrTexInfo =
// { smallLodLog2, largeLodLog2, aspectRatioLog2, format, data }, 5 dwords.
// In Glide3, LOD is log2 of the largest side (8 = 256), aspect ratio is a
// signed log2. We don't need byte-exactness here — just the ORDER OF
// MAGNITUDE of per-frame texture traffic, which is the main risk on a
// bandwidth-limited console.
uint64_t gl_texbytes(d2rt::Cpu& c, uint32_t info){
    if(!info) return 0;
    const uint32_t lod = c.read_u32(info+4);          // largeLodLog2
    const int32_t  ar  = (int32_t)c.read_u32(info+8); // aspectRatioLog2 (signed)
    const uint32_t fmt = c.read_u32(info+12);
    if(lod>8) return 0;
    uint32_t big = 1u<<lod, w=big, h=big;
    if(ar>0 && ar<=3)      h = big>>ar;               // wide
    else if(ar<0 && ar>=-3) w = big>>(-ar);           // tall
    const uint32_t bpp = (fmt>=8)? 2u : 1u;           // >=8: 16-bit formats
    return (uint64_t)w*h*bpp;
}
// ===================== HOST-SIDE GLIDE RING BUFFER READER =====================
// See src/glide_ring/glide_ring.h for the format. On the guest side, our
// glide3x.dll x86 serializes state and draws; here we replay them.
//
// UNDER QEMU this is a COUNTING SINK: nothing is drawn, we count what a real
// sceGxm path would have to submit (records, draws, verts, bytes, BATCHES
// after merging consecutive same-state draws) and hash the stream. The hash
// IS the replacement oracle: between GDI and Glide there's no longer a
// pixel-comparable image, but two identical runs must produce the SAME ring
// sequence. On Vita, this is where GPU submission hooks in.
// D2GR_MAGIC_H : glide_ring/gx_host.h
static uint32_t g_grHdrVA=0, g_grDataVA=0, g_grSize=0, g_grMask=0;
static uint32_t g_grTail=0;
// PERMANENT HOST VIEW of the ring and its header, captured ONCE at arm time
// (d2vGlideInit, game thread). The ring is our glide3x.dll's .bss: a
// contiguous span of the guest arena whose host address never moves again.
// This pointer — not `Cpu&` — is what the FLUSH THREAD (D2_FLUSHFIL) uses: it
// never touches the game thread's Cpu object.
static const uint8_t* g_grRingHost=nullptr;
static uint8_t*       g_grHdrHost=nullptr;
// Reads a ring word by ABSOLUTE byte counter (wraparound handled here).
// Strictly the same byte as c.read_u32(g_grDataVA+(x&mask)).
static inline uint32_t gr_rd(uint32_t absoff){
    uint32_t w; std::memcpy(&w, g_grRingHost+(absoff&g_grMask), 4); return w; }
static uint64_t g_grRecs=0,g_grDraws=0,g_grVerts=0,g_grBytes=0,g_grBatches=0,g_grState=0,g_grTexUp=0,g_grTexUpB=0;
static uint64_t g_grHash=1469598103934665603ull;
static uint64_t g_grStateSig=0, g_grLastDrawSig=~0ull;
static uint32_t g_grFrames=0, g_grStalls=0, g_grDropped=0, g_grDedupDll=0;
// REDUNDANT STATES (`redundant=` in the `ring:` line): a state record whose
// arguments match the LAST record of the same function in the SAME frame
// (same invalidation rules as the deduplication in glide3x_ring.c: frame,
// upload, palette, window; grEnable/grDisable excluded). This MEASURES what
// the DLL's dedup could omit. It costs ~8 comparisons and 8 writes per state
// record, so it is NOT enabled on the default path (where it would read 0 by
// construction): it arms itself with `D2_GRDEDUP=0` — the measurement leg —
// and can be forced with `D2_GRDUP=1`.
static uint32_t g_grDupLast[0x30][8]; static uint32_t g_grDupGen[0x30]; static uint32_t g_grDupCur=1;
static uint64_t g_grStateDup=0;
static bool grdup_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GRDUP");
        if(e&&*e) v=(atoi(e)!=0); else { const char* d=getenv("D2_GRDEDUP"); v=(d&&*d&&!strcmp(d,"0"))?1:0; } }
    return v!=0; }
uint64_t g_gbT1=0;   // timestamp of the last no-op call (crossing benchmark)
// D2_GRHASH=0 disables the stream's FNV hash (the `h=` oracle). Default is 1.
// The knob exists to MEASURE the oracle's cost in the ring walk, not to play
// without it: without hashing, `h=` stays at the FNV seed and the
// "ring-final" line no longer proves anything.
// D2_GRHASH=2: FULL hashing — every word of every record, vertices included,
// in order. This is the oracle that proves a stream is the SAME STREAM;
// `h=` (mode 1) only hashes each record's header, the draw's (count, stride)
// pair, and the state signature, so two streams differing by one argument
// VALUE can still hash the same. It's used to validate `D2_GRDEDUP=0` (the
// old stream, word for word); it costs one FNV step per word (~30000 per
// frame) and is not a play mode.
static bool g_grHashOn = true;
static int  g_grHashMode = 1;
static inline void gr_mix(uint64_t v){ if(g_grHashOn){ g_grHash^=v; g_grHash*=1099511628211ull; } }
static inline void gr_mix_all(const uint32_t* p, uint32_t len){
    if(g_grHashMode<2) return;
    for(uint32_t i=0;i<len;i++){ g_grHash^=p[i]; g_grHash*=1099511628211ull; } }

// ====== D2_REPLAY60 / D2_RINGTAG — RING side: records emitted by the HOST =====
// The guest hooks (unit draw Game+0xdc7b0, camera Game+0x5b440, simulation
// step Game+0x12fd90, D2_RINGPHASE_X phases) run on the game thread, between
// two guest instructions: they can therefore WRITE into the ring at `head`
// and publish `head` exactly like the x86 DLL's ring_alloc()
// (glide3x_ring.c) — same NOP padding at the end of the buffer, same refusal
// when full (counted, never blocking). The record lands IN ORDER in the
// stream, and the reader (gr_replay) sees it in place between draws. With no
// ring armed (GDI path), nothing is written. See src/glide_ring/replay60.h.
static uint64_t g_grEmitN=0, g_grEmitDrop=0;
bool gr_emit(d2rt::Cpu& c, uint32_t op, const uint32_t* w, uint32_t n){
    if(!g_grHdrVA||!g_grSize) return false;
    const uint32_t words=1u+n, bytes=words<<2;
    uint32_t head=c.read_u32(g_grHdrVA+16); const uint32_t tail=c.read_u32(g_grHdrVA+20);
    if(head-tail+bytes>g_grSize){ ++g_grEmitDrop; return false; }
    uint32_t off=head&g_grMask;
    if(off+bytes>g_grSize){
        const uint32_t pad=g_grSize-off;
        c.write_u32(g_grDataVA+off,(0u<<24)|(pad>>2));
        head+=pad; c.write_u32(g_grHdrVA+16,head);
        if(head-tail+bytes>g_grSize){ ++g_grEmitDrop; return false; }
        off=head&g_grMask; }
    c.write_u32(g_grDataVA+off,(op<<24)|(words&0x00FFFFFFu));
    for(uint32_t i=0;i<n;i++) c.write_u32(g_grDataVA+off+4u+4u*i,w[i]);
    c.write_u32(g_grHdrVA+16,head+bytes);
    ++g_grEmitN; return true;
}
// PER-FRAME reader state: unit spans (batch-builder vertices between
// UNITTAG and UNITEND), UI boundary, camera, step. Reset at FRAME_END.
static std::vector<d2gr::R60Unit> g_r60Units; static bool g_r60UnitOpen=false;
static uint32_t g_r60UiV0=0; static bool g_r60UiSeen=false;
uint32_t g_r60UiRva=0x00056ee0u;      // D2_REPLAY60_UI: rva of the function whose ENTRY opens the UI — Game+0x56ee0 (VA 0x456ee0),
                                             // 26 draws/frame = the panels; the cursor (Game+0x684c0) comes after
uint64_t g_r60Reent=0;
bool g_r60TagOn=false;                // = Replay60::tagOn(), set when the hooks are armed                // unit-draw entries seen while another was open
static bool g_r60CamValid=false; static uint32_t g_r60PlayerId=0; static int32_t g_r60PX=0,g_r60PY=0; static uint32_t g_r60OffX=0,g_r60OffY=0;
static uint32_t g_r60Tick=0; static uint64_t g_r60Frames=0, g_r60Recs=0;
bool g_r60Active=false;               // replay thread requested (D2_REPLAY60) and ring+GXM armed
// Per-PHASE inventory (D2_RINGPHASE_X=<rva,...>): ring draws and vertices
// emitted BETWEEN the entry and exit of each hooked function. Useful for
// locating markers like the UI boundary: hook the callees of the draw
// function at 0x44c990 and see who emits what.
// struct RPhase : glide_ring/gx_host.h
RPhase g_rph[8]; int g_rphN=0;
void cp_set_camera(int32_t ox,int32_t oy);
static void cp_line(const char* tag);
static void r60_ring_op(uint32_t op, uint32_t base, uint32_t len, uint32_t buildVerts){
    uint32_t a[8]={0,0,0,0,0,0,0,0};
    for(uint32_t i=1;i<len && i<8;i++) a[i]=gr_rd(base+4*i);
    ++g_r60Recs;
    switch(op){
      case D2GR_OP_UNITTAG: {
        if(g_r60UnitOpen && !g_r60Units.empty()) g_r60Units.back().v1=buildVerts;   // reentrance : on ferme l'ouvert
        d2gr::R60Unit u; u.id=a[1]; u.type=a[2]; u.x=(int32_t)a[3]; u.y=(int32_t)a[4]; u.mode=a[5];
        u.v0=buildVerts; u.v1=buildVerts; u.draws=0;
        if(g_r60Units.size()<2048) g_r60Units.push_back(u);
        g_r60UnitOpen=true; break; }
      case D2GR_OP_UNITEND:
        if(g_r60UnitOpen && !g_r60Units.empty()) g_r60Units.back().v1=buildVerts;
        g_r60UnitOpen=false; break;
      case D2GR_OP_CAMERA:
        g_r60OffX=a[1]; g_r60OffY=a[2]; g_r60PlayerId=a[3]; g_r60PX=(int32_t)a[4]; g_r60PY=(int32_t)a[5]; g_r60CamValid=(a[3]!=0);
        cp_set_camera((int32_t)a[1],(int32_t)a[2]); break;
      case D2GR_OP_TICK: g_r60Tick=a[1]; break;
      case D2GR_OP_PHASE: {
        const uint32_t rva=a[1]; const bool enter=a[2]!=0;
        if(rva==g_r60UiRva && enter && !g_r60UiSeen){ g_r60UiSeen=true; g_r60UiV0=buildVerts; }
        for(int i=0;i<g_rphN;i++) if(g_rph[i].rva==rva){
            if(enter){ if(g_rph[i].open) ++g_rph[i].nested; g_rph[i].open=true; ++g_rph[i].enters; }
            else g_rph[i].open=false; }
        break; }
      default: break;
    }
}
static void r60_on_draw(uint32_t cnt){
    if(g_r60UnitOpen && !g_r60Units.empty()) ++g_r60Units.back().draws;
    for(int i=0;i<g_rphN;i++) if(g_rph[i].open){ ++g_rph[i].draws; g_rph[i].verts+=cnt; }
}
static void r60_frame_reset(){
    g_r60Units.clear(); g_r60UnitOpen=false; g_r60UiSeen=false; g_r60UiV0=0;
    g_r60CamValid=false; ++g_r60Frames;
}

// ---------------- RING INVENTORY (D2_GLIDEINV=1) -------------------------
// WHY. Writing an sceGxm renderer requires the EXACT list of Glide states
// that D2 1.14d ACTUALLY emits — not the 200 combinations Glide3 allows. A
// shader that implements everything can't be validated or maintained; a
// shader that implements what the game actually emits can be checked against
// a counter. The inventory is therefore a MEASUREMENT, not a spec reading:
// every distinct argument tuple is counted, and the per-opcode total must
// match the state counter in the `ring:` line.
//
// It also decodes VERTICES: layout comes from grVertexLayout (the game
// declares it), and the min/max bounds of x,y,s,t,q,w say what space the game
// works in (640x480 or 800x600 window, texture coordinates in texels or
// normalized). Without these bounds, the vertex shader's projection matrix
// would be a guess.
static int g_grInvOn = -1;
static inline bool grinv_on(){ if(g_grInvOn<0) g_grInvOn = getenv("D2_GLIDEINV")?1:0; return g_grInvOn!=0; }
static std::map<std::string,uint64_t> g_grInv[64];
static const char* g_grOpName[64] = {0};
static void grinv_names_once(){
    static bool done=false; if(done) return; done=true;
    g_grOpName[0x10]="grTexSource";       g_grOpName[0x11]="grTexCombine";
    g_grOpName[0x12]="grTexFilterMode";   g_grOpName[0x13]="grTexMipMapMode";
    g_grOpName[0x14]="grAlphaBlendFunction"; g_grOpName[0x15]="grAlphaCombine";
    g_grOpName[0x16]="grColorCombine";    g_grOpName[0x17]="grConstantColorValue";
    g_grOpName[0x18]="grChromakeyMode";   g_grOpName[0x19]="grChromakeyValue";
    g_grOpName[0x1a]="grClipWindow";      g_grOpName[0x1b]="grDitherMode";
    g_grOpName[0x1c]="grColorMask";       g_grOpName[0x1d]="grDepthMask";
    g_grOpName[0x1e]="grDepthBufferMode"; g_grOpName[0x1f]="grDepthBufferFunction";
    g_grOpName[0x20]="grVertexLayout";    g_grOpName[0x21]="grEnable";
    g_grOpName[0x22]="grDisable";         g_grOpName[0x23]="grCoordinateSpace";
    g_grOpName[0x24]="grTexDownloadTable";g_grOpName[0x25]="grSstWinOpen";
    g_grOpName[0x30]="grBufferClear";
    g_grOpName[0x31]="grDrawVertexArray"; g_grOpName[0x32]="grDrawVertexArrayContiguous";
    g_grOpName[0x33]="grDrawTriangle";    g_grOpName[0x34]="grDrawLine";
    g_grOpName[0x35]="grDrawPoint";
}
// Glide3 names (3dfx's glide.h). An unknown value is rendered in hex: never
// guessed, the raw number is shown.
static const char* gl_cfunc(uint32_t v){ switch(v){
    case 0x0:return "ZERO"; case 0x1:return "LOCAL"; case 0x2:return "LOCAL_ALPHA";
    case 0x3:return "SCALE_OTHER"; case 0x4:return "SCALE_OTHER_ADD_LOCAL";
    case 0x5:return "SCALE_OTHER_ADD_LOCAL_ALPHA"; case 0x6:return "SCALE_OTHER_MINUS_LOCAL";
    case 0x7:return "SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL";
    case 0x8:return "SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA";
    case 0x9:return "SCALE_MINUS_LOCAL_ADD_LOCAL(BLEND)";
    case 0xa:return "SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA"; default:return 0; } }
static const char* gl_cfact(uint32_t v){ switch(v){
    case 0x0:return "ZERO/NONE"; case 0x1:return "LOCAL"; case 0x2:return "OTHER_ALPHA";
    case 0x3:return "LOCAL_ALPHA"; case 0x4:return "TEXTURE_ALPHA"; case 0x5:return "TEXTURE_RGB";
    case 0x8:return "ONE"; case 0x9:return "ONE_MINUS_LOCAL"; case 0xa:return "ONE_MINUS_OTHER_ALPHA";
    case 0xb:return "ONE_MINUS_LOCAL_ALPHA"; case 0xc:return "ONE_MINUS_TEXTURE_ALPHA";
    default:return 0; } }
static const char* gl_clocal(uint32_t v){ switch(v){
    case 0:return "ITERATED"; case 1:return "CONSTANT"; case 2:return "DEPTH"; default:return 0; } }
static const char* gl_cother(uint32_t v){ switch(v){
    case 0:return "ITERATED"; case 1:return "TEXTURE"; case 2:return "CONSTANT"; case 3:return "NONE";
    default:return 0; } }
static const char* gl_blend(uint32_t v){ switch(v){
    case 0x0:return "ZERO"; case 0x1:return "SRC_ALPHA"; case 0x2:return "SRC_COLOR/DST_COLOR";
    case 0x3:return "DST_ALPHA"; case 0x4:return "ONE"; case 0x5:return "ONE_MINUS_SRC_ALPHA";
    case 0x6:return "ONE_MINUS_SRC_COLOR/ONE_MINUS_DST_COLOR"; case 0x7:return "ONE_MINUS_DST_ALPHA";
    case 0xf:return "ALPHA_SATURATE/PREFOG"; default:return 0; } }
static const char* gl_texfmt(uint32_t v){ switch(v){
    case 0x0:return "RGB_332"; case 0x1:return "YIQ_422"; case 0x2:return "ALPHA_8";
    case 0x3:return "INTENSITY_8"; case 0x4:return "ALPHA_INTENSITY_44"; case 0x5:return "P_8";
    case 0x8:return "ARGB_8332"; case 0x9:return "AYIQ_8422"; case 0xa:return "RGB_565";
    case 0xb:return "ARGB_1555"; case 0xc:return "ARGB_4444"; case 0xd:return "ALPHA_INTENSITY_88";
    case 0xe:return "AP_88"; default:return 0; } }
static const char* gl_vparam(uint32_t v){ switch(v){
    case 0x01:return "XY"; case 0x02:return "Z"; case 0x03:return "W"; case 0x04:return "Q";
    case 0x05:return "FOG_EXT"; case 0x10:return "A"; case 0x20:return "RGB"; case 0x30:return "PARGB";
    case 0x40:return "ST0"; case 0x41:return "ST1"; case 0x42:return "ST2";
    case 0x50:return "Q0"; case 0x51:return "Q1"; case 0x52:return "Q2"; default:return 0; } }
static const char* gl_drawmode(uint32_t v){ switch(v){
    case 0:return "POINTS"; case 1:return "LINE_STRIP"; case 2:return "LINES"; case 3:return "POLYGON";
    case 4:return "TRIANGLE_STRIP"; case 5:return "TRIANGLE_FAN"; case 6:return "TRIANGLES";
    case 7:return "TRIANGLE_STRIP_CONTINUE"; case 8:return "TRIANGLE_FAN_CONTINUE"; default:return 0; } }
static void grinv_nm(char* d,size_t n,const char* s,uint32_t v){
    if(s) std::snprintf(d,n,"%s",s); else std::snprintf(d,n,"0x%x",v); }
// Vertex layout, declared by the game via grVertexLayout: BYTE offset of
// each parameter, -1 = disabled.
static int g_grVLoff[0x60] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
static bool g_grVLinit=false;
static uint32_t g_grVLstride=0;
// Component bounds, over the whole run.
static float g_grVxMin=1e30f,g_grVxMax=-1e30f,g_grVyMin=1e30f,g_grVyMax=-1e30f;
static float g_grVsMin=1e30f,g_grVsMax=-1e30f,g_grVtMin=1e30f,g_grVtMax=-1e30f;
static float g_grVqMin=1e30f,g_grVqMax=-1e30f,g_grVzMin=1e30f,g_grVzMax=-1e30f;
static uint64_t g_grVargbN=0, g_grVargbNonWhite=0, g_grVargbNonOpaque=0;
static uint64_t g_grVerbN=0, g_grVqOne=0, g_grVqOther=0;
// D2_GLIDEINV_V=<n>: prints the RAW vertices of the first n draws.
// This is the only way to settle the coordinate convention (s,t in texels or
// normalized; q constant or not) without guessing.
static uint32_t g_grVdump=0xFFFFFFFFu;
// Texture coordinate range PER TEXTURE SIZE. This is the question that
// decides the shader's UV: in Glide, are s,t in the bound texture's ACTUAL
// texels (0..w) or normalized against the largest LOD (0..256)? The global
// maximum doesn't answer this — the maximum PER size is needed.
struct GrStRange { float smax, tmax; uint64_t n, over; };
static std::map<uint32_t,GrStRange> g_grSt;   // key = (lod<<8)|(aspect&0xff)
static uint32_t g_grCurLod=0; static int32_t g_grCurAspect=0; static bool g_grCurTex=false;
static inline float gr_f32(uint32_t base, uint32_t off){
    union{uint32_t u; float f;} u; u.u=gr_rd(base+off); return u.f; }
// Decodes `cnt` vertices starting at ring offset `base` (absolute counter).
static void grinv_vertices(uint32_t base, uint32_t cnt, uint32_t stride){
    if(!g_grVLinit || !stride) return;
    const int oxy=g_grVLoff[0x01], oz=g_grVLoff[0x02], oq=g_grVLoff[0x04];
    const int oargb=g_grVLoff[0x30], ost0=g_grVLoff[0x40], oq0=g_grVLoff[0x50];
    if(g_grVdump==0xFFFFFFFFu){ const char* e=getenv("D2_GLIDEINV_V"); g_grVdump=e?(uint32_t)atoi(e):0u; }
    const bool dumpit = g_grVdump>0;
    if(dumpit){ --g_grVdump;
        std::printf("  [sommets] n=%u stride=%u :",cnt,stride); }
    for(uint32_t i=0;i<cnt && i<64;i++){
        const uint32_t v=base+i*stride;
        ++g_grVerbN;
        if(dumpit){ std::printf("\n     ");
            for(uint32_t o=0;o+4<=stride;o+=4){
                union{uint32_t u;float f;} uu; uu.u=gr_rd(v+o);
                std::printf(" +%02u=%08x(%.4f)",o,uu.u,(double)uu.f); } }
        if(oxy>=0){ float x=gr_f32(v,(uint32_t)oxy), y=gr_f32(v,(uint32_t)oxy+4);
            if(x<g_grVxMin)g_grVxMin=x; if(x>g_grVxMax)g_grVxMax=x;
            if(y<g_grVyMin)g_grVyMin=y; if(y>g_grVyMax)g_grVyMax=y; }
        if(oz>=0){ float z=gr_f32(v,(uint32_t)oz); if(z<g_grVzMin)g_grVzMin=z; if(z>g_grVzMax)g_grVzMax=z; }
        if(ost0>=0){ float s=gr_f32(v,(uint32_t)ost0), t=gr_f32(v,(uint32_t)ost0+4);
            if(s<g_grVsMin)g_grVsMin=s; if(s>g_grVsMax)g_grVsMax=s;
            if(t<g_grVtMin)g_grVtMin=t; if(t>g_grVtMax)g_grVtMax=t;
            if(g_grCurTex){
                GrStRange& r = g_grSt[(g_grCurLod<<8)|((uint32_t)g_grCurAspect&0xffu)];
                if(!r.n){ r.smax=s; r.tmax=t; } 
                if(s>r.smax) r.smax=s; if(t>r.tmax) r.tmax=t; ++r.n;
                uint32_t big=1u<<g_grCurLod, w=big, hh=big;
                if(g_grCurAspect>0 && g_grCurAspect<=3) hh=big>>g_grCurAspect;
                else if(g_grCurAspect<0 && g_grCurAspect>=-3) w=big>>(-g_grCurAspect);
                if(s>(float)w+0.01f || t>(float)hh+0.01f) ++r.over; } }
        { const int oqq = oq0>=0? oq0 : oq;
          if(oqq>=0){ float q=gr_f32(v,(uint32_t)oqq); if(q<g_grVqMin)g_grVqMin=q; if(q>g_grVqMax)g_grVqMax=q;
              if(q==1.0f) ++g_grVqOne; else ++g_grVqOther; } }
        if(oargb>=0){ uint32_t p=gr_rd(v+(uint32_t)oargb);
            ++g_grVargbN; if((p&0x00FFFFFFu)!=0x00FFFFFFu) ++g_grVargbNonWhite;
            if((p>>24)!=0xFFu) ++g_grVargbNonOpaque; } }
    if(dumpit){ std::printf("\n"); std::fflush(stdout); }
}
// A STATE record -> one readable inventory line.
static void grinv_state(uint32_t op, uint32_t base, uint32_t len){
    grinv_names_once();
    uint32_t a[8]={0,0,0,0,0,0,0,0};
    for(uint32_t i=1;i<len && i<8;i++) a[i]=gr_rd(base+4*i);
    char b[320]; char t1[64],t2[64],t3[64],t4[64];
    switch(op){
      case 0x16: case 0x15:   // grColorCombine / grAlphaCombine
        grinv_nm(t1,sizeof t1,gl_cfunc(a[1]),a[1]); grinv_nm(t2,sizeof t2,gl_cfact(a[2]),a[2]);
        grinv_nm(t3,sizeof t3,gl_clocal(a[3]),a[3]); grinv_nm(t4,sizeof t4,gl_cother(a[4]),a[4]);
        std::snprintf(b,sizeof b,"func=%s fact=%s local=%s other=%s invert=%u",t1,t2,t3,t4,a[5]); break;
      case 0x14:              // grAlphaBlendFunction
        grinv_nm(t1,sizeof t1,gl_blend(a[1]),a[1]); grinv_nm(t2,sizeof t2,gl_blend(a[2]),a[2]);
        grinv_nm(t3,sizeof t3,gl_blend(a[3]),a[3]); grinv_nm(t4,sizeof t4,gl_blend(a[4]),a[4]);
        std::snprintf(b,sizeof b,"rgb: %s / %s   alpha: %s / %s",t1,t2,t3,t4); break;
      case 0x10:              // grTexSource : tmu, 0, startAddr, evenOdd, lod, aspect, fmt
        grinv_nm(t1,sizeof t1,gl_texfmt(a[7]),a[7]);
        std::snprintf(b,sizeof b,"tmu=%u lod=%u aspect=%d fmt=%s",a[1],a[5],(int32_t)a[6],t1);
        if(a[1]==0){ g_grCurLod=a[5]; g_grCurAspect=(int32_t)a[6]; g_grCurTex=true; }
        break;
      case 0x11:              // grTexCombine
        grinv_nm(t1,sizeof t1,gl_cfunc(a[2]),a[2]); grinv_nm(t2,sizeof t2,gl_cfact(a[3]),a[3]);
        grinv_nm(t3,sizeof t3,gl_cfunc(a[4]),a[4]); grinv_nm(t4,sizeof t4,gl_cfact(a[5]),a[5]);
        std::snprintf(b,sizeof b,"tmu=%u rgb=%s/%s alpha=%s/%s rgbinv=%u ainv=%u",a[1],t1,t2,t3,t4,a[6],a[7]); break;
      case 0x20:              // grVertexLayout
        grinv_nm(t1,sizeof t1,gl_vparam(a[1]),a[1]);
        std::snprintf(b,sizeof b,"param=%s offset=%u mode=%u",t1,a[2],a[3]);
        if(a[1]<0x60){ g_grVLoff[a[1]] = a[3]? (int)a[2] : -1; g_grVLinit=true;
            if(a[3] && a[2]+16u>g_grVLstride) g_grVLstride=a[2]+16u; }
        break;
      case 0x1a: std::snprintf(b,sizeof b,"minx=%u miny=%u maxx=%u maxy=%u",a[1],a[2],a[3],a[4]); break;
      case 0x17: std::snprintf(b,sizeof b,"argb=0x%08x",a[1]); break;
      case 0x19: std::snprintf(b,sizeof b,"value=0x%08x",a[1]); break;
      case 0x24: std::snprintf(b,sizeof b,"type=%u (%s)",a[1],a[1]==2?"PALETTE":a[1]==3?"PALETTE_6666":"NCC"); break;
      case 0x25: {  // grSstWinOpen : res, refresh, cformat, origin, nColBuf, nAuxBuf
        static const char* R[]={"320x200","320x240","400x256","512x384","640x200","640x350","640x400","640x480",
                                "800x600","960x720","856x480","512x256","1024x768","1280x1024","1600x1200","400x300"};
        static const char* CF[]={"ARGB","ABGR","RGBA","BGRA"};
        std::snprintf(b,sizeof b,"res=%u (%s) refresh=%u cformat=%u (%s) origin=%u (%s) colbuf=%u auxbuf=%u",
          a[1],a[1]<16?R[a[1]]:"?",a[2],a[3],a[3]<4?CF[a[3]]:"?",a[4],a[4]==0?"UPPER_LEFT":"LOWER_LEFT",a[5],a[6]); break; }
      case 0x12: std::snprintf(b,sizeof b,"tmu=%u min=%u mag=%u",a[1],a[2],a[3]); break;
      case 0x1c: std::snprintf(b,sizeof b,"rgb=%u alpha=%u",a[1],a[2]); break;
      default:
        if(len>=2) std::snprintf(b,sizeof b,"%u (0x%x)",a[1],a[1]); else std::snprintf(b,sizeof b,"(sans argument)");
        break; }
    if(op<64) g_grInv[op][b]++;
}
static void gr_final_cumul();   // run totals (defined after the GPU path, which owns its counters)
static void ff_wait_idle();     // flush thread: waits for the current batch to finish (defined alongside the thread)
// Final report. What sceGxm must honor is exactly what's listed here.
void gr_inventory_dump(){
    gr_flush_stop();      // end of run: the flush thread has finished its last batch before we read its counters

    if(!g_grHdrVA) return;
    // DETERMINISM ORACLE for the Glide path (there is no pixel oracle between
    // GDI and Glide). The `ring:` line's hash is cumulative and read at the end
    // of a WINDOW: two runs that don't cut their windows at the same point show
    // different values WITHOUT having diverged. The only honest comparison is
    // at the end of the run, with an equal record count — this line.
    std::printf("[ring-final] enr=%llu dessins=%llu sommets=%llu lots=%llu etats=%llu"
                " texup=%llu octets=%llu images=%u | h=0x%016llx\n",
        (unsigned long long)g_grRecs,(unsigned long long)g_grDraws,(unsigned long long)g_grVerts,
        (unsigned long long)g_grBatches,(unsigned long long)g_grState,(unsigned long long)g_grTexUp,
        (unsigned long long)g_grBytes,g_grFrames,(unsigned long long)g_grHash);
    cp_line("-final");
    gr_final_cumul();
    std::fflush(stdout);
    if(!grinv_on()) return;
    grinv_names_once();
    std::printf("\n=== INVENTAIRE GLIDE (D2_GLIDEINV) — ce que D2 1.14d emet REELLEMENT ===\n");
    for(uint32_t op=0;op<64;op++){
        if(g_grInv[op].empty()) continue;
        uint64_t tot=0; for(auto&kv:g_grInv[op]) tot+=kv.second;
        std::printf("-- %-30s %llu appels, %zu combinaison(s) distincte(s)\n",
                    g_grOpName[op]?g_grOpName[op]:"(op inconnu)",(unsigned long long)tot,g_grInv[op].size());
        // Sorted by call count, descending: the top 24.
        std::vector<std::pair<uint64_t,const std::string*>> v;
        for(auto&kv:g_grInv[op]) v.push_back({kv.second,&kv.first});
        std::sort(v.begin(),v.end(),[](const std::pair<uint64_t,const std::string*>&a,
                                       const std::pair<uint64_t,const std::string*>&b){ return a.first>b.first; });
        size_t shown=0;
        for(auto&e:v){ if(shown++>=24){ std::printf("     ... et %zu autre(s)\n",v.size()-24); break; }
            std::printf("     %10llu x  %s\n",(unsigned long long)e.first,e.second->c_str()); }
    }
    std::printf("-- disposition des sommets (grVertexLayout) : stride declare >= %u octets\n",g_grVLstride);
    for(uint32_t p=0;p<0x60;p++) if(g_grVLoff[p]>=0){
        const char* n=gl_vparam(p); std::printf("     %-8s offset=%d\n",n?n:"?",g_grVLoff[p]); }
    if(g_grVerbN) std::printf(
        "-- bornes des sommets (%llu sommets echantillonnes)\n"
        "     x=[%.2f .. %.2f]  y=[%.2f .. %.2f]  z=[%.4f .. %.4f]\n"
        "     s=[%.2f .. %.2f]  t=[%.2f .. %.2f]  q=[%.6f .. %.6f]\n"
        "     pargb: %llu sommets, %llu a couleur != blanc, %llu a alpha != 255\n",
        (unsigned long long)g_grVerbN,
        (double)g_grVxMin,(double)g_grVxMax,(double)g_grVyMin,(double)g_grVyMax,
        (double)g_grVzMin,(double)g_grVzMax,
        (double)g_grVsMin,(double)g_grVsMax,(double)g_grVtMin,(double)g_grVtMax,
        (double)g_grVqMin,(double)g_grVqMax,
        (unsigned long long)g_grVargbN,(unsigned long long)g_grVargbNonWhite,
        (unsigned long long)g_grVargbNonOpaque);
    if(!g_grSt.empty()){
        std::printf("-- etendue s,t PAR TAILLE DE TEXTURE (repond : texels reels ou 0..256 ?)\n");
        for(auto&kv:g_grSt){ const uint32_t lod=kv.first>>8; const int32_t ar=(int8_t)(kv.first&0xff);
            uint32_t big=1u<<lod, w=big, hh=big;
            if(ar>0&&ar<=3) hh=big>>ar; else if(ar<0&&ar>=-3) w=big>>(-ar);
            std::printf("     %3ux%-3u (lod=%u aspect=%d) : smax=%.1f tmax=%.1f  sommets=%llu  hors-bornes=%llu\n",
                w,hh,lod,ar,(double)kv.second.smax,(double)kv.second.tmax,
                (unsigned long long)kv.second.n,(unsigned long long)kv.second.over); } }
    if(g_grVqOne+g_grVqOther) std::printf("     q : %llu sommets a q==1.0 exactement, %llu autres (%.2f%%)\n",
        (unsigned long long)g_grVqOne,(unsigned long long)g_grVqOther,
        100.0*(double)g_grVqOther/(double)(g_grVqOne+g_grVqOther));
    std::fflush(stdout);
}

// ===================== GPU PATH: RING -> ATLAS -> BATCHES =====================
// Armed by D2_GLIDEGXM=1 (default on; set =0 to disable). The section below
// is PORTABLE: it also runs under qemu, where d2gxm_* is inert. That's
// intentional — atlas hit rate, evictions, verts and batches per frame are
// exactly what needs measuring BEFORE wiring up a GPU, and qemu can run 6000
// frames in a few minutes.
bool grgx_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GLIDEGXM"); v=(e&&*e&&!strcmp(e,"0"))?0:1; } return v!=0; }
static d2gr::Atlas   g_gxAtlas;
static d2gr::Builder g_gxBuild;
static d2gr::BuildState g_gxSt;
static bool  g_gxInit=false, g_gxDead=false;
static uint32_t g_gxResW=800, g_gxResH=600;
static uint32_t g_gxClear=0;
// Current Glide state, as reported by the ring.
static uint32_t g_gxTexAddr=0;
static uint32_t g_gxLod=0; static int32_t g_gxAspect=0; static uint32_t g_gxFmt=5;
static uint32_t g_gxBlendSf=4, g_gxBlendDf=0;
static uint32_t g_gxCcFunc=3, g_gxCcOther=1;
static uint32_t g_gxAcFunc=0;
static uint32_t g_gxCst=0x000000FFu;
static uint32_t g_gxChroma=0, g_gxFilter=0;
static int      g_gxPalSlot=0; static uint64_t g_gxPalHash[D2GXM_PALETTES]={0};
static uint64_t g_gxFrame=0;
static uint32_t g_gxDroppedWin=0, g_gxDroppedPrev=0;   // batches/verts dropped by the builder, accumulated over the window
static uint64_t g_gxBlendUnknown=0, g_gxTexNoAtlas=0, g_gxSkipMode=0, g_gxFmtOther=0, g_gxLinesDrawn=0;
// Window counters (deltas are taken by gr_line).
static uint64_t g_gxVertsAcc=0, g_gxLotsAcc=0, g_gxFramesAcc=0;
static uint64_t g_gxFillAcc=0;      // pixels written per frame (D2_GXMOVERDRAW=1)
// ---- FLUSH TIMERS (host-side, portable) ---------------------------
// The ring flush does three distinct things — walking records, hashing and
// uploading textures, submitting to the GPU — and only measuring one of them
// leaves time unaccounted for. All three are measured here, and the
// `flush-total` figure published alongside lets you check for any remaining
// gap instead of assuming there is none.
static uint64_t g_gxFlushUs=0;      // total time inside d2vGlideFlush
// ⚡ Same timer, published for rt_boot's per-frame profile. Without this
// counter on the rt_boot side, there's no way to tell whether time is spent
// INSIDE the flush or ELSEWHERE, while the GIL is held by d2vGlideFlush.
extern "C" uint64_t d2rt_gx_flush_us = 0;
static uint64_t g_gxReplayUs=0;     // gr_replay (ring walk + batch building)
static uint64_t g_gxTexUs=0;        // gx_tex_upload: hash + atlas copy
static uint64_t g_gxSubmitUs=0;     // d2gxm_submit as seen from the caller side

// ---- RING WALK: THREE PERMANENT OPTIMIZATIONS -----------------------------
// These three used to be knobs (D2_RINGHOST, D2_GXSYNCCACHE, D2_GXVTXFAST)
// while they were being validated; they are now permanent, and the old path
// has been removed.
//   - direct host pointer into the ring (c.hostptr): a record never straddles
//     the end of the buffer — glide3x_ring.c pads with a NOP — so one pointer
//     per record is enough, with no fallback mid-vertex;
//   - batch state is only recomposed when a state record or an upload has
//     made it stale (the bind-ok/bind-miss counters are replayed to stay
//     identical);
//   - sNorm/tNorm inverses are precomputed per draw: since these are powers
//     of two (256>>k), s*(1/sNorm) == s/sNorm bit-for-bit, and the order of
//     the remaining operations is preserved.
//   D2_GRHASH=0      : disables the stream's FNV hash (measures the oracle's cost).
//   D2_GRPROF=1      : FINE-GRAINED per-record timers (state / draw, including
//                      recomposition and vertices / end of frame) — two clock
//                      reads per record, cost published (clock-ns), so never
//                      used in an A/B leg: it's an instrument.
//   D2_GXMLOTSHASH=1 : FNV hash of the built batches (verts, indices, batches)
//                      per frame, published as `lh=` — the oracle for the GPU
//                      half of the walk, which `h=` (the raw ring) can't see.
bool grprof_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GRPROF"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
static int  lotshash_mode(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GXMLOTSHASH"); v=(e&&*e)?atoi(e):0; } return v; }
static bool lotshash_on(){ return lotshash_mode()!=0; }   // 2 = additionally, one `gxlh:` line PER FRAME (that frame's hash alone): locates the first frame that diverges
static uint64_t g_grpStateUs=0, g_grpDrawUs=0, g_grpSyncUs=0, g_grpVtxUs=0, g_grpEndUs=0, g_grpClockNs=0;
static uint64_t g_grpTexHashUs=0, g_gxTexHashN=0, g_gxTexHashB=0;   // hashed P8 uploads: count, bytes, hashing time (GRPROF)
// TEXTURE HASHING (D2_TEXHASH). Hashing every uploaded texture (~9KB on
// average) with a WORD-BY-WORD 64-bit FNV-1a costs real per-frame CPU time,
// because FNV-64 is a SERIAL CHAIN of 64-bit multiplications (umull+2 mla per
// word on ARMv7).
//   0          : 64-bit FNV-1a over the words, then folded with (w<<32|h).
//   1 (default): 4 independent 32-bit multiply lanes (xxHash32-style rounds:
//                v = rotl(v + w*P2, 13) * P1), FULL COVERAGE, folded into 64
//                bits by two distinct avalanches of the 4 lanes (+ w,h,nb).
//                Same information hashed as mode 0, without the serial chain.
//   2          : SAMPLED — FNV-64 over every fourth word plus the first and
//                last 64 bytes plus the length. Bounds what a partial hash
//                would cost; does NOT prove the absence of collisions (two
//                textures differing only in unread bytes would be conflated):
//                measurement only, never used during play.
// Required validation (qemu bench): `hits=` and `lh=` must not change across modes.
// Mode 1 became the default on 2026-09-22 (it had been in the validated game
// env since 06/09, but the VPK ships no env.txt so no player ran it).
// Console, patrol bench: 8.16 -> 5.05 ns per hashed byte, +2.7 % frames/s ON
// ITS OWN (22.08 vs 21.49). Honest caveat: it buys NOTHING on top of
// D2_FLUSHFIL (23.82 vs 23.92, inside the noise) because the hashing then runs
// on the flush thread, which has ~23 ms of idle per frame. Kept as the default
// because it is strictly less work, not because it shows up in frames/s.
// NOT a collision risk: over a full run it produces 4499 atlas misses against
// the control's 4475 — MORE, not fewer. A collision would conflate two
// textures and produce fewer.
static int texhash_mode(){ static int v=-1; if(v<0){ const char* e=getenv("D2_TEXHASH"); v=(e&&*e)?atoi(e):1; if(v<0||v>2) v=1; } return v; }
static inline uint32_t rotl32(uint32_t x,int r){ return (x<<r)|(x>>(32-r)); }
static uint64_t tex_hash(const uint8_t* p, uint32_t nb, uint32_t w, uint32_t h){
    const int mode=texhash_mode();
    if(mode==1){
        const uint32_t P1=2654435761u,P2=2246822519u,P3=3266489917u,P4=668265263u,P5=374761393u;
        uint32_t v1=P1+P2, v2=P2, v3=0, v4=0u-P1;
        uint32_t i=0;
        for(; i+16<=nb; i+=16){
            uint32_t a,b,c,d; std::memcpy(&a,p+i,4); std::memcpy(&b,p+i+4,4); std::memcpy(&c,p+i+8,4); std::memcpy(&d,p+i+12,4);
            v1=rotl32(v1+a*P2,13)*P1; v2=rotl32(v2+b*P2,13)*P1; v3=rotl32(v3+c*P2,13)*P1; v4=rotl32(v4+d*P2,13)*P1; }
        uint32_t lo=rotl32(v1,1)+rotl32(v2,7)+rotl32(v3,12)+rotl32(v4,18)+nb;
        uint32_t hi=(rotl32(v1,5)^rotl32(v2,11)^rotl32(v3,17)^rotl32(v4,23))+P5+nb;
        for(; i+4<=nb; i+=4){ uint32_t a; std::memcpy(&a,p+i,4); lo=rotl32(lo+a*P3,17)*P4; hi=rotl32(hi+a*P2,11)*P3; }
        for(; i<nb; i++){ lo=rotl32(lo+p[i]*P5,11)*P1; hi=rotl32(hi+p[i]*P1,13)*P2; }
        lo^=(w<<16)^h; hi^=(h<<16)^w;
        lo^=lo>>15; lo*=P2; lo^=lo>>13; lo*=P3; lo^=lo>>16;
        hi^=hi>>15; hi*=P3; hi^=hi>>13; hi*=P2; hi^=hi>>16;
        return ((uint64_t)hi<<32)|lo;
    }
    uint64_t hsh=1469598103934665603ull;
    if(mode==2){
        uint32_t i=0;
        for(; i+4<=nb; i+=16){ uint32_t v; std::memcpy(&v,p+i,4); hsh^=v; hsh*=1099511628211ull; }
        const uint32_t head = nb<64? nb : 64u;
        for(i=0; i+4<=head; i+=4){ uint32_t v; std::memcpy(&v,p+i,4); hsh^=v; hsh*=1099511628211ull; }
        if(nb>64){ const uint32_t t0 = nb-64u; for(i=t0; i+4<=nb; i+=4){ uint32_t v; std::memcpy(&v,p+i,4); hsh^=v; hsh*=1099511628211ull; } }
        hsh^=nb; hsh*=1099511628211ull;
    } else {
        uint32_t i=0; for(; i+4<=nb; i+=4){ uint32_t v; std::memcpy(&v,p+i,4); hsh^=v; hsh*=1099511628211ull; }
        for(; i<nb; i++){ hsh^=p[i]; hsh*=1099511628211ull; }
    }
    hsh^=((uint64_t)w<<32)|h; hsh*=1099511628211ull;
    return hsh;
}
static uint64_t g_gxSyncDone=0, g_gxSyncSkipped=0;
static uint64_t g_gxLotsHash=1469598103934665603ull, g_gxLotsHashN=0;
static uint64_t g_gxLotsHashU=1469598103934665603ull;   // `lu=`: see gx_lots_hash
static bool g_gxStDirty=true;                     // batch state must be recomposed before the next draw

// ⚠️ Under D2_REPLAY60, submission happens on a DIFFERENT thread: every
// sceGxm call from the game thread (pages, palettes, drain) goes through the
// context lock (r60_gxm_lock, a no-op while the replay thread doesn't exist).
static void gx_page_upload_cb(int page,int x,int y,int w,int h,const uint8_t* src,int pitch){
    r60_gxm_lock(); d2gxm_page_upload(page,x,y,w,h,src,pitch); r60_gxm_unlock();
}
static int gx_page_make_cb(int page,int dim){ r60_gxm_lock(); const int r=d2gxm_page_create(page,dim); r60_gxm_unlock(); return r; }
// EVICTION FENCE. Returns the oldest frame the GPU hasn't finished reading.
// Off-target and under synchronous submission, d2gxm_busy_from() returns
// UINT64_MAX: the eviction policy then reduces to the plain case, including
// under qemu. Under asynchronous submission, the frame CURRENTLY BEING BUILT
// is also protected: its cells are about to go to the GPU.
static uint64_t gx_fence_cb(){
    const uint64_t busy = d2gxm_busy_from();
    if(busy == ~(uint64_t)0) return ~(uint64_t)0;
    return busy < g_gxFrame ? busy : g_gxFrame;
}
static void gx_drain_cb(){ r60_gxm_lock(); d2gxm_drain(); r60_gxm_unlock(); }

// OVERDRAW. Sum of triangle areas, in SCREEN pixels, roughly clipped to the
// visible window (the game emits off-screen geometry: x ranges from -374 to
// 1175). The ratio to look at is this sum over 960x544 = 522240: the number
// of times the hardware writes each screen pixel. PORTABLE — measurable under
// qemu, without a GPU.
static bool gxod_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GXMOVERDRAW"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
// HARDWARE PALETTE (D2_GXMPAL=1). Read HERE and not only in the backend:
// under it the palette belongs to the texture DESCRIPTOR, so a batch can only
// carry one palette and the batch builder must split on it. The knob is
// therefore read on both sides, portably — which is what lets the batches-
// per-frame overhead be measured under qemu, before any GPU.
static bool gxpal_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GXMPAL"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
static void gx_overdraw(){
    const d2gr::Vtx* v=g_gxBuild.vdata(); const uint16_t* ix=g_gxBuild.idata();
    const uint32_t ni=g_gxBuild.indices();
    const float sx=960.0f/(float)g_gxResW, sy=544.0f/(float)g_gxResH;
    double acc=0.0;
    for(uint32_t i=0;i+2<ni;i+=3){
        const d2gr::Vtx&A=v[ix[i]],&B=v[ix[i+1]],&C=v[ix[i+2]];
        const float ax=A.x*sx, ay=A.y*sy, bx=B.x*sx, by=B.y*sy, cx=C.x*sx, cy=C.y*sy;
        float cr=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax); if(cr<0) cr=-cr;
        const double area=0.5*(double)cr;
        if(area<=0.0) continue;
        // Visible fraction, estimated via the bounding box: exact for axis-aligned
        // quads (UI, sky), approximate for the ground's diamond tiles. This is
        // SPELLED OUT rather than implying an exact clip.
        float x0=ax<bx?ax:bx; if(cx<x0) x0=cx;
        float x1=ax>bx?ax:bx; if(cx>x1) x1=cx;
        float y0=ay<by?ay:by; if(cy<y0) y0=cy;
        float y1=ay>by?ay:by; if(cy>y1) y1=cy;
        const float w=x1-x0, h=y1-y0;
        if(w<=0.0f||h<=0.0f) continue;
        float qx0=x0<0.0f?0.0f:x0, qx1=x1>960.0f?960.0f:x1;
        float qy0=y0<0.0f?0.0f:y0, qy1=y1>544.0f?544.0f:y1;
        if(qx1<=qx0||qy1<=qy0) continue;
        acc += area * (double)(((qx1-qx0)*(qy1-qy0))/(w*h));
    }
    // The clear quad is NOT part of the batch builder (the backend adds it):
    // without it, the published figure would understate the real cost of a
    // full-screen clear. It's added here unless D2_GXMNOCLEAR removes it —
    // D2_GXMCLEAR=none removes it just the same. Either way, the full-screen
    // clear must be excluded from overdraw, or the variant would look like it
    // saves nothing. `flat` keeps the quad (only its SHADER changes, not its
    // fragments): it stays in the count.
    { static int nc=-1; if(nc<0){ const char* e=getenv("D2_GXMNOCLEAR"); nc=(e&&*e&&strcmp(e,"0"))?1:0;
        const char* c=getenv("D2_GXMCLEAR");
        if(c&&*c&&(*c=='a'||*c=='A'||*c=='0')) nc=1; }
      if(!nc) acc += 960.0*544.0; }
    g_gxFillAcc += (uint64_t)acc;
}

// SCREEN COVERAGE (D2_GXMCOVER=1). Question: can the clear be REMOVED? The
// only thing standing in the way is that a pixel not covered by the game's
// geometry would show the previous frame. This counter answers with a
// measurement instead of a guess: it actually RASTERIZES the batch's
// triangles (excluding the clear quad, which the backend adds) into a mask,
// and publishes the number of pixels the game does NOT cover.
// PORTABLE: runs under qemu, without a GPU. Half-pixel grid (480x272) — fine
// enough to see a UI strip or an unexplored map edge, and 4x cheaper than
// full screen; the figure is scaled up to 960x544 to compare against the
// screen's 522240 pixels.
static bool gxcov_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GXMCOVER"); v=(e&&*e&&strcmp(e,"0"))?1:0; } return v!=0; }
static uint64_t g_gxCovAcc=0, g_gxCovWorst=0, g_gxCovFrames=0;
static void gx_coverage(){
    static const int CW=480, CH=272;
    static std::vector<uint8_t> mask;
    if(mask.empty()) mask.resize((size_t)CW*CH);
    std::memset(mask.data(),0,mask.size());
    const d2gr::Vtx* v=g_gxBuild.vdata(); const uint16_t* ix=g_gxBuild.idata();
    const uint32_t ni=g_gxBuild.indices();
    // game window -> half-screen (the same projection as the vertex shader)
    const float sx=(float)CW/(float)g_gxResW, sy=(float)CH/(float)g_gxResH;
    for(uint32_t i=0;i+2<ni;i+=3){
        const d2gr::Vtx&A=v[ix[i]],&B=v[ix[i+1]],&C=v[ix[i+2]];
        const float ax=A.x*sx, ay=A.y*sy, bx=B.x*sx, by=B.y*sy, cx=C.x*sx, cy=C.y*sy;
        float x0=ax<bx?ax:bx; if(cx<x0) x0=cx;
        float x1=ax>bx?ax:bx; if(cx>x1) x1=cx;
        float y0=ay<by?ay:by; if(cy<y0) y0=cy;
        float y1=ay>by?ay:by; if(cy>y1) y1=cy;
        int px0=(int)x0; if(px0<0) px0=0;
        int px1=(int)x1+1; if(px1>CW) px1=CW;
        int py0=(int)y0; if(py0<0) py0=0;
        int py1=(int)y1+1; if(py1>CH) py1=CH;
        if(px1<=px0||py1<=py0) continue;
        // edge functions, sign-unified: the game emits both winding orders
        const float e0x=bx-ax, e0y=by-ay, e1x=cx-bx, e1y=cy-by, e2x=ax-cx, e2y=ay-cy;
        const float area=e0x*(cy-ay)-e0y*(cx-ax);
        if(area==0.0f) continue;
        const float sgn = area>0.0f?1.0f:-1.0f;
        for(int py=py0;py<py1;py++){
            const float fy=(float)py+0.5f;
            uint8_t* row=mask.data()+(size_t)py*CW;
            for(int px=px0;px<px1;px++){
                if(row[px]) continue;
                const float fx=(float)px+0.5f;
                const float w0=(e0x*(fy-ay)-e0y*(fx-ax))*sgn;
                const float w1=(e1x*(fy-by)-e1y*(fx-bx))*sgn;
                const float w2=(e2x*(fy-cy)-e2y*(fx-cx))*sgn;
                if(w0>=0.0f&&w1>=0.0f&&w2>=0.0f) row[px]=1;
            }
        }
    }
    uint32_t nu=0; for(size_t k=0;k<mask.size();k++) if(!mask[k]) ++nu;
    const uint64_t scaled=(uint64_t)nu*4ull;      // ramene a 960x544
    g_gxCovAcc+=scaled; ++g_gxCovFrames;
    if(scaled>g_gxCovWorst) g_gxCovWorst=scaled;
}

// Blend function index. Only these four pairs are handled, and NO others:
// any unexpected pair is counted and rendered opaque rather than drawn wrong
// silently.
static uint8_t gx_blend_index(uint32_t sf,uint32_t df){
    if(sf==4 && df==0) return 0;      // ONE / ZERO
    if(sf==1 && df==5) return 1;      // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
    if(sf==4 && df==4) return 2;      // ONE / ONE
    if(sf==0 && df==2) return 3;      // ZERO / SRC_COLOR  (shadows)
    ++g_gxBlendUnknown; return 0;
}
static void gx_lazy_init(){
    if(g_gxInit || g_gxDead) return;
    g_gxInit=true;
    // Defaults picked for a memory/batch-count tradeoff: 512-pixel pages =>
    // ~21MiB actually used, ZERO eviction, ~115 batches/frame; 1024-pixel
    // pages => ~90 batches/frame but ~34MiB. Memory is favored.
    uint32_t mio=32, dim=512;
    if(const char* e=getenv("D2_GXMATLAS")){ unsigned v=atoi(e); if(v>=1&&v<=96) mio=v; }
    if(const char* e=getenv("D2_GXMPAGE")){ unsigned v=atoi(e); if(v==256||v==512||v==1024||v==2048) dim=v; }
#ifdef __vita__
    if(!d2gxm_init((int)g_gxResW,(int)g_gxResH)){ g_gxDead=true; 
        d2vita_progress("gxm: init KO — le flush retombe sur le puits"); return; }
#endif
    g_gxAtlas.init(mio<<20, dim, gx_page_upload_cb, gx_page_make_cb, gx_fence_cb, gx_drain_cb);
    g_gxStDirty=true;              // the atlas was just created: batch state must be recomposed
    g_gxBuild.init(65535, 196608, 8192);   // caps raised (see vita_gxm.cpp MAXV)
    g_gxBuild.splitByPalette(gxpal_on());
    char m[208];
    std::snprintf(m,sizeof m,"gxm: atlas %u Mio, pages %ux%u (max %u), fenetre %ux%u, lots coupes par palette=%s",
                  mio,dim,dim,g_gxAtlas.maxPages(),g_gxResW,g_gxResH, gxpal_on()?"OUI":"non");
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
}
// Recomposes batch state from the current Glide state. Only called when the
// state has changed (gx_state) or the atlas has moved (gx_tex_upload) —
// resolve() is a hash-table lookup, and real state changes are much less
// frequent than draws.
// ORDERED BINDINGS. Binding TMU address -> cell AT UPLOAD TIME (an immediate
// crossing) is unsafe because draws are replayed AT FLUSH TIME: if D2 reuses
// an address within the same frame (glyphs, panel pieces — its 4MiB TMU
// memory is full), every draw in the frame would pick up the LAST binding
// (visible as NPCs replaced by font glyphs, or a garbled inventory panel).
// Instead, every upload emits a TEXBIND record into the ring (gr_emit, in
// order) and the replay keeps its own address -> (cell, generation) table,
// read by gx_sync_state. D2_GXBINDORDER=0 selects the unsafe immediate-bind
// behavior (for comparison/regression testing).
static bool bindorder_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_GXBINDORDER"); v=(e&&*e&&!strcmp(e,"0"))?0:1; } return v!=0; }
struct GxBind { int32_t cell; uint32_t gen; };
static std::unordered_map<uint32_t,GxBind> g_gxBindAt;
static uint64_t g_gxBindOrdered=0, g_gxBindFallback=0, g_gxBindStale=0;
static void gx_sync_state(){
    ++g_gxSyncDone;
    int32_t cell = -1;
    if(g_gxAtlas.ready()){
        if(bindorder_on()){
            auto it=g_gxBindAt.find(g_gxTexAddr);
            if(it!=g_gxBindAt.end()){
                const int32_t ci=it->second.cell;
                if(ci>=0 && g_gxAtlas.cell(ci).gen==it->second.gen){ cell=ci; ++g_gxBindOrdered; }
                else { ++g_gxBindStale; cell=g_gxAtlas.resolve(g_gxTexAddr); }
            } else { ++g_gxBindFallback; cell=g_gxAtlas.resolve(g_gxTexAddr); }
        } else cell=g_gxAtlas.resolve(g_gxTexAddr);
    }
    if(cell<0) ++g_gxTexNoAtlas;
    g_gxSt.cell = cell;
    // Coordinate convention: s in [0,S], t in [0,T]
    // with S = 256>>max(0,-aspect) and T = 256>>max(0,aspect). NOT texels.
    g_gxSt.sNorm = (g_gxAspect<0 && g_gxAspect>=-3) ? (256u >> (uint32_t)(-g_gxAspect)) : 256u;
    g_gxSt.tNorm = (g_gxAspect>0 && g_gxAspect<= 3) ? (256u >> (uint32_t)( g_gxAspect)) : 256u;
    g_gxSt.blend = gx_blend_index(g_gxBlendSf,g_gxBlendDf);
    uint8_t f=0;
    if(g_gxChroma) f|=1;
    if(g_gxFilter) f|=2;
    if(g_gxCcFunc==3 && g_gxCcOther==1 && cell>=0) f|=4;   // color = texture x vertex
    if(g_gxAcFunc==1) f|=8;                                 // alpha = the constant's alpha
    g_gxSt.flags=f;
    g_gxSt.cst=g_gxCst;
    g_gxSt.palv=((float)g_gxPalSlot+0.5f)/(float)D2GXM_PALETTES;
    g_gxSt.pal =(uint8_t)g_gxPalSlot;
}
// The upcoming draw's batch state. With the cache, a still-valid state
// REPLAYS the counters resolve() would have incremented (bind ok/miss,
// texture outside atlas): the `gxm:` line must stay the same down to the count.
static inline void gx_sync_for_draw(){
    if(g_gxStDirty){ gx_sync_state(); g_gxStDirty=false; return; }
    ++g_gxSyncSkipped;
    if(g_gxAtlas.ready()) g_gxAtlas.noteResolve(g_gxSt.cell>=0);
    if(g_gxSt.cell<0) ++g_gxTexNoAtlas;
}
// ============ FLUSH THREAD (D2_FLUSHFIL=1, default 0) ========================
// The ring flush (walking records, building batches, GXM submission) runs on
// the GAME THREAD, on core USER_0, holding the GIL — while USER_1 sits idle
// and the GPU waits. This knob moves it to a dedicated host thread.
//
// WHAT MAKES THIS SAFE — each point below is enforced in code:
//  (a) the ring is a producer/consumer buffer: `head` is written only by the
//      guest, `tail` only by the host (glide_ring.h). The game thread hands
//      the flush thread the `head` value captured at grBufferSwap; the flush
//      thread NEVER consumes past it. Everything before that head was written
//      by the game thread BEFORE the handoff (program order), and the handoff
//      carries a full barrier on both sides.
//  (b) reading the ring no longer goes through `Cpu&`: `g_grRingHost` is
//      captured once at arm time (gr_ring_arm) and the walk uses only that.
//      `tail` is written back by the flush thread into the guest header
//      through this same host view, after a barrier: it's one aligned 32-bit
//      word.
//  (c) TEXTURE UPLOADS ARE OFFLOADED TOO. They can't stay on the game thread:
//      they write into the ATLAS, which the flush thread reads on every draw
//      — and more importantly, the guest memory pointed to by
//      grTexDownloadMipMap is reusable as soon as the call returns. The GAME
//      thread therefore copies the bytes into a HOST staging buffer
//      (ff_stg_put) and emits a TEXUP record into the ring; the flush thread
//      does the hashing, the atlas copy, and the binding WHEN IT REACHES THAT
//      RECORD. This is exactly the position the TEXBIND occupies under
//      synchronous submission: the sequence of atlas operations is UNCHANGED,
//      so the batches produced are the same (the `lh=` and `lu=` oracles).
//  (d) the atlas, the batch builder, the bind table, and the Glide state are
//      then touched ONLY by the flush thread (the game thread only keeps
//      gx_lazy_init, forced before the thread starts).
//  (e) sceGxm: already protected by r60_gxm_lock; this thread arms the lock
//      even without 60Hz replay (r60_gxm_arm).
//  (f) the 60Hz replay consumes the same structures: it only touches them via
//      Replay60::push(), called here from the flush thread — the push/step
//      contract (spin lock + slots) is unchanged, only the caller's thread
//      changes.
//  (g) BACKPRESSURE: the ring is bounded (2MiB). The game thread waits, at
//      grBufferSwap, for the previous batch to be consumed (`waits=`): at most
//      ONE frame in flight, so at most TWO frames' worth of ring occupancy —
//      double the synchronous case, on a ring that holds about 10.
// ON by default since 2026-09-22, D2_FLUSHFIL=0 to disable. It had been in the
// validated game env since 06/09, but the VPK ships no env.txt, so no player
// ever ran it. Console, patrol bench on the real save (4 controls dispersed by
// 0.3 % over 8 hours): 21.49 -> 23.92 frames/s (+11.3 %), and — the reason it
// matters more than the average — frames taking 50-100 ms drop from 1480 to 68
// out of 6200. The game thread's time inside the flush goes from 9600 us to 3.
// ⚠️ Removing 12-13 ms from the game thread only returns ~4.7 ms of frame time:
// the three cores share L2 and memory bandwidth, and the walk moves ~1 MB per
// frame. Deporting MEMORY-bound work to another core recovers about a third of
// it, not all. Do not size future work on the un-deported figure.
static bool ff_on(){ static int v=-1; if(v<0){ const char* e=getenv("D2_FLUSHFIL"); v=(e&&*e&&!strcmp(e,"0"))?0:1; } return v!=0; }
static bool g_ffArmed=false;                 // the thread is running: the async path is active
static volatile int      g_ffPending=0;      // 1 = a batch is waiting to be consumed
static volatile uint32_t g_ffHead=0;         // handed-off head (absolute byte counter)
static volatile int      g_ffStop=0, g_ffRunning=0;
static uint64_t g_ffWaits=0, g_ffWaitUs=0, g_ffWaitMax=0, g_ffLots=0, g_ffBusyUs=0;
static uint64_t g_ffStgWaits=0, g_ffStgFallback=0, g_ffCopyUs=0, g_ffCopyB=0;
static uint32_t g_ffOccMax=0;                // max ring occupancy (bytes)
// Texture staging buffer, in HOST memory (not in the guest arena).
// D2_FLUSHFIL_STG = size in MiB (default 4, forced to a power of two).
// 4 MiB costs ~3.3 MB of the newlib heap, taking its room to grow from 11.1 to
// 8.6 MB. 2 MiB was measured on console (patrol, 6200 frames): same frames/s
// (24.00 vs 23.92, noise) but it recovers only ~500 KB at the high-water mark
// and it produced one `attentes-tampon` stall where 4 MiB produced none. Kept
// at 4; drop to 2 via the knob if heap pressure ever becomes the binding
// constraint (see the ALLOC FAIL / heap-ceiling history).
static uint8_t*  g_ffStg=nullptr;
static uint32_t  g_ffStgSize=0, g_ffStgMask=0;
static volatile uint32_t g_ffStgHead=0, g_ffStgTail=0;   // monotonic byte counters
// ---- flush thread platform glue (same pattern as replay60.cpp) -------------
static bool ff_thread_main();
#ifdef __vita__
static void ff_sleep_us(unsigned us){ if(us) sceKernelDelayThread((SceUInt)us); }
static void ff_pin_self(){
    // Thread's core: D2_FLUSHFIL_CPU (default 1 = USER_1, the FREE core — the
    // game is on USER_0, presenter/replay on USER_2 by default, see D2_COEURS).
    static const int kMaskU3=0x00080000;
    const char* e=getenv("D2_FLUSHFIL_CPU"); int cpu = (e&&*e)? atoi(e) : 1; if(cpu<0||cpu>3) cpu=1;
    const int mask = cpu==0?0x10000 : cpu==1?0x20000 : cpu==3?kMaskU3 : 0x40000;
    unsigned relu=0; const int rc=d2vita_pin_self(mask,&relu);
    d2vita_core_register("flushfil",(int)sceKernelGetThreadId(),(unsigned)mask,rc);
    jpline("flushfil: fil parti, epinglage masque=0x%x rc=0x%08x relu=0x%x",(unsigned)mask,(unsigned)rc,relu);
}
static int ff_vita_tramp(SceSize,void*){ ff_thread_main(); return 0; }
static bool ff_spawn(){
    SceUID th=sceKernelCreateThread("d2_flushfil",ff_vita_tramp,0x10000100,128*1024,0,0,nullptr);
    if(th<0) return false;
    return sceKernelStartThread(th,0,nullptr)>=0;
}
#else
static void ff_sleep_us(unsigned us){ if(!us) return; struct timespec ts;
    ts.tv_sec=(time_t)(us/1000000u); ts.tv_nsec=(long)((us%1000000u)*1000u); nanosleep(&ts,nullptr); }
static void ff_pin_self(){ jpline("flushfil: fil parti (pthread)"); }
static void* ff_pth_tramp(void*){ ff_thread_main(); return nullptr; }
static bool ff_spawn(){
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at,256*1024);
    return pthread_create(&th,&at,ff_pth_tramp,nullptr)==0 && (pthread_detach(th),true);
}
#endif
// Waits for the flush thread to finish the handed-off batch. Called by the GAME thread.
static void ff_wait_idle(){
    if(!g_ffPending) return;
    const uint64_t t0=rt_now_us();
    while(g_ffPending){ for(int i=0;i<200 && g_ffPending;i++) __sync_synchronize(); if(g_ffPending) ff_sleep_us(50); }
    const uint64_t d=rt_now_us()-t0;
    ++g_ffWaits; g_ffWaitUs+=d; if(d>g_ffWaitMax) g_ffWaitMax=d;
}
// Hands off a batch [tail, head) to the flush thread. Game thread.
static void ff_handoff(uint32_t head){
    const uint32_t occ = head-g_grTail; if(occ>g_ffOccMax) g_ffOccMax=occ;
    g_ffHead=head; __sync_synchronize(); g_ffPending=1; ++g_ffLots;
}
// Drains IMMEDIATELY up to the guest's current head, then waits. Used when
// the texture staging buffer or the ring itself fills up mid-frame (loading
// screens): the walk is a STREAM consumer, a batch boundary in the middle of
// a frame doesn't change what it produces — only the frame (FRAME_END) closes
// a frame.
static void ff_drain_now(d2rt::Cpu& c){
    ff_wait_idle();
    const uint32_t head=c.read_u32(g_grHdrVA+16);
    if((int32_t)(head-g_grTail)>0){ ff_handoff(head); ff_wait_idle(); }
}
// Copies a texture's bytes into the staging buffer. Returns the ABSOLUTE
// offset (byte counter), or 0xFFFFFFFF if there's no room even after a full
// drain (a texture bigger than the buffer: impossible given gx_tex_upload's
// 256x256 = 64KiB bound, but not assumed here).
static uint32_t ff_stg_put(d2rt::Cpu& c, const uint8_t* p, uint32_t nb){
    const uint32_t need=(nb+3u)&~3u;
    if(need>g_ffStgSize) return 0xFFFFFFFFu;
    for(int attempt=0; attempt<3; ++attempt){
        uint32_t head=g_ffStgHead; const uint32_t tail=g_ffStgTail;
        uint32_t off=head&g_ffStgMask;
        uint32_t pad = (off+need>g_ffStgSize)? (g_ffStgSize-off) : 0u;
        if((head-tail)+pad+need<=g_ffStgSize){
            head+=pad; off=head&g_ffStgMask;
            const uint64_t t0=rt_now_us();
            std::memcpy(g_ffStg+off,p,nb);
            g_ffCopyUs+=rt_now_us()-t0; g_ffCopyB+=nb;
            __sync_synchronize(); g_ffStgHead=head+need;
            return head; }
        ++g_ffStgWaits; ff_drain_now(c);
    }
    return 0xFFFFFFFFu;
}
// Upload to the atlas: hash, copy, bind. CONSUMER side (flush thread when
// async, game thread when sync). `emit` non-null = sync mode: binding goes
// through a TEXBIND record, read later at its place in the stream. In async
// mode we ARE at that place: bind directly.
static void gx_tex_apply(uint32_t tmuAddr, uint32_t w, uint32_t h, uint32_t nb,
                         const uint8_t* p, d2rt::Cpu* emit){
    // `uploads-us` stays the same line item: in sync mode it's timed by
    // gx_tex_upload (game thread), in async mode HERE (flush thread).
    const uint64_t ta = emit? 0ull : rt_now_us();
    if(grdup_on()) g_grDupGen[0x10]=0;   // rebind: the next grTexSource isn't "redundant" (mirrors the DLL)
    ++g_gxTexHashN; g_gxTexHashB+=nb;
    const bool prof=grprof_on();
    const uint64_t th0 = prof? rt_now_us() : 0;
    const uint64_t hsh=tex_hash(p,nb,w,h);
    if(prof) g_grpTexHashUs += rt_now_us()-th0;
    const int32_t ci=g_gxAtlas.upload(hsh,tmuAddr,w,h,p,g_gxFrame);
    if(ci>=0 && bindorder_on()){
        if(emit){ const uint32_t rec[3]={tmuAddr,(uint32_t)ci,g_gxAtlas.cell(ci).gen}; gr_emit(*emit,D2GR_OP_TEXBIND,rec,3); }
        else      g_gxBindAt[tmuAddr]=GxBind{ ci, g_gxAtlas.cell(ci).gen };
    }
    if(!emit) g_gxTexUs += rt_now_us()-ta;
}
// Uploads a P8 texture to the atlas. Called from d2vGlideTexUpload (the ONLY
// per-texture crossing on the ring path). Hashing is done on the ACTUALLY
// pointed-to content: that's what allows most redundant copies to be
// skipped.
static void gx_tex_upload_inner(d2rt::Cpu& c, uint32_t tmuAddr, uint32_t info);
void gx_tex_upload(d2rt::Cpu& c, uint32_t tmuAddr, uint32_t info){
    const uint64_t t0=rt_now_us();
    gx_tex_upload_inner(c,tmuAddr,info);
    if(!g_ffArmed) g_gxTexUs += rt_now_us()-t0;   // async: on the game side it's g_ffCopyUs, on the thread side it's gx_tex_apply
}
static void gx_tex_upload_inner(d2rt::Cpu& c, uint32_t tmuAddr, uint32_t info){
    gx_lazy_init();
    if(!g_ffArmed) g_gxStDirty=true;   // the atlas may bind/evict: resolve() must be replayed
                                       // (in async mode, it's the TEXUP record, read AT ITS PLACE, that does it)
    if(g_gxDead || !g_gxAtlas.ready() || !info) return;
    const uint32_t lod=c.read_u32(info+4);
    const int32_t  ar =(int32_t)c.read_u32(info+8);
    const uint32_t fmt=c.read_u32(info+12);
    const uint32_t src=c.read_u32(info+16);
    if(lod>8 || !src) return;
    if(fmt!=5){ ++g_gxFmtOther; return; }        // only P_8 is handled; the rest is COUNTED, not guessed
    uint32_t big=1u<<lod, w=big, h=big;
    if(ar>0 && ar<=3) h=big>>ar; else if(ar<0 && ar>=-3) w=big>>(-ar);
    const uint32_t nb=w*h;
    if(!nb || nb>(256u*256u)) return;
    static std::vector<uint8_t> tmp;
    const uint8_t* p=(const uint8_t*)c.hostptr(src,nb);
    if(!p){ tmp.resize(nb); c.read(src,tmp.data(),nb); p=tmp.data(); }
    if(!g_ffArmed){ gx_tex_apply(tmuAddr,w,h,nb,p,&c); return; }
    // --- async: copy + record, the work goes to the flush thread
    const uint32_t off=ff_stg_put(c,p,nb);
    if(off==0xFFFFFFFFu){                        // no room found: fall back IN PLACE, flush thread stalled
        ++g_ffStgFallback; ff_drain_now(c); g_gxStDirty=true; gx_tex_apply(tmuAddr,w,h,nb,p,&c); return; }
    const uint32_t rec[5]={tmuAddr,w,h,off,nb};
    if(!gr_emit(c,D2GR_OP_TEXUP,rec,5)){         // ring full: drain and retry once
        ff_drain_now(c);
        if(!gr_emit(c,D2GR_OP_TEXUP,rec,5)){
            // Nobody will come free these staged bytes: we reclaim them
            // ourselves (the flush thread is stalled, and the buffer's head
            // has only one writer — us). Without this the buffer would leak
            // on every drop.
            ++g_ffStgFallback; g_ffStgHead=off; } }
}
// A STATE record -> the GPU path's current state. `rp` = host view of the
// record (word 0 = header).
static void gx_state(uint32_t op, uint32_t len, const uint32_t* rp){
    uint32_t a[8]={0,0,0,0,0,0,0,0};
    for(uint32_t i=1;i<len && i<8;i++) a[i]=rp[i];
    g_gxStDirty=true;
    switch(op){
      case 0x10:                                  // grTexSource
        if(a[1]==0){ g_gxTexAddr=a[3]; g_gxLod=a[5]; g_gxAspect=(int32_t)a[6]; g_gxFmt=a[7]; }
        break;
      case D2GR_OP_TEXBIND:                       // ordered binding (emitted by the host)
        g_gxBindAt[a[1]] = GxBind{ (int32_t)a[2], a[3] };
        break;
      case D2GR_OP_TEXUP: {                       // OFFLOADED upload (D2_FLUSHFIL): emitted by the host at
        // crossing time, executed HERE — the exact place the TEXBIND occupies
        // under synchronous submission. The bytes are in the HOST staging
        // buffer; they are freed as soon as the atlas has copied them.
        const uint32_t tmu=a[1], w=a[2], h=a[3], off=a[4], nb=a[5];
        if(g_ffStg && nb) gx_tex_apply(tmu,w,h,nb,g_ffStg+(off&g_ffStgMask),nullptr);
        __sync_synchronize(); g_ffStgTail = off + ((nb+3u)&~3u);
        break; }
      case 0x14: g_gxBlendSf=a[1]; g_gxBlendDf=a[2]; break;      // grAlphaBlendFunction
      case 0x15: g_gxAcFunc=a[1]; break;                          // grAlphaCombine
      case 0x16: g_gxCcFunc=a[1]; g_gxCcOther=a[4]; break;        // grColorCombine
      case 0x17: g_gxCst=a[1]; break;                             // grConstantColorValue
      case 0x18: g_gxChroma=a[1]; break;                          // grChromakeyMode
      case 0x12: g_gxFilter=a[2]; break;                          // grTexFilterMode (min)
      case 0x25:                                                  // grSstWinOpen
        { static const uint16_t RW[16]={320,320,400,512,640,640,640,640,800,960,856,512,1024,1280,1600,400};
          static const uint16_t RH[16]={200,240,256,384,200,350,400,480,600,720,480,256, 768,1024,1200,300};
          // D2 ne demande jamais que l'index 7 (640x480) ou 8 (800x600). Avec
          // D2_RES arme, on ouvre la taille demandee par l'override quel que
          // soit l'index : c'est legitime parce que NOUS sommes le pilote
          // Glide (meme modele que D2DX), et les crochets de
          // native_hooks_resolution ont deja mis les globals du jeu d'accord.
          // Sans l'override (defaut), rien ne change.
          if(d2res_active && d2res_active()){
              g_gxResW=(uint16_t)d2res_w(); g_gxResH=(uint16_t)d2res_h();
              r60_gxm_lock(); d2gxm_set_window((int)g_gxResW,(int)g_gxResH); r60_gxm_unlock(); }
          else if(a[1]<16){ g_gxResW=RW[a[1]]; g_gxResH=RH[a[1]];
              r60_gxm_lock(); d2gxm_set_window((int)g_gxResW,(int)g_gxResH); r60_gxm_unlock(); } }
        break;
      case 0x30: g_gxClear=a[1]; break;                           // grBufferClear
      case 0x24: {                                                // grTexDownloadTable (palette)
        if(a[1]!=2) break;                                        // only the PALETTE type exists (checked)
        uint32_t pal[256]; uint64_t h=1469598103934665603ull;
        for(uint32_t i=0;i<256;i++){ pal[i]=rp[2+i]; h^=pal[i]; h*=1099511628211ull; }
        if(g_gxPalHash[g_gxPalSlot]==h) break;                    // already in place: nothing to do
        // Next slot, round-robin. Two palettes alive in the SAME frame must
        // coexist: that's why the slot number travels in the VERTEX rather
        // than in a batch uniform.
        for(int i=0;i<D2GXM_PALETTES;i++) if(g_gxPalHash[i]==h){ g_gxPalSlot=i; return; }
        g_gxPalSlot=(g_gxPalSlot+1)%D2GXM_PALETTES;
        g_gxPalHash[g_gxPalSlot]=h;
        r60_gxm_lock(); d2gxm_palette(g_gxPalSlot,pal); r60_gxm_unlock();
        break; }
      default: break;
    }
}

// ============================================================================
// D2_CACHEPROBE — MEASURES (and nothing else) WHAT A CACHE COULD SAVE
// ----------------------------------------------------------------------------
// Can work be avoided by noticing that a frame, a batch, a draw, or a phase
// REPEATS from one frame to the next? This probe caches NOTHING: it counts.
// Three granularities, two metrics:
//   * DRAW  (the unit the GUEST emits: roughly 1300-1800 per frame on console)
//   * BATCH (the unit the GPU receives after merging: 350-1000 per frame)
//   * FRAME (the whole built stream)
// and for each: identical in ABSOLUTE terms (same bytes) or identical up to a
// TRANSLATION (same bytes once x,y are brought back to camera space,
// [0x7a520c]/[0x7a5208] published by the CAMERA record — so this only makes
// sense with D2_RINGTAG=1 or D2_REPLAY60=1; without them offX=offY=0 and the
// relative figure equals the absolute one, which the line states).
// The GROUND is identified via bilinear filtering (`flags&2`): D2 only
// requests grTexFilterMode(LINEAR) for DT1 floor tiles.
// Cost: two FNV per vertex and one hash table per frame. Not a play mode —
// a measurement leg.
// ============================================================================
static int g_cpMode=-1;
static inline bool cacheprobe_on(){
    if(g_cpMode<0){ const char* e=getenv("D2_CACHEPROBE"); g_cpMode=(e&&*e)?atoi(e):0; }
    return g_cpMode!=0; }
// Three hashes per unit, to separate THREE causes of non-repetition:
//   abs  = everything, x,y included    -> "the same draw, in the same place"
//   shp  = x,y relative to the draw's FIRST VERTEX, color included
//          -> "the same draw, up to a translation"
//   shpG = same, WITHOUT vertex color
//          -> "the same GEOMETRY, up to a translation" (isolates lighting)
// x0,y0 keep the first vertex's absolute position: over pairs matched by
// `shp`, the distribution of (x0-x0', y0-y0') tells whether the scene moved
// by a UNIFORM VECTOR (the floor-cache bet).
struct CPItem { uint64_t abs, shp, shpG; float x0, y0; uint32_t verts; uint32_t cst; int32_t cell; uint8_t floorTile, q256; };
static std::vector<CPItem> g_cpDrawCur, g_cpDrawPrev;
static std::vector<CPItem> g_cpLotCur,  g_cpLotPrev;
static uint64_t g_cpFrameAbsPrev=0;
static int32_t  g_cpOffX=0, g_cpOffY=0, g_cpOffXPrev=0, g_cpOffYPrev=0;
// Counters are doubled: [0] = frames where the CAMERA DIDN'T MOVE, [1] =
// frames where it did. This is the only way to read the probe: in real play
// the player walks, and an "identical" reading with a fixed camera says
// nothing about what happens when it moves. Each counter is published as a
// window DELTA (a running total would mix menus, loading, and play).
struct CPBucket {
    uint64_t frames, frameSameA, frameSameR;
    uint64_t d, dA, dR, dv, dvA, dvR;          // draws / verts
    uint64_t f, fA, fR, fv, fvA, fvR;          // FLOOR draws (flags&2)
    uint64_t l, lA, lR, lv, lvA, lvR;          // batches
    uint64_t bytes;
    uint64_t q256, q256A, q256R;   // 256x128px draws (cross-check for the FLOOR classifier)
    uint64_t dG, fG;               // matched by GEOMETRY alone (color excluded)
    uint64_t pair, pairMode, movedFrames; int64_t modeDx, modeDy;  // uniform translation
};
static CPBucket g_cpB[2], g_cpBPrev[2];
static RPhase g_rphPrev[8];
static uint64_t g_cpFrames=0;
static inline uint64_t cp_mix(uint64_t h,uint32_t w){ h^=w; return h*1099511628211ull; }
#define CP_FNV0 1469598103934665603ull
// Current draw's state, filled by gx_draw (both hashes are built inside
// the vertex loop that's already being walked: no re-reading).
static uint64_t g_cpDrawA=0, g_cpDrawS=0, g_cpDrawG=0; static uint32_t g_cpDrawV=0;
static float g_cpX0=0,g_cpY0=0; static bool g_cpFirst=false;
static uint64_t g_cpFrameBytes=0;
static int g_cpDumpF=-2, g_cpDumpN=0, g_cpDump2=-1, g_cpDump2N=0, g_cpLastBk=0;
// A draw record -> triangles. `vh` = host view of the FIRST vertex (the
// following cnt*stride bytes are contiguous: a record never straddles the
// end of the ring).
static void gx_draw(uint32_t mode, uint32_t cnt, uint32_t stride, const uint8_t* vh){
    if(!g_gxAtlas.ready() || !cnt || cnt>4096) return;
    const int oxy=g_grVLoff[0x01], oargb=g_grVLoff[0x30], ost0=g_grVLoff[0x40];
    if(oxy<0 || ost0<0) return;
    const bool prof=grprof_on();
    const uint64_t tS = prof? rt_now_us() : 0;
    gx_sync_for_draw();
    const uint64_t tV = prof? rt_now_us() : 0;
    if(prof) g_grpSyncUs += tV-tS;
    g_gxBuild.begin(g_gxSt,g_gxAtlas);
    if(g_gxSt.cell>=0) g_gxAtlas.touchCell(g_gxSt.cell,g_gxFrame);
    // D2_GXMLOTS=<frame>: dumps ALL draws of that frame (and the next 2) with
    // the actual s,t range against the sNorm/tNorm convention and the cell.
    // Diagnoses texel noise on the ground with sprites intact: if s or t falls
    // outside [0,sNorm]x[0,tNorm], the uv escapes the cell and the GPU samples
    // neighboring cells = noise. Portable: measurable under qemu.
    static int lotsFrom=-2; if(lotsFrom==-2){ const char* e=getenv("D2_GXMLOTS"); lotsFrom=(e&&*e)?atoi(e):-1; }
    const bool dumpLots = lotsFrom>=0 && g_gxFrame>=(uint64_t)lotsFrom && g_gxFrame<(uint64_t)lotsFrom+3;
    float sMin=1e30f,sMax=-1e30f,tMin=1e30f,tMax=-1e30f;
    const float sx=(float)960.0f/(float)g_gxResW, sy=(float)544.0f/(float)g_gxResH;
    (void)sx;(void)sy;
    const float invDim = g_gxAtlas.dim()? 1.0f/(float)g_gxAtlas.dim() : 0.0f;
    uint16_t idx[4096];
    const uint32_t n = cnt<4096?cnt:4096;
    // Coordinates stay in the game's WINDOW space: the 960x544 scaling is done
    // by the vertex shader's matrix (uScale), not here — otherwise it would be
    // recomputed thousands of times per frame.
    d2gr::VtxPre pre; g_gxBuild.prep(pre,g_gxSt,g_gxAtlas,invDim);
    // Reads one word of vertex i: host view (one ldr).
    auto ld=[&](uint32_t i,int off)->uint32_t{
        uint32_t w; std::memcpy(&w, vh+(size_t)i*stride+(uint32_t)off, 4); return w; };
    // Raw vertices of the first two points: Glide LINES and POINTS
    // (grDrawLine/grDrawPoint: unit crosses on the map) are rendered as thin
    // quads — see case 0xFF below.
    float lx[2]={0,0},ly[2]={0,0},ls[2]={0,0},lt[2]={0,0}; uint32_t lc[2]={0,0};
    const bool cprobe = cacheprobe_on();
    if(cprobe){
        // The DRAW's hash: the state that decorates it + the raw vertices.
        uint64_t h=CP_FNV0;
        h=cp_mix(h,mode); h=cp_mix(h,cnt); h=cp_mix(h,stride);
        h=cp_mix(h,(uint32_t)g_gxSt.blend|((uint32_t)g_gxSt.flags<<8)|((uint32_t)g_gxSt.pal<<16));
        h=cp_mix(h,(uint32_t)(int32_t)g_gxSt.cell);
        h=cp_mix(h,g_gxSt.sNorm); h=cp_mix(h,g_gxSt.tNorm);
        // PURE GEOMETRY: without the constant color (= the tile's LIGHTING,
        // reset on every draw) and without vertex color. This is what tells
        // whether the scene is REDRAWN geometrically identical.
        g_cpDrawG=h;
        h=cp_mix(h,g_gxSt.cst);
        g_cpDrawA=h; g_cpDrawS=h; g_cpDrawV=n; g_cpFirst=false;
    }
    float cpXmin=1e30f,cpXmax=-1e30f,cpYmin=1e30f,cpYmax=-1e30f;
    for(uint32_t i=0;i<n;i++){
        union{uint32_t u;float f;} X,Y,S,T;
        X.u=ld(i,oxy); Y.u=ld(i,oxy+4); S.u=ld(i,ost0); T.u=ld(i,ost0+4);
        const uint32_t col = oargb>=0? ld(i,oargb) : 0xFFFFFFFFu;
        if(i<2){ lx[i]=X.f; ly[i]=Y.f; ls[i]=S.f; lt[i]=T.f; lc[i]=col; }
        idx[i]= g_gxBuild.vertexPre(pre,X.f,Y.f,col,S.f,T.f);
        if(dumpLots){ if(S.f<sMin)sMin=S.f; if(S.f>sMax)sMax=S.f; if(T.f<tMin)tMin=T.f; if(T.f>tMax)tMax=T.f; }
        if(cprobe){
            if(!g_cpFirst){ g_cpFirst=true; g_cpX0=X.f; g_cpY0=Y.f; }
            g_cpDrawA=cp_mix(cp_mix(cp_mix(cp_mix(g_cpDrawA,X.u),Y.u),S.u),T.u);
            union{float f;uint32_t u;} RX,RY; RX.f=X.f-g_cpX0; RY.f=Y.f-g_cpY0;
            g_cpDrawS=cp_mix(cp_mix(cp_mix(cp_mix(g_cpDrawS,RX.u),RY.u),S.u),T.u);
            g_cpDrawG=cp_mix(cp_mix(cp_mix(cp_mix(g_cpDrawG,RX.u),RY.u),S.u),T.u);
            // The vertex color is only read by the fragment shader if
            // `flags&4`: outside of that, it's guest-side noise and would
            // make the probe lie.
            if(g_gxSt.flags&4){ const uint32_t cu=col&0x00FFFFFFu; g_cpDrawA=cp_mix(g_cpDrawA,cu); g_cpDrawS=cp_mix(g_cpDrawS,cu); }
            if(X.f<cpXmin)cpXmin=X.f; if(X.f>cpXmax)cpXmax=X.f;
            if(Y.f<cpYmin)cpYmin=Y.f; if(Y.f>cpYmax)cpYmax=Y.f;
        }
    }
    if(cprobe && g_cpDump2>=-1){
        if(g_cpDump2==-1){ const char* e=getenv("D2_CACHEDUMP2"); g_cpDump2=(e&&*e)?atoi(e):-2; }
    }
    if(cprobe && g_cpDump2>=0 && (int)g_gxFrame>=g_cpDump2 && (g_gxSt.flags&2) && g_cpDump2N<80){
        ++g_cpDump2N; char b2[256]; int q=std::snprintf(b2,sizeof b2,"cpv: f=%llu cell=%d cst=%08x sn=%u tn=%u:",
            (unsigned long long)g_gxFrame,(int)g_gxSt.cell,g_gxSt.cst,g_gxSt.sNorm,g_gxSt.tNorm);
        for(uint32_t i=0;i<n&&i<6;i++){ union{uint32_t u;float f;} X,Y,S,T;
            X.u=ld(i,oxy); Y.u=ld(i,oxy+4); S.u=ld(i,ost0); T.u=ld(i,ost0+4);
            q+=std::snprintf(b2+q,sizeof b2-q," (%.2f,%.2f;%.2f,%.2f)",X.f,Y.f,S.f,T.f); }
        std::printf("%s\n",b2);
    }
    if(cprobe){
        CPItem it; it.abs=g_cpDrawA; it.shp=g_cpDrawS; it.shpG=g_cpDrawG; it.x0=g_cpX0; it.y0=g_cpY0; it.verts=g_cpDrawV;
        it.cst=g_gxSt.cst; it.cell=g_gxSt.cell;
        it.floorTile=(g_gxSt.flags&2)?1:0;
        // "256x128 draws" refers to the BOUND TEXTURE (lod 8, aspect ratio 1
        // = 256x128), not the on-screen quad (a DT1 tile covers 160x80px).
        // This is the FLOOR classifier's cross-check.
        it.q256=(g_gxLod==8 && g_gxAspect==1)?1:0;
        (void)cpXmin;(void)cpXmax;(void)cpYmin;(void)cpYmax;
        if(g_cpDrawCur.size()<65536) g_cpDrawCur.push_back(it);
        g_cpFrameBytes += (uint64_t)cnt*stride;
    }
    if(prof) g_grpVtxUs += rt_now_us()-tV;
    if(dumpLots){
        int cw=0,ch=0,cx=0,cy=0,cp=-1;
        if(g_gxSt.cell>=0){ const d2gr::Cell& ce=g_gxAtlas.cell(g_gxSt.cell); cw=ce.w; ch=ce.h; cx=ce.x; cy=ce.y; cp=ce.page; }
        const bool out = (sMin<-0.01f || tMin<-0.01f || sMax>(float)g_gxSt.sNorm+0.01f || tMax>(float)g_gxSt.tNorm+0.01f);
        std::printf("gxlot: f=%llu mode=%u n=%u tmu=0x%x lod=%u ar=%d cell=%d page=%d xy=(%d,%d) wh=(%d,%d) norm=(%u,%u) s=[%.1f,%.1f] t=[%.1f,%.1f] pal=%u blend=%u flags=0x%x%s\n",
            (unsigned long long)g_gxFrame,mode,cnt,g_gxTexAddr,g_gxLod,(int)g_gxAspect,g_gxSt.cell,cp,cx,cy,cw,ch,
            g_gxSt.sNorm,g_gxSt.tNorm,sMin,sMax,tMin,tMax,(unsigned)g_gxSt.pal,(unsigned)g_gxSt.blend,(unsigned)g_gxSt.flags,
            out?" HORS-CELLULE":"");
    }
    switch(mode){
      case 3: case 5:                                    // POLYGON / TRIANGLE_FAN
        for(uint32_t i=1;i+1<n;i++) g_gxBuild.tri(idx[0],idx[i],idx[i+1]); break;
      case 4:                                            // TRIANGLE_STRIP
        for(uint32_t i=0;i+2<n;i++) g_gxBuild.tri(idx[i],idx[i+1],idx[i+2]); break;
      case 6:                                            // TRIANGLES
        for(uint32_t i=0;i+2<n;i+=3) g_gxBuild.tri(idx[i],idx[i+1],idx[i+2]); break;
      case 0xFF: {                                       // grDrawLine (n=2) / grDrawPoint (n=1)
        // These are the map overlay's unit-position crosses. Rendered as a
        // quad about 1.5 window px wide (the vertex keeps its color, s,t, and
        // batch state). D2_GXLINES=0 reverts to ignoring them.
        static int lines=-1; if(lines<0){ const char* e=getenv("D2_GXLINES"); lines=(e&&*e&&!strcmp(e,"0"))?0:1; }
        if(!lines || n<1){ ++g_gxSkipMode; break; }
        const float hw=0.75f;
        float x0=lx[0],y0=ly[0],x1=(n>=2)?lx[1]:lx[0],y1=(n>=2)?ly[1]:ly[0];
        float dx=x1-x0, dy=y1-y0; const float L=std::sqrt(dx*dx+dy*dy);
        float px,py;
        if(L<0.5f){ x1=x0+1.5f; y1=y0; px=0.0f; py=hw; }            // point: small square
        else { px=-dy/L*hw; py=dx/L*hw; }
        const uint16_t a=g_gxBuild.vertexPre(pre,x0+px,y0+py,lc[0],ls[0],lt[0]);
        const uint16_t b=g_gxBuild.vertexPre(pre,x0-px,y0-py,lc[0],ls[0],lt[0]);
        const uint16_t c=g_gxBuild.vertexPre(pre,x1-px,y1-py,lc[n>=2?1:0],ls[n>=2?1:0],lt[n>=2?1:0]);
        const uint16_t d=g_gxBuild.vertexPre(pre,x1+px,y1+py,lc[n>=2?1:0],ls[n>=2?1:0],lt[n>=2?1:0]);
        g_gxBuild.tri(a,b,c); g_gxBuild.tri(a,c,d);
        ++g_gxLinesDrawn; break; }
      default: ++g_gxSkipMode; break;                    // other modes: never emitted by D2
    }
}
// HASH OF THE BUILT BATCHES (D2_GXMLOTSHASH=1). The ring's `h=` proves the
// host READ the same sequence of records; it says nothing about what it DID
// with them. This hash covers exactly what goes to the GPU: vertex bytes (x,
// y, u, v, argb, pal), indices, and each batch's fields. Two walks that
// produce the same `lh=` have produced the bit-identical GPU stream — this is
// the proof required to change a default.
// USEFUL HASH (`lu=`, same knob as `lh=`). `lh=` hashes vertex bytes AS-IS,
// `argb` included. But vita_gxm.cpp's fragment shader does:
//     rgb = lerp(uConst.xyz, texel.xyz * vCol.xyz, uMode.x)   uMode.x = flags&4
//     a   = uConst.w * uMode.z
// so vertex color is only read by batches with `flags&4`, and its ALPHA is
// NEVER read. D2 leaves that word UNINITIALIZED in its vertex array for draws
// that don't use it — which can read back as leftover guest stack data (e.g.
// an arena address). `lh=` is therefore sensitive to GUEST-SIDE NOISE: any DLL
// change that shifts the x86 stack moves it with nothing visibly changing.
// `lu=` masks that word — argb zeroed outside `flags&4` batches, alpha always
// stripped — and therefore covers ONLY bytes the GPU actually reads. THIS is
// the oracle for a DLL change; `lh=` is still published and remains the
// oracle for a HOST change (with an identical DLL, there's no more noise and
// it's stricter).
static std::vector<uint8_t> g_gxVColUsed;
static void gx_lots_hash(){
    uint64_t h=g_gxLotsHash;
    auto mix=[&](const void* p,size_t n){ const uint8_t* b=(const uint8_t*)p; size_t i=0;
        for(; i+4<=n; i+=4){ uint32_t w; std::memcpy(&w,b+i,4); h^=w; h*=1099511628211ull; }
        for(; i<n; i++){ h^=b[i]; h*=1099511628211ull; } };
    mix(g_gxBuild.vdata(), (size_t)g_gxBuild.verts()*sizeof(d2gr::Vtx));
    mix(g_gxBuild.idata(), (size_t)g_gxBuild.indices()*sizeof(uint16_t));
    const d2gr::Batch* b=g_gxBuild.bdata();
    for(uint32_t k=0;k<g_gxBuild.batches();k++){
        const uint32_t f[4]={b[k].first,b[k].count,(uint32_t)(int32_t)b[k].page,
                             ((uint32_t)b[k].blend)|((uint32_t)b[k].flags<<8)|((uint32_t)b[k].pal<<16)};
        mix(f,sizeof f); mix(&b[k].cst,4); }
    h^=g_gxClear; h*=1099511628211ull;
    // --- `lu=`: the same bytes, minus guest-side noise ---------------
    { uint64_t u=g_gxLotsHashU;
      auto mixu=[&](const void* p,size_t n){ const uint8_t* bb=(const uint8_t*)p; size_t i=0;
          for(; i+4<=n; i+=4){ uint32_t w; std::memcpy(&w,bb+i,4); u^=w; u*=1099511628211ull; }
          for(; i<n; i++){ u^=bb[i]; u*=1099511628211ull; } };
      const uint32_t nv=g_gxBuild.verts(), ni=g_gxBuild.indices(), nbt=g_gxBuild.batches();
      g_gxVColUsed.assign(nv,0);
      const uint16_t* ix=g_gxBuild.idata();
      for(uint32_t k=0;k<nbt;k++){ if(!(b[k].flags&4)) continue;
          const uint32_t e=b[k].first+b[k].count<=ni? b[k].first+b[k].count : ni;
          for(uint32_t q=b[k].first;q<e;q++){ const uint16_t vi=ix[q]; if(vi<nv) g_gxVColUsed[vi]=1; } }
      const d2gr::Vtx* vv=g_gxBuild.vdata();
      for(uint32_t q=0;q<nv;q++){
          const uint32_t w[5]={ *(const uint32_t*)&vv[q].x, *(const uint32_t*)&vv[q].y,
                                *(const uint32_t*)&vv[q].u, *(const uint32_t*)&vv[q].v,
                                *(const uint32_t*)&vv[q].pal };
          mixu(w,sizeof w);
          const uint32_t col = g_gxVColUsed[q]? (vv[q].argb & 0x00FFFFFFu) : 0u;
          mixu(&col,4); }
      mixu(ix,(size_t)ni*sizeof(uint16_t));
      for(uint32_t k=0;k<nbt;k++){
          const uint32_t f[4]={b[k].first,b[k].count,(uint32_t)(int32_t)b[k].page,
                               ((uint32_t)b[k].blend)|((uint32_t)b[k].flags<<8)|((uint32_t)b[k].pal<<16)};
          mixu(f,sizeof f); mixu(&b[k].cst,4); }
      u^=g_gxClear; u*=1099511628211ull;
      g_gxLotsHashU=u; }
    if(lotshash_mode()==2){
        uint64_t hf=1469598103934665603ull; std::swap(h,hf);   // h = FNV seed, hf = pending accumulator
        mix(g_gxBuild.vdata(), (size_t)g_gxBuild.verts()*sizeof(d2gr::Vtx));
        mix(g_gxBuild.idata(), (size_t)g_gxBuild.indices()*sizeof(uint16_t));
        for(uint32_t k=0;k<g_gxBuild.batches();k++){
            const uint32_t f[4]={b[k].first,b[k].count,(uint32_t)(int32_t)b[k].page,
                                 ((uint32_t)b[k].blend)|((uint32_t)b[k].flags<<8)|((uint32_t)b[k].pal<<16)};
            mix(f,sizeof f); mix(&b[k].cst,4); }
        std::printf("gxlh: f=%llu v=%u i=%u b=%u clear=%08x lh=0x%016llx rh=0x%016llx lu=0x%016llx\n",(unsigned long long)g_gxFrame,
            g_gxBuild.verts(),g_gxBuild.indices(),g_gxBuild.batches(),g_gxClear,(unsigned long long)h,
            (unsigned long long)g_grHash,(unsigned long long)g_gxLotsHashU);
        h=hf; }
    g_gxLotsHash=h; ++g_gxLotsHashN;
}

// Hashes of the built BATCHES + frame-to-frame comparison. Called at
// FRAME_END, before the builder is reset.
void cp_set_camera(int32_t ox,int32_t oy){ g_cpOffX=ox; g_cpOffY=oy; }
static void gx_cache_probe(){
    if(g_cpDumpF==-2){ const char* e=getenv("D2_CACHEDUMP"); g_cpDumpF=(e&&*e)?atoi(e):-1; }
    const d2gr::Vtx* vv=g_gxBuild.vdata(); const uint16_t* ix=g_gxBuild.idata();
    const d2gr::Batch* bb=g_gxBuild.bdata();
    const uint32_t nb=g_gxBuild.batches(), ni=g_gxBuild.indices(), nv=g_gxBuild.verts();
    g_cpLotCur.clear();
    for(uint32_t k=0;k<nb;k++){
        uint64_t ha=CP_FNV0, hs=CP_FNV0; uint32_t vc=0; float x0=0,y0=0; bool first=false;
        const uint32_t e=(bb[k].first+bb[k].count<=ni)? bb[k].first+bb[k].count : ni;
        for(uint32_t q=bb[k].first;q<e;q++){
            const uint16_t vi=ix[q]; if(vi>=nv) continue; ++vc;
            const d2gr::Vtx& v=vv[vi];
            if(!first){ first=true; x0=v.x; y0=v.y; }
            union{float f;uint32_t u;} X,Y,U,V,P,RX,RY;
            X.f=v.x; Y.f=v.y; U.f=v.u; V.f=v.v; P.f=v.pal; RX.f=v.x-x0; RY.f=v.y-y0;
            const uint32_t cu=(bb[k].flags&4)? (v.argb&0x00FFFFFFu):0u;
            ha=cp_mix(cp_mix(cp_mix(cp_mix(cp_mix(cp_mix(ha,X.u),Y.u),U.u),V.u),P.u),cu);
            hs=cp_mix(cp_mix(cp_mix(cp_mix(cp_mix(cp_mix(hs,RX.u),RY.u),U.u),V.u),P.u),cu);
        }
        const uint32_t f[3]={ bb[k].count,(uint32_t)(int32_t)bb[k].page,
            ((uint32_t)bb[k].blend)|((uint32_t)bb[k].flags<<8)|((uint32_t)bb[k].pal<<16) };
        for(int j=0;j<3;j++){ ha=cp_mix(ha,f[j]); hs=cp_mix(hs,f[j]); }
        ha=cp_mix(ha,bb[k].cst); hs=cp_mix(hs,bb[k].cst);
        CPItem it; it.abs=ha; it.shp=hs; it.shpG=hs; it.x0=x0; it.y0=y0; it.verts=vc; it.cst=bb[k].cst; it.cell=-1;
        it.floorTile=(bb[k].flags&2)?1:0; it.q256=0;
        g_cpLotCur.push_back(it);
    }
    uint64_t fa=CP_FNV0;
    for(size_t k=0;k<g_cpLotCur.size();k++)
        fa=cp_mix(cp_mix(fa,(uint32_t)g_cpLotCur[k].abs),(uint32_t)(g_cpLotCur[k].abs>>32));
    fa=cp_mix(fa,g_gxClear);
    // --- comparisons against the previous frame (multiset) ---
    const int bk = (g_cpOffX==g_cpOffXPrev && g_cpOffY==g_cpOffYPrev) ? 0 : 1;
    g_cpLastBk = bk;
    if(g_cpFrames){
        CPBucket& B=g_cpB[bk];
        ++B.frames; B.bytes+=g_cpFrameBytes;
        if(fa==g_cpFrameAbsPrev) ++B.frameSameA;
        // Previous frame's table: hash -> indices (to read back x0,y0).
        std::unordered_map<uint64_t,std::vector<uint32_t> > A,S,G;
        auto build=[&](std::vector<CPItem>& prev){ A.clear(); S.clear(); G.clear();
            for(uint32_t k=0;k<prev.size();k++){ A[prev[k].abs].push_back(k); S[prev[k].shp].push_back(k); G[prev[k].shpG].push_back(k); } };
        auto take=[&](std::unordered_map<uint64_t,std::vector<uint32_t> >& m,uint64_t h,int* who)->bool{
            auto it=m.find(h); if(it==m.end()||it->second.empty()) return false;
            if(who) *who=(int)it->second.back(); it->second.pop_back(); return true; };
        // deltas of pairs matched by SHAPE (for the uniform translation check)
        std::unordered_map<uint64_t,uint32_t> dlt; uint64_t nPair=0;
        build(g_cpDrawPrev);
        for(size_t k=0;k<g_cpDrawCur.size();k++){ const CPItem& d=g_cpDrawCur[k];
            int who=-1;
            const bool sa=take(A,d.abs,nullptr), ss=take(S,d.shp,&who), sg=take(G,d.shpG,nullptr);
            ++B.d; B.dv+=d.verts;
            if(sa){ ++B.dA; B.dvA+=d.verts; } if(ss){ ++B.dR; B.dvR+=d.verts; } if(sg) ++B.dG;
            if(ss && who>=0){ const int dx=(int)(d.x0-g_cpDrawPrev[who].x0), dy=(int)(d.y0-g_cpDrawPrev[who].y0);
                ++dlt[((uint64_t)(uint32_t)dx<<32)|(uint32_t)dy]; ++nPair; }
            if(d.floorTile){ ++B.f; B.fv+=d.verts;
                if(sa){ ++B.fA; B.fvA+=d.verts; } if(ss){ ++B.fR; B.fvR+=d.verts; } if(sg) ++B.fG; }
                if(d.q256){ ++B.q256; if(sa) ++B.q256A; if(ss) ++B.q256R; }
            // D2_CACHEDUMP=<frame>: the first 12 FLOOR draws of this frame and
            // the next, to read BY HAND why they don't repeat (position,
            // shape, lighting, atlas cell).
            if(g_cpDumpF>=0 && (int)g_cpFrames>=g_cpDumpF && bk==1 && d.floorTile && g_cpDumpN<40){
                ++g_cpDumpN;
                std::printf("cpd: f=%llu dcam=(%d,%d) i=%zu xy=(%.1f,%.1f) shp=%016llx geo=%016llx cst=%08x cell=%d n=%u %s%s\n",
                    (unsigned long long)g_cpFrames,g_cpOffX-g_cpOffXPrev,g_cpOffY-g_cpOffYPrev,k,d.x0,d.y0,(unsigned long long)d.shp,(unsigned long long)d.shpG,
                    d.cst,(int)d.cell,d.verts,sa?"EN-PLACE ":"",ss?"MEME-FORME":""); } }
        // mode of the displacement distribution
        uint32_t best=0; uint64_t bkey=0;
        for(auto& kv:dlt) if(kv.second>best){ best=kv.second; bkey=kv.first; }
        B.pair+=nPair; B.pairMode+=best;
        if(bkey && best) { B.movedFrames++; B.modeDx+=(int32_t)(uint32_t)(bkey>>32); B.modeDy+=(int32_t)(uint32_t)bkey; }
        build(g_cpLotPrev);
        for(size_t k=0;k<g_cpLotCur.size();k++){ const CPItem& d=g_cpLotCur[k];
            const bool sa=take(A,d.abs,nullptr), ss=take(S,d.shp,nullptr);
            ++B.l; B.lv+=d.verts; if(sa){ ++B.lA; B.lvA+=d.verts; } if(ss){ ++B.lR; B.lvR+=d.verts; } }
    }
    ++g_cpFrames; g_cpFrameBytes=0;
    g_cpFrameAbsPrev=fa;
    g_cpOffXPrev=g_cpOffX; g_cpOffYPrev=g_cpOffY;
    g_cpDrawPrev.swap(g_cpDrawCur); g_cpDrawCur.clear();
    g_cpLotPrev.swap(g_cpLotCur);   g_cpLotCur.clear();
}
// The draw that was just built feeds into the hash of every open phase
// (D2_RINGPHASE_X).
static void cp_phase_draw(){
    if(!cacheprobe_on()) return;
    for(int i=0;i<g_rphN;i++) if(g_rph[i].open){
        g_rph[i].hA=cp_mix(cp_mix(g_rph[i].hA,(uint32_t)g_cpDrawA),(uint32_t)(g_cpDrawA>>32));
        g_rph[i].hR=cp_mix(cp_mix(g_rph[i].hR,(uint32_t)g_cpDrawS),(uint32_t)(g_cpDrawS>>32));
        ++g_rph[i].dr; g_rph[i].vt+=g_cpDrawV; }
}
static void cp_phase_frame(){
    for(int i=0;i<g_rphN;i++){
        if(g_rph[i].frames){ const bool eq=(g_rph[i].hA==g_rph[i].pA);
                             if(eq) ++g_rph[i].sameA;
                             if(g_rph[i].hR==g_rph[i].pR) ++g_rph[i].sameR;
                             if(g_cpLastBk){ ++g_rph[i].fr1; if(eq) ++g_rph[i].sm1; }
                             else          { ++g_rph[i].fr0; if(eq) ++g_rph[i].sm0; } }
        g_rph[i].pA=g_rph[i].hA; g_rph[i].pR=g_rph[i].hR;
        g_rph[i].hA=CP_FNV0; g_rph[i].hR=CP_FNV0; ++g_rph[i].frames; }
}
// Window line (DELTA) or final line (cumulative). Two lines: fixed camera
// and moving camera. Percentages are computed on the window's delta.
static void cp_line(const char* tag){
    if(!cacheprobe_on() || !g_cpFrames) return;
    auto pc=[](uint64_t a,uint64_t b)->unsigned{ return b? (unsigned)(a*100/b) : 0u; };
    const bool fin = tag && tag[0];
    for(int k=0;k<2;k++){
        CPBucket B=g_cpB[k];
        if(!fin){ const CPBucket& P=g_cpBPrev[k]; CPBucket D;
            uint64_t* d=(uint64_t*)&D; const uint64_t* c=(const uint64_t*)&g_cpB[k]; const uint64_t* q=(const uint64_t*)&P;
            for(size_t z=0;z<sizeof(CPBucket)/sizeof(uint64_t);z++) d[z]=c[z]-q[z];
            B=D; }
        if(!B.frames) continue;
        char m[860];
        std::snprintf(m,sizeof m,
          "cache%s[camera %s]: images=%llu images-identiques=%u%% | dessins/img=%llu sommets/img=%llu"
          " | dessins repetes : en-place=%u%% meme-forme=%u%% meme-geometrie=%u%%"
          " (sommets en-place=%u%% meme-forme=%u%%)"
          " | SOL(bilineaire) %u%% des dessins %u%% des sommets : en-place=%u%% meme-forme=%u%% meme-geometrie=%u%%"
          " | 256x128 %u%% des dessins : en-place=%u%% meme-forme=%u%%"
          " | lots/img=%llu en-place=%u%% meme-forme=%u%%"
          " | translation uniforme : %u%% des paires appariees, vecteur median (%lld,%lld)"
          " | octets-sommets-invites/img=%llu",
          tag, k? "MOBILE":"fixe", (unsigned long long)B.frames, pc(B.frameSameA,B.frames),
          (unsigned long long)(B.d/B.frames),(unsigned long long)(B.dv/B.frames),
          pc(B.dA,B.d),pc(B.dR,B.d),pc(B.dG,B.d),pc(B.dvA,B.dv),pc(B.dvR,B.dv),
          pc(B.f,B.d),pc(B.fv,B.dv),pc(B.fA,B.f),pc(B.fR,B.f),pc(B.fG,B.f),
          pc(B.q256,B.d),pc(B.q256A,B.q256),pc(B.q256R,B.q256),
          (unsigned long long)(B.l/B.frames),pc(B.lA,B.l),pc(B.lR,B.l),
          pc(B.pairMode,B.pair),
          (long long)(B.movedFrames? B.modeDx/(int64_t)B.movedFrames:0),
          (long long)(B.movedFrames? B.modeDy/(int64_t)B.movedFrames:0),
          (unsigned long long)(B.bytes/B.frames));
        d2vita_progress(m); std::printf("[%s]\n",m);
    }
    for(int i=0;i<g_rphN;i++){
        RPhase P=g_rph[i];
        if(!fin){ P.frames=g_rph[i].frames-g_rphPrev[i].frames; P.sameA=g_rph[i].sameA-g_rphPrev[i].sameA;
                  P.sameR=g_rph[i].sameR-g_rphPrev[i].sameR; P.dr=g_rph[i].dr-g_rphPrev[i].dr;
                  P.vt=g_rph[i].vt-g_rphPrev[i].vt; P.fr0=g_rph[i].fr0-g_rphPrev[i].fr0; P.sm0=g_rph[i].sm0-g_rphPrev[i].sm0;
                  P.fr1=g_rph[i].fr1-g_rphPrev[i].fr1; P.sm1=g_rph[i].sm1-g_rphPrev[i].sm1; }
        if(!P.frames) continue;
        char q[256];
        std::snprintf(q,sizeof q,"cache%s/phase: Game+0x%x images=%llu dessins/img=%llu sommets/img=%llu"
            " span-identique abs=%u%% trans=%u%% | camera fixe %llu img -> %u%% ; camera MOBILE %llu img -> %u%%",tag,(unsigned)g_rph[i].rva,
            (unsigned long long)P.frames,(unsigned long long)(P.dr/P.frames),
            (unsigned long long)(P.vt/P.frames),pc(P.sameA,P.frames),pc(P.sameR,P.frames),
            (unsigned long long)P.fr0,pc(P.sm0,P.fr0),(unsigned long long)P.fr1,pc(P.sm1,P.fr1));
        d2vita_progress(q); std::printf("[%s]\n",q); }
    if(!fin){ for(int k=0;k<2;k++) g_cpBPrev[k]=g_cpB[k];
              for(int i=0;i<8;i++) g_rphPrev[i]=g_rph[i]; }
}
// Replays [tail, head) and advances tail. Returns the number of records read.
static uint32_t gr_replay(uint32_t head){
    if(!g_grDataVA||!g_grSize) return 0;
    { static bool once=false; if(!once){ once=true;
        const char* e=getenv("D2_GRHASH"); g_grHashMode = (e&&*e)? atoi(e) : 1;
        g_grHashOn = g_grHashMode!=0; } }
    // HOST VIEW OF THE RING. hostptr() is PURE and only validates the arena:
    // on both targets (qemu, Vita) the ring — our glide3x.dll's .bss — lives
    // inside the arena, so one call for the WHOLE SESSION (gr_ring_arm)
    // suffices. A ring outside the arena would be a violation of
    // glide3x_ring.c's contract, not a case to handle silently. This is also
    // what makes the walk callable from ANOTHER HOST THREAD (D2_FLUSHFIL):
    // there is no `Cpu&` involved here.
    const uint8_t* rh = g_grRingHost;
    if(!rh){ d2_crashlog("FATAL: ring Glide hors arene (VA=0x%08x taille=%u) — pas de vue hote",
                         g_grDataVA, g_grSize); _exit(2); }
    auto RD=[&](uint32_t absoff)->uint32_t{
        uint32_t w; std::memcpy(&w, rh+(absoff&g_grMask), 4); return w; };
    const bool prof=grprof_on();
    uint32_t n=0;
    while((int32_t)(head-g_grTail)>0){
        const uint32_t off=g_grTail&g_grMask;
        const uint32_t* rp = (const uint32_t*)(rh+off);   // the record, as seen from the host
        const uint32_t w0 = rp[0];
        const uint32_t op=w0>>24, len=w0&0x00FFFFFFu;
        if(!len||len>(g_grSize>>2)) break;                 // inconsistent stream: stop
        const uint32_t bytes=len<<2;
        ++n; ++g_grRecs; g_grBytes+=bytes; gr_mix(w0); gr_mix_all(rp,len);
        // D2_GRDUMP=<frame>: traces ALL records of that frame (op, length,
        // first 12 words). Used to locate, word by word, the first
        // divergence between two streams (see `rh=` in the gxlh line).
        { static int dumpF=-2; if(dumpF==-2){ const char* e=getenv("D2_GRDUMP"); dumpF = (e&&*e)? atoi(e) : -1; }
          if(dumpF>=0 && (int)g_grFrames==dumpF){
            uint64_t rh=1469598103934665603ull;
            for(uint32_t i=0;i<len;i++){ rh^=rp[i]; rh*=1099511628211ull; }
            char b[256]; int k=std::snprintf(b,sizeof b,"grd: op=%02x len=%u rh=%016llx",op,len,(unsigned long long)rh);
            for(uint32_t i=1;i<len&&i<9;i++) k+=std::snprintf(b+k,sizeof b-k," %08x",rp[i]);
            std::printf("%s\n",b);
            { const char* e2=getenv("D2_GRDUMPREC");
              if(e2&&*e2&&atoi(e2)==(int)n){ for(uint32_t i=0;i<len;i+=8){ char c[160]; int j=std::snprintf(c,sizeof c,"grw: %4u:",i);
                    for(uint32_t q=i;q<len&&q<i+8;q++) j+=std::snprintf(c+j,sizeof c-j," %08x",rp[q]);
                    std::printf("%s\n",c); } } } } }
        const uint64_t tRec = prof? rt_now_us() : 0;
        switch(op){
          case 0x31: case 0x32: {                          // DRAWVERTEXARRAY(_CONT)
            // Ring format: p[1]=mode, p[2]=count, p[3]=stride
            // (src/glide_ring/glide3x_ring.c).
            const uint32_t md =RD(g_grTail+4);
            const uint32_t cnt=RD(g_grTail+8);
            const uint32_t str=RD(g_grTail+12);
            ++g_grDraws; g_grVerts+=cnt; gr_mix(((uint64_t)cnt<<32)|str);
            if(g_grStateSig!=g_grLastDrawSig){ ++g_grBatches; g_grLastDrawSig=g_grStateSig; }
            if(grinv_on()){ char b[96]; const char* nm=gl_drawmode(md);
                std::snprintf(b,sizeof b,"mode=%s count=%u stride=%u",nm?nm:"?",cnt,str);
                g_grInv[op][b]++; grinv_vertices(g_grTail+16,cnt,str); }
            if(grgx_on()) gx_draw(md,cnt,str, rh+off+16);
            r60_on_draw(cnt); cp_phase_draw();
            if(prof) g_grpDrawUs += rt_now_us()-tRec;
            break; }
          case 0x33: case 0x34: case 0x35: {
            const uint32_t nv = op==0x33?3u : op==0x34?2u : 1u;
            ++g_grDraws; g_grVerts+=nv;
            if(g_grStateSig!=g_grLastDrawSig){ ++g_grBatches; g_grLastDrawSig=g_grStateSig; }
            if(grinv_on()){ const uint32_t str=RD(g_grTail+4);
                char b[96]; std::snprintf(b,sizeof b,"stride=%u sommets=%u",str,nv);
                g_grInv[op][b]++; grinv_vertices(g_grTail+8,nv,str); }
            if(grgx_on()){ const uint32_t str=RD(g_grTail+4);
                gx_draw(op==0x33?6u:0xFFu,nv,str, rh+off+8); }
            r60_on_draw(nv); cp_phase_draw();
            if(prof) g_grpDrawUs += rt_now_us()-tRec;
            break; }
          case 0x3f:                                       // FRAME_END
            ++g_grFrames; ++g_grDupCur;
            if(grgx_on()){
                gx_lazy_init();
                if(!g_gxDead && g_gxAtlas.ready()){
                    g_gxVertsAcc+=g_gxBuild.verts(); g_gxLotsAcc+=g_gxBuild.batches(); ++g_gxFramesAcc;
                    if(gxod_on()) gx_overdraw();
                    if(lotshash_on()) gx_lots_hash();
                    if(cacheprobe_on()) gx_cache_probe();
                    const uint64_t tsub=rt_now_us();
                    if(gxcov_on()) gx_coverage();
                    if(g_r60Active){
                        // D2_REPLAY60. "IN-GAME" SIGNAL = CAMERA record with a non-null
                        // player (g_r60CamValid: exit of Game+0x5b440, one call per frame
                        // from the in-game draw function, player [0x7a6a70] non-null;
                        // 0/frame in menus and loading, 1/frame in-game — see replay60.h
                        // "IDLE").
                        //   in-game    : the frame goes to the replay engine, which submits
                        //                it (60Hz, dedicated thread, at most 3 times). The
                        //                game thread does not submit.
                        //   out-of-game: the engine goes idle (its thread takes no lock)
                        //                and the game thread submits it itself, as if there
                        //                were no replay.
                        if(!d2gr::r60().running()) d2gr::r60().start();
                        if(g_r60CamValid){
                            if(g_r60UnitOpen && !g_r60Units.empty()) g_r60Units.back().v1=g_gxBuild.verts();
                            d2gr::r60().push(g_gxBuild.vdata(),g_gxBuild.verts(),
                                             g_gxBuild.idata(),g_gxBuild.indices(),
                                             g_gxBuild.bdata(),g_gxBuild.batches(),
                                             g_r60Units.data(),(uint32_t)g_r60Units.size(),
                                             g_r60UiSeen?g_r60UiV0:g_gxBuild.verts(),
                                             g_r60CamValid,g_r60PlayerId,g_r60PX,g_r60PY,g_r60OffX,g_r60OffY,
                                             g_r60Tick,g_gxFrame,g_gxClear);
                        } else {
                            d2gr::r60().sleepFrame();
                            r60_gxm_lock();
                            d2gxm_submit(g_gxBuild.vdata(),g_gxBuild.verts(),
                                         g_gxBuild.idata(),g_gxBuild.indices(),
                                         g_gxBuild.bdata(),g_gxBuild.batches(),g_gxClear,g_gxFrame);
                            r60_gxm_unlock();
                        }
                    } else
                    d2gxm_submit(g_gxBuild.vdata(),g_gxBuild.verts(),
                                 g_gxBuild.idata(),g_gxBuild.indices(),
                                 g_gxBuild.bdata(),g_gxBuild.batches(),g_gxClear,g_gxFrame);
                    g_gxSubmitUs += rt_now_us()-tsub;
                    if(prof) g_grpEndUs += tsub-tRec;
                    g_gxDroppedWin+=g_gxBuild.dropped(); g_gxBuild.reset(); ++g_gxFrame; }
            }
            if(cacheprobe_on()) cp_phase_frame();
            r60_frame_reset();
            break;
          case 0x00: break;                                // end-of-buffer padding
          case D2GR_OP_UNITTAG: case D2GR_OP_UNITEND: case D2GR_OP_CAMERA: case D2GR_OP_TICK: case D2GR_OP_PHASE:
            // records EMITTED BY THE HOST (D2_REPLAY60 / D2_RINGTAG): outside
            // the state signature, they only exist if a knob requested them.
            r60_ring_op(op,g_grTail,len,g_gxBuild.verts());
            break;
          default: {                                       // state: enters the signature
            ++g_grState;
            if(grdup_on() && op<0x30 && len<=8 && op!=0x21 && op!=0x22){   // redundant? (mirrors the DLL's deduplication)
                bool same = g_grDupGen[op]==g_grDupCur;
                for(uint32_t i=1;i<len;i++){ if(same && g_grDupLast[op][i]!=rp[i]) same=false; g_grDupLast[op][i]=rp[i]; }
                g_grDupGen[op]=g_grDupCur; if(same) ++g_grStateDup;
                if(op==0x24) g_grDupGen[0x10]=0; else if(op==0x25) ++g_grDupCur; }
            // Vertex layout is tracked UNCONDITIONALLY: the GPU path depends
            // on it, and gating it on the inventory knob would draw from zero
            // offsets whenever the inventory is off.
            if(op==0x20){ const uint32_t pa=RD(g_grTail+4);
                const uint32_t of=RD(g_grTail+8);
                const uint32_t md=RD(g_grTail+12);
                if(pa<0x60) g_grVLoff[pa]= md? (int)of : -1; }
            if(grgx_on()) gx_state(op,len,rp);
            if(grinv_on()) grinv_state(op,g_grTail,len);
            uint64_t sig=g_grStateSig;
            for(uint32_t i=1;i<len&&i<8;i++){ sig^=RD(g_grTail+4*i); sig*=1099511628211ull; }
            g_grStateSig=sig; gr_mix(sig);
            if(cacheprobe_on()) for(int pi=0;pi<g_rphN;pi++) if(g_rph[pi].open)
                for(uint32_t i=0;i<len&&i<64;i++){ g_rph[pi].hA=cp_mix(g_rph[pi].hA,rp[i]); g_rph[pi].hR=cp_mix(g_rph[pi].hR,rp[i]); }
            if(prof) g_grpStateUs += rt_now_us()-tRec;
            break; }
        }
        g_grTail+=bytes;
    }
    return n;
}
// GPU path line, in the same 10s window as the rest of the profile.
// Silent without D2_GLIDEGXM. Under qemu, everything is published EXCEPT the
// two durations (submission/wait), which only exist on target: that's the
// half that CAN be measured ahead of time, and it is.
static void gx_line(uint32_t frames){
    if(!grgx_on() || !g_gxAtlas.ready() || !frames) return;
    static uint64_t pH=0,pM=0,pE=0,pC=0,pCB=0,pBM=0,pBO=0,pV=0,pL=0,pF=0,pFill=0;
    const uint64_t dH=g_gxAtlas.hits()-pH, dM=g_gxAtlas.miss()-pM, dE=g_gxAtlas.evictions()-pE;
    const uint64_t dC=g_gxAtlas.copies()-pC, dCB=g_gxAtlas.copyBytes()-pCB;
    const uint64_t dBM=g_gxAtlas.bindMiss()-pBM, dBO=g_gxAtlas.bindOk()-pBO;
    const uint64_t dV=g_gxVertsAcc-pV, dL=g_gxLotsAcc-pL, dF=g_gxFramesAcc-pF;
    const uint32_t dDrop=g_gxDroppedWin-g_gxDroppedPrev; g_gxDroppedPrev=g_gxDroppedWin;
    pH=g_gxAtlas.hits();pM=g_gxAtlas.miss();pE=g_gxAtlas.evictions();
    pC=g_gxAtlas.copies();pCB=g_gxAtlas.copyBytes();
    pBM=g_gxAtlas.bindMiss();pBO=g_gxAtlas.bindOk();
    pV=g_gxVertsAcc;pL=g_gxLotsAcc;pF=g_gxFramesAcc;
    const uint64_t n = dF?dF:frames;
    char m[640];
    std::snprintf(m,sizeof m,
      // Reminder of the mechanism (glide_atlas.h): a page belongs to ONE size
      // CLASS, and evictOne() can only evict a cell of the SAME class. Once
      // all pages are taken, a sprite size NEVER SEEN before cannot be
      // served — even if other classes still have free cells. This is a
      // design limitation, not a tuning knob.
      "gxm: atlas-plein=%llu | lots=%llu sommets=%llu | atlas: pages=%u/%u cellules=%lu reussites=%llu%%"
      " (%llu/%llu) evictions=%llu (vivantes=%llu en-vol=%llu) copies=%llu (%llu Ko/img)"
      " | liaisons manquees=%llu%% ordre=%llu repli=%llu perimees=%llu | perdus=%u melange-inconnu=%llu fmt-autre=%llu"
      " | surdessin=%llu Kpx/img (%llu%% de l'ecran) lots-par-palette=%s"
      " | non-couverts=%llu px/img (%llu%% de l'ecran, pire %llu)",
      (unsigned long long)g_gxAtlas.pagesFull(),
      (unsigned long long)(dL/n),(unsigned long long)(dV/n),
      g_gxAtlas.pages(),g_gxAtlas.maxPages(),(unsigned long)g_gxAtlas.cellCount(),
      (dH+dM)? (unsigned long long)(dH*100/(dH+dM)) : 0ull,
      (unsigned long long)dH,(unsigned long long)(dH+dM),
      (unsigned long long)dE,(unsigned long long)g_gxAtlas.evictLive(),
      (unsigned long long)g_gxAtlas.evictHeld(),
      (unsigned long long)dC,(unsigned long long)((dCB/n)>>10),
      (dBM+dBO)? (unsigned long long)(dBM*100/(dBM+dBO)) : 0ull,
      (unsigned long long)g_gxBindOrdered,(unsigned long long)g_gxBindFallback,(unsigned long long)g_gxBindStale,
      (unsigned)dDrop,(unsigned long long)g_gxBlendUnknown,(unsigned long long)g_gxFmtOther,
      (unsigned long long)((g_gxFillAcc-pFill)/n>>10),
      (unsigned long long)(((g_gxFillAcc-pFill)/n)*100/(960ull*544ull)),
      g_gxBuild.splitsByPalette()?"coupes":"melanges",
      (unsigned long long)(g_gxCovFrames? g_gxCovAcc/g_gxCovFrames : 0ull),
      (unsigned long long)(g_gxCovFrames? (g_gxCovAcc/g_gxCovFrames)*100/(960ull*544ull) : 0ull),
      (unsigned long long)g_gxCovWorst);
    pFill=g_gxFillAcc;
    d2vita_progress(m); std::printf("[%s]\n",m);
    cp_line("");
    char q[640];
    if(d2gxm_counters(q,sizeof q)>0){ d2vita_progress(q); std::printf("[%s]\n",q); }
    // Breakdown of the host-side FLUSH. `parcours-ring` (ring walk) is what
    // remains of the record walk once uploads and submission are subtracted
    // out: it's the only line item no other counter sees.
    { static uint64_t pF=0,pR=0,pT=0,pS=0;
      const uint64_t dF=g_gxFlushUs-pF, dR=g_gxReplayUs-pR, dT=g_gxTexUs-pT, dS=g_gxSubmitUs-pS;
      pF=g_gxFlushUs; pR=g_gxReplayUs; pT=g_gxTexUs; pS=g_gxSubmitUs;
      const uint64_t reste = (dR>dT+dS)? (dR-dT-dS) : 0ull;
      static uint64_t pN=0,pB=0,pH=0;
      const uint64_t dN=g_gxTexHashN-pN, dB=g_gxTexHashB-pB, dHh=g_grpTexHashUs-pH;
      pN=g_gxTexHashN; pB=g_gxTexHashB; pH=g_grpTexHashUs;
      char f[320];
      std::snprintf(f,sizeof f,
        "gxm-flush: total-us=%llu (parcours-ring-us=%llu televersements-us=%llu soumission-us=%llu"
        " hors-replay-us=%llu) | televersement: n/img=%llu octets-moy=%llu us/tele=%llu hachage-us/tele=%llu (%s)",
        (unsigned long long)(dF/n),(unsigned long long)(reste/n),
        (unsigned long long)(dT/n),(unsigned long long)(dS/n),
        (unsigned long long)((dF>dR? dF-dR : 0ull)/n),
        (unsigned long long)(dN/n),(unsigned long long)(dN? dB/dN : 0ull),(unsigned long long)(dN? dT/dN : 0ull),
        (unsigned long long)(dN? dHh/dN : 0ull), grprof_on()? (texhash_mode()==1?"4voies":texhash_mode()==2?"echantillon":"fnv64") : "GRPROF=0");
      d2vita_progress(f); std::printf("[%s]\n",f); }
    // FINE-GRAINED timers for the walk (D2_GRPROF=1): where the walk's
    // microseconds go, by record type. `clock-ns` is the cost of ONE clock
    // read, measured on the first flush: two per record is what the
    // instrument itself adds to the walk.
    if(grprof_on()){
      static uint64_t pS=0,pD=0,pY=0,pV=0,pE=0,pR=0;
      const uint64_t dS=g_grpStateUs-pS, dD=g_grpDrawUs-pD, dY=g_grpSyncUs-pY, dV=g_grpVtxUs-pV, dE=g_grpEndUs-pE;
      const uint64_t dRec=g_grRecs-pR;
      pS=g_grpStateUs; pD=g_grpDrawUs; pY=g_grpSyncUs; pV=g_grpVtxUs; pE=g_grpEndUs; pR=g_grRecs;
      char f[320];
      std::snprintf(f,sizeof f,
        "gxm-ring: etat-us=%llu dessins-us=%llu (recomposition-us=%llu sommets-us=%llu) fin-image-us=%llu"
        " | horloge-ns=%llu x %llu lectures/img | recompositions=%llu evitees=%llu",
        (unsigned long long)(dS/n),(unsigned long long)(dD/n),(unsigned long long)(dY/n),(unsigned long long)(dV/n),
        (unsigned long long)(dE/n),(unsigned long long)g_grpClockNs,(unsigned long long)(dRec*2/n),
        (unsigned long long)g_gxSyncDone,(unsigned long long)g_gxSyncSkipped);
      d2vita_progress(f); std::printf("[%s]\n",f); }
    if(lotshash_on()){ char f[96];
      std::snprintf(f,sizeof f,"gxm-lots: images=%llu lh=0x%016llx",(unsigned long long)g_gxLotsHashN,(unsigned long long)g_gxLotsHash);
      d2vita_progress(f); std::printf("[%s]\n",f); }
    std::fflush(stdout);
}
static void gr_final_cumul(){
    // Cumulative total for the WHOLE RUN (not the last window): this is the
    // figure to compare between two A/B legs — 10s windows don't land at the
    // same point and per-frame load varies widely (hundreds to thousands of
    // verts).
    if(g_grFrames){
        const uint64_t f=g_grFrames;
        const uint64_t reste=(g_gxReplayUs>g_gxTexUs+g_gxSubmitUs)? g_gxReplayUs-g_gxTexUs-g_gxSubmitUs : 0ull;
        std::printf("[ring-final] parcours-ring-us/img=%llu televersements-us/img=%llu soumission-us/img=%llu"
                    " flush-us/img=%llu | recompositions=%llu evitees=%llu | hachage=%s",
            (unsigned long long)(reste/f),(unsigned long long)(g_gxTexUs/f),(unsigned long long)(g_gxSubmitUs/f),
            (unsigned long long)(g_gxFlushUs/f),(unsigned long long)g_gxSyncDone,(unsigned long long)g_gxSyncSkipped,
            g_grHashMode==2?"INTEGRAL (D2_GRHASH=2)":g_grHashOn?"oui":"NON (D2_GRHASH=0)");
        if(g_gxLotsHashN) std::printf(" | lots=%llu lh=0x%016llx lu=0x%016llx",(unsigned long long)g_gxLotsHashN,
            (unsigned long long)g_gxLotsHash,(unsigned long long)g_gxLotsHashU);
        std::printf(" | etats=%llu redondants=%llu evites-dll=%u | tele: n=%llu octets-moy=%llu us/tele=%llu texhash=%d",
            (unsigned long long)g_grState,(unsigned long long)g_grStateDup,g_grDedupDll,
            (unsigned long long)g_gxTexHashN,(unsigned long long)(g_gxTexHashN? g_gxTexHashB/g_gxTexHashN:0ull),
            (unsigned long long)(g_gxTexHashN? g_gxTexUs/g_gxTexHashN:0ull),texhash_mode());
        if(grprof_on()) std::printf(" | fins: etat=%llu dessins=%llu (recomp=%llu sommets=%llu) fin=%llu us/img horloge-ns=%llu hachage-tex=%llu us/img (%llu us/tele)",
            (unsigned long long)(g_grpStateUs/f),(unsigned long long)(g_grpDrawUs/f),(unsigned long long)(g_grpSyncUs/f),
            (unsigned long long)(g_grpVtxUs/f),(unsigned long long)(g_grpEndUs/f),(unsigned long long)g_grpClockNs,
            (unsigned long long)(g_grpTexHashUs/f),(unsigned long long)(g_gxTexHashN? g_grpTexHashUs/g_gxTexHashN:0ull));
        std::printf("\n");
    }
}
void gr_line(uint32_t frames){
    if(!g_grHdrVA||!frames) return;
    // The counters published below are written by the FLUSH THREAD. We wait
    // for it to finish its batch before reading them: once per 10s window,
    // this is free, and it avoids publishing half-written numbers.
    if(g_ffArmed) ff_wait_idle();
    static uint64_t pR=0,pD=0,pV=0,pB=0,pL=0,pS=0,pT=0,pTB=0,pDup=0,pDD=0;
    char m[340];
    std::snprintf(m,sizeof m,
      "ring: %llu enr/img (etat=%llu redondants=%llu evites-dll=%llu dessins=%llu lots=%llu) | %llu sommets/img | %llu Ko/img"
      " | texup=%llu (%llu Ko/img) | attentes=%u perdus=%u | h=0x%016llx",
      (unsigned long long)((g_grRecs-pR)/frames),(unsigned long long)((g_grState-pS)/frames),
      (unsigned long long)((g_grStateDup-pDup)/frames),(unsigned long long)((g_grDedupDll-pDD)/frames),
      (unsigned long long)((g_grDraws-pD)/frames),(unsigned long long)((g_grBatches-pL)/frames),
      (unsigned long long)((g_grVerts-pV)/frames),(unsigned long long)(((g_grBytes-pB)/frames)>>10),
      (unsigned long long)((g_grTexUp-pT)/frames),(unsigned long long)(((g_grTexUpB-pTB)/frames)>>10),
      g_grStalls,g_grDropped,(unsigned long long)g_grHash);
    pR=g_grRecs;pD=g_grDraws;pV=g_grVerts;pB=g_grBytes;pL=g_grBatches;pS=g_grState;pT=g_grTexUp;pTB=g_grTexUpB;pDup=g_grStateDup;pDD=g_grDedupDll;
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
    gx_line(frames);
    gr_flush_line(frames);
    if(d2gr::Replay60::tagOn()){
        char q[900];
        int o=std::snprintf(q,sizeof q,"ringtag: emis=%llu/img refuses=%llu unites=%s",
                            (unsigned long long)(g_grEmitN/frames),(unsigned long long)g_grEmitDrop,
                            g_r60Active?"->rejeu":"comptes");
        for(int i=0;i<g_rphN && o<(int)sizeof q-96;i++){
            o+=std::snprintf(q+o,sizeof q-(size_t)o," | Game+0x%x: entrees=%llu dessins=%llu sommets=%llu%s",
                             (unsigned)g_rph[i].rva,(unsigned long long)(g_rph[i].enters/frames),
                             (unsigned long long)(g_rph[i].draws/frames),(unsigned long long)(g_rph[i].verts/frames),
                             g_rph[i].nested?" (reentrant)":"");
            g_rph[i].enters=0; g_rph[i].draws=0; g_rph[i].verts=0; }
        g_grEmitN=0;
        d2vita_progress(q); std::printf("[%s]\n",q); std::fflush(stdout);
        if(d2gr::Replay60::on()){
            char r[1400]; d2gr::r60().line(r,sizeof r,frames);
            d2vita_progress(r); std::printf("[%s]\n",r); std::fflush(stdout); }
    }
}

// ---- WRAPPERS for the call sites left in rt_boot.cpp (bodies of
// d2vGlideInit / d2vGlideFlush / d2vGlideTexUpload, and the final [ring] line
// of the exit report). -----------------------------------------
void gr_ring_arm(d2rt::Cpu& c, uint32_t hdr, uint32_t* dataVA, uint32_t* size){
          g_grHdrVA=hdr;
          g_grSize=c.read_u32(hdr+8);
          g_grDataVA=hdr+c.read_u32(hdr+12);
          g_grMask=g_grSize-1u;
          g_grTail=0;
          // Host views captured HERE, on the game thread: the walk (and the
          // flush thread) will never call hostptr() again.
          g_grRingHost=(const uint8_t*)c.hostptr(g_grDataVA,g_grSize);
          g_grHdrHost =(uint8_t*)c.hostptr(hdr,64);
          if(!g_grRingHost||!g_grHdrHost)
              d2_crashlog("FATAL: ring Glide hors arene (en-tete 0x%08x donnees 0x%08x taille %u)",hdr,g_grDataVA,g_grSize);
          *dataVA=g_grDataVA; *size=g_grSize;
}
// --- flush thread body -------------------------------------------------------
// A batch = [g_grTail, g_ffHead). The walk, batch building, offloaded
// uploads, and GXM submission all happen HERE; the game thread only hands
// off the head. `tail` is written back into the guest header through the
// host view, after a barrier: it's the only word this thread writes into
// guest memory, and the guest only ever READS it (glide_ring.h).
static void ff_consume_one(uint32_t head){
    const uint64_t trep=rt_now_us();
    gr_replay(head);
    g_gxReplayUs += rt_now_us()-trep;
    if(g_grHdrHost){ __sync_synchronize(); std::memcpy(g_grHdrHost+20,&g_grTail,4); __sync_synchronize(); }
}
static bool ff_thread_main(){
    ff_pin_self();
    while(!g_ffStop){
        if(!g_ffPending){
            for(int i=0;i<4000 && !g_ffPending && !g_ffStop;i++) __sync_synchronize();
            if(!g_ffPending){ ff_sleep_us(200); continue; } }
        __sync_synchronize();
        const uint32_t head=g_ffHead;
        const uint64_t t0=rt_now_us();
        ff_consume_one(head);
        g_ffBusyUs += rt_now_us()-t0;
        __sync_synchronize(); g_ffPending=0;
    }
    g_ffRunning=0; return true;
}
// Startup: happens AFTER gx_lazy_init() (done on the game thread, which
// creates the sceGxm context and atlas pages) and AFTER the sceGxm lock is
// armed.
static bool ff_start(){
    if(g_ffRunning) return true;
    {   unsigned mio=4; if(const char* e=getenv("D2_FLUSHFIL_STG")){ unsigned v=atoi(e); if(v>=1&&v<=32) mio=v; }
        uint32_t sz=1u; while(sz < (mio<<20)) sz<<=1;
        g_ffStg=(uint8_t*)std::malloc(sz);
        if(!g_ffStg){ jpline("flushfil: REFUS — %u Mio d'attente textures introuvables", mio); return false; }
        g_ffStgSize=sz; g_ffStgMask=sz-1u; g_ffStgHead=g_ffStgTail=0; }
    r60_gxm_arm();                     // the sceGxm context lock must exist even without 60Hz replay
    g_ffStop=0; g_ffRunning=1;
    if(!ff_spawn()){ g_ffRunning=0; std::free(g_ffStg); g_ffStg=nullptr; jpline("flushfil: CreateThread KO — flush synchrone"); return false; }
    g_ffArmed=true;
    return true;
}
void gr_flush_stop(){
    if(!g_ffRunning) return;
    ff_wait_idle(); g_ffStop=1;
    for(int i=0;i<400 && g_ffRunning;i++) ff_sleep_us(1000);
    g_ffArmed=false;
}
// Flush thread's window line (silent without the knob).
void gr_flush_line(uint32_t frames){
    if(!g_ffArmed || !frames) return;
    static uint64_t pW=0,pU=0,pL=0,pB=0,pSW=0,pC=0,pCB=0;
    const uint64_t dW=g_ffWaits-pW, dU=g_ffWaitUs-pU, dL=g_ffLots-pL, dB=g_ffBusyUs-pB;
    const uint64_t dSW=g_ffStgWaits-pSW, dC=g_ffCopyUs-pC, dCB=g_ffCopyB-pCB;
    pW=g_ffWaits;pU=g_ffWaitUs;pL=g_ffLots;pB=g_ffBusyUs;pSW=g_ffStgWaits;pC=g_ffCopyUs;pCB=g_ffCopyB;
    char m[400];
    std::snprintf(m,sizeof m,
      "flushfil: lots=%llu/img fil-occupe=%llu us/img | attentes du jeu=%llu (%llu us/img, max %llu us)"
      " | recopie textures=%llu us/img (%llu Ko/img) attentes-tampon=%llu replis=%llu"
      " | ring: occupation-max=%u Ko/%u Ko attentes-dll=%u perdus=%u",
      (unsigned long long)(dL/frames),(unsigned long long)(dB/frames),
      (unsigned long long)dW,(unsigned long long)(dU/frames),(unsigned long long)g_ffWaitMax,
      (unsigned long long)(dC/frames),(unsigned long long)((dCB/frames)>>10),
      (unsigned long long)dSW,(unsigned long long)g_ffStgFallback,
      (unsigned)(g_ffOccMax>>10),(unsigned)(g_grSize>>10),g_grStalls,g_grDropped);
    d2vita_progress(m); std::printf("[%s]\n",m); std::fflush(stdout);
}
void gr_flush(d2rt::Cpu& c, uint32_t head, void (*between)()){
          if(grprof_on() && !g_grpClockNs){       // calibration: 4096 clock reads, on the first flush
              const uint64_t a=rt_now_us(); volatile uint64_t sink=0;
              for(int i=0;i<4096;i++) sink+=rt_now_us();
              g_grpClockNs=((rt_now_us()-a)*1000ull)/4096ull; if(!g_grpClockNs) g_grpClockNs=1; }
          const uint64_t tflush=rt_now_us();
          between();                           // pp_flush_begin: D2_PHASEPROF, flush = ring walk + submission
          if(ff_on() && !g_ffArmed && !g_ffRunning){
              static bool tried=false;
              if(!tried){ tried=true;
                  if(!grgx_on()) jpline("flushfil: INACTIF — exige D2_GLIDEGXM=1 (le puits de comptage reste synchrone)");
                  else { gx_lazy_init();       // sceGxm context + atlas + builder: on the GAME THREAD
                         if(g_gxDead) jpline("flushfil: INACTIF — le chemin GXM est mort");
                         else if(ff_start()) jpline("flushfil: ARME — flush deporte (attente textures %u Ko), le fil de jeu ne fait plus que remettre la tete",g_ffStgSize>>10); } } }
          if(g_ffArmed){
              // BACKPRESSURE (g): at most ONE batch in flight. We wait for the
              // previous one to finish before handing off this one, so the
              // ring never carries more than two frames' worth.
              ff_wait_idle();
              ff_handoff(head);
          } else {
              const uint64_t trep=rt_now_us();
              gr_replay(head);
              g_gxReplayUs += rt_now_us()-trep;
              if(g_grHdrVA) c.write_u32(g_grHdrVA+20,g_grTail);   // tail: what the host has consumed
          }
          if(g_grHdrVA){
              g_grStalls =c.read_u32(g_grHdrVA+28);
              g_grDropped=c.read_u32(g_grHdrVA+32);
              g_grDedupDll=c.read_u32(g_grHdrVA+36);              // dedup_evites (DLL, cumulative)
          }
          // Frame boundary for the ring path: same point as grBufferSwap.
          // ⚠️ The flush timer stops BEFORE fp_tick/cmd_poll: those aren't
          // rendering, and including them would charge the GPU for the cost
          // of profiling and remote control.
          g_gxFlushUs += rt_now_us()-tflush;
          d2rt_gx_flush_us = g_gxFlushUs;
}
void gr_tex_upload_count(uint64_t nb){ ++g_grTexUp; g_grTexUpB+=nb; }
void gr_final_line(){
            if(g_grHdrVA) std::printf("  [ring] images=%u enr=%llu dessins=%llu sommets=%llu emis-hote=%llu empreinte=0x%016llx\n",
                        g_grFrames,(unsigned long long)g_grRecs,(unsigned long long)g_grDraws,
                        (unsigned long long)g_grVerts,(unsigned long long)g_grEmitN,(unsigned long long)g_grHash);
}
