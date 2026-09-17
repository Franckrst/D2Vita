// src/crashreport/cr_progress_parse.cpp — facts from boot_progress.txt / crash.log.
//
// Writers (winx86 sources):
//   prefix "[%4u.%02us] "            winx86 platform/vita_host.cpp:34-35
//   NATIVE FAULT block               winx86 runtime/sched_native.cpp:607-630
//   alive: heartbeat                 src/platform/vita_present.cpp:895-941
//     run= field                     winx86 runtime/sched_native.cpp:120-160
//   scheduler stopped (3 forms)      tools/rt_boot.cpp:4840-4845, 4855-4856
//   CLEAN EXIT                       tools/rt_boot.cpp:5195 (and self-test exits)
//   UNSHIMMED                        tools/rt_boot.cpp:3389
//   FatalAppExitA / RaiseException   src/runtime/win32_import_remainder.cpp:45, 54
//   ExitProcess / TerminateProcess   src/runtime/win32_import_remainder.cpp:72-78, 94, 98
//   env.txt: KEY=VALUE               src/platform/d2_boot_config.cpp:241
#include "crashreport/cr_progress_parse.h"

#include <cstdio>
#include <cstring>

namespace d2cr {

namespace {

// What one fault block may accumulate: the writer lists the loaded modules
// and at most 16 stack words (sched_native.cpp:612-626).
constexpr size_t kBlockModulesMax = 64;
constexpr size_t kBlockWordsMax = 32;
// ExitProcess/TerminateProcess log at most ten return addresses
// (win32_import_remainder.cpp:72-78).
constexpr size_t kExitChainMax = 16;

// Strict left-to-right matcher over one line.
class Scan {
 public:
    Scan(const std::string& s, size_t pos = 0) : s_(s), p_(pos) {}

    bool lit(const char* t) {
        size_t q = p_;
        for (; *t; ++t, ++q) if (q >= s_.size() || s_[q] != *t) return false;
        p_ = q;
        return true;
    }
    bool hex_fixed(size_t n, uint32_t* v) {
        if (p_ + n > s_.size()) return false;
        uint32_t r = 0;
        for (size_t i = 0; i < n; ++i) {
            int d = hexval(s_[p_ + i]);
            if (d < 0) return false;
            r = (r << 4) | (uint32_t)d;
        }
        p_ += n;
        *v = r;
        return true;
    }
    bool hex_var(uint32_t* v) {
        size_t n = 0;
        uint32_t r = 0;
        while (p_ + n < s_.size() && hexval(s_[p_ + n]) >= 0) {
            if (n == 8) return false;
            r = (r << 4) | (uint32_t)hexval(s_[p_ + n]);
            ++n;
        }
        if (n == 0) return false;
        p_ += n;
        *v = r;
        return true;
    }
    bool dec(uint64_t* v) {
        size_t n = 0;
        uint64_t r = 0;
        while (p_ + n < s_.size() && s_[p_ + n] >= '0' && s_[p_ + n] <= '9') {
            if (n == 19) return false;
            r = r * 10 + (uint64_t)(s_[p_ + n] - '0');
            ++n;
        }
        if (n == 0) return false;
        p_ += n;
        *v = r;
        return true;
    }
    bool dec32(uint32_t* v) {
        uint64_t r;
        size_t save = p_;
        if (!dec(&r) || r > 0xFFFFFFFFull) { p_ = save; return false; }
        *v = (uint32_t)r;
        return true;
    }
    bool sdec(int64_t* v) {
        size_t save = p_;
        const bool neg = lit("-");
        uint64_t r;
        if (!dec(&r)) { p_ = save; return false; }
        *v = neg ? -(int64_t)r : (int64_t)r;
        return true;
    }
    void skip_spaces() { while (p_ < s_.size() && s_[p_] == ' ') ++p_; }
    bool at_end() const { return p_ == s_.size(); }
    size_t pos() const { return p_; }

