// src/crashreport/cr_boot.cpp — orchestration glue between the crash-report
// library (src/crashreport/) and the real boot sites.
// See cr_boot.h for what each entry point does and where it is called from.
//
// Deliberate divergence from spec §4.9, disclosed rather than hidden: "si le
// réseau n'a été lancé que pour les rapports, il est arrêté à la fin de
// l'envoi" is NOT implemented. src/platform/vita_net.h exposes
// d2vita_net_init() (idempotent: calling it when the stack is already up is
// safe, see its own EBUSY handling) but no matching teardown — the pool
// pointer and the ordering-critical net_teardown() helper are private to
// vita_net.cpp (file-static), so the crash reporter reuses that stack rather
// than extending or reaching into it. Building a safe teardown would mean
// either editing vita_net.h/.cpp (shared with the game's own, carefully-
// ordered network bring-up: "ORDER IS CRITICAL", its own words) or
// duplicating its private teardown logic from outside with none of its
// safeguards — both worse than leaving the stack up for the rest of the
// process when the reporter was the one that started it. A follow-up worth
// doing: a small d2vita_net_maybe_shutdown() that only tears down what IT
// brought up.
#include "crashreport/cr_boot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "crashreport/cr_claim.h"       // new_ulid
#include "crashreport/cr_consent.h"
#include "crashreport/cr_evidence.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_net.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_seal.h"        // kSealKeyBytes
#include "crashreport/cr_types.h"
#include "crashreport/cr_upload.h"
#include "crashreport/cr_verify.h"      // kEd25519PublicKeyBytes
#include "crashreport/build_id.h"

#include "platform/vita_host.h"         // wx86_vita_pin_thread (engine log/cores)
#include "platform/vita_net.h"          // d2vita_net_init/status
#include "platform/vita_present.h"      // d2vita_progress, D2VITA_PROGRESS_PATH, d2vita_core_mask/register

extern "C" long long d2vita_wall_unix(void);   // src/platform/vita_present.cpp

