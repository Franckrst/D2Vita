// tools/tests/crashreport_transport_test.cpp — host-side tests for the console
// transport of the crash reporter: D2VSEAL1 sealing,
// Ed25519 response signatures, sockets, HTTP/1.1 and the upload state machine.
//
// Like the extraction tests, everything under src/crashreport/ is plain C++17
// with no VitaSDK dependency, so it builds and runs on a dev machine. Run
// through tools/tests/run_crashreport_tests.sh.
#include "crashreport/cr_api.h"
#include "crashreport/cr_http.h"
#include "crashreport/cr_json.h"
#include "crashreport/cr_net.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_seal.h"
#include "crashreport/cr_upload.h"
#include "crashreport/cr_verify.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <pthread.h>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace d2cr;

// ---------------------------------------------------------------- allocation counter
// Counts every C++ allocation to measure peak heap use, and can scan each
// freed block for bytes that must not reach the heap again (key material).
// Disabled under ASan, which owns the allocator.
struct Needle { const char* data; size_t len; };
static const Needle* g_scan_needles = nullptr;   // array ending with {nullptr, 0}; scanning while non-null
static int g_scan_hits = 0;

#ifndef D2CR_NO_ALLOC_COUNTER
static bool block_holds(const char* b, size_t n, const Needle& nd) {
    if (nd.len == 0 || n < nd.len) return false;
    for (size_t i = 0; i + nd.len <= n; ++i)
        if (b[i] == nd.data[0] && std::memcmp(b + i, nd.data, nd.len) == 0) return true;
    return false;
}