 private:
    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    const std::string& s_;
    size_t p_;
};

// "[%4u.%02us] " — returns the body offset, or 0 when there is no prefix.
size_t strip_prefix(const std::string& line, int64_t* seconds) {
    Scan sc(line);
    if (!sc.lit("[")) return 0;
    sc.skip_spaces();
    uint64_t s = 0, frac = 0;
    if (!sc.dec(&s) || !sc.lit(".") || !sc.dec(&frac) || !sc.lit("s] ")) return 0;
    *seconds = (int64_t)s;
    return sc.pos();
}

// sched_native.cpp:627-630  "NATIVE FAULT thr %u eip=%08x addr=%08x"
bool match_native_fault(const std::string& line, size_t body, uint32_t* thr, uint32_t* eip, uint32_t* addr) {
    Scan sc(line, body);
    return sc.lit("NATIVE FAULT thr ") && sc.dec32(thr) && sc.lit(" eip=") && sc.hex_fixed(8, eip) &&
           sc.lit(" addr=") && sc.hex_fixed(8, addr) && sc.at_end();
}

struct PendingBlock {
    bool active = false;
    uint32_t thread = 0;
    uint32_t reg[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // EAX EBX ECX EDX ESI EDI EBP ESP
    std::vector<GuestModule> modules;
    struct Word { uint32_t value, base, offset; };
    std::vector<Word> words;
};

struct Beat {
    int64_t frames = 0;
    uint32_t eip = 0;
    std::string run;
};

// One entry of run=: "<id>:<state>:<blocks>" (sched_native.cpp:147-148).
struct RunEntry { uint32_t id; char state; uint64_t blocks; };

// Entries in increasing id order; a repeated id keeps its last value.
std::vector<RunEntry> parse_run(const std::string& run) {
    std::vector<RunEntry> r;
    size_t pos = 0;
    while (pos < run.size()) {
        size_t end = run.find(',', pos);
        if (end == std::string::npos) end = run.size();
        const std::string ent = run.substr(pos, end - pos);
        pos = end + 1;
        Scan sc(ent);
        uint32_t id;
        uint64_t blocks;
        if (!sc.dec32(&id) || !sc.lit(":") || sc.pos() >= ent.size()) continue;
        const char st = ent[sc.pos()];
        Scan sb(ent, sc.pos() + 1);
        if (!sb.lit(":") || !sb.dec(&blocks)) continue;   // "+k" overflow suffix is ignored
        size_t i = 0;
        while (i < r.size() && r[i].id < id) ++i;
        const RunEntry e = {id, st, blocks};
        if (i < r.size() && r[i].id == id) r[i] = e;
        else r.insert(r.begin() + (std::ptrdiff_t)i, e);
    }
    return r;
}

const RunEntry* find_run(const std::vector<RunEntry>& v, uint32_t id) {
    for (const RunEntry& e : v) if (e.id == id) return &e;
    return nullptr;
}

// Verdict of the vivacity field over the stall (sched_native.cpp:103-116):
// a counter that moves while frames are frozen is starvation; no counter
// moving with a runner in R is a runner making no progress (deadlock);
// no counter moving with every runner waiting is "blocked". `ref` is the
// beat where the stall began (null when there is no stall).
std::string runner_class(const Beat& last_beat, const Beat* ref) {
    if (!ref || last_beat.run.empty() || ref->run.empty()) return std::string();
    const std::vector<RunEntry> last = parse_run(last_beat.run);
    const std::vector<RunEntry> before = parse_run(ref->run);
    if (last.empty()) return std::string();
    bool moved = false, running = false;
    for (const RunEntry& e : last) {
        const RunEntry* b = find_run(before, e.id);
        if (!b || b->blocks != e.blocks) moved = true;
        if (e.state == 'R') running = true;
    }
    return moved ? "starvation" : running ? "deadlock" : "blocked";
}

std::string runner_state(const Beat& last_beat, const Beat* ref) {
    if (last_beat.run.empty()) return std::string();
    const std::vector<RunEntry> last = parse_run(last_beat.run);
    const bool has_ref = ref && !ref->run.empty();
    std::vector<RunEntry> before;
    if (has_ref) before = parse_run(ref->run);
    std::string out;
    for (const RunEntry& e : last) {
        char b[32];
        std::snprintf(b, sizeof b, "%s%u:%c", out.empty() ? "" : ",", (unsigned)e.id, e.state);
        out += b;
        if (has_ref) {
            const RunEntry* p = find_run(before, e.id);
            if (!p || p->blocks != e.blocks) out += '+';
        }
    }
    return out;
}

Addr module_word(const PendingBlock::Word& w, const std::vector<GuestModule>& modules, uint32_t game_base) {
    for (const GuestModule& m : modules)
        if (m.base == w.base) return guest_addr(w.value, game_base, modules);
    // Module line missing: the pile line still names the base.
    std::vector<GuestModule> one(1);
    one[0].base = w.base;
    one[0].size = w.offset + 1;
    return guest_addr(w.value, game_base, one);
}

}  // namespace

struct ProgressParser::State {
    explicit State(uint32_t gb) : game_base(gb) { line.reserve(512); }

