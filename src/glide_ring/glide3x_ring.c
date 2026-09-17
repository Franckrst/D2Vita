/* glide3x_ring.c — glide3x.dll x86 "ring buffer".
 *
 * WHAT THIS IS. A glide3x.dll we build ourselves (i686-w64-mingw32-gcc) that
 * the loader loads like any other PE DLL. It is therefore TRANSLATED by the
 * dynarec exactly like Game.exe: a Glide call from the game into this DLL is
 * an x86 -> x86 call, it never crosses to the host.
 *
 * WHAT IT DOES. It serializes Glide state and draws into a ring buffer
 * (src/glide_ring/glide_ring.h) and crosses to the host only three times:
 *   d2vGlideInit      once for the whole session;
 *   d2vGlideTexUpload per actual texture upload;
 *   d2vGlideFlush     once per frame, from grBufferSwap.
 *
 * WHAT IT DOESN'T DO. It draws nothing and knows nothing about sceGxm: the
 * HOST reader replays the ring. It does no floating-point arithmetic (only
 * copies), so the dynarec never has to emit x87/SSE for it.
 *
 * ⚠️ The game calls these exports by their DECORATED names with a leading
 * underscore (`_grDrawTriangle@12`): that's what Game.exe expects, enforced
 * by the .def file. Don't "clean up" the names.
 */
#include <windows.h>
#include "glide_ring.h"

/* No msvcrt: we want ZERO dependency outside kernel32, otherwise we'd have to
 * shim a guest CRT for three functions. Hand-rolled copies instead. */
static void d2gr_copy(void* d, const void* s, unsigned int n)
{
    unsigned char* dd=(unsigned char*)d; const unsigned char* ss=(const unsigned char*)s;
    while (n >= 4) { *(unsigned int*)dd = *(const unsigned int*)ss; dd+=4; ss+=4; n-=4; }
    while (n--) *dd++ = *ss++;
}
static void d2gr_zero(void* d, unsigned int n)
{
    unsigned char* dd=(unsigned char*)d;
    while (n >= 4) { *(unsigned int*)dd = 0u; dd+=4; n-=4; }
    while (n--) *dd++ = 0;
}

#define FXAPI __declspec(dllexport) void __stdcall
#define FXAPIU __declspec(dllexport) unsigned int __stdcall

/* ---- host imports: the only three crossings ----------------------------- */
typedef struct {
    unsigned int tex_min, tex_max;   /* TMU memory advertised to the game */
    unsigned int gamma_entries;      /* grGet(0x05) */
    unsigned int gamma_bits;         /* grGet(0x2a) */
    unsigned int flags;
    unsigned int dedup;              /* 1 = host requests state DEDUPLICATION (D2_GRDEDUP, default 1) */
    unsigned int reserved[10];
} D2GRConfig;

/* Resolved at runtime via LoadLibraryA/GetProcAddress against a fake SYSTEM
 * DLL `d2vhost.dll` (declared in SYS_DLLS on the host side). This avoids both
 * a MinGW import library and a static import table: the loader has nothing
 * special to do for this DLL, and if the host doesn't provide these entry
 * points, the DLL silently degrades into a no-op stub instead of crashing. */
typedef unsigned int (__stdcall *PFN_INIT)(void*, D2GRConfig*);
typedef unsigned int (__stdcall *PFN_FLUSH)(unsigned int, unsigned int);
typedef unsigned int (__stdcall *PFN_TEXUP)(unsigned int, unsigned int, unsigned int, void*);
typedef unsigned int (__stdcall *PFN_NOP)(unsigned int);
static PFN_INIT  d2vGlideInit  = 0;
static PFN_FLUSH d2vGlideFlush = 0;
static PFN_TEXUP d2vGlideTexUpload = 0;
static PFN_NOP   d2vGlideNop = 0;

/* ---- the ring lives in THIS DLL's .bss: i.e. in GUEST memory ------------- */
static D2GRHeader g_hdr;
static unsigned char g_ring[D2GR_RING_BYTES];
static D2GRConfig  g_cfg;
static int         g_ready = 0;
static unsigned int g_frame = 0;
static unsigned int g_vtxSize = 64;   /* derived from grVertexLayout */

