// replay60.cpp — host-side 60Hz replay engine. See replay60.h.
//
// PORTABLE: builds and runs under qemu (pthread thread, CLOCK_MONOTONIC timer,
// d2gxm_submit is a no-op => only the replay's CPU cost is measured) and on
// Vita (self-pinned sceKernel thread, paced by sceDisplayWaitVblankStart,
// real GPU submission). The game thread only calls push() and line().
#include "replay60.h"
#include "platform/vita_gxm.h"
#include "platform/vita_present.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
extern "C" int d2vita_pin_self(int mask, unsigned* relu);
// vita_host.h is the single source of truth for the per-core pin masks used below.
#include "platform/vita_host.h"
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

namespace d2gr {

// ---- clock / sleep ------------------------------------------------------
static uint64_t r60_now_us() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}
static void r60_sleep_us(uint64_t us) {
    if (!us) return;
#ifdef __vita__
    sceKernelDelayThread((SceUInt)us);
#else
    struct timespec ts; ts.tv_sec = (time_t)(us / 1000000ull); ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
#endif
}
static void r60_say(const char* m) {
    d2vita_progress(m);
    std::printf("[%s]\n", m); std::fflush(stdout);
}

// ---- knobs, read once -----------------------------------------------------
static int knob_int(const char* name, int def, int lo, int hi) {
    const char* e = getenv(name); if (!e || !*e) return def;
    long v = atol(e); if (v < lo || v > hi) return def; return (int)v;
}
bool Replay60::on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("D2_REPLAY60"); v = (e && *e && strcmp(e, "0")) ? 1 : 0; }
    return v != 0;
}
bool Replay60::tagOn() {
    static int v = -1;
    if (v < 0) {
        const char* a = getenv("D2_RINGTAG"); const char* b = getenv("D2_RINGPHASE_X");
        v = (on() || (a && *a && strcmp(a, "0")) || (b && *b)) ? 1 : 0; }
    return v != 0;
}
int Replay60::mode() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("D2_REPLAY60_MODE"); v = (e && (*e == 'e' || *e == 'E')) ? 1 : 0; }
    return v;
}
int Replay60::dumpLeft() {
    static int left = -1;
    if (left < 0) left = knob_int("D2_REPLAY60_DUMP", 0, 0, 100000);
    if (left > 0) return left--;
    return 0;
}

Replay60& r60() { static Replay60 s; return s; }

// ---- spin lock (critical sections lasting a few tens of ns) ------------------
void Replay60::lock()   { while (__sync_lock_test_and_set(&lock_, 1)) { } }
void Replay60::unlock() { __sync_lock_release(&lock_); }

int Replay60::freeSlot() const {
    for (int i = 0; i < SLOTS; ++i)
        if (i != cur_ && i != prev_ && !slot_[i].busy) return i;
    return -1;
}

uint64_t Replay60::hashVerts(const Vtx* v, uint32_t n) const {
    uint64_t h = 1469598103934665603ull;
    const uint32_t* w = (const uint32_t*)v;
    const size_t nw = (size_t)n * (sizeof(Vtx) / 4);
    for (size_t i = 0; i < nw; ++i) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}

// ---- push by the game thread -------------------------------------------------
void Replay60::push(const Vtx* v, uint32_t nv, const uint16_t* idx, uint32_t ni,
                    const Batch* b, uint32_t nb, const R60Unit* units, uint32_t nu,
                    uint32_t uiV0, bool camValid, uint32_t playerId, int32_t px, int32_t py,
                    uint32_t offX, uint32_t offY, uint32_t tick, uint64_t frame, uint32_t clear) {
    lock();
    const int s = freeSlot();
    if (s >= 0) slot_[s].busy = true;       // reserved: the replay thread won't take it
    unlock();
    if (s < 0) { ++st_.dropped; return; }
    R60Frame& f = slot_[s];
    f.v.assign(v, v + nv);
    f.idx.assign(idx, idx + ni);
    f.b.assign(b, b + nb);
    f.units.assign(units, units + nu);
    f.uiV0 = uiV0 > nv ? nv : uiV0;
    f.camValid = camValid; f.playerId = playerId; f.playerX = px; f.playerY = py;
    f.offX = offX; f.offY = offY;
    f.tick = tick; f.frame = frame; f.clear = clear;
    f.tUs = r60_now_us(); f.valid = true;
    // composition stats
    ++st_.pushes; st_.vertsTotal += nv;
    uint64_t vu = 0; for (uint32_t i = 0; i < nu; ++i) { if (units[i].v1 > units[i].v0) vu += units[i].v1 - units[i].v0; st_.unitDraws += units[i].draws; }
    st_.unitsTagged += nu; st_.vertsUnit += vu;
    st_.vertsUi += nv - f.uiV0;
    st_.vertsWorld += (f.uiV0 > vu) ? f.uiV0 - vu : 0;
    lock();
    const int old = prev_;
    prev_ = cur_; cur_ = s;
    f.busy = false;
    if (old >= 0 && old != prev_ && old != cur_) slot_[old].valid = false;
    if (!active_) { active_ = true; ++st_.wakeups; }   // IN-GAME: the replay thread wakes up
    unlock();
}

