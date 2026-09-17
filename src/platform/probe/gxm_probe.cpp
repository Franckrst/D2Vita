// src/platform/probe/gxm_probe.cpp — BARE APP: sceGxm display mechanics alone,
// no D2, no dynarec, no Glide ring.
//
// WHY THIS EXISTS: a freeze with no fault, no crash.log, and no psp2core gives
// no clue where it happened. In a binary where a dynarec, a guest scheduler, a
// network stack, and three host threads all coexist, "it froze" doesn't point
// at anyone. This app keeps ONLY the suspect mechanics — same sceGxm params,
// same CDRAM, same carousel, same display queue, same shaders — so if it
// freezes, the freeze names its own step; if it runs cleanly, the display
// mechanics are cleared and the bug is elsewhere in the game.
//
// TITLE_ID DTWO00009, log at ux0:data/d2vita/gxmprobe_progress.txt. Runs
// ALONGSIDE the game; replaces nothing.
//
// Knobs: ux0:data/d2vita/gxmprobe.txt, one KEY=VALUE line per line, same names
// as the game: D2_GXMASYNC (0 synchronous / 1 display queue / 2 deferred),
// D2_GXMRING, D2_GXMVSYNC, D2_GXMCDRAM, D2_GXMSECONDS (duration, default 30).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

namespace {

constexpr int SCR_W = 960, SCR_H = 544;
constexpr int MAXRING = 4;

// ---- logging ----------------------------------------------------------------
// Each line is timestamped and the file is closed after every write, so a
// freeze can't take down the lines that would explain it.
const char* kLog = "ux0:data/d2vita/gxmprobe_progress.txt";
uint64_t g_t0 = 0;
void plog(const char* m) {
    const uint64_t now = sceKernelGetProcessTimeWide();
    char line[320];
    const int n = std::snprintf(line, sizeof line, "[%6u ms] %s\n",
                                (unsigned)((now - g_t0) / 1000ull), m);
    SceUID fd = sceIoOpen(kLog, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, (SceSize)(n > 0 ? n : 0)); sceIoClose(fd); }
}
void plogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void plogf(const char* fmt, ...) {
    char b[288]; va_list ap; va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    plog(b);
}

// ---- knobs -----------------------------------------------------------------
struct Kv { char k[48]; char v[48]; };
Kv  g_kv[16]; int g_kvN = 0;
void load_knobs() {
    SceUID fd = sceIoOpen("ux0:data/d2vita/gxmprobe.txt", SCE_O_RDONLY, 0);
    if (fd < 0) { plog("knobs: gxmprobe.txt absent — valeurs par defaut"); return; }
    char buf[1024]; const int n = sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char* p = buf;
    while (*p && g_kvN < 16) {
        char* e = std::strpbrk(p, "\r\n");
        if (e) *e = 0;
        char* eq = std::strchr(p, '=');
        if (eq && eq != p && p[0] != '#') {
            *eq = 0;
            std::snprintf(g_kv[g_kvN].k, sizeof g_kv[0].k, "%s", p);
            std::snprintf(g_kv[g_kvN].v, sizeof g_kv[0].v, "%s", eq + 1);
            plogf("knobs: %s=%s", g_kv[g_kvN].k, g_kv[g_kvN].v);
            ++g_kvN;
        }
        if (!e) break;
        p = e + 1;
        while (*p == '\n' || *p == '\r') ++p;
    }
}
int knob(const char* name, int def) {
    for (int i = 0; i < g_kvN; ++i) if (!std::strcmp(g_kv[i].k, name)) return atoi(g_kv[i].v);
    return def;
}

// ---- state ------------------------------------------------------------------
int g_asyncMode = 1, g_ring = 2, g_vsync = 1, g_cdram = 1, g_seconds = 30;

struct GpuBlock { SceUID uid = -1; void* p = nullptr; uint32_t size = 0; };
uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