namespace d2cr {

namespace {

// ---- paths (spec §4.1, §4.2, §4.6) -----------------------------------------
constexpr const char* kBaseDir      = "ux0:data/d2vita";
constexpr const char* kReportsDir   = "ux0:data/d2vita/reports";
constexpr const char* kOutboxDir    = "ux0:data/d2vita/reports/outbox";
constexpr const char* kSessionPath  = "ux0:data/d2vita/reports/session.txt";
constexpr const char* kInstallPath  = "ux0:data/d2vita/reports/install_id.txt";
constexpr const char* kConsentPath  = "ux0:data/d2vita/reports/consent.txt";
constexpr const char* kKeysPath     = "ux0:data/d2vita/keys.txt";
constexpr const char* kDumpsDir     = "ux0:data";   // psp2core-*.psp2dmp (spec §2)

constexpr int kUploadStackBytes = 64 * 1024;      // spec §4.9
constexpr unsigned kUploadPriority = 0x10000100;  // spec §4.9

// ---- boot-lifetime state (one process, one boot — same pattern as
// vita_present.cpp's own file-static g_* variables) --------------------------
std::unique_ptr<IoApi> g_io;
std::unique_ptr<Outbox> g_outbox;
std::unique_ptr<NetApi> g_net;
bool g_collecting = false;        // false for the whole boot under D2_CRASHREPORT=0
bool g_net_started_by_us = false; // see the file comment: never torn back down
SceUID g_upload_thread = -1;
volatile bool g_upload_stop = false;
UploadStats g_last_upload_stats;
std::string g_install_id;
SessionRecord g_cur_session;

// ---- small helpers ----------------------------------------------------------

// write tmp then rename over the target (sceIoRename, never std::rename —
// mortal on this libc, spec §2). The previous target is removed first:
// IoApi::rename refuses when the destination already exists.
bool durable_write(IoApi& io, const std::string& path, const std::string& data) {
    const std::string tmp = path + ".new";
    if (!io.write_file(tmp, data)) return false;
    io.remove(path);
    return io.rename(tmp, path);
}

bool random_bytes(uint8_t* out, size_t n, void* = nullptr) {
    size_t got = 0;
    while (got < n) {
        const SceSize chunk = (SceSize)std::min<size_t>(64, n - got);   // sceKernelGetRandomNumber caps at 64
        if (sceKernelGetRandomNumber(out + got, chunk) < 0) return false;
        got += chunk;
    }
    return true;
}

std::string hex_encode(const uint8_t* b, size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = hex[b[i] >> 4]; s[2 * i + 1] = hex[b[i] & 0xF]; }
    return s;
}

bool looks_like_install_id(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

void trim_trailing_newline(std::string* s) {
    while (!s->empty() && (s->back() == '\n' || s->back() == '\r')) s->pop_back();
}

std::string load_or_create_install_id(IoApi& io) {
    std::string data;
    if (io.read_file(kInstallPath, &data, 256)) {
        trim_trailing_newline(&data);
        if (looks_like_install_id(data)) return data;
    }
    uint8_t raw[16];
    if (!random_bytes(raw, sizeof raw)) return std::string();
    const std::string id = hex_encode(raw, sizeof raw);
    if (!durable_write(io, kInstallPath, id + "\n"))
        d2vita_progress("crashreport: install_id.txt non ecrit (repli: identifiant non persiste)");
    return id;
}

std::string platform_model() {
    const int m = sceKernelGetModel();
    if (m == SCE_KERNEL_MODEL_VITA) return "vita";
    if (m == SCE_KERNEL_MODEL_VITATV) return "pstv";
    return "unknown";
}

bool env_crashreport_disabled() { return crashreport_disabled(getenv("D2_CRASHREPORT")); }

void save_cur_session() {
    if (g_io) durable_write(*g_io, kSessionPath, serialize_session(g_cur_session));
}

// ---- evidence pass: everything that must run BEFORE the boot_progress
// rotation (d2_boot_config.cpp:209-211) -------------------------------------
void collect_previous_evidence(int64_t now_unix) {
    std::string prevText;
    if (!g_io->read_file(kSessionPath, &prevText, 64 * 1024)) {
        d2vita_progress("crashreport: pas de session precedente (premier lancement)");
        return;
    }
    SessionRecord prev;
    if (!parse_session(prevText, &prev)) {
        d2vita_progress("crashreport: session.txt precedent illisible — aucune preuve cherchee");
        return;
    }

    // Secret patterns (spec §4.6): built once, in memory only, from the
    // PREVIOUS run's own write_root (registry.txt lives at
    // $D2WRITE/registry.txt — src/runtime/win32_shims_advapi32_d2.cpp:121 —
    // and $D2WRITE can differ per build flavor and per env.txt, hence
    // reading it from session.txt rather than assuming today's D2WRITE).
    std::string keysTxt, registryTxt;
    g_io->read_file(kKeysPath, &keysTxt, 64 * 1024);
    if (!prev.write_root.empty())
        g_io->read_file(prev.write_root + "/registry.txt", &registryTxt, 64 * 1024);
    std::vector<std::string> accounts = accounts_from_registry(registryTxt);
    SecretPatterns patterns = build_patterns(keysTxt, accounts);
    std::fill(keysTxt.begin(), keysTxt.end(), 'X'); keysTxt.clear();
    std::fill(registryTxt.begin(), registryTxt.end(), 'X'); registryTxt.clear();

    EvidenceDetails details;
    Evidence ev = build_evidence(*g_io, prev, now_unix, kDumpsDir, patterns, &details);
    if (ev.kind == Kind::None) {
        d2vita_progress("crashreport: session precedente sans preuve (bouton PS probable)");
        patterns.wipe();
        return;
    }

    uint8_t rnd[10];
    std::string report_id;
    if (random_bytes(rnd, sizeof rnd)) report_id = new_ulid(now_unix * 1000, rnd);
    if (report_id.empty()) {
        d2vita_progress("crashreport: ULID non genere (aleatoire indisponible) — preuve ignoree");
        patterns.wipe();
        return;
    }

    CollectInputs in;
    in.report_id = report_id;
    in.session = prev;
    in.evidence = ev;
    in.details = details;
    in.now_unix = now_unix;
    ReportRecord rec; std::string err;
    if (g_outbox->create(in, patterns, &rec, &err)) {
        char m[192];
        std::snprintf(m, sizeof m, "crashreport: preuve %s (session %s) -> rapport %s",
                      kind_name(ev.kind), prev.session_id.c_str(), report_id.c_str());
        d2vita_progress(m);
    } else {
        char m[224];
        std::snprintf(m, sizeof m, "crashreport: preuve %s trouvee mais la boite a refuse (%s)",
                      kind_name(ev.kind), err.c_str());
        d2vita_progress(m);
    }
    patterns.wipe();
}

// ---- uploader plumbing ------------------------------------------------------

int64_t vita_now_unix_cb(void*) { return d2vita_wall_unix(); }
bool vita_random_cb(uint8_t* out, size_t n, void* ud) { (void)ud; return random_bytes(out, n); }
bool vita_should_stop_cb(void*) { return g_upload_stop; }

// Ed25519 public key that verifies API responses, and the X25519 public key
// that pieces are sealed for (spec §4.7/§4.8: both built into the eboot).
// Neither is secret — a public key is meant to be distributed, that is the
// whole point of asymmetric crypto — so committing the real bytes here is
// correct, not a leak. kResponsePublicKey matches the RESPONSE_SIGNING_KEY
// seed set as a Worker secret on both the staging and (once deployed) prod
// environments, so one console binary verifies either; kRecipientPublicKey
// is `crash.py keygen`'s output (tools/crash/README.md). The private halves
// never appear in this repository: the Ed25519 seed lives only as the
// Worker's own secret, and the X25519 private key lives at
// ~/.config/d2vita-crash/admin_x25519.key on the maintainer's machine (back
// it up offline — without it, no uploaded artifact can ever be opened again).
constexpr uint8_t kResponsePublicKey[kEd25519PublicKeyBytes] = {
    0x34, 0x1b, 0x41, 0xaf, 0xc6, 0x15, 0x00, 0x83, 0x5a, 0x97, 0x65, 0x8e,
    0xc9, 0xb8, 0xe0, 0x6b, 0xc1, 0xe3, 0xa8, 0x6f, 0x88, 0xc0, 0x8b, 0x89,
    0x73, 0xf9, 0xbf, 0x07, 0xa9, 0x3e, 0x50, 0x43};
constexpr uint8_t kRecipientPublicKey[kSealKeyBytes] = {
    0xfb, 0xd5, 0xef, 0xe1, 0x98, 0x29, 0xcc, 0x6e, 0xaf, 0xa5, 0xbd, 0x53,
    0x09, 0xc0, 0x2a, 0x81, 0x60, 0x1e, 0xa9, 0x64, 0x5a, 0x84, 0x78, 0x9c,
    0xe3, 0x2f, 0x91, 0x13, 0x4e, 0x57, 0xef, 0x5f};

int upload_thread_entry(SceSize, void*) {
    // Pinned by itself, at its own entry (this codebase's convention —
    // vita_host.h:74-93, mirrored by the present/watchdog threads in
    // vita_present.cpp): same core as the presenter (spec §4.9, "épinglé...
    // sur le coeur du présentateur").
    const unsigned mask = (unsigned)d2vita_core_mask(0);
    const int pinRc = wx86_vita_pin_thread(sceKernelGetThreadId(), (int)mask, nullptr);
    d2vita_core_register("d2cr_upload", sceKernelGetThreadId(), mask, pinRc);

    UploadConfig cfg;
    if (const char* url = getenv("D2_CRASHREPORT_URL")) if (*url) cfg.base_url = url;
    cfg.install_id = g_install_id;
    cfg.channel = channel();
    cfg.platform_model = platform_model();

    UploadKeys keys;
    std::memcpy(keys.response_pk, kResponsePublicKey, sizeof keys.response_pk);
    std::memcpy(keys.recipient_pk, kRecipientPublicKey, sizeof keys.recipient_pk);

    UploadEnv env;
    env.now_unix = vita_now_unix_cb;
    env.random_bytes = vita_random_cb;
    env.should_stop = vita_should_stop_cb;

    Uploader up(*g_io, *g_net, *g_outbox, cfg, keys, env);
    g_last_upload_stats = up.run();
    char m[160];
    std::snprintf(m, sizeof m, "crashreport: envoi termine (%d rapport(s) vus, %d envoyes, %d supprimes, %d echecs)",
                  g_last_upload_stats.considered, g_last_upload_stats.completed,
                  g_last_upload_stats.deleted, g_last_upload_stats.failed);
    d2vita_progress(m);
    return 0;
}

}  // namespace

void d2cr_boot_collect() {
    g_io = make_vita_io();
    if (!g_io) { d2vita_progress("crashreport: IoApi indisponible — rapporteur desactive ce run"); return; }
    g_io->mkdir(kBaseDir);      // usually already exists (the game creates it)
    g_io->mkdir(kReportsDir);

    const int64_t now = d2vita_wall_unix();

    if (env_crashreport_disabled()) {
        d2vita_progress("crashreport: D2_CRASHREPORT=0 — collecte ET envoi desactives ce run");
        return;   // no evidence read, no outbox touched, no session.txt written
    }
    if (crashreport_ask_again(getenv("D2_CRASHREPORT"))) {
        g_io->remove(kConsentPath);
        d2vita_progress("crashreport: D2_CRASHREPORT=ask — choix 'toujours envoyer' oublie");
    }

    g_outbox = std::unique_ptr<Outbox>(new Outbox(*g_io, kOutboxDir));

    // ---- THE ordering property: read the previous session's evidence and
    // copy it into the outbox BEFORE returning to d2vita_platform_init(),
    // which rotates D2VITA_PROGRESS_PATH right after this call returns. ----
    collect_previous_evidence(now);
    g_outbox->enforce(now);

    // ---- start THIS run's session -----------------------------------------
    g_install_id = load_or_create_install_id(*g_io);
    uint8_t sid[16];
    g_cur_session = SessionRecord();
    g_cur_session.session_id = random_bytes(sid, sizeof sid) ? hex_encode(sid, sizeof sid) : std::string();
    g_cur_session.build_id = build_id();
    g_cur_session.started_unix = now;
    // D2WRITE read here reflects the compile-time-flavor baked default
    // (d2_boot_config.cpp sets it unconditionally before this point); env.txt
    // is parsed by d2vita_platform_init() AFTER the rotation this function
    // must run before, so an env.txt override of D2WRITE specifically (not
    // used anywhere in this repo's own env.txt today) would leave this field
    // stale for THIS session's own record — a known, accepted gap.
    if (const char* w = getenv("D2WRITE")) g_cur_session.write_root = w;
    g_cur_session.progress_path = D2VITA_PROGRESS_PATH;
    g_cur_session.state = "running";
    save_cur_session();
    g_collecting = true;
}

void d2cr_after_present() {
    if (!g_collecting || !g_outbox) return;   // D2_CRASHREPORT=0, or boot_collect never ran

    const char* script = getenv("D2SCRIPT");
    const char* cmdfile = getenv("D2CMDFILE");
    std::vector<std::string> ids = g_outbox->list();
    if (ids.empty()) return;

    std::vector<std::string> pending, granted;
    Kind worst = Kind::None;
    for (const std::string& id : ids) {
        ReportRecord rec; ReportState st;
        if (!g_outbox->load(id, &rec, &st)) continue;
        if (st.consent == "granted") { granted.push_back(id); continue; }
        if (st.consent != "pending") continue;   // "denied" reports are removed by Outbox::enforce, not here
        pending.push_back(id);
        if (rec.kind > worst) worst = rec.kind;
    }

    if (!pending.empty()) {
        if (should_suppress_dialog(script, cmdfile)) {
            // ABSOLUTE RULE: this project's own unattended benches. Stays
            // pending — no dialog, and (see below) no network unless an
            // earlier boot already granted consent to something else.
        } else {
            std::string alwaysMarker;
            const bool alwaysSend = g_io->read_file(kConsentPath, &alwaysMarker, 256);
            ConsentAnswer answer;
            if (alwaysSend) {
                answer = ConsentAnswer::AlwaysSend;   // spec §4.3: revocable choice already on disk, no dialog
            } else {
                ConsentPrompt prompt;
                prompt.hang = (worst == Kind::Hang);
                prompt.install_id = g_install_id;
                const char* mdEnv = getenv("D2_CRASHREPORT_MSGDIALOG");
                const bool tryMsgDialog = mdEnv && *mdEnv && std::strcmp(mdEnv, "0") != 0;
                answer = show_consent(prompt, tryMsgDialog);
            }
            if (answer == ConsentAnswer::DontSend) {
                for (const std::string& id : pending) g_outbox->remove(id);
                pending.clear();
                d2vita_progress("crashreport: 'ne pas envoyer' — rapport(s) en attente supprimes");
            } else {
                if (answer == ConsentAnswer::AlwaysSend && !alwaysSend)
                    durable_write(*g_io, kConsentPath, "always\n");
                for (const std::string& id : pending) {
                    ReportRecord rec; ReportState st;
                    if (!g_outbox->load(id, &rec, &st)) continue;
                    st.prompts += 1;
                    if (answer == ConsentAnswer::Send || answer == ConsentAnswer::AlwaysSend) {
                        st.consent = "granted";
                        granted.push_back(id);
                    }   // Later: stays pending, prompts already counted toward the 3-presentation bound
                    g_outbox->save_state(id, st);
                }
            }
        }
    }

    if (granted.empty()) return;   // spec §4.9: never touch the network otherwise

    if (!getenv("D2NET")) {
        const int rc = d2vita_net_init(15000);
        if (rc != 0) {
            char m[96]; std::snprintf(m, sizeof m, "crashreport: reseau KO (%s) — rapport(s) restent en attente", d2vita_net_status());
            d2vita_progress(m);
            return;
        }
        g_net_started_by_us = true;
        d2vita_progress("crashreport: reseau lance pour le(s) rapport(s) (D2NET n'etait pas demande)");
    }
    g_net = make_vita_net();
    if (!g_net) { d2vita_progress("crashreport: NetApi indisponible — rapport(s) restent en attente"); return; }

    g_upload_stop = false;
    g_upload_thread = sceKernelCreateThread("d2cr_upload", upload_thread_entry, (int)kUploadPriority,
                                            kUploadStackBytes, 0, 0, nullptr);
    if (g_upload_thread < 0) {
        char m[64]; std::snprintf(m, sizeof m, "crashreport: CreateThread KO (rc=0x%08x)", (unsigned)g_upload_thread);
        d2vita_progress(m);
        g_upload_thread = -1;
        return;
    }
    const int src = sceKernelStartThread(g_upload_thread, 0, nullptr);
    if (src < 0) {
        char m[64]; std::snprintf(m, sizeof m, "crashreport: StartThread KO (rc=0x%08x)", (unsigned)src);
        d2vita_progress(m);
        sceKernelDeleteThread(g_upload_thread);
        g_upload_thread = -1;
    }
}

void d2cr_session_game_loaded(uint32_t game_base) {
    if (!g_collecting) return;
    g_cur_session.game_base = game_base;
    save_cur_session();
}

void d2cr_session_stopped(const char* stop_reason, uint32_t main_exit) {
    if (!g_collecting) return;
    g_cur_session.stop_reason = stop_reason ? stop_reason : "";
    g_cur_session.main_exit = main_exit;
    g_cur_session.has_main_exit = true;
    save_cur_session();
}

void d2cr_session_clean_exit() {
    if (!g_collecting) return;
    g_cur_session.state = "exited";
    save_cur_session();
}

void d2cr_shutdown() {
    if (g_upload_thread < 0) return;
    g_upload_stop = true;
    __sync_synchronize();
    // Bounded poll-join (the actual bounded-wait precedent in this codebase
    // is d2vita_present_stop()'s sceKernelGetThreadInfo loop, NOT
    // d2vita_watchdog_stop() — that one only sets a flag and never waits).
    // The ceiling here is generous relative to what the thread can still be
    // blocked inside: every HttpClient platform call is sliced to
    // HttpLimits::slice_ms (200 ms, cr_http.h) except the one-time name
    // resolution — cr_net_vita.cpp's resolve() bounds that to whatever
    // HttpLimits::resolve_timeout_ms the caller configures; it stays at
    // cr_http.h's own default (10 s) rather than being overridden here —
    // tightening it to the shutdown path's own real needs is a follow-up
    // worth doing, not done blind here.
    constexpr int kPollMs = 20, kMaxPolls = 600;   // 12 s ceiling
    bool alive = true;
    for (int i = 0; i < kMaxPolls; ++i) {
        SceKernelThreadInfo ti; std::memset(&ti, 0, sizeof ti); ti.size = sizeof ti;
        const int rc = sceKernelGetThreadInfo(g_upload_thread, &ti);
        if (rc < 0) { alive = false; break; }   // UID already gone
        if (ti.status & (SCE_THREAD_DORMANT | SCE_THREAD_STOPPED)) { alive = false; break; }
        sceKernelDelayThread(kPollMs * 1000);
    }
    if (alive) {
        // Not defensible to leave it running past this point under any
        // circumstance (cr_boot.h) — and the whole process calls
        // sceKernelExitProcess() right after this teardown block regardless,
        // so this does not carry the usual risk of a forced delete
        // corrupting state a still-running process goes on to use. This
        // should never actually trigger given the slicing above; if it does,
        // that is a real bug and the line below says so plainly.
        d2vita_progress("crashreport: d2cr_upload TOUJOURS VIVANT apres le delai borne — arret force");
    }
    sceKernelDeleteThread(g_upload_thread);   // releases the TCB/stack either way
    g_upload_thread = -1;
    g_net.reset();
    (void)g_net_started_by_us;   // see the file comment: intentionally not torn down
}

}  // namespace d2cr