    void end_line();
    void process(const std::string& line);
    void on_beat(Beat& b);

    uint32_t game_base;
    ProgressFacts f;
    PendingBlock pend;
    std::string line;          // current line, at most kProgressLineMax bytes
    bool overlong = false;     // current line went over kProgressLineMax
    int line_no = 0;           // index of the current line
    bool have_beat = false;
    Beat last;                 // last beat
    Beat stall_start;          // beat where the final stall began (valid while f.stalled_beats > 0)
    bool finished = false;
};

ProgressParser::ProgressParser(uint32_t game_base) : st_(new State(game_base)) {}
ProgressParser::~ProgressParser() = default;

void ProgressParser::feed(const char* data, size_t n) {
    State& s = *st_;
    if (s.finished) return;
    while (n > 0) {
        const char* nl = static_cast<const char*>(std::memchr(data, '\n', n));
        const size_t take = nl ? (size_t)(nl - data) : n;
        if (!s.overlong) {
            if (s.line.size() + take <= kProgressLineMax) {
                s.line.append(data, take);
            } else {
                s.overlong = true;
                s.line.clear();
            }
        }
        if (!nl) return;
        s.end_line();
        data = nl + 1;
        n -= take + 1;
    }
}

ProgressFacts ProgressParser::finish() {
    State& s = *st_;
    if (s.finished) return s.f;
    s.finished = true;
    if (!s.line.empty() || s.overlong) s.end_line();
    if (s.have_beat) {
        ProgressFacts& f = s.f;
        f.has_last_eip = true;
        f.last_eip = s.last.eip;
        f.last_eip_addr = guest_addr(f.last_eip, s.game_base, std::vector<GuestModule>());
        const Beat* ref = f.stalled_beats > 0 ? &s.stall_start : nullptr;
        f.runner_state = runner_state(s.last, ref);
        f.runner_class = runner_class(s.last, ref);
    }
    return s.f;
}

void ProgressParser::State::end_line() {
    if (!overlong) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        process(line);
    }
    line.clear();
    overlong = false;
    ++line_no;
}

void ProgressParser::State::on_beat(Beat& b) {
    ++f.beats;
    // Same count as walking back from the last beat while frames= repeats.
    if (have_beat && b.frames >= 0 && b.frames == last.frames) {
        if (f.stalled_beats == 0) stall_start = last;
        ++f.stalled_beats;
    } else {
        f.stalled_beats = 0;
    }
    last.frames = b.frames;
    last.eip = b.eip;
    last.run.swap(b.run);
    have_beat = true;
}

void ProgressParser::State::process(const std::string& line) {
    int64_t secs = -1;
    const size_t body = strip_prefix(line, &secs);
    if (secs >= 0) f.last_uptime_s = secs;

    // sched_native.cpp:627-630  "NATIVE FAULT thr %u eip=%08x addr=%08x"
    {
        uint32_t thr, eip, addr;
        if (match_native_fault(line, body, &thr, &eip, &addr)) {
            ++f.native_faults;
            if (!f.native_fault) {
                f.native_fault = true;
                f.native_fault_line = line_no;
                f.fault_thread = thr;
                // Thread ids start at 1 and set_main() takes the first one
                // (sched_native.h:272, sched_native.cpp:228).
                f.fault_main = (thr == 1);
                f.fault_eip = eip;
                f.fault_addr = addr;
                if (pend.active && pend.thread == thr) {
                    f.has_fault_regs = true;
                    f.eax = pend.reg[0]; f.ebx = pend.reg[1]; f.ecx = pend.reg[2]; f.edx = pend.reg[3];
                    f.esi = pend.reg[4]; f.edi = pend.reg[5]; f.ebp = pend.reg[6]; f.esp = pend.reg[7];
                    f.fault_modules = pend.modules;
                    for (const auto& w : pend.words) {
                        if (f.fault_frames.size() >= 16) break;
                        f.fault_frames.push_back(module_word(w, pend.modules, game_base));
                    }
                }
                f.fault_eip_addr = guest_addr(eip, game_base, f.fault_modules);
            }
            pend = PendingBlock();
            return;
        }
    }
    // sched_native.cpp:607-611  "  fault thr %u: EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x"
    {
        Scan sc(line, body);
        uint32_t thr;
        if (sc.lit("  fault thr ") && sc.dec32(&thr) && sc.lit(":")) {
            static const char* const kReg[8] = {" EAX=", " EBX=", " ECX=", " EDX=", " ESI=", " EDI=", " EBP=", " ESP="};
            PendingBlock nb;
            bool ok = true;
            for (int i = 0; i < 8 && ok; ++i) ok = sc.lit(kReg[i]) && sc.hex_fixed(8, &nb.reg[i]);
            if (ok && sc.at_end()) {
                nb.active = true;
                nb.thread = thr;
                pend = nb;
            }
            return;
        }
    }
    // sched_native.cpp:612-615  "  fault module @%08x taille %08x%s"  (%s = "" or "  <- EIP ICI")
    {
        Scan sc(line, body);
        GuestModule m;
        if (sc.lit("  fault module @") && sc.hex_fixed(8, &m.base) && sc.lit(" taille ") && sc.hex_fixed(8, &m.size)) {
            if (pend.active && (sc.at_end() || sc.lit("  <- EIP ICI")) && pend.modules.size() < kBlockModulesMax)
                pend.modules.push_back(m);
            return;
        }
    }
    // sched_native.cpp:622-626  "  fault pile [esp+0x%02x] 0x%08x (module @%08x +0x%x)"
    {
        Scan sc(line, body);
        uint32_t off, value, base, rel;
        if (sc.lit("  fault pile [esp+0x") && sc.hex_var(&off) && sc.lit("] 0x") && sc.hex_fixed(8, &value) &&
            sc.lit(" (module @") && sc.hex_fixed(8, &base) && sc.lit(" +0x") && sc.hex_var(&rel) &&
            sc.lit(")") && sc.at_end()) {
            if (pend.active && pend.words.size() < kBlockWordsMax) pend.words.push_back(PendingBlock::Word{value, base, rel});
            return;
        }
    }
    // vita_present.cpp:895-941  "alive: pump=%llu frames=%d reads=%llu eip=%08x ..." [+ " run=..."]
    {
        Scan sc(line, body);
        if (sc.lit("alive: ")) {
            const size_t fr = line.find(" frames=", body);
            const size_t ep = line.find(" eip=", body);
            Beat b;
            bool ok = fr != std::string::npos && ep != std::string::npos;
            if (ok) { Scan s1(line, fr + 8); ok = s1.sdec(&b.frames) && (s1.at_end() || line[s1.pos()] == ' '); }
            if (ok) { Scan s2(line, ep + 5); ok = s2.hex_fixed(8, &b.eip) && (s2.at_end() || line[s2.pos()] == ' '); }
            if (ok) {
                const size_t rp = line.find(" run=", body);
                if (rp != std::string::npos) {
                    size_t e = line.find(' ', rp + 5);
                    if (e == std::string::npos) e = line.size();
                    b.run.assign(line, rp + 5, e - (rp + 5));
                }
                on_beat(b);
            }
            return;
        }
    }
    // rt_boot.cpp:4855-4856  "scheduler stopped: %s | frame=%d main-exit=0x%08x"            (native)
    // rt_boot.cpp:4844-4845  "scheduler stopped: %s | frame=%d switches=%llu main-exit=0x%08x" (coop)
    // rt_boot.cpp:4840-4841  "scheduler stopped: %s (%llu switches) exit=0x%08x"             (coop)
    {
        Scan sc(line, body);
        if (sc.lit("scheduler stopped: ")) {
            const size_t rstart = sc.pos();
            const size_t bar = line.rfind(" | frame=");
            if (bar != std::string::npos && bar > rstart) {
                Scan s1(line, bar + 9);
                int64_t frame;
                uint64_t sw;
                uint32_t code;
                if (s1.sdec(&frame) && (s1.lit(" switches=") ? s1.dec(&sw) : true) &&
                    s1.lit(" main-exit=0x") && s1.hex_fixed(8, &code) && s1.at_end()) {
                    f.sched_stopped = true;
                    f.stop_reason = line.substr(rstart, bar - rstart);
                    f.has_main_exit = true;
                    f.main_exit = code;
                }
                return;
            }
            const size_t par = line.rfind(" switches) exit=0x");
            if (par != std::string::npos) {
                size_t open = line.rfind(" (", par);
                uint64_t sw;
                uint32_t code;
                Scan s2(line, open == std::string::npos ? line.size() : open + 2);
                Scan s3(line, par + 18);
                if (open != std::string::npos && open > rstart && s2.dec(&sw) && s2.pos() == par &&
                    s3.hex_fixed(8, &code) && s3.at_end()) {
                    f.sched_stopped = true;
                    f.stop_reason = line.substr(rstart, open - rstart);
                    f.has_main_exit = true;
                    f.main_exit = code;
                }
            }
            return;
        }
    }
    // rt_boot.cpp:5195  "CLEAN EXIT (game path, frame=%d)" (and the self-test variants)
    { Scan sc(line, body); if (sc.lit("CLEAN EXIT")) { f.clean_exit = true; return; } }
    // rt_boot.cpp:3389  "UNSHIMMED %s -> arret controle (import manquant)"
    {
        Scan sc(line, body);
        if (sc.lit("UNSHIMMED ")) {
            static const char kTail[] = " -> arret controle (import manquant)";
            const size_t t = line.rfind(kTail);
            if (t != std::string::npos && t > sc.pos() && t + sizeof kTail - 1 == line.size()) {
                f.unshimmed = true;
                f.unshimmed_import = line.substr(sc.pos(), t - sc.pos());
            }
            return;
        }
    }
    // win32_import_remainder.cpp:45  "FatalAppExitA: %s"
    { Scan sc(line, body); if (sc.lit("FatalAppExitA: ")) { f.fatal_app_exit = true; return; } }
    // win32_import_remainder.cpp:54  "RaiseException 0x%08x -> arret controle"
    {
        Scan sc(line, body);
        uint32_t code;
        if (sc.lit("RaiseException 0x") && sc.hex_fixed(8, &code) && sc.lit(" -> arret controle") && sc.at_end()) {
            f.raise_exception = true;
            f.raise_code = code;
            return;
        }
    }
    // win32_import_remainder.cpp:72-78 (exit_chain), called by ExitProcess (:94) and TerminateProcess (:98)
    //   "%s code=%u chain:" followed by up to ten " %08x"
    {
        Scan sc(line, body);
        const bool ep = sc.lit("ExitProcess code=");
        const bool tp = !ep && sc.lit("TerminateProcess code=");
        uint32_t code;
        if ((ep || tp) && sc.dec32(&code) && sc.lit(" chain:")) {
            std::vector<Addr> chain;
            bool ok = true;
            while (ok && !sc.at_end()) {
                uint32_t v;
                ok = sc.lit(" ") && sc.hex_fixed(8, &v);
                if (ok && chain.size() < kExitChainMax) chain.push_back(guest_addr(v, game_base, std::vector<GuestModule>()));
            }
            if (ok) {
                f.exit_process = f.exit_process || ep;
                f.terminate_process = f.terminate_process || tp;
                f.exit_code = code;
                f.exit_chain.swap(chain);
            }
            return;
        }
    }
    // d2_boot_config.cpp:241  "env.txt: %s=%s"
    {
        Scan sc(line, body);
        if (sc.lit("env.txt: D2NET=")) {
            const size_t v = sc.pos();
            f.online = v < line.size() && !(line.size() == v + 1 && line[v] == '0');
        }
    }
}

ProgressFacts parse_progress(const std::string& text, uint32_t game_base) {
    ProgressParser p(game_base);
    p.feed(text.data(), text.size());
    return p.finish();
}

bool progress_line_is_native_fault(const std::string& line) {
    size_t n = line.size();
    if (n && line[n - 1] == '\n') --n;
    if (n && line[n - 1] == '\r') --n;
    if (n > kProgressLineMax || line.find('\n') < n) return false;
    if (line.find("NATIVE FAULT thr ") >= n) return false;   // cheap filter, no copy
    const std::string body_line = line.substr(0, n);
    int64_t secs = -1;
    uint32_t thr, eip, addr;
    return match_native_fault(body_line, strip_prefix(body_line, &secs), &thr, &eip, &addr);
}

namespace {
bool progress_cb(const uint8_t* data, size_t n, void* ud) {
    static_cast<ProgressParser*>(ud)->feed(static_cast<const char*>(static_cast<const void*>(data)), n);
    return true;
}
}  // namespace

bool parse_progress_file(IoApi& io, const std::string& path, uint32_t game_base, ProgressFacts* out, size_t chunk) {
    ProgressParser p(game_base);
    if (!io.read_stream(path, chunk, progress_cb, &p)) return false;
    *out = p.finish();
    return true;
}

}  // namespace d2cr
