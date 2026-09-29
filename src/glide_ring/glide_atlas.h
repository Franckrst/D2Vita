/* glide_atlas.h — HOST-side P8 TEXTURE ATLAS and BATCH BUILDER.
 *
 * WHAT THIS IS. The PORTABLE half of the "ring -> GPU" path: it takes texture
 * uploads and draw records from the ring and produces (a) packed 8-bit
 * pages, (b) a vertex buffer, (c) a list of same-state BATCHES. Nothing here
 * knows about sceGxm: the only platform link is a copy callback (`upload_cb`),
 * which is a no-op under qemu. Everything measurable here — cache hit rate,
 * evictions, verts and batches per frame — is therefore also measurable under
 * qemu.
 *
 * WHY AN ATLAS. `grTexSource` changes on almost every sprite, so a
 * same-state merge alone merges NOTHING. Most uploaded bytes carry a hash
 * that's already been seen, and the distinct-texture set is small enough to
 * fit entirely in memory: the same content gets re-uploaded endlessly. The
 * atlas turns this into a single mechanism that solves both problems: it
 * removes redundant copies, and it makes batches mergeable (two sprites from
 * the same page share their texture).
 *
 * COORDINATE CONVENTION. Glide's s,t are NOT texels of the bound texture,
 * they are normalized against the largest LOD's space:
 *      s in [0, S] with S = 256 >> max(0, -aspect)
 *      t in [0, T] with T = 256 >> max(0, +aspect)
 * The transform into atlas space is folded INTO THE VERTEX when the batch is
 * built — so there is no per-texture uniform, and a batch can span an entire
 * page.
 */
#ifndef D2VITA_GLIDE_ATLAS_H
#define D2VITA_GLIDE_ATLAS_H

#include <stdint.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <map>

