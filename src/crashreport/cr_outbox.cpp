// src/crashreport/cr_outbox.cpp — report outbox (spec §4.2, §4.5).
#include "crashreport/cr_outbox.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "crashreport/cr_progress_parse.h"

namespace d2cr {

namespace {

constexpr uint64_t kSealHeader = 72, kSealTag = 16, kSealChunk = 65536;
constexpr size_t kStateMax = 4096;
constexpr size_t kRecordMax = 64 * 1024;
// Pieces of the fault-window stream: a whole line of the progress parser
// (kProgressLineMax bytes) plus its newline fits in one piece.
constexpr size_t kWindowLineMax = kProgressLineMax + 1;

std::string one_line(const std::string& v) {
    std::string r = v;
    for (char& c : r) if (c == '\n' || c == '\r') c = ' ';
    return r;
}

bool parse_i64(const std::string& v, int64_t* out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (!end || *end) return false;
    *out = x;
    return true;
}

bool kind_from_name(const std::string& n, Kind* k) {
    static const Kind all[] = {Kind::None, Kind::Hang, Kind::GuestFault, Kind::AbnormalExit, Kind::Halt, Kind::HostFault};
    for (Kind x : all)
        if (n == kind_name(x)) { *k = x; return true; }
    return false;
}

std::vector<std::pair<std::string, std::string>> key_values(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> kv;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
        kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return kv;
}

// Redacts all of *s; counts the matches that overlap [keep_lo, keep_hi).
int redact_count_in(std::string* s, const SecretPatterns& p, size_t keep_lo, size_t keep_hi) {
    if (p.empty() || s->empty()) return 0;
    const uint8_t* data = (const uint8_t*)s->data();
    const size_t n = s->size();
    int count = 0;
    size_t i = 0;
    while (i < n) {
        bool utf16 = false;
        const size_t len = p.match_at(data, n, i, true, &utf16);
        if (len == 0) { ++i; continue; }
        for (size_t k = i; k < i + len; k += utf16 ? 2 : 1) (*s)[k] = 'X';
        if (i < keep_hi && i + len > keep_lo) ++count;
        i += len;
    }
    return count;
}

std::string omitted(uint64_t bytes) {
    char b[80];
    std::snprintf(b, sizeof b, "\n=== d2cr: %llu bytes omitted ===\n", (unsigned long long)bytes);
    return b;
}

// Bytes around the first NATIVE FAULT line of a log (the line the progress
// parser reports), found by streaming with bounded memory: at most
// max_before bytes and max_lines lines before the fault line, at most
// max_after bytes and max_lines lines after it. Holds about
// 2 * max_before + kWindowLineMax bytes before the fault is seen, and
// max_before + kWindowLineMax + max_after once it is.
class FaultWindow {
 public:
    FaultWindow(size_t max_before, size_t max_after, size_t max_lines)
        : max_before_(max_before), max_after_(max_after), max_lines_(max_lines), starts_(max_lines + 1, 0) {}

    bool feed(const uint8_t* d, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (cur_.empty()) cur_off_ = pos_ + i;
            const size_t avail = std::min(n - i, kWindowLineMax - cur_.size());
            const void* nl = std::memchr(d + i, '\n', avail);
            const size_t take = nl ? (size_t)(static_cast<const uint8_t*>(nl) - (d + i)) + 1 : avail;
            cur_.append(static_cast<const char*>(static_cast<const void*>(d + i)), take);
            i += take;
            if (nl || cur_.size() >= kWindowLineMax) {
                const bool more = piece(nl != nullptr);
                cur_.clear();
                if (!more) {
                    done_ = true;
                    return false;
                }
            }
        }
        pos_ += n;
        return true;
    }
    // A last piece without a newline (the log was cut).
    void finish() {
        if (!done_ && !cur_.empty()) piece(false);
        cur_.clear();
        std::string().swap(before_);
    }

    bool found() const { return found_; }
    uint64_t fault_off() const { return fault_off_; }
    uint64_t begin() const { return begin_; }
    uint64_t end() const { return begin_ + text_.size(); }
    std::string& text() { return text_; }      // bytes [begin, end)

