/* replay60.h — HOST-side 60Hz REPLAY of the last frame built.
 *
 * WHAT THIS IS. D2 simulates at 25Hz with NO interpolation: under
 * D2_ONEDRAW=2 the game draws once per 40ms step. The engine below keeps the
 * LAST TWO frames built by the ring reader (verts, batches, unit positions,
 * camera, step number) and, on a dedicated HOST thread running at 60Hz,
 * REPLAYS the latest one by moving:
 *   - each unit by  Δu = pos(t) − pos(N)   (converted to screen pixels),
 *   - the entire WORLD by −Δcam (the camera follows the player),
 *   - NOTHING in the UI (fixed).
 * then submits to the GPU via d2gxm_submit (off-target: no-op, only CPU cost
 * is measured). Two modes: `interp` (t in [N−1, N], +40ms latency, no
 * overshoot artifacts) and `extrap` (t beyond N, no latency, risk of
 * "rewind" when a unit stops or turns).
 *
 * WHAT MOVES AND WHAT DOESN'T. Only units TAGGED by the unit-draw entry hook
 * (Game+0xdc7b0, ECX = UnitAny*) carry a position: players, monsters, NPCs,
 * objects, missiles, items — everything that passes through this function.
 * Particles, floating text, and effects drawn outside this function are
 * "world": they move with the camera but not on their own. The ground (DT1)
 * is world. Panels, the cursor, and UI text come after the UI marker and stay
 * fixed.
 *
 * EXACT POINT. When t = N (interp, α = 1; extrap, α = 0) every Δ is zero and
 * the replayed frame is BIT-IDENTICAL to the game's frame (no floating-point
 * arithmetic is applied when the factor is 0: vertices are just copied). The
 * `exact=` counter verifies this by hash on every occurrence.
 *
 * METRIC. Game positions are in 16.16 sub-tiles (pPath+0 / pPath+4). Screen:
 *   dx_px = (Δx − Δy) * 16 / 65536       dy_px = (Δx + Δy) * 8 / 65536
 * (a sub-tile is a 32x16 px diamond). This is NOT assumed: the `conv-err=`
 * counter compares, for every unit present in both N−1 and N, the REAL
 * displacement of its first vertex against the displacement PREDICTED by the
 * formula (minus the camera). If it isn't ~0, the formula or the camera model
 * is wrong, and that's known before ever looking at a screen.
 *
 * OUT-OF-GAME IDLE (wt/r60-lazy). The replay thread submits NOTHING unless
 * the game is actually IN-GAME: menus, loading screens, returning to the menu
 * at the end of a game. The "in-game" SIGNAL is the CAMERA record carrying a
 * non-null player (camValid), emitted at the exit of Game+0x5b440 — called
 * once per frame by the IN-GAME draw function Game+0x4c990 (site 0x44cae3)
 * right before the world, and only when the player [0x7a6a70] exists. It
 * fires once per frame in-game and never in menus or during loading. It
 * depends on no knob (unlike the UI boundary, movable via D2_REPLAY60_UI) and
 * is not emitted by the simulation step (the TICK already lands during
 * loading); it is also, by construction, the one piece of data without which
 * the engine has nothing to move (most vertices follow the camera). Each
 * frame decides: with a valid CAMERA -> push() (the engine goes "active");
 * without -> sleepFrame() (the engine goes "idle", frames N/N-1 are
 * forgotten) and the game thread submits by itself, as if there were no
 * replay. While idle the replay thread sleeps in 20ms slices
 * (sceKernelDelayThread) and takes NO lock — neither its own, nor
 * r60_gxm_lock. Replay submission is gated by state UNDER the sceGxm lock: a
 * replayed game frame can never slot in right after a menu frame the game
 * thread just submitted (see R60Stats::replaysOffGame, which must stay 0, and
 * refusedTransition, which counts replays abandoned at the switchover).
 *
 * BOUND. Never more than MAX_REPLAYS_PER_FRAME (3) replays of the same game
 * frame: if the game falls behind (e.g. texture uploads on entering a game
 * can stall a frame for a while under the same lock), the replay thread stops
 * after the 3rd and waits for the next frame (see R60Stats::capped and
 * maxPerFrame). In steady state (60Hz / 25Hz = 2.4 per frame) the bound isn't
 * hit; in interp mode a 4th replay would be f=0 anyway, identical to what the
 * first replay of the next frame shows.
 *
 * THREADS AND LOCKS. The game thread only does `push()` (copies ~100KB into a
 * free slot, then swaps pointers under a spin lock held for a few tens of
 * ns). The replay thread never touches guest memory or the atlas. Every
 * sceGxm call from either thread (pages, palettes, drain on the game side;
 * submission on the replay side) is serialized by r60_gxm_lock() — a single
 * sceGxm context, never two threads inside it. On Vita the thread pins
 * ITSELF (d2vita_pin_self) to the presenter's core (D2_COEURS[0]), since the
 * GXM path has no presentation thread of its own.
 *
 * KNOBS (all read once; default = all unset = not a single byte changes):
 *   D2_REPLAY60=1            arms the engine (requires D2_GLIDERING=1 + D2_GLIDEGXM on target)
 *   D2_REPLAY60_MODE=interp|extrap   (default interp)
 *   D2_REPLAY60_HZ=<n>       off-vsync rate (default 60); on Vita, vsync is used by default
 *   D2_REPLAY60_VSYNC=0      Vita: replaces sceDisplayWaitVblankStart with a timer
 *   D2_REPLAY60_CPU=0..3     Vita: thread's core (default = D2_COEURS[0], the presenter)
 *   D2_REPLAY60_DUMP=<n>     prints n diagnostic lines (units, camera) then goes quiet
 *   (no knob for idle or the bound: that's behavior, not an option)
 *   D2_RINGTAG=1             ring tagging ONLY (tags/camera/tick), without the replay thread
 *   D2_RINGPHASE_X=<rva,..>  inventory: ring draws/verts per hooked function
 */