namespace d2gr {

// ---- vertex handed to the GPU ----------------------------------------------
// 24 bytes. u,v are ALREADY atlas-page coordinates (0..1); pal is already the
// v coordinate into the palette texture: the shader does no atlas arithmetic.
struct Vtx { float x, y; float u, v; uint32_t argb; float pal; };

// ---- batch ------------------------------------------------------------------
struct Batch {
    uint32_t first;      // first index
    uint32_t count;      // index count
    int16_t  page;       // atlas page, -1 = untextured flat
    uint8_t  blend;      // 0=opaque 1=alpha 2=additive 3=multiplicative
    uint8_t  flags;      // bit0 = chromakey, bit1 = bilinear filtering,
                         // bit2 = color = texture x vertex (else flat constant),
                         // bit3 = alpha from the constant color
    uint32_t cst;        // constant color, already 0xAARRGGBB
    // Batch's PALETTE SLOT. Under shader-side palettization (default) it is
    // UNUSED: the palette index travels in the vertex (Vtx::pal) and a batch
    // can mix palettes. Under HARDWARE PALETTE (D2_GXMPAL=1) the palette
    // belongs to the texture DESCRIPTOR, so a batch can only carry one:
    // Builder::splitByPalette(true) then adds this field to the split key.
    // The field is ALWAYS filled in — only the split is optional — so the
    // cost of splitting can be measured under qemu.
    uint8_t  pal;
};

// ---- atlas cell -------------------------------------------------------------
struct Cell {
    uint64_t hash;
    uint32_t gen;        // generation: invalidates lookups to a reused cell
    int32_t  page;
    uint16_t x, y, w, h;
    uint64_t used;       // LRU timestamp (frame number)
    int32_t  prevLru, nextLru;
    // ⚡ NUMBER OF TMU ADDRESSES STILL POINTING HERE. This is what makes
    // eviction SAFE. The game keeps its own texture cache: once it believes a
    // texture is resident, it NEVER re-uploads it. Evicting a cell still
    // bound by a TMU address therefore makes that sprite disappear for good —
    // a silent failure, never a crash. This counter lets the atlas stay small
    // WITHOUT that risk: cells the game has itself dropped are evicted first.
    uint16_t ref;
    uint8_t  inLru;      // 1 = allocated (in the LRU list), 0 = free or dead
};

// Lookup stored by TMU address: the game binds textures by `startAddress`,
// not by content. The generation makes the lookup self-invalidating after
// eviction — without it, a reused cell would draw a DIFFERENT sprite,
// silently and without crashing.
struct TmuRef { int32_t cell; uint32_t gen; uint32_t bytes; };   // bytes: TMU footprint (w*h, P8)

typedef void (*UploadCb)(int page, int x, int y, int w, int h,
                         const uint8_t* src, int srcPitch);
typedef int  (*PageCb)(int page, int dim);   // returns 0 if the page could not be created
// IN-FLIGHT FRAME FENCE (asynchronous submission).
//   fence() returns the OLDEST frame still being read by the GPU. Any cell
//   with `used >= fence()` is currently being sampled: overwriting it produces
//   a wrong sprite, one frame in a hundred, with no diagnostic. UINT64_MAX
//   means nothing is in flight, which is the synchronous-mode value: the
//   eviction policy then reduces to plain LRU.
//   drain() forces in-flight frames to finish. It's called only as a last
//   resort, when the fence forbids the only possible eviction: a COUNTED wait
//   (evictWait()) is preferred over a wrong sprite.
typedef uint64_t (*FenceCb)();
typedef void     (*DrainCb)();

class Atlas {
public:
    // budgetBytes: page memory cap. pageDim: side length of a page.
    void init(uint32_t budgetBytes, uint32_t pageDim, UploadCb up, PageCb mk,
              FenceCb fence = 0, DrainCb drain = 0) {
        budget_ = budgetBytes; dim_ = pageDim; up_ = up; mk_ = mk;
        fence_ = fence; drain_ = drain; evictWait_ = 0; evictHeld_ = 0;
        maxPages_ = budget_ / (dim_ * dim_); if (!maxPages_) maxPages_ = 1;
        cells_.clear(); byHash_.clear(); byTmu_.clear(); classes_.clear();
        pageKey_.clear(); pageCells_.clear(); pageLive_.clear(); pageUsed_.clear(); deadCells_.clear();
        reclaimed_ = 0;
        pages_ = 0; lruHead_ = lruTail_ = -1;
        hits_ = miss_ = evict_ = copies_ = copyBytes_ = bindMiss_ = bindOk_ = 0;
        pagesFull_ = 0; evictLive_ = 0; overlapDrop_ = 0; failUnbind_ = 0;
    }
    bool ready() const { return dim_ != 0; }
    uint32_t pages() const { return pages_; }
    uint32_t maxPages() const { return maxPages_; }
    uint32_t dim() const { return dim_; }
    size_t   cellCount() const { return cells_.size(); }
    uint64_t hits() const { return hits_; }
    uint64_t miss() const { return miss_; }
    uint64_t evictions() const { return evict_; }
    uint64_t copies() const { return copies_; }
    uint64_t copyBytes() const { return copyBytes_; }
    uint64_t bindMiss() const { return bindMiss_; }
    uint64_t bindOk() const { return bindOk_; }
    uint64_t pagesFull() const { return pagesFull_; }
    // Evictions of cells STILL BOUND by a TMU address: each one is a sprite
    // that disappears for good. Must stay at ZERO; if it rises, the atlas is
    // too small for the game's own texture cache.
    uint64_t evictLive() const { return evictLive_; }
    // Evictions REFUSED because the cell was being read by an in-flight
    // frame: each one cost a drain wait. Should stay at zero in steady state;
    // if it rises, the atlas is too small FOR THE PIPELINE DEPTH.
    uint64_t evictHeld() const { return evictHeld_; }
    // TMU MEMORY MODEL. An upload writes [addr, addr+w*h) of TMU memory: every
    // older texture overlapping that range is overwritten on real hardware,
    // so its binding is dropped here (overlapDrop) and its cell stops counting
    // as live. Without this, bindings whose address the game never reused
    // EXACTLY leaked their reference forever, and eviction mistook those
    // cells for live ones.
    uint64_t overlapDrops() const { return overlapDrop_; }
    // Uploads that found no room: their address is left UNBOUND (drawn
    // without texture), never still bound to the texture it held before.
    uint64_t failUnbinds() const { return failUnbind_; }
    // Pages taken back from one cell size and re-cut for another: all their
    // textures unused by the game and out of the GPU's reach. Without this a
    // page belonged to its size class forever, and a new size could find no
    // room while the atlas was full of dead textures of other sizes.
    uint64_t reclaimedPages() const { return reclaimed_; }
    uint64_t evictWait() const { return evictWait_; }