// ---- out-of-game frame: idle -------------------------------------------------------
// Game thread. No valid CAMERA in the frame (menus, loading, returning to the
// menu): the engine forgets N and N-1 (a fresh game must not interpolate from
// a previous game's frame) and goes idle. The caller submits it itself.
void Replay60::sleepFrame() {
    ++st_.directFrames;
    if (!active_) return;
    lock();
    active_ = false; cur_ = -1; prev_ = -1;
    for (int i = 0; i < SLOTS; ++i) slot_[i].valid = false;   // busy is left to the replay thread
    ++st_.sleeps;
    unlock();
}

// ---- one replay iteration ---------------------------------------------------
bool Replay60::step(uint64_t nowUs) {
    lock();
    if (!active_ || cur_ < 0 || !slot_[cur_].valid) { unlock(); return false; }
    R60Frame& N = slot_[cur_];
    // BOUND: never more than MAX_REPLAYS_PER_FRAME replays of the same game frame
    if (N.frame != capFrame_) { capFrame_ = N.frame; capCount_ = 0; }
    if (capCount_ >= (unsigned)MAX_REPLAYS_PER_FRAME) { unlock(); ++st_.capped; return false; }
    ++capCount_; if (capCount_ > st_.maxPerFrame) st_.maxPerFrame = capCount_;
    R60Frame* P = (prev_ >= 0 && slot_[prev_].valid) ? &slot_[prev_] : nullptr;
    N.busy = true; if (P) P->busy = true;
    unlock();

    const uint64_t t0 = r60_now_us();
    // displacement factor f: Δ = f * (pos(N) − pos(N−1))
    //   interp: t in [N−1, N]  => f = α − 1, α = age/period in [0,1]
    //   extrap: t beyond N     => f = α,     α in [0, 1.5]
    uint64_t period = 40000;
    if (P && N.tUs > P->tUs) period = N.tUs - P->tUs;
    if (period < 20000) period = 20000; if (period > 80000) period = 80000;
    const uint64_t age = nowUs > N.tUs ? nowUs - N.tUs : 0;
    double alpha = (double)age / (double)period;
    double f;
    if (mode() == 0) { if (alpha > 1.0) alpha = 1.0; f = alpha - 1.0; }
    else             { if (alpha > 1.5) alpha = 1.5; f = alpha; }
    if (!P) { f = 0.0; ++st_.noPrev; }
    st_.alphaSum1000 += (uint64_t)(alpha * 1000.0);

    // camera: Δcam = f * (player(N) − player(N−1)), in screen pixels
    float camDx = 0.f, camDy = 0.f;
    if (P && N.camValid && P->camValid && f != 0.0) {
        float sx, sy; worldToScreen((int64_t)N.playerX - P->playerX, (int64_t)N.playerY - P->playerY, sx, sy);
        camDx = (float)(f * sx); camDy = (float)(f * sy);
    } else if (!N.camValid) ++st_.noCam;

    // table of N−1's units, rebuilt only when N−1 changes
    if (P && prevUnitsFrame_ != P->frame) { prevUnits_ = P->units; prevUnitsFrame_ = P->frame; }

    // On arrival of a NEW frame N: metric verification. For every unit present
    // in both N−1 and N with the same vertex count, the average REAL
    // displacement of its vertices must equal conv(Δunit) − conv(Δcam).
    if (P && lastSubmitFrame_ != N.frame) {
        float csx = 0.f, csy = 0.f;
        if (N.camValid && P->camValid) worldToScreen((int64_t)N.playerX - P->playerX, (int64_t)N.playerY - P->playerY, csx, csy);
        for (size_t i = 0; i < N.units.size(); ++i) {
            const R60Unit& u = N.units[i];
            if (u.v1 <= u.v0) continue;
            ++st_.unitsDrawn;
            for (size_t j = 0; j < prevUnits_.size(); ++j) {
                const R60Unit& q = prevUnits_[j];
                if (q.id != u.id || q.type != u.type) continue;
                ++st_.unitsMatched;
                const int64_t ddx = (int64_t)u.x - q.x, ddy = (int64_t)u.y - q.y;
                const uint64_t d = (uint64_t)(ddx < 0 ? -ddx : ddx) + (uint64_t)(ddy < 0 ? -ddy : ddy);
                st_.dposSum += d; if (d > st_.dposMax) st_.dposMax = d; ++st_.dposN;
                if (u.v1 - u.v0 == q.v1 - q.v0 && q.v1 <= P->v.size() && u.v1 <= N.v.size()) {
                    double rx = 0, ry = 0; const uint32_t n = u.v1 - u.v0;
                    for (uint32_t k = 0; k < n; ++k) { rx += (double)N.v[u.v0 + k].x - P->v[q.v0 + k].x; ry += (double)N.v[u.v0 + k].y - P->v[q.v0 + k].y; }
                    rx /= n; ry /= n;
                    float usx, usy; worldToScreen(ddx, ddy, usx, usy);
                    const double ex = rx - ((double)usx - csx), ey = ry - ((double)usy - csy);
                    const double err = std::sqrt(ex * ex + ey * ey);
                    st_.convErrSum += err; if (err > st_.convErrMax) st_.convErrMax = err; ++st_.convErrN; if (err < 1.0) ++st_.convErrLt1;
                    if (ddx || ddy) { st_.movRealX += rx; st_.movRealY += ry; st_.movPredX += (double)usx - csx; st_.movPredY += (double)usy - csy; ++st_.movN; }
                    static int convBudget = 60;
                    if (N.units.size() && (ddx || ddy) && convBudget > 0 && dumpLeft()) { --convBudget;
                        std::printf("replay60/conv: f=%llu id=%u type=%u mode=%u dW=(%lld,%lld) reel=(%.2f,%.2f) predit=(%.2f,%.2f) cam=(%.2f,%.2f) err=%.2f px n=%u\n",
                                    (unsigned long long)N.frame, u.id, u.type, u.mode, (long long)ddx, (long long)ddy,
                                    rx, ry, (double)usx - csx, (double)usy - csy, (double)csx, (double)csy, err, n); }
                }
                break;
            }
        }
        // dump: prefer frames where the PLAYER moves (that's the camera we want to see), otherwise the first ones in-game
        const bool camMoved = (N.playerX != P->playerX) || (N.playerY != P->playerY);
        if (N.units.size() && camMoved && dumpLeft()) {
            // GROUND VERIFICATION: a world vertex (outside units, outside UI) of N
            // that shares (u,v,argb) with a vertex of N-1 within 48px is the same
            // floor tile; its screen displacement is EXACTLY -conv(Δplayer) if the
            // camera follows the player per the assumed metric. Voting at 1/2px
            // resolution.
            int best = 0; float bdx = 0.f, bdy = 0.f; int votes = 0;
            {
                struct V { int dx2, dy2, n; }; static std::vector<V> hist; hist.clear();
                const uint32_t lim = N.uiV0 < 400u ? N.uiV0 : 400u;
                for (uint32_t i = 0; i < lim; ++i) {
                    bool inUnit = false;
                    for (size_t k = 0; k < N.units.size(); ++k) if (i >= N.units[k].v0 && i < N.units[k].v1) { inUnit = true; break; }
                    if (inUnit) continue;
                    const Vtx& a = N.v[i];
                    if (a.u == 0.f && a.v == 0.f) continue;
                    for (uint32_t j = 0; j < P->uiV0; ++j) {
                        const Vtx& b = P->v[j];
                        if (b.u != a.u || b.v != a.v || b.argb != a.argb) continue;
                        const float dx = a.x - b.x, dy = a.y - b.y;
                        if (dx > 48.f || dx < -48.f || dy > 48.f || dy < -48.f) continue;
                        const int dx2 = (int)std::lround(dx * 2.f), dy2 = (int)std::lround(dy * 2.f);
                        bool found = false;
                        for (size_t h = 0; h < hist.size(); ++h) if (hist[h].dx2 == dx2 && hist[h].dy2 == dy2) { ++hist[h].n; found = true; break; }
                        if (!found) hist.push_back(V{dx2, dy2, 1});
                        ++votes; break;
                    }
                }
                for (size_t h = 0; h < hist.size(); ++h) if (hist[h].n > best) { best = hist[h].n; bdx = hist[h].dx2 / 2.f; bdy = hist[h].dy2 / 2.f; }
            }
            std::printf("replay60/cam: f=%llu dJoueur=(%lld,%lld) predit-sol=(%.2f,%.2f) mesure-sol=(%.1f,%.1f) (%d/%d votes) dOff-jeu=(%d,%d)\n",
                        (unsigned long long)N.frame, (long long)N.playerX - P->playerX, (long long)N.playerY - P->playerY,
                        (double)-csx, (double)-csy, (double)bdx, (double)bdy, best, votes, (int)(N.offX - P->offX), (int)(N.offY - P->offY));
            std::printf("replay60/img: f=%llu tick=%u unites=%u ui@%u/%u cam=%d joueur=%u (%d,%d) off=(%d,%d) periode=%llu us | dJoueur=(%lld,%lld) predit-ecran=(%.2f,%.2f) dOff-jeu=(%d,%d)\n",
                        (unsigned long long)N.frame, N.tick, (unsigned)N.units.size(), N.uiV0, (unsigned)N.v.size(),
                        (int)N.camValid, N.playerId, N.playerX, N.playerY, (int)N.offX, (int)N.offY, (unsigned long long)period,
                        (long long)N.playerX - P->playerX, (long long)N.playerY - P->playerY, (double)csx, (double)csy,
                        (int)(N.offX - P->offX), (int)(N.offY - P->offY));
            for (size_t i = 0; i < N.units.size() && i < 12; ++i) {
                const R60Unit& u = N.units[i];
                std::printf("   unite id=%u type=%u mode=%u pos=(%d,%d)=(%.3f,%.3f) sommets=[%u,%u) dessins=%u\n",
                            u.id, u.type, u.mode, u.x, u.y, u.x / 65536.0, u.y / 65536.0, u.v0, u.v1, u.draws);
            }
        }
    }

    // transformation : recopie + deplacement
    const uint32_t nv = (uint32_t)N.v.size();
    if (out_.size() < nv) out_.resize(nv);
    if (nv) std::memcpy(out_.data(), N.v.data(), (size_t)nv * sizeof(Vtx));
    const bool moving = (f != 0.0) && P;
    if (moving) {
        const uint32_t ui = N.uiV0;
        if (camDx != 0.f || camDy != 0.f)
            for (uint32_t i = 0; i < ui; ++i) { out_[i].x -= camDx; out_[i].y -= camDy; }
        for (size_t i = 0; i < N.units.size(); ++i) {
            const R60Unit& u = N.units[i];
            if (u.v1 <= u.v0 || u.v1 > nv) continue;
            const R60Unit* q = nullptr;
            for (size_t j = 0; j < prevUnits_.size(); ++j) if (prevUnits_[j].id == u.id && prevUnits_[j].type == u.type) { q = &prevUnits_[j]; break; }
            if (!q) continue;
            const int64_t ddx = (int64_t)u.x - q->x, ddy = (int64_t)u.y - q->y;
            if (!ddx && !ddy) continue;
            float sx, sy; worldToScreen(ddx, ddy, sx, sy);
            const float ux = (float)(f * sx), uy = (float)(f * sy);
            for (uint32_t k = u.v0; k < u.v1; ++k) { out_[k].x += ux; out_[k].y += uy; }
        }
    } else {
        // exact point: the copy must be bit-identical
        ++st_.replaysExact;
        if (hashVerts(out_.data(), nv) != hashVerts(N.v.data(), nv)) ++st_.replaysExactKo;
    }
    const uint64_t t1 = r60_now_us();
    st_.xformUs += t1 - t0; if (t1 - t0 > st_.xformMax) st_.xformMax = t1 - t0;

    // Submission is gated by state UNDER the sceGxm lock: if the game thread
    // has just gone idle (sleepFrame) and submits a menu frame itself, this
    // replay can't slot in after it.
    r60_gxm_lock();
    const bool live = active_;
    if (live) d2gxm_submit(out_.data(), nv, N.idx.data(), (uint32_t)N.idx.size(),
                           N.b.data(), (uint32_t)N.b.size(), N.clear, N.frame);
    r60_gxm_unlock();
    const uint64_t t2 = r60_now_us();
    if (live) {
        st_.submitUs += t2 - t1; if (t2 - t1 > st_.submitMax) st_.submitMax = t2 - t1;
        ++st_.replays; lastSubmitFrame_ = N.frame;
        // contract on the game side: a pushed frame ALWAYS carries the in-game
        // signal (valid CAMERA); a replay without it is an out-of-game replay.
        // Must stay 0.
        if (!N.camValid) ++st_.replaysOffGame;
    } else ++st_.refusedTransition;

    lock(); N.busy = false; if (P) P->busy = false; unlock();
    return live;
}

