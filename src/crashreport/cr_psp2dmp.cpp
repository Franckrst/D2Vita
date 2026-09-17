// src/crashreport/cr_psp2dmp.cpp — streaming reader for PS Vita core dumps.
//
// Layouts read here were established empirically from real dumps and are
// shared with tools/tests/gen_fake_psp2dmp.py:
//   ELF32 LE core; PT_NOTE segments first (one note each, name field 16
//   bytes), then PT_LOAD in increasing file offset.
//   note walk            tools/autopsie_psp2dmp.py:36-43 (later names win)
//   THREAD_REG_INFO      tools/autopsie_psp2dmp.py:55-64: skip 8, entries of
//                        `size` bytes = size, tid, r0..r12, sp, lr, pc, cpsr
//   THREAD_INFO          u32, u32 count, entries (+0x00 size, +0x04 uid,
//                        +0x08 name[32], +0x30 status, +0x74 stop reason,
//                        +0x9c pc)
//   MODULE_INFO          u32, u32 count, modules (+0x04 uid, +0x24 name[32],
//                        +0x4c segment count, 0x14-byte segments at +0x50
//                        {?, attr, vaddr, memsz, align}, then 0x10 bytes)
//   APP_INFO             title id at +0x08
//   SYSTEM_INFO          firmware at +0x08 (0x03650011 = "3.65")
//   guest registers      box86 map r8 = ESP, r9 = EBP (autopsie_psp2dmp.py:21)
//   EBP chain            autopsie_psp2dmp.py:102-111
#include "crashreport/cr_psp2dmp.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace d2cr {

namespace {

constexpr size_t kInChunk = 64 * 1024;
constexpr size_t kOutChunk = 64 * 1024;
constexpr uint32_t kMaxPhnum = 4096;
constexpr size_t kPrefixCap = 52 + kMaxPhnum * 32 + 128 * 1024;
constexpr size_t kNotesCap = 256 * 1024;
constexpr size_t kCaptureCap = 256 * 1024;
constexpr size_t kMaxThreads = 256;
constexpr size_t kMaxModules = 256;
constexpr uint32_t kPcWindow = 0x20;
constexpr char kStampMarker[] = "D2VSTAMP1 ";
constexpr size_t kStampMax = 200;

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

std::string cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string((const char*)p, n);
}

voidpf z_alloc(voidpf, uInt items, uInt size) {
    if (size && items > (uInt)-1 / size) return Z_NULL;
    return ::operator new((size_t)items * size, std::nothrow);
}
void z_free(voidpf, voidpf p) { ::operator delete(p); }

struct Seg { uint32_t type = 0, off = 0, va = 0, fsz = 0; };

struct NoteSeg {
    uint32_t off = 0, fsz = 0;
    bool leading = false;          // located before the first PT_LOAD
    std::vector<uint8_t> bytes;    // empty when over the cap
    uint64_t received = 0;
};

struct Capture {
    size_t load = 0;               // index in loads_
    uint32_t va_lo = 0;
    uint64_t off_lo = 0;
    std::vector<uint8_t> bytes;
};

struct ModSeg { uint32_t va = 0, size = 0; };
struct Module { uint32_t uid = 0; std::string name; std::vector<ModSeg> segs; };

enum class ReadStatus { Ok, NotInDump, NotCaptured };

class Reader {
 public:
    Reader(const SessionRecord& s, const SecretPatterns& p, const DumpReadOptions& o)
        : s_(s), opt_(o), scanner_(p), out_(new uint8_t[kOutChunk]) {
        std::memset(&zs_, 0, sizeof zs_);
        zs_.zalloc = z_alloc;
        zs_.zfree = z_free;
    }
    ~Reader() { if (inflating_) inflateEnd(&zs_); }

    bool on_input(const uint8_t* in, size_t n);
    void finish(bool read_ok);
    DumpFacts f;