    // A game upload. `src` points to w*h bytes of palette indices.
    // Returns the cell index, or -1 if there's no room.
    int32_t upload(uint64_t hash, uint32_t tmuAddr, uint32_t w, uint32_t h,
                   const uint8_t* src, uint64_t frame) {
        if (!dim_ || !w || !h || w > dim_ || h > dim_) return -1;
        // The TMU range now holds THIS content, whatever happens next: drop
        // every binding it overwrites, the address's own previous one
        // included. If no cell can be found below, the address stays
        // unbound — an untextured draw, never the previous sprite (which is
        // what made Act V's Death Maulers blink once the atlas was full).
        unbindRange(tmuAddr, w * h);
        auto it = byHash_.find(hash);
        if (it != byHash_.end()) {
            const int32_t ci = it->second;
            Cell& c = cells_[(size_t)ci];
            if (c.w == w && c.h == h) {           // same content, same shape: NOTHING to copy
                ++hits_;
                touch(ci, frame);
                bind(tmuAddr, ci);
                return ci;
            }
            // Hash collision on different shapes: don't trust it, fall back
            // to the slow path rather than draw garbage.
            byHash_.erase(it);
        }
        ++miss_;
        const int32_t ci = alloc(w, h, frame);
        if (ci < 0) { ++failUnbind_; return -1; }
        Cell& c = cells_[(size_t)ci];
        c.hash = hash;
        byHash_[hash] = ci;
        bind(tmuAddr, ci);
        if (up_) up_(c.page, c.x, c.y, w, h, src, (int)w);
        ++copies_; copyBytes_ += (uint64_t)w * h;
        return ci;
    }

    // Lookup by TMU address (grTexSource). -1 = the game is binding a texture
    // the atlas doesn't know (uploaded before our init, or evicted).
    int32_t resolve(uint32_t tmuAddr) {
        auto it = byTmu_.find(tmuAddr);
        if (it == byTmu_.end()) { ++bindMiss_; return -1; }
        const int32_t ci = it->second.cell;
        if (ci < 0 || (size_t)ci >= cells_.size() || cells_[(size_t)ci].gen != it->second.gen) {
            ++bindMiss_; return -1; }
        ++bindOk_; return ci;
    }
    const Cell& cell(int32_t i) const { return cells_[(size_t)i]; }
    void touchCell(int32_t i, uint64_t frame) { touch(i, frame); }
    // Replays the resolve() counter WITHOUT redoing the lookup: for the ring
    // walk's batch-state cache (rt_boot.cpp, gx_sync_for_draw), which knows
    // the answer hasn't changed and wants "bind-miss=%" to stay the same
    // number.
    void noteResolve(bool ok) { if (ok) ++bindOk_; else ++bindMiss_; }

private:
    // A CLASS = one cell size. Each page belongs to a class and is cut into a
    // regular grid: allocation is O(1) and there is no fragmentation, which
    // makes LRU eviction trivial (a freed cell is immediately reusable by the
    // same class).
    struct Klass { uint32_t w, h; std::vector<int32_t> freeCells; };

    static uint32_t keyOf(uint32_t w, uint32_t h) { return (w << 16) | h; }

    // Sets the TMU address -> cell lookup and maintains the reference count.
    void bind(uint32_t tmuAddr, int32_t ci) {
        auto it = byTmu_.find(tmuAddr);
        if (it != byTmu_.end()) {
            const int32_t old = it->second.cell;
            if (old >= 0 && (size_t)old < cells_.size() && cells_[(size_t)old].gen == it->second.gen) {
                if (old == ci) { return; }                       // already bound here
                refDec(old);
            }
        }
        refInc(ci);
        byTmu_[tmuAddr] = TmuRef{ ci, cells_[(size_t)ci].gen,
                                  (uint32_t)cells_[(size_t)ci].w * cells_[(size_t)ci].h };
    }

    // Reference count, mirrored per page (pageLive_: cells with ref > 0), so
    // that finding a page with no live cell costs O(pages), not O(cells).
    void refInc(int32_t ci) {
        Cell& c = cells_[(size_t)ci];
        if (c.ref == 0 && c.page >= 0) ++pageLive_[(size_t)c.page];
        if (c.ref < 0xFFFFu) ++c.ref;
    }
    void refDec(int32_t ci) {
        Cell& c = cells_[(size_t)ci];
        if (!c.ref) return;
        if (--c.ref == 0 && c.page >= 0) --pageLive_[(size_t)c.page];
    }
    void refClear(int32_t ci) {
        Cell& c = cells_[(size_t)ci];
        if (c.ref && c.page >= 0) --pageLive_[(size_t)c.page];
        c.ref = 0;
    }

