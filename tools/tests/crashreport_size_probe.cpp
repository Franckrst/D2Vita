// tools/tests/crashreport_size_probe.cpp — how much .text the reporter really
// costs once linked.
//
// Summing the object files is an upper bound: Monocypher is one translation
// unit and carries Argon2, EdDSA signing and Elligator, which the reporter
// never calls. This probe is the root of a collected link that leaves them
// out. Leg 7 of tools/tests/run_crashreport_tests.sh builds it and every unit
// of the reporter with -ffunction-sections -fdata-sections, joins them with
// `ld -r --gc-sections -u main`, and prints
//
//     .text(probe + reporter, collected) - .text(probe alone, collected)
//
// which is what the reporter would add to an eboot that already has a main
// and zlib -- the REACHABLE figure, next to the plain sum the budget of spec
// section 4.9 is checked against. The probe alone is collected the same way,
// so what is subtracted is the scaffolding that actually survives, no more.
//
// It is never run. The calls sit behind a test on argc so that the compiler
// cannot drop them, and the IoApi and NetApi stubs stand in for cr_io_vita.cpp
// / cr_net_vita.cpp: their code sits behind that same interface in
// both links, so it counts as scaffolding rather than something --gc-sections
// could drop either way. cr_boot.cpp and cr_consent_vita.cpp are different:
// nothing else calls them, so without the calls below --gc-sections would
// drop them from the REACHABLE figure entirely, undercounting what the
// crash reporter actually added. Only built when __vita__ is defined (leg 7
// only — the host legs link cr_io_posix.cpp/cr_net_posix.cpp instead, which
// never define it).
#include "crashreport/cr_api.h"
#include "crashreport/cr_claim.h"
#include "crashreport/cr_crashtxt_parse.h"
#include "crashreport/cr_evidence.h"
#include "crashreport/cr_http.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_json.h"
#include "crashreport/cr_net.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_progress_parse.h"
#include "crashreport/cr_psp2dmp.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_seal.h"
#include "crashreport/cr_upload.h"
#include "crashreport/cr_verify.h"
#ifdef __vita__
#include "crashreport/cr_boot.h"
#include "crashreport/cr_consent.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include <zlib.h>

using namespace d2cr;

// zlib is linked either way, so its code cancels out in the difference (the
// budget of spec 4.9 excludes it).
static int touch_zlib(int n) {
    z_stream z;
    std::memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return 0;
    z.avail_in = (unsigned)n;
    const int rc = inflate(&z, Z_NO_FLUSH);
    inflateEnd(&z);
    return rc;
}