 private:
    bool piece(bool ends_line) {
        const bool line_start = at_line_start_;
        at_line_start_ = ends_line;
        if (found_) {
            const size_t add = std::min(cur_.size(), max_after_ - after_bytes_);
            text_.append(cur_, 0, add);
            after_bytes_ += add;
            if (ends_line) ++after_lines_;
            return after_bytes_ < max_after_ && after_lines_ < max_lines_;
        }
        if (line_start) {                       // ring of the last max_lines + 1 line starts
            starts_[next_start_] = cur_off_;
            next_start_ = (next_start_ + 1) % starts_.size();
            if (seen_starts_ < starts_.size()) ++seen_starts_;
        }
        // A whole line, or the unterminated last one, as the parser sees it.
        if (line_start && (ends_line || cur_.size() < kWindowLineMax) && progress_line_is_native_fault(cur_)) {
            found_ = true;
            fault_off_ = cur_off_;
            uint64_t lo = before_off_;
            if (fault_off_ - lo > max_before_) lo = fault_off_ - max_before_;
            if (seen_starts_ == starts_.size()) lo = std::max(lo, starts_[next_start_]);   // oldest: max_lines lines back
            begin_ = lo;
            text_.assign(before_, (size_t)(lo - before_off_), std::string::npos);
            std::string().swap(before_);
            text_ += cur_;
            return max_after_ > 0 && max_lines_ > 0;
        }
        before_ += cur_;
        if (before_.size() > 2 * max_before_ + kWindowLineMax) {
            const size_t drop = before_.size() - max_before_;
            before_.erase(0, drop);
            before_off_ += drop;
        }
        return true;
    }

    const size_t max_before_, max_after_, max_lines_;
    std::string cur_;
    uint64_t cur_off_ = 0, pos_ = 0;
    bool at_line_start_ = true, done_ = false;
    std::string before_;                        // bytes [before_off_, +size) preceding the current piece
    uint64_t before_off_ = 0;
    std::vector<uint64_t> starts_;
    size_t next_start_ = 0, seen_starts_ = 0;
    bool found_ = false;
    uint64_t fault_off_ = 0, begin_ = 0;
    std::string text_;
    size_t after_bytes_ = 0, after_lines_ = 0;
};

bool window_cb(const uint8_t* p, size_t n, void* ud) { return static_cast<FaultWindow*>(ud)->feed(p, n); }

}  // namespace

uint64_t sealed_size(uint64_t plain) { return kSealHeader + (plain / kSealChunk + 1) * kSealTag + plain; }

uint64_t max_plain_for_sealed(uint64_t cap) {
    if (cap < kSealHeader + kSealTag) return 0;
    uint64_t p = cap - kSealHeader - kSealTag * (cap / kSealChunk + 1);
    while (p > 0 && sealed_size(p) > cap) --p;
    while (sealed_size(p + 1) <= cap) ++p;
    return p;
}

bool parse_report_state(const std::string& text, ReportState* out) {
    ReportState s;
    bool have_created = false;
    for (const auto& kv : key_values(text)) {
        int64_t v = 0;
        if (kv.first == "consent") {
            if (kv.second != "pending" && kv.second != "granted" && kv.second != "denied") return false;
            s.consent = kv.second;
        } else if (kv.first == "attempts" || kv.first == "prompts" || kv.first == "created_unix" || kv.first == "next_attempt_unix") {
            if (!parse_i64(kv.second, &v)) return false;
            if (kv.first == "attempts") s.attempts = (int)v;
            else if (kv.first == "prompts") s.prompts = (int)v;
            else if (kv.first == "created_unix") { s.created_unix = v; have_created = true; }
            else s.next_attempt_unix = v;
        }
    }
    if (!have_created) return false;
    *out = s;
    return true;
}

std::string serialize_report_state(const ReportState& s) {
    char b[256];
    std::snprintf(b, sizeof b, "attempts=%d\nprompts=%d\ncreated_unix=%lld\nnext_attempt_unix=%lld\nconsent=%s\n",
                  s.attempts, s.prompts, (long long)s.created_unix, (long long)s.next_attempt_unix, one_line(s.consent).c_str());
    return b;
}