static size_t g_heap_cur = 0, g_heap_peak = 0;
static void* counted_alloc(size_t n) {
    void* p = std::malloc(n + sizeof(size_t));
    if (!p) throw std::bad_alloc();
    std::memcpy(p, &n, sizeof n);
    g_heap_cur += n;
    if (g_heap_cur > g_heap_peak) g_heap_peak = g_heap_cur;
    return (char*)p + sizeof(size_t);
}
static void counted_free(void* q) {
    if (!q) return;
    char* p = (char*)q - sizeof(size_t);
    size_t n;
    std::memcpy(&n, p, sizeof n);
    g_heap_cur -= n;
    if (g_scan_needles)
        for (const Needle* nd = g_scan_needles; nd->data; ++nd)
            if (block_holds(p + sizeof(size_t), n, *nd)) ++g_scan_hits;
    std::free(p);
}
void* operator new(size_t n) { return counted_alloc(n); }
void* operator new[](size_t n) { return counted_alloc(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { try { return counted_alloc(n); } catch (...) { return nullptr; } }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { try { return counted_alloc(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, size_t) noexcept { counted_free(p); }
void operator delete[](void* p, size_t) noexcept { counted_free(p); }
static const bool kAllocCounter = true;
#else
[[maybe_unused]] static size_t g_heap_cur = 0, g_heap_peak = 0;
static const bool kAllocCounter = false;
#endif

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
#define CHECK_STR(got, want) do { const std::string g_ = (got), w_ = (want); \
    CHECK(g_ == w_, "%s: got \"%s\", want \"%s\"", #got, g_.c_str(), w_.c_str()); } while (0)
#define CHECK_U64(got, want) do { const uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
    CHECK(g_ == w_, "%s: got %" PRIu64 " (0x%" PRIx64 "), want %" PRIu64 " (0x%" PRIx64 ")", #got, g_, g_, w_, w_); } while (0)

static std::string g_work, g_vectors;

static std::string test_dir(const char* name) {
    const std::string d = g_work + "/" + name;
    const std::string cmd = "rm -rf '" + d + "' && mkdir -p '" + d + "'";
    if (std::system(cmd.c_str()) != 0) { std::printf("  cannot create %s\n", d.c_str()); std::exit(2); }
    return d;
}

static void put_file(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("  cannot write %s\n", path.c_str()); std::exit(2); }
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::string get_file(const std::string& path) {
    std::string r;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return r;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) r.append(buf, n);
    std::fclose(f);
    return r;
}

static bool exists(const std::string& path) { struct stat st; return ::stat(path.c_str(), &st) == 0; }

// One of the contract vector files, copied in tests/crashreport/vectors/.
static std::string vector_file(const char* name) {
    const std::string p = g_vectors + "/" + name;
    if (!exists(p)) { std::printf("  missing vector file %s\n", p.c_str()); std::exit(2); }
    return get_file(p);
}

static std::string from_hex(const std::string& hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) { std::printf("  bad hex string\n"); std::exit(2); }
        out += (char)((hi << 4) | lo);
    }
    return out;
}

static std::string to_hex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) { out += d[c >> 4]; out += d[c & 15]; }
    return out;
}

// ---------------------------------------------------------------- cr_json reader

static void t_json_parse_shapes() {
    JsonDoc doc;
    CHECK(doc.parse("{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"x\"}}"), "parse: %s", doc.error().c_str());
    const JsonNode* root = doc.root();
    CHECK(root && root->type == JsonType::Object && root->count == 3, "root object");
    const JsonNode* a = doc.member(*root, "a");
    CHECK(a && a->type == JsonType::Number && a->is_u64() && a->u64() == 1, "a");
    const JsonNode* b = doc.member(*root, "b");
    CHECK(b && b->type == JsonType::Array && b->count == 3, "b array");
    const JsonNode* e0 = doc.first(*b);
    CHECK(e0 && e0->type == JsonType::Bool && e0->bval, "b[0]");
    const JsonNode* e1 = doc.next(*e0);
    CHECK(e1 && e1->type == JsonType::Bool && !e1->bval, "b[1]");
    const JsonNode* e2 = doc.next(*e1);
    CHECK(e2 && e2->type == JsonType::Null && !doc.next(*e2), "b[2]");
    const JsonNode* c = doc.member(*root, "c");
    CHECK(c && doc.member(*c, "d") && doc.member(*c, "d")->text == "x", "c.d");
    CHECK(!doc.member(*root, "zz"), "absent member must be null");

    // Whole-document parse: trailing content is refused, whitespace is fine.
    CHECK(doc.parse("  {\"a\":1}\n\t "), "trailing whitespace refused: %s", doc.error().c_str());
    CHECK(!doc.parse("{\"a\":1} {}"), "trailing value accepted");
    CHECK(!doc.parse(""), "empty document accepted");
    CHECK(!doc.parse("{\"a\":1,}"), "trailing comma accepted");
    CHECK(!doc.parse("{'a':1}"), "single quotes accepted");
    CHECK(!doc.parse("{\"a\":01}"), "leading zero accepted");
    CHECK(!doc.parse("{\"a\":+1}"), "leading plus accepted");
    CHECK(!doc.parse("{\"a\":.5}"), "bare fraction accepted");
    CHECK(!doc.parse("{\"a\":1."), "unterminated number accepted");
    CHECK(!doc.parse("{\"a\":nul}"), "bad literal accepted");
    CHECK(!doc.parse("{\"a\":1,\"a\":2}"), "duplicate key accepted");
    CHECK(!doc.parse("{\"a\":\"\x01\"}"), "raw control character accepted");
    CHECK(!doc.parse("{\"a\":\"\xff\"}"), "invalid UTF-8 accepted");
    CHECK(!doc.parse("[1,2"), "unterminated array accepted");
    CHECK(!doc.parse("{\"a\"1}"), "missing colon accepted");
}

static void t_json_strings_and_numbers() {
    JsonDoc doc;
    CHECK(doc.parse("{\"s\":\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\te\\u0041\\u00e9\\u20ac\\ud83d\\ude00\"}"),
          "escapes: %s", doc.error().c_str());
    const JsonNode* s = doc.member(*doc.root(), "s");
    CHECK(s != nullptr, "s missing");
    if (s) {
        CHECK_STR(s->text, std::string("a\"b\\c/d\b\f\n\r\teA\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"));
        // Code points, not bytes: 23 bytes of UTF-8, 17 code points, and the
        // surrogate pair of the emoji counts as one (JSON Schema maxLength).
        CHECK_U64(s->text.size(), 23);
        CHECK_U64(s->codepoints, 17);
    }
    CHECK(!doc.parse("{\"s\":\"\\x41\"}"), "unknown escape accepted");
    CHECK(!doc.parse("{\"s\":\"\\u00g1\"}"), "bad hex escape accepted");
    CHECK(!doc.parse("{\"s\":\"\\u\"}"), "short escape accepted");

    // JSON Schema integers: 1420, 1420.0 and 1.42e3 are the same integer.
    struct { const char* json; bool integer; uint64_t value; } cases[] = {
        {"0", true, 0}, {"1420", true, 1420}, {"1420.0", true, 1420}, {"1.42e3", true, 1420},
        {"1.42E+3", true, 1420}, {"14200e-1", true, 1420}, {"4294967295", true, 4294967295ull},
        {"18446744073709551615", true, 18446744073709551615ull},
        {"1420.5", false, 0}, {"1e400", false, 0}, {"-1", false, 0}, {"-0", true, 0},
        {"18446744073709551616", false, 0}, {"0.0", true, 0}, {"1e3", true, 1000},
    };
    for (const auto& c : cases) {
        const std::string text = std::string("{\"n\":") + c.json + "}";
        CHECK(doc.parse(text), "%s: %s", c.json, doc.error().c_str());
        const JsonNode* n = doc.member(*doc.root(), "n");
        CHECK(n && n->type == JsonType::Number, "%s: not a number", c.json);
        if (!n) continue;
        CHECK(n->is_u64() == c.integer, "%s: is_u64 %d, want %d", c.json, (int)n->is_u64(), (int)c.integer);
        if (c.integer) CHECK_U64(n->u64(), c.value);
    }
}

static void t_json_limits() {
    JsonDoc doc;
    JsonLimits lim;
    lim.max_depth = 3;
    lim.max_nodes = 8;
    lim.max_bytes = 64;
    CHECK(doc.parse("[[1]]", lim), "depth 3 refused: %s", doc.error().c_str());
    CHECK(!doc.parse("[[[1]]]", lim), "depth 4 accepted");
    CHECK(!doc.parse("[1,2,3,4,5,6,7,8,9]", lim), "node limit ignored");
    CHECK(!doc.parse(std::string("[\"") + std::string(100, 'x') + "\"]", lim), "byte limit ignored");
    // The vector files are large: the default limits must not be console-sized.
    JsonDoc big;
    CHECK(big.parse(vector_file("response-sig.v1.json"), JsonLimits::vectors()), "vectors: %s", big.error().c_str());
    const JsonNode* cases = big.member(*big.root(), "cases");
    CHECK(cases && cases->count >= 9, "response-sig cases: %zu", cases ? cases->count : 0);
}

// ---------------------------------------------------------------- cr_seal
// Every case of the contract vectors (tests/crashreport/vectors/sealed.v1.json,
// a copy of D2Vita-website contract/vectors/) must come out byte for byte,
// whatever the size of the pieces fed to the sealer.

// Text of a member, or "" (with a failed check) when it is missing.
static std::string jstr(const JsonDoc& doc, const JsonNode& obj, const char* key) {
    const JsonNode* n = doc.member(obj, key);
    CHECK(n && n->type == JsonType::String, "member %s missing or not a string", key);
    return n && n->type == JsonType::String ? n->text : std::string();
}

static uint64_t jnum(const JsonDoc& doc, const JsonNode& obj, const char* key) {
    const JsonNode* n = doc.member(obj, key);
    CHECK(n && n->is_u64(), "member %s missing or not an integer", key);
    return n && n->is_u64() ? n->u64() : 0;
}

static bool collect_sink(const uint8_t* data, size_t n, void* ud) {
    static_cast<std::string*>(ud)->append((const char*)data, n);
    return true;
}

static bool refuse_sink(const uint8_t*, size_t, void*) { return false; }

// A random source that hands out fixed bytes, so that begin() reproduces a
// vector: first the ephemeral secret key, then the nonce prefix.
struct FixedRandom {
    std::string bytes;
    size_t used = 0;
    int calls = 0;
    int fail_at = -1;
};

static bool fixed_random(uint8_t* out, size_t n, void* ud) {
    FixedRandom* r = static_cast<FixedRandom*>(ud);
    if (r->calls++ == r->fail_at) return false;
    if (r->used + n > r->bytes.size()) return false;
    std::memcpy(out, r->bytes.data() + r->used, n);
    r->used += n;
    return true;
}

// Seals `plain` feeding it in pieces of `step` bytes (0 = one call).
static std::string seal_with_step(const std::string& recipient_pk, const std::string& eph_sk,
                                  const std::string& nonce_prefix, uint32_t chunk, const std::string& plain,
                                  size_t step) {
    std::string out;
    Sealer s;
    if (!s.begin_fixed((const uint8_t*)recipient_pk.data(), (const uint8_t*)eph_sk.data(),
                       (const uint8_t*)nonce_prefix.data(), collect_sink, &out, chunk)) {
        CHECK(false, "begin_fixed failed");
        return std::string();
    }
    const size_t piece = step ? step : (plain.empty() ? 1 : plain.size());
    for (size_t i = 0; i < plain.size(); i += piece) {
        const size_t n = piece < plain.size() - i ? piece : plain.size() - i;
        if (!s.feed((const uint8_t*)plain.data() + i, n)) { CHECK(false, "feed failed"); return std::string(); }
    }
    if (!s.finish()) { CHECK(false, "finish failed"); return std::string(); }
    CHECK_U64(s.plain_bytes(), plain.size());
    CHECK_U64(s.sealed_bytes(), out.size());
    return out;
}

static void t_seal_contract_vectors() {
    JsonDoc doc;
    CHECK(doc.parse(vector_file("sealed.v1.json"), JsonLimits::vectors()), "sealed.v1.json: %s", doc.error().c_str());
    const JsonNode* root = doc.root();
    const JsonNode* cases = root ? doc.member(*root, "cases") : nullptr;
    CHECK(cases && cases->count >= 10, "sealed cases: %zu", cases ? cases->count : 0);
    if (!cases) return;
    int done = 0;
    for (const JsonNode* c = doc.first(*cases); c; c = doc.next(*c)) {
        const std::string name = jstr(doc, *c, "name");
        const std::string recipient_pk = from_hex(jstr(doc, *c, "recipient_pk_hex"));
        const std::string eph_sk = from_hex(jstr(doc, *c, "eph_sk_hex"));
        const std::string nonce_prefix = from_hex(jstr(doc, *c, "nonce_prefix_hex"));
        const std::string plain = from_hex(jstr(doc, *c, "plaintext_hex"));
        const std::string want = from_hex(jstr(doc, *c, "sealed_hex"));
        const uint32_t chunk = (uint32_t)jnum(doc, *c, "chunk_size");
        CHECK(recipient_pk.size() == 32 && eph_sk.size() == 32 && nonce_prefix.size() == 16, "%s: key sizes",
              name.c_str());

        // Whole plaintext at once, then in pieces that cross chunk borders.
        const size_t steps[] = {0, 1, 7, chunk, (size_t)chunk + 1, 4096};
        for (size_t step : steps) {
            if (step > 0 && step < 4096 && plain.size() > 100000) continue;   // keep the 70 KiB case quick
            const std::string got = seal_with_step(recipient_pk, eph_sk, nonce_prefix, chunk, plain, step);
            CHECK(got == want, "%s: sealed bytes differ (step %zu)\n    got  %s\n    want %s", name.c_str(), step,
                  to_hex(got.substr(0, got.size() < 96 ? got.size() : 96)).c_str(),
                  to_hex(want.substr(0, want.size() < 96 ? want.size() : 96)).c_str());
            if (got != want) break;
        }
        // The intermediate values of the vector, to locate a disagreement.
        const std::string got = seal_with_step(recipient_pk, eph_sk, nonce_prefix, chunk, plain, 0);
        if (got.size() >= 72) {
            CHECK_STR(to_hex(got.substr(8, 8)), jstr(doc, *c, "key_id_hex"));
            CHECK_STR(to_hex(got.substr(16, 32)), jstr(doc, *c, "eph_pk_hex"));
            CHECK_STR(to_hex(got.substr(48, 16)), jstr(doc, *c, "nonce_prefix_hex"));
        }
        CHECK_U64(sealed_size_for(plain.size(), chunk), want.size());
        ++done;
    }
    CHECK(done >= 10, "only %d sealed cases exercised", done);
}

static void t_seal_sizes_and_failures() {
    // sealed_size_for and the extraction library's sealed_size are the same
    // function for the console chunk size (the claim announces one, the PUT
    // sends the other).
    const uint64_t sizes[] = {0, 1, 87, 65535, 65536, 65537, 131072, 2 * 1024 * 1024, 2096568};
    for (uint64_t n : sizes) CHECK_U64(sealed_size_for(n), sealed_size(n));
    CHECK_U64(sealed_size_for(0), 88);
    CHECK_U64(sealed_size_for(65536), 72 + 32 + 65536);      // exact multiple: an empty last chunk
    CHECK_U64(sealed_size_for(16, 16), 120);
    CHECK_U64(sealed_size_for(0, 1), 88);
    // The caps of spec §4.5 hold for the largest plaintext the contract lists.
    CHECK_U64(sealed_size_for(2096568), 2 * 1024 * 1024);
    CHECK_U64(sealed_size_for(65448), 64 * 1024);
    CHECK_U64(sealed_size_for(327528), 320 * 1024);

    const std::string pk(32, '\x11'), sk(32, '\x22'), nonce(16, '\x33');
    std::string out;
    {   // A sink that refuses stops the sealing.
        Sealer s;
        CHECK(!s.begin_fixed((const uint8_t*)pk.data(), (const uint8_t*)sk.data(), (const uint8_t*)nonce.data(),
                             refuse_sink, nullptr, 64),
              "a sink refusing the header must fail begin");
        CHECK(!s.feed((const uint8_t*)"x", 1) && !s.finish(), "a failed sealer must stay failed");
    }
    {   // Out-of-range chunk sizes (contract: readers accept 1 to 1048576).
        Sealer s;
        CHECK(!s.begin_fixed((const uint8_t*)pk.data(), (const uint8_t*)sk.data(), (const uint8_t*)nonce.data(),
                             collect_sink, &out, 0), "chunk size 0 accepted");
        CHECK(!s.begin_fixed((const uint8_t*)pk.data(), (const uint8_t*)sk.data(), (const uint8_t*)nonce.data(),
                             collect_sink, &out, kSealMaxChunkBytes + 1), "chunk size over 1 MiB accepted");
        CHECK(s.begin_fixed((const uint8_t*)pk.data(), (const uint8_t*)sk.data(), (const uint8_t*)nonce.data(),
                            collect_sink, &out, kSealMaxChunkBytes), "chunk size 1 MiB refused");
    }
    {   // A recipient key that gives an all-zero shared secret is refused.
        const std::string low_order(32, '\0');
        Sealer s;
        CHECK(!s.begin_fixed((const uint8_t*)low_order.data(), (const uint8_t*)sk.data(),
                             (const uint8_t*)nonce.data(), collect_sink, &out, 64),
              "a low-order recipient key was accepted");
    }
    {   // No entropy: no sealed object at all.
        FixedRandom r;
        r.bytes = std::string(48, '\x44');
        r.fail_at = 1;                      // the nonce prefix draw fails
        Sealer s;
        out.clear();
        CHECK(!s.begin((const uint8_t*)pk.data(), fixed_random, &r, collect_sink, &out, 64),
              "begin must fail when the entropy source does");
        CHECK(out.empty(), "a failed begin wrote %zu bytes", out.size());
        CHECK(!s.begin((const uint8_t*)pk.data(), nullptr, nullptr, collect_sink, &out, 64), "begin without a source");
    }
}

// Production path: begin() draws the ephemeral key and the nonce prefix from
// the injected source, and reproduces the vector when that source hands out
// the vector's bytes.
static void t_seal_injected_entropy() {
    JsonDoc doc;
    CHECK(doc.parse(vector_file("sealed.v1.json"), JsonLimits::vectors()), "sealed.v1.json: %s", doc.error().c_str());
    const JsonNode* cases = doc.member(*doc.root(), "cases");
    if (!cases) return;
    for (const JsonNode* c = doc.first(*cases); c; c = doc.next(*c)) {
        if (jstr(doc, *c, "name") != "seventy_thousand_bytes") continue;
        const std::string recipient_pk = from_hex(jstr(doc, *c, "recipient_pk_hex"));
        const std::string plain = from_hex(jstr(doc, *c, "plaintext_hex"));
        const std::string want = from_hex(jstr(doc, *c, "sealed_hex"));
        FixedRandom r;
        r.bytes = from_hex(jstr(doc, *c, "eph_sk_hex")) + from_hex(jstr(doc, *c, "nonce_prefix_hex"));
        std::string out;
        Sealer s;
        CHECK(s.begin((const uint8_t*)recipient_pk.data(), fixed_random, &r, collect_sink, &out), "begin failed");
        CHECK(s.feed((const uint8_t*)plain.data(), plain.size()) && s.finish(), "seal failed");
        CHECK(out == want, "begin() did not reproduce the vector");
        CHECK_U64(r.used, 48);
        CHECK_U64(r.calls, 2);
        return;
    }
    CHECK(false, "vector seventy_thousand_bytes not found");
}

// A 2 MiB artifact must seal within one chunk of memory, and no plaintext or
// key byte may return to the heap.
static void t_seal_bounded_memory() {
    if (!kAllocCounter) return;
    std::string plain;
    plain.reserve(2 * 1024 * 1024);
    while (plain.size() < 2u * 1024 * 1024) plain += "SEALEDPLAINTEXTMARKER-0123456789";
    const std::string pk(32, '\x51'), sk(32, '\x52'), nonce(16, '\x53');
    std::string out;
    out.reserve(sealed_size_for(plain.size()) + 64);
    const Needle needles[] = {{"SEALEDPLAINTEXTMARKER", 21}, {sk.data(), sk.size()}, {nullptr, 0}};
    const size_t before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    g_scan_hits = 0;
    g_scan_needles = needles;
    {
        Sealer s;
        CHECK(s.begin_fixed((const uint8_t*)pk.data(), (const uint8_t*)sk.data(), (const uint8_t*)nonce.data(),
                            collect_sink, &out),
              "begin_fixed failed");
        for (size_t i = 0; i < plain.size(); i += 64 * 1024)
            CHECK(s.feed((const uint8_t*)plain.data() + i, 64 * 1024), "feed failed");
        CHECK(s.finish(), "finish failed");
    }
    g_scan_needles = nullptr;
    const size_t peak = g_heap_peak - before;
    CHECK_U64(out.size(), sealed_size_for(plain.size()));
    // The plaintext and the output buffer are allocated before the window, so
    // the peak is what the sealer itself holds: one chunk plus its tag.
    std::printf("    seal of %zu bytes: %zu bytes of heap peak\n", plain.size(), peak);
    CHECK(peak < 64 * 1024 + 4096, "sealer heap peak %zu over one chunk", peak);
    CHECK(g_scan_hits == 0, "%d freed heap block(s) still hold plaintext or key material", g_scan_hits);
}

// ---------------------------------------------------------------- cr_verify
// Every case of tests/crashreport/vectors/response-sig.v1.json: the positive
// ones must verify, the negative ones must not (among them S + L, which a
// verifier that forgets to bound S would accept).

static void t_verify_contract_vectors() {
    JsonDoc doc;
    CHECK(doc.parse(vector_file("response-sig.v1.json"), JsonLimits::vectors()), "response-sig: %s",
          doc.error().c_str());
    const JsonNode* root = doc.root();
    if (!root) return;
    int positives = 0, negatives = 0;
    const JsonNode* cases = doc.member(*root, "cases");
    CHECK(cases != nullptr, "no cases");
    for (const JsonNode* c = cases ? doc.first(*cases) : nullptr; c; c = doc.next(*c)) {
        const std::string name = jstr(doc, *c, "name");
        const std::string pk = from_hex(jstr(doc, *c, "public_key_hex"));
        const std::string body = jstr(doc, *c, "body_utf8");
        const std::string sig = jstr(doc, *c, "signature_b64");
        CHECK(pk.size() == 32, "%s: public key size %zu", name.c_str(), pk.size());
        CHECK(verify_response((const uint8_t*)pk.data(), sig, body), "%s: valid signature refused", name.c_str());
        // The signature covers the exact bytes: one byte more or less fails.
        CHECK(!verify_response((const uint8_t*)pk.data(), sig, body + "\n"), "%s: trailing byte accepted",
              name.c_str());
        if (!body.empty())
            CHECK(!verify_response((const uint8_t*)pk.data(), sig, body.substr(0, body.size() - 1)),
                  "%s: truncated body accepted", name.c_str());
        ++positives;
    }
    const JsonNode* negs = doc.member(*root, "negative_cases");
    CHECK(negs != nullptr, "no negative cases");
    for (const JsonNode* c = negs ? doc.first(*negs) : nullptr; c; c = doc.next(*c)) {
        const std::string name = jstr(doc, *c, "name");
        const std::string pk = from_hex(jstr(doc, *c, "public_key_hex"));
        CHECK(!verify_response((const uint8_t*)pk.data(), jstr(doc, *c, "signature_b64"), jstr(doc, *c, "body_utf8")),
              "%s: an invalid signature verified", name.c_str());
        ++negatives;
    }
    CHECK(positives >= 9 && negatives >= 5, "%d positive, %d negative cases", positives, negatives);
}

static void t_verify_signature_encoding() {
    JsonDoc doc;
    CHECK(doc.parse(vector_file("response-sig.v1.json"), JsonLimits::vectors()), "response-sig: %s",
          doc.error().c_str());
    const JsonNode* cases = doc.member(*doc.root(), "cases");
    const JsonNode* first = cases ? doc.first(*cases) : nullptr;
    CHECK(first != nullptr, "no case to work from");
    if (!first) return;
    const std::string pk = from_hex(jstr(doc, *first, "public_key_hex"));
    const std::string body = jstr(doc, *first, "body_utf8");
    const std::string sig = jstr(doc, *first, "signature_b64");
    uint8_t raw[kEd25519SignatureBytes];
    CHECK(sig.size() == kSignatureHeaderChars, "header value is %zu characters", sig.size());
    CHECK(decode_signature(sig, raw), "the vector signature does not decode");

    // Everything the Worker never writes is refused rather than guessed.
    const std::string bad[] = {
        "",                                          // absent header
        sig.substr(0, 87),                           // one character short
        sig + "=",                                   // one character too many
        sig.substr(0, 86) + "=A",                    // padding not at the end
        sig.substr(0, 85) + "=" + sig.substr(86),    // '=' inside the data
        " " + sig.substr(1),                         // whitespace
        sig.substr(0, 10) + "-" + sig.substr(11),    // base64url character
        sig.substr(0, 10) + "\n" + sig.substr(11),   // newline
    };
    for (const std::string& b : bad)
        CHECK(!decode_signature(b, raw), "bad encoding accepted: \"%s\"", b.c_str());
    // Non-canonical padding bits (the last character carries four unused bits).
    std::string non_canonical = sig;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t value = alphabet.find(sig[85]);
    CHECK(value != std::string::npos, "last character outside the alphabet");
    non_canonical[85] = alphabet[(value & ~(size_t)0x0F) | ((value & 0x0F) ^ 0x01)];
    CHECK(!decode_signature(non_canonical, raw), "non-canonical padding bits accepted");
    // It would decode to the same 64 bytes, so a lax decoder would verify it.
    CHECK(!verify_response((const uint8_t*)pk.data(), non_canonical, body), "non-canonical signature verified");
    CHECK(!verify_response(nullptr, sig, body), "verification without a public key");
}

// ---------------------------------------------------------------- cr_net
// The POSIX NetApi against a listening socket opened by the test itself.

// A listening socket on 127.0.0.1, port chosen by the kernel.
struct Listener {
    int fd = -1;
    uint16_t port = 0;

    bool start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        socklen_t len = sizeof sa;
        if (::bind(fd, (struct sockaddr*)&sa, sizeof sa) != 0 || ::listen(fd, 8) != 0 ||
            ::getsockname(fd, (struct sockaddr*)&sa, &len) != 0) {
            ::close(fd);
            fd = -1;
            return false;
        }
        port = ntohs(sa.sin_port);
        return true;
    }
    int accept_one() { return fd >= 0 ? ::accept(fd, nullptr, nullptr) : -1; }
    void stop() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

static void t_net_posix_basics() {
    auto net = make_posix_net();
    const uint64_t t0 = net->now_ms();
    net->sleep_ms(30);
    const uint64_t slept = net->now_ms() - t0;
    CHECK(slept >= 25 && slept < 2000, "sleep_ms(30) took %" PRIu64 " ms", slept);

    uint32_t ip = 0;
    CHECK(net->resolve("127.0.0.1", &ip, 1000) && ip == 0x7f000001u, "resolve of a dotted quad: 0x%x", ip);
    ip = 0;
    CHECK(net->resolve("localhost", &ip, 1000) && (ip >> 24) == 127, "resolve of localhost: 0x%x", ip);
    CHECK(!net->resolve("nonexistent.invalid", &ip, 1000), "an unknown name resolved");
    CHECK(!net->resolve("", &ip, 1000), "the empty name resolved");

    Listener l;
    CHECK(l.start(), "cannot listen on 127.0.0.1");
    if (l.fd < 0) return;
    const NetHandle h = net->connect(0x7f000001u, l.port, 1000);
    CHECK(h != kNoHandle, "connect to 127.0.0.1:%u failed", l.port);
    const int server = l.accept_one();
    CHECK(server >= 0, "accept failed");
    if (h == kNoHandle || server < 0) { l.stop(); return; }

    // Client to server.
    size_t sent = 0;
    CHECK(net->send(h, (const uint8_t*)"ping", 4, 1000, &sent) == NetResult::Ok && sent == 4, "send");
    char buf[16] = {0};
    CHECK(::recv(server, buf, sizeof buf, 0) == 4 && std::memcmp(buf, "ping", 4) == 0, "server got \"%s\"", buf);

    // Nothing to read yet: a poll (timeout 0) and a short wait both time out.
    uint8_t in[16];
    size_t got = 0;
    CHECK(net->recv(h, in, sizeof in, 0, &got) == NetResult::Timeout, "recv poll must time out");
    const uint64_t t1 = net->now_ms();
    CHECK(net->recv(h, in, sizeof in, 120, &got) == NetResult::Timeout, "recv must time out");
    const uint64_t waited = net->now_ms() - t1;
    CHECK(waited >= 100 && waited < 3000, "recv(120 ms) waited %" PRIu64 " ms", waited);

    // Server to client, then the server closes: Closed, not Error.
    CHECK(::send(server, "pong", 4, 0) == 4, "server send");
    CHECK(net->recv(h, in, sizeof in, 1000, &got) == NetResult::Ok && got == 4 && std::memcmp(in, "pong", 4) == 0,
          "client recv");
    ::close(server);
    CHECK(net->recv(h, in, sizeof in, 1000, &got) == NetResult::Closed, "end of stream must be Closed");
    net->close(h);
    l.stop();
}

static void t_net_posix_failures() {
    auto net = make_posix_net();
    // A port nobody listens on: refused, and quickly.
    Listener l;
    CHECK(l.start(), "cannot listen");
    const uint16_t dead_port = l.port;
    l.stop();
    const uint64_t t0 = net->now_ms();
    CHECK(net->connect(0x7f000001u, dead_port, 2000) == kNoHandle, "connect to a closed port succeeded");
    CHECK(net->now_ms() - t0 < 2000, "a refused connection took %" PRIu64 " ms", net->now_ms() - t0);

    // Sending to a peer that is gone ends as Error, never as a signal.
    CHECK(l.start(), "cannot listen again");
    const NetHandle h = net->connect(0x7f000001u, l.port, 1000);
    const int server = l.accept_one();
    CHECK(h != kNoHandle && server >= 0, "setup connection");
    if (h != kNoHandle && server >= 0) {
        ::close(server);
        NetResult r = NetResult::Ok;
        const std::string payload(4096, 'x');
        for (int i = 0; i < 200 && r == NetResult::Ok; ++i) {
            size_t sent = 0;
            r = net->send(h, (const uint8_t*)payload.data(), payload.size(), 500, &sent);
        }
        CHECK(r == NetResult::Error, "send to a closed peer ended as %s", net_result_name(r));
        size_t got = 0;
        uint8_t in[8];
        const NetResult rr = net->recv(h, in, sizeof in, 500, &got);
        CHECK(rr == NetResult::Closed || rr == NetResult::Error, "recv on a broken connection: %s",
              net_result_name(rr));
        net->close(h);
    }
    l.stop();
    // Bad handles are refused rather than crashing.
    uint8_t in[4];
    size_t n = 0;
    CHECK(net->send(kNoHandle, in, 1, 10, &n) == NetResult::Error, "send on no handle");
    CHECK(net->recv(kNoHandle, in, sizeof in, 10, &n) == NetResult::Error, "recv on no handle");
    net->close(kNoHandle);
}

// ---------------------------------------------------------------- fake server
// tools/tests/fake_crash_api.py, started per scenario: it writes the port it
// listens on, logs every request as one JSON object per line, and answers
// either exact bytes (raw mode) or the console routes of the contract (api
// mode).

static std::string g_fake_api;   // --fake-api, empty when Python or PyNaCl is missing

struct FakeServer {
    pid_t pid = -1;
    uint16_t port = 0;
    std::string dir, log_path, artifacts_dir;

    bool start(const std::string& work_dir, const std::string& script_json, bool artifacts = false) {
        dir = work_dir;
        log_path = dir + "/requests.jsonl";
        artifacts_dir = dir + "/artifacts";
        const std::string script_path = dir + "/script.json";
        const std::string port_path = dir + "/port";
        put_file(script_path, script_json);
        ::unlink(port_path.c_str());
        pid = ::fork();
        if (pid == 0) {
            if (artifacts)
                ::execlp("python3", "python3", g_fake_api.c_str(), "--script", script_path.c_str(), "--port-file",
                         port_path.c_str(), "--log", log_path.c_str(), "--artifacts", artifacts_dir.c_str(),
                         (char*)nullptr);
            else
                ::execlp("python3", "python3", g_fake_api.c_str(), "--script", script_path.c_str(), "--port-file",
                         port_path.c_str(), "--log", log_path.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        if (pid < 0) return false;
        for (int i = 0; i < 1500 && !exists(port_path); ++i) {
            struct timespec ts = {0, 10 * 1000 * 1000};
            ::nanosleep(&ts, nullptr);
        }
        if (!exists(port_path)) return false;
        port = (uint16_t)std::atoi(get_file(port_path).c_str());
        return port != 0;
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    std::string base_url() const {
        char b[64];
        std::snprintf(b, sizeof b, "http://127.0.0.1:%u", (unsigned)port);
        return b;
    }

    // One JSON object per request, in order.
    std::vector<std::string> log_lines() const {
        std::vector<std::string> out;
        const std::string all = get_file(log_path);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t nl = all.find('\n', pos);
            if (nl == std::string::npos) nl = all.size();
            if (nl > pos) out.push_back(all.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return out;
    }
};

static bool have_fake_api() {
    if (g_fake_api.empty()) {
        std::printf("    SKIPPED: no --fake-api (python3 with PyNaCl not available)\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- cr_http

struct BodyString {
    std::string data;
    size_t pieces = 1;
    bool fail = false;
    bool short_write = false;
    // Waited once, after the first piece. The client looks for an answer
    // before each piece it writes, so a test about an answer that arrives
    // mid-body has to leave the server time to send it -- otherwise the whole
    // body can go into the socket buffers before the server is even scheduled,
    // and the test becomes a race on how big those buffers are.
    uint32_t pause_after_first_ms = 0;
};

static bool body_from_string(HttpBodySinkFn sink, void* sink_ud, void* ud) {
    BodyString* b = static_cast<BodyString*>(ud);
    const size_t step = b->pieces ? (b->data.size() + b->pieces - 1) / b->pieces : b->data.size();
    size_t limit = b->data.size();
    if (b->short_write && limit > 0) limit -= 1;
    for (size_t i = 0; i < limit; i += step) {
        const size_t n = step < limit - i ? step : limit - i;
        if (!sink((const uint8_t*)b->data.data() + i, n, sink_ud)) return false;
        if (i == 0 && b->pause_after_first_ms) {
            struct timespec ts;
            ts.tv_sec = b->pause_after_first_ms / 1000;
            ts.tv_nsec = (long)(b->pause_after_first_ms % 1000) * 1000000L;
            nanosleep(&ts, nullptr);
        }
    }
    return !b->fail;
}

static std::string raw_script(const std::string& exchanges) {
    return "{\"mode\":\"raw\",\"exchanges\":[" + exchanges + "]}";
}

// One raw exchange sending `text` (C escapes already resolved) in one write.
static std::string raw_send(const std::string& text, const std::string& extra = "") {
    JsonWriter w;
    w.begin_object().key("send").begin_array().begin_object().key("text").str(text).end_object().end_array()
        .end_object();
    std::string out = w.out();
    if (!extra.empty()) out = out.substr(0, out.size() - 1) + "," + extra + "}";
    return out;
}

static HttpError run_request(HttpClient& http, const char* method, const std::string& path, HttpResponse* out,
                             const std::vector<std::string>& headers = {}, BodyString* body = nullptr) {
    HttpRequest req;
    req.method = method;
    req.path = path;
    req.headers = headers;
    if (body) {
        req.content_length = body->data.size();
        req.body = body_from_string;
        req.body_ud = body;
    }
    return http.request(req, out);
}

static void t_http_url_parsing() {
    auto net = make_posix_net();
    HttpClient http(*net);
    CHECK(http.set_base_url("http://d2vita-crash.franck-rst-c3d.workers.dev") &&
          http.host() == "d2vita-crash.franck-rst-c3d.workers.dev" && http.port() == 80 && http.base_path().empty(),
          "plain host");
    CHECK(http.set_base_url("http://127.0.0.1:8787") && http.host() == "127.0.0.1" && http.port() == 8787,
          "host and port");
    CHECK(http.set_base_url("http://example.test:8080/api/") && http.base_path() == "/api", "prefix, trailing slash");
    // The address the console ships with (spec §3), and D2_CRASHREPORT_URL
    // replacing it.
    CHECK(http.set_base_url(kDefaultApiUrl) && http.host() == "d2vita-crash.franck-rst-c3d.workers.dev" &&
          http.port() == 80 && http.base_path().empty(), "the default API address does not parse");
    const char* bad[] = {"https://example.test", "http://", "ftp://example.test", "example.test",
                         "http://example.test:0", "http://example.test:70000", "http://user@example.test",
                         "http://exa mple.test", "http://example.test:80x"};
    for (const char* u : bad) CHECK(!http.set_base_url(u), "bad URL accepted: %s", u);

    // The request target is pasted into the request line as it is: a space or
    // a control character would cut that line in two, and a CR or LF would add
    // a header of the caller's choosing. It is refused before a connection is
    // opened -- nothing listens on port 1, so a good target gets as far as
    // Connect and a refused one never does.
    CHECK(http.set_base_url("http://127.0.0.1:1"), "base url for the request-target checks");
    HttpResponse r;
    CHECK(run_request(http, "GET", "/v1/claims", &r) == HttpError::Connect, "a good target must reach the socket");
    const char* bad_paths[] = {"", "v1/claims", "/v1/re ports", "/v1/a\tb", "/v1/a\r\nX-D2V-Install: y",
                               "/v1/a\nb", "/v1/\x7f", "/v1/re\xc3\xa9ports"};
    for (const char* p : bad_paths)
        CHECK(run_request(http, "GET", p, &r) == HttpError::BadUrl, "bad request target accepted: \"%s\"", p);
}

static void t_http_get_and_headers() {
    if (!have_fake_api()) return;
    const std::string dir = test_dir("http_basic");
    FakeServer s;
    CHECK(s.start(dir, raw_script(raw_send(
        "HTTP/1.1 200 OK\r\nContent-Length: 17\r\nX-D2V-Signature: AAAA\r\nConnection: close\r\n\r\n"
        "{\"v\":1,\"ok\":true}"))), "server did not start");
    if (s.pid < 0) return;
    auto net = make_posix_net();
    HttpClient http(*net);
    CHECK(http.set_base_url(s.base_url() + "/v1"), "base url");
    HttpResponse r;
    const HttpError e = run_request(http, "POST", "/claims", &r, {"X-D2V-Client: d2vita/0.1.0+ab12cd34ef56"});
    CHECK(e == HttpError::None, "request failed: %s (%s)", http_error_name(e), http.error_detail().c_str());
    CHECK_U64(r.status, 200);
    CHECK_STR(r.body, "{\"v\":1,\"ok\":true}");
    CHECK_STR(r.signature, "AAAA");
    s.stop();
    // What the server saw: the path is the base plus the request path, and the
    // console headers are there, with a Content-Length even for an empty body.
    const std::vector<std::string> lines = s.log_lines();
    CHECK(lines.size() == 1, "%zu request(s) logged", lines.size());
    if (lines.empty()) return;
    JsonDoc doc;
    CHECK(doc.parse(lines[0], JsonLimits::vectors()), "log line: %s", doc.error().c_str());
    const JsonNode* root = doc.root();
    CHECK_STR(jstr(doc, *root, "method"), "POST");
    CHECK_STR(jstr(doc, *root, "path"), "/v1/claims");
    CHECK_STR(jstr(doc, *root, "version"), "HTTP/1.1");
    const JsonNode* h = doc.member(*root, "headers");
    CHECK(h != nullptr, "no headers in the log");
    if (!h) return;
    CHECK_STR(jstr(doc, *h, "host"), std::string("127.0.0.1:") + std::to_string(s.port));
    CHECK_STR(jstr(doc, *h, "connection"), "close");
    CHECK_STR(jstr(doc, *h, "content-length"), "0");
    CHECK_STR(jstr(doc, *h, "x-d2v-client"), "d2vita/0.1.0+ab12cd34ef56");
    CHECK(doc.member(*h, "accept-encoding") == nullptr, "the console must not ask for a content encoding");
}

static void t_http_response_framings() {
    if (!have_fake_api()) return;
    const std::string dir = test_dir("http_framing");
    // 1. chunked, split across writes, with an extension and a trailer;
    // 2. a 100 Continue before the real answer;
    // 3. no framing at all: the body ends with the connection;
    // 4. a chunked answer whose chunks arrive one byte at a time.
    const std::string exchanges =
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\nConnection: close\\r\\n\\r\\n"
        "5;name=a\\r\\nhello\\r\\n\"},{\"delay_ms\":30},"
        "{\"text\":\"6\\r\\n world\\r\\n0\\r\\nX-Trailer: v\\r\\n\\r\\n\"}]}," +
        raw_send("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nhi") + "," +
        raw_send("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nno framing at all") + "," +
        raw_send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n");
    FakeServer s;
    CHECK(s.start(dir, raw_script(exchanges)), "server did not start");
    if (s.pid < 0) return;
    auto net = make_posix_net();
    HttpClient http(*net);
    CHECK(http.set_base_url(s.base_url()), "base url");
    HttpResponse r;
    CHECK(run_request(http, "GET", "/a", &r) == HttpError::None, "chunked: %s", http.error_detail().c_str());
    CHECK_U64(r.status, 200);
    CHECK_STR(r.body, "hello world");
    CHECK(run_request(http, "GET", "/b", &r) == HttpError::None, "1xx: %s", http.error_detail().c_str());
    CHECK_U64(r.status, 201);
    CHECK_STR(r.body, "hi");
    CHECK(run_request(http, "GET", "/c", &r) == HttpError::None, "no framing: %s", http.error_detail().c_str());
    CHECK_STR(r.body, "no framing at all");
    CHECK(run_request(http, "GET", "/d", &r) == HttpError::None, "one-byte chunks: %s", http.error_detail().c_str());
    CHECK_STR(r.body, "abc");
    s.stop();
}

static void t_http_protocol_errors() {
    if (!have_fake_api()) return;
    const std::string dir = test_dir("http_errors");
    const std::string exchanges =
        // 1. a head far over the cap;
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\n\"},{\"text\":\"X-Pad: 0123456789abcdef\\r\\n\",\"repeat\":600},"
        "{\"text\":\"Content-Length: 2\\r\\n\\r\\nhi\"}]},"
        // 2. a body far over the cap;
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\nContent-Length: 999999\\r\\n\\r\\n\"},"
        "{\"text\":\"0123456789abcdef\",\"repeat\":64}]},"
        // 3. a chunked body over the cap;
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n\"},"
        "{\"text\":\"5000\\r\\n\"},{\"text\":\"0123456789abcdef\",\"repeat\":1280},{\"text\":\"\\r\\n0\\r\\n\\r\\n\"}]},"
        // 4. a body cut in the middle;
        + raw_send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a few bytes") + ","
        // 5. nothing at all;
        "{\"send\":[]},"
        // 6. a head that is not HTTP;
        + raw_send("nonsense\r\n\r\n") + ","
        // 7. both framings at once (request smuggling shape);
        + raw_send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nhi") + ","
        // 8. a bare-LF head;
        + raw_send("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi") + ","
        // 9. two different Content-Length values.
        + raw_send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 7\r\n\r\nhi");
    FakeServer s;
    CHECK(s.start(dir, raw_script(exchanges)), "server did not start");
    if (s.pid < 0) return;
    auto net = make_posix_net();
    HttpLimits lim;
    lim.idle_timeout_ms = 1500;
    lim.total_timeout_ms = 8000;
    HttpClient http(*net, lim);
    CHECK(http.set_base_url(s.base_url()), "base url");
    struct { const char* path; HttpError want; const char* what; } cases[] = {
        {"/head", HttpError::TooLarge, "oversized head"},
        {"/body", HttpError::TooLarge, "oversized body"},
        {"/chunked", HttpError::TooLarge, "oversized chunked body"},
        {"/cut", HttpError::Protocol, "body cut short"},
        {"/silent", HttpError::Protocol, "no answer"},
        {"/nonsense", HttpError::Protocol, "not HTTP"},
        {"/smuggle", HttpError::Protocol, "both framings"},
        {"/barelf", HttpError::Protocol, "bare LF head"},
        {"/twolengths", HttpError::Protocol, "two Content-Length values"},
    };
    for (const auto& c : cases) {
        HttpResponse r;
        const HttpError e = run_request(http, "GET", c.path, &r);
        CHECK(e == c.want, "%s: got %s (%s), want %s", c.what, http_error_name(e), http.error_detail().c_str(),
              http_error_name(c.want));
        CHECK_U64(r.status, 0);
    }
    s.stop();
}

static void t_http_timeout_and_stop() {
    if (!have_fake_api()) return;
    const std::string dir = test_dir("http_timeout");
    const std::string exchanges =
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\nContent-Length: 5\\r\\n\\r\\nab\"}],\"hang_ms\":4000},"
        "{\"send\":[{\"text\":\"HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nhi\"}],\"delay_ms\":3000}";
    FakeServer s;
    CHECK(s.start(dir, raw_script(exchanges)), "server did not start");
    if (s.pid < 0) return;
    auto net = make_posix_net();
    HttpLimits lim;
    lim.idle_timeout_ms = 400;
    lim.total_timeout_ms = 6000;
    lim.slice_ms = 50;
    HttpClient http(*net, lim);
    CHECK(http.set_base_url(s.base_url()), "base url");
    HttpResponse r;
    const uint64_t t0 = net->now_ms();
    const HttpError e = run_request(http, "GET", "/hang", &r);
    const uint64_t took = net->now_ms() - t0;
    CHECK(e == HttpError::Timeout, "a peer that stops mid-body: %s (%s)", http_error_name(e),
          http.error_detail().c_str());
    CHECK(took >= 300 && took < 3000, "the idle timeout took %" PRIu64 " ms", took);

    // The stop flag wins over the wait, and quickly.
    static bool stop_now = true;
    http.set_stop([](void*) { return stop_now; }, nullptr);
    const uint64_t t1 = net->now_ms();
    const HttpError e2 = run_request(http, "GET", "/slow", &r);
    CHECK(e2 == HttpError::Cancelled, "stop flag: %s", http_error_name(e2));
    CHECK(net->now_ms() - t1 < 1000, "cancelling took %" PRIu64 " ms", net->now_ms() - t1);
    stop_now = false;
    http.set_stop(nullptr, nullptr);
    s.stop();
}

static void t_http_streamed_request_body() {
    if (!have_fake_api()) return;
    const std::string dir = test_dir("http_body");
    const std::string ok = raw_send("HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok");
    // The third exchange answers 409 without reading the body, as the API does
    // for a piece it already holds.
    const std::string exchanges = ok + "," + ok + "," +
        "{\"answer_before_body\":true,\"send\":[{\"text\":\"HTTP/1.1 409 Conflict\\r\\nContent-Length: 7\\r\\n\\r\\nexists!\"}]}";
    FakeServer s;
    CHECK(s.start(dir, raw_script(exchanges)), "server did not start");
    if (s.pid < 0) return;
    auto net = make_posix_net();
    HttpClient http(*net);
    CHECK(http.set_base_url(s.base_url()), "base url");

    BodyString body;
    body.data.assign(200 * 1024, 'p');
    for (size_t i = 0; i < body.data.size(); i += 4096) body.data[i] = 'M';
    body.pieces = 50;
    HttpResponse r;
    CHECK(run_request(http, "PUT", "/piece", &r, {}, &body) == HttpError::None, "streamed body: %s",
          http.error_detail().c_str());
    CHECK_U64(r.status, 201);
    CHECK_U64(r.body_bytes_sent, body.data.size());

    // A source that stops early never sends a body that lies about its length.
    BodyString shortb;
    shortb.data.assign(1024, 'x');
    shortb.short_write = true;
    const HttpError e = run_request(http, "PUT", "/short", &r, {}, &shortb);
    CHECK(e == HttpError::Body, "a short body gave %s (%s)", http_error_name(e), http.error_detail().c_str());

    // An answer that arrives while the body is going out stops the upload.
    // The server answers as soon as it has the head; the pause after the first
    // piece is what makes that answer sure to be there before the second one,
    // whatever the socket buffers hold.
    BodyString big;
    big.data.assign(400 * 1024, 'z');
    big.pieces = 100;
    big.pause_after_first_ms = 200;
    const HttpError e2 = run_request(http, "PUT", "/early", &r, {}, &big);
    CHECK(e2 == HttpError::None, "early answer: %s (%s)", http_error_name(e2), http.error_detail().c_str());
    CHECK_U64(r.status, 409);
    CHECK_STR(r.body, "exists!");
    CHECK(r.early, "the answer was not marked as early");
    CHECK(r.body_bytes_sent < big.data.size(), "the whole body was sent anyway (%" PRIu64 ")", r.body_bytes_sent);
    s.stop();

    const std::vector<std::string> lines = s.log_lines();
    CHECK(lines.size() == 3, "%zu request(s) logged", lines.size());
    if (lines.size() < 3) return;
    JsonDoc doc;
    CHECK(doc.parse(lines[0], JsonLimits::vectors()), "log: %s", doc.error().c_str());
    CHECK_U64(jnum(doc, *doc.root(), "body_bytes"), 200 * 1024);
    CHECK_U64(jnum(doc, *doc.root(), "content_length"), 200 * 1024);
    JsonDoc doc2;
    CHECK(doc2.parse(lines[1], JsonLimits::vectors()), "log: %s", doc2.error().c_str());
    const JsonNode* violations = doc2.member(*doc2.root(), "violations");
    CHECK(violations && violations->count == 1, "a short body must be seen as one violation");
}

// ---------------------------------------------------------------- cr_api
// The bodies of response-sig.v1.json are real answers: they must read, and
// everything a v1 Worker never writes must be refused (the console then treats
// the answer like a network failure).

// body_utf8 of a case of response-sig.v1.json.
static std::string signed_body(const char* name) {
    JsonDoc doc;
    CHECK(doc.parse(vector_file("response-sig.v1.json"), JsonLimits::vectors()), "response-sig: %s",
          doc.error().c_str());
    const JsonNode* cases = doc.member(*doc.root(), "cases");
    for (const JsonNode* c = cases ? doc.first(*cases) : nullptr; c; c = doc.next(*c))
        if (jstr(doc, *c, "name") == name) return jstr(doc, *c, "body_utf8");
    CHECK(false, "no vector body named %s", name);
    return std::string();
}

static void t_api_contract_bodies() {
    const std::string report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCD";
    Decision d;
    CHECK(parse_decision(signed_body("decision_upload"), &d), "the decision of the vectors did not read");
    CHECK_STR(d.report_id, report_id);
    CHECK_STR(d.signature, "SZYGIRBIXGHOM3AH");
    CHECK(d.upload, "action upload");
    CHECK_STR(d.token, "eyJyIjoiMDFKOVo2VDRROE0zSzdWMkI1TjBYV0FZQ0QiLCJlIjoxNzg5Mjg2MDAwfQ.q8vLbWEt");
    CHECK_U64(d.expires_unix, 1789286000);
    CHECK(d.artifacts.size() == 3, "%zu pieces requested", d.artifacts.size());
    if (d.artifacts.size() == 3) {
        CHECK_STR(d.artifacts[0].name, "crash_txt");
        CHECK_U64(d.artifacts[0].max_bytes, 65536);
        CHECK_STR(d.artifacts[2].name, "boot_progress");
        CHECK_U64(d.artifacts[2].max_bytes, 327680);
    }
    CHECK(!d.has_retry_after && !d.has_disable_until, "null means absent");

    Decision c;
    CHECK(parse_decision(signed_body("decision_count_only"), &c), "count_only decision");
    CHECK(!c.upload && c.artifacts.empty() && c.token.empty(), "count_only carries no upload");

    ArtifactStored stored;
    CHECK(parse_artifact_stored(signed_body("artifact_stored"), &stored), "ArtifactStored");
    CHECK(stored.report_id == report_id && stored.name == "crash_txt" && stored.bytes == 2210, "stored fields");

    CompleteResponse done;
    CHECK(parse_complete_response(signed_body("complete_response"), &done), "CompleteResponse");
    CHECK(done.report_id == report_id && done.sample_stored, "complete fields");

    ErrorBody e;
    CHECK(parse_error_body(signed_body("error_rate_limited"), &e), "rate_limited");
    CHECK(e.error == ApiError::RateLimited && e.has_retry_after && e.retry_after_s == 32400, "rate_limited fields");
    CHECK_U64(api_error_status(e.error), 429);
    CHECK(parse_error_body(signed_body("error_not_accepting"), &e), "not_accepting");
    CHECK(e.error == ApiError::NotAccepting && e.has_disable_until && e.disable_until_unix == 1789372800,
          "not_accepting fields");
    CHECK(parse_error_body(signed_body("error_exists"), &e), "exists");
    CHECK(e.error == ApiError::Exists && e.report_id == report_id && e.artifact == "crash_txt", "exists fields");
    CHECK(parse_error_body(signed_body("error_non_ascii_message"), &e), "a message outside ASCII");
    CHECK(e.error == ApiError::InvalidPayload, "invalid_payload");

    // The empty body of the vectors is a valid signature over nothing, and no
    // schema at all.
    const std::string empty = signed_body("empty_body");
    CHECK(!parse_decision(empty, &d) && !parse_error_body(empty, &e), "an empty body read as a schema");
}

static void t_api_refuses_what_v1_forbids() {
    const char* kDecision =
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"signature\":\"SZYGIRBIXGHOM3AH\","
        "\"action\":\"upload\",\"upload\":{\"token\":\"t\",\"expires_unix\":1789286000,"
        "\"artifacts\":[{\"name\":\"crash_txt\",\"max_bytes\":65536}]},"
        "\"retry_after_s\":null,\"disable_until_unix\":null}";
    Decision d;
    CHECK(parse_decision(kDecision, &d), "the reference decision must read");
    const std::string ok = kDecision;
    struct { std::string body; const char* what; } bad[] = {
        {ok.substr(0, ok.size() - 1) + ",\"extra\":1}", "an unknown member"},
        {"{\"v\":2," + ok.substr(6), "v other than 1"},
        {ok.substr(0, ok.find(",\"disable_until_unix\":null")) + "}", "a missing required member"},
        {std::string(kDecision).replace(ok.find("\"action\":\"upload\""), 17, "\"action\":\"count_only\""),
         "count_only with an upload grant"},
        {std::string(kDecision).replace(ok.find("SZYGIRBIXGHOM3AH"), 16, "S4KQ7M2X9D3T8B6A"),
         "a signature id with 8 and 9 (the design's illustrative one)"},
        {std::string(kDecision).replace(ok.find("01J9Z6T4Q8M3K7V2B5N0XWAYCD"), 26, "81J9Z6T4Q8M3K7V2B5N0XWAYCD"),
         "a report id starting with 8"},
        {std::string(kDecision).replace(ok.find("01J9Z6T4Q8M3K7V2B5N0XWAYCD"), 26, "01j9z6t4q8m3k7v2b5n0xwaycd"),
         "a lowercase report id"},
        {std::string(kDecision).replace(ok.find("65536"), 5, "65537"), "max_bytes over the crash_txt cap"},
        {std::string(kDecision).replace(ok.find("crash_txt"), 9, "crash_zip"), "an artifact name outside the enum"},
        {std::string(kDecision).replace(ok.find("\"token\":\"t\""), 11, "\"token\":\"a b\""), "a token with a space"},
        {std::string(kDecision).replace(ok.find("\"token\":\"t\""), 11, "\"token\":\"\""), "an empty token"},
        {std::string(kDecision).replace(ok.find("1789286000"), 10, "\"1789286000\""), "expires_unix as a string"},
        {std::string(kDecision).replace(ok.find("1789286000"), 10, "4294967296"), "expires_unix over 2^32-1"},
        {std::string(kDecision).replace(ok.find("[{\"name\":\"crash_txt\",\"max_bytes\":65536}]"), 40,
                                        "[{\"name\":\"crash_txt\",\"max_bytes\":65536},"
                                        "{\"name\":\"crash_txt\",\"max_bytes\":65536}]"),
         "the same artifact twice"},
        {std::string(kDecision).replace(ok.find("[{\"name\":\"crash_txt\",\"max_bytes\":65536}]"), 40, "[]"),
         "an empty artifact list"},
        {std::string(kDecision).replace(ok.find("{\"name\":\"crash_txt\",\"max_bytes\":65536}"), 38,
                                        "{\"name\":\"crash_txt\",\"max_bytes\":65536,\"sha256\":\"ab\"}"),
         "an unknown member inside a piece"},
    };
    for (const auto& c : bad) CHECK(!parse_decision(c.body, &d), "decision accepted with %s", c.what);
    // A number written with a fraction is still an integer (JSON Schema).
    CHECK(parse_decision(std::string(kDecision).replace(ok.find("65536"), 5, "6.5536e4"), &d),
          "max_bytes written as 6.5536e4 refused");

    ArtifactStored stored;
    const std::string stored_ok =
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"name\":\"crash_txt\",\"bytes\":2298}";
    CHECK(parse_artifact_stored(stored_ok, &stored), "the reference ArtifactStored must read");
    const char* bad_stored[] = {
        // What the live API adds today: an extra member is a v2 change.
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"name\":\"crash_txt\",\"bytes\":2298,"
        "\"sha256\":\"0f1e\"}",
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"name\":\"crash_txt\",\"bytes\":87}",
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"name\":\"crash_txt\",\"bytes\":65537}",
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"name\":\"crash_txt\"}",
    };
    for (const char* b : bad_stored) CHECK(!parse_artifact_stored(b, &stored), "ArtifactStored accepted: %s", b);

    CompleteResponse done;
    CHECK(!parse_complete_response("{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\","
                                   "\"sample_stored\":\"true\"}", &done), "sample_stored as a string");
    CHECK(parse_complete_response("{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\","
                                  "\"sample_stored\":false}", &done), "sample_stored false");

    ErrorBody e;
    const char* bad_errors[] = {
        // Conditional fields of the schema.
        "{\"v\":1,\"error\":\"rate_limited\",\"message\":\"wait\"}",
        "{\"v\":1,\"error\":\"not_accepting\",\"message\":\"paused\"}",
        "{\"v\":1,\"error\":\"exists\",\"message\":\"stored\",\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\"}",
        "{\"v\":1,\"error\":\"incomplete\",\"message\":\"missing\"}",
        // Codes and members outside v1 (what the live API sends today).
        "{\"v\":1,\"error\":\"storage_unavailable\",\"message\":\"retry\"}",
        "{\"v\":1,\"error\":\"length_required\",\"message\":\"no length\"}",
        "{\"v\":1,\"error\":\"internal\",\"message\":\"oops\"}",
        "{\"v\":1,\"error\":\"exists\",\"message\":\"stored\",\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\","
        "\"name\":\"crash_txt\"}",
        "{\"v\":1,\"error\":\"incomplete\",\"message\":\"missing\",\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\","
        "\"missing\":[\"crash_log\"]}",
        "{\"v\":1,\"error\":\"rate_limited\",\"message\":\"wait\",\"retry_after_s\":-1}",
        "{\"v\":1,\"error\":\"rate_limited\",\"message\":\"wait\",\"retry_after_s\":4294967296}",
    };
    for (const char* b : bad_errors) CHECK(!parse_error_body(b, &e), "ErrorBody accepted: %s", b);
    // message is at most 500 code points, counted as ajv does.
    std::string long_message = "{\"v\":1,\"error\":\"invalid_payload\",\"message\":\"";
    for (int i = 0; i < 500; ++i) long_message += "\xc3\xa9";       // 500 code points, 1000 bytes
    CHECK(parse_error_body(long_message + "\"}", &e), "a 500 code point message refused");
    CHECK(!parse_error_body(long_message + "\xc3\xa9\"}", &e), "a 501 code point message accepted");
}

// ---------------------------------------------------------------- cr_upload
// The state machine of spec §4.8 against fake_crash_api.py in contract mode:
// it signs its answers with a key of response-sig.v1.json, opens the pieces
// with a key of sealed.v1.json, and records every breach of the contract it
// sees as a violation.

static int64_t g_now_unix = 1789290000;
static bool g_stop_flag = false;
static uint32_t g_random_state = 0x13579bdf;

static int64_t fake_now(void*) { return g_now_unix; }
static bool fake_stop(void*) { return g_stop_flag; }
static bool fake_random(uint8_t* out, size_t n, void*) {
    for (size_t i = 0; i < n; ++i) {
        g_random_state = g_random_state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(g_random_state >> 24);
    }
    return true;
}

// Keys taken from the contract vectors: the API signs with the seed of
// response-sig.v1.json, the maintainer's X25519 pair comes from
// sealed.v1.json, so the server can open what the console seals.
static const std::string& vector_key(const char* file, const char* list, const char* name, const char* field) {
    static std::vector<std::pair<std::string, std::string>> cache;
    const std::string key = std::string(file) + "/" + name + "/" + field;
    for (const auto& e : cache)
        if (e.first == key) return e.second;
    JsonDoc doc;
    CHECK(doc.parse(vector_file(file), JsonLimits::vectors()), "%s: %s", file, doc.error().c_str());
    std::string value;
    const JsonNode* cases = doc.member(*doc.root(), list);
    for (const JsonNode* c = cases ? doc.first(*cases) : nullptr; c; c = doc.next(*c))
        if (jstr(doc, *c, "name") == name) value = jstr(doc, *c, field);
    CHECK(!value.empty(), "%s: no %s in case %s", file, field, name);
    cache.push_back({key, value});
    return cache.back().second;
}

static std::string signing_seed_hex() {
    return vector_key("response-sig.v1.json", "cases", "decision_upload", "seed_hex");
}
static std::string signing_public_hex() {
    return vector_key("response-sig.v1.json", "cases", "decision_upload", "public_key_hex");
}
static std::string other_seed_hex() {
    return vector_key("response-sig.v1.json", "cases", "error_non_ascii_message", "seed_hex");
}
static std::string recipient_sk_hex() {
    return vector_key("sealed.v1.json", "cases", "one_byte", "recipient_sk_hex");
}
static std::string recipient_pk_hex() {
    return vector_key("sealed.v1.json", "cases", "one_byte", "recipient_pk_hex");
}

static std::string api_script(const std::string& overrides, const std::string& extra = "") {
    return "{\"mode\":\"api\",\"signing_seed_hex\":\"" + signing_seed_hex() + "\",\"other_signing_seed_hex\":\"" +
           other_seed_hex() + "\",\"recipient_sk_hex\":\"" + recipient_sk_hex() + "\",\"overrides\":[" + overrides +
           "]" + extra + "}";
}

static const char* kTestBuild = "0.1.0+ab12cd34ef56";
// The Halt code of the reports the tests build. The live leg varies it so
// that every run is a new signature for the API (a known one is only counted).
static int g_halt_code = 1420;
static const char* kTestInstall = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e";

struct Piece {
    std::string name;    // claim artifact name
    std::string file;    // file name in the report directory
    std::string data;
};

// A report in the outbox, as the collector leaves it. A host_fault one when
// the pieces hold a dump: only that kind may offer one (claim.v1).
static void make_report(IoApi& io, Outbox& ob, const std::string& root, const std::string& id,
                        const std::vector<Piece>& pieces, const char* consent = "granted",
                        const char* build_id = kTestBuild, int attempts = 0, int64_t next_attempt = 0,
                        int64_t created = 0) {
    const std::string dir = root + "/" + id;
    io.mkdir(root);
    CHECK(io.mkdir(dir), "cannot create %s", dir.c_str());
    bool has_dump = false;
    for (const Piece& p : pieces) has_dump = has_dump || p.name == "dump";
    ReportRecord r;
    r.report_id = id;
    r.session_id = "0123456789abcdef0123456789abcdef";
    r.build_id = build_id;
    r.kind = has_dump ? Kind::HostFault : Kind::Halt;
    r.features_json = has_dump
        ? std::string(
          "{\"stop_reason\":\"0x30004\",\"thread_name\":\"d2_main\","
          "\"pc\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24184\"},"
          "\"lr\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24101\"},"
          "\"guest_frames\":[\"Game+0x1fedf4\"],\"redaction\":\"clean\"}")
        : "{\"code\":" + std::to_string(g_halt_code) + ",\"location\":null,\"frames\":[\"Game+0x1fedf4\"]}";
    r.started_unix = g_now_unix - 900;
    r.uptime_s = 900;
    r.online = false;
    r.fw = "3.65";
    r.redactions = 0;
    r.dump = has_dump ? "included" : "none";
    for (const Piece& p : pieces) {
        CHECK(io.write_file(dir + "/" + p.file, p.data), "cannot write %s", p.file.c_str());
        r.artifacts.push_back(ArtifactFile{p.name, p.file, p.data.size()});
    }
    CHECK(io.write_file(dir + "/evidence.txt", serialize_report_record(r)), "cannot write evidence.txt");
    ReportState st;
    st.created_unix = created ? created : g_now_unix - 60;
    st.consent = consent;
    st.attempts = attempts;
    st.next_attempt_unix = next_attempt;
    CHECK(ob.save_state(id, st), "cannot write state.txt");
}

static std::vector<Piece> two_pieces() {
    std::vector<Piece> v;
    Piece log;
    log.name = "crash_log";
    log.file = "crash_log.txt";
    for (int i = 0; i < 200; ++i) log.data += "crash.log line " + std::to_string(i) + "\n";
    Piece progress;
    progress.name = "boot_progress";
    progress.file = "boot_progress.txt";
    for (int i = 0; i < 4000; ++i) progress.data += "[  " + std::to_string(i) + ".00s] boot progress line\n";
    v.push_back(log);
    v.push_back(progress);
    return v;
}

// "claim/200 artifact:crash_log/201 complete/200", plus the violations the
// server recorded.
static std::string log_summary(const FakeServer& s, int* violations, std::vector<std::string>* lines_out = nullptr) {
    std::string out;
    *violations = 0;
    const std::vector<std::string> lines = s.log_lines();
    if (lines_out) *lines_out = lines;
    for (const std::string& line : lines) {
        JsonDoc doc;
        if (!doc.parse(line, JsonLimits::vectors())) {
            out += " ?";
            continue;
        }
        const JsonNode* root = doc.root();
        const JsonNode* route = doc.member(*root, "route");
        const JsonNode* status = doc.member(*root, "status");
        const JsonNode* artifact = doc.member(*root, "artifact");
        const JsonNode* v = doc.member(*root, "violations");
        if (!out.empty()) out += " ";
        out += route ? route->text : "?";
        if (artifact) out += ":" + artifact->text;
        out += "/";
        out += status && status->is_u64() ? std::to_string(status->u64()) : std::string("cut");
        if (v) {
            *violations += (int)v->count;
            for (const JsonNode* m = doc.first(*v); m; m = doc.next(*m))
                std::printf("    contract violation seen by the server: %s\n", m->text.c_str());
        }
    }
    return out;
}

struct UploadFixture {
    std::string dir, root;
    std::unique_ptr<IoApi> io;
    std::unique_ptr<NetApi> net;
    FakeServer server;
    UploadConfig cfg;
    UploadKeys keys;
    UploadEnv env;

    bool start(const char* name, const std::string& overrides, const std::string& extra = "") {
        dir = test_dir(name);
        root = dir + "/outbox";
        io = make_posix_io();
        net = make_posix_net();
        io->mkdir(root);
        if (!server.start(dir, api_script(overrides, extra), true)) {
            CHECK(false, "the fake API did not start");
            return false;
        }
        cfg = UploadConfig();
        cfg.base_url = server.base_url();
        cfg.install_id = kTestInstall;
        cfg.channel = "test";
        cfg.platform_model = "vita";
        cfg.retry_delay_ms = 1;              // the tests do not wait for real
        cfg.http.idle_timeout_ms = 4000;
        cfg.http.total_timeout_ms = 20000;
        const std::string pk = from_hex(signing_public_hex()), rpk = from_hex(recipient_pk_hex());
        std::memcpy(keys.response_pk, pk.data(), sizeof keys.response_pk);
        std::memcpy(keys.recipient_pk, rpk.data(), sizeof keys.recipient_pk);
        env = UploadEnv();
        env.now_unix = fake_now;
        env.random_bytes = fake_random;
        env.should_stop = fake_stop;
        return true;
    }
    void stop() { server.stop(); }
};

static void t_upload_count_only() {
    if (!have_fake_api()) return;
    UploadFixture f;
    if (!f.start("up_count_only", "{\"route\":\"claim\",\"action\":\"count_only\"}")) return;
    Outbox ob(*f.io, f.root);
    const std::string id = "01M2CT6880000G40R40M30E201";
    make_report(*f.io, ob, f.root, id, two_pieces());
    Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
    const UploadStats s = up.run();
    f.stop();
    CHECK(s.deleted == 1 && s.completed == 0 && s.failed == 0, "deleted %d, completed %d, failed %d (%s)", s.deleted,
          s.completed, s.failed, up.detail().c_str());
    CHECK(ob.list().empty(), "a counted report must be deleted");
    int violations = 0;
    std::vector<std::string> lines;
    CHECK_STR(log_summary(f.server, &violations, &lines), "claim/200");
    CHECK(violations == 0, "%d contract violation(s)", violations);
    // What the console sent: the two headers, the claim of the record.
    if (lines.empty()) return;
    JsonDoc doc;
    CHECK(doc.parse(lines[0], JsonLimits::vectors()), "log: %s", doc.error().c_str());
    const JsonNode* h = doc.member(*doc.root(), "headers");
    const JsonNode* claim = doc.member(*doc.root(), "claim");
    CHECK(h && claim, "no headers or claim in the log");
    if (!h || !claim) return;
    CHECK_STR(jstr(doc, *h, "x-d2v-client"), std::string("d2vita/") + kTestBuild);
    CHECK_STR(jstr(doc, *h, "x-d2v-install"), kTestInstall);
    CHECK_STR(jstr(doc, *h, "content-type"), "application/json");
    CHECK(doc.member(*h, "content-length") != nullptr, "a claim without Content-Length");
    CHECK_STR(jstr(doc, *claim, "report_id"), id);
    CHECK_STR(jstr(doc, *claim, "build_id"), kTestBuild);
    CHECK_STR(jstr(doc, *claim, "install_id"), kTestInstall);
    CHECK_STR(jstr(doc, *claim, "channel"), "test");
    CHECK_STR(jstr(doc, *claim, "kind"), "halt");
    const JsonNode* artifacts = doc.member(*claim, "artifacts");
    CHECK(artifacts && artifacts->count == 2, "the claim must offer the two pieces");
    // The announced size is the sealed size, not the size on disk.
    const JsonNode* first = artifacts ? doc.first(*artifacts) : nullptr;
    CHECK(first && jnum(doc, *first, "bytes") == sealed_size_for(two_pieces()[0].data.size()),
          "the offered size is not the sealed size");
}

static void t_upload_pieces_and_complete() {
    if (!have_fake_api()) return;
    UploadFixture f;
    if (!f.start("up_pieces", "")) return;       // no override: the API asks for what is offered
    Outbox ob(*f.io, f.root);
    const std::string id = "01M2CT6880000G40R40M30E202";
    const std::vector<Piece> pieces = two_pieces();
    make_report(*f.io, ob, f.root, id, pieces);
    Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
    const UploadStats s = up.run();
    f.stop();
    CHECK(s.completed == 1 && s.artifacts_stored == 2 && s.failed == 0, "completed %d, pieces %d (%s)", s.completed,
          s.artifacts_stored, up.detail().c_str());
    CHECK(ob.list().empty(), "a completed report must be deleted");
    int violations = 0;
    std::vector<std::string> lines;
    CHECK_STR(log_summary(f.server, &violations, &lines),
              "claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/200");
    CHECK(violations == 0, "%d contract violation(s)", violations);
    // The pieces the server opened are exactly the files of the report.
    for (const Piece& p : pieces) {
        const std::string opened = get_file(f.server.artifacts_dir + "/" + id + "." + p.name);
        CHECK(opened == p.data, "%s: opened %zu bytes, the file holds %zu", p.name.c_str(), opened.size(),
              p.data.size());
    }
    // The upload headers and the complete body.
    CHECK(lines.size() == 4, "%zu requests", lines.size());
    if (lines.size() != 4) return;
    JsonDoc put;
    CHECK(put.parse(lines[1], JsonLimits::vectors()), "log: %s", put.error().c_str());
    const JsonNode* h = put.member(*put.root(), "headers");
    CHECK(h != nullptr, "no headers");
    if (h) {
        const std::string auth = jstr(put, *h, "authorization");
        CHECK(auth.size() > 11 && auth.compare(0, 11, "D2V-Upload ") == 0,
              "the piece must carry the upload token, not \"%s\"", auth.c_str());
        CHECK_STR(jstr(put, *h, "content-type"), "application/octet-stream");
        CHECK_U64(jnum(put, *put.root(), "content_length"), sealed_size_for(pieces[0].data.size()));
        CHECK_U64(jnum(put, *put.root(), "plain_bytes"), pieces[0].data.size());
    }
    JsonDoc done;
    CHECK(done.parse(lines[3], JsonLimits::vectors()), "log: %s", done.error().c_str());
    const JsonNode* body = done.member(*done.root(), "complete_body");
    CHECK(body != nullptr, "complete without a body");
    if (body) {
        CHECK(jnum(done, *body, "v") == 1, "complete body v");
        const JsonNode* names = done.member(*body, "artifacts");
        CHECK(names && names->count == 2, "complete must list the pieces it uploaded");
        if (names && names->count == 2) {
            CHECK_STR(done.first(*names)->text, "crash_log");
            CHECK_STR(done.next(*done.first(*names))->text, "boot_progress");
        }
    }
}

static void t_upload_gates() {
    if (!have_fake_api()) return;
    {   // 429: the whole outbox waits, the report keeps its attempts.
        UploadFixture f;
        if (!f.start("up_429", "{\"route\":\"claim\",\"status\":429,\"error\":\"rate_limited\","
                               "\"retry_after_s\":3600,\"bind_report\":true}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E203";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        CHECK(s.last == ReportOutcome::Gated, "outcome %s (%s)", report_outcome_name(s.last), up.detail().c_str());
        CHECK_U64(up.gate().not_before_unix, (uint64_t)(g_now_unix + 3600));
        CHECK_STR(up.gate().reason, "rate_limited");
        OutboxGate saved;
        CHECK(parse_outbox_gate(get_file(f.root + "/state.txt"), &saved) &&
              saved.not_before_unix == g_now_unix + 3600, "the gate must be written next to the reports");
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(id, &rec, &st) && st.attempts == 0, "a rate limit is not the report's fault");
        // A second run sends nothing at all.
        Uploader again(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s2 = again.run();
        f.stop();
        CHECK(s2.requests == 0 && s2.considered == 0, "%d request(s) while the outbox waits", s2.requests);
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations), "claim/429");
        CHECK(ob.list().size() == 1, "the report must stay");
    }
    {   // 503: an absolute date, and an unsigned 503 changes nothing.
        UploadFixture f;
        if (!f.start("up_503", "{\"route\":\"claim\",\"status\":503,\"error\":\"not_accepting\","
                               "\"disable_until_unix\":1789372800,\"unsigned\":true},"
                               "{\"route\":\"claim\",\"status\":503,\"error\":\"not_accepting\","
                               "\"disable_until_unix\":1789372800}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E204";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        // The unsigned answer is a network failure: retried inside the run,
        // and the second (signed) one closes the gate.
        CHECK(s.last == ReportOutcome::Gated, "outcome %s (%s)", report_outcome_name(s.last), up.detail().c_str());
        CHECK_U64(up.gate().not_before_unix, 1789372800);
        CHECK(exists(f.root + "/state.txt"), "the gate must be written");
        f.stop();
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations), "claim/503 claim/503");
    }
    {   // An unsigned 503 alone must leave no gate behind.
        UploadFixture f;
        if (!f.start("up_503_unsigned", "{\"route\":\"claim\",\"status\":503,\"error\":\"not_accepting\","
                                        "\"disable_until_unix\":1789372800,\"unsigned\":true,\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E205";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.failed == 1 && s.last == ReportOutcome::Failed, "outcome %s (%s)", report_outcome_name(s.last),
              up.detail().c_str());
        CHECK(!exists(f.root + "/state.txt"), "an unsigned 503 must not hold the outbox back");
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(id, &rec, &st) && st.attempts == 1 && st.next_attempt_unix > g_now_unix,
              "a failed send costs one attempt and a wait");
    }
}

static void t_upload_failures_and_retries() {
    if (!have_fake_api()) return;
    {   // A 500 on the claim, then the real answer: one report, two requests.
        UploadFixture f;
        if (!f.start("up_5xx", "{\"route\":\"claim\",\"status\":500,\"error\":\"internal_error\"}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E206";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.completed == 1, "completed %d (%s)", s.completed, up.detail().c_str());
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations),
                  "claim/500 claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/200");
        CHECK(violations == 0, "%d contract violation(s)", violations);
    }
    {   // A connection cut in the middle of the answer to a piece: the retry
        // meets 409 exists, which counts as stored.
        UploadFixture f;
        if (!f.start("up_cut", "{\"route\":\"artifact\",\"cut\":\"mid_body\"}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E207";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.completed == 1, "completed %d (%s)", s.completed, up.detail().c_str());
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations),
                  "claim/200 artifact:crash_log/201 artifact:crash_log/409 artifact:boot_progress/201 complete/200");
        CHECK(violations == 0, "%d contract violation(s)", violations);
    }
    {   // 409 incomplete: upload again, complete again.
        UploadFixture f;
        if (!f.start("up_incomplete", "{\"route\":\"complete\",\"status\":409,\"error\":\"incomplete\","
                                      "\"bind_report\":true}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E208";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.completed == 1, "completed %d (%s)", s.completed, up.detail().c_str());
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations),
                  "claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/409 "
                  "artifact:crash_log/409 artifact:boot_progress/409 complete/200");
        CHECK(violations == 0, "%d contract violation(s)", violations);
    }
    {   // A decision that also asks for a wait: the pieces of this report
        // still go out, and no other claim leaves before then (decision.v1).
        UploadFixture f;
        if (!f.start("up_decision_wait", "{\"route\":\"claim\",\"retry_after_s\":600}")) return;
        Outbox ob(*f.io, f.root);
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E250", two_pieces());
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E251", two_pieces(), "granted", kTestBuild, 0, 0,
                    g_now_unix - 50);
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.completed == 1, "completed %d (%s)", s.completed, up.detail().c_str());
        CHECK_U64(up.gate().not_before_unix, (uint64_t)(g_now_unix + 600));
        CHECK(ob.list().size() == 1, "the second report must stay for later");
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations),
                  "claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/200");
        CHECK(violations == 0, "%d contract violation(s)", violations);
    }
    {   // The answer to the claim never arrives: next boot replays the same
        // report_id and the API hands back the decision it stored.
        UploadFixture f;
        if (!f.start("up_replay", "{\"route\":\"claim\",\"cut\":\"mid_body\"}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E260";
        make_report(*f.io, ob, f.root, id, two_pieces());
        UploadConfig once = f.cfg;
        once.max_request_retries = 0;              // one try, as a boot with a flaky link
        Uploader first(*f.io, *f.net, ob, once, f.keys, f.env);
        const UploadStats s1 = first.run();
        CHECK(s1.failed == 1 && s1.completed == 0, "first boot: failed %d (%s)", s1.failed, first.detail().c_str());
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(id, &rec, &st) && st.attempts == 1 && st.next_attempt_unix > g_now_unix,
              "the report must wait before the next try");
        // Too early: nothing leaves.
        Uploader early(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s2 = early.run();
        CHECK(s2.requests == 0 && s2.skipped == 1, "%d request(s) before next_attempt_unix", s2.requests);
        // Later: the claim is replayed and the report goes through.
        const int64_t saved_now = g_now_unix;
        g_now_unix += 400;
        Uploader later(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s3 = later.run();
        g_now_unix = saved_now;
        f.stop();
        CHECK(s3.completed == 1, "second boot: completed %d (%s)", s3.completed, later.detail().c_str());
        CHECK(ob.list().empty(), "the report must be gone");
        int violations = 0;
        std::vector<std::string> lines;
        CHECK_STR(log_summary(f.server, &violations, &lines),
                  "claim/200 claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/200");
        CHECK(violations == 0, "%d contract violation(s)", violations);
        if (lines.size() > 1) {
            JsonDoc doc;
            CHECK(doc.parse(lines[1], JsonLimits::vectors()), "log: %s", doc.error().c_str());
            CHECK(doc.member(*doc.root(), "replay") != nullptr, "the API did not see the second claim as a replay");
        }
    }
    {   // An answer that names another report is a network failure.
        UploadFixture f;
        if (!f.start("up_wrong_report", "{\"route\":\"claim\",\"report_id\":\"01M2CT6880000G40R40M30E299\","
                                        "\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E209";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.failed == 1 && s.completed == 0, "failed %d (%s)", s.failed, up.detail().c_str());
        CHECK(ob.list().size() == 1, "the report must stay");
        CHECK(up.detail().find("another report") != std::string::npos, "detail: %s", up.detail().c_str());
    }
    {   // A badly signed decision is refused the same way.
        UploadFixture f;
        if (!f.start("up_badsig", "{\"route\":\"claim\",\"bad_signature\":true,\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E20A";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.failed == 1, "failed %d (%s)", s.failed, up.detail().c_str());
        CHECK(up.detail().find("invalid signature") != std::string::npos, "detail: %s", up.detail().c_str());
        CHECK(ob.list().size() == 1, "the report must stay");
    }
    {   // 403 bad_token on a piece: the report waits for another boot.
        UploadFixture f;
        if (!f.start("up_bad_token", "{\"route\":\"artifact\",\"status\":403,\"error\":\"bad_token\","
                                     "\"bind_report\":true,\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E20B";
        make_report(*f.io, ob, f.root, id, two_pieces());
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.failed == 1 && s.artifacts_stored == 0, "failed %d (%s)", s.failed, up.detail().c_str());
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(id, &rec, &st) && st.attempts == 1, "one attempt spent");
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations), "claim/200 artifact:crash_log/403");
    }
}

static void t_upload_policy() {
    if (!have_fake_api()) return;
    {   // 403 unknown_build stops that build for the run, the other goes on.
        UploadFixture f;
        if (!f.start("up_unknown_build", "{\"route\":\"claim\",\"status\":403,\"error\":\"unknown_build\","
                                         "\"bind_report\":true},{\"route\":\"claim\",\"action\":\"count_only\"}"))
            return;
        Outbox ob(*f.io, f.root);
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E210", two_pieces(), "granted", "0.1.0+aaaaaaaaaaaa");
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E211", two_pieces(), "granted", "0.1.0+aaaaaaaaaaaa");
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E212", two_pieces(), "granted", "0.2.0+bbbbbbbbbbbb");
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.deleted == 1 && s.failed == 1, "deleted %d, failed %d (%s)", s.deleted, s.failed,
              up.detail().c_str());
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations), "claim/403 claim/200");
        CHECK(ob.list().size() == 2, "both reports of the unknown build must stay");
    }
    {   // At most three reports per boot.
        UploadFixture f;
        if (!f.start("up_three", "{\"route\":\"claim\",\"action\":\"count_only\",\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        for (int i = 0; i < 5; ++i) {
            const std::string id = std::string("01M2CT6880000G40R40M30E2") + (char)('0' + i) + "0";
            make_report(*f.io, ob, f.root, id, two_pieces(), "granted", kTestBuild, 0, 0, g_now_unix - 100 + i);
        }
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.deleted == 3 && s.attempted == 3, "deleted %d, attempted %d", s.deleted, s.attempted);
        CHECK(ob.list().size() == 2, "%zu report(s) left for the next boot", ob.list().size());
        int violations = 0;
        CHECK_STR(log_summary(f.server, &violations), "claim/200 claim/200 claim/200");
    }
    {   // Consent, attempts, age and next_attempt_unix: nothing leaves.
        UploadFixture f;
        if (!f.start("up_eligibility", "{\"route\":\"claim\",\"action\":\"count_only\",\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E220", two_pieces(), "pending");
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E221", two_pieces(), "denied");
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E222", two_pieces(), "granted", kTestBuild, 3);
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E223", two_pieces(), "granted", kTestBuild, 1,
                    g_now_unix + 600);
        make_report(*f.io, ob, f.root, "01M2CT6880000G40R40M30E224", two_pieces(), "granted", kTestBuild, 0, 0,
                    g_now_unix - 8 * 24 * 3600);
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        f.stop();
        CHECK(s.skipped == 5 && s.requests == 0, "skipped %d, %d request(s)", s.skipped, s.requests);
        CHECK(ob.list().size() == 5, "nothing must be deleted");
        CHECK(f.server.log_lines().empty(), "the server saw a request");
    }
    {   // The stop flag ends the run without spending an attempt.
        UploadFixture f;
        if (!f.start("up_stop", "{\"route\":\"claim\",\"action\":\"count_only\",\"count\":9}")) return;
        Outbox ob(*f.io, f.root);
        const std::string id = "01M2CT6880000G40R40M30E230";
        make_report(*f.io, ob, f.root, id, two_pieces());
        g_stop_flag = true;
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        const UploadStats s = up.run();
        g_stop_flag = false;
        f.stop();
        CHECK(s.last == ReportOutcome::Stopped && s.requests == 0, "outcome %s, %d request(s)",
              report_outcome_name(s.last), s.requests);
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(id, &rec, &st) && st.attempts == 0, "a teardown must cost nothing");
    }
}

