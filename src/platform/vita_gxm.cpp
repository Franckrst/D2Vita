// src/platform/vita_gxm.cpp — Glide ring rendering on the Vita GPU.
// Vita-only: off target, vita_gxm.h provides empty inline functions and this
// file isn't even compiled (it's only included in the VPK build's sources).
#ifdef __vita__
#include "platform/vita_gxm.h"
#include "platform/vita_present.h"
#include "platform/vita_gpumem.h"   // engine: console GPU memory
#include "platform/radial_menu.h"   // incrustation GPU: atlas + quads du menu

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/types.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/modulemgr.h>
// LOAD DEPENDENCY: linking libSceShaccCg_stub adds an import on SceShaccCg,
// which is NOT a system module present by default on a stock Vita
// (libshacccg.suprx comes from the PSM Dev Assistant and is installed
// manually). An unresolved import can get the whole eboot rejected, breaking
// the unrelated GDI path too. So the compiler is only linked in a BUILD
// flavor (D2VPK_SHACC=1) used once to produce the .gxp files; the shipped
// VPK just reads them and imports nothing.
#ifdef D2_SHACC
#include <psp2/shacccg.h>
#endif

namespace {

// RENDER RESOLUTION. 960x544 is the screen's native size and the default.
// D2_GXMRES=720|640|480 renders smaller and lets the display hardware
// upscale, trading fill rate for resolution (640x368 cuts pixels written by
// 2.2x). If sceDisplaySetFrameBuf rejects the size, init reports it and falls
// back to 960x544 — never a silent black screen.
int SCR_W = 960, SCR_H = 544;
// Sized from real in-game peaks (outside town, minimap overlay on): per-frame
// vertex and batch counts can spike well past a smaller budget, and the
// builder would DROP the last batches on overflow (objects and UI vanishing).
// Indices are 16-bit, so 65535 vertices is the hard ceiling; 1.5 MiB per
// buffer set in CDRAM.
constexpr uint32_t MAXV = 65535;
constexpr uint32_t MAXI = 196608;
constexpr uint32_t MAXB = 8192;
constexpr int MAXRING = 4;           // max depth of the async carousel

// Reads an integer knob once per knob (call sites are in the per-frame path;
// calling getenv per batch would itself cost measurable time).
int knob_int(const char* name, int def) {
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return atoi(e);
}

// ---- GPU allocation ---------------------------------------------------------
// Console GPU memory allocation lives in the engine (winx86,
// platform/vita_gpumem.h): allocating a kernel block, mapping it for the GPU,
// and mapping it into USSE space doesn't depend on any particular game.
//
// The aliases below keep every call site unchanged: ownership lives in the
// engine, this file just uses it rather than reimplementing it.
using GpuBlock = wx86::vita::GpuBlock;
using wx86::vita::gpu_alloc;
using wx86::vita::usse_alloc;
using wx86::vita::gpu_free;

// D2_GXMCDRAM (default 1): everything the GPU reads or writes goes in CDRAM,
// falling back to user RAM. Free user RAM on this console is scarce, and the
// GXM path used to eat several MiB of it (depth buffer, ring buffers,
// vertices/indices) while tens of MiB of CDRAM sat idle. CDRAM is GPU memory:
// these blocks belong there.
//   =0: places everything in user RAM instead, to reproduce the original
//   baseline for comparison.
// USSE memory is the ONLY exception: sceGxmMapVertex/FragmentUsseMemory
// doesn't accept CDRAM. It stays in user RAM (144 KiB total).
inline bool gpu_alloc_gpu(GpuBlock& b, uint32_t size, uint32_t attribs, const char* name) {
    return wx86::vita::gpu_alloc_best(b, size, attribs, name);
}

// INIT MILESTONES: each step is named and publishes free memory, so a freeze
// during init points at a specific step and its memory footprint instead of
// leaving a silent gap.
// LAST STEP REACHED and CURRENT PHASE: the two words the watchdog prints if
// nothing advances. Plain string literals, never a shared buffer — a freeze
// must not depend on an allocation to describe itself.
volatile const char* g_step  = "avant d2gxm_init";
volatile const char* g_phase = "repos";
volatile uint64_t    g_phaseT = 0;
volatile uint32_t    g_submitted = 0;
int g_userFreeAtEntry = 0;

inline void phase(const char* p) { g_phase = p; g_phaseT = sceKernelGetProcessTimeWide(); }

void mem_step(const char* what) {
    g_step = what;
    SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
    sceKernelGetFreeMemorySize(&fi);
    char m[128];
    std::snprintf(m, sizeof m, "gxm[init] %s | libre user=%d Ko cdram=%d Ko",
                  what, fi.size_user >> 10, fi.size_cdram >> 10);
    d2vita_progress(m);
}


void* patcher_host_alloc(void*, unsigned int size) { return std::malloc(size); }
void  patcher_host_free(void*, void* mem) { std::free(mem); }

// ---- shaders (Cg) -----------------------------------------------------------
// One pair of programs: all observed Glide state variability collapses into
// FOUR uniforms; only blending, which on GXM is part of the fragment program,
// needs multiple variants.
//
// Vertex color arrives as U8N x4 from a 0xAARRGGBB word: in little-endian
// memory the bytes are B,G,R,A, hence the `.zyx` swizzle on the shader side.
// Don't "fix" this without re-checking — the reversed swizzle produces a
// false-color image that looks like a palette bug.
const char* kVertexCg =
"void main(\n"
"  float2 aPos,\n"
"  float2 aTex,\n"
"  float4 aCol,\n"
"  float  aPal,\n"
"  uniform float4 uScale,\n"
"  float4 out vPosition : POSITION,\n"
"  float2 out vTex : TEXCOORD0,\n"
"  float4 out vCol : COLOR0,\n"
"  float  out vPal : TEXCOORD1)\n"
"{\n"
"  vPosition = float4(aPos.x * uScale.x + uScale.z, aPos.y * uScale.y + uScale.w, 0.5f, 1.0f);\n"
"  vTex = aTex;\n"
"  vCol = float4(aCol.z, aCol.y, aCol.x, aCol.w);\n"
"  vPal = aPal;\n"
"}\n";

// uMode.x = 1 -> color = texture * vertex color; 0 -> flat constant color
// uMode.y = 1 -> chromakey active (compared AFTER the palette lookup, per Glide's rule)
// uMode.z = 1 -> alpha = constant color's alpha; 0 -> alpha zero
const char* kFragmentCg =
"float4 main(\n"
"  float2 vTex : TEXCOORD0,\n"
"  float4 vCol : COLOR0,\n"
"  float  vPal : TEXCOORD1,\n"
"  uniform sampler2D uPage : TEXUNIT0,\n"
"  uniform sampler2D uPalette : TEXUNIT1,\n"
"  uniform float4 uConst,\n"
"  uniform float4 uMode,\n"
"  uniform float4 uKey) : COLOR\n"
"{\n"
"  float idx = tex2D(uPage, vTex).x;\n"
"  float4 texel = tex2D(uPalette, float2(idx * 0.99609375f + 0.001953125f, vPal));\n"
"  float d = abs(texel.x - uKey.x) + abs(texel.y - uKey.y) + abs(texel.z - uKey.z);\n"
"  if (uMode.y > 0.5f && d < 0.002f) discard;\n"
"  float3 rgb = lerp(uConst.xyz, texel.xyz * vCol.xyz, uMode.x);\n"
"  return float4(rgb.x, rgb.y, rgb.z, uConst.w * uMode.z);\n"
"}\n";

// MEASUREMENT VARIANT (D2_GXMPROBE=3): the SAME shader without the DEPENDENT
// texture read (8-bit index -> palette) and without `discard`. On an SGX
// these are the two usual suspects for per-pixel cost: a dependent read
// serializes the texture unit, and discard flips the draw into
// "punch-through", handled separately and more slowly than opaque.
// Output is WRONG (grayscale): this variant exists only to measure cost,
// never to actually render. Same parameter names and texture units as the
// normal shader so the submission code stays identical.
// Without the matching .gxp, the knob says so and falls back to normal
// rendering.
const char* kFragmentFlatCg =
"float4 main(\n"
"  float2 vTex : TEXCOORD0,\n"
"  float4 vCol : COLOR0,\n"
"  float  vPal : TEXCOORD1,\n"
"  uniform sampler2D uPage : TEXUNIT0,\n"
"  uniform sampler2D uPalette : TEXUNIT1,\n"
"  uniform float4 uConst,\n"
"  uniform float4 uMode,\n"
"  uniform float4 uKey) : COLOR\n"
"{\n"
"  float idx = tex2D(uPage, vTex).x;\n"
"  float3 rgb = lerp(uConst.xyz, float3(idx, idx, idx) * vCol.xyz, uMode.x);\n"
"  return float4(rgb.x, rgb.y, rgb.z, uConst.w * uMode.z);\n"
"}\n";

// ---------------------------------------------------------------------------
// FLAT CLEAR (`D2_GXMCLEAR=plat`).
// Clearing the screen through the full shader costs roughly 3 ms by itself.
// The cost isn't from covering the screen — it's from covering it WITH THE
// FULL SHADER: 522,240 fragments (960x544) each paying for the dependent
// texture read and the `discard`, just to write black.
//
// sceGxm has NO native clear: no `sceGxmClear`, and the color surface has no
// load mode either (`sceGxmColorSurfaceSet*` only exposes clip, scale, data,
// format, gamma, dithering — no ForceLoad, unlike the depth surface). On this
// tiler, tile memory isn't loaded from the render target buffer: the
// fullscreen quad IS the intended clear mechanism. What can be removed is its
// SHADER, not the quad itself.
//
// This program samples NOTHING, discards NOTHING, and has no varying input:
// it just writes a constant color. That's the absolute floor for a
// full-screen draw's cost, and the gap versus the default is what the full
// shader costs on top of it.
const char* kFragmentClearCg =
"float4 main(uniform float4 uConst) : COLOR\n"
"{\n"
"  return uConst;\n"
"}\n";

// ---------------------------------------------------------------------------
// "HARDWARE PALETTE" VARIANT (D2_GXMPAL=1).
// The page is a SCE_GXM_TEXTURE_FORMAT_P8_ABGR: the texture unit itself does
// the index -> color translation in ONE read. The shader no longer needs a
// second tex2D indexed by the first one's result — the DEPENDENT read, the
// classic cost of palettized rendering on SGX, is gone.
// `uPalette` is deliberately NOT declared here, so "a single texture read"
// is verifiable by disassembly rather than just asserted in a comment.
// `vPal` stays declared (the vertex program is shared and the
// vertex->fragment interface must stay complete) but is no longer read: the
// palette slot now travels in the texture DESCRIPTOR, so a single batch can
// no longer mix two palettes — see Builder::splitByPalette.
const char* kFragmentPalCg =
"float4 main(\n"
"  float2 vTex : TEXCOORD0,\n"
"  float4 vCol : COLOR0,\n"
"  float  vPal : TEXCOORD1,\n"
"  uniform sampler2D uPage : TEXUNIT0,\n"
"  uniform float4 uConst,\n"
"  uniform float4 uMode,\n"
"  uniform float4 uKey) : COLOR\n"
"{\n"
"  float4 texel = tex2D(uPage, vTex);\n"
"  float d = abs(texel.x - uKey.x) + abs(texel.y - uKey.y) + abs(texel.z - uKey.z);\n"
"  if (uMode.y > 0.5f && d < 0.002f) discard;\n"
"  float3 rgb = lerp(uConst.xyz, texel.xyz * vCol.xyz, uMode.x);\n"
"  return float4(rgb.x, rgb.y, rgb.z, uConst.w * uMode.z);\n"
"}\n";

// ---------------------------------------------------------------------------
// INCRUSTATION RGBA (menu radial). Le pipeline du jeu est palettisé et tire
// son alpha d'un uniforme ; une incrustation d'interface a besoin de l'alpha
// DE SA TEXTURE, par pixel. D'où ce programme, le plus court du fichier : il
// rend le texel tel quel, et c'est l'étage de mélange (kBlend[1], SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA) qui fait le reste, dans la mémoire de tuiles du GPU.
//
// vCol et vPal ne sont pas lus mais restent DÉCLARÉS : le programme de sommet
// est partagé, et l'interface sommet -> fragment doit rester complète (même
// raison que pour vPal dans kFragmentPalCg).
const char* kFragmentRgbaCg =
"float4 main(\n"
"  float2 vTex : TEXCOORD0,\n"
"  float4 vCol : COLOR0,\n"
"  float  vPal : TEXCOORD1,\n"
"  uniform sampler2D uPage : TEXUNIT0) : COLOR\n"
"{\n"
"  return tex2D(uPage, vTex);\n"
"}\n";

// ---- runtime Cg compilation + disk cache ------------------------------------
// psp2cgc isn't free and isn't in the VitaSDK, so .gxp files can't be produced
// on the host. The console's own compiler (libshacccg.suprx, module
// SCE_SYSMODULE_SHACCCG) does it at runtime. To avoid depending on it
// forever, the result is WRITTEN to disk: once the .gxp files exist (from
// Vita3K or console), they can ship alongside the VPK and the module is no
// longer needed.
//
// Try order: (1) ux0:data/d2vita/shaders/<name>.gxp — shipped or already
//                compiled;
//            (2) app0:shaders/<name>.gxp — bundled in the VPK;
//            (3) compile via SceShaccCg, then write to (1).
#ifdef D2_SHACC
void* shacc_malloc(unsigned int n) { return std::malloc(n); }
void  shacc_free(void* p) { std::free(p); }
SceShaccCgSourceFile g_srcFile;

SceShaccCgSourceFile* cb_open(const char*, const SceShaccCgSourceLocation*,
                              const SceShaccCgCompileOptions*, const char**) { return &g_srcFile; }
void cb_release(const SceShaccCgSourceFile*, const SceShaccCgCompileOptions*) {}
const char* cb_locate(const char* fn, const SceShaccCgSourceLocation*, SceUInt32,
                      const char* const*, const SceShaccCgCompileOptions*, const char**) { return fn; }
const char* cb_abs(const char* fn, const SceShaccCgSourceLocation*, const SceShaccCgCompileOptions*) { return fn; }
void cb_relname(const char*, const SceShaccCgCompileOptions*) {}
SceInt32 cb_date(const SceShaccCgSourceFile*, const SceShaccCgSourceLocation*,
                 const SceShaccCgCompileOptions*, int64_t*, int64_t*) { return -1; }
#endif // D2_SHACC

uint8_t* slurp_file(const char* path, uint32_t* outSize) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return nullptr;
    SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (n <= 0 || n > (SceOff)(1 << 20)) { sceIoClose(fd); return nullptr; }
    uint8_t* buf = (uint8_t*)std::malloc((size_t)n);
    if (!buf) { sceIoClose(fd); return nullptr; }
    int rd = sceIoRead(fd, buf, (SceSize)n);
    sceIoClose(fd);
    if (rd != (int)n) { std::free(buf); return nullptr; }
    *outSize = (uint32_t)n;
    return buf;
}