bool gpu_alloc(GpuBlock& b, SceKernelMemBlockType type, uint32_t size, uint32_t attribs, const char* name) {
    const bool cd = (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
    b.size = align_up(size, cd ? (256u << 10) : (4u << 10));
    b.uid = sceKernelAllocMemBlock(name, type, b.size, nullptr);
    if (b.uid < 0) { plogf("  AllocMemBlock(%s,%s,%u Ko) rc=0x%08x", name, cd ? "CDRAM" : "user",
                           b.size >> 10, (unsigned)b.uid); b.uid = -1; return false; }
    const int g = sceKernelGetMemBlockBase(b.uid, &b.p);
    if (g < 0) { plogf("  GetMemBlockBase(%s) rc=0x%08x", name, (unsigned)g);
                 sceKernelFreeMemBlock(b.uid); b.uid = -1; b.p = nullptr; return false; }
    const int m = sceGxmMapMemory(b.p, b.size, (SceGxmMemoryAttribFlags)attribs);
    if (m < 0) { plogf("  sceGxmMapMemory(%s,%u Ko) rc=0x%08x", name, b.size >> 10, (unsigned)m);
                 sceKernelFreeMemBlock(b.uid); b.uid = -1; b.p = nullptr; return false; }
    return true;
}
bool gpu_alloc_gpu(GpuBlock& b, uint32_t size, uint32_t attribs, const char* name) {
    if (g_cdram && gpu_alloc(b, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, attribs, name)) return true;
    return gpu_alloc(b, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, attribs, name);
}
bool usse_alloc(GpuBlock& b, uint32_t size, bool frag, unsigned int* off, const char* name) {
    b.size = align_up(size, 4096);
    b.uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, b.size, nullptr);
    if (b.uid < 0) { plogf("  AllocMemBlock USSE(%s) rc=0x%08x", name, (unsigned)b.uid); b.uid = -1; return false; }
    if (sceKernelGetMemBlockBase(b.uid, &b.p) < 0) { plogf("  GetMemBlockBase USSE(%s)", name); return false; }
    const int rc = frag ? sceGxmMapFragmentUsseMemory(b.p, b.size, off)
                        : sceGxmMapVertexUsseMemory(b.p, b.size, off);
    if (rc < 0) { plogf("  Map%sUsseMemory(%s) rc=0x%08x", frag ? "Fragment" : "Vertex", name, (unsigned)rc);
                  return false; }
    return true;
}
void* ph_alloc(void*, unsigned int n) { return std::malloc(n); }
void  ph_free(void*, void* p) { std::free(p); }

// Matches the game's vertex layout exactly: the shipped shader expects this.
struct Vtx { float x, y; float u, v; uint32_t argb; float pal; };

SceGxmContext* g_ctx = nullptr;
SceGxmRenderTarget* g_rt = nullptr;
GpuBlock g_vdm, g_vring, g_fring, g_depth, g_patchBuf, g_fusseRing, g_vusse, g_fusse;
unsigned int g_fusseRingOff = 0, g_vusseOff = 0, g_fusseOff = 0;
void* g_hostMem = nullptr;
SceGxmShaderPatcher* g_patcher = nullptr;
SceGxmShaderPatcherId g_vpId = nullptr, g_fpId = nullptr;
SceGxmVertexProgram* g_vp = nullptr;
SceGxmFragmentProgram* g_fp = nullptr;
const SceGxmProgramParameter *g_pScale = nullptr, *g_pConst = nullptr, *g_pMode = nullptr, *g_pKey = nullptr;
GpuBlock g_fb[MAXRING], g_vtx[MAXRING], g_idx[MAXRING], g_pal, g_tex;
SceGxmColorSurface g_color[MAXRING];
SceGxmSyncObject* g_sync[MAXRING] = { nullptr, nullptr, nullptr, nullptr };
SceGxmDepthStencilSurface g_ds;
SceGxmTexture g_palTex, g_dumTex;
int g_back = 0;