#ifndef D2VITA_REPLAY60_H
#define D2VITA_REPLAY60_H

#include <stdint.h>
#include <vector>
#include "glide_ring.h"
#include "glide_atlas.h"

/* ---- Records EMITTED BY THE HOST into the ring (never by the DLL) --------
 * Same convention (op<<24 | length in words). An old reader skips them. */
enum {
    D2GR_OP_UNITTAG  = 0x40, /* id, type, xFine, yFine, mode, raw8, rawC: unit-draw ENTRY */
    D2GR_OP_UNITEND  = 0x41, /* id: unit-draw EXIT (return trap) */
    D2GR_OP_CAMERA   = 0x42, /* offX, offY ([0x7a520c],[0x7a5208]), playerId, playerX, playerY (fine) */
    D2GR_OP_TICK     = 0x43, /* simulation step number (host-counted, 25 Hz) */
    D2GR_OP_PHASE    = 0x44  /* rva, 1=enter 0=exit: phase boundary (UI, inventory) */
};

namespace d2gr {

struct R60Unit {
    uint32_t id, type, mode;
    int32_t  x, y;          /* 16.16 sub-tiles */
    uint32_t v0, v1;        /* vertices [v0, v1) of this unit within the frame */
    uint32_t draws;
};

struct R60Frame {
    std::vector<Vtx>      v;
    std::vector<uint16_t> idx;
    std::vector<Batch>    b;
    std::vector<R60Unit>  units;
    uint32_t uiV0 = 0;              /* first UI vertex (= v.size() if no UI) */
    uint32_t playerId = 0;
    int32_t  playerX = 0, playerY = 0;   /* camera = player, 16.16 */
    uint32_t offX = 0, offY = 0;    /* game screen offsets, for verification */
    bool     camValid = false;
    uint32_t tick = 0;
    uint64_t frame = 0;
    uint32_t clear = 0;
    uint64_t tUs = 0;               /* host timestamp of the push */
    bool     valid = false;
    bool     busy = false;          /* read by the replay thread */
};

struct R60Stats {
    /* current window (reset by line()) */
    uint64_t pushes = 0, replays = 0, replaysExact = 0, replaysExactKo = 0;
    uint64_t unitsTagged = 0, unitsDrawn = 0, unitsMatched = 0, unitDraws = 0;
    uint64_t vertsTotal = 0, vertsUnit = 0, vertsWorld = 0, vertsUi = 0;
    uint64_t dposSum = 0, dposMax = 0, dposN = 0;      /* 16.16 -> published as 1/1000 sub-tile */
    double   convErrSum = 0; double convErrMax = 0; uint64_t convErrN = 0, convErrLt1 = 0;
    uint64_t xformUs = 0, xformMax = 0, submitUs = 0, submitMax = 0;
    uint64_t noPrev = 0, noCam = 0, dropped = 0, ticks = 0;
    uint64_t alphaSum1000 = 0;
    /* out-of-game idle and frame bound (wt/r60-lazy) */
    uint64_t directFrames = 0;      /* OUT-OF-GAME frames submitted by the game thread (engine idle) */
    uint64_t replaysOffGame = 0;    /* replays submitted for a frame WITHOUT the in-game signal (valid CAMERA): must stay 0 */
    uint64_t refusedTransition = 0; /* replays abandoned under the sceGxm lock because the game had just left in-game state */
    uint64_t capped = 0;            /* replays refused by the MAX_REPLAYS_PER_FRAME bound */
    uint64_t maxPerFrame = 0;       /* max replays of the same frame seen in the window */
    uint64_t wakeups = 0, sleeps = 0; /* idle->active and active->idle transitions */
    /* units IN MOTION: vector sums of real vs. predicted screen displacement
     * (a single frame's animation noise averages out over the window) */
    double movRealX = 0, movRealY = 0, movPredX = 0, movPredY = 0; uint64_t movN = 0;
};

class Replay60 {
public:
    /* knobs, read once */
    static bool on();          /* D2_REPLAY60 */
    static bool tagOn();       /* D2_REPLAY60 || D2_RINGTAG || D2_RINGPHASE_X */
    static int  mode();        /* 0 interp, 1 extrap */
    static int  dumpLeft();    /* D2_REPLAY60_DUMP: lines left (decremented) */