void spit_file(const char* path, const void* data, uint32_t n) {
    sceIoMkdir("ux0:data/d2vita/shaders", 0777);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, data, n);
    sceIoClose(fd);
}

bool g_shaccLoaded = false;
char g_shaderOrigin[3][40] = { "", "", "" };   // 0 = vertex, 1 = main fragment, 2 = flat probe

// `profile`: 0 = vertex, 1 = fragment. A plain int, not the ShaccCg enum — the
// signature must exist even in builds that don't link the compiler.
const SceGxmProgram* get_shader(const char* name, const char* src, int profile, int slot) {
    char p1[96], p2[96];
    std::snprintf(p1, sizeof p1, "ux0:data/d2vita/shaders/%s.gxp", name);
    std::snprintf(p2, sizeof p2, "app0:shaders/%s.gxp", name);
    uint32_t sz = 0;
    if (uint8_t* b = slurp_file(p1, &sz)) {
        if (sceGxmProgramCheck((const SceGxmProgram*)b) == 0) {
            std::snprintf(g_shaderOrigin[slot], sizeof g_shaderOrigin[slot], "ux0 (%u o)", sz);
            return (const SceGxmProgram*)b; }
        std::free(b);
    }
    if (uint8_t* b = slurp_file(p2, &sz)) {
        if (sceGxmProgramCheck((const SceGxmProgram*)b) == 0) {
            std::snprintf(g_shaderOrigin[slot], sizeof g_shaderOrigin[slot], "app0 (%u o)", sz);
            return (const SceGxmProgram*)b; }
        std::free(b);
    }
#ifndef D2_SHACC
    (void)src; (void)profile;
    { char m[160];
      std::snprintf(m, sizeof m, "gxm: %s introuvable et build SANS compilateur — poser %s (build D2VPK_SHACC=1 pour le fabriquer)",
                    name, p1);
      d2vita_progress(m); }
    return nullptr;
#else
    if (!g_shaccLoaded) {
        // NOT sceSysmoduleLoadModule: module 0x3E isn't a system module.
        // `libshacccg.suprx` is installed by hand in ur0:data and loaded like
        // an ordinary module. Going through sysmodule instead returns
        // 0xc05a1000 on Vita3K and leaves SceShaccCg's NIDs unresolved.
        g_shaccLoaded = true;
        const char* paths[2] = { "ur0:data/libshacccg.suprx", "ux0:data/libshacccg.suprx" };
        int id = -1;
        for (int i = 0; i < 2 && id < 0; ++i)
            id = sceKernelLoadStartModule(paths[i], 0, nullptr, 0, nullptr, nullptr);
        char m[144];
        std::snprintf(m, sizeof m, "gxm: libshacccg.suprx -> 0x%08x (%s)", (unsigned)id,
                      id >= 0 ? "charge" : "absent, la compilation va echouer");
        d2vita_progress(m);
        if (id >= 0) sceShaccCgSetDefaultAllocator(shacc_malloc, shacc_free);
    }
    g_srcFile.fileName = name;
    g_srcFile.text = src;
    g_srcFile.size = (SceUInt32)std::strlen(src);

    SceShaccCgCompileOptions opts;
    sceShaccCgInitializeCompileOptions(&opts);
    opts.mainSourceFile = name;
    opts.targetProfile = profile ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
    opts.entryFunctionName = "main";
    opts.useFx = 0;

    SceShaccCgCallbackList cbs;
    sceShaccCgInitializeCallbackList(&cbs, SCE_SHACCCG_TRIVIAL);
    cbs.openFile = cb_open;
    cbs.releaseFile = cb_release;
    cbs.locateFile = cb_locate;
    cbs.absolutePath = cb_abs;
    cbs.releaseFileName = cb_relname;
    cbs.fileDate = cb_date;

    const SceShaccCgCompileOutput* out = sceShaccCgCompileProgram(&opts, &cbs, 0);
    if (!out || !out->programData || !out->programSize) {
        char m[192];
        const char* msg = (out && out->diagnosticCount && out->diagnostics && out->diagnostics[0].message)
                        ? out->diagnostics[0].message : "(sans diagnostic)";
        std::snprintf(m, sizeof m, "gxm: compilation %s ECHOUEE : %s", name, msg);
        d2vita_progress(m);
        if (out) sceShaccCgDestroyCompileOutput(out);
        return nullptr;
    }
    uint8_t* keep = (uint8_t*)std::malloc(out->programSize);
    std::memcpy(keep, out->programData, out->programSize);
    spit_file(p1, keep, out->programSize);
    std::snprintf(g_shaderOrigin[slot], sizeof g_shaderOrigin[slot], "compile (%u o)", (unsigned)out->programSize);
    sceShaccCgDestroyCompileOutput(out);
    return (const SceGxmProgram*)keep;
#endif // D2_SHACC
}

// ---- global state ------------------------------------------------------------
bool g_knobRead = false, g_knob = false, g_ready = false, g_failed = false;
int  g_gameW = 800, g_gameH = 600;

// ---- PIPELINE KNOBS and GPU-COST VARIANT KNOBS -----------------------------
// All default to the shipped behavior: with none set, nothing on the
// measured console path changes by a single byte.
bool g_asyncRead = false, g_async = false;   // D2_GXMASYNC=1|2
// 1 = sceGxm's own display queue (the driver's display thread presents).
// 2 = DEFERRED ON THE GAME THREAD: no dependency on a thread we don't
//     control. Frame N is submitted with a fragment-done NOTIFICATION; it's
//     only waited on at the end of submitting frame N+1, by which time the
//     GPU has had the whole game frame to finish. Same overlap, without the
//     display thread — the fallback if mode 1 hangs.
int  g_asyncMode = 0;
int  g_ring = 2;                             // D2_GXMRING (2..4), defaults to 3 when async
int  g_vsyncMode = 1;                        // D2_GXMVSYNC: 0=IMMEDIATE 1=NEXTFRAME(default) 2=+wait for vblank
int  g_noClear = 0;                          // D2_GXMNOCLEAR=1: no clear quad
int  g_forceBlend = -1;                      // D2_GXMBLEND=0..3: forces a single blend program
int  g_probe = 0;                            // D2_GXMPROBE: 1=red quad 2=no chromakey 3=no palette
bool g_overlayOk = true;                     // overlays: only at 960x544
// D2_GXMYIELD: microseconds the game thread sleeps after a submit that did
// NOT block. Not a cosmetic detail: the Vita kernel schedules RUN-TO-BLOCK
// among threads of equal priority on a core, so a thread that never blocks
// starves its neighbors. In synchronous mode, the blocking sceGxmFinish call
// WAS that yield point; removing it without replacing it turns the game
// thread into a CPU hog. 0 disables it.
int  g_yieldUs = 50;
// ---- GPU-COST VARIANT KNOBS -------------------------------------------------
// All default to the shipped behavior: with none set, neither the texture
// format, the fragment program, nor the atlas byte layout changes at all.
int  g_hwPal = 0;      // D2_GXMPAL=1: atlas in P8_ABGR, palette lookup done by the texture unit
int  g_shotFrom = 0, g_shotEvery = 1500;   // D2_GXMSHOT / D2_GXMSHOTPAS
volatile int g_shotPending = 0;           // on-demand capture (controller: L + Start)
// D2_GXMFILTRE: 0 = POINT everywhere (default, in both palette modes)
//               1 = honors Glide's filter flag (bilinear on the floor). With
//                   the hardware palette this is correct filtering (the GPU
//                   filters COLORS); with the software palette it produces a
//                   broken-looking floor.
//               2 = point everywhere (alias of 0, kept for existing env.txt files)
// LINEAR filtering on a P8_ABGR (indexed) texture is UNVALIDATED on real
// hardware and has caused a GPU hang, and Vita3K doesn't validate P8 texture
// formats at all. Until P8+LINEAR passes on console, it stays opt-in: the
// hardware palette must prove itself alone first, filtering second (=1).
int  g_filtre = 0;
uint64_t g_filtreRefuse = 0;                // bilinear batches rendered as point instead (windowed count)
uint32_t g_shotN = 0;
int  g_gpuMHz = 222;   // D2_GPUCLOCK: forwarded to vita_present (read-only here)
// D2_GXMCLEAR: 0 = quad + full shader (default)
//              1 = quad + FLAT shader (nothing sampled, nothing discarded)
//              2 = no quad at all (equivalent to D2_GXMNOCLEAR=1, kept so
//                  this knob is self-contained)
int  g_clearMode = 0;
char g_clearName[8] = "quad";

SceGxmContext* g_ctx = nullptr;
SceGxmRenderTarget* g_rt = nullptr;
GpuBlock g_vdm, g_vring, g_fring, g_patchBuf;
GpuBlock g_vtxBuf[MAXRING], g_idxBuf[MAXRING], g_palBlk[MAXRING];
void* g_hostMem = nullptr;
GpuBlock g_fusseRing, g_vusse, g_fusse;
unsigned int g_fusseRingOff = 0, g_vusseOff = 0, g_fusseOff = 0;

SceGxmShaderPatcher* g_patcher = nullptr;
SceGxmShaderPatcherId g_vpId = nullptr, g_fpId = nullptr, g_fpFlatId = nullptr;
SceGxmVertexProgram* g_vp = nullptr;
SceGxmFragmentProgram* g_fp[4] = { nullptr, nullptr, nullptr, nullptr };
SceGxmFragmentProgram* g_fpFlat[4] = { nullptr, nullptr, nullptr, nullptr };
// Incrustation d'interface sur le GPU : un programme (mélange alpha seul) et
// un atlas RGBA. g_uiOk ne passe à vrai que si TOUT est en place — un menu à
// moitié armé dessinerait du noir sur le jeu.
SceGxmFragmentProgram* g_fpUi = nullptr;
SceGxmShaderPatcherId  g_fpUiId = 0;
GpuBlock  g_uiTexBlk{};
SceGxmTexture g_uiTex;
bool g_uiOk = false;
bool g_flatOk = false;
const SceGxmProgramParameter* g_pScale = nullptr;
const SceGxmProgramParameter* g_pConst = nullptr;
const SceGxmProgramParameter* g_pMode = nullptr;
const SceGxmProgramParameter* g_pKey = nullptr;
// Uniform OFFSETS belong to their program. Reusing the normal shader's
// offsets for the flat variant would write to the wrong place — a silently
// wrong image. Hence a separate set per program.
const SceGxmProgramParameter* g_pConstF = nullptr;
const SceGxmProgramParameter* g_pModeF = nullptr;
const SceGxmProgramParameter* g_pKeyF = nullptr;
// Name of the actual fragment program loaded — published as-is in the mode
// line, so a requested-but-missing variant is visible.
const char* g_fragName = "d2_ring_f";
// FLAT clear program (D2_GXMCLEAR=plat). A single blend mode is enough: the
// clear is opaque by construction.
SceGxmShaderPatcherId g_fpClearId = nullptr;
SceGxmFragmentProgram* g_fpClear = nullptr;
const SceGxmProgramParameter* g_pConstC = nullptr;

GpuBlock g_fb[MAXRING];
SceGxmColorSurface g_color[MAXRING];
SceGxmSyncObject* g_sync[MAXRING] = { nullptr, nullptr, nullptr, nullptr };
SceGxmDepthStencilSurface g_ds;
int g_back = 0;                 // carousel slot used by the current frame
int g_front = 0;                // slot previously submitted to the display queue

// GPU atlas
constexpr int MAXPAGES = 256;
GpuBlock g_page[MAXPAGES];
SceGxmTexture g_pageTex[MAXPAGES];
int g_pageDim = 0, g_pageN = 0;
GpuBlock g_dummy;               // dummy page: a sampler is NEVER left unbound
SceGxmTexture g_dummyTex;
SceGxmTexture g_palTex[MAXRING];

// SCRATCH TEXTURE DESCRIPTORS (hardware palette). A SceGxmTexture is 16
// bytes; with the hardware palette one is needed PER (page, palette) pair
// since the palette is carried by the descriptor. Rather than precomputing
// 256 x 16 descriptors per buffer set, they're built on the fly in a ring:
// each descriptor stays valid until its buffer set is reused, i.e. g_ring
// frames later — well after the GPU is done with it. That's exactly the
// lifetime needed, and it assumes nothing about sceGxmSetFragmentTexture's
// copy semantics.
constexpr int TEXSCRATCH = 1024;              // headroom: typically ~148 batches/frame
SceGxmTexture g_texScratch[MAXRING][TEXSCRATCH];
int g_texScratchN = 0;
uint64_t g_texScratchOver = 0;   // batches rendered with the WRONG palette, ring exhausted
bool     g_texScratchOverSaid = false;

// hwPal BINDING MILESTONES. A GPU hang freezes the whole console, so only
// what was logged BEFORE submission survives — no psp2core, no Crash.txt.
// Each (page, palette, filter) combo is logged the FIRST time it's bound,
// with its four control words and palette address: if the console hangs
// again, the last line names the descriptor that had just been introduced,
// and the words can be checked offline (format, palette address >> 6,
// filters). Index 0 = dummy page, page+1 otherwise. bit0 = seen in POINT,
// bit1 = seen in LINEAR.
uint8_t  g_hwpalSeen[MAXPAGES + 1][D2GXM_PALETTES];
uint32_t g_hwpalNewThisFrame = 0;   // new descriptors bound in the current scene
uint32_t g_hwpalLines = 0;          // milestone lines emitted so far (capped: a log, not a stream)
bool     g_hwpalRcSaid = false;     // first negative return code from SetPalette/SetMinFilter/SetMagFilter
constexpr uint32_t HWPAL_MAXLINES = 400;

// Address of palette `p` within buffer set `slot`. The 16 slots live in one
// 16 KiB block, one 1 KiB row each, so every row is aligned to 1024 bytes —
// well past the 64-byte alignment sceGxmTextureSetPalette requires.
inline const void* pal_ptr(int slot, int p) {
    return (const uint8_t*)g_palBlk[slot].p + (size_t)p * 256 * 4;
}

// ---- PALETTES: CPU-side shadow + one copy per buffer set -------------------
// The game can rewrite a palette WHILE the GPU is still reading the previous
// one: a single shared palette buffer would corrupt frame N with an event
// from frame N+1 — an intermittent bug, the worst kind. The CPU shadow is the
// source of truth; each slot's copy is refreshed only if a palette changed
// since, which in practice is rare, so the 16 KiB copy almost never happens.
uint32_t g_palShadow[D2GXM_PALETTES * 256];
uint32_t g_palGen = 1;                       // incremented on every change
uint32_t g_palBlkGen[MAXRING] = { 0, 0, 0, 0 };