// A report directory whose name is not a ReportId never reaches the wire: the
// id is pasted into the request target ("/v1/reports/<id>/complete") and into
// the claim, where claim.v1 requires ^[0-7][0-9A-HJKMNP-TV-Z]{25}$. Nothing
// will make such a report acceptable, so it is a permanent failure, like a
// claim that cannot be built.
static void t_upload_bad_report_id() {
    if (!have_fake_api()) return;
    UploadFixture f;
    if (!f.start("up_bad_id", "")) return;      // no override: the API accepts what is offered
    // Too short, lowercase, a space (it would cut the request line in two),
    // an excluded letter, and a first character over 7.
    // (None of them is a prefix of the valid id below: the log is searched for
    // them as plain text.)
    const char* bad[] = {"01M2CT6880000G40R40M30V", "01m2ct6880000g40r40m30e262", "01M2CT68 0000G40R40M30E263",
                         "01M2CT6880000G40R40M30I264", "91M2CT6880000G40R40M30E265"};
    const int nbad = (int)(sizeof bad / sizeof bad[0]);
    OutboxLimits lim;
    lim.max_reports = nbad + 1;                 // the run must reach the valid report
    Outbox ob(*f.io, f.root, lim);
    for (int i = 0; i < nbad; ++i)
        make_report(*f.io, ob, f.root, bad[i], two_pieces(), "granted", kTestBuild, 0, 0, g_now_unix - 200 + i);
    const std::string good = "01M2CT6880000G40R40M30E266";
    make_report(*f.io, ob, f.root, good, two_pieces(), "granted", kTestBuild, 0, 0, g_now_unix - 60);
    Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
    const UploadStats s = up.run();
    f.stop();
    CHECK(s.completed == 1 && s.failed == nbad, "completed %d, failed %d (%s)", s.completed, s.failed,
          up.detail().c_str());
    CHECK(s.requests == 4, "%d request(s): only the valid report may reach the API", s.requests);
    int violations = 0;
    std::vector<std::string> lines;
    CHECK_STR(log_summary(f.server, &violations, &lines),
              "claim/200 artifact:crash_log/201 artifact:boot_progress/201 complete/200");
    CHECK(violations == 0, "%d contract violation(s)", violations);
    for (const std::string& line : lines)
        for (int i = 0; i < nbad; ++i)
            CHECK(line.find(bad[i]) == std::string::npos, "a bad id reached the API: %s", bad[i]);
    // Each of them is done for: the outbox drops it at the next enforce.
    for (int i = 0; i < nbad; ++i) {
        ReportRecord rec;
        ReportState st;
        CHECK(ob.load(bad[i], &rec, &st) && st.attempts >= lim.max_attempts, "%s: attempts %d, no second try",
              bad[i], st.attempts);
    }
    CHECK(ob.enforce(g_now_unix) == nbad, "enforce must drop them all");
}