bool parse_report_record(const std::string& text, ReportRecord* out) {
    ReportRecord r;
    bool have_kind = false;
    for (const auto& kv : key_values(text)) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        int64_t n = 0;
        if (k == "report_id") r.report_id = v;
        else if (k == "session_id") r.session_id = v;
        else if (k == "build_id") r.build_id = v;
        else if (k == "kind") { if (!kind_from_name(v, &r.kind)) return false; have_kind = true; }
        else if (k == "hints") {
            size_t p = 0;
            while (p < v.size()) {
                size_t c = v.find(',', p);
                if (c == std::string::npos) c = v.size();
                Kind h;
                if (!kind_from_name(v.substr(p, c - p), &h)) return false;
                r.hints.push_back(h);
                p = c + 1;
            }
        }
        else if (k == "features") r.features_json = v;
        else if (k == "started_unix") { if (!parse_i64(v, &n)) return false; r.started_unix = n; }
        else if (k == "uptime_s") { if (!parse_i64(v, &n)) return false; r.uptime_s = n; }
        else if (k == "online") r.online = (v == "1");
        else if (k == "fw") r.fw = v;
        else if (k == "redactions") { if (!parse_i64(v, &n)) return false; r.redactions = (int)n; }
        else if (k == "dump") r.dump = v;
        else if (k == "artifact") {
            // "<name> <file> <bytes>"
            const size_t a = v.find(' '), b = v.find(' ', a == std::string::npos ? a : a + 1);
            if (a == std::string::npos || b == std::string::npos) return false;
            ArtifactFile f;
            f.name = v.substr(0, a);
            f.file = v.substr(a + 1, b - a - 1);
            if (!parse_i64(v.substr(b + 1), &n) || n < 0) return false;
            f.bytes = (uint64_t)n;
            r.artifacts.push_back(f);
        }
    }
    if (r.report_id.empty() || !have_kind) return false;
    *out = r;
    return true;
}

std::string serialize_report_record(const ReportRecord& r) {
    std::string o;
    auto kv = [&o](const char* k, const std::string& v) { o += k; o += '='; o += one_line(v); o += '\n'; };
    kv("report_id", r.report_id);
    kv("session_id", r.session_id);
    kv("build_id", r.build_id);
    kv("kind", kind_name(r.kind));
    std::string hints;
    for (Kind h : r.hints) { if (!hints.empty()) hints += ','; hints += kind_name(h); }
    kv("hints", hints);
    kv("features", r.features_json);
    kv("started_unix", std::to_string((long long)r.started_unix));
    kv("uptime_s", std::to_string((long long)r.uptime_s));
    kv("online", r.online ? "1" : "0");
    kv("fw", r.fw);
    kv("redactions", std::to_string(r.redactions));
    kv("dump", r.dump);
    for (const ArtifactFile& a : r.artifacts)
        kv("artifact", a.name + " " + a.file + " " + std::to_string((unsigned long long)a.bytes));
    return o;
}

Outbox::Outbox(IoApi& io, const std::string& root, const OutboxLimits& limits) : io_(io), root_(root), limits_(limits) {}