// ---- in-flight frame tracking ----------------------------------------------
// g_doneFrame is written by sceGxm's display thread and read by the game
// thread: 32 bits so the read is atomic on ARMv7 (a 64-bit value would be
// read as two halves and could produce a frame number that never existed).
volatile uint32_t g_doneFrame = 0;           // last PRESENTED frame
uint32_t g_lastSubmitted = 0;                // last submitted frame
bool     g_anySubmitted = false;
struct DispData { uint32_t slot; uint32_t frame; };

// ---- mode 2: fragment-done notifications + local queue ---------------------
// One notification per slot: the GPU writes `value` to `address` when the
// scene's fragment work is done. It's the only mechanism that says "the GPU
// finished THIS frame" without going through a driver-imposed thread.
volatile unsigned int* g_notifBase = nullptr;
SceGxmNotification g_notif[MAXRING];
uint32_t g_notifSeq = 0;
struct Pend { int slot; uint32_t frame; };
Pend g_pend[MAXRING];
int  g_pendN = 0;

// per-window counters
// PER-STEP FRAME TIMERS. Rationale: without a timer on every step that can
// block, unaccounted time gets attributed by guesswork instead of evidence.
// Every call that can wait has its own timer, and their sum is published
// alongside the total so any gap between them is visible instead of assumed.
uint64_t g_subUs = 0, g_waitUs = 0, g_presUs = 0, g_frames = 0, g_lots = 0, g_verts = 0, g_calls = 0;
uint64_t g_copyUs = 0;      // copying vertices/indices/palette into GPU memory
uint64_t g_beginUs = 0;     // sceGxmBeginScene  <-- prime suspect: waiting on the previous buffer
uint64_t g_batchUs = 0;     // the batch loop (uniforms, textures, sceGxmDraw)
uint64_t g_endUs = 0;       // sceGxmEndScene
uint64_t g_totUs = 0;       // total time spent in d2gxm_submit
uint64_t g_inflightAcc = 0, g_bufWaits = 0, g_drains = 0;
// Display-thread witness counter, written by IT and read by the game thread.
// It's the only proof, from the game side, that the display queue callback
// runs at all: a counter, not a log — the callback must touch NEITHER a file
// NOR a lock.
volatile uint32_t g_cbRuns = 0;
volatile uint32_t g_pixNonNoirs = 0;
bool g_cbAlarm = false;

void set_default_state() {
    // DEPTH: never tested, never written. Set for both front and back — not
    // decorative: without DEPTH_WRITE_DISABLED, tiled rendering would have
    // somewhere to store a depth surface if one were ever provided.
    sceGxmSetFrontDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthFunc(g_ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthWriteEnable(g_ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
    // STENCIL: the depth/stencil surface is DISABLED, so there's nothing to
    // read or write. State is still set explicitly so "cleanly disabled" is
    // a verifiable fact, not an assumed context default.
    sceGxmSetFrontStencilFunc(g_ctx, SCE_GXM_STENCIL_FUNC_ALWAYS,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, 0, 0);
    sceGxmSetBackStencilFunc(g_ctx, SCE_GXM_STENCIL_FUNC_ALWAYS,
                             SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                             SCE_GXM_STENCIL_OP_KEEP, 0, 0);
    sceGxmSetCullMode(g_ctx, SCE_GXM_CULL_NONE);
}

// This callback runs on sceGxm's DISPLAY THREAD, not the game thread. It's
// only called AFTER the GPU has finished the frame, so "done" is a fact here,
// not a hope. It draws the overlays (same draw as the GDI path, already
// called off the game thread), presents the buffer, then PUBLISHES the frame
// number — and that publication is what frees atlas cells.
// WATCHDOG: it doesn't fix anything, it NAMES what's stuck. A log line once
// per second of silence is enough to say what the game thread is waiting on.
// Pinned to USER_2 (the presentation/watchdog core) and pinned as the FIRST
// INSTRUCTION: on this firmware, pinning a thread from its creator doesn't
// stick — only a thread pinning itself does.
extern "C" int d2vita_pin_self(int mask, unsigned* relu);
int gxm_watchdog(SceSize, void*) {
    unsigned relu = 0;
    d2vita_pin_self(SCE_KERNEL_CPU_MASK_USER_2, &relu);
    uint32_t last = 0; int quiet = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        if (g_submitted != last) { last = g_submitted; quiet = 0; continue; }
        ++quiet;
        if (quiet == 3 || (quiet > 3 && (quiet % 15) == 0)) {
            const uint64_t now = sceKernelGetProcessTimeWide();
            char m[248];
            std::snprintf(m, sizeof m,
                "gxm: CHIEN DE GARDE — %d s sans image soumise | derniere etape=%s"
                " | phase=%s depuis %u ms | soumises=%u presentees=%u en-file=%d carrousel=%d mode=%d",
                quiet, (const char*)g_step, (const char*)g_phase,
                (unsigned)((now - g_phaseT) / 1000ull),
                (unsigned)g_submitted, (unsigned)g_cbRuns, g_pendN, g_ring, g_asyncMode);
            d2vita_progress(m);
        }
    }
}

void display_cb(const void* data) {
    if (!data) return;
    const DispData* d = (const DispData*)data;
    if (d->slot >= (uint32_t)MAXRING || !g_fb[d->slot].p) return;
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    if (g_overlayOk) d2vita_overlay((uint32_t*)g_fb[d->slot].p);
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb; fb.base = g_fb[d->slot].p; fb.pitch = (unsigned)SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = (unsigned)SCR_W; fb.height = (unsigned)SCR_H;
    sceDisplaySetFrameBuf(&fb, g_vsyncMode == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE
                                                : SCE_DISPLAY_SETBUF_NEXTFRAME);
    // D2_GXMVSYNC=2: WAITS for vblank here. Not the default — at 28-50 fps, a
    // 60 Hz vblank wait costs up to 16 ms — but it's the only strict guarantee
    // that a buffer rendered for frame N+3 doesn't overwrite one still being
    // scanned out. Only enable it if tearing is visible.
    if (g_vsyncMode >= 2) sceDisplayWaitVblankStart();
    g_presUs += sceKernelGetProcessTimeWide() - t0;
    // NO LOGGING HERE. d2vita_progress opens a file; doing that from sceGxm's
    // display thread means taking filesystem locks on a thread whose core and
    // priority we don't control — exactly the kind of dependency that hangs
    // silently. This callback only touches two counters; the GAME thread logs.
    g_pixNonNoirs = 0;
    if (g_cbRuns < 3) {
        const uint32_t* px = (const uint32_t*)g_fb[d->slot].p;
        const uint32_t tot = (uint32_t)SCR_W * (uint32_t)SCR_H;
        uint32_t nz = 0;
        for (uint32_t k = 0; k < tot; k += 7) if ((px[k] & 0x00FFFFFFu)) ++nz;
        g_pixNonNoirs = nz;
    }
    g_doneFrame = d->frame;          // 32-bit write is atomic on ARMv7
    ++g_cbRuns;                      // LAST: seeing this move proves everything above ran
}

// MODE 2 — retires the oldest in-flight frame: waits on its notification (the
// GPU has already had the whole next frame's compute time, so the wait is
// normally zero), draws overlays, presents, and PUBLISHES the frame number.
// All on the game thread: no third thread, no shared lock.
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
    if (w1 - w0 > 200) ++g_bufWaits;
    if (g_overlayOk) d2vita_overlay((uint32_t*)g_fb[pd.slot].p);
    if (g_cbRuns < 3) {
        const uint32_t* px = (const uint32_t*)g_fb[pd.slot].p;
        const uint32_t tot = (uint32_t)SCR_W * (uint32_t)SCR_H;
        uint32_t nz = 0;
        for (uint32_t k = 0; k < tot; k += 7) if ((px[k] & 0x00FFFFFFu)) ++nz;
        g_pixNonNoirs = nz;
    }
    SceDisplayFrameBuf fb;
    std::memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb; fb.base = g_fb[pd.slot].p; fb.pitch = (unsigned)SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = (unsigned)SCR_W; fb.height = (unsigned)SCR_H;
    phase("sceDisplaySetFrameBuf (differe)");
    sceDisplaySetFrameBuf(&fb, g_vsyncMode == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE
                                                : SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (g_vsyncMode >= 2) sceDisplayWaitVblankStart();
    g_presUs += sceKernelGetProcessTimeWide() - w1;
    g_doneFrame = pd.frame;
    ++g_cbRuns;
}

} // namespace

bool d2gxm_knob() {
    if (!g_knobRead) { g_knobRead = true;
        const char* e = getenv("D2_GLIDEGXM");
        g_knob = !(e && *e && !std::strcmp(e, "0")); }
    return g_knob;
}
bool d2gxm_ready() { return g_ready; }
// Armee seulement si la scene GXM tourne ET que le shader+atlas sont en place.
bool d2gxm_ui_active() { return g_ready && g_uiOk; }
// The INPUT layer (vita_present.cpp, g_game_w/g_game_h) learns the game's
// resolution via d2vita_present(), which is only called on the GDI path. In
// Glide the game changes resolution via grSstWinOpen, so without this call
// input would stay at stale dimensions after an in-game resolution change,
// misaligning the touch cursor. This notifies input here too.
extern "C" { __attribute__((weak)) void d2vita_set_game_size(int w, int h); }
void d2gxm_set_window(int w, int h) {
    if (w > 0) g_gameW = w; if (h > 0) g_gameH = h;
    if (d2vita_set_game_size && w > 0 && h > 0) d2vita_set_game_size(w, h);
}

bool d2gxm_async() {
    if (!g_asyncRead) { g_asyncRead = true;
        // Mode 1 (ASYNC-QUEUE) is the default; D2_GXMASYNC=2 selects mode 2
        // (DEFERRED), D2_GXMASYNC=0 disables async.
        const char* e = getenv("D2_GXMASYNC");
        g_async = !(e && *e && !std::strcmp(e, "0"));
        g_asyncMode = g_async ? (e && atoi(e) == 2 ? 2 : 1) : 0; }
    return g_async;
}

// FIRST frame still in flight. In synchronous mode NOTHING is ever in
// flight: returns the "no protection needed" sentinel, and the atlas
// eviction barrier is a no-op.
uint64_t d2gxm_busy_from() {
    if (!g_ready || !d2gxm_async() || !g_anySubmitted) return ~(uint64_t)0;
    return (uint64_t)g_doneFrame + 1u;
}

void d2gxm_drain() {
    if (!g_ready || !d2gxm_async()) return;
    ++g_drains;
    if (g_asyncMode == 2) { while (g_pendN > 0) retire_one(); }
    else                  { sceGxmDisplayQueueFinish(); }
}