 private:
    void on_output(const uint8_t* data, size_t n);
    void deliver(uint64_t pos, const uint8_t* data, size_t n);
    void parse_header();
    void parse_phdrs(const uint8_t* table);
    void parse_notes();
    void plan_captures(uint64_t from);
    void want_trailing(uint64_t pos, const uint8_t* data, size_t n);
    void stamp_scan_notes(uint64_t pos, const uint8_t* data, size_t n);
    void stamp_feed(const uint8_t* data, size_t n);
    void stamp_reset();
    HostAddr classify(uint32_t a) const;
    ReadStatus read_va(uint64_t va, uint32_t n, uint8_t* out) const;
    void walk_chain();

    const SessionRecord& s_;
    DumpReadOptions opt_;
    StreamScanner scanner_;
    std::unique_ptr<uint8_t[]> out_;
    z_stream zs_;
    enum class Mode { Unknown, Gzip, Raw } mode_ = Mode::Unknown;
    bool inflating_ = false, stream_end_ = false, stop_ = false, intentional_stop_ = false;
    uint64_t pos_ = 0;

    uint8_t hdr_[52];
    bool header_ok_ = false, phdrs_ok_ = false;
    bool lead_parsed_ = false, trail_wanted_ = false, trail_parsed_ = false;
    uint32_t phoff_ = 0, phnum_ = 0;
    std::vector<uint8_t> prefix_;        // stream start, until the program headers are parsed
    std::vector<Seg> loads_;
    std::vector<size_t> load_order_;     // loads_ sorted by (va, fsz, off)
    std::vector<NoteSeg> notes_;
    uint64_t lead_end_ = 0, trail_end_ = 0, data_end_ = 0;
    size_t note_bytes_ = 0;
    std::vector<Module> modules_;
    std::vector<Capture> caps_;

    size_t stamp_state_ = 0;
    bool stamp_capturing_ = false;
    std::string stamp_buf_;
    uint64_t stamp_next_ = 0;            // stream offset right after the last byte scanned
};

bool Reader::on_input(const uint8_t* in, size_t n) {
    if (stop_) return false;
    if (mode_ == Mode::Unknown) {
        if (n >= 2 && in[0] == 0x1f && in[1] == 0x8b) {
            mode_ = Mode::Gzip;
            if (inflateInit2(&zs_, 16 + MAX_WBITS) != Z_OK) {
                f.error = "zlib init failed";
                stop_ = true;
                return false;
            }
            inflating_ = true;
        } else {
            mode_ = Mode::Raw;
        }
    }
    if (mode_ == Mode::Raw) {
        on_output(in, n);
        return !stop_;
    }
    if (stream_end_) return true;          // trailing bytes after the gzip member
    zs_.next_in = (Bytef*)in;
    zs_.avail_in = (uInt)n;
    for (;;) {
        zs_.next_out = out_.get();
        zs_.avail_out = (uInt)kOutChunk;
        const int r = inflate(&zs_, Z_NO_FLUSH);
        const size_t produced = kOutChunk - zs_.avail_out;
        if (produced) on_output(out_.get(), produced);
        if (stop_) break;
        if (r == Z_STREAM_END) { stream_end_ = true; break; }
        if (r == Z_BUF_ERROR) break;
        if (r != Z_OK) {
            f.error = "gzip data error";
            stop_ = true;
            break;
        }
        if (zs_.avail_in == 0 && zs_.avail_out != 0) break;
    }
    return !stop_;
}

// Copies the part of [pos, pos+n) that overlaps [lo, lo+len) into dst[..].
static void overlap_copy(uint64_t pos, const uint8_t* data, size_t n, uint64_t lo, uint64_t len, uint8_t* dst) {
    const uint64_t a = std::max(pos, lo), b = std::min(pos + n, lo + len);
    if (a < b) std::memcpy(dst + (a - lo), data + (a - pos), (size_t)(b - a));
}