bool Outbox::create(const CollectInputs& in, const SecretPatterns& p, ReportRecord* out, std::string* err) {
    const std::string d = dir(in.report_id);
    DirEntry de;
    if (in.report_id.empty() || in.report_id.find('/') != std::string::npos) { *err = "bad report id"; return false; }
    if (in.evidence.kind == Kind::None) { *err = "no evidence to report"; return false; }
    if (!io_.stat(root_, &de) && !io_.mkdir(root_)) { *err = "cannot create " + root_; return false; }
    if (io_.stat(d, &de)) { *err = "report already exists"; return false; }
    if (!io_.mkdir(d)) { *err = "cannot create " + d; return false; }
    auto fail = [this, &d, err](const std::string& why) {
        *err = why;
        std::vector<DirEntry> es;
        if (io_.list_dir(d, &es)) for (const DirEntry& e : es) io_.remove(d + "/" + e.name);
        io_.remove(d);
        return false;
    };

    ReportRecord rec;
    rec.report_id = in.report_id;
    rec.session_id = in.session.session_id;
    rec.build_id = in.session.build_id;
    rec.kind = in.evidence.kind;
    rec.hints = in.evidence.hints;
    rec.features_json = in.evidence.features_json;
    rec.started_unix = in.session.started_unix;
    rec.uptime_s = in.details.uptime_s;
    rec.online = in.details.online;
    rec.fw = in.details.fw;
    const size_t ext = p.max_match_len();

    // ---- boot_progress: head + [fault window] + tail, or the whole file.
    // Read in three bounded steps (spec 4.9: 1 MiB of heap at most); the
    // copy itself is at most 320 KiB sealed, and it is the largest buffer.
    if (!in.evidence.progress_path.empty()) {
        const std::string& path = in.evidence.progress_path;
        const uint64_t cap = max_plain_for_sealed(kSealedCapBootProgress);
        std::string text;
        if (!io_.stat(path, &de)) return fail("boot_progress vanished");
        const uint64_t size = de.size;
        if (size <= cap) {
            if (!io_.read_file(path, &text, (size_t)cap)) return fail("cannot read boot_progress");
            rec.redactions += redact_count_in(&text, p, 0, text.size());
        } else {
            const uint64_t gap_lo = kProgressHeadBytes, gap_hi = size - kProgressTailBytes;
            // Each omission marker is at most as long as the one for `size`.
            const uint64_t fixed = kProgressHeadBytes + kProgressTailBytes + 2 * omitted(size).size();
            const uint64_t budget = cap > fixed ? cap - fixed : 0;

            std::string head;
            if (!io_.read_head_tail(path, kProgressHeadBytes + ext, 0, &head) || head.size() != kProgressHeadBytes + ext)
                return fail("cannot read boot_progress head");
            rec.redactions += redact_count_in(&head, p, 0, kProgressHeadBytes);
            head.resize(kProgressHeadBytes);

            std::string window;
            uint64_t win_lo = gap_lo, win_hi = gap_lo;
            {
                // No more than `budget` bytes on either side of the fault
                // line can reach the copy, so no more are kept, plus `ext`
                // bytes so that a secret crossing the cut is still matched.
                FaultWindow fw((size_t)budget + ext, (size_t)budget + ext, (size_t)kFaultWindowLines);
                io_.read_stream(path, 64 * 1024, window_cb, &fw);   // stops early once the window is complete
                fw.finish();
                if (budget > 0 && fw.found() && fw.fault_off() >= gap_lo && fw.fault_off() < gap_hi) {
                    uint64_t lo = std::max(fw.begin(), gap_lo), hi = std::min(fw.end(), gap_hi);
                    if (hi - lo > budget) {
                        lo = std::max(lo, fw.fault_off() > budget / 2 ? fw.fault_off() - budget / 2 : 0);
                        hi = std::min(hi, lo + budget);
                        lo = std::max(std::max(fw.begin(), gap_lo), hi - budget);
                    }
                    rec.redactions += redact_count_in(&fw.text(), p, (size_t)(lo - fw.begin()), (size_t)(hi - fw.begin()));
                    window.assign(fw.text(), (size_t)(lo - fw.begin()), (size_t)(hi - lo));
                    win_lo = lo;
                    win_hi = hi;
                }
            }

            const std::string mark1 = omitted((window.empty() ? gap_hi : win_lo) - gap_lo);
            const std::string mark2 = window.empty() ? std::string() : omitted(gap_hi - win_hi);
            text.reserve(head.size() + mark1.size() + window.size() + mark2.size() + kProgressTailBytes);
            text += head;
            std::string().swap(head);
            text += mark1;
            text += window;
            std::string().swap(window);
            text += mark2;

            std::string tail;
            if (!io_.read_head_tail(path, 0, kProgressTailBytes + ext, &tail) || tail.size() != kProgressTailBytes + ext)
                return fail("cannot read boot_progress tail");
            rec.redactions += redact_count_in(&tail, p, ext, tail.size());
            text.append(tail, ext, std::string::npos);
        }
        if (!io_.write_file(d + "/boot_progress.txt", text)) return fail("cannot write boot_progress");
        rec.artifacts.push_back(ArtifactFile{"boot_progress", "boot_progress.txt", text.size()});
    }

    // ---- crash.log: its end (the last lines name how far the run got).
    if (!in.evidence.crash_log_path.empty()) {
        const uint64_t cap = max_plain_for_sealed(kSealedCapCrashLog);
        std::string t;
        if (!io_.read_head_tail(in.evidence.crash_log_path, 0, (size_t)cap + ext, &t)) return fail("cannot read crash.log");
        const size_t keep_from = t.size() > cap ? t.size() - (size_t)cap : 0;
        rec.redactions += redact_count_in(&t, p, keep_from, t.size());
        t.erase(0, keep_from);
        if (!io_.write_file(d + "/crash_log.txt", t)) return fail("cannot write crash_log");
        rec.artifacts.push_back(ArtifactFile{"crash_log", "crash_log.txt", t.size()});
    }

    // ---- Crash.txt: its start (summary, halting thread's stack).
    if (!in.evidence.crash_txt_path.empty()) {
        const uint64_t cap = max_plain_for_sealed(kSealedCapCrashTxt);
        std::string t;
        if (!io_.read_head_tail(in.evidence.crash_txt_path, (size_t)cap + ext, 0, &t)) return fail("cannot read Crash.txt");
        rec.redactions += redact_count_in(&t, p, 0, std::min<size_t>(t.size(), (size_t)cap));
        if (t.size() > cap) t.resize((size_t)cap);
        if (!io_.write_file(d + "/crash_txt.txt", t)) return fail("cannot write crash_txt");
        rec.artifacts.push_back(ArtifactFile{"crash_txt", "crash_txt.txt", t.size()});
    }

    // ---- The dump is moved last, once everything else is safely written.
    bool move = false;
    uint64_t dump_bytes = 0;
    rec.dump = "none";
    if (!in.evidence.dump_path.empty() && io_.stat(in.evidence.dump_path, &de)) {
        dump_bytes = de.size;
        if (dump_bytes > max_plain_for_sealed(kSealedCapDump)) {
            rec.dump = "too_large";
        } else {
            move = true;
            rec.dump = in.evidence.dump_withheld ? "withheld" : "included";
            if (!in.evidence.dump_withheld) rec.artifacts.push_back(ArtifactFile{"dump", "dump.psp2dmp", dump_bytes});
        }
    }
    ReportState st;
    st.created_unix = in.now_unix;
    if (!io_.write_file(d + "/state.txt", serialize_report_state(st))) return fail("cannot write state");
    if (!io_.write_file(d + "/evidence.txt", serialize_report_record(rec))) return fail("cannot write evidence");
    if (move && !io_.rename(in.evidence.dump_path, d + "/dump.psp2dmp")) {
        rec.dump = "move_failed";
        rec.artifacts.erase(std::remove_if(rec.artifacts.begin(), rec.artifacts.end(),
                                           [](const ArtifactFile& a) { return a.name == "dump"; }),
                            rec.artifacts.end());
        io_.remove(d + "/evidence.txt");
        if (!io_.write_file(d + "/evidence.txt", serialize_report_record(rec))) return fail("cannot rewrite evidence");
    }
    if (!in.evidence.crash_txt_path.empty()) io_.remove(in.evidence.crash_txt_path);
    *out = rec;
    return true;
}