    // Drops every binding whose TMU range [a, a+bytes) overlaps [addr,
    // addr+bytes). Keys carry the TMU in their top bits (gx_tmu_key), and a
    // texture is at most 256x256 = 64 KiB, so only keys in
    // [addr-64K, addr+bytes) of the SAME TMU can overlap.
    void unbindRange(uint32_t addr, uint32_t bytes) {
        const uint32_t tmuBase = addr & 0xF0000000u;
        const uint32_t lo = (addr - tmuBase >= 0x10000u) ? addr - 0x10000u : tmuBase;
        const uint32_t hi = addr + bytes;
        for (auto it = byTmu_.lower_bound(lo); it != byTmu_.end() && it->first < hi; ) {
            const uint32_t a = it->first, end = a + it->second.bytes;
            if ((a & 0xF0000000u) != tmuBase || end <= addr) { ++it; continue; }
            const int32_t old = it->second.cell;
            if (old >= 0 && (size_t)old < cells_.size() && cells_[(size_t)old].gen == it->second.gen)
                refDec(old);
            if (a != addr) ++overlapDrop_;
            it = byTmu_.erase(it);
        }
    }

    // Order of preference, so that a texture the game still holds is only
    // ever sacrificed when nothing else is possible:
    //   1. a free cell of this size;   2. a new page;
    //   3. an UNUSED cell of this size (ref == 0), least recently used;
    //   4. an entire page of ANOTHER size whose cells are all unused and
    //      out of the GPU's reach, re-cut for this size;
    //   5. last resort: a still-bound cell of this size (counted, evictLive).
    int32_t alloc(uint32_t w, uint32_t h, uint64_t frame) {
        Klass& k = klassFor(w, h);
        if (k.freeCells.empty()) {
            if (pages_ < maxPages_ && newPage(k)) { /* ok */ }
            else if (evictOne(w, h, false)) { /* ok */ }
            else if (reclaimPage(k)) { /* ok */ }
            else if (!evictOne(w, h, true)) { ++pagesFull_; return -1; }
        }
        if (k.freeCells.empty()) { ++pagesFull_; return -1; }
        const int32_t ci = k.freeCells.back(); k.freeCells.pop_back();
        Cell& c = cells_[(size_t)ci];
        ++c.gen;                       // any reuse invalidates old lookups
        c.used = frame;
        if ((size_t)c.page < pageUsed_.size() && pageUsed_[(size_t)c.page] < frame) pageUsed_[(size_t)c.page] = frame;
        lruPush(ci);
        return ci;
    }

    // Step 4 of alloc(): the least recently used page of another size with no
    // live cell and nothing the GPU may still be reading, re-cut for `k`.
    bool reclaimPage(Klass& k) {
        const uint32_t key = keyOf(k.w, k.h);
        const uint64_t fence = fence_ ? fence_() : ~(uint64_t)0;
        int32_t best = -1;
        for (uint32_t p = 0; p < pages_; ++p) {
            if (pageKey_[p] == key || pageLive_[p] || pageUsed_[p] >= fence) continue;
            if (best < 0 || pageUsed_[p] < pageUsed_[(size_t)best]) best = (int32_t)p;
        }
        if (best < 0) return false;
        const uint32_t p = (uint32_t)best;
        // Detach every cell of the page from its old size class.
        Klass& old = classes_[pageKey_[p]];
        std::vector<int32_t> keep; keep.reserve(old.freeCells.size());
        for (int32_t ci : old.freeCells) if (cells_[(size_t)ci].page != (int32_t)p) keep.push_back(ci);
        old.freeCells.swap(keep);
        std::vector<int32_t> olds; olds.swap(pageCells_[p]);
        for (int32_t ci : olds) {
            Cell& c = cells_[(size_t)ci];
            if (c.inLru) lruRemove(ci);
            auto h = byHash_.find(c.hash);
            if (h != byHash_.end() && h->second == ci) byHash_.erase(h);
            c.hash = 0; refClear(ci);
            ++c.gen;                               // stale TMU lookups now miss
        }
        // Re-cut, reusing the page's own cell slots first, then dead ones.
        size_t reuse = 0;
        const uint32_t nx = dim_ / k.w, ny = dim_ / k.h;
        for (uint32_t gy = 0; gy < ny; ++gy)
            for (uint32_t gx = 0; gx < nx; ++gx) {
                int32_t ci;
                if (reuse < olds.size()) ci = olds[reuse++];
                else if (!deadCells_.empty()) { ci = deadCells_.back(); deadCells_.pop_back(); }
                else { Cell z; memset(&z, 0, sizeof z); z.gen = 1; cells_.push_back(z); ci = (int32_t)(cells_.size() - 1); }
                setupCell(ci, (int32_t)p, gx * k.w, gy * k.h, k.w, k.h);
                pageCells_[p].push_back(ci);
                k.freeCells.push_back(ci);
            }
        for (; reuse < olds.size(); ++reuse) {     // slots the new cut doesn't need
            Cell& c = cells_[(size_t)olds[reuse]];
            c.page = -1; c.w = c.h = 0;
            deadCells_.push_back(olds[reuse]);
        }
        pageKey_[p] = key; pageLive_[p] = 0; pageUsed_[p] = 0;
        ++reclaimed_;
        return true;
    }
    void setupCell(int32_t ci, int32_t page, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        Cell& c = cells_[(size_t)ci];
        const uint32_t gen = c.gen;                // keep increasing across reuse
        memset(&c, 0, sizeof c);
        c.gen = gen ? gen : 1; c.page = page;
        c.x = (uint16_t)x; c.y = (uint16_t)y; c.w = (uint16_t)w; c.h = (uint16_t)h;
        c.prevLru = c.nextLru = -1;
    }