void Reader::on_output(const uint8_t* data, size_t n) {
    const uint64_t pos = pos_;
    pos_ += n;
    f.decompressed_bytes = pos_;
    if (!opt_.notes_only) scanner_.feed(data, n);
    if (phdrs_ok_) { deliver(pos, data, n); return; }

    // Until the program headers are known, keep the stream prefix (bounded)
    // and replay it once they are: nothing before them is lost.
    if (prefix_.size() + n > kPrefixCap) {
        f.error = "program headers not found near the start of the dump";
        stop_ = true;
        return;
    }
    prefix_.insert(prefix_.end(), data, data + n);
    if (!header_ok_) {
        if (prefix_.size() < sizeof hdr_) return;
        std::memcpy(hdr_, prefix_.data(), sizeof hdr_);
        parse_header();
        if (stop_) return;
    }
    if (prefix_.size() < (uint64_t)phoff_ + (uint64_t)phnum_ * 32) return;
    parse_phdrs(prefix_.data() + phoff_);
    std::vector<uint8_t> replay;
    replay.swap(prefix_);
    deliver(0, replay.data(), replay.size());
}

void Reader::deliver(uint64_t pos, const uint8_t* data, size_t n) {
    stamp_scan_notes(pos, data, n);
    for (NoteSeg& ns : notes_) {
        if (ns.bytes.empty()) continue;
        const uint64_t a = std::max(pos, (uint64_t)ns.off), b = std::min(pos + n, (uint64_t)ns.off + ns.fsz);
        if (a < b) {
            std::memcpy(ns.bytes.data() + (a - ns.off), data + (a - pos), (size_t)(b - a));
            ns.received = std::max(ns.received, b - ns.off);
        }
    }
    // Real dumps have two note groups: thread/module/application notes before
    // the memory, and MEM_BLK_INFO ... SUMMARY_INFO (with a 4 MiB SYSTEM_INFO2)
    // after it. Captures are planned from the first group, while the memory
    // is still ahead; the second group is only kept when the first one had
    // no thread notes.
    if (!lead_parsed_ && pos + n >= lead_end_) {
        lead_parsed_ = true;
        parse_notes();
        if (f.fault_index >= 0) {
            plan_captures(pos);
            if (opt_.notes_only) {
                stop_ = true;
                intentional_stop_ = true;
                return;
            }
        } else {
            want_trailing(pos, data, n);
        }
    }
    if (trail_wanted_ && !trail_parsed_ && pos + n >= trail_end_) {
        trail_parsed_ = true;
        parse_notes();
        plan_captures(pos);
        if (opt_.notes_only) {
            stop_ = true;
            intentional_stop_ = true;
            return;
        }
    }
    for (Capture& c : caps_) overlap_copy(pos, data, n, c.off_lo, c.bytes.size(), c.bytes.data());
}

void Reader::want_trailing(uint64_t pos, const uint8_t* data, size_t n) {
    trail_wanted_ = true;
    for (NoteSeg& ns : notes_) {
        if (ns.leading || !ns.bytes.empty()) continue;
        if ((uint64_t)ns.off < pos) { f.notes_truncated = true; continue; }   // already streamed by
        if (note_bytes_ + ns.fsz > kNotesCap) { f.notes_truncated = true; continue; }
        ns.bytes.assign(ns.fsz, 0);
        note_bytes_ += ns.fsz;
        overlap_copy(pos, data, n, ns.off, ns.fsz, ns.bytes.data());
        const uint64_t b = std::min(pos + n, (uint64_t)ns.off + ns.fsz);
        if (b > ns.off) ns.received = b - ns.off;
    }
}