namespace {

struct Held { std::string id; int64_t created; uint64_t bytes; };

// Oldest first, then by id. Insertion sort: an outbox holds a handful of
// reports, and std::sort would add kilobytes of .text (spec 4.9 budget).
void sort_oldest_first(std::vector<Held>* v) {
    for (size_t i = 1; i < v->size(); ++i) {
        for (size_t j = i; j > 0; --j) {
            const Held& a = (*v)[j - 1];
            const Held& b = (*v)[j];
            if (a.created < b.created || (a.created == b.created && !(b.id < a.id))) break;
            std::swap((*v)[j - 1], (*v)[j]);
        }
    }
}

}  // namespace

std::vector<std::string> Outbox::list() {
    std::vector<Held> rs;
    std::vector<DirEntry> es;
    if (!io_.list_dir(root_, &es)) return std::vector<std::string>();
    for (const DirEntry& e : es) {
        if (!e.is_dir) continue;
        ReportState st;
        if (read_state(e.name, &st, nullptr)) rs.push_back(Held{e.name, st.created_unix, 0});
    }
    sort_oldest_first(&rs);
    std::vector<std::string> ids;
    for (const Held& r : rs) ids.push_back(r.id);
    return ids;
}

bool Outbox::load(const std::string& id, ReportRecord* rec, ReportState* st) {
    std::string e;
    return io_.read_file(dir(id) + "/evidence.txt", &e, kRecordMax) && parse_report_record(e, rec) &&
           read_state(id, st, nullptr);
}