// A whole report with a 2 MiB dump, sent inside a thread with the 64 KiB
// stack of spec §4.9, with the heap peak measured: budget 1 MiB, everything
// freed at the end.
struct ThreadedRun {
    Uploader* up = nullptr;
    UploadStats stats;
};

static void* upload_thread(void* ud) {
    ThreadedRun* r = static_cast<ThreadedRun*>(ud);
    r->stats = r->up->run();
    return nullptr;
}

static void t_upload_budget_two_mib_dump() {
    if (!have_fake_api()) return;
    if (!kAllocCounter) return;                 // the counter is the measurement
    UploadFixture f;
    if (!f.start("up_budget", "")) return;
    Outbox ob(*f.io, f.root);
    const std::string id = "01M2CT6880000G40R40M30E240";
    std::vector<Piece> pieces;
    Piece dump;
    dump.name = "dump";
    dump.file = "dump.psp2dmp";
    dump.data.reserve(2 * 1024 * 1024);
    while (dump.data.size() < max_plain_for_sealed(kSealedCapDump)) dump.data += "\x1f\x8b\x08\x00DUMPBYTES";
    dump.data.resize(max_plain_for_sealed(kSealedCapDump));
    Piece log;
    log.name = "crash_log";
    log.file = "crash_log.txt";
    for (int i = 0; i < 400; ++i) log.data += "crash.log line " + std::to_string(i) + "\n";
    pieces.push_back(dump);
    pieces.push_back(log);
    make_report(*f.io, ob, f.root, id, pieces);

    // The console runs this in a host thread with a 64 KiB stack (spec §4.9).
    const size_t stack_bytes = 64 * 1024;
    void* stack = nullptr;
    bool in_thread = false;
    std::string detail;
    detail.reserve(256);                        // no allocation inside the window
    ThreadedRun run;
    const size_t before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    size_t peak = 0;
    {
        Uploader up(*f.io, *f.net, ob, f.cfg, f.keys, f.env);
        run.up = &up;
        if (::posix_memalign(&stack, (size_t)::sysconf(_SC_PAGESIZE), stack_bytes) == 0 && stack) {
            std::memset(stack, 0xA5, stack_bytes);
            pthread_attr_t attr;
            pthread_t thread;
            if (::pthread_attr_init(&attr) == 0) {
                if (::pthread_attr_setstack(&attr, stack, stack_bytes) == 0 &&
                    ::pthread_create(&thread, &attr, upload_thread, &run) == 0) {
                    ::pthread_join(thread, nullptr);
                    in_thread = true;
                }
                ::pthread_attr_destroy(&attr);
            }
        }
        if (!in_thread) {
            std::printf("    note: no 64 KiB thread here, the run stays on the main stack\n");
            run.stats = up.run();
        }
        peak = g_heap_peak - before;
        detail.assign(up.detail());
    }
    // Everything the send held is freed once the sender is gone (spec §4.9).
    const size_t leaked = g_heap_cur - before;
    f.stop();

    CHECK(run.stats.completed == 1 && run.stats.artifacts_stored == 2, "completed %d, pieces %d (%s)",
          run.stats.completed, run.stats.artifacts_stored, detail.c_str());
    int violations = 0;
    CHECK_STR(log_summary(f.server, &violations),
              "claim/200 artifact:dump/201 artifact:crash_log/201 complete/200");
    CHECK(violations == 0, "%d contract violation(s)", violations);
    const std::string opened = get_file(f.server.artifacts_dir + "/" + id + ".dump");
    CHECK(opened == dump.data, "the dump opened by the server differs (%zu bytes for %zu)", opened.size(),
          dump.data.size());
    size_t high_water = 0;
    if (in_thread && stack) {
        const uint8_t* bytes = (const uint8_t*)stack;
        size_t untouched = 0;
        while (untouched < stack_bytes && bytes[untouched] == 0xA5) ++untouched;
        high_water = stack_bytes - untouched;
    }
    std::printf("    upload of a %zu byte dump (sealed %" PRIu64 "): heap peak %zu bytes, %zu still held, "
                "stack high water %zu of %zu\n",
                dump.data.size(), sealed_size_for(dump.data.size()), peak, leaked, high_water, stack_bytes);
    CHECK(peak < 1024u * 1024u, "heap peak %zu over the 1 MiB budget of spec 4.9", peak);
    CHECK(leaked == 0, "%zu bytes still held after the sender is gone", leaked);
    if (stack) std::free(stack);
}

