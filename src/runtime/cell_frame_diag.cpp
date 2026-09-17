// cell_frame_diag.cpp -- see cell_frame_diag.h.
#include "cell_frame_diag.h"
#include "runtime/rt_host.h"
#include "platform/vita_present.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// ---- D2_LOOPSHAPE=1: the SHAPE of the frame --------------------------------
// Two questions decide whether a dedicated thread can help with compositing,
// and NEITHER can be guessed:
//   (1) Is the burst of calls to the cell-blit loop CONTIGUOUS, or do other
//       frame writes interleave with it? If they interleave, deferring
//       REORDERS drawing: sprites would end up under the floor. This is the
//       correctness lock.
//   (2) Is there a WINDOW left between the end of the burst and presentation?
//       If the floor is the last thing drawn, another core has no time to
//       absorb any of the work and the critical thread just waits at the
//       barrier — the work would have moved without any gain.
// This counts events (free integers) and samples the clock on every call to
// the loop — about 0.25% of a frame's budget: visible but not structural, and
// the only way to know when the burst ENDS. The knob keeps all of this out of
// the reference build.
// Who CALLS the floor loop? It is never called directly (no rel32 call in
// .text): it's entry #3 of a 7-pointer table in .data at 0x72da60, registered
// with the render backend (0x4f5448/0x4f5492, via ds:0x7c8cc0 -> [eax+4]). The
// caller is therefore not findable statically. It's captured at runtime
// instead: at shim entry, [esp] IS the return address, i.e. the call site.
// Open-addressed table, bounded linear probing, never reallocates. MANDATORY
// VERIFICATION: a return address always follows a `call` — if disassembly
// doesn't show one right before it, the capture is wrong (exactly the defect
// the TLSWRAP histogram had).
uint32_t g_callerKey[D2_CALLER_N];
uint64_t g_callerHit[D2_CALLER_N];
void note_caller(uint32_t ra){
    const unsigned h=(ra>>2)&(D2_CALLER_N-1);
    for(unsigned i=0;i<D2_CALLER_N;i++){
        const unsigned q=(h+i)&(D2_CALLER_N-1);
        if(!g_callerHit[q]){ g_callerKey[q]=ra; g_callerHit[q]=1; return; }
        if(g_callerKey[q]==ra){ ++g_callerHit[q]; return; } } }

static uint32_t g_shSeq=0, g_shFirst=0, g_shLast=0, g_shN=0;
static uint64_t g_shUsFirst=0, g_shUsLast=0, g_shUsFrame0=0;
uint64_t g_shFrames=0, g_shSumEv=0, g_shSumN=0, g_shSumBefore=0,
                g_shSumIntrus=0, g_shSumAfter=0,
                g_shSumUsBurst=0, g_shSumUsWindow=0, g_shSumUsFrame=0;
// ANOTHER frame write (rle, light, grid, blend, and a FALLBACK call's guest blit).
// Intruders are not all equal. A FALLBACK call's guest blit would disappear if
// the port were widened; the rle and light writes would not — they are real,
// independent writes. Counting a single total would conflate "should we widen
// the port" with "can we defer", which are separate questions. So the two
// families are counted separately, between the FIRST and LAST call of the burst.
// THREE families, because they don't raise the same question:
//   0 = INDEPENDENT frame write (the RLE) — remains after widening
//   1 = a FALLBACK call's guest blit — disappears if the port is widened
//   2 = does NOT write the frame (light, grid) — doesn't participate in ordering
static uint64_t g_shBlitSeen=0, g_shOtherSeen=0, g_shNodrawSeen=0;
static uint64_t g_shBlitAtLast=0, g_shOtherAtLast=0, g_shNodrawAtLast=0;
uint64_t g_shSumIntrusBlit=0, g_shSumIntrusOther=0, g_shSumIntrusNodraw=0;
void sh_ev(int fam){ if(!g_shOn) return; ++g_shSeq;
    if(g_shN){ if(fam==1) ++g_shBlitSeen; else if(fam==2) ++g_shNodrawSeen; else ++g_shOtherSeen; } }
// A call to the cell-blit loop — served OR fell back: the burst is defined
// by calls, not by whether they were served.
void sh_loop(){ if(!g_shOn) return;
    uint32_t q=++g_shSeq;
    if(!g_shN){ g_shFirst=q; g_shUsFirst=rt_now_us(); }
    g_shLast=q; g_shUsLast=rt_now_us(); ++g_shN;
    g_shBlitAtLast=g_shBlitSeen; g_shOtherAtLast=g_shOtherSeen;
    g_shNodrawAtLast=g_shNodrawSeen; }
// Frame boundary (called from dumpFrame, hooked to StretchBlt).
void sh_frame(){ if(!g_shOn) return;
    uint64_t now=rt_now_us();
    if(g_shN){
        ++g_shFrames;
        g_shSumEv+=g_shSeq; g_shSumN+=g_shN;
        g_shSumBefore+=(g_shFirst-1);
        // INTRUDER = an event that falls BETWEEN the burst's first and last call
        // without being one of them: exactly what deferring would break.
        g_shSumIntrus+=(g_shLast-g_shFirst+1-g_shN);
        g_shSumIntrusBlit+=g_shBlitAtLast; g_shSumIntrusOther+=g_shOtherAtLast;
        g_shSumIntrusNodraw+=g_shNodrawAtLast;
        g_shSumAfter+=(g_shSeq-g_shLast);
        g_shSumUsBurst+=(g_shUsLast-g_shUsFirst);
        g_shSumUsWindow+=(now>g_shUsLast? now-g_shUsLast : 0);
        if(g_shUsFrame0) g_shSumUsFrame+=(now-g_shUsFrame0);
    }
    g_shUsFrame0=now; g_shSeq=0; g_shN=0; g_shFirst=0; g_shLast=0;
    g_shBlitSeen=0; g_shOtherSeen=0; g_shNodrawSeen=0;
    g_shBlitAtLast=0; g_shOtherAtLast=0; g_shNodrawAtLast=0; }