volatile const char* g_phase = "demarrage";
volatile uint64_t g_phaseT = 0;
volatile uint32_t g_submitted = 0, g_presented = 0;
uint64_t g_waitUs = 0, g_presUs = 0, g_subUs = 0;
int g_pendN = 0;
struct Pend { int slot; uint32_t frame; };
Pend g_pend[MAXRING];
volatile unsigned int* g_notifBase = nullptr;
SceGxmNotification g_notif[MAXRING];
uint32_t g_notifSeq = 0;

void phase(const char* p) { g_phase = p; g_phaseT = sceKernelGetProcessTimeWide(); }

struct DispData { uint32_t slot; uint32_t frame; };
void display_cb(const void* data) {
    if (!data) return;
    const DispData* d = (const DispData*)data;
    const uint64_t a = sceKernelGetProcessTimeWide();
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb; fb.base = g_fb[d->slot].p; fb.pitch = SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCR_W; fb.height = SCR_H;
    sceDisplaySetFrameBuf(&fb, g_vsync == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE : SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (g_vsync >= 2) sceDisplayWaitVblankStart();
    g_presUs += sceKernelGetProcessTimeWide() - a;
    ++g_presented;
}

int watchdog(SceSize, void*) {
    uint32_t last = 0; int quiet = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (g_submitted != last) { last = g_submitted; quiet = 0; continue; }
        ++quiet;
        if (quiet == 3 || (quiet > 3 && (quiet % 10) == 0))
            plogf("CHIEN DE GARDE — %d s sans image | phase=%s depuis %u ms | soumises=%u presentees=%u en-file=%d",
                  quiet, (const char*)g_phase,
                  (unsigned)((sceKernelGetProcessTimeWide() - g_phaseT) / 1000ull),
                  (unsigned)g_submitted, (unsigned)g_presented, g_pendN);
    }
}

uint8_t* slurp(const char* path, uint32_t* n) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return nullptr;
    const SceOff sz = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { sceIoClose(fd); return nullptr; }
    uint8_t* b = (uint8_t*)std::malloc((size_t)sz);
    const int rd = b ? sceIoRead(fd, b, (SceSize)sz) : -1;
    sceIoClose(fd);
    if (rd != (int)sz) { std::free(b); return nullptr; }
    *n = (uint32_t)sz;
    return b;
}

void mem(const char* what) {
    SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
    sceKernelGetFreeMemorySize(&fi);
    plogf("%s | libre user=%d Ko cdram=%d Ko", what, fi.size_user >> 10, fi.size_cdram >> 10);
}