    Klass& klassFor(uint32_t w, uint32_t h) {
        const uint32_t key = keyOf(w, h);
        auto it = classes_.find(key);
        if (it != classes_.end()) return it->second;
        Klass k; k.w = w; k.h = h;
        return classes_.emplace(key, k).first->second;
    }

    bool newPage(Klass& k) {
        const int page = (int)pages_;
        if (mk_ && !mk_(page, (int)dim_)) return false;
        ++pages_;
        pageKey_.push_back(keyOf(k.w, k.h)); pageLive_.push_back(0); pageUsed_.push_back(0);
        pageCells_.emplace_back();
        const uint32_t nx = dim_ / k.w, ny = dim_ / k.h;
        for (uint32_t gy = 0; gy < ny; ++gy)
            for (uint32_t gx = 0; gx < nx; ++gx) {
                int32_t ci;
                if (!deadCells_.empty()) { ci = deadCells_.back(); deadCells_.pop_back(); }
                else { Cell z; memset(&z, 0, sizeof z); z.gen = 1; cells_.push_back(z); ci = (int32_t)(cells_.size() - 1); }
                setupCell(ci, page, gx * k.w, gy * k.h, k.w, k.h);
                pageCells_[(size_t)page].push_back(ci);
                k.freeCells.push_back(ci);
            }
        return true;
    }

    // Eviction: the LEAST RECENTLY USED cell of the RIGHT class, PREFERRING
    // ones the game has already dropped (ref == 0). A cell that's still bound
    // is only taken as a last resort, and that case is COUNTED separately:
    // it's the only place in the GPU path that can make a sprite disappear
    // without any diagnostic.
    bool evictOne(uint32_t w, uint32_t h, bool allowLive) {
        // Two attempts: the first under the in-flight fence; if it finds
        // NOTHING, drain the queue (a counted wait) and retry with a
        // refreshed fence. Overwriting an in-flight cell would be silent —
        // not an option.
        for (int round = 0; round < 2; ++round) {
            const uint64_t fence = fence_ ? fence_() : ~(uint64_t)0;
            bool held = false;
            for (int pass = 0; pass < (allowLive ? 2 : 1); ++pass) {
                for (int32_t ci = lruHead_; ci >= 0; ci = cells_[(size_t)ci].nextLru) {
                    Cell& c = cells_[(size_t)ci];
                    if (c.w != w || c.h != h) continue;
                    if (c.used >= fence) { held = true; continue; }   // being read by an in-flight frame
                    if (pass == 0 && c.ref) continue;
                    if (pass == 1) ++evictLive_;
                    lruRemove(ci);
                    byHash_.erase(c.hash);
                    c.hash = 0; refClear(ci);
                    klassFor(w, h).freeCells.push_back(ci);
                    ++evict_;
                    return true;
                }
            }
            if (!held || !drain_ || round == 1) break;
            ++evictHeld_; ++evictWait_;
            drain_();                  // wait for in-flight frames to finish, THEN retry
        }
        return false;
    }