// ---------------------------------------------------------------- driver

struct TestCase { const char* name; void (*fn)(); };
static const TestCase kTests[] = {
    {"json_parse_shapes", t_json_parse_shapes},
    {"json_strings_and_numbers", t_json_strings_and_numbers},
    {"json_limits", t_json_limits},
    {"seal_contract_vectors", t_seal_contract_vectors},
    {"seal_sizes_and_failures", t_seal_sizes_and_failures},
    {"seal_injected_entropy", t_seal_injected_entropy},
    {"seal_bounded_memory", t_seal_bounded_memory},
    {"verify_contract_vectors", t_verify_contract_vectors},
    {"verify_signature_encoding", t_verify_signature_encoding},
    {"net_posix_basics", t_net_posix_basics},
    {"net_posix_failures", t_net_posix_failures},
    {"http_url_parsing", t_http_url_parsing},
    {"http_get_and_headers", t_http_get_and_headers},
    {"http_response_framings", t_http_response_framings},
    {"http_protocol_errors", t_http_protocol_errors},
    {"http_timeout_and_stop", t_http_timeout_and_stop},
    {"http_streamed_request_body", t_http_streamed_request_body},
    {"api_contract_bodies", t_api_contract_bodies},
    {"api_refuses_what_v1_forbids", t_api_refuses_what_v1_forbids},
    {"upload_count_only", t_upload_count_only},
    {"upload_pieces_and_complete", t_upload_pieces_and_complete},
    {"upload_gates", t_upload_gates},
    {"upload_failures_and_retries", t_upload_failures_and_retries},
    {"upload_policy", t_upload_policy},
    {"upload_bad_report_id", t_upload_bad_report_id},
    {"upload_budget_two_mib_dump", t_upload_budget_two_mib_dump},
};