// ---- 10s window line -----------------------------------------------
int Replay60::line(char* out, unsigned n, uint32_t gameFrames) {
    R60Stats& s = st_;
    const uint64_t F = s.pushes ? s.pushes : 1, R = s.replays ? s.replays : 1;
    const double convMean = s.convErrN ? s.convErrSum / (double)s.convErrN : 0.0;
    const uint64_t dposMean = s.dposN ? s.dposSum / s.dposN : 0;
    int w = std::snprintf(out, n,
        "replay60: images-jeu=%llu rejeux=%llu (%.2f/img) mode=%s fil=%s | unites=%llu/img dont dessinees=%llu (dessins/unite=%.2f, appariees=%llu%% des dessinees)"
        " | sommets: unites=%llu%% monde=%llu%% ui=%llu%% | dpos/pas: moy=%llu.%03llu max=%llu.%03llu sous-tuiles"
        " | conv-err: moy=%.2f max=%.2f px (<1px=%llu%%, n=%llu) | cout rejeu: transf=%llu us (max %llu) soumission=%llu us (max %llu)"
        " | exact=%llu/%llu ko=%llu | alpha-moy=%llu.%03llu sans-prec=%llu sans-cam=%llu perdus=%llu"
        " | mouvement (n=%llu): reel=(%.1f,%.1f) predit=(%.1f,%.1f) px cumules"
        " | etat=%s images-hors-jeu=%llu rejeux-hors-jeu=%llu refus-transition=%llu bascules=%llu/%llu"
        " | borne%d: refus=%llu max-rejeux/img=%llu",
        (unsigned long long)s.pushes, (unsigned long long)s.replays, (double)s.replays / (double)F,
        mode() ? "extrap" : "interp", running_ ? "actif" : "ARRETE",
        (unsigned long long)(s.unitsTagged / F), (unsigned long long)(s.unitsDrawn / F),
        s.unitsTagged ? (double)s.unitDraws / (double)s.unitsTagged : 0.0,
        s.unitsDrawn ? (unsigned long long)(s.unitsMatched * 100 / s.unitsDrawn) : 0ull,
        s.vertsTotal ? (unsigned long long)(s.vertsUnit * 100 / s.vertsTotal) : 0ull,
        s.vertsTotal ? (unsigned long long)(s.vertsWorld * 100 / s.vertsTotal) : 0ull,
        s.vertsTotal ? (unsigned long long)(s.vertsUi * 100 / s.vertsTotal) : 0ull,
        (unsigned long long)(dposMean >> 16), (unsigned long long)(((dposMean & 0xFFFF) * 1000) >> 16),
        (unsigned long long)(s.dposMax >> 16), (unsigned long long)(((s.dposMax & 0xFFFF) * 1000) >> 16),
        convMean, s.convErrMax, s.convErrN ? (unsigned long long)(s.convErrLt1 * 100 / s.convErrN) : 0ull, (unsigned long long)s.convErrN,
        (unsigned long long)(s.xformUs / R), (unsigned long long)s.xformMax,
        (unsigned long long)(s.submitUs / R), (unsigned long long)s.submitMax,
        (unsigned long long)s.replaysExact, (unsigned long long)s.replays, (unsigned long long)s.replaysExactKo,
        (unsigned long long)(s.alphaSum1000 / R / 1000), (unsigned long long)(s.alphaSum1000 / R % 1000),
        (unsigned long long)s.noPrev, (unsigned long long)s.noCam, (unsigned long long)s.dropped,
        (unsigned long long)s.movN, s.movRealX, s.movRealY, s.movPredX, s.movPredY,
        active_ ? "actif" : "veille", (unsigned long long)s.directFrames, (unsigned long long)s.replaysOffGame,
        (unsigned long long)s.refusedTransition, (unsigned long long)s.wakeups, (unsigned long long)s.sleeps,
        (int)MAX_REPLAYS_PER_FRAME, (unsigned long long)s.capped, (unsigned long long)s.maxPerFrame);
    (void)gameFrames;
    s = R60Stats();
    return w;
}

