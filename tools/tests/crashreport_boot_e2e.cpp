// tools/tests/crashreport_boot_e2e.cpp — end-to-end proof of the properties
// specific to the crash-report boot wiring (cr_boot.cpp), which
// tools/tests/crashreport_transport_test.cpp's --live-upload mode does not
// cover: that one fabricates a ReportRecord directly to test the UPLOAD
// state machine. This one drives the EVIDENCE side for real
// (build_evidence + Outbox::create, exactly as d2cr_boot_collect() calls
// them) from a genuine boot_progress.txt fixture, then the same Uploader
// cr_boot.cpp's upload thread uses, against a real API.
//
// It is a parallel harness, not cr_boot.cpp's own compiled code: it links
// the platform-independent library (cr_evidence.cpp, cr_outbox.cpp,
// cr_upload.cpp, ...) through the POSIX IoApi/NetApi, the same way the host
// tests already do, and calls d2cr::should_suppress_dialog /
// d2cr::crashreport_disabled from cr_boot.h — the two rules cr_boot.h
// documents as shared between cr_boot.cpp and cr_consent_vita.cpp — so the
// gating decisions are the ACTUAL functions, not a reimplementation of them.
// The Vita-only glue itself (sceIo/sceNet/sceKernel calls, the upload
// thread's bounded stop-and-join) can only be proven by VitaSDK compilation
// succeeding (tools/build_rt_boot_vpk.sh) and by code inspection — actually
// exercising it needs real hardware or Vita3K, which this harness does not
// cover.
//
// Driven by tools/tests/run_crashreport_boot_e2e.py.
#include "crashreport/cr_boot.h"
#include "crashreport/cr_claim.h"
#include "crashreport/cr_evidence.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_net.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_types.h"
#include "crashreport/cr_upload.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

using namespace d2cr;