// --live-upload: one report against a real API (a local `wrangler dev`),
// driven by tools/tests/crash_dev_integration.py. Everything comes from the
// command line, nothing is scripted: this is the console code talking to the
// Worker.
static int live_upload_main(int argc, char** argv) {
    std::string url, response_pk, recipient_pk, work, build_id, install_id, report_id;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!next) continue;
        if (a == "--url") url = argv[++i];
        else if (a == "--response-pk") response_pk = argv[++i];
        else if (a == "--recipient-pk") recipient_pk = argv[++i];
        else if (a == "--work") work = argv[++i];
        else if (a == "--build-id") build_id = argv[++i];
        else if (a == "--install-id") install_id = argv[++i];
        else if (a == "--report-id") report_id = argv[++i];
        else if (a == "--halt-code") g_halt_code = std::atoi(argv[++i]);
    }
    if (url.empty() || response_pk.size() != 64 || recipient_pk.size() != 64 || work.empty() || build_id.empty() ||
        install_id.size() != 32 || report_id.size() != 26) {
        std::printf("usage: --live-upload --url URL --response-pk HEX --recipient-pk HEX --work DIR "
                    "--build-id ID --install-id HEX --report-id ULID\n");
        return 2;
    }
    if (std::system(("mkdir -p '" + work + "/outbox' '" + work + "/pieces'").c_str()) != 0) return 2;

    auto io = make_posix_io();
    auto net = make_posix_net();
    Outbox ob(*io, work + "/outbox");
    std::vector<Piece> pieces = two_pieces();
    // A marker the integration script looks for in what it decrypts.
    pieces[0].data += "live integration marker " + report_id + "\n";
    for (const Piece& p : pieces) put_file(work + "/pieces/" + p.name, p.data);
    g_now_unix = (int64_t)::time(nullptr);
    make_report(*io, ob, work + "/outbox", report_id, pieces, "granted", build_id.c_str());

    UploadConfig cfg;
    cfg.base_url = url;
    cfg.install_id = install_id;
    cfg.channel = "test";
    cfg.platform_model = "vita";
    UploadKeys keys;
    const std::string pk = from_hex(response_pk), rpk = from_hex(recipient_pk);
    std::memcpy(keys.response_pk, pk.data(), sizeof keys.response_pk);
    std::memcpy(keys.recipient_pk, rpk.data(), sizeof keys.recipient_pk);
    UploadEnv env;
    env.now_unix = fake_now;
    env.random_bytes = fake_random;
    env.should_stop = fake_stop;
    Uploader up(*io, *net, ob, cfg, keys, env);
    const UploadStats s = up.run();
    std::printf("outcome=%s requests=%d pieces=%d completed=%d deleted=%d failed=%d detail=%s\n",
                report_outcome_name(s.last), s.requests, s.artifacts_stored, s.completed, s.deleted, s.failed,
                up.detail().c_str());
    const bool ok = s.completed == 1 && ob.list().empty();
    if (!ok) std::printf("FAIL: the report did not complete\n");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--live-upload")) return live_upload_main(argc, argv);
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--work") && i + 1 < argc) g_work = argv[++i];
        else if (!std::strcmp(argv[i], "--vectors") && i + 1 < argc) g_vectors = argv[++i];
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!std::strcmp(argv[i], "--fake-api") && i + 1 < argc) g_fake_api = argv[++i];
    }
    if (g_work.empty() || g_vectors.empty()) {
        std::printf("usage: crashreport_transport_test --work DIR --vectors DIR [--filter NAME]\n");
        return 2;
    }
    { const std::string cmd = "mkdir -p '" + g_work + "'";
      if (std::system(cmd.c_str()) != 0) { std::printf("cannot create %s\n", g_work.c_str()); return 2; } }
    int ran = 0;
    for (const TestCase& t : kTests) {
        if (filter && !std::strstr(t.name, filter)) continue;
        const int before = g_fail;
        t.fn();
        ++ran;
        std::printf("  %-40s %s\n", t.name, g_fail == before ? "ok" : "FAILED");
    }
    std::printf("%s: %d tests, %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", ran, g_checks, g_fail);
    return g_fail > 255 ? 255 : g_fail;
}
