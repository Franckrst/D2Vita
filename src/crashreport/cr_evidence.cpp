// src/crashreport/cr_evidence.cpp — evidence left by the previous session (spec §4.1).
#include "crashreport/cr_evidence.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "crashreport/cr_addr.h"
#include "crashreport/cr_crashtxt_parse.h"
#include "crashreport/cr_json.h"
#include "crashreport/cr_progress_parse.h"
#include "crashreport/cr_psp2dmp.h"

namespace d2cr {

namespace {

bool starts_with(const std::string& s, const char* p) {
    size_t i = 0;
    for (; p[i]; ++i) if (i >= s.size() || s[i] != p[i]) return false;
    return true;
}

// "psp2core-<epoch>-0x<id>-<name>.psp2dmp" (the name given by the system).
bool parse_dump_name(const std::string& name, int64_t* epoch) {
    static const char kPre[] = "psp2core-", kSuf[] = ".psp2dmp";
    if (!starts_with(name, kPre) || name.size() <= sizeof kPre + sizeof kSuf) return false;
    if (name.compare(name.size() - (sizeof kSuf - 1), sizeof kSuf - 1, kSuf) != 0) return false;
    size_t i = sizeof kPre - 1;
    int64_t e = 0;
    size_t digits = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9' && digits < 18) { e = e * 10 + (name[i] - '0'); ++i; ++digits; }
    if (digits == 0 || name.compare(i, 3, "-0x") != 0) return false;
    i += 3;
    size_t hex = 0;
    while (i < name.size() && std::isxdigit((unsigned char)name[i])) { ++i; ++hex; }
    if (hex == 0 || i >= name.size() || name[i] != '-') return false;
    *epoch = e;
    return true;
}

// Character sets of contract/schemas/claim.v1.schema.json: values outside
// them would make the whole claim invalid, so they are made safe here.
std::string safe(const std::string& s, const char* extra, size_t max) {
    std::string r;
    for (char c : s) {
        if (r.size() >= max) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        (c && std::strchr(extra, c) != nullptr);
        r += ok ? c : '_';
    }
    return r;
}

// "<basename>:<line>" matching ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}:(0|[1-9][0-9]{0,9})$, or "".
std::string safe_location(const std::string& loc) {
    const size_t colon = loc.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= loc.size()) return std::string();
    std::string file = safe(loc.substr(0, colon), ".-", 64);
    if (file[0] == '.' || file[0] == '-') file[0] = '_';
    const std::string line = loc.substr(colon + 1);
    if (line.size() > 10 || (line.size() > 1 && line[0] == '0')) return std::string();
    for (char c : line) if (c < '0' || c > '9') return std::string();
    return file + ":" + line;
}

void addr_array(JsonWriter& w, const std::vector<Addr>& v) {
    w.begin_array();
    for (const Addr& a : v) w.str(format_addr(a));
    w.end_array();
}

// The reader already gives the contract's HostAddress shape (cr_psp2dmp.h):
// a module name is never rewritten, since a rewritten name would be a
// spelling of the module that the dump does not use.
void host_addr(JsonWriter& w, const HostAddr& a) {
    char off[16];
    std::snprintf(off, sizeof off, "0x%x", (unsigned)a.offset);
    w.begin_object().key("region").str(a.region).key("module").str(a.module).key("offset").str(off).end_object();
}

// Exit codes the schedulers give a thread that faulted. The emulator names the
// fault when it can (divide-by-zero, illegal instruction) and falls back to the
// access-violation code when it cannot, so this is a family, not one value —
// matching only the fallback classified a named fault as an ordinary exit.
constexpr bool is_fault_exit_code(uint32_t c) {
    return c == 0xC0000005u     // EXCEPTION_ACCESS_VIOLATION (unnamed fault)
        || c == 0xC0000094u     // EXCEPTION_INT_DIVIDE_BY_ZERO
        || c == 0xC000001Du;    // EXCEPTION_ILLEGAL_INSTRUCTION
}

struct Abnormal {
    int prio = 99;
    std::string reason, import;
    bool has_code = false;
    uint32_t code = 0;
    std::vector<Addr> frames;
    void consider(int p, const char* why, bool with_code, uint32_t c, const std::string& imp, const std::vector<Addr>& fr) {
        if (p >= prio) return;
        prio = p;
        reason = why;
        has_code = with_code;
        code = c;
        import = imp;
        frames = fr;
    }
};

}  // namespace