static void ring_boot(void)
{
    if (g_ready) return;
    g_hdr.magic = D2GR_MAGIC; g_hdr.version = D2GR_VERSION;
    g_hdr.size = D2GR_RING_BYTES;
    g_hdr.data_off = (unsigned int)((unsigned char*)g_ring - (unsigned char*)&g_hdr);
    g_hdr.head = g_hdr.tail = g_hdr.frame = 0;
    g_hdr.stalls = g_hdr.dropped = g_hdr.dedup_evites = 0;
    g_cfg.tex_min = 0; g_cfg.tex_max = 0x400000u;
    g_cfg.gamma_entries = 256; g_cfg.gamma_bits = 8;
    {
        HMODULE h = LoadLibraryA("d2vhost.dll");
        if (h) {
            d2vGlideInit      = (PFN_INIT )GetProcAddress(h, "d2vGlideInit");
            d2vGlideFlush     = (PFN_FLUSH)GetProcAddress(h, "d2vGlideFlush");
            d2vGlideTexUpload = (PFN_TEXUP)GetProcAddress(h, "d2vGlideTexUpload");
            d2vGlideNop       = (PFN_NOP  )GetProcAddress(h, "d2vGlideNop");
        }
    }
    g_ready = 1;                            /* set BEFORE the call: ring_boot is reentrant */
    if (d2vGlideInit) d2vGlideInit(&g_hdr, &g_cfg);   /* CROSSING #1, once only */
    /* Calibrates the COST OF A CROSSING: g_cfg.flags carries the call count
     * requested by the host (D2_GLIDEBENCH=<n>). The host timestamps the
     * first and last call, so it measures n full crossings — prologue, GIL,
     * emutls and return included, not just the stub body.
     * Reference point: the SAME loop without crossing (local increment only)
     * gives the cost of pure translated x86. */
    if (d2vGlideNop && g_cfg.flags) {
        unsigned int i, n = g_cfg.flags;
        for (i = 0; i < n; ++i) d2vGlideNop(i);
        { volatile unsigned int acc = 0; for (i = 0; i < n; ++i) acc += i; d2vGlideNop(0x80000000u | acc); }
    }
}

/* Reserves `words` words. Returns NULL if the ring is full: the record is
 * then DROPPED and counted (`dropped`) instead of blocking the game thread —
 * a full ring is a sizing bug, not a nominal case, and blocking here would
 * freeze the game. */
static unsigned int* ring_alloc(unsigned int words)
{
    unsigned int bytes = words << 2;
    unsigned int head, used, off;
    ring_boot();
    head = g_hdr.head;
    used = head - g_hdr.tail;
    if (used + bytes > D2GR_RING_BYTES) { g_hdr.dropped++; return 0; }
    off = head & D2GR_RING_MASK;
    /* A record must never straddle the end of the buffer: if there isn't
     * enough room at the tail, pad with a NOP and wrap back to 0. */
    if (off + bytes > D2GR_RING_BYTES) {
        unsigned int pad = D2GR_RING_BYTES - off;
        *(unsigned int*)(g_ring + off) = D2GR_HDR(D2GR_OP_NOP, pad >> 2);
        head += pad; g_hdr.head = head;
        used = head - g_hdr.tail;
        if (used + bytes > D2GR_RING_BYTES) { g_hdr.dropped++; return 0; }
        off = head & D2GR_RING_MASK;
    }
    g_hdr.head = head + bytes;             /* published AFTER the write, see rec_end */
    return (unsigned int*)(g_ring + off);
}

#define REC0(op)                     do{ unsigned int*_p=ring_alloc(1); if(_p){_p[0]=D2GR_HDR(op,1);} }while(0)
#define REC1(op,a)                   do{ unsigned int*_p=ring_alloc(2); if(_p){_p[0]=D2GR_HDR(op,2);_p[1]=(unsigned int)(a);} }while(0)
#define REC2(op,a,b)                 do{ unsigned int*_p=ring_alloc(3); if(_p){_p[0]=D2GR_HDR(op,3);_p[1]=(unsigned int)(a);_p[2]=(unsigned int)(b);} }while(0)
#define REC3(op,a,b,c)               do{ unsigned int*_p=ring_alloc(4); if(_p){_p[0]=D2GR_HDR(op,4);_p[1]=(unsigned int)(a);_p[2]=(unsigned int)(b);_p[3]=(unsigned int)(c);} }while(0)
#define REC4(op,a,b,c,d)             do{ unsigned int*_p=ring_alloc(5); if(_p){_p[0]=D2GR_HDR(op,5);_p[1]=(unsigned int)(a);_p[2]=(unsigned int)(b);_p[3]=(unsigned int)(c);_p[4]=(unsigned int)(d);} }while(0)
#define REC5(op,a,b,c,d,e)           do{ unsigned int*_p=ring_alloc(6); if(_p){_p[0]=D2GR_HDR(op,6);_p[1]=(unsigned int)(a);_p[2]=(unsigned int)(b);_p[3]=(unsigned int)(c);_p[4]=(unsigned int)(d);_p[5]=(unsigned int)(e);} }while(0)
#define REC7(op,a,b,c,d,e,f,g)       do{ unsigned int*_p=ring_alloc(8); if(_p){_p[0]=D2GR_HDR(op,8);_p[1]=(unsigned int)(a);_p[2]=(unsigned int)(b);_p[3]=(unsigned int)(c);_p[4]=(unsigned int)(d);_p[5]=(unsigned int)(e);_p[6]=(unsigned int)(f);_p[7]=(unsigned int)(g);} }while(0)