// ---- the replay thread -----------------------------------------------------------
bool Replay60::threadMain() {
    const int hz = knob_int("D2_REPLAY60_HZ", 60, 10, 240);
    const uint64_t periodUs = 1000000ull / (uint64_t)hz;
    bool vsync = false;
#ifdef __vita__
    vsync = knob_int("D2_REPLAY60_VSYNC", 1, 0, 1) != 0;
    {   // self-pin: on this firmware, the mask set by the thread creator doesn't stick
        const int cpu = knob_int("D2_REPLAY60_CPU", -1, 0, 3);
        const int mask = cpu < 0 ? d2vita_core_mask(0)
                       : cpu == 0 ? 0x10000 : cpu == 1 ? 0x20000 : cpu == 3 ? WX86_CPU_MASK_USER_3 : 0x40000;
        unsigned relu = 0; const int rc = d2vita_pin_self(mask, &relu);
        d2vita_core_register("replay60", (int)sceKernelGetThreadId(), (unsigned)mask, rc);
        char m[160]; std::snprintf(m, sizeof m, "replay60: fil parti, epinglage masque=0x%x rc=0x%08x relu=0x%x, %s, mode=%s",
                                   (unsigned)mask, (unsigned)rc, relu, vsync ? "vsync" : "minuterie", mode() ? "extrap" : "interp");
        r60_say(m);
    }
#else
    { char m[120]; std::snprintf(m, sizeof m, "replay60: fil parti (pthread), minuterie %d Hz, mode=%s", hz, mode() ? "extrap" : "interp"); r60_say(m); }
#endif
    uint64_t next = r60_now_us() + periodUs;
    while (!stopReq_) {
        if (!active_) {                 // IDLE (out-of-game): no lock, 20ms slices
            r60_sleep_us(20000);
            next = r60_now_us() + periodUs;
            continue;
        }
        if (vsync) {
#ifdef __vita__
            sceDisplayWaitVblankStart();
#endif
        } else {
            const uint64_t now = r60_now_us();
            if (next > now) r60_sleep_us(next - now);
            next += periodUs;
            if (r60_now_us() > next + periodUs) next = r60_now_us() + periodUs;   // running behind: don't try to catch up
        }
        if (!step(r60_now_us())) r60_sleep_us(1000);
    }
    running_ = false;
    return true;
}