    void touch(int32_t ci, uint64_t frame) {
        Cell& c = cells_[(size_t)ci];
        if (c.used == frame) return;         // at most one LRU move per frame
        c.used = frame;
        if ((size_t)c.page < pageUsed_.size() && pageUsed_[(size_t)c.page] < frame) pageUsed_[(size_t)c.page] = frame;
        lruRemove(ci); lruPush(ci);
    }
    void lruPush(int32_t ci) {               // at the TAIL = most recently used
        Cell& c = cells_[(size_t)ci];
        c.prevLru = lruTail_; c.nextLru = -1; c.inLru = 1;
        if (lruTail_ >= 0) cells_[(size_t)lruTail_].nextLru = ci; else lruHead_ = ci;
        lruTail_ = ci;
    }
    void lruRemove(int32_t ci) {
        Cell& c = cells_[(size_t)ci];
        if (c.prevLru >= 0) cells_[(size_t)c.prevLru].nextLru = c.nextLru; else if (lruHead_ == ci) lruHead_ = c.nextLru;
        if (c.nextLru >= 0) cells_[(size_t)c.nextLru].prevLru = c.prevLru; else if (lruTail_ == ci) lruTail_ = c.prevLru;
        c.prevLru = c.nextLru = -1; c.inLru = 0;
    }

    std::vector<Cell> cells_;
    // PER-PAGE tables (index = page): its size class key, its cells, how many
    // of them are live (ref > 0), and the last frame any of them was used.
    std::vector<uint32_t> pageKey_;
    std::vector<std::vector<int32_t>> pageCells_;
    std::vector<uint32_t> pageLive_;
    std::vector<uint64_t> pageUsed_;
    std::vector<int32_t>  deadCells_;   // cell slots freed by a re-cut, reused first
    uint64_t reclaimed_ = 0;
    std::unordered_map<uint64_t, int32_t> byHash_;
    std::map<uint32_t, TmuRef>            byTmu_;   // ordered: overlap queries (unbindRange)
    std::unordered_map<uint32_t, Klass>   classes_;
    uint32_t budget_ = 0, dim_ = 0, maxPages_ = 0, pages_ = 0;
    int32_t  lruHead_ = -1, lruTail_ = -1;
    UploadCb up_ = nullptr;
    PageCb   mk_ = nullptr;
    FenceCb  fence_ = nullptr;
    DrainCb  drain_ = nullptr;
    uint64_t hits_ = 0, miss_ = 0, evict_ = 0, copies_ = 0, copyBytes_ = 0;
    uint64_t bindMiss_ = 0, bindOk_ = 0, pagesFull_ = 0, evictLive_ = 0;
    uint64_t evictHeld_ = 0, evictWait_ = 0;
    uint64_t overlapDrop_ = 0, failUnbind_ = 0;
};

// PER-DRAW PRECOMPUTE. Everything that depends only on the current draw is
// computed once here instead of per-vertex.
// EXACTNESS: sNorm and tNorm are always 256>>k (k = 0..3), powers of two; for
// an IEEE float, s * (1/2^k) and s / 2^k are the SAME number bit-for-bit
// (exact rescale of the exponent, denormals aside — s is in [0, 256]). The
// ORDER of the remaining operations is kept identical:
// (cx + (s*invS)*cw) * invDim  ==  ((float)c.x + (s/sNorm)*(float)c.w) * invDim.
struct VtxPre {
    float cx = 0, cy = 0, cw = 0, ch = 0;   // cell, already in float
    float invS = 1.0f / 256.0f, invT = 1.0f / 256.0f;
    float invDim = 0, palv = 0;
    bool  tex = false;
};

// ---------------------------------------------------------------------------
// BATCH BUILDER
//
// Current Glide state plus vertex accumulation. A batch closes as soon as the
// state signature changes; DRAW ORDER IS PRESERVED (D2 draws painter-style,
// with no depth buffer: reordering would change the image).
// ---------------------------------------------------------------------------
struct BuildState {
    int32_t  cell = -1;        // bound atlas cell
    uint32_t sNorm = 256, tNorm = 256;   // S,T from the coordinate convention
    uint8_t  blend = 0;
    uint8_t  flags = 0;
    uint32_t cst = 0xFF000000u;
    float    palv = 0.5f / 16.0f;
    uint8_t  pal = 0;          // the SAME slot, as a number (hardware palette)
};

class Builder {
public:
    void init(uint32_t maxVerts, uint32_t maxIdx, uint32_t maxBatches) {
        maxV_ = maxVerts; maxI_ = maxIdx; maxB_ = maxBatches;
        v_.resize(maxV_); i_.resize(maxI_); b_.resize(maxB_);
        reset();
    }
    void reset() { nv_ = 0; ni_ = 0; nb_ = 0; open_ = false; dropped_ = 0; }
    // SPLITTING BATCHES BY PALETTE. Default is FALSE: the palette slot isn't
    // part of the split key, so batches merge as if it didn't exist. True
    // only under hardware palette mode (D2_GXMPAL=1), where the palette is
    // carried by the texture descriptor rather than the vertex. The overhead
    // this adds is visible under qemu in "batches/frame".
    void splitByPalette(bool on) { splitPal_ = on; }
    bool splitsByPalette() const { return splitPal_; }
    uint32_t verts() const { return nv_; }
    uint32_t indices() const { return ni_; }
    uint32_t batches() const { return nb_; }
    uint32_t dropped() const { return dropped_; }
    const Vtx*   vdata() const { return v_.data(); }
    const uint16_t* idata() const { return i_.data(); }
    const Batch* bdata() const { return b_.data(); }