void Reader::parse_header() {
    static const uint8_t kIdent[6] = {0x7f, 'E', 'L', 'F', 1, 1};   // ELFCLASS32, little endian
    if (std::memcmp(hdr_, kIdent, sizeof kIdent) != 0) {
        f.error = "not an ELF core (nor a gzip of one)";
        stop_ = true;
        return;
    }
    phoff_ = rd32(hdr_ + 0x1c);
    const uint16_t phentsize = rd16(hdr_ + 0x2a);
    phnum_ = rd16(hdr_ + 0x2c);
    if (phentsize != 32 || phnum_ == 0 || phnum_ > kMaxPhnum || phoff_ < sizeof hdr_) {
        f.error = "unsupported ELF program header table";
        stop_ = true;
        return;
    }
    header_ok_ = true;
}

void Reader::parse_phdrs(const uint8_t* table) {
    std::vector<Seg> segs(phnum_);
    uint64_t first_load = ~0ull;
    for (uint32_t i = 0; i < phnum_; ++i) {
        const uint8_t* p = table + (size_t)i * 32;
        Seg& sg = segs[i];
        sg.type = rd32(p);
        sg.off = rd32(p + 4);
        sg.va = rd32(p + 8);
        sg.fsz = rd32(p + 16);
        data_end_ = std::max(data_end_, (uint64_t)sg.off + sg.fsz);
        if (sg.type == 1) first_load = std::min<uint64_t>(first_load, sg.off);
    }
    for (const Seg& sg : segs) {
        if (sg.type == 4) {
            NoteSeg ns;
            ns.off = sg.off;
            ns.fsz = sg.fsz;
            ns.leading = sg.off < first_load;
            if (ns.leading) {
                if (note_bytes_ + sg.fsz <= kNotesCap) {
                    ns.bytes.assign(sg.fsz, 0);
                    note_bytes_ += sg.fsz;
                } else {
                    f.notes_truncated = true;
                }
                lead_end_ = std::max(lead_end_, (uint64_t)sg.off + sg.fsz);
            } else {
                trail_end_ = std::max(trail_end_, (uint64_t)sg.off + sg.fsz);
            }
            notes_.push_back(std::move(ns));
        } else if (sg.type == 1) {
            loads_.push_back(sg);
        }
    }
    load_order_.resize(loads_.size());
    for (size_t i = 0; i < loads_.size(); ++i) load_order_[i] = i;
    std::stable_sort(load_order_.begin(), load_order_.end(), [this](size_t a, size_t b) {
        const Seg& x = loads_[a];
        const Seg& y = loads_[b];
        if (x.va != y.va) return x.va < y.va;
        if (x.fsz != y.fsz) return x.fsz < y.fsz;
        return x.off < y.off;
    });
    phdrs_ok_ = true;
}