/* ---- STATE DEDUPLICATION ------------------------------------------------
 * D2 does not track its own current Glide state, so it re-issues state calls
 * (grColorCombine/grAlphaBlendFunction/grTexSource/...) identically between
 * draws. Each redundant record costs ring bytes, a host-side walk step
 * (gx_state + FNV hash), and a batch-state rebuild. We drop them HERE, in the
 * translated DLL (0 crossings): a cache PER FUNCTION of the last recorded
 * arguments; a call whose arguments match the last record for that same
 * function is not written.
 *
 * SAFETY RULES (the cache must never cause the host to miss a state change):
 *   - the cache is INVALIDATED IN BULK on every grBufferSwap (generation): the
 *     first occurrence of each state in a frame is ALWAYS written, regardless
 *     of the previous frame;
 *   - grTexSource is invalidated by any upload (grTexDownloadMipMap) and any
 *     palette change (grTexDownloadTable): a "rebind" to the same TMU address
 *     is re-written;
 *   - grSstWinOpen invalidates everything;
 *   - grEnable/grDisable are NOT deduplicated (two calls with the same
 *     argument bracketing an inverse call are not idempotent);
 *   - a DROPPED record (ring full) does not update the cache;
 *   - deduplication is requested by the host (g_cfg.dedup, D2_GRDEDUP): an
 *     older host leaves the field at zero and gets the old, undeduplicated
 *     stream.
 * This only changes the SEQUENCE of records (the ring's `h=`), never what the
 * host does with them (`lh=` batching, GPU-half oracle). */
#define D2GR_STC_OPS 0x30
static unsigned int g_stc[D2GR_STC_OPS][8];       /* last written arguments, per op */
static unsigned int g_stcGen[D2GR_STC_OPS];       /* generation they were written in */
static unsigned int g_gen = 1;                    /* current generation (>= 1) */
static unsigned int g_stcDrop = 0;                /* records skipped (diagnostic) */

static void st_rec(unsigned int op, const unsigned int* a, unsigned int n)
{
    unsigned int i, *p;
    if (g_cfg.dedup && g_stcGen[op] == g_gen) {
        const unsigned int* c = g_stc[op];
        for (i = 0; i < n; ++i) if (c[i] != a[i]) break;
        if (i == n) { ++g_stcDrop; return; }        /* identical to the last write: nothing to do */
    }
    p = ring_alloc(n + 1);
    if (!p) return;                                 /* dropped: leave the cache unchanged */
    p[0] = D2GR_HDR(op, n + 1);
    for (i = 0; i < n; ++i) { p[i + 1] = a[i]; g_stc[op][i] = a[i]; }
    g_stcGen[op] = g_gen;
}
#define ST1(op,a)               do{ unsigned int _a[1]={(unsigned int)(a)}; st_rec(op,_a,1); }while(0)
#define ST2(op,a,b)             do{ unsigned int _a[2]={(unsigned int)(a),(unsigned int)(b)}; st_rec(op,_a,2); }while(0)
#define ST3(op,a,b,c)           do{ unsigned int _a[3]={(unsigned int)(a),(unsigned int)(b),(unsigned int)(c)}; st_rec(op,_a,3); }while(0)
#define ST4(op,a,b,c,d)         do{ unsigned int _a[4]={(unsigned int)(a),(unsigned int)(b),(unsigned int)(c),(unsigned int)(d)}; st_rec(op,_a,4); }while(0)
#define ST5(op,a,b,c,d,e)       do{ unsigned int _a[5]={(unsigned int)(a),(unsigned int)(b),(unsigned int)(c),(unsigned int)(d),(unsigned int)(e)}; st_rec(op,_a,5); }while(0)
#define ST7(op,a,b,c,d,e,f,g)   do{ unsigned int _a[7]={(unsigned int)(a),(unsigned int)(b),(unsigned int)(c),(unsigned int)(d),(unsigned int)(e),(unsigned int)(f),(unsigned int)(g)}; st_rec(op,_a,7); }while(0)