void retire_one() {
    if (g_pendN <= 0) return;
    const Pend pd = g_pend[0];
    for (int i = 1; i < g_pendN; ++i) g_pend[i-1] = g_pend[i];
    --g_pendN;
    const uint64_t w0 = sceKernelGetProcessTimeWide();
    phase("sceGxmNotificationWait");
    sceGxmNotificationWait(&g_notif[pd.slot]);
    const uint64_t w1 = sceKernelGetProcessTimeWide();
    g_waitUs += (w1 - w0);
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb; fb.base = g_fb[pd.slot].p; fb.pitch = SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCR_W; fb.height = SCR_H;
    phase("sceDisplaySetFrameBuf (differe)");
    sceDisplaySetFrameBuf(&fb, g_vsync == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE : SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (g_vsync >= 2) sceDisplayWaitVblankStart();
    g_presUs += sceKernelGetProcessTimeWide() - w1;
    ++g_presented;
}

bool init() {
    mem("etape 0 : entree");

    SceGxmInitializeParams ip;
    std::memset(&ip, 0, sizeof ip);
    ip.displayQueueMaxPendingCount = (unsigned)(g_ring - 1);
    ip.displayQueueCallback = display_cb;
    ip.displayQueueCallbackDataSize = sizeof(DispData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int rc = sceGxmInitialize(&ip);
    plogf("etape 1 : sceGxmInitialize rc=0x%08x (max en attente=%u, carrousel=%d)",
          (unsigned)rc, ip.displayQueueMaxPendingCount, g_ring);
    if (rc < 0 && (unsigned)rc != 0x805B0004u) return false;

    const uint32_t FRING = 512u << 10;
    if (!gpu_alloc_gpu(g_vdm, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, "pr_vdm")) return false;
    if (!gpu_alloc_gpu(g_vring, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ, "pr_vring")) return false;
    if (!gpu_alloc_gpu(g_fring, FRING, SCE_GXM_MEMORY_ATTRIB_READ, "pr_fring")) return false;
    if (!usse_alloc(g_fusseRing, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, true, &g_fusseRingOff, "pr_fusser")) return false;
    mem("etape 2 : tampons circulaires");

    SceGxmContextParams cp;
    std::memset(&cp, 0, sizeof cp);
    g_hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMem = g_hostMem; cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = g_vdm.p; cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = g_vring.p; cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = g_fring.p; cp.fragmentRingBufferMemSize = FRING;
    cp.fragmentUsseRingBufferMem = g_fusseRing.p;
    cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = g_fusseRingOff;
    rc = sceGxmCreateContext(&cp, &g_ctx);
    plogf("etape 3 : sceGxmCreateContext rc=0x%08x", (unsigned)rc);
    if (rc < 0 || !g_ctx) return false;

    for (int i = 0; i < g_ring; ++i) {
        if (!gpu_alloc(g_fb[i], SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SCR_W * SCR_H * 4,
                       SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "pr_fb")) return false;
        uint32_t* px = (uint32_t*)g_fb[i].p;
        for (int k = 0; k < SCR_W * SCR_H; ++k) px[k] = 0xFF000000u;
        rc = sceGxmColorSurfaceInit(&g_color[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                    SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                    SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCR_W, SCR_H, SCR_W, g_fb[i].p);
        if (rc < 0) { plogf("  ColorSurfaceInit[%d] rc=0x%08x", i, (unsigned)rc); return false; }
        const int s = sceGxmSyncObjectCreate(&g_sync[i]);
        if (s < 0 || !g_sync[i]) { plogf("  SyncObjectCreate[%d] rc=0x%08x", i, (unsigned)s); return false; }
    }
    mem("etape 4 : tampons d'affichage + objets de synchro");

    SceGxmRenderTargetParams rtp;
    std::memset(&rtp, 0, sizeof rtp);
    rtp.width = SCR_W; rtp.height = SCR_H; rtp.scenesPerFrame = 1;
    rtp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rtp.driverMemBlock = -1;
    rc = sceGxmCreateRenderTarget(&rtp, &g_rt);
    plogf("etape 5 : sceGxmCreateRenderTarget rc=0x%08x", (unsigned)rc);
    if (rc < 0 || !g_rt) return false;

    const uint32_t dw = align_up(SCR_W, SCE_GXM_TILE_SIZEX), dh = align_up(SCR_H, SCE_GXM_TILE_SIZEY);
    if (!gpu_alloc_gpu(g_depth, dw * dh * 4, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "pr_depth")) return false;
    sceGxmDepthStencilSurfaceInit(&g_ds, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                  SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, (int)dw, g_depth.p, nullptr);
    mem("etape 6 : profondeur");

    if (!gpu_alloc_gpu(g_patchBuf, 64 << 10, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "pr_patch")) return false;
    if (!usse_alloc(g_vusse, 64 << 10, false, &g_vusseOff, "pr_vusse")) return false;
    if (!usse_alloc(g_fusse, 64 << 10, true, &g_fusseOff, "pr_fusse")) return false;
    SceGxmShaderPatcherParams pp;
    std::memset(&pp, 0, sizeof pp);
    pp.hostAllocCallback = ph_alloc; pp.hostFreeCallback = ph_free;
    pp.bufferMem = g_patchBuf.p; pp.bufferMemSize = g_patchBuf.size;
    pp.vertexUsseMem = g_vusse.p; pp.vertexUsseMemSize = 64 << 10; pp.vertexUsseOffset = g_vusseOff;
    pp.fragmentUsseMem = g_fusse.p; pp.fragmentUsseMemSize = 64 << 10; pp.fragmentUsseOffset = g_fusseOff;
    rc = sceGxmShaderPatcherCreate(&pp, &g_patcher);
    plogf("etape 7 : sceGxmShaderPatcherCreate rc=0x%08x", (unsigned)rc);
    if (rc < 0 || !g_patcher) return false;

    // SAME SHADERS AS THE GAME: that's the point. Same fallback try order.
    uint32_t vs = 0, fs = 0;
    uint8_t* vprogB = slurp("ux0:data/d2vita/shaders/d2_ring_v.gxp", &vs);
    if (!vprogB) vprogB = slurp("app0:shaders/d2_ring_v.gxp", &vs);
    uint8_t* fprogB = slurp("ux0:data/d2vita/shaders/d2_ring_f.gxp", &fs);
    if (!fprogB) fprogB = slurp("app0:shaders/d2_ring_f.gxp", &fs);
    if (!vprogB || !fprogB) { plog("etape 8 : SHADERS INTROUVABLES (ux0 puis app0) — ARRET"); return false; }
    const SceGxmProgram* vprog = (const SceGxmProgram*)vprogB;
    const SceGxmProgram* fprog = (const SceGxmProgram*)fprogB;
    plogf("etape 8 : shaders v=%u o (check=%d) f=%u o (check=%d)",
          vs, sceGxmProgramCheck(vprog), fs, sceGxmProgramCheck(fprog));
    if (sceGxmProgramCheck(vprog) != 0 || sceGxmProgramCheck(fprog) != 0) return false;

    if (sceGxmShaderPatcherRegisterProgram(g_patcher, vprog, &g_vpId) < 0 ||
        sceGxmShaderPatcherRegisterProgram(g_patcher, fprog, &g_fpId) < 0) { plog("  RegisterProgram KO"); return false; }
    const SceGxmProgramParameter* aPos = sceGxmProgramFindParameterByName(vprog, "aPos");
    const SceGxmProgramParameter* aTex = sceGxmProgramFindParameterByName(vprog, "aTex");
    const SceGxmProgramParameter* aCol = sceGxmProgramFindParameterByName(vprog, "aCol");
    const SceGxmProgramParameter* aPal = sceGxmProgramFindParameterByName(vprog, "aPal");
    if (!aPos || !aTex || !aCol || !aPal) { plog("  attributs de sommet introuvables"); return false; }
    SceGxmVertexAttribute at[4]; SceGxmVertexStream st[1];
    at[0].streamIndex = 0; at[0].offset = 0;  at[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; at[0].componentCount = 2; at[0].regIndex = sceGxmProgramParameterGetResourceIndex(aPos);
    at[1].streamIndex = 0; at[1].offset = 8;  at[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; at[1].componentCount = 2; at[1].regIndex = sceGxmProgramParameterGetResourceIndex(aTex);
    at[2].streamIndex = 0; at[2].offset = 16; at[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N; at[2].componentCount = 4; at[2].regIndex = sceGxmProgramParameterGetResourceIndex(aCol);
    at[3].streamIndex = 0; at[3].offset = 20; at[3].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; at[3].componentCount = 1; at[3].regIndex = sceGxmProgramParameterGetResourceIndex(aPal);
    st[0].stride = sizeof(Vtx); st[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    rc = sceGxmShaderPatcherCreateVertexProgram(g_patcher, g_vpId, at, 4, st, 1, &g_vp);
    if (rc < 0) { plogf("  CreateVertexProgram rc=0x%08x", (unsigned)rc); return false; }
    static const SceGxmBlendInfo kOpaque = {
        SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B,
        SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO };
    rc = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fpId, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                  SCE_GXM_MULTISAMPLE_NONE, &kOpaque, vprog, &g_fp);
    if (rc < 0) { plogf("  CreateFragmentProgram rc=0x%08x", (unsigned)rc); return false; }
    g_pScale = sceGxmProgramFindParameterByName(vprog, "uScale");
    g_pConst = sceGxmProgramFindParameterByName(fprog, "uConst");
    g_pMode  = sceGxmProgramFindParameterByName(fprog, "uMode");
    g_pKey   = sceGxmProgramFindParameterByName(fprog, "uKey");
    if (!g_pScale || !g_pConst || !g_pMode) { plog("  uniformes introuvables"); return false; }
    plog("etape 9 : programmes de sommet et de fragment crees");

    for (int i = 0; i < g_ring; ++i) {
        if (!gpu_alloc_gpu(g_vtx[i], 64 * sizeof(Vtx), SCE_GXM_MEMORY_ATTRIB_READ, "pr_vtx")) return false;
        if (!gpu_alloc_gpu(g_idx[i], 64 * 2, SCE_GXM_MEMORY_ATTRIB_READ, "pr_idx")) return false;
    }
    if (!gpu_alloc_gpu(g_pal, 256 * 16 * 4, SCE_GXM_MEMORY_ATTRIB_READ, "pr_pal")) return false;
    std::memset(g_pal.p, 0xFF, 256 * 16 * 4);
    sceGxmTextureInitLinear(&g_palTex, g_pal.p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 256, 16, 0);
    if (!gpu_alloc_gpu(g_tex, 64, SCE_GXM_MEMORY_ATTRIB_READ, "pr_tex")) return false;
    std::memset(g_tex.p, 0, 64);
    sceGxmTextureInitLinear(&g_dumTex, g_tex.p, SCE_GXM_TEXTURE_FORMAT_U8_RRRR, 8, 8, 0);
    mem("etape 10 : sommets, indices, palette, texture factice");

    g_notifBase = sceGxmGetNotificationRegion();
    plogf("etape 11 : region de notification = %p", (void*)g_notifBase);
    if (g_notifBase) for (int i = 0; i < MAXRING; ++i) {
        g_notif[i].address = g_notifBase + i; *g_notif[i].address = 0xFFFFFFFFu; g_notif[i].value = 0; }
    else if (g_asyncMode == 2) { plog("  pas de region de notification — mode 2 impossible"); return false; }

    g_back = g_ring - 1;
    return true;
}

// One frame: a fullscreen quad (mirrors the game's clear) plus a triangle
// that changes color, so a frozen screen is visually obvious.
void frame(uint32_t n) {
    const int prev = g_back;
    const int slot = (g_back + 1) % g_ring;
    const uint64_t t0 = sceKernelGetProcessTimeWide();

    if (g_asyncMode == 2)
        for (int guard = 0; guard <= MAXRING; ++guard) {
            bool busy = false;
            for (int i = 0; i < g_pendN; ++i) if (g_pend[i].slot == slot) busy = true;
            if (!busy) break;
            retire_one();
        }

    Vtx* v = (Vtx*)g_vtx[slot].p;
    uint16_t* ix = (uint16_t*)g_idx[slot].p;
    const float W = 960.0f, H = 544.0f;
    const float xs[4] = { 0, W, W, 0 }, ys[4] = { 0, 0, H, H };
    for (int k = 0; k < 4; ++k) { v[k].x = xs[k]; v[k].y = ys[k]; v[k].u = 0; v[k].v = 0;
                                  v[k].argb = 0xFFFFFFFFu; v[k].pal = 0.0f; }
    // moving triangle
    const float ph = (float)(n % 120) / 120.0f;
    v[4].x = 200.0f + 400.0f * ph; v[4].y = 100.0f;
    v[5].x = 100.0f + 400.0f * ph; v[5].y = 400.0f;
    v[6].x = 500.0f + 400.0f * ph; v[6].y = 400.0f;
    for (int k = 4; k < 7; ++k) { v[k].u = 0; v[k].v = 0; v[k].argb = 0xFFFFFFFFu; v[k].pal = 0.0f; }
    ix[0]=0; ix[1]=1; ix[2]=2; ix[3]=0; ix[4]=2; ix[5]=3;
    ix[6]=4; ix[7]=5; ix[8]=6;

    phase("sceGxmBeginScene");
    const int rc = sceGxmBeginScene(g_ctx, 0, g_rt, nullptr, nullptr,
                                    g_asyncMode == 2 ? nullptr : g_sync[slot], &g_color[slot], &g_ds);
    if (rc < 0) { static bool said = false; if (!said) { said = true; plogf("BeginScene rc=0x%08x", (unsigned)rc); } return; }
    sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetCullMode(g_ctx, SCE_GXM_CULL_NONE);
    sceGxmSetVertexProgram(g_ctx, g_vp);
    float scale[4] = { 2.0f / 960.0f, -2.0f / 544.0f, -1.0f, 1.0f };
    void* vu = nullptr; sceGxmReserveVertexDefaultUniformBuffer(g_ctx, &vu);
    if (vu) sceGxmSetUniformDataF(vu, g_pScale, 0, 4, scale);
    sceGxmSetVertexStream(g_ctx, 0, g_vtx[slot].p);
    sceGxmSetFragmentProgram(g_ctx, g_fp);
    sceGxmSetFragmentTexture(g_ctx, 0, &g_dumTex);
    sceGxmSetFragmentTexture(g_ctx, 1, &g_palTex);
    // midnight-blue background
    { void* fu = nullptr; sceGxmReserveFragmentDefaultUniformBuffer(g_ctx, &fu);
      if (fu) { float c[4] = { 0.05f, 0.05f, 0.20f, 1.0f }, m[4] = { 0, 0, 1, 0 }, k[4] = { 0, 0, 0, 0 };
                sceGxmSetUniformDataF(fu, g_pConst, 0, 4, c);
                sceGxmSetUniformDataF(fu, g_pMode, 0, 4, m);
                if (g_pKey) sceGxmSetUniformDataF(fu, g_pKey, 0, 4, k); } }
    sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, ix, 6);
    // hue-shifting triangle: makes a frozen screen visually obvious
    { void* fu = nullptr; sceGxmReserveFragmentDefaultUniformBuffer(g_ctx, &fu);
      if (fu) { const float t = (float)(n % 90) / 90.0f;
                float c[4] = { t, 1.0f - t, 0.5f, 1.0f }, m[4] = { 0, 0, 1, 0 }, k[4] = { 0, 0, 0, 0 };
                sceGxmSetUniformDataF(fu, g_pConst, 0, 4, c);
                sceGxmSetUniformDataF(fu, g_pMode, 0, 4, m);
                if (g_pKey) sceGxmSetUniformDataF(fu, g_pKey, 0, 4, k); } }
    sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, ix + 6, 3);

    if (g_asyncMode == 2) {
        *g_notif[slot].address = 0xFFFFFFFFu;
        if (++g_notifSeq == 0xFFFFFFFFu) g_notifSeq = 1;
        g_notif[slot].value = g_notifSeq;
    }
    phase("sceGxmEndScene");
    sceGxmEndScene(g_ctx, nullptr, g_asyncMode == 2 ? &g_notif[slot] : nullptr);
    const uint64_t t1 = sceKernelGetProcessTimeWide();
    g_subUs += (t1 - t0);

    if (g_asyncMode == 0) {
        phase("sceGxmFinish");
        sceGxmFinish(g_ctx);
        const uint64_t t2 = sceKernelGetProcessTimeWide();
        g_waitUs += (t2 - t1);
        SceDisplayFrameBuf fb;
        std::memset(&fb, 0, sizeof fb);
        fb.size = sizeof fb; fb.base = g_fb[slot].p; fb.pitch = SCR_W;
        fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = SCR_W; fb.height = SCR_H;
        phase("sceDisplaySetFrameBuf");
        sceDisplaySetFrameBuf(&fb, g_vsync == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE : SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (g_vsync >= 2) sceDisplayWaitVblankStart();
        g_presUs += sceKernelGetProcessTimeWide() - t2;
        ++g_presented;
    } else if (g_asyncMode == 2) {
        if (g_pendN < MAXRING) { g_pend[g_pendN].slot = slot; g_pend[g_pendN].frame = n; ++g_pendN; }
        if (g_pendN > 1) retire_one();
    } else {
        sceGxmPadHeartbeat(&g_color[slot], g_sync[slot]);
        DispData dd; dd.slot = (uint32_t)slot; dd.frame = n;
        phase("sceGxmDisplayQueueAddEntry");
        const int a = sceGxmDisplayQueueAddEntry(g_sync[prev], g_sync[slot], &dd);
        const uint64_t t2 = sceKernelGetProcessTimeWide();
        g_waitUs += (t2 - t1);
        if (a < 0) { static bool said = false; if (!said) { said = true; plogf("AddEntry rc=0x%08x", (unsigned)a); } }
    }
    phase("boucle");
    g_back = slot;
    ++g_submitted;
}

} // namespace

int main() {
    g_t0 = sceKernelGetProcessTimeWide();
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/d2vita", 0777);
    sceIoRemove(kLog);
    plog("=== app nue sceGxm (DTWO00009) : la mecanique d'affichage, toute seule ===");
    load_knobs();
    g_asyncMode = knob("D2_GXMASYNC", 1); if (g_asyncMode < 0 || g_asyncMode > 2) g_asyncMode = 1;
    g_ring      = knob("D2_GXMRING", 2);  if (g_ring < 2) g_ring = 2; if (g_ring > MAXRING) g_ring = MAXRING;
    g_vsync     = knob("D2_GXMVSYNC", 1); if (g_vsync < 0 || g_vsync > 2) g_vsync = 1;
    g_cdram     = knob("D2_GXMCDRAM", 1) ? 1 : 0;
    g_seconds   = knob("D2_GXMSECONDS", 30); if (g_seconds < 5) g_seconds = 5; if (g_seconds > 600) g_seconds = 600;
    plogf("config : mode=%d (0 synchrone / 1 file / 2 differe) carrousel=%d vsync=%d cdram=%d duree=%d s",
          g_asyncMode, g_ring, g_vsync, g_cdram, g_seconds);

    { const SceUID th = sceKernelCreateThread("pr_wd", watchdog, 0x10000100, 0x4000, 0, 0, nullptr);
      if (th >= 0) sceKernelStartThread(th, 0, nullptr);
      plogf("chien de garde uid=0x%08x", (unsigned)th); }

    if (!init()) { plog("INIT KO — voir la derniere etape ci-dessus. ARRET."); sceKernelDelayThread(3000000);
                   sceKernelExitProcess(1); return 1; }
    plog("INIT OK — debut de la boucle d'images");

    const uint64_t tEnd = sceKernelGetProcessTimeWide() + (uint64_t)g_seconds * 1000000ull;
    uint64_t tWin = sceKernelGetProcessTimeWide();
    uint32_t n = 0, nWin = 0;
    while (sceKernelGetProcessTimeWide() < tEnd) {
        frame(n); ++n; ++nWin;
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (now - tWin >= 1000000ull) {
            const uint64_t dt = now - tWin;
            plogf("images=%u | %u.%u img/s | soumission-us=%u attente-us=%u presentation-us=%u"
                  " | soumises=%u presentees=%u en-file=%d",
                  n, (unsigned)((uint64_t)nWin * 10000000ull / dt) / 10u,
                  (unsigned)((uint64_t)nWin * 10000000ull / dt) % 10u,
                  (unsigned)(g_subUs / (nWin ? nWin : 1)),
                  (unsigned)(g_waitUs / (nWin ? nWin : 1)),
                  (unsigned)(g_presUs / (nWin ? nWin : 1)),
                  (unsigned)g_submitted, (unsigned)g_presented, g_pendN);
            g_subUs = g_waitUs = g_presUs = 0; nWin = 0; tWin = now;
        }
    }
    plog("fin de la duree demandee — vidage puis sortie");
    if (g_asyncMode == 2) while (g_pendN > 0) retire_one();
    sceGxmFinish(g_ctx);
    sceGxmDisplayQueueFinish();
    plogf("TERMINE : %u images en %d s, %u presentees", n, g_seconds, (unsigned)g_presented);
    sceKernelDelayThread(1000000);
    sceKernelExitProcess(0);
    return 0;
}