Evidence build_evidence(IoApi& io, const SessionRecord& prev, int64_t now_unix, const std::string& dumps_dir,
                        const SecretPatterns& patterns, EvidenceDetails* details) {
    Evidence ev;
    EvidenceDetails det;
    std::string features[6];
    bool found[6] = {false, false, false, false, false, false};
    const std::vector<Addr> no_frames;
    auto mark = [&found, &features](Kind k, const std::string& json) {
        found[(int)k] = true;
        features[(int)k] = json;
    };

    // ---- boot_progress: guest fault, abnormal stop lines, heartbeats. The
    // whole file is streamed (it can grow by ~100 KiB per hour of play) and
    // never held in memory.
    ProgressFacts pf;
    DirEntry de;
    if (!prev.progress_path.empty() && io.stat(prev.progress_path, &de) && !de.is_dir &&
        parse_progress_file(io, prev.progress_path, prev.game_base, &pf)) {
        ev.progress_path = prev.progress_path;
        det.uptime_s = pf.last_uptime_s;
        det.online = pf.online;
        det.native_fault_line = pf.native_fault_line;
    }

    // ---- Crash.txt (halt) and crash.log.
    if (!prev.write_root.empty()) {
        const std::string log = prev.write_root + "/crash.log";
        if (io.stat(log, &de) && !de.is_dir) ev.crash_log_path = log;
        const std::string ct = prev.write_root + "/Crash.txt";
        if (io.stat(ct, &de) && !de.is_dir && de.mtime_unix >= prev.started_unix) {
            std::string text;
            if (!io.read_head_tail(ct, kCrashTxtParseBytes, 0, &text)) text.clear();
            const HaltFacts h = parse_crash_txt(text, prev.game_base);
            JsonWriter w;
            w.begin_object().key("code").num(h.is_halt ? h.code : 0).key("location");
            const std::string loc = safe_location(h.location);
            if (loc.empty()) w.null(); else w.str(loc);
            w.key("frames");
            addr_array(w, h.frames);
            w.end_object();
            mark(Kind::Halt, w.out());
            ev.crash_txt_path = ct;
        }
    }

    // ---- NATIVE FAULT (guest thread fault under the native scheduler). The
    // log block carries no exception code (the box86 error kind of
    // cpu_box86.cpp:1339-1345 only reaches stdout), so the code is unknown.
    if (pf.native_fault) {
        JsonWriter w;
        w.begin_object()
            .key("exception").null()
            .key("thread").str(pf.fault_main ? "main" : "worker")
            .key("eip").str(format_addr(pf.fault_eip_addr))
            .key("frames");
        addr_array(w, pf.fault_frames);
        w.end_object();
        mark(Kind::GuestFault, w.out());
    }

    // ---- Abnormal stop, from the log and from the session record. Reasons
    // are the contract's (claim.v1 AbnormalExitFeatures): TerminateProcess
    // with a non-zero code counts as exit_process; a main thread that ended
    // with one of the scheduler's fault codes is main_thread_fault, any other
    // non-zero main exit code exit_process. The code itself is reported as-is,
    // so a divide-by-zero is distinguishable from an access violation.
    Abnormal ab;
    auto main_exit = [&ab, &no_frames](uint32_t code) {
        if (code == 0) return;
        if (is_fault_exit_code(code)) ab.consider(5, "main_thread_fault", true, code, "", no_frames);
        else ab.consider(6, "exit_process", true, code, "", no_frames);
    };
    if (pf.unshimmed) ab.consider(0, "unshimmed_import", false, 0, pf.unshimmed_import, no_frames);
    if (pf.raise_exception) ab.consider(1, "raise_exception", true, pf.raise_code, "", no_frames);
    if (pf.fatal_app_exit) ab.consider(2, "fatal_app_exit", false, 0, "", no_frames);
    if (pf.terminate_process && pf.exit_code != 0) ab.consider(3, "exit_process", true, pf.exit_code, "", pf.exit_chain);
    if (pf.exit_process && pf.exit_code != 0) ab.consider(4, "exit_process", true, pf.exit_code, "", pf.exit_chain);
    if (pf.sched_stopped && pf.has_main_exit) main_exit(pf.main_exit);
    // The session's stop_reason may hold the scheduler reason or the
    // controlled-stop reason of the shims (g_stopReason: "UNSHIMMED <tag>",
    // "FatalAppExitA", "RaiseException 0x%08x -> arret controle",
    // "ExitProcess", "TerminateProcess").
    const std::string& sr = prev.stop_reason;
    if (starts_with(sr, "UNSHIMMED ")) {
        ab.consider(0, "unshimmed_import", false, 0, sr.substr(10), no_frames);
    } else if (starts_with(sr, "RaiseException")) {
        unsigned code = 0;
        const bool has = std::sscanf(sr.c_str(), "RaiseException 0x%x", &code) == 1;
        ab.consider(1, "raise_exception", has, code, "", no_frames);
    } else if (starts_with(sr, "FatalAppExit")) {
        ab.consider(2, "fatal_app_exit", false, 0, "", no_frames);
    } else if (sr == "TerminateProcess" || sr == "ExitProcess") {
        if (prev.has_main_exit && prev.main_exit != 0) ab.consider(sr == "TerminateProcess" ? 3 : 4, "exit_process", true, prev.main_exit, "", no_frames);
    }
    // Any other reason ("shutdown requested", "main exited", a fault string)
    // is judged by the main exit code alone.
    if (prev.has_main_exit) main_exit(prev.main_exit);
    if (ab.prio < 99) {
        JsonWriter w;
        w.begin_object().key("reason").str(ab.reason).key("code");
        if (ab.has_code) w.unum(ab.code); else w.null();
        w.key("import");
        const std::string imp = safe(ab.import, ".!@?$#", 128);
        if (imp.empty()) w.null(); else w.str(imp);
        w.key("frames");
        addr_array(w, ab.frames);
        w.end_object();
        mark(Kind::AbnormalExit, w.out());
    }

    // ---- Hang detection was removed (see cr_evidence.h's file comment):
    // the general watchdog already covers a genuine freeze without a
    // dedicated crash-report path. This used to mark(Kind::Hang, ...) when
    // prev.state == "running" and pf.stalled_beats reached a threshold; a
    // stalled heartbeat with no other evidence now simply falls through to
    // Kind::None below — no evidence collected, nothing reported, nothing
    // sent. pf.stalled_beats/pf.runner_class/pf.has_last_eip/
    // pf.last_eip_addr (cr_progress_parse.h) are still parsed (that parser is
    // general boot_progress.txt fact extraction, unrelated to hang
    // detection) but nothing here reads them any more.

    // ---- System core dump matching the previous session.
    std::vector<DirEntry> entries;
    if (!dumps_dir.empty() && io.list_dir(dumps_dir, &entries)) {
        struct Cand { int64_t epoch; DirEntry e; };
        std::vector<Cand> cands;
        for (const DirEntry& e : entries) {
            int64_t epoch;
            if (!e.is_dir && parse_dump_name(e.name, &epoch)) cands.push_back(Cand{epoch, e});
        }
        // Newest first, then by name. Insertion sort: few dumps sit in
        // ux0:data, and std::sort would add kilobytes of .text (spec 4.9).
        for (size_t i = 1; i < cands.size(); ++i) {
            for (size_t j = i; j > 0; --j) {
                const Cand& a = cands[j - 1];
                const Cand& b = cands[j];
                if (a.epoch > b.epoch || (a.epoch == b.epoch && !(a.e.name < b.e.name))) break;
                std::swap(cands[j - 1], cands[j]);
            }
        }
        DumpReadOptions peek_opt;
        peek_opt.notes_only = true;
        const SecretPatterns none;
        for (const Cand& c : cands) {
            const std::string path = dumps_dir + "/" + c.e.name;
            const DumpFacts peek = read_psp2dmp(io, path, prev, none, peek_opt);
            if (!peek.ok) continue;
            const bool match = peek.has_stamp
                ? peek.stamp_session_id == prev.session_id
                : (c.epoch >= prev.started_unix && c.epoch <= now_unix && peek.app_title == kD2VitaTitleId);
            if (!match) continue;
            const DumpFacts full = read_psp2dmp(io, path, prev, patterns);
            if (!full.ok) continue;
            const bool withheld = full.secret_hits > 0;
            JsonWriter w;
            char code[16];
            std::snprintf(code, sizeof code, "0x%x", (unsigned)full.stop_reason);
            bool in_thread_info = false;
            for (const DumpThread& t : full.threads) in_thread_info = in_thread_info || t.uid == full.tid;
            w.begin_object().key("stop_reason");
            if (in_thread_info) w.str(code); else w.null();
            w.key("thread_name");
            if (in_thread_info) {
                std::string name;
                for (char ch : full.thread_name) if (name.size() < 32) name += (ch >= 0x20 && ch <= 0x7e) ? ch : '?';
                w.str(name);
            } else {
                w.null();
            }
            w.key("pc");
            host_addr(w, full.pc_addr);
            w.key("lr");
            host_addr(w, full.lr_addr);
            w.key("guest_frames").begin_array();
            for (size_t i = 0; i < full.guest_chain.size() && i < 8; ++i)
                w.str(format_addr(guest_addr(full.guest_chain[i].ret, prev.game_base, std::vector<GuestModule>())));
            w.end_array().key("redaction").str(withheld ? "withheld" : "clean").end_object();
            mark(Kind::HostFault, w.out());
            ev.dump_path = path;
            ev.dump_withheld = withheld;
            det.fw = full.fw;
            det.dump_epoch = c.epoch;
            det.dump_bytes = c.e.size;
            det.dump_by_stamp = peek.has_stamp;
            break;
        }
    }

    for (int k = (int)Kind::HostFault; k > (int)Kind::None; --k) {
        if (!found[k]) continue;
        if (ev.kind == Kind::None) {
            ev.kind = (Kind)k;
            ev.features_json = features[k];
        } else {
            ev.hints.push_back((Kind)k);
        }
    }
    if (details) *details = det;
    return ev;
}

}  // namespace d2cr