/* ---- state ---------------------------------------------------------------- */
FXAPI grAlphaBlendFunction(unsigned int a,unsigned int b,unsigned int c,unsigned int d){ ST4(D2GR_OP_ALPHABLENDFUNCTION,a,b,c,d); }
FXAPI grAlphaCombine(unsigned int a,unsigned int b,unsigned int c,unsigned int d,unsigned int e){ ST5(D2GR_OP_ALPHACOMBINE,a,b,c,d,e); }
FXAPI grColorCombine(unsigned int a,unsigned int b,unsigned int c,unsigned int d,unsigned int e){ ST5(D2GR_OP_COLORCOMBINE,a,b,c,d,e); }
FXAPI grColorMask(unsigned int rgb,unsigned int a){ ST2(D2GR_OP_COLORMASK,rgb,a); }
FXAPI grConstantColorValue(unsigned int v){ ST1(D2GR_OP_CONSTANTCOLORVALUE,v); }
FXAPI grChromakeyMode(unsigned int m){ ST1(D2GR_OP_CHROMAKEYMODE,m); }
FXAPI grChromakeyValue(unsigned int v){ ST1(D2GR_OP_CHROMAKEYVALUE,v); }
FXAPI grClipWindow(unsigned int a,unsigned int b,unsigned int c,unsigned int d){ ST4(D2GR_OP_CLIPWINDOW,a,b,c,d); }
FXAPI grCoordinateSpace(unsigned int m){ ST1(D2GR_OP_COORDINATESPACE,m); }
FXAPI grDepthBufferFunction(unsigned int f){ ST1(D2GR_OP_DEPTHBUFFERFUNCTION,f); }
FXAPI grDepthBufferMode(unsigned int m){ ST1(D2GR_OP_DEPTHBUFFERMODE,m); }
FXAPI grDepthMask(unsigned int m){ ST1(D2GR_OP_DEPTHMASK,m); }
FXAPI grDisable(unsigned int m){ REC1(D2GR_OP_DISABLE,m); }          /* never deduplicated (toggle) */
FXAPI grDitherMode(unsigned int m){ ST1(D2GR_OP_DITHERMODE,m); }
FXAPI grEnable(unsigned int m){ REC1(D2GR_OP_ENABLE,m); }            /* never deduplicated (toggle) */
FXAPI grTexCombine(unsigned int a,unsigned int b,unsigned int c,unsigned int d,unsigned int e,unsigned int f,unsigned int g){ ST7(D2GR_OP_TEXCOMBINE,a,b,c,d,e,f,g); }
FXAPI grTexFilterMode(unsigned int a,unsigned int b,unsigned int c){ ST3(D2GR_OP_TEXFILTERMODE,a,b,c); }
FXAPI grTexMipMapMode(unsigned int a,unsigned int b,unsigned int c){ ST3(D2GR_OP_TEXMIPMAPMODE,a,b,c); }
FXAPI grFinish(void){ }
FXAPI grFlush(void){ }

/* grVertexLayout(param, offset, mode): this is where the vertex SIZE for
 * grDrawVertexArray comes from (which doesn't provide it itself). We keep the
 * largest `offset` seen, plus 16 bytes of margin (a parameter can be 4 words).
 * Deduplicated like a state: only a call STRICTLY identical to the last one
 * recorded (same param, offset, mode) is omitted. */
FXAPI grVertexLayout(unsigned int param,unsigned int offset,unsigned int mode)
{
    if (mode && offset + 16u > g_vtxSize) g_vtxSize = (offset + 16u + 3u) & ~3u;
    ST3(D2GR_OP_VERTEXLAYOUT,param,offset,mode);
}