void Reader::parse_notes() {
    // May run twice (first note group, then both groups): start from scratch.
    f.app_title.clear();
    f.fw.clear();
    f.threads.clear();
    f.fault_index = -1;
    modules_.clear();
    // The five notes read below, by name; a later note of the same name wins.
    enum { kApp, kSystem, kThread, kThreadReg, kModule, kWanted };
    static const char* const kNames[kWanted] = {"APP_INFO", "SYSTEM_INFO", "THREAD_INFO", "THREAD_REG_INFO", "MODULE_INFO"};
    struct View { const uint8_t* p; uint32_t n; };
    View views[kWanted] = {{nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}, {nullptr, 0}};
    for (const NoteSeg& ns : notes_) {
        if (ns.bytes.empty()) continue;
        const uint64_t have = std::min<uint64_t>(ns.received, ns.fsz);
        const uint8_t* d = ns.bytes.data();
        uint64_t p = 0;
        while (p + 12 < have) {
            const uint32_t nsz = rd32(d + p), dsz = rd32(d + p + 4);
            const uint64_t name_end = p + 12 + std::min<uint64_t>(nsz, 64);
            if (name_end > have) break;
            size_t len = 0;
            while (p + 12 + len < name_end && d[p + 12 + len]) ++len;
            const uint64_t dp = p + 12 + ((uint64_t)nsz + 3) / 4 * 4;
            if (dp + dsz > have) break;
            for (int k = 0; k < kWanted; ++k)
                if (std::strlen(kNames[k]) == len && std::memcmp(d + p + 12, kNames[k], len) == 0) views[k] = View{d + dp, dsz};
            p = dp + ((uint64_t)dsz + 3) / 4 * 4;
        }
    }
    auto get = [&views](int k) -> const View* { return views[k].p ? &views[k] : nullptr; };
    if (const View* v = get(kApp)) if (v->n >= 0x18) f.app_title = cstr(v->p + 8, std::min<size_t>(16, v->n - 8));
    if (const View* v = get(kSystem)) {
        if (v->n >= 12) {
            const uint32_t fw = rd32(v->p + 8);
            char b[16];
            std::snprintf(b, sizeof b, "%x.%02x", (unsigned)(fw >> 24) & 0xff, (unsigned)(fw >> 16) & 0xff);
            f.fw = b;
        }
    }
    if (const View* v = get(kThread)) {
        uint64_t p = 8;
        while (p + 8 <= v->n && f.threads.size() < kMaxThreads) {
            const uint32_t size = rd32(v->p + p);
            if (size < 0xa0 || p + size > v->n) break;
            DumpThread t;
            t.uid = rd32(v->p + p + 4);
            t.name = cstr(v->p + p + 8, 32);
            t.status = rd32(v->p + p + 0x30) & 0xffff;
            t.stop_reason = rd32(v->p + p + 0x74);
            t.pc = rd32(v->p + p + 0x9c);
            f.threads.push_back(t);
            p += size;
        }
    }
    struct Reg { uint32_t tid, r[13], sp, lr, pc, cpsr; };
    std::vector<Reg> regs;
    if (const View* v = get(kThreadReg)) {
        uint64_t p = 8;
        while (p + 4 < v->n && regs.size() < kMaxThreads) {
            const uint32_t size = rd32(v->p + p);
            if (size < 8 || p + size > v->n) break;
            if (size < 76) break;                       // too short to hold cpsr
            Reg g;
            g.tid = rd32(v->p + p + 4);
            for (int i = 0; i < 13; ++i) g.r[i] = rd32(v->p + p + 8 + 4 * (uint64_t)i);
            g.sp = rd32(v->p + p + 60);
            g.lr = rd32(v->p + p + 64);
            g.pc = rd32(v->p + p + 68);
            g.cpsr = rd32(v->p + p + 72);
            regs.push_back(g);
            p += size;
        }
    }
    if (const View* v = get(kModule)) {
        const uint32_t count = v->n >= 8 ? rd32(v->p + 4) : 0;
        uint64_t p = 8;
        for (uint32_t i = 0; i < count && modules_.size() < kMaxModules && p + 0x50 <= v->n; ++i) {
            const uint32_t nseg = rd32(v->p + p + 0x4c);
            if (nseg > 16 || p + 0x50 + (uint64_t)nseg * 0x14 + 0x10 > v->n) break;
            Module m;
            m.uid = rd32(v->p + p + 4);
            m.name = cstr(v->p + p + 0x24, 32);
            for (uint32_t k = 0; k < nseg; ++k) {
                const uint64_t sp = p + 0x50 + (uint64_t)k * 0x14;
                ModSeg ms;
                ms.va = rd32(v->p + sp + 8);
                ms.size = rd32(v->p + sp + 12);
                m.segs.push_back(ms);
            }
            modules_.push_back(m);
            p += 0x50 + (uint64_t)nseg * 0x14 + 0x10;
        }
    }

    if (regs.empty()) return;
    int idx = 0;
    if (opt_.rule == ThreadRule::StopReason) {
        for (const DumpThread& t : f.threads) {
            if (t.stop_reason == 0) continue;
            for (size_t i = 0; i < regs.size(); ++i)
                if (regs[i].tid == t.uid) { idx = (int)i; break; }
            break;
        }
    }
    const Reg& g = regs[(size_t)idx];
    f.fault_index = idx;
    f.tid = g.tid;
    std::memcpy(f.r, g.r, sizeof f.r);
    f.sp = g.sp;
    f.lr = g.lr;
    f.pc = g.pc;
    f.cpsr = g.cpsr;
    for (const DumpThread& t : f.threads)
        if (t.uid == g.tid) { f.thread_name = t.name; f.stop_reason = t.stop_reason; break; }
    f.pc_addr = classify(f.pc);
    f.lr_addr = classify(f.lr);
}