void r60_thread_entry(void* p) { ((Replay60*)p)->threadMain(); }

#ifdef __vita__
static int r60_vita_tramp(SceSize, void* argp) { r60_thread_entry(*(void**)argp); return 0; }
#else
static void* r60_pth_tramp(void* p) { r60_thread_entry(p); return nullptr; }
#endif

bool Replay60::start() {
    if (running_) return true;
    stopReq_ = false; running_ = true;
#ifdef __vita__
    SceUID th = sceKernelCreateThread("d2_replay60", r60_vita_tramp, 0x10000100, 64 * 1024, 0, 0, nullptr);
    if (th < 0) { running_ = false; r60_say("replay60: CreateThread KO — pas de rejeu"); return false; }
    void* self = this;
    if (sceKernelStartThread(th, sizeof self, &self) < 0) { running_ = false; r60_say("replay60: StartThread KO — pas de rejeu"); return false; }
#else
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, r60_pth_tramp, this) != 0) { running_ = false; r60_say("replay60: pthread_create KO — pas de rejeu"); return false; }
    pthread_detach(th);
#endif
    return true;
}
void Replay60::stop() { stopReq_ = true; for (int i = 0; i < 200 && running_; ++i) r60_sleep_us(1000); }

} // namespace d2gr

// ---- sceGxm context lock ---------------------------------------------------------
// It only exists when a second host thread touches sceGxm. Two clients: the
// 60Hz replay (Replay60::running()) and the FLUSH THREAD (D2_FLUSHFIL), which
// arms it via r60_gxm_arm() before starting. Without them the lock stays
// inert: the default path doesn't pay for a single atomic instruction.
static volatile int g_r60GxmLock = 0;
static volatile int g_r60GxmArmed = 0;
extern "C" void r60_gxm_arm()    { g_r60GxmArmed = 1; __sync_synchronize(); }
static inline bool r60_gxm_needed() { return g_r60GxmArmed || d2gr::r60().running(); }
extern "C" void r60_gxm_lock()   { if (!r60_gxm_needed()) return; while (__sync_lock_test_and_set(&g_r60GxmLock, 1)) { } }
extern "C" void r60_gxm_unlock() { if (!r60_gxm_needed()) return; __sync_lock_release(&g_r60GxmLock); }