// ---- D2_CAMVEC: INTEGER camera vector, measured from floor blocks ---------
// The cell-blit loop Game+0xf8b80 receives the ABSOLUTE SCREEN anchor point
// (Xa=EDX, Ya=[ESP+4]) of each cell block (ECX). From one frame to the next, a
// static scenery block's anchor moves by EXACTLY the camera vector. The mode
// of the (Xa,Ya) differences, indexed by block pointer, therefore gives the
// vector with no image search at all.
// Logging: one line per frame on stdout. No effect on rendering.
static std::unordered_map<uint32_t,uint64_t> g_cvCur, g_cvPrev;   // blk -> (Xa<<32)|Ya
void cv_note(uint32_t blk,int32_t Xa,int32_t Ya){
    if(!g_cvOn) return;
    g_cvCur[blk] = ((uint64_t)(uint32_t)Xa<<32) | (uint32_t)Ya; }
void cv_tick(int frame){
    if(!g_cvOn) return;
    // mode of the differences over blocks present in BOTH frames
    std::unordered_map<uint64_t,int> vote; int common=0;
    for(const auto& kv : g_cvCur){
        auto it=g_cvPrev.find(kv.first); if(it==g_cvPrev.end()) continue;
        int32_t x0=(int32_t)(uint32_t)(it->second>>32),  y0=(int32_t)(uint32_t)it->second;
        int32_t x1=(int32_t)(uint32_t)(kv.second>>32),   y1=(int32_t)(uint32_t)kv.second;
        ++common; ++vote[((uint64_t)(uint32_t)(x1-x0)<<32)|(uint32_t)(y1-y0)]; }
    int best=0; uint64_t bk=0;
    for(const auto& v : vote) if(v.second>best){ best=v.second; bk=v.first; }
    int32_t dx=(int32_t)(uint32_t)(bk>>32), dy=(int32_t)(uint32_t)bk;
    std::printf("[camvec] f=%d dx=%d dy=%d votes=%d/%d blocs=%zu\n",
                frame,(int)dx,(int)dy,best,common,g_cvCur.size());
    g_cvPrev.swap(g_cvCur); g_cvCur.clear(); }

// ---- SUB-TIMERS FOR THE PER-FRAME BODY (D2_FRAMEUS) ------------------------
// The StretchBlt shim costs several ms per frame on console, and much of that
// budget wasn't attributed to ANYTHING: it was known the body was expensive,
// not WHERE. Four microsecond accumulators answer that — cmd_poll,
// d2vita_input_tick, d2vita_present, and the REST (total body minus those three).
//
// PUBLISHED VIA d2vita_progress, not printf: on console printf reaches no log
// at all.
//
// SCOPE: only calls made FROM the per-frame body are counted. The message
// pump also calls cmd_poll and d2vita_input_tick (16 ms gated); those calls
// are deliberately NOT counted — the question being asked is "what does the
// per-frame body cost", not "what does the function cost". A cmd=0 therefore
// means "the pump paid for it", not "it's free".
//
// COST: 8 clock reads per frame (~150/s at 19 fps). D2_FRAMEUS=0 removes all
// of them, including publication.
uint64_t g_fuCmd=0, g_fuInput=0, g_fuPresent=0;
static uint64_t g_fuBody=0, g_fuBodyMax=0;
static uint64_t g_fuN=0, g_fuPubT=0;
static inline bool fu_on(){ static int e=-1;
    if(e<0){ const char* v=getenv("D2_FRAMEUS"); e=(v&&!std::strcmp(v,"0"))?0:1; }
    return e!=0; }
static void fu_body(uint64_t t0){
    const uint64_t now=rt_now_us(), dt=now-t0;
    g_fuBody+=dt; ++g_fuN; if(dt>g_fuBodyMax) g_fuBodyMax=dt;
    if(!g_fuPubT){ g_fuPubT=now; return; }
    if(now-g_fuPubT < 10000000ull) return;          // 10 s
    const uint64_t n=g_fuN?g_fuN:1, som=g_fuCmd+g_fuInput+g_fuPresent;
    const uint64_t reste = g_fuBody>som ? g_fuBody-som : 0;
    char m[192];
    std::snprintf(m,sizeof m,
        "corps/img n=%llu tot=%llu us (max %llu) cmd=%llu input=%llu present=%llu reste=%llu",
        (unsigned long long)n,(unsigned long long)(g_fuBody/n),(unsigned long long)g_fuBodyMax,
        (unsigned long long)(g_fuCmd/n),(unsigned long long)(g_fuInput/n),
        (unsigned long long)(g_fuPresent/n),(unsigned long long)(reste/n));
    d2vita_progress(m);
    std::printf("  [corps] %s\n",m); std::fflush(stdout);
    g_fuCmd=g_fuInput=g_fuPresent=g_fuBody=g_fuBodyMax=g_fuN=0; g_fuPubT=now;
}
// RAII guards: dumpFrame has five early returns; a hand-placed timer would
// miss four of them.
FuScope::FuScope(uint64_t* a): t0(fu_on()?rt_now_us():0), acc(a) {}
FuScope::~FuScope(){ if(t0) *acc += rt_now_us()-t0; }
FuBody::FuBody(): t0(fu_on()?rt_now_us():0) {}
FuBody::~FuBody(){ if(t0) fu_body(t0); }