bool d2gxm_init(int gameW, int gameH) {
    if (g_ready) return true;
    if (g_failed) return false;
    if (!d2gxm_knob()) return false;
    g_failed = true;                       // only cleared back to false on full success
    g_gameW = gameW > 0 ? gameW : 800;
    g_gameH = gameH > 0 ? gameH : 600;

    // ---- knobs, read ONCE ---------------------------------------------------
    g_vsyncMode  = knob_int("D2_GXMVSYNC", 1);   if (g_vsyncMode < 0 || g_vsyncMode > 2) g_vsyncMode = 1;
    g_noClear    = knob_int("D2_GXMNOCLEAR", 0) ? 1 : 0;
    g_forceBlend = knob_int("D2_GXMBLEND", -1);  if (g_forceBlend < 0 || g_forceBlend > 3) g_forceBlend = -1;
    g_probe      = knob_int("D2_GXMPROBE", 0);   if (g_probe < 0 || g_probe > 3) g_probe = 0;
    // The CHOICE belongs to this port; the mechanism AND the state belong to
    // the engine. It's set there and read back from there when needed — no
    // local copy, since two sources of truth for one bool eventually diverge.
    wx86::vita::gpu_prefer_cdram(knob_int("D2_GXMCDRAM", 1) != 0);
    g_hwPal      = knob_int("D2_GXMPAL", 0) ? 1 : 0;
    // D2_GXMSHOT=<frame> [D2_GXMSHOTPAS=<step>, default 1500]: BMP capture of
    // the color buffer, written to ux0:data/d2vita/shot_<frame>.bmp. Default 0
    // = never. Rationale: a broken-looking floor can appear on screen while
    // every fidelity counter reads zero — a counter can't see a wrong
    // texture, only the image can.
    g_shotFrom   = knob_int("D2_GXMSHOT", 0);
    g_filtre     = knob_int("D2_GXMFILTRE", 0);
    g_shotEvery  = knob_int("D2_GXMSHOTPAS", 1500);
    if (g_shotEvery <= 0) g_shotEvery = 1500;
    g_gpuMHz     = knob_int("D2_GPUCLOCK", 222);
    // D2_GXMCLEAR: `quad` | `plat` (default) | `aucun`. `natif` is accepted as
    // an alias for `plat`: sceGxm has no clear primitive (see the comment on
    // kFragmentClearCg), the flat quad IS the native mechanism.
    { const char* e = getenv("D2_GXMCLEAR");
      if (e && *e) {
          if (*e == 'p' || *e == 'P') { g_clearMode = 1; std::strcpy(g_clearName, "plat"); }
          else if (*e == 'n' || *e == 'N') {
              if (e[1] == 'a' && e[2] == 't') {      // "natif"
                  g_clearMode = 1; std::strcpy(g_clearName, "plat");
                  d2vita_progress("gxm: sceGxm n'a AUCUNE primitive d'effacement (ni Clear, ni ForceLoad de couleur)"
                                  " — `natif` vaut `plat`");
              } else { g_clearMode = 2; std::strcpy(g_clearName, "aucun"); }
          }
          else if (*e == 'a' || *e == 'A' || *e == '0') { g_clearMode = 2; std::strcpy(g_clearName, "aucun"); }
          else if (*e == 'q' || *e == 'Q') { g_clearMode = 0; std::strcpy(g_clearName, "quad"); }
      } else { g_clearMode = 1; std::strcpy(g_clearName, "plat"); } }
    if (g_noClear) { g_clearMode = 2; std::strcpy(g_clearName, "aucun"); }
    // The flat probe (D2_GXMPROBE=3) already replaces the fragment program:
    // combining it with the hardware palette would be two changes for one
    // measurement. Pick one, and say so.
    if (g_probe == 3 && g_hwPal) {
        d2vita_progress("gxm: D2_GXMPROBE=3 l'emporte sur D2_GXMPAL (une variante a la fois)");
        g_hwPal = 0;
    }
    // The four sizes the display hardware can upscale.
    switch (knob_int("D2_GXMRES", 960)) {
        case 720: SCR_W = 720; SCR_H = 408; break;
        case 640: SCR_W = 640; SCR_H = 368; break;
        case 480: SCR_W = 480; SCR_H = 272; break;
        default:  SCR_W = 960; SCR_H = 544; break;
    }
    // Overlays (frame counter, keyboard) are drawn for 960x544: at any other
    // size they'd draw in the wrong place. They're disabled (and this is
    // logged) rather than smearing the image.
    g_overlayOk = (SCR_W == 960 && SCR_H == 544);
    if (d2gxm_async()) {
        // With 2 buffer sets, async costs almost exactly the same memory as
        // sync (one extra 16 KiB palette area), and already allows ONE frame
        // in flight, enough to overlap the GPU with the game. A third buffer
        // only pushes back reuse of a buffer still being scanned out; it
        // costs 2 MiB of CDRAM + 0.45 MiB of user RAM, which matters on a
        // console where free user RAM is scarce. Default is 4; drop to 2 via
        // D2_GXMRING if memory-constrained.
        g_ring = knob_int("D2_GXMRING", 4);
        if (g_ring < 2) g_ring = 2;
        if (g_ring > MAXRING) g_ring = MAXRING;
    } else {
        g_ring = 2;                        // synchronous mode: plain double buffering is enough
    }
    g_yieldUs = knob_int("D2_GXMYIELD", 50); if (g_yieldUs < 0) g_yieldUs = 0;

    { SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
      sceKernelGetFreeMemorySize(&fi);
      g_userFreeAtEntry = fi.size_user >> 10; }
    mem_step("entree d2gxm_init");

    // The watchdog starts BEFORE sceGxmInitialize: a hang during init itself
    // must also be named. It's only armed in async mode, so the default
    // (synchronous) path doesn't gain a thread it didn't have — and with the
    // hardware palette (D2_GXMWD, default = D2_GXMPAL), because that path can
    // hang silently with no way to tell whether the game thread is stuck in
    // sceGxmFinish (GPU not yielding) or the whole console is frozen (GPU
    // fault): if the watchdog can still log, it's the former.
    if (d2gxm_async() || knob_int("D2_GXMWD", g_hwPal ? 1 : 0) != 0) {
        const SceUID th = sceKernelCreateThread("d2_gxmwd", gxm_watchdog, 0x10000100, 0x4000, 0, 0, nullptr);
        char m[112];
        if (th >= 0) { const int src = sceKernelStartThread(th, 0, nullptr);
            std::snprintf(m, sizeof m, "gxm: chien de garde uid=0x%08x start=0x%08x", (unsigned)th, (unsigned)src); }
        else std::snprintf(m, sizeof m, "gxm: CreateThread(chien de garde) rc=0x%08x", (unsigned)th);
        d2vita_progress(m);
    }

    SceGxmInitializeParams ip;
    std::memset(&ip, 0, sizeof ip);
    // In async mode, THIS number is what creates back-pressure: once the
    // queue is full, sceGxmDisplayQueueAddEntry BLOCKS the game thread, and
    // that block is exactly the residual GPU wait we want to measure. With
    // g_ring buffer sets, g_ring-1 frames are allowed pending, so the frame
    // g_ring submissions ago is guaranteed presented by the time its slot is
    // reused. That's a guarantee, not a hope.
    ip.displayQueueMaxPendingCount = d2gxm_async() ? (unsigned)(g_ring - 1) : 1u;
    ip.displayQueueCallback = display_cb;
    ip.displayQueueCallbackDataSize = sizeof(DispData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int rc = sceGxmInitialize(&ip);
    { char m[176];
      std::snprintf(m, sizeof m, "gxm: sceGxmInitialize rc=0x%08x | file: max en attente=%u (carrousel=%d, doit valoir carrousel-1)"
                    " taille donnees=%u",
                    (unsigned)rc, ip.displayQueueMaxPendingCount, g_ring, ip.displayQueueCallbackDataSize);
      d2vita_progress(m); }
    // 0x805B0004 = ALREADY initialized. Not an allocation error, but not
    // harmless either: our params (including the display callback) are then
    // IGNORED, and the display queue would call someone else's callback — or
    // none. In async mode that's fatal and silent: log it and fall back to
    // synchronous.
    if ((unsigned)rc == 0x805B0004u && d2gxm_async() && g_asyncMode == 1) {
        d2vita_progress("gxm: sceGxm etait DEJA initialise — notre rappel d'affichage n'est pas pose."
                        " Bascule en mode DIFFERE (D2_GXMASYNC=2), qui n'en depend pas.");
        g_asyncMode = 2;
    }
    if (rc < 0 && (unsigned)rc != 0x805B0004u) {
        d2vita_progress("gxm: sceGxmInitialize a echoue — ARRET");
        return false; }

    // Context. The fragment uniform buffer is sized for ~350 batches per
    // frame, each reserving its own block: the default (64 KiB) is just
    // enough, so this takes 512 KiB to avoid revisiting it.
    // DO NOT MULTIPLY THESE BUFFERS BY THE CAROUSEL DEPTH. They only ever
    // hold the PER-SCENE uniforms, not one allocation per buffered frame:
    //   vertex:   4 floats (uScale) reserved once per scene = 16 bytes,
    //             against 2 MiB available;
    //   fragment: ~350 batches x 12 floats per frame, ~22 to 90 KiB, against
    //             512 KiB.
    // There's already an order of magnitude of headroom for several frames in
    // flight without multiplying anything. Scaling these by g_ring instead
    // eats several extra MiB of user RAM per buffer set and has driven this
    // console to hang right after init with no fault and nothing logged.
    const uint32_t FRING = 512u << 10;
    if (!gpu_alloc_gpu(g_vdm, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE,
                       SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_vdm")) { d2vita_progress("gxm: vdm KO — ARRET"); return false; }
    const uint32_t VRING = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    if (!gpu_alloc_gpu(g_vring, VRING,
                       SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_vring")) { d2vita_progress("gxm: vring KO — ARRET"); return false; }
    if (!gpu_alloc_gpu(g_fring, FRING,
                       SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_fring")) { d2vita_progress("gxm: fring KO — ARRET"); return false; }
    if (!usse_alloc(g_fusseRing, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, true,
                    &g_fusseRingOff, "d2gxm_fusse_ring")) {
        d2vita_progress("gxm: fragment USSE ring KO"); return false; }

    SceGxmContextParams cp;
    std::memset(&cp, 0, sizeof cp);
    g_hostMem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    cp.hostMem = g_hostMem;
    cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = g_vdm.p;                cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = g_vring.p;           cp.vertexRingBufferMemSize = VRING;
    cp.fragmentRingBufferMem = g_fring.p;         cp.fragmentRingBufferMemSize = FRING;
    cp.fragmentUsseRingBufferMem = g_fusseRing.p; cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = g_fusseRingOff;
    rc = sceGxmCreateContext(&cp, &g_ctx);
    if (rc < 0) { char m[80]; std::snprintf(m, sizeof m, "gxm: CreateContext rc=0x%08x", (unsigned)rc);
        d2vita_progress(m); return false; }
    mem_step("contexte");

    // ---- display buffers, THEN the render target --------------------------
    // Order matters: if the display rejects the requested size (D2_GXMRES),
    // this falls back to 960x544, and the render target must then be created
    // at the RIGHT size. Creating it first would leave a target that no
    // longer matches the surface.
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (int i = 0; i < g_ring; ++i) {
            // CDRAM first, main RAM as fallback. The console's "extended
            // memory" mode can make CDRAM unavailable — already the GDI
            // path's fallback (vita_present.cpp, alloc_fb). Without it, init
            // would fail on real hardware and Glide mode would show nothing
            // at all, where GDI already knows how to fall back.
            const uint32_t bytes = (uint32_t)SCR_W * (uint32_t)SCR_H * 4u;
            if (!gpu_alloc(g_fb[i], SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, bytes,
                           SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "d2gxm_fb") &&
                !gpu_alloc(g_fb[i], SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bytes,
                           SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "d2gxm_fb_ram")) {
                char m[112]; std::snprintf(m, sizeof m, "gxm: framebuffer %d/%d KO (ni CDRAM ni RAM) — ARRET", i, g_ring);
                d2vita_progress(m); return false; }
            uint32_t* px = (uint32_t*)g_fb[i].p;
            for (uint32_t k = 0; k < bytes / 4u; ++k) px[k] = 0xFF000000u;
            rc = sceGxmColorSurfaceInit(&g_color[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                        SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                        SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCR_W, SCR_H, SCR_W, g_fb[i].p);
            if (rc < 0) { d2vita_progress("gxm: ColorSurfaceInit KO"); return false; }
            if (!g_sync[i]) { const int src = sceGxmSyncObjectCreate(&g_sync[i]);
                if (src < 0 || !g_sync[i]) {
                    char m[112]; std::snprintf(m, sizeof m, "gxm: SyncObjectCreate[%d] rc=0x%08x — ARRET", i, (unsigned)src);
                    d2vita_progress(m); return false; } }
        }
        // Does the hardware actually accept THIS display size? This is ASKED
        // rather than assumed: a rejected size would leave a silent black
        // screen, and the documented list of scalable resolutions isn't a
        // guarantee.
        SceDisplayFrameBuf probe;
        std::memset(&probe, 0, sizeof probe);
        probe.size = sizeof probe; probe.base = g_fb[0].p; probe.pitch = (unsigned)SCR_W;
        probe.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        probe.width = (unsigned)SCR_W; probe.height = (unsigned)SCR_H;
        const int prc = sceDisplaySetFrameBuf(&probe, SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (prc >= 0) { mem_step("tampons d'affichage"); break; }
        char m[152];
        std::snprintf(m, sizeof m, "gxm: sceDisplay REFUSE %dx%d (rc=0x%08x)%s",
                      SCR_W, SCR_H, (unsigned)prc,
                      SCR_W == 960 ? " — et 960x544 aussi : init KO" : " — repli 960x544");
        d2vita_progress(m);
        if (SCR_W == 960) return false;
        for (int i = 0; i < g_ring; ++i) gpu_free(g_fb[i]);
        SCR_W = 960; SCR_H = 544; g_overlayOk = true;
    }

    SceGxmRenderTargetParams rtp;
    std::memset(&rtp, 0, sizeof rtp);
    rtp.width = SCR_W; rtp.height = SCR_H;
    // scenesPerFrame = the number of scenes rendered INTO THIS TARGET per
    // frame, not the number of frames in flight. Exactly one is rendered in
    // both modes. Setting it to g_ring made the driver allocate its internal
    // resources multiple times over, for nothing.
    rtp.scenesPerFrame = 1;
    rtp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rtp.driverMemBlock = -1;
    rc = sceGxmCreateRenderTarget(&rtp, &g_rt);
    if (rc < 0) { char m[80]; std::snprintf(m, sizeof m, "gxm: CreateRenderTarget rc=0x%08x", (unsigned)rc);
        d2vita_progress(m); return false; }
    mem_step("cible de rendu");

    // ---- depth --------------------------------------------------------------
    // D2 draws painter's-style: `grDepthBufferMode` is never called. The
    // depth buffer therefore serves NO purpose: NO surface is created
    // (sceGxmDepthStencilSurfaceInitDisabled). A tiled depth surface made no
    // measurable difference to GPU wait time, so that path was removed.
    sceGxmDepthStencilSurfaceInitDisabled(&g_ds);

    // Shader patcher.
    if (!gpu_alloc_gpu(g_patchBuf, 64 << 10,
                       SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE, "d2gxm_patch")) {
        d2vita_progress("gxm: patcher buffer KO — ARRET"); return false; }
    if (!usse_alloc(g_vusse, 64 << 10, false, &g_vusseOff, "d2gxm_vusse") ||
        !usse_alloc(g_fusse, 64 << 10, true,  &g_fusseOff, "d2gxm_fusse")) {
        d2vita_progress("gxm: USSE patcher KO"); return false; }
    SceGxmShaderPatcherParams pp;
    std::memset(&pp, 0, sizeof pp);
    pp.hostAllocCallback = patcher_host_alloc;
    pp.hostFreeCallback = patcher_host_free;
    pp.bufferMem = g_patchBuf.p; pp.bufferMemSize = g_patchBuf.size;
    pp.vertexUsseMem = g_vusse.p; pp.vertexUsseMemSize = 64 << 10; pp.vertexUsseOffset = g_vusseOff;
    pp.fragmentUsseMem = g_fusse.p; pp.fragmentUsseMemSize = 64 << 10; pp.fragmentUsseOffset = g_fusseOff;
    rc = sceGxmShaderPatcherCreate(&pp, &g_patcher);
    if (rc < 0) { char m[80]; std::snprintf(m, sizeof m, "gxm: ShaderPatcherCreate rc=0x%08x", (unsigned)rc);
        d2vita_progress(m); return false; }
    mem_step("patcher de shaders");

    // D2_GXMSHADERGEN=1: BUILDS every shader variant in one pass, into
    // ux0:data/d2vita/shaders. Only meaningful in the D2VPK_SHACC=1 build
    // flavor (the one linking the console's Cg compiler); elsewhere
    // get_shader just re-reads what already exists and the knob costs
    // nothing. Without it, producing each .gxp would take a separate run per
    // variant — more chances to ship a stale file.
    if (knob_int("D2_GXMSHADERGEN", 0)) {
        get_shader("d2_ring_f_flat",  kFragmentFlatCg,  1, 2);
        get_shader("d2_ring_f_pal",   kFragmentPalCg,   1, 2);
        get_shader("d2_ring_f_clear", kFragmentClearCg, 1, 2);
        d2vita_progress("gxm: D2_GXMSHADERGEN — variantes ecrites dans ux0:data/d2vita/shaders");
    }

    const SceGxmProgram* vprog = get_shader("d2_ring_v", kVertexCg, 0, 0);
    // MAIN FRAGMENT PROGRAM CHOICE. Two programs, default first: with no
    // knob it's d2_ring_f; D2_GXMPAL=1 selects d2_ring_f_pal (hardware
    // palette, single texture read).
    // If the requested variant's .gxp is missing (console without
    // libshacccg and an older VPK), this FALLS BACK to the default and
    // DISARMS the knob — otherwise the mode line would announce a variant
    // the hardware isn't actually running.
    const char* fname = "d2_ring_f";
    const char* fsrc  = kFragmentCg;
    if (g_hwPal) { fname = "d2_ring_f_pal"; fsrc = kFragmentPalCg; }
    const SceGxmProgram* fprog = get_shader(fname, fsrc, 1, 1);
    if (!fprog && g_hwPal) {
        char m[176];
        std::snprintf(m, sizeof m, "gxm: %s.gxp ABSENT — palette materielle DESARMEE, rendu normal", fname);
        d2vita_progress(m);
        g_hwPal = 0;
        fname = "d2_ring_f"; fsrc = kFragmentCg;
        fprog = get_shader(fname, fsrc, 1, 1);
    }
    g_fragName = fname;
    if (!vprog || !fprog) { d2vita_progress("gxm: shaders indisponibles — repli sur le puits"); return false; }
    { char m[160]; std::snprintf(m, sizeof m, "gxm: shaders v=%s f=%s [%s]",
                                 g_shaderOrigin[0], g_shaderOrigin[1], g_fragName);
      d2vita_progress(m); }
    { const int r1 = sceGxmShaderPatcherRegisterProgram(g_patcher, vprog, &g_vpId);
      const int r2 = sceGxmShaderPatcherRegisterProgram(g_patcher, fprog, &g_fpId);
      if (r1 < 0 || r2 < 0 || !g_vpId || !g_fpId) {
          char m[112]; std::snprintf(m, sizeof m, "gxm: RegisterProgram v=0x%08x f=0x%08x — ARRET",
                                     (unsigned)r1, (unsigned)r2);
          d2vita_progress(m); return false; } }

    const SceGxmProgramParameter* aPos = sceGxmProgramFindParameterByName(vprog, "aPos");
    const SceGxmProgramParameter* aTex = sceGxmProgramFindParameterByName(vprog, "aTex");
    const SceGxmProgramParameter* aCol = sceGxmProgramFindParameterByName(vprog, "aCol");
    const SceGxmProgramParameter* aPal = sceGxmProgramFindParameterByName(vprog, "aPal");
    if (!aPos || !aTex || !aCol || !aPal) { d2vita_progress("gxm: attributs de sommet introuvables"); return false; }
    SceGxmVertexAttribute attr[4];
    SceGxmVertexStream stream[1];
    attr[0].streamIndex = 0; attr[0].offset = 0;  attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[0].componentCount = 2; attr[0].regIndex = sceGxmProgramParameterGetResourceIndex(aPos);
    attr[1].streamIndex = 0; attr[1].offset = 8;  attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[1].componentCount = 2; attr[1].regIndex = sceGxmProgramParameterGetResourceIndex(aTex);
    attr[2].streamIndex = 0; attr[2].offset = 16; attr[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    attr[2].componentCount = 4; attr[2].regIndex = sceGxmProgramParameterGetResourceIndex(aCol);
    attr[3].streamIndex = 0; attr[3].offset = 20; attr[3].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attr[3].componentCount = 1; attr[3].regIndex = sceGxmProgramParameterGetResourceIndex(aPal);
    stream[0].stride = sizeof(d2gr::Vtx);
    stream[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    rc = sceGxmShaderPatcherCreateVertexProgram(g_patcher, g_vpId, attr, 4, stream, 1, &g_vp);
    if (rc < 0) { char m[80]; std::snprintf(m, sizeof m, "gxm: CreateVertexProgram rc=0x%08x", (unsigned)rc);
        d2vita_progress(m); return false; }

    // The FOUR blend functions Glide actually uses, nothing else. The color
    // mask excludes alpha: the display buffer keeps its 0xFF.
    static const SceGxmBlendInfo kBlend[4] = {
        // opaque: ONE / ZERO
        { SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B,
          SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO },
        // alpha blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA
        { SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B,
          SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD,
          SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO },
        // additive: ONE / ONE
        { SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B,
          SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO },
        // multiplicative (D2's shadows): ZERO / SRC_COLOR
        { SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B,
          SCE_GXM_BLEND_FUNC_ADD, SCE_GXM_BLEND_FUNC_ADD,
          SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_SRC_COLOR,
          SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO },
    };
    for (int i = 0; i < 4; ++i) {
        rc = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fpId,
                SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                &kBlend[i], vprog, &g_fp[i]);
        if (rc < 0) { char m[96]; std::snprintf(m, sizeof m, "gxm: CreateFragmentProgram[%d] rc=0x%08x", i, (unsigned)rc);
            d2vita_progress(m); return false; }
    }
    g_pScale = sceGxmProgramFindParameterByName(vprog, "uScale");
    g_pConst = sceGxmProgramFindParameterByName(fprog, "uConst");
    g_pMode  = sceGxmProgramFindParameterByName(fprog, "uMode");
    g_pKey   = sceGxmProgramFindParameterByName(fprog, "uKey");
    if (!g_pScale || !g_pConst || !g_pMode || !g_pKey) {
        d2vita_progress("gxm: uniformes introuvables"); return false; }

    // FLAT CLEAR (D2_GXMCLEAR=plat). A single program, opaque blend: the
    // clear is opaque by construction. A failure here FALLS BACK to the
    // normal quad and logs it — silently measuring a disarmed variant would
    // be worse than not measuring it at all.
    if (g_clearMode == 1) {
        const SceGxmProgram* fclear = get_shader("d2_ring_f_clear", kFragmentClearCg, 1, 2);
        bool ok = false;
        if (fclear) {
            sceGxmShaderPatcherRegisterProgram(g_patcher, fclear, &g_fpClearId);
            ok = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fpClearId,
                    SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                    &kBlend[0], vprog, &g_fpClear) >= 0;
            // Uniform OFFSETS belong to their program. Reusing g_pConst here
            // would write to the wrong place and clear to an arbitrary color
            // — visible, but unexplained.
            g_pConstC = sceGxmProgramFindParameterByName(fclear, "uConst");
            ok = ok && g_pConstC != nullptr;
        }
        char m[144];
        std::snprintf(m, sizeof m, "gxm: effacement PLAT %s (%s)", ok ? "ARME" : "KO — repli quad complet",
                      g_shaderOrigin[2][0] ? g_shaderOrigin[2] : "absent");
        d2vita_progress(m);
        if (!ok) { g_fpClear = nullptr; g_clearMode = 0; std::strcpy(g_clearName, "quad"); }
    }

    // "BYPASSED PALETTE" MEASUREMENT VARIANT (D2_GXMPROBE=3). Only built on
    // request: otherwise it would eat patcher memory for nothing. Its
    // ABSENCE isn't an error — normal rendering continues, and the knob says so.
    if (g_probe == 3) {
        const SceGxmProgram* fflat = get_shader("d2_ring_f_flat", kFragmentFlatCg, 1, 2);
        if (!fflat) {
            d2vita_progress("gxm: D2_GXMPROBE=3 demande mais d2_ring_f_flat.gxp absent — rendu NORMAL");
        } else {
            sceGxmShaderPatcherRegisterProgram(g_patcher, fflat, &g_fpFlatId);
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i)
                ok = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fpFlatId,
                        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                        &kBlend[i], vprog, &g_fpFlat[i]) >= 0;
            g_pConstF = sceGxmProgramFindParameterByName(fflat, "uConst");
            g_pModeF  = sceGxmProgramFindParameterByName(fflat, "uMode");
            g_pKeyF   = sceGxmProgramFindParameterByName(fflat, "uKey");
            g_flatOk = ok && g_pConstF && g_pModeF;
            char m[128];
            std::snprintf(m, sizeof m, "gxm: PROBE=3 shader plat %s (%s)",
                          g_flatOk ? "ARME" : "KO", g_shaderOrigin[2]);
            d2vita_progress(m);
        }
    }

    // ---- INCRUSTATION D'INTERFACE SUR LE GPU (menu radial) -----------------
    // Son absence n'est PAS une erreur : sans .gxp (console sans libshacccg et
    // VPK plus ancien), g_uiOk reste faux et le blitter CPU reprend la main,
    // plus lent mais correct. Ce qui serait une faute, c'est de le laisser à
    // moitié armé — d'où le ET final sur les trois conditions.
    {
        const SceGxmProgram* fui = get_shader("d2_ring_f_rgba", kFragmentRgbaCg, 1, 2);
        bool ok = false;
        if (fui) {
            sceGxmShaderPatcherRegisterProgram(g_patcher, fui, &g_fpUiId);
            ok = sceGxmShaderPatcherCreateFragmentProgram(g_patcher, g_fpUiId,
                    SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                    &kBlend[1], vprog, &g_fpUi) >= 0;   // [1] = SRC_ALPHA / 1-SRC_ALPHA
        }
        if (ok) {
            const uint32_t bytes = (uint32_t)radial_menu::RM_ATLAS_DIM
                                 * radial_menu::RM_ATLAS_DIM * 4;
            if (gpu_alloc_gpu(g_uiTexBlk, bytes, SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_ui")) {
                radial_menu::fill_atlas((unsigned char*)g_uiTexBlk.p);
                ok = sceGxmTextureInitLinear(&g_uiTex, g_uiTexBlk.p,
                        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8,
                        (unsigned)radial_menu::RM_ATLAS_DIM,
                        (unsigned)radial_menu::RM_ATLAS_DIM, 0) >= 0;
                // POINT, comme le blitter CPU qu'on remplace : filtrer
                // changerait le rendu du menu en même temps qu'on change sa
                // façon d'être dessiné, et on ne saurait plus à quoi attribuer
                // une différence à l'écran.
                sceGxmTextureSetMinFilter(&g_uiTex, SCE_GXM_TEXTURE_FILTER_POINT);
                sceGxmTextureSetMagFilter(&g_uiTex, SCE_GXM_TEXTURE_FILTER_POINT);
                sceGxmTextureSetUAddrMode(&g_uiTex, SCE_GXM_TEXTURE_ADDR_CLAMP);
                sceGxmTextureSetVAddrMode(&g_uiTex, SCE_GXM_TEXTURE_ADDR_CLAMP);
            } else ok = false;
        }
        g_uiOk = ok;
        char m[144];
        std::snprintf(m, sizeof m, "gxm: incrustation menu sur GPU %s (%s) — sinon blitter CPU",
                      g_uiOk ? "ARMEE" : "KO", g_shaderOrigin[2][0] ? g_shaderOrigin[2] : "absent");
        d2vita_progress(m);
        if (!g_uiOk && g_uiTexBlk.p) { gpu_free(g_uiTexBlk); g_uiTexBlk = GpuBlock{}; }
    }

    // Vertex/index buffers: ONE SET PER FRAME IN FLIGHT. This is the first
    // thing async submission forces to be duplicated: the batch builder
    // writes frame N+1 here while the GPU is still reading frame N.
    for (int i = 0; i < g_ring; ++i) {
        if (!gpu_alloc_gpu(g_vtxBuf[i], MAXV * sizeof(d2gr::Vtx),
                           SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_vtx")) { d2vita_progress("gxm: vtx KO — ARRET"); return false; }
        if (!gpu_alloc_gpu(g_idxBuf[i], MAXI * 2,
                           SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_idx")) { d2vita_progress("gxm: idx KO — ARRET"); return false; }
    }

    // Palette textures: 256 x D2GXM_PALETTES, ONE PER FRAME IN FLIGHT. Palette
    // uploads are rare in practice, so 16 slots (16 KiB) is an order of
    // magnitude more than needed. A palette rewritten while the GPU still
    // reads the previous one would produce a WRONG image: hence the CPU
    // shadow (g_palShadow) and one copy per buffer set.
    std::memset(g_palShadow, 0, sizeof g_palShadow);
    for (int i = 0; i < g_ring; ++i) {
        if (!gpu_alloc_gpu(g_palBlk[i], 256 * D2GXM_PALETTES * 4,
                           SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_pal")) { d2vita_progress("gxm: palette KO — ARRET"); return false; }
        std::memset(g_palBlk[i].p, 0, 256 * D2GXM_PALETTES * 4);
        g_palBlkGen[i] = 0;
        sceGxmTextureInitLinear(&g_palTex[i], g_palBlk[i].p, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8, 256, D2GXM_PALETTES, 0);
        sceGxmTextureSetMinFilter(&g_palTex[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&g_palTex[i], SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&g_palTex[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&g_palTex[i], SCE_GXM_TEXTURE_ADDR_CLAMP);
    }
    // Fragment-done notifications (mode 2). The region belongs to sceGxm; one
    // word per slot is taken and initialized to A VALUE THAT ISN'T THE
    // EXPECTED ONE, or the very first wait would return immediately before
    // the GPU had done anything.
    g_notifBase = sceGxmGetNotificationRegion();
    if (!g_notifBase && g_asyncMode == 2) {
        d2vita_progress("gxm: pas de region de notification — D2_GXMASYNC=2 impossible, repli SYNCHRONE");
        g_async = false; g_asyncMode = 0; g_ring = 2;
    }
    if (g_notifBase) for (int i = 0; i < MAXRING; ++i) {
        g_notif[i].address = g_notifBase + i;
        *g_notif[i].address = 0xFFFFFFFFu;
        g_notif[i].value = 0;
    }
    mem_step("sommets/indices/palettes");

    // Dummy page: an unbound sampler is undefined behavior, and "flat" batches
    // (grColorCombine LOCAL/CONSTANT) have no texture.
    if (!gpu_alloc_gpu(g_dummy, 64,
                       SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_dummy")) { d2vita_progress("gxm: dummy KO — ARRET"); return false; }
    std::memset(g_dummy.p, 0, 64);
    // With the hardware palette, the dummy page must have the SAME format as
    // real pages: a P8 texture with no palette bound is undefined behavior,
    // and the texture unit shouldn't have to change format between the clear
    // quad and the first sprite.
    sceGxmTextureInitLinear(&g_dummyTex, g_dummy.p,
                            g_hwPal ? SCE_GXM_TEXTURE_FORMAT_P8_ABGR
                                    : SCE_GXM_TEXTURE_FORMAT_U8_RRRR, 8, 8, 0);
    if (g_hwPal) sceGxmTextureSetPalette(&g_dummyTex, pal_ptr(0, 0));
    sceGxmTextureSetMinFilter(&g_dummyTex, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&g_dummyTex, SCE_GXM_TEXTURE_FILTER_POINT);

    // The first slot taken will be (g_back+1)%g_ring = 0, and the "previous
    // slot" submitted to the display queue will be g_ring-1: never the same
    // sync object on both sides of one sceGxmDisplayQueueAddEntry call.
    g_back = g_ring - 1;
    g_front = g_ring - 1;
    g_failed = false; g_ready = true;
    { char m[176];
      SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
      sceKernelGetFreeMemorySize(&fi);
      std::snprintf(m, sizeof m, "gxm: PRET — fenetre %dx%d -> %dx%d, libre user=%d Ko cdram=%d Ko",
                    g_gameW, g_gameH, SCR_W, SCR_H, fi.size_user >> 10, fi.size_cdram >> 10);
      d2vita_progress(m); }
    { SceKernelFreeMemorySizeInfo fi; fi.size = sizeof fi;
      sceKernelGetFreeMemorySize(&fi);
      char m[208];
      std::snprintf(m, sizeof m, "gxm: memoire — RAM user %d -> %d Ko (cout %d Ko), cdram libre %d Ko, placement GPU=%s"
                    " | USSE en RAM user par obligation (144 Ko)",
                    g_userFreeAtEntry, fi.size_user >> 10, g_userFreeAtEntry - (fi.size_user >> 10),
                    fi.size_cdram >> 10,
                    wx86::vita::gpu_prefers_cdram() ? "CDRAM" : "RAM user (D2_GXMCDRAM=0)");
      d2vita_progress(m); }
    { char m[320];
      std::snprintf(m, sizeof m, "gxm: mode=%s carrousel=%d vsync=%d rendement-us=%d effacement=%s melange=%s sonde=%d incrust=%s"
                                 " | pal=%s f=%s filtre=%s",
                    !d2gxm_async() ? "synchrone" : (g_asyncMode == 2 ? "ASYNCHRONE-DIFFERE" : "ASYNCHRONE-FILE"),
                    g_ring, g_vsyncMode, g_yieldUs,
                    g_clearName, g_forceBlend < 0 ? "mesure" : "force",
                    g_probe, g_overlayOk ? "oui" : "non",
                    g_hwPal ? "hw" : "shader", g_fragName,
                    g_filtre == 1 ? "glide(LINEAR sol)" : "point");
      d2vita_progress(m); }
    return true;
}

int d2gxm_page_create(int page, int dim) {
    if (!g_ready || page < 0 || page >= MAXPAGES) return 0;
    if (g_page[page].p) return 1;
    if (!gpu_alloc(g_page[page], SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, (uint32_t)dim * dim,
                   SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_page") &&
        !gpu_alloc(g_page[page], SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (uint32_t)dim * dim,
                   SCE_GXM_MEMORY_ATTRIB_READ, "d2gxm_page_ram")) {
        char m[96]; std::snprintf(m, sizeof m, "gxm: page %d (%d Ko) refusee — l'atlas evincera", page, dim*dim>>10);
        d2vita_progress(m); return 0; }
    std::memset(g_page[page].p, 0, (size_t)dim * dim);
    // PAGE FORMAT:
    //   default      -> U8_RRRR: indices, palette lookup done by the shader.
    //   D2_GXMPAL=1  -> P8_ABGR: the texture unit does the palette lookup, no
    //                   dependent read left in the shader. Palette entries
    //                   are U8U8U8U8_ABGR words, EXACTLY the format
    //                   d2gxm_palette already produces: no byte reordering
    //                   needed.
    // LINEAR layout only. A Morton/swizzled layout was evaluated and dropped:
    // no measurable throughput gain over linear, same GPU wait time, and
    // roughly double the upload cost.
    const SceGxmTextureFormat pf = g_hwPal ? SCE_GXM_TEXTURE_FORMAT_P8_ABGR
                                           : SCE_GXM_TEXTURE_FORMAT_U8_RRRR;
    const int trc = sceGxmTextureInitLinear(&g_pageTex[page], g_page[page].p, pf, (unsigned)dim, (unsigned)dim, 0);
    if (trc < 0) {
        // An uninitialized page would render garbage with no diagnostic at
        // all: it's REFUSED and logged instead; the atlas evicts as usual.
        char m[112];
        std::snprintf(m, sizeof m, "gxm: TextureInitLinear page %d rc=0x%08x — page REFUSEE", page, (unsigned)trc);
        d2vita_progress(m);
        gpu_free(g_page[page]); return 0;
    }
    if (g_hwPal) sceGxmTextureSetPalette(&g_pageTex[page], pal_ptr(0, 0));
    sceGxmTextureSetMinFilter(&g_pageTex[page], SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&g_pageTex[page], SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&g_pageTex[page], SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&g_pageTex[page], SCE_GXM_TEXTURE_ADDR_CLAMP);
    g_pageDim = dim;
    if (page + 1 > g_pageN) g_pageN = page + 1;
    return 1;
}

void d2gxm_page_upload(int page, int x, int y, int w, int h, const uint8_t* src, int srcPitch) {
    if (!g_ready || page < 0 || page >= MAXPAGES || !g_page[page].p || !src) return;
    uint8_t* base = (uint8_t*)g_page[page].p;
    uint8_t* dst = base + (size_t)y * g_pageDim + x;
    for (int r = 0; r < h; ++r) std::memcpy(dst + (size_t)r * g_pageDim, src + (size_t)r * srcPitch, (size_t)w);
}

void d2gxm_palette(int slot, const uint32_t* argb256) {
    if (!g_ready || slot < 0 || slot >= D2GXM_PALETTES || !argb256) return;
    { static int shown = 0;
      if (shown < 3) { ++shown; char m[176];
        std::snprintf(m, sizeof m, "gxm: palette -> emplacement %d | [0]=%08x [1]=%08x [2]=%08x [255]=%08x",
                      slot, (unsigned)argb256[0], (unsigned)argb256[1], (unsigned)argb256[2], (unsigned)argb256[255]);
        d2vita_progress(m); } }
    // Glide's palette arrives as 0xAARRGGBB; the texture expects, in memory,
    // R,G,B,A — i.e. the word 0xAABBGGRR. Alpha is FORCED to 255: for the P8
    // format Glide ignores the table's alpha, and it's the chromakey
    // (typically black) that decides transparency.
    // This never writes into the memory the GPU reads. The CPU shadow is the
    // source of truth; each buffer set copies from it when submitted. A
    // direct write here would, in async mode, change the palette of a frame
    // ALREADY submitted — an occasional sprite with the wrong colors.
    uint32_t* d = g_palShadow + (size_t)slot * 256;
    for (int i = 0; i < 256; ++i) {
        const uint32_t c = argb256[i];
        d[i] = 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
    }
    ++g_palGen;
}

// ---------------------------------------------------------------------------
// SUBMITTING A FRAME
//
// TWO PATHS, one per run:
//
//  * SYNCHRONOUS (default): sceGxmFinish, overlays, sceDisplaySetFrameBuf, all
//    on the game thread. The frame then costs `game + submit + GPU`, in
//    series.
//
//  * ASYNCHRONOUS (D2_GXMASYNC=1): sceGxmEndScene returns immediately, the
//    frame goes into sceGxm's DISPLAY QUEUE, and the game thread immediately
//    starts computing the next frame. The display thread waits on the GPU,
//    draws overlays, and presents the buffer. The game thread blocks ONLY if
//    the queue is full (displayQueueMaxPendingCount back-pressure) — and that
//    block is published as-is in `attente-gpu-us`. The frame then costs
//    `max(game, GPU)` instead of their sum.
//
// WHAT'S DUPLICATED TO MAKE THIS SAFE (missing just one of these produces an
// intermittently wrong frame, the worst kind of bug):
//   - the vertex buffer and the index buffer: one set per slot;
//   - the display buffer and its sync object: one per slot;
//   - the 16 palette slots: one set per slot, fed from a CPU shadow
//     (d2gxm_palette never writes into the memory the GPU reads);
//   - uniforms: sceGxm's own ring buffer is sized x g_ring;
//   - the ATLAS: the one thing that can't be duplicated (32 MiB). It's
//     protected by a BARRIER instead: d2gxm_busy_from() publishes the first
//     frame still in flight, and the atlas refuses to evict any cell used by
//     a frame >= that bound (glide_atlas.h, noEvictFrom_). When the barrier
//     rules out its last possible eviction, it calls d2gxm_drain(): a
//     measured wait instead of a wrong sprite.
// ---------------------------------------------------------------------------
// SCREENSHOT (D2_GXMSHOT). Writes color buffer `slot` as 24-bit BMP (BGR,
// bottom-to-top rows). Called in the submission prologue, on the slot about
// to be OVERWRITTEN: in sync mode it went through sceGxmFinish, in mode 2
// through retire_one, in mode 1 through sceGxmDisplayQueueAddEntry's
// back-pressure (max pending = carousel-1). So it's always a FINISHED frame,
// the one submitted `carousel` frames ago. Costs ~1.5 MiB of writes: reserved
// for fidelity checks, never used during a perf run.
static void shot_write(int slot, uint64_t frame) {
    if (!g_fb[slot].p) return;
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    char path[96];
    std::snprintf(path, sizeof path, "ux0:data/d2vita/shot_%llu.bmp", (unsigned long long)frame);
    FILE* f = std::fopen(path, "wb");
    if (!f) { char m[128]; std::snprintf(m, sizeof m, "gxm: capture %s IMPOSSIBLE (fopen)", path); d2vita_progress(m); return; }
    const uint32_t w = (uint32_t)SCR_W, h = (uint32_t)SCR_H, row = w * 3u, img = row * h;
    uint8_t hdr[54]; std::memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    auto put32 = [&](int off, uint32_t v) { hdr[off] = (uint8_t)v; hdr[off+1] = (uint8_t)(v >> 8); hdr[off+2] = (uint8_t)(v >> 16); hdr[off+3] = (uint8_t)(v >> 24); };
    put32(2, 54u + img); put32(10, 54u); put32(14, 40u); put32(18, w); put32(22, h);
    hdr[26] = 1; hdr[28] = 24; put32(34, img);
    std::fwrite(hdr, 1, sizeof hdr, f);
    static uint8_t line[4096 * 3];
    const uint32_t* px = (const uint32_t*)g_fb[slot].p;
    for (uint32_t y = h; y-- > 0; ) {
        const uint32_t* s = px + (size_t)y * w;
        for (uint32_t x = 0; x < w; ++x) {      // memoire A8B8G8R8 : octets R,G,B,A -> mot 0xAABBGGRR
            const uint32_t c = s[x];
            line[x*3+0] = (uint8_t)(c >> 16); line[x*3+1] = (uint8_t)(c >> 8); line[x*3+2] = (uint8_t)c; }
        std::fwrite(line, 1, row, f);
    }
    std::fclose(f);
    ++g_shotN;
    char m[160];
    std::snprintf(m, sizeof m, "gxm: capture %s (slot %d, image soumise %llu) en %u us",
                  path, slot, (unsigned long long)(frame > (uint64_t)g_ring ? frame - (uint64_t)g_ring : 0),
                  (unsigned)(sceKernelGetProcessTimeWide() - t0));
    d2vita_progress(m);
}
extern "C" void d2gxm_shot_request(void) { g_shotPending = 1; }

void d2gxm_submit(const d2gr::Vtx* v, uint32_t nv,
                  const uint16_t* idx, uint32_t ni,
                  const d2gr::Batch* b, uint32_t nb, uint32_t clearARGB,
                  uint64_t frame) {
    if (!g_ready) return;
    const bool async = d2gxm_async();
    const uint64_t tEntry = sceKernelGetProcessTimeWide();
    const uint64_t t0 = tEntry;
    const int prev = g_back;
    const int slot = (g_back + 1) % g_ring;

    // MODE 2: is the slot about to be overwritten still being read by the
    // GPU? If so, retire it first — the ONLY possible wait in mode 2, and
    // it's measured. In steady state it's zero: the previous frame had the
    // whole game frame's compute time to finish.
    if (g_asyncMode == 2) {
        for (int guard = 0; guard <= MAXRING; ++guard) {
            bool busy = false;
            for (int i = 0; i < g_pendN; ++i) if (g_pend[i].slot == slot) busy = true;
            if (!busy) break;
            retire_one();
        }
    }

    if (g_shotFrom > 0 && frame >= (uint64_t)g_shotFrom &&
        (frame - (uint64_t)g_shotFrom) % (uint64_t)g_shotEvery == 0 && g_shotN < 8)
        shot_write(slot, frame);
    if (g_shotPending) { g_shotPending = 0; shot_write(slot, frame); }   // L + Start

    // Copies from CPU into GPU-visible memory. ~4000 vertices = 96 KiB.
    if (nv > MAXV) nv = MAXV;
    if (ni > MAXI) ni = MAXI;
    const uint64_t tCopy0 = sceKernelGetProcessTimeWide();
    std::memcpy(g_vtxBuf[slot].p, v, (size_t)nv * sizeof(d2gr::Vtx));
    std::memcpy(g_idxBuf[slot].p, idx, (size_t)ni * 2);

    // Palettes: this slot's copy is only refreshed if a palette changed since
    // it was last used. Uploads are rare in practice, so this 16 KiB copy
    // almost never happens.
    if (g_palBlkGen[slot] != g_palGen) {
        std::memcpy(g_palBlk[slot].p, g_palShadow, sizeof g_palShadow);
        g_palBlkGen[slot] = g_palGen;
    }

    // The fragment sync object is ONLY used by the display queue. Mode 2
    // doesn't use it, so NULL is passed instead: binding a scene to an object
    // nobody then releases is the classic recipe for a hang the second time a
    // slot is reused.
    const uint64_t tBeg0 = sceKernelGetProcessTimeWide();
    g_copyUs += (tBeg0 - tCopy0);
    // PRIME SUSPECT for missing time: sceGxmBeginScene can WAIT — the display
    // buffer about to be redrawn must have been released by the display.
    // With a carousel of 2, this wait is the system's "queue depth".
    phase("sceGxmBeginScene");
    int rc = sceGxmBeginScene(g_ctx, 0, g_rt, nullptr, nullptr,
                              g_asyncMode == 2 ? nullptr : g_sync[slot],
                              &g_color[slot], &g_ds);
    const uint64_t tBeg1 = sceKernelGetProcessTimeWide();
    g_beginUs += (tBeg1 - tBeg0);
    if (rc < 0) { static bool said = false; if (!said) { said = true;
            char m[80]; std::snprintf(m, sizeof m, "gxm: BeginScene rc=0x%08x", (unsigned)rc); d2vita_progress(m); }
        return; }
    set_default_state();
    sceGxmSetVertexProgram(g_ctx, g_vp);
    g_texScratchN = 0;
    g_hwpalNewThisFrame = 0;

    // Projection uniform: game window (800x600) -> screen, the same stretch
    // as the GDI path's StretchBlt.
    float scale[4] = { 2.0f / (float)g_gameW, -2.0f / (float)g_gameH, -1.0f, 1.0f };
    void* vu = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(g_ctx, &vu);
    if (vu) sceGxmSetUniformDataF(vu, g_pScale, 0, 4, scale);

    sceGxmSetVertexStream(g_ctx, 0, g_vtxBuf[slot].p);

    // Which set of fragment programs to use. Normally the real shader;
    // D2_GXMPROBE=3 substitutes the variant WITHOUT the dependent texture
    // read and WITHOUT discard — wrong image, honest numbers.
    const bool flat = (g_probe == 3) && g_flatOk;
    SceGxmFragmentProgram* const* FP = flat ? g_fpFlat : g_fp;
    const SceGxmProgramParameter* const PC = flat ? g_pConstF : g_pConst;
    const SceGxmProgramParameter* const PM = flat ? g_pModeF  : g_pMode;
    const SceGxmProgramParameter* const PK = flat ? g_pKeyF   : g_pKey;
    // Binds the page texture (unit 0), accounting for the batch's palette.
    // In SOFTWARE palette mode (default) this is the original path, unchanged.
    auto bind_page = [&](int page, SceGxmTextureFilter f, int palIdx) {
        SceGxmTexture* base = (page >= 0) ? &g_pageTex[page] : &g_dummyTex;
        if (!g_hwPal) {
            if (page >= 0) { sceGxmTextureSetMinFilter(base, f); sceGxmTextureSetMagFilter(base, f); }
            sceGxmSetFragmentTexture(g_ctx, 0, base);
            return;
        }
        if (g_texScratchN >= TEXSCRATCH) {
            // Ring overflow: the batch goes out with the page descriptor's
            // placeholder palette, i.e. WRONG COLORS. This is a known weak
            // spot; it must not fail silently, or someone would go chasing a
            // palette bug.
            ++g_texScratchOver;
            if (!g_texScratchOverSaid) { g_texScratchOverSaid = true; char m[160];
                std::snprintf(m, sizeof m, "gxm[hwpal]: DEBORDEMENT de l'anneau de descripteurs (image %llu, %d lots) — "
                              "lot rendu avec la palette placeholder", (unsigned long long)frame, TEXSCRATCH);
                d2vita_progress(m); }
            sceGxmSetFragmentTexture(g_ctx, 0, base);
            return;
        }
        SceGxmTexture* t = &g_texScratch[slot][g_texScratchN++];
        *t = *base;
        const int pi = (palIdx >= 0 && palIdx < D2GXM_PALETTES) ? palIdx : 0;
        const void* pp = pal_ptr(slot, pi);
        // Return codes ARE checked here. Sony validates palette alignment and
        // texture type; a silent failure would let a half-built descriptor
        // reach the GPU with no diagnostic — precisely the kind of failure
        // that gives no clue in a log that just stops.
        const int rcP = sceGxmTextureSetPalette(t, pp);
        int rcF = 0, rcG = 0;
        if (page >= 0) { rcF = sceGxmTextureSetMinFilter(t, f); rcG = sceGxmTextureSetMagFilter(t, f); }
        if ((rcP < 0 || rcF < 0 || rcG < 0) && !g_hwpalRcSaid) { g_hwpalRcSaid = true; char m[176];
            std::snprintf(m, sizeof m, "gxm[hwpal]: REFUS descripteur page %d palette %d : SetPalette=0x%08x SetMinFilter=0x%08x SetMagFilter=0x%08x",
                          page, pi, (unsigned)rcP, (unsigned)rcF, (unsigned)rcG);
            d2vita_progress(m); }
        // Milestone: first time this (page, palette, filter) combo is bound.
        const bool lin = (page >= 0) && (f == SCE_GXM_TEXTURE_FILTER_LINEAR);
        const uint8_t bit = lin ? 2u : 1u;
        uint8_t& seen = g_hwpalSeen[page + 1][pi];
        if (!(seen & bit)) {
            seen |= bit; ++g_hwpalNewThisFrame;
            if (g_hwpalLines < HWPAL_MAXLINES) { ++g_hwpalLines;
                const uint32_t* w = (const uint32_t*)t;
                char m[240];
                std::snprintf(m, sizeof m, "gxm[hwpal]: image %llu lot %d — 1re liaison page %d palette %d filtre=%s"
                              " | pal=%p (jeu %d) data=%p | mots=%08x %08x %08x %08x",
                              (unsigned long long)frame, g_texScratchN - 1, page, pi, lin ? "LINEAR" : "POINT",
                              pp, slot, page >= 0 ? g_page[page].p : g_dummy.p, w[0], w[1], w[2], w[3]);
                d2vita_progress(m); }
        }
        sceGxmSetFragmentTexture(g_ctx, 0, t);
    };

    // 1. CLEAR. sceGxm's tiled rendering doesn't reload previous tile content
    //    if the first thing drawn covers the screen: an opaque fullscreen
    //    quad IS the clear, and costs less than a tile load. grBufferClear(0)
    //    is always black in practice.
    //    This quad still writes 522,240 fragments PER FRAME, each paying for
    //    the shader's dependent texture read AND its `discard` — on the
    //    order of 3 ms just to write black.
    //      D2_GXMCLEAR=aucun (= NOCLEAR): no quad at all. The frame is only
    //        correct if the game covers the whole screen.
    //      D2_GXMCLEAR=plat: THE SAME QUAD, with a fragment program that
    //        samples nothing and discards nothing. The frame stays CORRECT,
    //        and the gap versus the default is what the full shader costs on
    //        top of the clear. This is the one to use; `aucun` was only ever
    //        a measurement tool.
    if (g_clearMode != 2) {
        const bool flatClear = (g_clearMode == 1) && g_fpClear && g_pConstC;
        d2gr::Vtx* cv = (d2gr::Vtx*)g_vtxBuf[slot].p + nv;    // after the game's own vertices
        uint16_t* ci = (uint16_t*)g_idxBuf[slot].p + ni;
        if (nv + 4 <= MAXV && ni + 6 <= MAXI) {
            const float W = (float)g_gameW, H = (float)g_gameH;
            const float xs[4] = { 0, W, W, 0 }, ys[4] = { 0, 0, H, H };
            for (int k = 0; k < 4; ++k) { cv[k].x = xs[k]; cv[k].y = ys[k]; cv[k].u = 0; cv[k].v = 0;
                                          cv[k].argb = 0xFFFFFFFFu; cv[k].pal = 0.0f; }
            ci[0] = (uint16_t)(nv+0); ci[1] = (uint16_t)(nv+1); ci[2] = (uint16_t)(nv+2);
            ci[3] = (uint16_t)(nv+0); ci[4] = (uint16_t)(nv+2); ci[5] = (uint16_t)(nv+3);
            sceGxmSetFragmentProgram(g_ctx, flatClear ? g_fpClear : FP[0]);
            void* fu = nullptr;
            sceGxmReserveFragmentDefaultUniformBuffer(g_ctx, &fu);
            if (fu && flatClear) {
                // The flat program has ONLY ONE uniform and no texture: there's
                // nothing else to set, which is exactly the point of this variant.
                float cst[4] = { (float)((clearARGB >> 24) & 0xFF) / 255.0f,   // RGBA (cformat=2)
                                 (float)((clearARGB >> 16) & 0xFF) / 255.0f,
                                 (float)((clearARGB >>  8) & 0xFF) / 255.0f, 1.0f };
                if (g_probe == 1) { cst[0] = 1.0f; cst[1] = 0.0f; cst[2] = 0.0f; }
                sceGxmSetUniformDataF(fu, g_pConstC, 0, 4, cst);
            } else if (fu) {
                // D2_GXMPROBE=1: the clear turns BRIGHT RED. This isolates
                // "the pipeline draws nothing" from "the data is wrong": this
                // quad uses no atlas, no palette, no chromakey — only the
                // vertex shader, the opaque fragment program, the surface and
                // the display. If the screen doesn't turn red, there's no
                // point looking at the sprites.
                float cst[4] = { (float)((clearARGB >> 24) & 0xFF) / 255.0f,   // RGBA (cformat=2)
                                 (float)((clearARGB >> 16) & 0xFF) / 255.0f,
                                 (float)((clearARGB >>  8) & 0xFF) / 255.0f, 1.0f };
                if (g_probe == 1) { cst[0] = 1.0f; cst[1] = 0.0f; cst[2] = 0.0f; }
                float mode[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
                float key[4]  = { 0, 0, 0, 0 };
                sceGxmSetUniformDataF(fu, PC, 0, 4, cst);
                sceGxmSetUniformDataF(fu, PM, 0, 4, mode);
                if (PK) sceGxmSetUniformDataF(fu, PK, 0, 4, key);
            }
            if (!flatClear) {
                bind_page(-1, SCE_GXM_TEXTURE_FILTER_POINT, 0);
                if (!g_hwPal) sceGxmSetFragmentTexture(g_ctx, 1, &g_palTex[slot]);
            }
            sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                       (uint16_t*)g_idxBuf[slot].p + ni, 6);
            ++g_calls;
        }
    }

    // 2. THE BATCHES, IN ORDER. D2 draws painter's-style: reordering changes
    //    the image, and there's no depth buffer to compensate.
    for (uint32_t k = 0; k < nb; ++k) {
        const d2gr::Batch& bb = b[k];
        if (!bb.count) continue;
        // D2_GXMBLEND=n: forces EVERY batch through the same blend program.
        // The image is wrong; what this measures is the cost of blending
        // itself (0 = opaque, no buffer read).
        int bi = (g_forceBlend >= 0) ? g_forceBlend : (int)(bb.blend & 3);
        const bool keyed = (bb.flags & 1) && (g_probe < 2);
        sceGxmSetFragmentProgram(g_ctx, FP[bi]);
        void* fu = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(g_ctx, &fu);
        if (fu) {
            // Glide's constant color is RGBA: r<<24 | g<<16 | b<<8 | a.
            float cst[4] = { (float)((bb.cst >> 24) & 0xFF) / 255.0f,
                             (float)((bb.cst >> 16) & 0xFF) / 255.0f,
                             (float)((bb.cst >>  8) & 0xFF) / 255.0f,
                             (float)( bb.cst        & 0xFF) / 255.0f };
            // D2_GXMPROBE=2: disables chromakey for ALL batches. Second half
            // of the isolation test: if sprites then show up as solid
            // rectangles, chromakey was eating them; if they're still
            // invisible, it's the page sampling.
            // This knob does NOT remove the cost of `discard`: the
            // instruction stays in the program, and its mere PRESENCE is what
            // flips the SGX into "punch-through" mode. To measure that cost,
            // use the shader without discard: D2_GXMPROBE=3.
            float mode[4] = { (bb.flags & 4) ? 1.0f : 0.0f,
                              keyed ? 1.0f : 0.0f,
                              (bb.flags & 8) ? 1.0f : 0.0f, 0.0f };
            float key[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // chromakey color: black
            sceGxmSetUniformDataF(fu, PC, 0, 4, cst);
            sceGxmSetUniformDataF(fu, PM, 0, 4, mode);
            if (PK) sceGxmSetUniformDataF(fu, PK, 0, 4, key);
        }
        {
            // D2 only requests bilinear (bit 1) for the floor. With the
            // SOFTWARE palette, the page is an INDEX texture: the GPU would
            // interpolate palette indices between neighboring texels, and the
            // shader's LUT turns that into unrelated colors — visible as
            // pixel noise across the floor, sprites unaffected. Filtering
            // only makes sense on COLORS, so it's only valid with the
            // hardware palette (P8_ABGR: the GPU filters AFTER the palette
            // lookup).
            // It still isn't honored by default even there: LINEAR on a P8
            // page is an unvalidated hardware combination that has hung the
            // console. D2_GXMFILTRE=1 re-enables it (and, with the software
            // palette, brings back the broken floor); try it only after the
            // hardware palette alone is confirmed working.
            const bool wantLinear = (bb.flags & 2) && (g_filtre == 1);
            const SceGxmTextureFilter f = wantLinear ? SCE_GXM_TEXTURE_FILTER_LINEAR
                                                     : SCE_GXM_TEXTURE_FILTER_POINT;
            if ((bb.flags & 2) && !wantLinear) ++g_filtreRefuse;
            const bool hasPage = (bb.page >= 0 && bb.page < MAXPAGES && g_page[bb.page].p);
            bind_page(hasPage ? (int)bb.page : -1, f, (int)bb.pal);
        }
        // With the HARDWARE palette, unit 1 no longer exists in the program:
        // not binding it is also one fewer call per batch.
        if (!g_hwPal) sceGxmSetFragmentTexture(g_ctx, 1, &g_palTex[slot]);
        sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                   (uint16_t*)g_idxBuf[slot].p + bb.first, bb.count);
        ++g_calls;
    }

    // ---- MENU RADIAL, SUR LE GPU -------------------------------------------
    // Dernier dessin de la scène, donc au-dessus de tout, et dans le même
    // passage : rien à relire, rien à recopier. Les sommets sont écrits APRÈS
    // ceux du jeu (et après le quad d'effacement, qui en consomme 4/6) dans la
    // place libre des tampons du slot — exactement le procédé du quad
    // d'effacement juste au-dessus.
    if (g_uiOk) {
        if (const radial_menu::State* rm = d2vita_radial_state()) {
            radial_menu::Quad q[2 * RM_N];
            const int nq = radial_menu::build_quads(*rm, SCR_W, SCR_H, q, 2 * RM_N);
            const uint32_t vb = nv + 4, ib = ni + 6;
            if (nq > 0 && vb + (uint32_t)nq * 4 <= MAXV && ib + (uint32_t)nq * 6 <= MAXI) {
                d2gr::Vtx* uv = (d2gr::Vtx*)g_vtxBuf[slot].p + vb;
                uint16_t*  ui = (uint16_t*)g_idxBuf[slot].p + ib;
                // build_quads rend des pixels ÉCRAN ; les sommets vivent dans
                // l'espace de la FENÊTRE DE JEU, que la projection étire de
                // façon non isotrope (800x600 -> 960x544). Sans cette
                // conversion la roue deviendrait une ellipse.
                const float kx = (float)g_gameW / (float)SCR_W;
                const float ky = (float)g_gameH / (float)SCR_H;
                for (int k = 0; k < nq; ++k) {
                    for (int c = 0; c < 4; ++c) {
                        d2gr::Vtx& v2 = uv[k * 4 + c];
                        v2.x = q[k].x[c] * kx; v2.y = q[k].y[c] * ky;
                        v2.u = q[k].u[c];      v2.v = q[k].v[c];
                        v2.argb = 0xFFFFFFFFu; v2.pal = 0.0f;
                    }
                    const uint16_t b0 = (uint16_t)(vb + k * 4);
                    ui[k*6+0] = b0;              ui[k*6+1] = (uint16_t)(b0 + 1);
                    ui[k*6+2] = (uint16_t)(b0+2); ui[k*6+3] = b0;
                    ui[k*6+4] = (uint16_t)(b0+2); ui[k*6+5] = (uint16_t)(b0 + 3);
                }
                sceGxmSetFragmentProgram(g_ctx, g_fpUi);
                sceGxmSetFragmentTexture(g_ctx, 0, &g_uiTex);
                sceGxmDraw(g_ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                           (uint16_t*)g_idxBuf[slot].p + ib, (uint32_t)nq * 6);
                ++g_calls;
            }
        }
    }

    if (g_asyncMode == 2) {
        // Arm the notification BEFORE closing the scene: the GPU will write
        // `value` to this address once fragment work is done.
        *g_notif[slot].address = 0xFFFFFFFFu;
        if (++g_notifSeq == 0xFFFFFFFFu) g_notifSeq = 1;
        g_notif[slot].value = g_notifSeq;
    }
    const uint64_t tEnd0 = sceKernelGetProcessTimeWide();
    g_batchUs += (tEnd0 - tBeg1);
    // hwPal bracket entry: EndScene is what launches the GPU on this scene.
    // Logged BEFORE, because after this point there might be nothing left to
    // log.
    if (g_hwPal && g_hwpalNewThisFrame && g_hwpalLines < HWPAL_MAXLINES) { ++g_hwpalLines; char m[160];
        std::snprintf(m, sizeof m, "gxm[hwpal]: image %llu -> sceGxmEndScene (%u descripteur(s) inedit(s), lots=%u, descripteurs=%d)",
                      (unsigned long long)frame, g_hwpalNewThisFrame, (unsigned)nb, g_texScratchN);
        d2vita_progress(m); }
    phase("sceGxmEndScene");
    const int erc = sceGxmEndScene(g_ctx, nullptr,
                                   g_asyncMode == 2 ? &g_notif[slot] : nullptr);
    if (erc < 0) { static bool said = false; if (!said) { said = true;
        char m[80]; std::snprintf(m, sizeof m, "gxm: EndScene rc=0x%08x", (unsigned)erc);
        d2vita_progress(m); } }
    const uint64_t t1 = sceKernelGetProcessTimeWide();
    g_endUs += (t1 - tEnd0);
    uint64_t t2 = t1;

    // DIAGNOSTIC FOR THE FIRST FRAMES, on the DATA side (valid in both modes:
    // these are the buffers the game thread builds, not the render output).
    { static int shown = 0;
      if (shown < 6 && nb) { ++shown;
        const d2gr::Batch& b0 = b[0];
        char q[200];
        std::snprintf(q, sizeof q, "gxm[lot 0]: page=%d melange=%u drapeaux=0x%x cst=0x%08x indices=%u"
                      " | s0=(%.1f,%.1f) uv0=(%.4f,%.4f) argb0=%08x pal0=%.4f",
                      (int)b0.page, b0.blend, b0.flags, b0.cst, b0.count,
                      (double)v[0].x, (double)v[0].y, (double)v[0].u, (double)v[0].v,
                      (unsigned)v[0].argb, (double)v[0].pal);
        d2vita_progress(q);
        if (b0.page >= 0 && b0.page < MAXPAGES && g_page[b0.page].p) {
            const int px0 = (int)(v[0].u * (float)g_pageDim), py0 = (int)(v[0].v * (float)g_pageDim);
            const uint8_t* pg = (const uint8_t*)g_page[b0.page].p + (size_t)py0 * g_pageDim + px0;
            char r[176];
            std::snprintf(r, sizeof r, "gxm[page %d]: cellule (%d,%d) octets %02x %02x %02x %02x %02x %02x %02x %02x",
                          (int)b0.page, px0, py0, pg[0], pg[1], pg[2], pg[3],
                          pg[g_pageDim], pg[g_pageDim+1], pg[g_pageDim+2], pg[g_pageDim+3]);
            d2vita_progress(r); } } }

    if (!async) {
        // ---- SYNCHRONOUS PATH ---------------------------------------------
        phase("sceGxmFinish");
        sceGxmFinish(g_ctx);
        t2 = sceKernelGetProcessTimeWide();
        // hwPal bracket exit: if this line is missing after a "-> sceGxmEndScene"
        // one, the GPU never returned control on the scene that introduced
        // the descriptors named just above.
        if (g_hwPal && g_hwpalNewThisFrame && g_hwpalLines < HWPAL_MAXLINES) { ++g_hwpalLines; char m[128];
            std::snprintf(m, sizeof m, "gxm[hwpal]: image %llu <- sceGxmFinish OK en %u us",
                          (unsigned long long)frame, (unsigned)(t2 - t1));
            d2vita_progress(m); }
        if (g_overlayOk) d2vita_overlay((uint32_t*)g_fb[slot].p);
        SceDisplayFrameBuf fb;
        std::memset(&fb, 0, sizeof fb);
        fb.size = sizeof fb; fb.base = g_fb[slot].p; fb.pitch = (unsigned)SCR_W;
        fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        fb.width = (unsigned)SCR_W; fb.height = (unsigned)SCR_H;
        phase("sceDisplaySetFrameBuf");
        const int drc = sceDisplaySetFrameBuf(&fb, g_vsyncMode == 0 ? SCE_DISPLAY_SETBUF_IMMEDIATE
                                                                    : SCE_DISPLAY_SETBUF_NEXTFRAME);
        if (g_vsyncMode >= 2) sceDisplayWaitVblankStart();
        const uint64_t t3 = sceKernelGetProcessTimeWide();
        g_presUs += (t3 - t2);
        // A black screen has two similar-looking causes: "the GPU wrote
        // nothing" and "the display isn't looking at our buffer". Counting
        // non-black pixels AFTER sceGxmFinish tells them apart, and it's the
        // only way not to waste time chasing the wrong one.
        static int shown = 0;
        if (shown < 3) { ++shown;
            const uint32_t* px = (const uint32_t*)g_fb[slot].p;
            const uint32_t tot = (uint32_t)SCR_W * (uint32_t)SCR_H;
            uint32_t nz = 0;
            for (uint32_t k = 0; k < tot; k += 7) if ((px[k] & 0x00FFFFFFu)) ++nz;
            char m[176];
            std::snprintf(m, sizeof m, "gxm[img %d]: SetFrameBuf rc=0x%08x | lots=%u sommets=%u"
                          " | pixels non noirs=%u/%u | px[0]=%08x",
                          shown, (unsigned)drc, (unsigned)nb, (unsigned)nv, nz, tot / 7,
                          (unsigned)px[0]);
            d2vita_progress(m); }
    } else if (g_asyncMode == 2) {
        // ---- DEFERRED ASYNC PATH, ALL ON THE GAME THREAD -------------------
        if (g_pendN < MAXRING) { g_pend[g_pendN].slot = slot;
                                 g_pend[g_pendN].frame = (uint32_t)frame; ++g_pendN; }
        if (g_anySubmitted) g_inflightAcc += (uint64_t)(g_lastSubmitted - g_doneFrame);
        // Frame N just went to the GPU. Frame N-1 is presented now — the GPU
        // has had the whole compute time of frame N to finish it. This is the
        // overlap, and the published wait should tend toward zero.
        if (g_pendN > 1) retire_one();
        t2 = sceKernelGetProcessTimeWide();
        if (g_cbRuns < 3 || frame < 3) { char m[160];
            std::snprintf(m, sizeof m, "gxm[differe %u]: EndScene rc=0x%08x slot=%d en-file=%d"
                          " | presentees=%u derniere=%u pixels-non-noirs=%u",
                          (unsigned)frame, (unsigned)erc, slot, g_pendN,
                          (unsigned)g_cbRuns, (unsigned)g_doneFrame, (unsigned)g_pixNonNoirs);
            d2vita_progress(m); }
        if (!g_cbAlarm && frame >= 16 && g_cbRuns == 0) { g_cbAlarm = true;
            d2vita_progress("gxm: ALARME — 16 images soumises, 0 presentee :"
                            " la notification de fin de fragment n'arrive jamais."); }
        g_lastSubmitted = (uint32_t)frame;
        g_anySubmitted = true;
        g_front = slot;
        if (g_yieldUs > 0) sceKernelDelayThread((SceUInt)g_yieldUs);
    } else {
        // ---- ASYNC PATH VIA sceGxm's DISPLAY QUEUE -------------------------
        // MILESTONES FOR THE FIRST FRAMES. Logging each step here makes it
        // possible to tell whether the game thread is stuck in EndScene, in
        // PadHeartbeat, or in the queue. Limited to the first three frames.
        const bool trace = (g_cbRuns < 3 || frame < 3);
        if (trace) { char m[120];
            std::snprintf(m, sizeof m, "gxm[async %u]: EndScene rc=0x%08x, slot=%d prec=%d -> PadHeartbeat",
                          (unsigned)frame, (unsigned)erc, slot, prev);
            d2vita_progress(m); }
        // sceGxmPadHeartbeat: required by sceGxm as soon as a display surface
        // lives across more than one scene — without it, the driver can miss
        // the dependency and the hardware resumes too late.
        const int hrc = sceGxmPadHeartbeat(&g_color[slot], g_sync[slot]);
        DispData dd; dd.slot = (uint32_t)slot; dd.frame = (uint32_t)frame;
        // Frames ALREADY submitted but not yet presented, BEFORE this entry:
        // this measures the overlap (0 = effectively still synchronous
        // without realizing it).
        if (g_anySubmitted) g_inflightAcc += (uint64_t)(g_lastSubmitted - g_doneFrame);
        if (trace) { char m[120];
            std::snprintf(m, sizeof m, "gxm[async %u]: PadHeartbeat rc=0x%08x -> AddEntry (rappels=%u)",
                          (unsigned)frame, (unsigned)hrc, (unsigned)g_cbRuns);
            d2vita_progress(m); }
        // THIS IS THE ONLY PLACE THE GAME THREAD CAN BLOCK.
        // sceGxmDisplayQueueAddEntry waits if the queue already has
        // displayQueueMaxPendingCount entries. That block IS the residual GPU
        // wait: it's published under the same name as the synchronous path so
        // the two modes compare line for line.
        phase("sceGxmDisplayQueueAddEntry");
        const int arc = sceGxmDisplayQueueAddEntry(g_sync[prev], g_sync[slot], &dd);
        t2 = sceKernelGetProcessTimeWide();
        if (arc < 0) { static bool said = false; if (!said) { said = true;
            char m[96]; std::snprintf(m, sizeof m, "gxm: DisplayQueueAddEntry rc=0x%08x", (unsigned)arc);
            d2vita_progress(m); } }
        if (t2 - t1 > 200) ++g_bufWaits;
        if (trace) { char m[144];
            std::snprintf(m, sizeof m, "gxm[async %u]: AddEntry rc=0x%08x en %u us | rappels=%u presentee=%u pixels-non-noirs=%u",
                          (unsigned)frame, (unsigned)arc, (unsigned)(t2 - t1),
                          (unsigned)g_cbRuns, (unsigned)g_doneFrame, (unsigned)g_pixNonNoirs);
            d2vita_progress(m); }
        g_lastSubmitted = (uint32_t)frame;
        g_anySubmitted = true;
        g_front = slot;
        // ALARM: the display queue never returned. Logged once — but it
        // turns a silent hang into a named failure, and says which way to
        // look.
        if (!g_cbAlarm && frame >= 16 && g_cbRuns == 0) { g_cbAlarm = true;
            d2vita_progress("gxm: ALARME — 16 images soumises, 0 rappel d'affichage :"
                            " le fil d'affichage de sceGxm ne tourne pas (coeur ? priorite ?)."
                            " Repli : D2_GXMASYNC absent."); }
        // YIELDING. The Vita kernel schedules RUN-TO-BLOCK among threads of
        // equal priority on a core. In synchronous mode, the blocking
        // sceGxmFinish call gave up the game thread's core to its neighbors
        // every frame — including sceGxm's own display thread, whose core and
        // priority aren't ours to choose. Removing that block without
        // replacing it turns the game thread into a CPU hog, a strong
        // candidate for causing a hang. So control is yielded explicitly,
        // once per frame, for a few tens of microseconds. D2_GXMYIELD=0
        // disables it.
        if (g_yieldUs > 0) sceKernelDelayThread((SceUInt)g_yieldUs);
    }

    phase("jeu (hors gxm)");
    g_totUs += (sceKernelGetProcessTimeWide() - tEntry);
    ++g_submitted;
    g_back = slot;
    g_subUs += (t1 - t0);
    // In mode 2, retire_one() counts the wait AND the presentation,
    // separately. Adding (t2-t1) here would count the same duration twice and
    // fabricate a GPU wait that doesn't exist.
    if (g_asyncMode != 2) g_waitUs += (t2 - t1);
    ++g_frames; g_lots += nb; g_verts += nv;
}

int d2gxm_counters(char* out, unsigned n) {
    if (!g_ready || !g_frames) return 0;
    const uint64_t f = g_frames;
    const int r = std::snprintf(out, n,
        "gxm: images=%llu lots/img=%llu sommets/img=%llu appels/img=%llu pages=%d rendu=%dx%d"
        " soumission-us=%llu attente-gpu-us=%llu presentation-us=%llu"
        " mode=%d en-vol=%llu,%02llu attentes-tampon=%llu vidages=%llu rappels=%u vsync=%d"
        " | recopie-us=%llu debut-scene-us=%llu lots-us=%llu fin-scene-us=%llu"
        " finish-us=%llu setframebuf-us=%llu somme-us=%llu total-submit-us=%llu"
        " | pal=%s effacement=%s melange=%s sonde=%d f=%s"
        " palette-debordee=%llu filtre-refuse/img=%llu",
        (unsigned long long)f, (unsigned long long)(g_lots / f),
        (unsigned long long)(g_verts / f), (unsigned long long)(g_calls / f),
        g_pageN, SCR_W, SCR_H,
        (unsigned long long)(g_subUs / f), (unsigned long long)(g_waitUs / f),
        (unsigned long long)(g_presUs / f), g_asyncMode,
        (unsigned long long)(g_inflightAcc * 100 / f / 100),
        (unsigned long long)(g_inflightAcc * 100 / f % 100),
        (unsigned long long)g_bufWaits, (unsigned long long)g_drains,
        (unsigned)g_cbRuns, g_vsyncMode,
        (unsigned long long)(g_copyUs / f), (unsigned long long)(g_beginUs / f),
        (unsigned long long)(g_batchUs / f), (unsigned long long)(g_endUs / f),
        (unsigned long long)(g_waitUs / f), (unsigned long long)(g_presUs / f),
        (unsigned long long)((g_copyUs + g_beginUs + g_batchUs + g_endUs + g_waitUs + g_presUs) / f),
        (unsigned long long)(g_totUs / f),
        g_hwPal ? "hw" : "shader",
        g_clearName,
        g_forceBlend < 0 ? "mesure" : "force", g_probe, g_fragName,
        (unsigned long long)g_texScratchOver, (unsigned long long)(g_filtreRefuse / f));
    g_frames = 0; g_lots = 0; g_verts = 0; g_calls = 0; g_subUs = 0; g_waitUs = 0;
    g_presUs = 0; g_inflightAcc = 0; g_bufWaits = 0; g_drains = 0; g_filtreRefuse = 0;
    g_texScratchOver = 0;
    g_copyUs = 0; g_beginUs = 0; g_batchUs = 0; g_endUs = 0; g_totUs = 0;
    return r;
}

void d2gxm_shutdown() {
    if (!g_ready) return;
    g_ready = false;
    sceGxmFinish(g_ctx);
    sceGxmDisplayQueueFinish();
}

#endif // __vita__