// ModuleName of contract claim.v1: ^[A-Za-z0-9_.]{1,32}$.
bool host_module_name_ok(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

HostAddr Reader::classify(uint32_t a) const {
    HostAddr h;
    if (s_.eboot_size && a - s_.eboot_base < s_.eboot_size) {
        h.region = "eboot"; h.module = "eboot"; h.offset = a - s_.eboot_base;
        return h;
    }
    if (s_.jit_hi > s_.jit_lo && a >= s_.jit_lo && a < s_.jit_hi) {
        h.region = "jit"; h.module = "jit"; h.offset = a - s_.jit_lo;
        return h;
    }
    // The application's own ELF is the first module the kernel loads: it
    // had the lowest uid (0x40010001) in every real dump examined.
    uint32_t main_uid = 0xFFFFFFFFu;
    for (const Module& m : modules_) main_uid = std::min(main_uid, m.uid);
    for (const Module& m : modules_) {
        for (const ModSeg& sg : m.segs) {
            if (sg.size == 0 || a - sg.va >= sg.size) continue;
            if (m.uid == main_uid) {
                h.region = "eboot"; h.module = "eboot"; h.offset = a - sg.va;
                return h;
            }
            // A system module keeps its name exactly as listed; a name the
            // contract cannot carry is reported as region unknown
            // (signature-rules.v1.md §7), never rewritten into another name.
            if (host_module_name_ok(m.name)) {
                h.region = "sysmodule"; h.module = m.name; h.offset = a - sg.va;
                return h;
            }
            h.region = "unknown"; h.module = "unknown"; h.offset = a;
            return h;
        }
    }
    h.region = "unknown"; h.module = "unknown"; h.offset = a;
    return h;
}

// Captures are planned once the notes are parsed, while the stream is at
// `from`: only ranges starting at or after it can still be captured.
void Reader::plan_captures(uint64_t from) {
    if (f.fault_index < 0) return;
    struct Win { uint64_t lo, hi; };
    std::vector<Win> wins;
    const uint64_t host = s_.arena_host_base;
    const uint64_t esp = host + f.r[8], ebp = host + f.r[9];
    wins.push_back(Win{esp, esp + kGuestStackWindow});
    if (!(ebp >= esp && ebp + 8 <= esp + kGuestStackWindow)) wins.push_back(Win{ebp, ebp + kGuestStackWindow});
    if (f.pc >= kPcWindow) wins.push_back(Win{(uint64_t)f.pc - kPcWindow, (uint64_t)f.pc + kPcWindow});
    size_t total = 0;
    for (const Win& w0 : wins) {
        const Win w{w0.lo, std::min<uint64_t>(w0.hi, 0x100000000ull)};
        if (w.lo >= w.hi) continue;
        for (size_t i = 0; i < loads_.size(); ++i) {
            const Seg& sg = loads_[i];
            const uint64_t lo = std::max<uint64_t>(w.lo, sg.va), hi = std::min<uint64_t>(w.hi, (uint64_t)sg.va + sg.fsz);
            if (lo >= hi) continue;
            const uint64_t off_lo = (uint64_t)sg.off + (lo - sg.va);
            if (off_lo < from) continue;                // already streamed past
            if (total + (hi - lo) > kCaptureCap) continue;
            Capture c;
            c.load = i;
            c.va_lo = (uint32_t)lo;
            c.off_lo = off_lo;
            c.bytes.assign((size_t)(hi - lo), 0);
            total += (size_t)(hi - lo);
            caps_.push_back(std::move(c));
        }
    }
}

// The session stamp is the user data of sceCoredumpWriteUserData, which the
// system writes as a note: only PT_NOTE bytes are scanned. Memory can hold
// stamp-like bytes that are no stamp of the dump (the eboot's format string,
// an old stamp left in the heap), and a false stamp would veto the fallback
// on epoch + title in build_evidence. Bytes of successive notes are scanned
// as one text only where they are contiguous.
void Reader::stamp_scan_notes(uint64_t pos, const uint8_t* data, size_t n) {
    for (const NoteSeg& ns : notes_) {
        if (f.has_stamp) return;
        const uint64_t a = std::max(pos, (uint64_t)ns.off), b = std::min(pos + n, (uint64_t)ns.off + ns.fsz);
        if (a >= b) continue;
        if (a != stamp_next_) stamp_reset();
        stamp_feed(data + (a - pos), (size_t)(b - a));
        stamp_next_ = b;
    }
}

void Reader::stamp_reset() {
    stamp_state_ = 0;
    stamp_capturing_ = false;
    stamp_buf_.clear();
}

bool stamp_session_id_ok(const std::string& s) {
    if (s.size() != 32) return false;   // 128 random bits (spec 4.1), lowercase hex
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool stamp_build_id_ok(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '-'))
            return false;
    return true;
}