    /* Game thread. Pushes the built frame: copies into a free slot, then
     * swaps pointers under lock. `units`: spans already closed (v1 set). */
    void push(const Vtx* v, uint32_t nv, const uint16_t* idx, uint32_t ni,
              const Batch* b, uint32_t nb, const R60Unit* units, uint32_t nu,
              uint32_t uiV0, bool camValid, uint32_t playerId, int32_t px, int32_t py,
              uint32_t offX, uint32_t offY, uint32_t tick, uint64_t frame, uint32_t clear);

    /* Game thread. OUT-OF-GAME frame (no valid CAMERA): the engine goes idle
     * (idempotent), forgets N and N-1, counts the frame; the caller submits
     * the frame itself. */
    void sleepFrame();
    /* State: active = in-game (the replay thread submits), idle = out-of-game. */
    bool active() const { return active_; }

    /* Starts the replay thread (idempotent). Returns false if the thread could not start. */
    bool start();
    void stop();
    bool running() const { return running_; }

    enum { MAX_REPLAYS_PER_FRAME = 3 };

    /* One replay iteration (called by the thread; public for tests). */
    bool step(uint64_t nowUs);

    /* Line for the 10s window; returns the number of bytes written, resets
     * the window counters to zero. */
    int line(char* out, unsigned n, uint32_t gameFrames);

    /* World (Δ 16.16) -> screen (window px) conversion. */
    static inline void worldToScreen(int64_t dx, int64_t dy, float& sx, float& sy) {
        sx = (float)((double)(dx - dy) * (16.0 / 65536.0));
        sy = (float)((double)(dx + dy) * (8.0 / 65536.0));
    }

    R60Stats& stats() { return st_; }

private:
    enum { SLOTS = 4 };
    R60Frame slot_[SLOTS];
    int cur_ = -1, prev_ = -1;      /* indices of frames N and N−1 */
    volatile int lock_ = 0;
    volatile bool running_ = false, stopReq_ = false;
    volatile bool active_ = false;  /* in-game: set by push(), cleared by sleepFrame() */
    uint64_t capFrame_ = ~0ull; unsigned capCount_ = 0;   /* bound: counted frame and its replays (under lock_) */
    std::vector<Vtx> out_;
    std::vector<R60Unit> prevUnits_; uint64_t prevUnitsFrame_ = ~0ull;
    R60Stats st_;
    uint64_t lastSubmitFrame_ = ~0ull;
    void lock(); void unlock();
    int  freeSlot() const;
    uint64_t hashVerts(const Vtx* v, uint32_t n) const;
    bool threadMain();
    friend void r60_thread_entry(void*);
};

Replay60& r60();

} // namespace d2gr

/* sceGxm context lock shared between the game thread (pages, palettes,
 * drain) and the replay thread (submission). No-op until the replay thread
 * has started: the knob-free path takes no lock at all. */
extern "C" void r60_gxm_arm();      // arms the lock even without 60Hz replay (flush thread, D2_FLUSHFIL)
extern "C" void r60_gxm_lock();
extern "C" void r60_gxm_unlock();

#endif /* D2VITA_REPLAY60_H */