namespace {

std::string from_hex(const std::string& hex) {
    std::string out(hex.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned v = 0;
        std::sscanf(hex.c_str() + 2 * i, "%2x", &v);
        out[i] = (char)v;
    }
    return out;
}

std::string arg(int argc, char** argv, const char* name, const std::string& def = "") {
    const std::string n = name;
    for (int i = 1; i + 1 < argc; ++i)
        if (n == argv[i]) return argv[i + 1];
    return def;
}

int64_t now_unix_cb(void*) { return (int64_t)::time(nullptr); }
bool random_bytes_cb(uint8_t* out, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(std::rand() & 0xFF);
    return true;
}
bool never_stop_cb(void*) { return false; }

// ---- init-session: writes reports/session.txt as a genuinely crashed prior
// run would have left it (state=running, never updated to "exited") --------
int cmd_init_session(int argc, char** argv) {
    const std::string root = arg(argc, argv, "--root");
    auto io = make_posix_io();
    io->mkdir(root);
    io->mkdir(root + "/reports");
    SessionRecord s;
    s.session_id = arg(argc, argv, "--session-id");
    s.build_id = arg(argc, argv, "--build-id", "0.1.0+000000000000");
    s.started_unix = std::atoll(arg(argc, argv, "--started-unix", "0").c_str());
    s.write_root = root;
    s.progress_path = root + "/boot_progress.txt";
    s.state = "running";
    if (!io->write_file(root + "/reports/session.txt", serialize_session(s))) {
        std::printf("FAIL: cannot write session.txt\n");
        return 1;
    }
    std::printf("OK: wrote prior session %s (state=running, progress_path=%s)\n", s.session_id.c_str(),
                s.progress_path.c_str());
    return 0;
}

// ---- collect: the d2cr_boot_collect() sequence, called BEFORE the caller
// (the driving shell script) performs the boot_progress.txt rotation -------
int cmd_collect(int argc, char** argv) {
    const std::string root = arg(argc, argv, "--root");
    const int64_t now = std::atoll(arg(argc, argv, "--now").c_str());
    const std::string build_id = arg(argc, argv, "--build-id", "0.1.0+000000000000");

    auto io = make_posix_io();
    io->mkdir(root);
    io->mkdir(root + "/reports");

    if (crashreport_disabled(arg(argc, argv, "--d2-crashreport", "").c_str())) {
        std::printf("DISABLED: D2_CRASHREPORT=0, nothing read, nothing written\n");
        return 0;
    }

    std::string prevText;
    SessionRecord prev;
    if (!io->read_file(root + "/reports/session.txt", &prevText, 1 << 20) || !parse_session(prevText, &prev)) {
        std::printf("NOSESSION: no readable previous session.txt\n");
    } else {
        SecretPatterns patterns = build_patterns("", {});
        EvidenceDetails details;
        Evidence ev = build_evidence(*io, prev, now, root, patterns, &details);
        if (ev.kind == Kind::None) {
            std::printf("NOEVIDENCE: session %s carried no evidence\n", prev.session_id.c_str());
        } else {
            uint8_t rnd[10];
            random_bytes_cb(rnd, sizeof rnd, nullptr);
            const std::string report_id = new_ulid(now * 1000, rnd);
            Outbox outbox(*io, root + "/reports/outbox");
            CollectInputs in;
            in.report_id = report_id;
            in.session = prev;
            in.evidence = ev;
            in.details = details;
            in.now_unix = now;
            ReportRecord rec;
            std::string err;
            if (outbox.create(in, patterns, &rec, &err)) {
                std::printf("CREATED: report %s kind=%s from session %s (%zu artifact(s))\n", report_id.c_str(),
                            kind_name(ev.kind), prev.session_id.c_str(), rec.artifacts.size());
            } else {
                std::printf("FAIL: outbox refused the report (%s)\n", err.c_str());
            }
            outbox.enforce(now);
        }
    }

    SessionRecord cur;
    uint8_t sid[16];
    random_bytes_cb(sid, sizeof sid, nullptr);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", sid[i]);
    cur.session_id = hex;
    cur.build_id = build_id;
    cur.started_unix = now;
    cur.write_root = root;
    cur.progress_path = root + "/boot_progress.txt";
    cur.state = "running";
    io->write_file(root + "/reports/session.txt", serialize_session(cur));
    std::printf("OK: new session %s started\n", cur.session_id.c_str());
    return 0;
}

// ---- list: prints one line per report still in the outbox -----------------
int cmd_list(int argc, char** argv) {
    const std::string root = arg(argc, argv, "--root");
    auto io = make_posix_io();
    Outbox outbox(*io, root + "/reports/outbox");
    for (const std::string& id : outbox.list()) {
        ReportRecord rec;
        ReportState st;
        if (!outbox.load(id, &rec, &st)) continue;
        std::printf("REPORT %s kind=%s consent=%s attempts=%d prompts=%d\n", id.c_str(), kind_name(rec.kind),
                    st.consent.c_str(), st.attempts, st.prompts);
    }
    return 0;
}

// ---- after-present: the d2cr_after_present() sequence ---------------------
int cmd_after_present(int argc, char** argv) {
    const std::string root = arg(argc, argv, "--root");
    const std::string mode = arg(argc, argv, "--mode", "none");   // always | denied | script | none
    auto io = make_posix_io();
    Outbox outbox(*io, root + "/reports/outbox");

    if (mode == "script") {
        // should_suppress_dialog is the ACTUAL ABSOLUTE RULE function
        // cr_boot.cpp and cr_consent_vita.cpp both call.
        const bool suppressed = should_suppress_dialog("300:activate", "");
        std::printf("%s: D2SCRIPT non-empty, should_suppress_dialog()=%d -> no dialog, no network touch\n",
                    suppressed ? "OK" : "FAIL", suppressed);
        return suppressed ? 0 : 1;
    }

    std::vector<std::string> ids = outbox.list();
    int granted = 0;
    for (const std::string& id : ids) {
        ReportRecord rec;
        ReportState st;
        if (!outbox.load(id, &rec, &st)) continue;
        if (st.consent != "pending") { if (st.consent == "granted") ++granted; continue; }
        if (mode == "always") { st.consent = "granted"; outbox.save_state(id, st); ++granted; }
        else if (mode == "denied") { outbox.remove(id); }
        // mode == "none": leave pending (simulates a 60s timeout / Later answer)
    }

    if (granted == 0) {
        std::printf("NO NETWORK: nothing consented (spec 4.9)\n");
        return 0;
    }

    const std::string url = arg(argc, argv, "--url");
    const std::string response_pk = arg(argc, argv, "--response-pk");
    const std::string recipient_pk = arg(argc, argv, "--recipient-pk");
    auto net = make_posix_net();
    UploadConfig cfg;
    cfg.base_url = url;
    cfg.install_id = arg(argc, argv, "--install-id");
    cfg.channel = "test";
    cfg.platform_model = "vita";
    UploadKeys keys;
    const std::string pk = from_hex(response_pk), rpk = from_hex(recipient_pk);
    std::memcpy(keys.response_pk, pk.data(), sizeof keys.response_pk);
    std::memcpy(keys.recipient_pk, rpk.data(), sizeof keys.recipient_pk);
    UploadEnv env;
    env.now_unix = now_unix_cb;
    env.random_bytes = random_bytes_cb;
    env.should_stop = never_stop_cb;
    Uploader up(*io, *net, outbox, cfg, keys, env);
    const UploadStats s = up.run();
    std::printf("outcome=%s requests=%d pieces=%d completed=%d deleted=%d failed=%d detail=%s\n",
                report_outcome_name(s.last), s.requests, s.artifacts_stored, s.completed, s.deleted, s.failed,
                up.detail().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "init-session") return cmd_init_session(argc, argv);
    if (cmd == "collect") return cmd_collect(argc, argv);
    if (cmd == "list") return cmd_list(argc, argv);
    if (cmd == "after-present") return cmd_after_present(argc, argv);
    std::printf("usage: %s {init-session|collect|list|after-present} --root DIR ...\n", argv[0]);
    return 2;
}