    // Opens (or extends) a batch for the current state.
    void begin(const BuildState& st, const Atlas& at) {
        const int16_t page = st.cell >= 0 ? (int16_t)at.cell(st.cell).page : (int16_t)-1;
        if (open_ && b_[nb_-1].page == page && b_[nb_-1].blend == st.blend &&
            b_[nb_-1].flags == st.flags && b_[nb_-1].cst == st.cst &&
            (!splitPal_ || b_[nb_-1].pal == st.pal)) return;
        if (nb_ >= maxB_) { open_ = false; return; }
        Batch nb; nb.first = ni_; nb.count = 0; nb.page = page;
        nb.blend = st.blend; nb.flags = st.flags; nb.cst = st.cst; nb.pal = st.pal;
        b_[nb_++] = nb; open_ = true;
    }

    // Precompute for the current draw (see VtxPre).
    void prep(VtxPre& p, const BuildState& st, const Atlas& at, float invDim) const {
        p.tex = st.cell >= 0;
        if (p.tex) {
            const Cell& c = at.cell(st.cell);
            p.cx = (float)c.x; p.cy = (float)c.y; p.cw = (float)c.w; p.ch = (float)c.h;
        }
        p.invS = 1.0f / (float)st.sNorm; p.invT = 1.0f / (float)st.tNorm;
        p.invDim = invDim; p.palv = st.palv;
    }
    // Adds a transformed vertex. Returns its index, or 0xFFFF if full.
    // Division-free: same operations, same order, same bits (x * 1.0f and
    // y * 1.0f are the IEEE identity).
    uint16_t vertexPre(const VtxPre& p, float x, float y, uint32_t argb, float s, float t) {
        if (nv_ >= maxV_ || nv_ >= 0xFFFFu) { ++dropped_; return 0xFFFFu; }
        Vtx& o = v_[nv_];
        o.x = x; o.y = y;
        if (p.tex) {
            o.u = (p.cx + (s * p.invS) * p.cw) * p.invDim;
            o.v = (p.cy + (t * p.invT) * p.ch) * p.invDim;
        } else { o.u = 0.0f; o.v = 0.0f; }
        o.argb = argb; o.pal = p.palv;
        return (uint16_t)nv_++;
    }
    void tri(uint16_t a, uint16_t b, uint16_t c) {
        if (!open_ || ni_ + 3 > maxI_ || a == 0xFFFFu || b == 0xFFFFu || c == 0xFFFFu) { ++dropped_; return; }
        i_[ni_++] = a; i_[ni_++] = b; i_[ni_++] = c;
        b_[nb_-1].count += 3;
    }
private:
    std::vector<Vtx> v_; std::vector<uint16_t> i_; std::vector<Batch> b_;
    uint32_t maxV_ = 0, maxI_ = 0, maxB_ = 0, nv_ = 0, ni_ = 0, nb_ = 0, dropped_ = 0;
    bool open_ = false, splitPal_ = false;
};

} // namespace d2gr
#endif /* D2VITA_GLIDE_ATLAS_H */