/* ---- textures -------------------------------------------------------------- */
FXAPI grTexSource(unsigned int tmu,unsigned int startAddress,unsigned int evenOdd,unsigned int* info)
{
    unsigned int lod=0,aspect=0,fmt=0;
    if (info) { lod = info[1]; aspect = info[2]; fmt = info[3]; }
    ST7(D2GR_OP_TEXSOURCE,tmu,0,startAddress,evenOdd,lod,aspect,fmt);
}
FXAPI grTexDownloadMipMap(unsigned int tmu,unsigned int startAddress,unsigned int evenOdd,void* info)
{
    ring_boot();
    g_stcGen[D2GR_OP_TEXSOURCE] = 0;        /* rebind: the next grTexSource is re-written, even if identical */
    if (d2vGlideTexUpload) d2vGlideTexUpload(tmu,startAddress,evenOdd,info);  /* CROSSING #3 */
}
FXAPI grTexDownloadTable(unsigned int type,unsigned int* data)
{
    unsigned int* p = ring_alloc(2 + 256);
    g_stcGen[D2GR_OP_TEXSOURCE] = 0;        /* palette: same, to be safe (a rare call) */
    if (!p) return;
    p[0]=D2GR_HDR(D2GR_OP_TEXDOWNLOADTABLE,2+256); p[1]=type;
    if (data) d2gr_copy(p+2,data,256*4); else d2gr_zero(p+2,256*4);
}
FXAPIU grTexMaxAddress(unsigned int tmu){ (void)tmu; ring_boot(); return g_cfg.tex_max; }
FXAPIU grTexMinAddress(unsigned int tmu){ (void)tmu; ring_boot(); return g_cfg.tex_min; }
FXAPIU grTexTextureMemRequired(unsigned int evenOdd,unsigned int* info)
{
    unsigned int lod, ar, fmt, big, w, h, bpp, n;
    (void)evenOdd;
    if (!info) return 0x1000u;
    lod = info[1]; ar = info[2]; fmt = info[3];
    if (lod > 8) return 0x1000u;
    big = 1u << lod; w = big; h = big;
    if ((int)ar > 0 && (int)ar <= 3) h = big >> ar;
    else if ((int)ar < 0 && (int)ar >= -3) w = big >> (-(int)ar);
    bpp = (fmt >= 8) ? 2u : 1u;
    n = w * h * bpp; if (n < 8) n = 8;
    { unsigned int p2 = 8; while (p2 < n) p2 <<= 1; return p2; }
}

/* ---- drawing --------------------------------------------------------------- */
FXAPI grBufferClear(unsigned int color,unsigned int alpha,unsigned int depth){ REC3(D2GR_OP_BUFFERCLEAR,color,alpha,depth); }

FXAPI grDrawVertexArrayContiguous(unsigned int mode,unsigned int count,void* vertex,unsigned int stride)
{
    unsigned int bytes, words, *p;
    if (!count || !vertex || !stride || stride > D2GR_MAX_STRIDE) return;
    /* The stride given here is the TRUE vertex size. grDrawVertexArray, on the
     * other hand, doesn't provide it: we borrow this one instead of deriving a
     * worst-case value from grVertexLayout (which overestimates, copying padding). */
    g_vtxSize = stride;
    bytes = count * stride; words = 4 + ((bytes + 3) >> 2);
    p = ring_alloc(words);
    if (!p) return;
    p[0]=D2GR_HDR(D2GR_OP_DRAWVERTEXARRAYCONT,words); p[1]=mode; p[2]=count; p[3]=stride;
    d2gr_copy(p+4,vertex,bytes);
}
FXAPI grDrawVertexArray(unsigned int mode,unsigned int count,void* pointers)
{
    unsigned int stride = g_vtxSize, bytes, words, i, *p;
    void** pp = (void**)pointers;
    if (!count || !pointers || stride > D2GR_MAX_STRIDE) return;
    bytes = count * stride; words = 4 + ((bytes + 3) >> 2);
    p = ring_alloc(words);
    if (!p) return;
    p[0]=D2GR_HDR(D2GR_OP_DRAWVERTEXARRAY,words); p[1]=mode; p[2]=count; p[3]=stride;
    for (i = 0; i < count; ++i)
        if (pp[i]) d2gr_copy(((unsigned char*)(p+4)) + i*stride, pp[i], stride);
}
FXAPI grDrawTriangle(void* a,void* b,void* c)
{
    unsigned int stride=g_vtxSize, words=2+((3*stride+3)>>2), *p;
    if (stride > D2GR_MAX_STRIDE) return;
    p = ring_alloc(words); if(!p) return;
    p[0]=D2GR_HDR(D2GR_OP_DRAWTRIANGLE,words); p[1]=stride;
    if(a) d2gr_copy((unsigned char*)(p+2),a,stride);
    if(b) d2gr_copy((unsigned char*)(p+2)+stride,b,stride);
    if(c) d2gr_copy((unsigned char*)(p+2)+2*stride,c,stride);
}
FXAPI grDrawLine(void* a,void* b)
{
    unsigned int stride=g_vtxSize, words=2+((2*stride+3)>>2), *p;
    if (stride > D2GR_MAX_STRIDE) return;
    p = ring_alloc(words); if(!p) return;
    p[0]=D2GR_HDR(D2GR_OP_DRAWLINE,words); p[1]=stride;
    if(a) d2gr_copy((unsigned char*)(p+2),a,stride);
    if(b) d2gr_copy((unsigned char*)(p+2)+stride,b,stride);
}
FXAPI grDrawPoint(void* a)
{
    unsigned int stride=g_vtxSize, words=2+((stride+3)>>2), *p;
    if (stride > D2GR_MAX_STRIDE) return;
    p = ring_alloc(words); if(!p) return;
    p[0]=D2GR_HDR(D2GR_OP_DRAWPOINT,words); p[1]=stride;
    if(a) d2gr_copy((unsigned char*)(p+2),a,stride);
}