bool Outbox::read_state(const std::string& id, ReportState* st, bool* from_new) {
    const std::string path = dir(id) + "/state.txt";
    std::string t;
    DirEntry de;
    if (from_new) *from_new = false;
    if (io_.stat(path, &de)) return io_.read_file(path, &t, kStateMax) && parse_report_state(t, st);
    // save_state stopped between removing state.txt and renaming the new
    // one: state.txt.new is complete (it is written before anything is
    // removed).
    if (!io_.read_file(path + ".new", &t, kStateMax) || !parse_report_state(t, st)) return false;
    if (from_new) *from_new = true;
    return true;
}

bool Outbox::save_state(const std::string& id, const ReportState& st) {
    const std::string path = dir(id) + "/state.txt", tmp = path + ".new";
    DirEntry de;
    if (io_.stat(tmp, &de)) io_.remove(tmp);
    if (!io_.write_file(tmp, serialize_report_state(st))) return false;
    if (io_.stat(path, &de) && !io_.remove(path)) return false;
    return io_.rename(tmp, path);
}

bool Outbox::remove(const std::string& id) {
    const std::string d = dir(id);
    std::vector<DirEntry> es;
    if (!io_.list_dir(d, &es)) return false;
    for (const DirEntry& e : es) io_.remove(d + "/" + e.name);
    return io_.remove(d);
}

uint64_t Outbox::report_bytes(const std::string& id) {
    std::vector<DirEntry> es;
    uint64_t total = 0;
    if (io_.list_dir(dir(id), &es))
        for (const DirEntry& e : es) if (!e.is_dir) total += e.size;
    return total;
}

int Outbox::enforce(int64_t now_unix) {
    int removed = 0;
    std::vector<DirEntry> es;
    if (!io_.list_dir(root_, &es)) return 0;
    std::vector<Held> keep;
    for (const DirEntry& e : es) {
        if (!e.is_dir) continue;
        ReportState st;
        bool from_new = false;
        const bool valid = read_state(e.name, &st, &from_new);
        if (valid && from_new) io_.rename(dir(e.name) + "/state.txt.new", dir(e.name) + "/state.txt");
        // The dialog bound only concerns a report still waiting for an
        // answer: a report the player agreed to send stays until 3 failed
        // sends or 7 days (spec 4.8), even if it took 3 dialogs to agree.
        const bool unanswered_too_often = st.consent == "pending" && st.prompts >= limits_.max_prompts;
        if (!valid || now_unix - st.created_unix > limits_.max_age_s || unanswered_too_often ||
            st.attempts >= limits_.max_attempts || st.consent == "denied") {
            if (remove(e.name)) ++removed;
            continue;
        }
        keep.push_back(Held{e.name, st.created_unix, report_bytes(e.name)});
    }
    sort_oldest_first(&keep);
    uint64_t total = 0;
    for (const Held& r : keep) total += r.bytes;
    size_t first = 0;
    while (first < keep.size() &&
           ((int)(keep.size() - first) > limits_.max_reports || total > limits_.max_total_bytes)) {
        if (remove(keep[first].id)) ++removed;
        total -= keep[first].bytes;
        ++first;
    }
    return removed;
}

}  // namespace d2cr