void Reader::stamp_feed(const uint8_t* data, size_t n) {
    if (f.has_stamp) return;
    const size_t mlen = sizeof kStampMarker - 1;
    for (size_t i = 0; i < n; ++i) {
        const char c = (char)data[i];
        if (stamp_capturing_) {
            if (c == '\n' || c == '\0' || stamp_buf_.size() >= kStampMax) {
                // "session_id=<32 lowercase hex> build_id=<id>"; anything
                // else is no stamp, and the scan goes on.
                std::string sid, bid;
                size_t p = 0;
                while (p < stamp_buf_.size()) {
                    size_t e = stamp_buf_.find(' ', p);
                    if (e == std::string::npos) e = stamp_buf_.size();
                    const std::string tok = stamp_buf_.substr(p, e - p);
                    if (tok.compare(0, 11, "session_id=") == 0) sid = tok.substr(11);
                    else if (tok.compare(0, 9, "build_id=") == 0) bid = tok.substr(9);
                    p = e + 1;
                }
                if (stamp_buf_.size() < kStampMax && stamp_session_id_ok(sid)) {
                    f.has_stamp = true;
                    f.stamp_session_id = sid;
                    f.stamp_build_id = stamp_build_id_ok(bid) ? bid : std::string();
                    stamp_buf_.clear();
                    return;
                }
                stamp_reset();
                continue;
            }
            stamp_buf_ += c;
            continue;
        }
        if (c == kStampMarker[stamp_state_]) {
            if (++stamp_state_ == mlen) { stamp_capturing_ = true; stamp_state_ = 0; }
        } else {
            stamp_state_ = (c == kStampMarker[0]) ? 1 : 0;
        }
    }
}

ReadStatus Reader::read_va(uint64_t va, uint32_t n, uint8_t* out) const {
    if (va + n > 0x100000000ull) return ReadStatus::NotInDump;
    for (size_t idx : load_order_) {
        const Seg& sg = loads_[idx];
        if (!(sg.va <= va && va + n <= (uint64_t)sg.va + sg.fsz)) continue;
        // First containing segment, as tools/autopsie_psp2dmp.py:47-52 does.
        const uint64_t off = (uint64_t)sg.off + (va - sg.va);
        if (off + n > pos_) return ReadStatus::NotInDump;       // segment cut short in this file
        for (const Capture& c : caps_) {
            if (c.load != idx || va < c.va_lo || va + n > (uint64_t)c.va_lo + c.bytes.size()) continue;
            std::memcpy(out, c.bytes.data() + (va - c.va_lo), n);
            return ReadStatus::Ok;
        }
        return ReadStatus::NotCaptured;
    }
    return ReadStatus::NotInDump;
}