/* ---- frame boundary: THE once-per-frame crossing -------------------------- */
FXAPI grBufferSwap(unsigned int interval)
{
    (void)interval;
    ring_boot();
    ++g_frame; g_hdr.frame = g_frame;
    REC1(D2GR_OP_FRAME_END, g_frame);
    g_hdr.dedup_evites = g_stcDrop;        /* diagnostic: states skipped since start (read by the host) */
    ++g_gen; if (!g_gen) g_gen = 1;        /* new frame: ALL state will be re-written on its first occurrence */
    if (d2vGlideFlush) d2vGlideFlush(g_hdr.head, g_frame);   /* CROSSING #2, once per frame */
}

/* ---- lifecycle / queries -------------------------------------------------- */
FXAPIU grGlideInit(void){ ring_boot(); return 1; }
FXAPI  grGlideShutdown(void){ }
FXAPIU grSstSelect(unsigned int n){ (void)n; return 1; }
FXAPIU grSstWinOpen(unsigned int hwnd,unsigned int res,unsigned int ref,unsigned int fmt,
                    unsigned int org,unsigned int nb,unsigned int nAux)
{
    (void)hwnd;
    ring_boot();
    /* Resolution (res), color format (fmt) and origin (org) are STATE the
     * host reader needs to know: without `res` it doesn't know which window
     * to project vertices onto, without `fmt` it can't read
     * grConstantColorValue (RGBA vs ARGB), without `org` it doesn't know
     * whether y=0 is at the top. Recording them costs one record per session. */
    REC7(D2GR_OP_WINOPEN,res,ref,fmt,org,nb,nAux,0);
    ++g_gen; if (!g_gen) g_gen = 1;        /* new window: all state is re-written */
    return 1;
}
FXAPIU grSstWinClose(unsigned int ctx){ (void)ctx; return 1; }
FXAPIU grGet(unsigned int param,unsigned int len,unsigned int* out)
{
    unsigned int v = 1;
    ring_boot();
    if (param == 0x05) v = g_cfg.gamma_entries;   /* gamma table entries */
    else if (param == 0x2a) v = g_cfg.gamma_bits; /* gamma bits */
    if (out && len >= 4) { *out = v; return 4; }
    return 0;
}
FXAPIU grGetString(unsigned int name){ (void)name; return 0; }
FXAPIU grGetProcAddress(char* n){ (void)n; return 0; }
FXAPI  grLoadGammaTable(unsigned int n,unsigned int* r,unsigned int* g,unsigned int* b){ (void)n;(void)r;(void)g;(void)b; }
FXAPI  guGammaCorrectionRGB(unsigned int r,unsigned int g,unsigned int b){ (void)r;(void)g;(void)b; }
FXAPIU grLfbLock(unsigned int t,unsigned int b,unsigned int wt,unsigned int o,unsigned int p,void* i)
{ (void)t;(void)b;(void)wt;(void)o;(void)p;(void)i; return 0; }   /* FXFALSE: D2 has a fallback */
FXAPIU grLfbUnlock(unsigned int t,unsigned int b){ (void)t;(void)b; return 1; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{ (void)h;(void)r; if (reason == DLL_PROCESS_ATTACH) { } return TRUE; }