namespace {

// Stand-ins for the sceIo and sceNet implementations.
class StubIo final : public IoApi {
 public:
    bool read_file(const std::string&, std::string*, size_t) override { return false; }
    bool read_head_tail(const std::string&, size_t, size_t, std::string*) override { return false; }
    bool write_file(const std::string&, const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    bool remove(const std::string&) override { return false; }
    bool mkdir(const std::string&) override { return false; }
    bool list_dir(const std::string&, std::vector<DirEntry>*) override { return false; }
    bool stat(const std::string&, DirEntry*) override { return false; }
    bool read_stream(const std::string&, size_t, bool (*)(const uint8_t*, size_t, void*), void*) override {
        return false;
    }
};

class StubNet final : public NetApi {
 public:
    uint64_t now_ms() override { return 0; }
    void sleep_ms(uint32_t) override {}
    bool resolve(const std::string&, uint32_t*, uint32_t) override { return false; }
    NetHandle connect(uint32_t, uint16_t, uint32_t) override { return kNoHandle; }
    NetResult send(NetHandle, const uint8_t*, size_t, uint32_t, size_t*) override { return NetResult::Error; }
    NetResult recv(NetHandle, uint8_t*, size_t, uint32_t, size_t*) override { return NetResult::Error; }
    void close(NetHandle) override {}
};

bool nothing_sink(const uint8_t*, size_t, void*) { return true; }
bool nothing_random(uint8_t*, size_t, void*) { return true; }
bool nothing_body(HttpBodySinkFn, void*, void*) { return true; }
int64_t zero_clock(void*) { return 0; }
bool never_stop(void*) { return false; }

}  // namespace

// Everything the reporter offers, so that nothing is dropped by mistake.
static void use_everything(int argc, char** argv) {
    StubIo io;
    StubNet net;
    const std::string text = argv[0];
    SessionRecord session;
    parse_session(text, &session);
    std::string out = serialize_session(session);
    Addr addr;
    out += format_addr(addr);
    out += kind_name(Kind::Halt);
    out += format_addr(guest_addr((uint32_t)argc, 0, std::vector<GuestModule>()));

    JsonWriter writer;
    writer.begin_object().key("v").num(1).end_object();
    out += writer.out();
    JsonDoc doc;
    doc.parse(text);

    ProgressFacts progress = parse_progress(text, 0);
    HaltFacts halt = parse_crash_txt(text, 0);
    SecretPatterns patterns = build_patterns(text, accounts_from_registry(text));
    redact_in_place(&out, patterns);
    StreamScanner scanner(patterns);
    scanner.feed((const uint8_t*)text.data(), text.size());
    scanner.finish();

    EvidenceDetails details;
    Evidence evidence = build_evidence(io, session, 0, text, patterns, &details);
    DumpReadOptions dump_options;
    DumpFacts dump = read_psp2dmp(io, text, session, patterns, dump_options);

    Outbox outbox(io, text);
    CollectInputs inputs;
    ReportRecord record;
    ReportState state;
    std::string error;
    outbox.create(inputs, patterns, &record, &error);
    outbox.list();
    outbox.load(text, &record, &state);
    outbox.save_state(text, state);
    outbox.remove(text);
    outbox.enforce(0);

    uint8_t random_bytes[10] = {0};
    out += new_ulid(0, random_bytes);
    out += build_claim_json(claim_inputs_from_record(record, text, text, text));

    uint8_t key[32] = {0}, nonce[16] = {0};
    Sealer sealer;
    sealer.begin(key, nothing_random, nullptr, nothing_sink, nullptr);
    sealer.begin_fixed(key, key, nonce, nothing_sink, nullptr);
    sealer.feed(random_bytes, sizeof random_bytes);
    sealer.finish();
    verify_response(key, text, text);

    HttpClient http(net);
    http.set_base_url(text);
    HttpRequest request;
    request.method = "PUT";
    request.body = nothing_body;
    HttpResponse response;
    http.request(request, &response);

    UploadConfig config;
    UploadKeys keys;
    UploadEnv env;
    env.now_unix = zero_clock;
    env.random_bytes = nothing_random;
    env.should_stop = never_stop;
    Uploader uploader(io, net, outbox, config, keys, env);
    uploader.run();
    uploader.send_report(text);

    Decision decision;
    ArtifactStored stored;
    CompleteResponse complete;
    ErrorBody body;
    parse_decision(text, &decision);
    parse_artifact_stored(text, &stored);
    parse_complete_response(text, &complete);
    parse_error_body(text, &body);

#ifdef __vita__
    // Crash-report orchestration (cr_boot.cpp, cr_consent_vita.cpp): nothing
    // else in this probe calls them, so without this they would be invisible
    // to the REACHABLE (--gc-sections) figure — see the file comment.
    d2cr::d2cr_boot_collect();
    d2cr::d2cr_after_present();
    d2cr::d2cr_session_game_loaded((uint32_t)argc);
    d2cr::d2cr_session_stopped(text.c_str(), (uint32_t)argc);
    d2cr::d2cr_session_clean_exit();
    d2cr::d2cr_shutdown();
    ConsentPrompt prompt;
    prompt.hang = argc != 0;
    prompt.install_id = text;
    show_consent(prompt, argc > 2000);
#endif

    std::printf("%zu %d %d %d %d %d\n", out.size(), progress.beats, (int)halt.code, (int)evidence.kind,
                (int)dump.ok, (int)decision.upload);
}

int main(int argc, char** argv) {
    int n = touch_zlib(argc);
    if (argc > 1000) use_everything(argc, argv);   // never true, never dropped
    std::printf("%d %s\n", n, argv[0]);
    return 0;
}