void Reader::walk_chain() {
    if (f.fault_index < 0) { f.chain_end = "no_thread"; return; }
    if (s_.game_base == 0) { f.chain_end = "no_game_base"; return; }
    uint32_t ebp = f.r[9];
    for (int i = 0; i < kEbpChainMaxFrames; ++i) {
        uint8_t fr[8];
        const ReadStatus st = read_va((uint64_t)s_.arena_host_base + ebp, 8, fr);
        if (st == ReadStatus::NotInDump) { f.chain_end = "not_in_dump"; return; }
        if (st == ReadStatus::NotCaptured) { f.chain_end = "outside_capture"; return; }
        const uint32_t nxt = rd32(fr), ra = rd32(fr + 4);
        if (!(ra >= s_.game_base && (uint64_t)ra < (uint64_t)s_.game_base + kEbpChainCodeSpan)) {
            f.chain_end = "ret_outside_game";
            return;
        }
        f.guest_chain.push_back(GuestFrame{ebp, ra});
        if (nxt <= ebp || nxt - ebp > kEbpChainMaxLink) { f.chain_end = "bad_link"; return; }
        ebp = nxt;
    }
    f.chain_end = "max_frames";
}

void Reader::finish(bool read_ok) {
    if (!opt_.notes_only) scanner_.finish();
    f.secret_hits = scanner_.hits();
    if (mode_ == Mode::Unknown) {
        f.error = read_ok ? "empty file" : "cannot read file";
        return;
    }
    if (!read_ok && !intentional_stop_ && f.error.empty()) f.error = "read error";
    if (mode_ == Mode::Gzip && !stream_end_ && !intentional_stop_) {
        f.truncated = true;
        if (f.error.empty()) f.error = "gzip stream truncated";
    }
    if (!header_ok_) {
        if (f.error.empty() || f.error == "gzip stream truncated") f.error = "not an ELF core (nor a gzip of one)";
        return;
    }
    if (!phdrs_ok_) {
        f.truncated = true;
        if (f.error.empty()) f.error = "program headers cut short";
        return;
    }
    if (!lead_parsed_) {
        f.truncated = true;
        lead_parsed_ = true;
        parse_notes();
        plan_captures(pos_);
    } else if (trail_wanted_ && !trail_parsed_) {
        f.truncated = true;
        trail_parsed_ = true;
        parse_notes();
    }
    if (!intentional_stop_ && pos_ < data_end_) f.truncated = true;
    f.ok = f.fault_index >= 0;
    if (!f.ok && f.error.empty()) f.error = "no THREAD_REG_INFO thread";
    if (opt_.notes_only) {
        f.chain_end = "notes_only";
        return;
    }
    walk_chain();
    if (f.fault_index >= 0 && f.pc >= kPcWindow) {
        uint8_t b[2 * kPcWindow];
        if (read_va((uint64_t)f.pc - kPcWindow, 2 * kPcWindow, b) == ReadStatus::Ok) f.pc_bytes.assign(b, b + sizeof b);
    }
}

bool reader_cb(const uint8_t* p, size_t n, void* ud) { return static_cast<Reader*>(ud)->on_input(p, n); }

}  // namespace

std::string stop_reason_name(uint32_t code) {
    switch (code) {
        case 0: return "none";
        case 0x30002: return "undefined_instruction";
        case 0x30003: return "prefetch_abort";
        case 0x30004: return "data_abort";
        default: break;
    }
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", (unsigned)code);
    return b;
}

DumpFacts read_psp2dmp(IoApi& io, const std::string& path, const SessionRecord& session,
                       const SecretPatterns& patterns, const DumpReadOptions& opt) {
    Reader r(session, patterns, opt);
    const bool ok = io.read_stream(path, kInChunk, reader_cb, &r);
    r.finish(ok);
    return std::move(r.f);
}

}  // namespace d2cr
