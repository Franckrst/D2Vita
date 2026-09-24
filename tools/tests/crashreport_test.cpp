// tools/tests/crashreport_test.cpp — host-side tests for src/crashreport/
// (crash-report extraction library).
//
// Everything under src/crashreport/ is plain C++17 with no VitaSDK
// dependency, so it builds and runs on a dev machine. Run through
// tools/tests/run_crashreport_tests.sh.
#include "crashreport/cr_addr.h"
#include "crashreport/cr_claim.h"
#include "crashreport/cr_crashtxt_parse.h"
#include "crashreport/cr_evidence.h"
#include "crashreport/cr_io.h"
#include "crashreport/cr_json.h"
#include "crashreport/cr_outbox.h"
#include "crashreport/cr_progress_parse.h"
#include "crashreport/cr_psp2dmp.h"
#include "crashreport/cr_redact.h"
#include "crashreport/cr_types.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <utime.h>

using namespace d2cr;

// ---------------------------------------------------------------- allocation counter
// Counts every C++ allocation (the dump reader also routes zlib through
// operator new) to measure peak heap use, and can scan each freed block for
// secret bytes left behind. Disabled under ASan, which owns the allocator.
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
static size_t g_heap_cur = 0, g_heap_peak = 0;
static const bool kAllocCounter = false;
#endif

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
#define CHECK_STR(got, want) do { const std::string g_ = (got), w_ = (want); \
    CHECK(g_ == w_, "%s: got \"%s\", want \"%s\"", #got, g_.c_str(), w_.c_str()); } while (0)
#define CHECK_U64(got, want) do { const uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
    CHECK(g_ == w_, "%s: got %" PRIu64 " (0x%" PRIx64 "), want %" PRIu64 " (0x%" PRIx64 ")", #got, g_, g_, w_, w_); } while (0)

static std::string g_fixtures, g_work, g_dumps;

// Fresh, empty directory under the work dir for one test.
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

static std::string fixture(const char* name) {
    const std::string p = g_fixtures + "/" + name;
    if (!exists(p)) { std::printf("  missing fixture %s\n", p.c_str()); std::exit(2); }
    return get_file(p);
}

static std::string addrs_str(const std::vector<Addr>& v) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) { if (i) r += ','; r += format_addr(v[i]); }
    return r;
}

static void set_mtime(const std::string& path, int64_t t) {
    struct utimbuf ub; ub.actime = (time_t)t; ub.modtime = (time_t)t;
    if (::utime(path.c_str(), &ub) != 0) { std::printf("  utime failed on %s\n", path.c_str()); std::exit(2); }
}

// ---------------------------------------------------------------- cr_types

static void t_kind_name() {
    CHECK_STR(kind_name(Kind::Hang), "hang");
    CHECK_STR(kind_name(Kind::GuestFault), "guest_fault");
    CHECK_STR(kind_name(Kind::AbnormalExit), "abnormal_exit");
    CHECK_STR(kind_name(Kind::Halt), "halt");
    CHECK_STR(kind_name(Kind::HostFault), "host_fault");
    CHECK_STR(kind_name(Kind::None), "none");
    CHECK(Kind::HostFault > Kind::Halt && Kind::Halt > Kind::AbnormalExit &&
          Kind::AbnormalExit > Kind::GuestFault && Kind::GuestFault > Kind::Hang &&
          Kind::Hang > Kind::None, "severity order broken");
}

static void t_format_addr() {
    Addr a; a.module = "Game"; a.offset = 0x1fedf4;
    CHECK_STR(format_addr(a), "Game+0x1fedf4");
    a.offset = 0; CHECK_STR(format_addr(a), "Game+0x0");
    a.module = "SceLibKernel"; a.offset = 0xFFFFFFFFu;
    CHECK_STR(format_addr(a), "SceLibKernel+0xffffffff");
    a.module = "Game"; a.offset = 0x0000ABCD;
    CHECK_STR(format_addr(a), "Game+0xabcd");
}

static const char* kSessionText =
    "session_id=4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e\n"
    "build_id=0.1.0+ab12cd34ef56\n"
    "started_unix=1789284000\n"
    "write_root=ux0:data/d2vita/save2\n"
    "progress_path=ux0:data/d2vita/boot_progress.txt\n"
    "state=running\n"
    "game_base=0x01900000\n"
    "arena_host_base=0x84000000\n"
    "eboot_base=0x8102c000\n"
    "eboot_size=0x003491bc\n"
    "jit_lo=0x96000000\n"
    "jit_hi=0x97000000\n"
    "stop_reason=shutdown requested\n"
    "main_exit=0xffffffff\n";

static void t_parse_session() {
    SessionRecord s;
    CHECK(parse_session(kSessionText, &s), "valid session rejected");
    CHECK_STR(s.session_id, "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e");
    CHECK_STR(s.build_id, "0.1.0+ab12cd34ef56");
    CHECK_U64(s.started_unix, 1789284000);
    CHECK_STR(s.write_root, "ux0:data/d2vita/save2");
    CHECK_STR(s.progress_path, "ux0:data/d2vita/boot_progress.txt");
    CHECK_STR(s.state, "running");
    CHECK_U64(s.game_base, 0x01900000u);
    CHECK_U64(s.arena_host_base, 0x84000000u);
    CHECK_U64(s.eboot_base, 0x8102c000u);
    CHECK_U64(s.eboot_size, 0x003491bcu);
    CHECK_U64(s.jit_lo, 0x96000000u);
    CHECK_U64(s.jit_hi, 0x97000000u);
    CHECK_STR(s.stop_reason, "shutdown requested");
    CHECK(s.has_main_exit, "main_exit present but has_main_exit=false");
    CHECK_U64(s.main_exit, 0xffffffffu);

    // CRLF, blank lines, comments and unknown keys (forward compatibility).
    SessionRecord t;
    CHECK(parse_session("\r\nsession_id=abc\r\n# comment\r\nfuture_key=1\r\nstate=exited\r\n", &t),
          "CRLF/unknown key session rejected");
    CHECK_STR(t.session_id, "abc");
    CHECK_STR(t.state, "exited");
    CHECK(!t.has_main_exit, "has_main_exit set without main_exit");

    SessionRecord u;
    CHECK(!parse_session("state=running\n", &u), "session without session_id accepted");
    CHECK(!parse_session("session_id=abc\ngame_base=0xZZ\n", &u), "malformed hex accepted");
    CHECK(!parse_session("session_id=abc\nstarted_unix=12x\n", &u), "malformed decimal accepted");
    CHECK(!parse_session("session_id=abc\ngame_base=0x100000000\n", &u), "32-bit overflow accepted");
}

static void t_serialize_session_roundtrip() {
    SessionRecord s;
    CHECK(parse_session(kSessionText, &s), "parse");
    const std::string text = serialize_session(s);
    SessionRecord r;
    CHECK(parse_session(text, &r), "serialized session does not parse back: %s", text.c_str());
    CHECK_STR(r.session_id, s.session_id); CHECK_STR(r.build_id, s.build_id);
    CHECK_STR(r.write_root, s.write_root); CHECK_STR(r.progress_path, s.progress_path);
    CHECK_STR(r.state, s.state); CHECK_STR(r.stop_reason, s.stop_reason);
    CHECK_U64(r.started_unix, s.started_unix); CHECK_U64(r.game_base, s.game_base);
    CHECK_U64(r.arena_host_base, s.arena_host_base); CHECK_U64(r.eboot_base, s.eboot_base);
    CHECK_U64(r.eboot_size, s.eboot_size); CHECK_U64(r.jit_lo, s.jit_lo); CHECK_U64(r.jit_hi, s.jit_hi);
    CHECK(r.has_main_exit && r.main_exit == s.main_exit, "main_exit lost in round trip");

    SessionRecord n; n.session_id = "x"; n.state = "running";
    const std::string t2 = serialize_session(n);
    CHECK(t2.find("main_exit") == std::string::npos, "main_exit written without has_main_exit: %s", t2.c_str());
    // A value can never smuggle a line break into the key=value file.
    n.stop_reason = "UNSHIMMED A\nstate=exited";
    SessionRecord m;
    CHECK(parse_session(serialize_session(n), &m), "sanitized session does not parse");
    CHECK_STR(m.state, "running");
}

// ---------------------------------------------------------------- cr_json

static void t_json_writer_ordered() {
    JsonWriter w;
    w.begin_object()
        .key("v").num(1)
        .key("report_id").str("01J9Z6T4Q8M3K7V2B5N0XWAYCD")
        .key("frames").begin_array().str("Game+0x1").str("Game+0x2").end_array()
        .key("location").null()
        .key("online").boolean(false)
        .key("empty").begin_array().end_array()
        .key("obj").begin_object().key("n").num(-5).key("u").unum(18446744073709551615ull).end_object()
        .key("features").raw("{\"code\":1420}")
        .key("last").boolean(true)
     .end_object();
    CHECK_STR(w.out(),
        "{\"v\":1,\"report_id\":\"01J9Z6T4Q8M3K7V2B5N0XWAYCD\",\"frames\":[\"Game+0x1\",\"Game+0x2\"],"
        "\"location\":null,\"online\":false,\"empty\":[],\"obj\":{\"n\":-5,\"u\":18446744073709551615},"
        "\"features\":{\"code\":1420},\"last\":true}");
    JsonWriter a;
    a.begin_array().begin_object().end_object().num(0).raw("[1]").end_array();
    CHECK_STR(a.out(), "[{},0,[1]]");
}

static void t_json_escaping() {
    CHECK_STR(json_quote("plain"), "\"plain\"");
    CHECK_STR(json_quote("a\"b\\c/d"), "\"a\\\"b\\\\c/d\"");
    CHECK_STR(json_quote(std::string("\b\f\n\r\t", 5)), "\"\\b\\f\\n\\r\\t\"");
    CHECK_STR(json_quote(std::string("\x01\x1f\x7f", 3)), "\"\\u0001\\u001f\x7f\"");
    CHECK_STR(json_quote(std::string("nul\0x", 5)), "\"nul\\u0000x\"");
    // Valid UTF-8 passes through untouched.
    CHECK_STR(json_quote("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"), "\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80\"");
    // Invalid UTF-8 never reaches the output: stray continuation byte, Latin-1
    // byte, truncated sequence, overlong NUL, UTF-16 surrogate, > U+10FFFF.
    CHECK_STR(json_quote("a\x80z"), "\"a\\ufffdz\"");
    CHECK_STR(json_quote("\xe9t\xe9"), "\"\\ufffdt\\ufffd\"");
    CHECK_STR(json_quote("\xe2\x82"), "\"\\ufffd\\ufffd\"");
    CHECK_STR(json_quote("\xc0\x80"), "\"\\ufffd\\ufffd\"");
    CHECK_STR(json_quote("\xed\xa0\x80"), "\"\\ufffd\\ufffd\\ufffd\"");
    CHECK_STR(json_quote("\xf4\x90\x80\x80"), "\"\\ufffd\\ufffd\\ufffd\\ufffd\"");
    JsonWriter w;
    w.begin_object().key("k\"ey").str("v\nal").end_object();
    CHECK_STR(w.out(), "{\"k\\\"ey\":\"v\\nal\"}");
}

// ---------------------------------------------------------------- cr_io_posix

static void t_posix_io_files() {
    const std::string d = test_dir("io_files");
    auto io = make_posix_io();
    CHECK(io != nullptr, "make_posix_io returned null");
    if (!io) return;
    const std::string p = d + "/a.txt";
    CHECK(io->write_file(p, "hello"), "write_file");
    CHECK_STR(get_file(p), "hello");
    CHECK(io->write_file(p, "hi"), "write_file truncating");
    CHECK_STR(get_file(p), "hi");
    std::string out = "stale";
    CHECK(io->read_file(p, &out, 16), "read_file");
    CHECK_STR(out, "hi");
    CHECK(io->read_file(p, &out, 2), "read_file at exactly max_bytes");
    CHECK(!io->read_file(p, &out, 1), "read_file must refuse a file larger than max_bytes");
    CHECK(!io->read_file(d + "/missing", &out, 16), "read_file on a missing file");
    CHECK(io->write_file(d + "/empty", ""), "write empty file");
    CHECK(io->read_file(d + "/empty", &out, 16) && out.empty(), "read empty file");
    CHECK(!io->write_file(d + "/no/such/dir/x", "x"), "write_file into a missing directory");
    const std::string bin("a\0b\xff", 4);
    CHECK(io->write_file(d + "/bin", bin) && io->read_file(d + "/bin", &out, 4) && out == bin, "binary round trip");
}

static void t_posix_io_head_tail() {
    const std::string d = test_dir("io_head_tail");
    auto io = make_posix_io();
    const std::string p = d + "/log.txt";
    put_file(p, "0123456789");
    std::string out;
    CHECK(io->read_head_tail(p, 3, 4, &out), "read_head_tail");
    CHECK_STR(out, "0126789");
    CHECK(io->read_head_tail(p, 6, 4, &out), "read_head_tail exact fit");
    CHECK_STR(out, "0123456789");
    CHECK(io->read_head_tail(p, 64, 64, &out), "read_head_tail small file");
    CHECK_STR(out, "0123456789");
    CHECK(io->read_head_tail(p, 0, 2, &out), "tail only");
    CHECK_STR(out, "89");
    CHECK(io->read_head_tail(p, 2, 0, &out), "head only");
    CHECK_STR(out, "01");
    CHECK(!io->read_head_tail(d + "/missing", 1, 1, &out), "missing file");
}

static void t_posix_io_dirs() {
    const std::string d = test_dir("io_dirs");
    auto io = make_posix_io();
    CHECK(io->mkdir(d + "/sub"), "mkdir");
    CHECK(io->mkdir(d + "/sub"), "mkdir on an existing directory must succeed");
    put_file(d + "/file", "12345");
    CHECK(!io->mkdir(d + "/file"), "mkdir over a file must fail");
    CHECK(!io->mkdir(d + "/missing/deeper"), "mkdir does not create parents");
    set_mtime(d + "/file", 1789284000);

    std::vector<DirEntry> es;
    CHECK(io->list_dir(d, &es), "list_dir");
    CHECK_U64(es.size(), 2);
    if (es.size() == 2) {
        CHECK_STR(es[0].name, "file");     // sorted by name
        CHECK(!es[0].is_dir && es[0].size == 5 && es[0].mtime_unix == 1789284000, "file entry wrong");
        CHECK_STR(es[1].name, "sub");
        CHECK(es[1].is_dir, "sub must be a directory");
    }
    CHECK(!io->list_dir(d + "/missing", &es), "list_dir on a missing directory");

    DirEntry e;
    CHECK(io->stat(d + "/file", &e), "stat");
    CHECK_STR(e.name, "file");
    CHECK(!e.is_dir && e.size == 5 && e.mtime_unix == 1789284000, "stat fields wrong");
    CHECK(io->stat(d + "/sub", &e) && e.is_dir, "stat dir");
    CHECK(!io->stat(d + "/missing", &e), "stat missing");

    // rename moves; it never replaces an existing destination (the portable
    // subset: callers must remove first).
    put_file(d + "/r1", "one");
    CHECK(io->rename(d + "/r1", d + "/sub/r2"), "rename");
    CHECK(!exists(d + "/r1") && get_file(d + "/sub/r2") == "one", "rename did not move");
    put_file(d + "/r3", "three");
    CHECK(!io->rename(d + "/r3", d + "/sub/r2"), "rename over an existing file must fail");
    CHECK_STR(get_file(d + "/sub/r2"), "one");
    CHECK(!io->rename(d + "/missing", d + "/x"), "rename of a missing file");

    CHECK(!io->remove(d + "/sub"), "remove of a non-empty directory must fail");
    CHECK(io->remove(d + "/sub/r2"), "remove file");
    CHECK(!exists(d + "/sub/r2"), "file still there");
    CHECK(io->remove(d + "/sub"), "remove empty directory");
    CHECK(!exists(d + "/sub"), "directory still there");
    CHECK(!io->remove(d + "/missing"), "remove missing");
}

struct StreamLog { std::vector<size_t> sizes; std::string data; int stop_after = -1; };
static bool stream_cb(const uint8_t* p, size_t n, void* ud) {
    StreamLog* s = (StreamLog*)ud;
    s->sizes.push_back(n);
    s->data.append((const char*)p, n);
    return s->stop_after < 0 || (int)s->sizes.size() < s->stop_after;
}

static void t_posix_io_stream() {
    const std::string d = test_dir("io_stream");
    auto io = make_posix_io();
    std::string content;
    for (int i = 0; i < 1000; ++i) content += (char)('a' + i % 26);
    put_file(d + "/s", content);
    StreamLog s;
    CHECK(io->read_stream(d + "/s", 300, stream_cb, &s), "read_stream");
    CHECK(s.data == content, "stream content differs");
    CHECK(s.sizes.size() == 4 && s.sizes[0] == 300 && s.sizes[3] == 100, "unexpected chunking (%zu chunks)", s.sizes.size());
    StreamLog a; a.stop_after = 2;
    CHECK(!io->read_stream(d + "/s", 300, stream_cb, &a), "aborted stream must return false");
    CHECK_U64(a.sizes.size(), 2);
    StreamLog e;
    put_file(d + "/e", "");
    CHECK(io->read_stream(d + "/e", 300, stream_cb, &e) && e.sizes.empty(), "empty stream");
    CHECK(!io->read_stream(d + "/missing", 300, stream_cb, &e), "missing stream");
    CHECK(!io->read_stream(d + "/s", 0, stream_cb, &e), "zero chunk size");
}

// ---------------------------------------------------------------- cr_addr

static void t_guest_addr() {
    // Module tokens of contract signature-rules.v1.md §7: Game for Game.exe,
    // ABS with the address itself for anything else the logs cannot name.
    const uint32_t gb = 0x01900000;
    std::vector<GuestModule> none;
    CHECK_STR(format_addr(guest_addr(0x0192f040, gb, none)), "Game+0x2f040");
    CHECK_STR(format_addr(guest_addr(gb, gb, none)), "Game+0x0");
    CHECK_STR(format_addr(guest_addr(gb + kGame114dImageSize - 1, gb, none)), "Game+0x5b9fff");
    CHECK_STR(format_addr(guest_addr(gb + kGame114dImageSize, gb, none)), "ABS+0x1eba000");
    CHECK_STR(format_addr(guest_addr(0x10e01b30, gb, none)), "ABS+0x10e01b30");
    CHECK_STR(format_addr(guest_addr(0x0192f040, 0, none)), "ABS+0x192f040");
    CHECK_STR(format_addr(guest_addr(0, gb, none)), "ABS+0x0");
    std::vector<GuestModule> mods;
    GuestModule m1; m1.base = 0x03000000; m1.size = 0x46000; mods.push_back(m1);
    GuestModule m2; m2.base = 0x01900000; m2.size = kGame114dImageSize; mods.push_back(m2);
    // A listed module known only by base and size has no file name to use:
    // its base must not enter the token (one bug, one spelling), so ABS.
    CHECK_STR(format_addr(guest_addr(0x03012345, gb, mods)), "ABS+0x3012345");
    CHECK_STR(format_addr(guest_addr(0x03046000, gb, mods)), "ABS+0x3046000");
    // Game.exe recognized by its 1.14d image size when the base is unknown.
    CHECK_STR(format_addr(guest_addr(0x01951c23, 0, mods)), "Game+0x51c23");
}

// ---------------------------------------------------------------- cr_progress_parse

static void t_progress_native_fault_real() {
    const ProgressFacts f = parse_progress(fixture("progress_native_fault_real.txt"), 0x01900000);
    CHECK(f.native_fault, "NATIVE FAULT not found");
    CHECK_U64(f.native_faults, 1);
    CHECK_U64(f.native_fault_line, 101);
    CHECK_U64(f.fault_thread, 1);
    CHECK(f.fault_main, "thread 1 is the main guest thread");
    CHECK_U64(f.fault_eip, 0x0192f040u);
    CHECK_U64(f.fault_addr, 0x0192f040u);
    CHECK_STR(format_addr(f.fault_eip_addr), "Game+0x2f040");
    CHECK(!f.has_fault_regs && f.fault_frames.empty(), "older log has no register/stack block");
    CHECK(!f.sched_stopped && !f.clean_exit && !f.exit_process, "no stop lines in this extract");
    CHECK_U64(f.beats, 4);
    CHECK_U64(f.stalled_beats, 0);
    CHECK(f.has_last_eip, "last beat eip");
    CHECK_U64(f.last_eip, 0x0192f040u);
    CHECK_STR(f.runner_state, "");       // the final beat carries no run= field
    CHECK_U64(f.last_uptime_s, 457);
    CHECK(!f.online, "no D2NET in this extract");
}

static void t_progress_native_fault_block() {
    const ProgressFacts f = parse_progress(fixture("progress_native_fault_block.txt"), 0x01900000);
    CHECK(f.native_fault, "NATIVE FAULT not found");
    CHECK_U64(f.native_faults, 2);
    CHECK_U64(f.native_fault_line, 63);
    CHECK_U64(f.fault_thread, 3);
    CHECK(!f.fault_main, "thread 3 is a worker");
    CHECK_U64(f.fault_eip, 0x0192f040u);
    CHECK(f.has_fault_regs, "register line not attached");
    CHECK_U64(f.eax, 0x85800004u); CHECK_U64(f.ebx, 0); CHECK_U64(f.ecx, 0x1cu); CHECK_U64(f.edx, 4);
    CHECK_U64(f.esi, 0x85800024u); CHECK_U64(f.edi, 0x21600001u);
    CHECK_U64(f.ebp, 0x0f2ffe48u); CHECK_U64(f.esp, 0x0f2ffe20u);
    CHECK_U64(f.fault_modules.size(), 2);
    CHECK_STR(format_addr(f.fault_eip_addr), "Game+0x2f040");
    CHECK_STR(addrs_str(f.fault_frames), "Game+0x51c23,Game+0x4f570,ABS+0x3012345,Game+0x4b959");
}

static void t_progress_clean_exit() {
    const ProgressFacts f = parse_progress(fixture("progress_clean_exit_real.txt"), 0x01900000);
    CHECK(!f.native_fault, "no fault in a clean quit");
    CHECK(f.sched_stopped, "scheduler stopped line not found");
    CHECK_STR(f.stop_reason, "shutdown requested");
    CHECK(f.has_main_exit, "main-exit missing");
    CHECK_U64(f.main_exit, 0);
    CHECK(f.clean_exit, "CLEAN EXIT not found");
    CHECK(f.exit_process && !f.terminate_process, "ExitProcess line not found");
    CHECK_U64(f.exit_code, 0);
    CHECK_STR(addrs_str(f.exit_chain), "Game+0x281ab5,Game+0x281df1,Game+0x284f50,Game+0x281e16,Game+0x28292b,Game+0x284f50");
    CHECK(!f.unshimmed && !f.fatal_app_exit && !f.raise_exception, "no abnormal stop line");
    CHECK_U64(f.stalled_beats, 0);
    CHECK(f.online, "env.txt: D2NET=1 means online");
    CHECK_U64(f.last_uptime_s, 687);
}

static void t_progress_abnormal_exit() {
    const ProgressFacts f = parse_progress(fixture("progress_abnormal_exit.txt"), 0x01900000);
    CHECK(f.sched_stopped && f.has_main_exit, "scheduler stopped line not found");
    CHECK_STR(f.stop_reason, "shutdown requested");
    CHECK_U64(f.main_exit, 0xffffffffu);
    CHECK(f.exit_process, "ExitProcess line not found");
    CHECK_U64(f.exit_code, 0xffffffffu);
    CHECK_STR(addrs_str(f.exit_chain), "Game+0x97be,Game+0xa0ed,Game+0xb451,Game+0x20c108");
    CHECK(f.clean_exit, "CLEAN EXIT is still written on this path");
}

static void t_progress_hang() {
    const std::string text = fixture("progress_hang_real.txt");
    const ProgressFacts f = parse_progress(text, 0x01900000);
    CHECK_U64(f.beats, 12);
    CHECK_U64(f.stalled_beats, 4);
    CHECK(f.has_last_eip, "last eip");
    CHECK_U64(f.last_eip, 0x019fa60cu);
    CHECK_STR(format_addr(f.last_eip_addr), "Game+0xfa60c");
    // Runner 1 kept executing blocks while frames stayed frozen (starvation),
    // runner 2 did not move, 3 and 4 moved a little.
    CHECK_STR(f.runner_state, "1:R+,2:B,3:B+,4:B+");
    CHECK_STR(f.runner_class, "starvation");
    CHECK(!f.native_fault && !f.sched_stopped && !f.clean_exit, "nothing else in a hang");
    CHECK(!f.online, "no D2NET in this log");
    // No counter moves: a running thread that makes no progress, or everyone waiting.
    const char* frozen_r =
        "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1 run=1:R:500,2:B:7\n"
        "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1 run=1:R:500,2:B:7\n"
        "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1 run=1:R:500,2:B:7\n";
    CHECK_STR(parse_progress(frozen_r, 0x01900000).runner_class, "deadlock");
    const char* frozen_b =
        "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1 run=1:B:500,2:B:7\n"
        "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1 run=1:B:500,2:B:7\n";
    CHECK_STR(parse_progress(frozen_b, 0x01900000).runner_class, "blocked");
    CHECK_STR(parse_progress("alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1\n"
                             "alive: pump=1 frames=38 reads=1 eip=019fa60c sw=1\n", 0).runner_class, "");
    // The same log cut right before its last beat: three beats without progress.
    const size_t last = text.rfind("] alive: ");
    const size_t cut = text.rfind('\n', last);
    const ProgressFacts g = parse_progress(text.substr(0, cut + 1), 0x01900000);
    CHECK_U64(g.beats, 11);
    CHECK_U64(g.stalled_beats, 3);
}

static void t_progress_no_evidence() {
    const ProgressFacts f = parse_progress(fixture("progress_no_evidence_real.txt"), 0x01900000);
    CHECK(!f.native_fault && !f.sched_stopped && !f.clean_exit && !f.exit_process, "no evidence expected");
    CHECK(!f.unshimmed && !f.fatal_app_exit && !f.raise_exception && !f.terminate_process, "no evidence expected");
    CHECK_U64(f.beats, 3);
    CHECK_U64(f.stalled_beats, 0);
    CHECK_U64(f.last_uptime_s, 36);
    const ProgressFacts e = parse_progress("", 0x01900000);
    CHECK(!e.native_fault && e.beats == 0 && e.last_uptime_s == -1, "empty log");
}

static void t_progress_stop_lines_inline() {
    // Formats of the other writers, one line each (no real sample exists).
    ProgressFacts u = parse_progress(
        "[  30.00s] UNSHIMMED KERNEL32.dll!GetNumaHighestNodeNumber -> arret controle (import manquant)\n", 0x01900000);
    CHECK(u.unshimmed, "UNSHIMMED line (rt_boot.cpp:3389)");
    CHECK_STR(u.unshimmed_import, "KERNEL32.dll!GetNumaHighestNodeNumber");
    ProgressFacts r = parse_progress("[  31.00s] RaiseException 0xe06d7363 -> arret controle\n", 0x01900000);
    CHECK(r.raise_exception, "RaiseException line (win32_import_remainder.cpp:54)");
    CHECK_U64(r.raise_code, 0xe06d7363u);
    ProgressFacts a = parse_progress("[  32.00s] FatalAppExitA: Something bad\n", 0x01900000);
    CHECK(a.fatal_app_exit, "FatalAppExitA line (win32_import_remainder.cpp:45)");
    ProgressFacts t = parse_progress("[  33.00s] TerminateProcess code=3 chain: 01905e6b\n", 0x01900000);
    CHECK(t.terminate_process && !t.exit_process, "TerminateProcess line (win32_import_remainder.cpp:98)");
    CHECK_U64(t.exit_code, 3);
    CHECK_STR(addrs_str(t.exit_chain), "Game+0x5e6b");
    // Cooperative scheduler, boot_progress form (rt_boot.cpp:4840) and
    // crash.log form without timestamp prefix (rt_boot.cpp:4844).
    ProgressFacts c1 = parse_progress(
        "[   4.00s] scheduler stopped: box86 dynarec fault (unimplemented/illegal/div0) (818 switches) exit=0xc0000005\n", 0);
    CHECK(c1.sched_stopped && c1.has_main_exit, "coop progress format");
    CHECK_STR(c1.stop_reason, "box86 dynarec fault (unimplemented/illegal/div0)");
    CHECK_U64(c1.main_exit, 0xc0000005u);
    ProgressFacts c2 = parse_progress(
        "scheduler stopped: main exited | frame=12 switches=308 main-exit=0x00000001\nCLEAN EXIT (game path, frame=12)\n", 0);
    CHECK(c2.sched_stopped && c2.clean_exit, "crash.log format");
    CHECK_STR(c2.stop_reason, "main exited");
    CHECK_U64(c2.main_exit, 1);
    CHECK_U64(c2.last_uptime_s, -1);
    // The LAST stop line wins; CRLF is tolerated.
    ProgressFacts c3 = parse_progress(
        "scheduler stopped: shutdown requested | frame=1 main-exit=0x00000000\r\n"
        "scheduler stopped: main exited | frame=2 main-exit=0x00000002\r\n", 0);
    CHECK_STR(c3.stop_reason, "main exited");
    CHECK_U64(c3.main_exit, 2);
    // Malformed lines are ignored rather than half-parsed.
    ProgressFacts m = parse_progress(
        "NATIVE FAULT thr x eip=zz addr=0\nalive: frames=abc eip=01900000\nscheduler stopped: \n", 0);
    CHECK(!m.native_fault && m.beats == 0 && !m.sched_stopped, "malformed lines must be ignored");
}

// Every fact in one line, to compare two parses.
static std::string facts_str(const ProgressFacts& f) {
    char b[640];
    std::snprintf(b, sizeof b,
                  "nf=%d n=%d line=%d thr=%u main=%d eip=%08x addr=%08x regs=%d:%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                  "mods=%zu stop=%d exit=%d:%08x clean=%d uns=%d fatal=%d raise=%d:%08x ep=%d tp=%d code=%u beats=%d "
                  "stalled=%d last=%d:%08x up=%lld online=%d",
                  f.native_fault, f.native_faults, f.native_fault_line, (unsigned)f.fault_thread, f.fault_main,
                  (unsigned)f.fault_eip, (unsigned)f.fault_addr, f.has_fault_regs, (unsigned)f.eax, (unsigned)f.ebx,
                  (unsigned)f.ecx, (unsigned)f.edx, (unsigned)f.esi, (unsigned)f.edi, (unsigned)f.ebp, (unsigned)f.esp,
                  f.fault_modules.size(), f.sched_stopped, f.has_main_exit, (unsigned)f.main_exit, f.clean_exit, f.unshimmed,
                  f.fatal_app_exit, f.raise_exception, (unsigned)f.raise_code, f.exit_process, f.terminate_process,
                  (unsigned)f.exit_code, f.beats, f.stalled_beats, f.has_last_eip, (unsigned)f.last_eip,
                  (long long)f.last_uptime_s, f.online);
    return std::string(b) + " eip_addr=" + format_addr(f.fault_eip_addr) + " frames=" + addrs_str(f.fault_frames) +
           " reason=" + f.stop_reason + " import=" + f.unshimmed_import + " chain=" + addrs_str(f.exit_chain) +
           " last_addr=" + format_addr(f.last_eip_addr) + " rs=" + f.runner_state + " rc=" + f.runner_class;
}

static void t_progress_stream() {
    auto io = make_posix_io();
    const std::string d = test_dir("progress_stream");
    const char* const names[] = {"progress_native_fault_real.txt", "progress_native_fault_block.txt",
                                 "progress_clean_exit_real.txt", "progress_abnormal_exit.txt",
                                 "progress_hang_real.txt", "progress_no_evidence_real.txt"};
    for (const char* n : names) {
        const std::string text = fixture(n);
        const std::string want = facts_str(parse_progress(text, 0x01900000));
        put_file(d + "/log.txt", text);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, kProgressStreamChunk}) {
            ProgressFacts f;
            CHECK(parse_progress_file(*io, d + "/log.txt", 0x01900000, &f, chunk), "%s: stream read failed", n);
            CHECK(facts_str(f) == want, "%s, chunk %zu:\n     got  %s\n     want %s", n, chunk, facts_str(f).c_str(), want.c_str());
        }
    }
    // A line over kProgressLineMax bytes is ignored as a whole (no writer
    // produces one: the alive: line has a 512-byte buffer), and the line
    // numbers go on.
    const std::string lines = "[   1.00s] NATIVE FAULT thr 2 eip=01900010 addr=01900010" + std::string(kProgressLineMax, ' ') +
                              "\n[   2.00s] NATIVE FAULT thr 3 eip=0192f040 addr=0192f040\n";
    const ProgressFacts o = parse_progress(lines, 0x01900000);
    CHECK(o.native_fault && o.native_faults == 1 && o.fault_thread == 3 && o.native_fault_line == 1,
          "overlong line: faults=%d thr=%u line=%d", o.native_faults, (unsigned)o.fault_thread, o.native_fault_line);
    CHECK_U64(o.last_uptime_s, 2);
    // A log cut in the middle of its last line still gives that line.
    const ProgressFacts p = parse_progress("[   4.00s] x\n[   5.00s] alive: pump=1 frames=3 reads=1 eip=01900010 sw=1", 0x01900000);
    CHECK(p.beats == 1 && p.last_uptime_s == 5, "unterminated last line: beats=%d up=%lld", p.beats, (long long)p.last_uptime_s);
    ProgressFacts q;
    CHECK(!parse_progress_file(*io, d + "/missing.txt", 0x01900000, &q), "missing log accepted");
}

// ---------------------------------------------------------------- cr_crashtxt_parse

static void t_crashtxt_halt1420() {
    const HaltFacts h = parse_crash_txt(fixture("crash_txt_halt1420.txt"), 0x01900000);
    CHECK(h.is_halt, "Halt not recognized");
    CHECK_STR(h.error_type, "Halt");
    CHECK_U64(h.code, 1420);
    CHECK_STR(h.location, "");
    CHECK_U64(h.reporter_frames_removed, 1);   // 0x408a81: inside the Fog Halt thunk
    CHECK_STR(addrs_str(h.frames),
        "Game+0x1fecc5,Game+0x2003a7,Game+0xdbda8,Game+0x71958,Game+0xdf5f4,Game+0x76cf8,"
        "Game+0x4cafd,Game+0x4f291,Game+0x51c23,Game+0x4f570,Game+0x4b959,Game+0x4b43c,"
        "Game+0x5e6b,Game+0x65c7,Game+0x671f,Game+0x28291c");
    // Without any base, frames cannot be made module-relative: none are guessed.
    const HaltFacts n = parse_crash_txt(fixture("crash_txt_halt1420.txt"), 0);
    CHECK(n.is_halt && n.code == 1420 && n.frames.empty(), "no base: code only");
}

static void t_crashtxt_halt904_location() {
    const HaltFacts h = parse_crash_txt(fixture("crash_txt_halt904_location.txt"), 0x01900000);
    CHECK(h.is_halt, "Halt not recognized");
    CHECK_U64(h.code, 904);
    CHECK_STR(h.location, "Codec.cpp:1377");
    CHECK_STR(addrs_str(h.frames), "Game+0x97b4,Game+0xa0ed,Game+0xb451,Game+0x20c108");
}

// Fog's top-level filter reporting a hardware exception it received from the
// runtime's SEH delivery (console, 2026-09-24, D2_SEHTEST): a bare
// "ACCESS_VIOLATION" summary, no line number, the halting thread's chain
// starting at the faulting instruction (Game.exe relocated at 0x03900000).
static void t_crashtxt_access_violation() {
    const HaltFacts h = parse_crash_txt(fixture("crash_txt_access_violation.txt"), 0x03900000);
    CHECK(h.is_halt, "exception summary not recognized");
    CHECK_STR(h.error_type, "ACCESS_VIOLATION");
    CHECK_U64(h.code, 0);
    CHECK_STR(h.location, "");
    CHECK_STR(addrs_str(h.frames),
        "Game+0xfa668,Game+0x35c5e,Game+0x5a31,Game+0x5e6b,Game+0x65c7,Game+0x671f,Game+0x28291c");
    // A lone word is not an exception name: nothing is guessed from prose.
    const HaltFacts w = parse_crash_txt("<Inspector.Summary:>\nUNKNOWN\n<:Inspector.Summary>\n", 0);
    CHECK(!w.is_halt, "a lone word is not an exception summary");
}

static void t_crashtxt_empty() {
    const HaltFacts h = parse_crash_txt(fixture("crash_txt_empty.txt"), 0x01900000);
    CHECK(!h.is_halt, "empty Crash.txt is not a named Halt");
    CHECK_U64(h.code, 0);
    CHECK(h.frames.empty() && h.location.empty(), "empty facts expected");
    CHECK(!parse_crash_txt("random text\nfailed at nothing\n", 0x01900000).is_halt, "garbage is not a Halt");
}

static void t_crashtxt_inline_forms() {
    // A named source file in the summary gives the location; LF endings.
    const HaltFacts a = parse_crash_txt(
        "<Inspector.Summary:>\n[Assertion Failure] (hFile) failed at .\\Source\\SFILE.CPP(898)\n<:Inspector.Summary>\n", 0);
    CHECK(a.is_halt, "assertion not recognized");
    CHECK_STR(a.error_type, "Assertion Failure");
    CHECK_U64(a.code, 898);
    CHECK_STR(a.location, "SFILE.CPP:898");
    // Message with parentheses of its own.
    const HaltFacts p = parse_crash_txt("[Halt] ((3) Pool Blocks overflowed at 12) failed at (904)\r\n", 0);
    CHECK(p.is_halt && p.code == 904 && p.location.empty(), "parenthesized message");
    // Truncated file: only the line number survived.
    const HaltFacts t = parse_crash_txt("<Inspector.LineNumber>316\r\n<Inspector.Assertion:>\r\nThread 0x0", 0);
    CHECK(t.is_halt && t.code == 316, "LineNumber fallback");
    CHECK_STR(t.error_type, "");
    // Base from the module list when the session has none; reporter frames
    // are stripped only while they lead the stack.
    const HaltFacts m = parse_crash_txt(
        "    Base:01900000h  Size: 5BA000h  Name:Game.exe         Path:C:\\Diablo II\\Game.exe\n"
        "[Halt] (Unrecoverable internal error 0019f6fc) failed at (448)\n"
        "<Inspector.Assertion:>\n"
        "DBG-ADDR<01908A81>(\"Game.exe\")\n"
        "DBG-ADDR<01908AB1>(\"Game.exe\")\n"
        "DBG-ADDR<01AB5D6D>(\"Game.exe\")\n"
        "DBG-ADDR<01908A81>(\"Game.exe\")\n"
        "DBG-ADDR<775C8484>(\"KERNEL32.DLL\")\n"
        "<:Inspector.Assertion>\n", 0);
    CHECK_U64(m.game_base, 0x01900000u);
    CHECK_U64(m.reporter_frames_removed, 2);
    CHECK_STR(addrs_str(m.frames), "Game+0x1b5d6d,Game+0x8a81");
    // At most 16 frames.
    std::string many = "[Halt] (x) failed at (1)\n<Inspector.Assertion:>\n";
    for (int i = 0; i < 20; ++i) { char b[64]; std::snprintf(b, sizeof b, "DBG-ADDR<%08X>(\"Game.exe\")\n", 0x01950000 + i); many += b; }
    many += "<:Inspector.Assertion>\n";
    CHECK_U64(parse_crash_txt(many, 0x01900000).frames.size(), 16);
}

// ---------------------------------------------------------------- cr_redact

// Fake secrets only: shaped like real ones (16/26 alphanumerics).
static const char* kFakeKeysTxt =
    "# fake keys for tests\r\n"
    "classic = FAKE-1234-CLAS-5678\r\n"
    "lod=fakelodkey0123456789abcdef\r\n"
    "owner= Fake Owner \r\n"
    "bad line\r\n"
    "classic_owner=Ab\r\n";

static std::string utf16le(const std::string& s) {
    std::string r;
    for (char c : s) { r += c; r += '\0'; }
    return r;
}

static SecretPatterns fake_patterns() {
    return build_patterns(kFakeKeysTxt, std::vector<std::string>(1, "FAKEACCOUNT"));
}

static void t_redact_build_patterns() {
    SecretPatterns p = fake_patterns();
    // 2 keys + owner + account, each in ASCII and UTF-16LE; the 2-letter
    // classic_owner is ignored.
    CHECK_U64(p.count(), 8);
    CHECK_U64(p.max_match_len(), (2 * 26 - 1) * 2);
    SecretPatterns bad = build_patterns("classic=SHORT\nlod=FAKELODKEY0123456789ABCDE!\n", std::vector<std::string>(1, "  "));
    CHECK(bad.empty(), "invalid keys and blank names must give no pattern (count %zu)", bad.count());
    p.wipe();
    CHECK(p.empty() && p.count() == 0 && p.max_match_len() == 0, "wipe must drop every pattern");
}

struct Seg { std::string text; bool secret; };

static std::vector<Seg> redact_segments() {
    std::vector<Seg> v;
    auto add = [&v](const std::string& t, bool s) { v.push_back(Seg{t, s}); v.push_back(Seg{" | ", false}); };
    add("FAKE1234CLAS5678", true);
    add("fake1234clas5678", true);
    add("FAKE-1234-CLAS-5678", true);
    add("FAKELO-DKEY-012345-6789-ABCDEF", true);
    add("fakelodkey 0123 456789 abcdef", true);
    add("FaKe1234ClAs5678", true);
    add(utf16le("FAKE1234CLAS5678"), true);
    add(utf16le("fake-1234-clas-5678"), true);
    add("Fake Owner", true);
    add("FAKE OWNER", true);
    add(utf16le("fake owner"), true);
    add("FAKEACCOUNT", true);
    add(utf16le("fakeaccount"), true);
    // Dashes around a key are not part of it.
    v.push_back(Seg{"-", false}); v.push_back(Seg{"FAKE1234CLAS5678", true}); v.push_back(Seg{"- | ", false});
    add("FAKE1234CLAS567 end", false);        // one character short
    add("FAKE--1234CLAS5678", false);          // two separators in a row
    add("Ab Fa owner", false);                 // ignored short name, partial owner
    return v;
}

static void t_redact_in_place_variants() {
    const SecretPatterns p = fake_patterns();
    const std::vector<Seg> segs = redact_segments();
    std::string text;
    int secrets = 0;
    for (const Seg& s : segs) { text += s.text; secrets += s.secret ? 1 : 0; }
    std::string red = text;
    const int n = redact_in_place(&red, p);
    CHECK_U64(n, secrets);
    CHECK_U64(red.size(), text.size());
    size_t at = 0;
    for (const Seg& s : segs) {
        const std::string got = red.substr(at, s.text.size());
        if (s.secret) {
            bool all_x = true;
            for (size_t i = 0; i < got.size(); ++i) {
                const bool utf16_zero = (s.text[i] == '\0');
                if (utf16_zero ? got[i] != '\0' : got[i] != 'X') all_x = false;
            }
            CHECK(all_x, "secret segment not fully redacted at offset %zu", at);
        } else {
            CHECK(got == s.text, "non-secret segment changed at offset %zu", at);
        }
        at += s.text.size();
    }
    std::string again = red;
    CHECK_U64(redact_in_place(&again, p), 0);
    std::string none = "nothing secret here";
    CHECK_U64(redact_in_place(&none, SecretPatterns()), 0);
    // Adjacent occurrences are counted once each.
    std::string twice = "FAKEACCOUNTFAKEACCOUNT";
    CHECK_U64(redact_in_place(&twice, p), 2);
    CHECK_STR(twice, "XXXXXXXXXXXXXXXXXXXXXX");
}

static void t_redact_stream_scanner() {
    const SecretPatterns p = fake_patterns();
    std::string text;
    for (const Seg& s : redact_segments()) text += s.text;
    // A 300 KiB stream with the secrets scattered, including across 64 KiB
    // chunk boundaries.
    std::string big(300 * 1024, '\0');
    uint32_t x = 12345;
    for (size_t i = 0; i < big.size(); ++i) { x = x * 1103515245u + 12345u; big[i] = (char)((x >> 24) & 0x3f); }
    const size_t spots[] = {0, 65536 - 7, 131072 - 40, 200000, big.size() - text.size()};
    for (size_t s : spots) big.replace(s, text.size(), text);
    std::string copy = big;
    const int expected = redact_in_place(&copy, p);
    CHECK(expected > 0, "no secret in the stream");
    const size_t chunks[] = {1, 3, 4096, 65536, big.size()};
    for (size_t c : chunks) {
        StreamScanner sc(p);
        for (size_t off = 0; off < big.size(); off += c)
            sc.feed((const uint8_t*)big.data() + off, std::min(c, big.size() - off));
        sc.finish();
        CHECK_U64(sc.hits(), (uint64_t)expected);
    }
    // A key cut exactly in two by the chunk boundary.
    const std::string key = "FAKE-1234-CLAS-5678";
    for (size_t cut = 1; cut < key.size(); ++cut) {
        StreamScanner sc(p);
        sc.feed((const uint8_t*)key.data(), cut);
        sc.feed((const uint8_t*)key.data() + cut, key.size() - cut);
        sc.finish();
        CHECK(sc.hits() == 1, "key split at %zu not found", cut);
    }
    // A pattern truncated by the end of the stream is not a match.
    StreamScanner tail(p);
    tail.feed((const uint8_t*)"xxFAKE1234CLAS567", 17);
    tail.finish();
    CHECK_U64(tail.hits(), 0);
}

static void t_redact_accounts_from_registry() {
    const std::string reg =
        "software\\blizzard entertainment\\diablo ii|gamma|Gamma|4|9b000000\n"
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|46414b454143434f554e5400\n"
        "software\\blizzard entertainment\\diablo ii|preferred realm|Preferred Realm|1|4575726f706500\n";
    const std::vector<std::string> a = accounts_from_registry(reg);
    CHECK_U64(a.size(), 1);
    if (!a.empty()) CHECK_STR(a[0], "FAKEACCOUNT");
    CHECK(accounts_from_registry("garbage|x\n").empty(), "garbage registry");
    CHECK(accounts_from_registry("software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|zz\n").empty(),
          "malformed hex must be ignored");
    CHECK(accounts_from_registry("software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|00\n").empty(),
          "empty account name");
    // CRLF, and one name per line at most.
    const std::vector<std::string> b = accounts_from_registry(
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|4142434400\r\n"
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|41424344\r\n"
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|5758595a\r\n");
    CHECK(b.size() == 2 && b[0] == "ABCD" && b[1] == "WXYZ", "CRLF registry: %zu names", b.size());
}

// Spec 4.6: patterns live in memory only and are wiped after use. No block
// freed while keys.txt and the registry are turned into patterns, used and
// wiped may still hold a key, a name, or the hex of a name (a later dump of
// the process would carry it, and would then be withheld).
static std::string hex_of(const std::string& s) {
    static const char* const kHex = "0123456789abcdef";
    std::string r;
    for (unsigned char c : s) { r += kHex[c >> 4]; r += kHex[c & 15]; }
    return r;
}

static void t_redact_no_plaintext_in_freed_heap() {
    if (!kAllocCounter) return;   // the scan lives in the counting allocator
    const std::string long_account = "FAKEACCOUNTWITHAVERYLONGNAME0123456789";
    const std::string long_account_hex = hex_of(long_account);
    const std::string keys = std::string(kFakeKeysTxt) + "lod_owner=Fake Owner With A Rather Long Name\n";
    const std::string registry =
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|" + hex_of("FAKEACCOUNT") + "00\r\n"
        "software\\blizzard entertainment\\diablo ii|last bnet|Last BNet|1|" + long_account_hex + "00\n";
    const std::string text = "log: FAKE-1234-CLAS-5678 then fakelodkey0123456789abcdef by FAKEACCOUNT\n";
    const Needle needles[] = {
        {"FAKE1234CLAS5678", 16}, {"FAKE-1234-CLAS-5678", 19}, {"FAKELODKEY0123456789ABCDEF", 26},
        {"fakelodkey0123456789abcdef", 26}, {"FAKE OWNER", 10}, {"Fake Owner", 10}, {"FAKEACCOUNT", 11},
        {"46414b454143434f554e54", 22}, {long_account_hex.c_str(), long_account_hex.size()},
        {"OWNER WITH A RATHER", 19}, {"Owner With A Rather", 19}, {nullptr, 0}};
    std::vector<std::string> accounts;
    std::string work = text;
    g_scan_hits = 0;
    g_scan_needles = needles;
    {
        accounts = accounts_from_registry(registry);
        SecretPatterns p = build_patterns(keys, accounts);
        SecretPatterns copy = p;
        copy = p;
        redact_in_place(&work, copy);
        StreamScanner sc(p);
        sc.feed((const uint8_t*)text.data(), text.size());
        sc.finish();
        p.wipe();
    }
    g_scan_needles = nullptr;
    CHECK(accounts.size() == 2, "accounts: %zu", accounts.size());
    CHECK(work.find("FAKE") == std::string::npos, "redaction did not run: %s", work.c_str());
    CHECK(g_scan_hits == 0, "%d freed heap block(s) still hold a secret", g_scan_hits);
}

// ---------------------------------------------------------------- cr_psp2dmp
// Synthetic dumps come from tools/tests/gen_fake_psp2dmp.py (constants below
// mirror it): arena host view 0x84000000, Game.exe at 0x01900000, JIT pool
// 0x96900000, eboot text 0x81000000.

static SessionRecord dump_session() {
    SessionRecord s;
    s.session_id = "0123456789abcdef0123456789abcdef";
    s.game_base = 0x01900000;
    s.arena_host_base = 0x84000000;
    s.jit_lo = 0x96900000;
    s.jit_hi = 0x97900000;
    s.eboot_base = 0x81000000;
    s.eboot_size = 0x349000;
    return s;
}

static std::string dump_path(const char* name) {
    const std::string p = g_dumps + "/" + name;
    if (g_dumps.empty() || !exists(p)) { std::printf("  missing synthetic dump %s (run through run_crashreport_tests.sh)\n", p.c_str()); std::exit(2); }
    return p;
}

static std::string chain_str(const DumpFacts& f, uint32_t game_base) {
    std::string r;
    for (const GuestFrame& g : f.guest_chain) {
        char b[48];
        std::snprintf(b, sizeof b, "%s%08x:%x", r.empty() ? "" : ",", (unsigned)g.frame, (unsigned)(g.ret - game_base));
        r += b;
    }
    return r;
}

static std::string host_addr_str(const HostAddr& a) {
    char b[96];
    std::snprintf(b, sizeof b, "%s/%s+0x%x", a.region.c_str(), a.module.c_str(), (unsigned)a.offset);
    return b;
}

static void t_psp2dmp_first_jit() {
    auto io = make_posix_io();
    const SecretPatterns pats = fake_patterns();
    const DumpFacts f = read_psp2dmp(*io, dump_path("first_jit.psp2dmp"), dump_session(), pats);
    CHECK(f.ok, "not ok: %s", f.error.c_str());
    CHECK(!f.truncated, "unexpected truncation");
    CHECK_STR(f.app_title, "DTWO00001");
    CHECK_STR(f.fw, "3.65");
    CHECK(f.has_stamp, "stamp not found");
    CHECK_STR(f.stamp_session_id, "0123456789abcdef0123456789abcdef");
    CHECK_STR(f.stamp_build_id, "0.1.0+ab12cd34ef56");
    CHECK_U64(f.threads.size(), 3);
    if (f.threads.size() == 3) {
        CHECK_STR(f.threads[1].name, "d2_present");
        CHECK_U64(f.threads[1].status, 8);
    }
    CHECK_U64(f.fault_index, 0);
    CHECK_U64(f.tid, 0x40010003u);
    CHECK_STR(f.thread_name, "DTWO00001");
    CHECK_U64(f.stop_reason, 0x30004u);
    CHECK_STR(stop_reason_name(f.stop_reason), "data_abort");
    CHECK_U64(f.pc, 0x96924184u); CHECK_U64(f.lr, 0x96924101u); CHECK_U64(f.sp, 0x81c003b8u);
    CHECK_U64(f.cpsr, 0x200f0010u); CHECK_U64(f.r[8], 0x0f2e1000u); CHECK_U64(f.r[9], 0x0f2e1028u);
    CHECK_STR(host_addr_str(f.pc_addr), "jit/jit+0x24184");
    CHECK_STR(host_addr_str(f.lr_addr), "jit/jit+0x24101");
    CHECK_STR(chain_str(f, 0x01900000), "0f2e1028:51c23,0f2e1100:4f570,0f2e1400:4b959,0f2e2000:4b43c,0f2e4000:5e6b");
    CHECK_STR(f.chain_end, "ret_outside_game");
    CHECK_U64(f.pc_bytes.size(), 0x40);
    if (f.pc_bytes.size() == 0x40) CHECK(f.pc_bytes[0] == 0x00 && f.pc_bytes[2] == 0xa0 && f.pc_bytes[3] == 0xe1, "pc bytes");
    CHECK_U64(f.secret_hits, 0);
    // The second note group (after the memory) is walked too: 0x60000 bytes of SYSTEM_INFO2.
    CHECK(f.decompressed_bytes > 0x88000, "decompressed %" PRIu64, f.decompressed_bytes);

    // Same dump, uncompressed: identical facts.
    const DumpFacts raw = read_psp2dmp(*io, dump_path("raw_elf.psp2dmp"), dump_session(), pats);
    CHECK(raw.ok && raw.tid == f.tid && raw.pc == f.pc && chain_str(raw, 0x01900000) == chain_str(f, 0x01900000) &&
          raw.has_stamp && raw.decompressed_bytes == f.decompressed_bytes, "raw ELF input differs from gzip input");
}

static void t_psp2dmp_worker_withheld() {
    auto io = make_posix_io();
    const SecretPatterns pats = fake_patterns();
    const DumpFacts f = read_psp2dmp(*io, dump_path("worker_withheld.psp2dmp"), dump_session(), pats);
    CHECK(f.ok, "not ok: %s", f.error.c_str());
    CHECK(!f.has_stamp, "no stamp in this dump");
    CHECK_U64(f.fault_index, 2);       // the thread that stopped, not the first one
    CHECK_U64(f.tid, 0x40020301u);
    CHECK_STR(f.thread_name, "d2_runner3");
    CHECK_STR(host_addr_str(f.pc_addr), "jit/jit+0x300a0");
    CHECK_STR(chain_str(f, 0x01900000), "0f4e1010:1fecc5,0f4e1200:2003a7,0f4e1800:dbda8");
    CHECK_STR(f.chain_end, "not_in_dump");
    // ASCII key in the guest stack, UTF-16LE key in the heap, account name
    // across a 64 KiB boundary of the decompressed stream.
    CHECK_U64(f.secret_hits, 3);
    const DumpFacts clean = read_psp2dmp(*io, dump_path("worker_withheld.psp2dmp"), dump_session(), SecretPatterns());
    CHECK_U64(clean.secret_hits, 0);

    DumpReadOptions first;
    first.rule = ThreadRule::First;
    const DumpFacts g = read_psp2dmp(*io, dump_path("worker_withheld.psp2dmp"), dump_session(), pats, first);
    CHECK_U64(g.fault_index, 0);
    CHECK_U64(g.tid, 0x40010003u);
    CHECK(g.guest_chain.empty(), "EBP 0xffffffff cannot be walked");
    CHECK_STR(g.chain_end, "not_in_dump");
}

static void t_psp2dmp_eboot_and_sysmodule() {
    auto io = make_posix_io();
    SessionRecord s = dump_session();
    const DumpFacts e = read_psp2dmp(*io, dump_path("eboot_nostamp.psp2dmp"), s, SecretPatterns());
    CHECK(e.ok, "not ok: %s", e.error.c_str());
    CHECK(!e.has_stamp, "no stamp");
    CHECK_STR(host_addr_str(e.pc_addr), "eboot/eboot+0x2f6e6e");
    CHECK_STR(host_addr_str(e.lr_addr), "eboot/eboot+0x2fa1f1");
    CHECK(e.guest_chain.empty() && e.chain_end == "not_in_dump", "chain from EBP 0xffffffff");
    CHECK_U64(e.pc_bytes.size(), 0x40);
    // Session without eboot bounds: the main module of MODULE_INFO (lowest uid) is the eboot.
    s.eboot_base = 0; s.eboot_size = 0;
    const DumpFacts e2 = read_psp2dmp(*io, dump_path("eboot_nostamp.psp2dmp"), s, SecretPatterns());
    CHECK_STR(host_addr_str(e2.pc_addr), "eboot/eboot+0x2f6e6e");

    const DumpFacts o = read_psp2dmp(*io, dump_path("other_app_sysmodule.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(o.ok, "not ok: %s", o.error.c_str());
    CHECK_STR(o.app_title, "CARN20001");
    CHECK_STR(stop_reason_name(o.stop_reason), "undefined_instruction");
    CHECK_STR(host_addr_str(o.pc_addr), "sysmodule/SceLibKernel+0x6304");
    CHECK_STR(host_addr_str(o.lr_addr), "sysmodule/SceLibKernel+0xa957");
    SessionRecord nojit = dump_session(); nojit.jit_lo = nojit.jit_hi = 0;
    const DumpFacts u = read_psp2dmp(*io, dump_path("first_jit.psp2dmp"), nojit, SecretPatterns());
    CHECK_STR(host_addr_str(u.pc_addr), "unknown/unknown+0x96924184");
    // A system module name outside ^[A-Za-z0-9_.]{1,32}$ cannot be written in
    // a claim: the address goes to region unknown, offset = the address
    // itself (contract signature-rules.v1.md §7).
    const DumpFacts sm = read_psp2dmp(*io, dump_path("d2_sysmodule.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(sm.ok, "not ok: %s", sm.error.c_str());
    CHECK_STR(host_addr_str(sm.pc_addr), "sysmodule/SceLibKernel+0x6304");
    CHECK_STR(host_addr_str(sm.lr_addr), "unknown/unknown+0xe1004321");
    CHECK_STR(stop_reason_name(0), "none");
    CHECK_STR(stop_reason_name(0x30003), "prefetch_abort");
    CHECK_STR(stop_reason_name(0x60080), "0x60080");
}

static void t_psp2dmp_chain_rules_and_damage() {
    auto io = make_posix_io();
    const DumpFacts t = read_psp2dmp(*io, dump_path("truncated.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(t.ok, "truncated dump must still give facts: %s", t.error.c_str());
    CHECK(t.truncated, "truncation not reported");
    CHECK_STR(chain_str(t, 0x01900000), "0f2e1028:100,0f2e1100:200");
    CHECK_STR(t.chain_end, "bad_link");

    const DumpFacts l = read_psp2dmp(*io, dump_path("long_chain.psp2dmp"), dump_session(), SecretPatterns());
    CHECK_U64(l.guest_chain.size(), kEbpChainMaxFrames);
    CHECK_STR(l.chain_end, "max_frames");

    const DumpFacts c = read_psp2dmp(*io, dump_path("gzip_cut.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(c.ok && c.truncated, "cut gzip: notes readable, truncation reported (ok=%d truncated=%d)", c.ok, c.truncated);
    CHECK_U64(c.tid, 0x40010003u);

    const DumpFacts n = read_psp2dmp(*io, dump_path("not_elf.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(!n.ok && !n.error.empty(), "garbage accepted");
    const DumpFacts m = read_psp2dmp(*io, g_dumps + "/missing.psp2dmp", dump_session(), SecretPatterns());
    CHECK(!m.ok && !m.error.empty(), "missing file accepted");

    // Thread notes only after the memory (hypothetical layout): the thread is
    // still named, but its stack has streamed by before the notes arrived.
    const DumpFacts late = read_psp2dmp(*io, dump_path("late_threads.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(late.ok && late.tid == 0x40010003u, "late thread notes: ok=%d tid=0x%x", late.ok, (unsigned)late.tid);
    CHECK_STR(late.chain_end, "outside_capture");

    DumpReadOptions notes;
    notes.notes_only = true;
    const DumpFacts q = read_psp2dmp(*io, dump_path("big.psp2dmp"), dump_session(), SecretPatterns(), notes);
    CHECK(q.ok && q.has_stamp && q.tid == 0x40010003u, "notes-only read");
    CHECK_STR(q.chain_end, "notes_only");
    CHECK(q.decompressed_bytes < 1024 * 1024, "notes-only read went through %" PRIu64 " bytes", q.decompressed_bytes);
}

// A stamp is "D2VSTAMP1 session_id=<32 lowercase hex> ..." inside a PT_NOTE
// segment; stamp-like bytes elsewhere, or a malformed id, are no stamp.
static void t_psp2dmp_stamp_rules() {
    auto io = make_posix_io();
    const DumpFacts l = read_psp2dmp(*io, dump_path("stamp_lookalikes.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(l.ok, "not ok: %s", l.error.c_str());
    CHECK(!l.has_stamp, "stamp taken from memory: session_id=%s", l.stamp_session_id.c_str());
    const DumpFacts m = read_psp2dmp(*io, dump_path("stamp_malformed_note.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(m.ok && !m.has_stamp, "malformed stamp accepted: session_id=%s", m.stamp_session_id.c_str());
    const DumpFacts t = read_psp2dmp(*io, dump_path("stamp_trailing.psp2dmp"), dump_session(), SecretPatterns());
    CHECK(t.ok && t.has_stamp, "stamp in the trailing note group not found");
    CHECK_STR(t.stamp_session_id, "0123456789abcdef0123456789abcdef");
    CHECK_STR(t.stamp_build_id, "0.1.0+ab12cd34ef56");
    DumpReadOptions notes;
    notes.notes_only = true;
    const DumpFacts q = read_psp2dmp(*io, dump_path("stamp_trailing.psp2dmp"), dump_session(), SecretPatterns(), notes);
    CHECK(q.ok && !q.has_stamp, "the notes-only read stops before the trailing group (has_stamp=%d, id=%s)", q.has_stamp,
          q.stamp_session_id.c_str());
}

static void t_psp2dmp_peak_memory() {
    auto io = make_posix_io();
    const SecretPatterns pats = fake_patterns();
    const std::string path = dump_path("big.psp2dmp");
    const size_t before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    const DumpFacts f = read_psp2dmp(*io, path, dump_session(), pats);
    const size_t peak = g_heap_peak - before;
    CHECK(f.ok, "big dump not ok: %s", f.error.c_str());
    CHECK(f.decompressed_bytes > 8u * 1024 * 1024, "big dump only %" PRIu64 " bytes", f.decompressed_bytes);
    CHECK_STR(chain_str(f, 0x01900000), "0f2e1028:51c23,0f2e1100:4f570,0f2f0f00:12345");
    if (kAllocCounter) {
        std::printf("    read_psp2dmp peak heap: %zu bytes for %" PRIu64 " decompressed bytes\n", peak, f.decompressed_bytes);
        CHECK(peak < 1024u * 1024u, "peak heap %zu >= 1 MiB", peak);
    }
}

// ---------------------------------------------------------------- cr_evidence

static const int64_t kStarted = 1789284000, kNow = 1789290000;

struct Scenario {
    std::string root, data_dir, progress;
    SessionRecord s;
};

static Scenario make_scenario(const char* name) {
    Scenario sc;
    sc.root = test_dir(name);
    sc.data_dir = sc.root + "/data";
    const std::string cmd = "mkdir -p '" + sc.data_dir + "' '" + sc.root + "/d2vita/save2'";
    if (std::system(cmd.c_str()) != 0) std::exit(2);
    sc.progress = sc.root + "/d2vita/boot_progress.txt";
    sc.s = dump_session();
    sc.s.build_id = "0.1.0+ab12cd34ef56";
    sc.s.started_unix = kStarted;
    sc.s.write_root = sc.root + "/d2vita/save2";
    sc.s.progress_path = sc.progress;
    sc.s.state = "running";
    return sc;
}

static std::string kinds_str(const std::vector<Kind>& v) {
    std::string r;
    for (Kind k : v) { if (!r.empty()) r += ','; r += kind_name(k); }
    return r;
}

static void put_dump(const Scenario& sc, const char* synthetic, int64_t epoch) {
    char name[128];
    std::snprintf(name, sizeof name, "/psp2core-%lld-0x00005f245d-eboot.bin.psp2dmp", (long long)epoch);
    put_file(sc.data_dir + name, get_file(dump_path(synthetic)));
}

static void t_evidence_none_and_hang() {
    auto io = make_posix_io();
    {
        Scenario sc = make_scenario("ev_none");
        put_file(sc.progress, fixture("progress_no_evidence_real.txt"));
        EvidenceDetails d;
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns(), &d);
        CHECK(e.kind == Kind::None, "state=running without evidence must give no report (got %s)", kind_name(e.kind));
        CHECK(e.hints.empty() && e.features_json.empty(), "no hints/features expected");
        CHECK_STR(e.progress_path, sc.progress);
        CHECK_U64(d.uptime_s, 36);
    }
    {
        // This fixture (a stalled "alive:" heartbeat, state=running) used to
        // classify Kind::Hang before automatic hang detection was removed
        // (see cr_evidence.h's file comment) — the exact same fixture now
        // yields no evidence at all, in either session state.
        Scenario sc = make_scenario("ev_hang");
        put_file(sc.progress, fixture("progress_hang_real.txt"));
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::None, "a stalled heartbeat alone is no longer a hang (got %s)", kind_name(e.kind));
        CHECK(e.hints.empty() && e.features_json.empty(), "no hints/features expected");
        sc.s.state = "exited";
        const Evidence x = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(x.kind == Kind::None, "same fixture, exited state: still no evidence (got %s)", kind_name(x.kind));
    }
    {
        // Frozen in the shim trap region, outside Game.exe, on a build
        // without the run= field: previously an ABS eip, unknown-runner-
        // state Kind::Hang; now no evidence at all (see the "ev_hang"
        // comment above).
        Scenario sc = make_scenario("ev_hang_abs");
        put_file(sc.progress,
                 "[ 100.00s] alive: pump=9 frames=500 reads=3 eip=10e01b30 sw=1\n"
                 "[ 110.00s] alive: pump=9 frames=500 reads=3 eip=10e01b30 sw=1\n"
                 "[ 120.00s] alive: pump=9 frames=500 reads=3 eip=10e01b30 sw=1\n");
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::None, "no evidence expected (got %s)", kind_name(e.kind));
        CHECK(e.hints.empty() && e.features_json.empty(), "no hints/features expected");
    }
    {
        Scenario sc = make_scenario("ev_nothing_at_all");
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::None && e.progress_path.empty() && e.crash_log_path.empty(), "no files, no evidence");
    }
}

static void t_evidence_guest_fault_and_abnormal() {
    auto io = make_posix_io();
    {
        Scenario sc = make_scenario("ev_guest");
        put_file(sc.progress, fixture("progress_native_fault_block.txt"));
        EvidenceDetails d;
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns(), &d);
        CHECK(e.kind == Kind::GuestFault, "guest_fault expected (got %s)", kind_name(e.kind));
        CHECK_STR(e.features_json,
            "{\"exception\":null,\"thread\":\"worker\",\"eip\":\"Game+0x2f040\","
            "\"frames\":[\"Game+0x51c23\",\"Game+0x4f570\",\"ABS+0x3012345\",\"Game+0x4b959\"]}");
        CHECK_U64(d.native_fault_line, 63);
    }
    {
        Scenario sc = make_scenario("ev_abnormal");
        put_file(sc.progress, fixture("progress_abnormal_exit.txt"));
        put_file(sc.s.write_root + "/crash.log", "=== D2Vita boot (build Sep 14 2026 09:54:00) ===\n");
        sc.s.state = "exited";
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::AbnormalExit, "abnormal_exit expected (got %s)", kind_name(e.kind));
        CHECK_STR(e.features_json,
            "{\"reason\":\"exit_process\",\"code\":4294967295,\"import\":null,"
            "\"frames\":[\"Game+0x97be\",\"Game+0xa0ed\",\"Game+0xb451\",\"Game+0x20c108\"]}");
        CHECK_STR(e.crash_log_path, sc.s.write_root + "/crash.log");
    }
    {
        // Session record alone (no log): unshimmed import.
        Scenario sc = make_scenario("ev_abnormal_session");
        sc.s.stop_reason = "UNSHIMMED KERNEL32.dll!GetNumaHighestNodeNumber";
        sc.s.has_main_exit = true;
        sc.s.main_exit = 0;
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::AbnormalExit, "abnormal_exit expected (got %s)", kind_name(e.kind));
        CHECK_STR(e.features_json,
            "{\"reason\":\"unshimmed_import\",\"code\":null,\"import\":\"KERNEL32.dll!GetNumaHighestNodeNumber\",\"frames\":[]}");
        // A normal quit recorded in the session is not evidence.
        sc.s.stop_reason = "shutdown requested";
        CHECK(build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns()).kind == Kind::None, "normal stop");
        sc.s.main_exit = 0xc0000005u;
        const Evidence m = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        // 0xC0000005 is the fallback exit code the scheduler gives a thread that
        // faulted without the emulator being able to name the fault.
        CHECK_STR(m.features_json, "{\"reason\":\"main_thread_fault\",\"code\":3221225477,\"import\":null,\"frames\":[]}");
        // ...and a NAMED fault must classify the same way, carrying its own code:
        // matching only the fallback reported a divide-by-zero as an ordinary exit.
        sc.s.main_exit = 0xc0000094u;
        const Evidence mdiv = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK_STR(mdiv.features_json, "{\"reason\":\"main_thread_fault\",\"code\":3221225620,\"import\":null,\"frames\":[]}");
        sc.s.main_exit = 0xc000001du;
        const Evidence mill = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK_STR(mill.features_json, "{\"reason\":\"main_thread_fault\",\"code\":3221225501,\"import\":null,\"frames\":[]}");
        sc.s.main_exit = 1;
        sc.s.stop_reason = "main exited";
        const Evidence x = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK_STR(x.features_json, "{\"reason\":\"exit_process\",\"code\":1,\"import\":null,\"frames\":[]}");
        sc.s.main_exit = 3;
        sc.s.stop_reason = "TerminateProcess";
        const Evidence t = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK_STR(t.features_json, "{\"reason\":\"exit_process\",\"code\":3,\"import\":null,\"frames\":[]}");
        // A fault reason alone, with a clean main exit code, names no crash of the run.
        sc.s.main_exit = 0;
        sc.s.stop_reason = "box86 dynarec fault (unimplemented/illegal/div0)";
        CHECK(build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns()).kind == Kind::None, "fault reason without exit code");
        // Import names outside the contract alphabet are made safe, never dropped.
        sc.s.stop_reason = "UNSHIMMED WEIRD dll!Bad Name|x";
        const Evidence w = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK_STR(w.features_json, "{\"reason\":\"unshimmed_import\",\"code\":null,\"import\":\"WEIRD_dll!Bad_Name_x\",\"frames\":[]}");
    }
}

static void t_evidence_halt() {
    auto io = make_posix_io();
    Scenario sc = make_scenario("ev_halt");
    const std::string ct = sc.s.write_root + "/Crash.txt";
    put_file(ct, fixture("crash_txt_halt1420.txt"));
    set_mtime(ct, kStarted + 600);
    const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
    CHECK(e.kind == Kind::Halt, "halt expected (got %s)", kind_name(e.kind));
    CHECK_STR(e.features_json,
        "{\"code\":1420,\"location\":null,\"frames\":[\"Game+0x1fecc5\",\"Game+0x2003a7\",\"Game+0xdbda8\","
        "\"Game+0x71958\",\"Game+0xdf5f4\",\"Game+0x76cf8\",\"Game+0x4cafd\",\"Game+0x4f291\",\"Game+0x51c23\","
        "\"Game+0x4f570\",\"Game+0x4b959\",\"Game+0x4b43c\",\"Game+0x5e6b\",\"Game+0x65c7\",\"Game+0x671f\",\"Game+0x28291c\"]}");
    CHECK_STR(e.crash_txt_path, ct);
    // Written before the session started: a leftover, not evidence.
    set_mtime(ct, kStarted - 1);
    CHECK(build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns()).kind == Kind::None, "old Crash.txt");
    // Created but never flushed: still a Halt, unnamed.
    put_file(ct, "");
    set_mtime(ct, kStarted);
    const Evidence z = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
    CHECK(z.kind == Kind::Halt, "empty Crash.txt of this session is a halt (got %s)", kind_name(z.kind));
    CHECK_STR(z.features_json, "{\"code\":0,\"location\":null,\"frames\":[]}");
    // Location present.
    put_file(ct, fixture("crash_txt_halt904_location.txt"));
    set_mtime(ct, kStarted + 1);
    const Evidence l = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
    CHECK(l.features_json.find("\"code\":904,\"location\":\"Codec.cpp:1377\"") != std::string::npos, "%s", l.features_json.c_str());
}

static void t_evidence_host_fault() {
    auto io = make_posix_io();
    {
        // Matched by stamp even though the file epoch is outside the session.
        Scenario sc = make_scenario("ev_host_stamp");
        put_dump(sc, "first_jit.psp2dmp", kStarted - 86400);
        put_dump(sc, "other_app_sysmodule.psp2dmp", kStarted + 10);   // another application
        EvidenceDetails d;
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns(), &d);
        CHECK(e.kind == Kind::HostFault, "host_fault expected (got %s)", kind_name(e.kind));
        CHECK_STR(e.features_json,
            "{\"stop_reason\":\"0x30004\",\"thread_name\":\"DTWO00001\","
            "\"pc\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24184\"},"
            "\"lr\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24101\"},"
            "\"guest_frames\":[\"Game+0x51c23\",\"Game+0x4f570\",\"Game+0x4b959\",\"Game+0x4b43c\",\"Game+0x5e6b\"],"
            "\"redaction\":\"clean\"}");
        CHECK(!e.dump_withheld, "clean dump");
        CHECK(e.dump_path.find("psp2core-1789197600-") != std::string::npos, "dump path %s", e.dump_path.c_str());
        CHECK(d.dump_by_stamp && d.dump_epoch == kStarted - 86400 && d.fw == "3.65" && d.dump_bytes > 0, "details");
        // Another session's stamp never matches, whatever its date.
        sc.s.session_id = "ffffffffffffffffffffffffffffffff";
        CHECK(build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns()).kind == Kind::None, "foreign stamp");
    }
    {
        // Stamp-like bytes that are no stamp (in memory, malformed, or beyond
        // the notes the matching read looks at) never veto the fallback on
        // epoch + title.
        for (const char* name : {"stamp_lookalikes.psp2dmp", "stamp_malformed_note.psp2dmp", "stamp_trailing.psp2dmp"}) {
            Scenario sc = make_scenario("ev_host_stamp_lookalike");
            put_dump(sc, name, kStarted + 300);
            EvidenceDetails d;
            const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns(), &d);
            CHECK(e.kind == Kind::HostFault && !d.dump_by_stamp, "%s: host_fault by epoch expected (got %s, by_stamp=%d)", name,
                  kind_name(e.kind), d.dump_by_stamp);
        }
    }
    {
        // No stamp: epoch within [started_unix, now] and the D2Vita title.
        Scenario sc = make_scenario("ev_host_epoch");
        put_dump(sc, "eboot_nostamp.psp2dmp", kStarted - 5);
        CHECK(build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns()).kind == Kind::None, "epoch before the session");
        put_dump(sc, "eboot_nostamp.psp2dmp", kStarted + 100);
        EvidenceDetails d;
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns(), &d);
        CHECK(e.kind == Kind::HostFault && !d.dump_by_stamp && d.dump_epoch == kStarted + 100, "fallback match");
        CHECK(e.features_json.find("\"pc\":{\"region\":\"eboot\",\"module\":\"eboot\",\"offset\":\"0x2f6e6e\"}") != std::string::npos,
              "%s", e.features_json.c_str());
        CHECK(e.features_json.find("\"guest_frames\":[]") != std::string::npos, "%s", e.features_json.c_str());
    }
    {
        // PC in a system module, LR in a module whose name the contract
        // cannot carry: sysmodule with the name as listed, and unknown.
        Scenario sc = make_scenario("ev_host_sysmodule");
        put_dump(sc, "d2_sysmodule.psp2dmp", kStarted + 200);
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::HostFault, "host_fault expected (got %s)", kind_name(e.kind));
        CHECK_STR(e.features_json,
            "{\"stop_reason\":\"0x30004\",\"thread_name\":\"DTWO00001\","
            "\"pc\":{\"region\":\"sysmodule\",\"module\":\"SceLibKernel\",\"offset\":\"0x6304\"},"
            "\"lr\":{\"region\":\"unknown\",\"module\":\"unknown\",\"offset\":\"0xe1004321\"},"
            "\"guest_frames\":[\"Game+0x51c23\"],\"redaction\":\"clean\"}");
    }
    {
        // Secrets inside: dump withheld.
        Scenario sc = make_scenario("ev_host_withheld");
        put_dump(sc, "worker_withheld.psp2dmp", kStarted + 50);
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, fake_patterns());
        CHECK(e.kind == Kind::HostFault, "host_fault expected (got %s)", kind_name(e.kind));
        CHECK(e.dump_withheld, "dump with secrets must be withheld");
        CHECK(e.features_json.find("\"thread_name\":\"d2_runner3\"") != std::string::npos, "%s", e.features_json.c_str());
        CHECK(e.features_json.find("\"redaction\":\"withheld\"") != std::string::npos, "%s", e.features_json.c_str());
    }
    {
        // Several kinds at once: the most severe wins, the others are hints.
        Scenario sc = make_scenario("ev_all");
        put_dump(sc, "first_jit.psp2dmp", kStarted + 900);
        const std::string ct = sc.s.write_root + "/Crash.txt";
        put_file(ct, fixture("crash_txt_halt1420.txt"));
        set_mtime(ct, kStarted + 890);
        put_file(sc.progress, fixture("progress_native_fault_block.txt") + fixture("progress_abnormal_exit.txt"));
        const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, SecretPatterns());
        CHECK(e.kind == Kind::HostFault, "host_fault expected (got %s)", kind_name(e.kind));
        CHECK_STR(kinds_str(e.hints), "halt,abnormal_exit,guest_fault");
    }
}

// A long console session: ~4.6 MiB of boot_progress in the writers' formats
// (beats of ~240 bytes with run= every 10 s among other lines, as in a real
// ~2.9 MiB console log), a NATIVE FAULT block of worker 3 near
// the end (line kLongFaultLine), then three beats with frozen frames, the
// last at kLongLastSecond.
static const int kLongLines = 44000, kLongFaultLine = 43005, kLongLastSecond = 44000 / 6 + 30;
static std::string long_session_progress() {
    std::string t = "[   0.00s] env.txt: D2NET=1\n";
    char line[400];
    int frames = 0;
    unsigned blocks = 1000;
    for (int i = 0; i < kLongLines; ++i) {
        const int s = i / 6;
        if (i % 6 == 0) {
            frames += 250;
            blocks += 123457;
            std::snprintf(line, sizeof line,
                          "[%4d.00s] alive: pump=%d frames=%d reads=883 eip=10e014a0 sw=%d io=1121ms jit=1129ms n=2632 "
                          "sync=162ms fail=0 va=13MB spr=0/8MB cel=0/500KB big=0 jm=288MB fam=4/6 gil=- "
                          "run=1:R:%u,2:B:7,3:B:%u,4:B:88040\n", s, i * 7, frames, i * 9, blocks, blocks / 3);
        } else {
            std::snprintf(line, sizeof line, "[%4d.00s] present: frame %06d cells=%d blit=%d spr=%d/%d draw=%dms swap=%dms\n",
                          s, i, i % 977, i % 131, i % 53, i % 29, i % 17, i % 13);
        }
        t += line;
        if (i == kLongFaultLine - 5) {
            t += "[2666.00s]   fault thr 3: EAX=85800004 EBX=00000000 ECX=0000001c EDX=00000004 ESI=85800024 EDI=21600001 EBP=0f2ffe48 ESP=0f2ffe20\n";
            t += "[2666.00s]   fault module @01900000 taille 005ba000  <- EIP ICI\n";
            t += "[2666.00s]   fault pile [esp+0x04] 0x01951c23 (module @01900000 +0x51c23)\n";
            t += "[2666.00s] NATIVE FAULT thr 3 eip=0192f040 addr=0192f040\n";
        }
    }
    for (int i = 0; i < 3; ++i) {
        std::snprintf(line, sizeof line,
                      "[%4d.00s] alive: pump=1 frames=%d reads=883 eip=0192f040 sw=1 run=1:B:%u,2:B:7,3:B:%u,4:B:88040\n",
                      kLongLastSecond - 20 + 10 * i, frames, blocks, blocks / 3);
        t += line;
    }
    return t;
}

static void t_evidence_peak_memory() {
    auto io = make_posix_io();
    Scenario sc = make_scenario("ev_peak");
    uint64_t log_bytes = 0;
    {
        const std::string text = long_session_progress();
        log_bytes = text.size();
        CHECK(text.size() > 4u * 1024 * 1024, "long session log is only %zu bytes", text.size());
        put_file(sc.progress, text);
    }
    const SecretPatterns pats = fake_patterns();
    EvidenceDetails d;
    const size_t before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    const Evidence e = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &d);
    const size_t peak = g_heap_peak - before;
    CHECK(e.kind == Kind::GuestFault, "guest_fault expected (got %s)", kind_name(e.kind));
    // The trailing frozen beats used to add a "hang" hint alongside the
    // primary guest_fault; automatic hang detection was removed, so no hint
    // is produced from them any more (nothing else in this fixture triggers
    // any other Kind).
    CHECK_STR(kinds_str(e.hints), "");
    CHECK_STR(e.features_json,
        "{\"exception\":null,\"thread\":\"worker\",\"eip\":\"Game+0x2f040\",\"frames\":[\"Game+0x51c23\"]}");
    CHECK(d.online && d.uptime_s == kLongLastSecond, "details: online=%d uptime=%lld", d.online, (long long)d.uptime_s);
    CHECK_U64(d.native_fault_line, kLongFaultLine);
    if (kAllocCounter) {
        std::printf("    build_evidence peak heap: %zu bytes for a %" PRIu64 "-byte boot_progress\n", peak, log_bytes);
        CHECK(peak < 1024u * 1024u, "build_evidence peak heap %zu >= 1 MiB (spec 4.9)", peak);
    }
}

// ---------------------------------------------------------------- cr_outbox

static void t_outbox_sizes_and_codecs() {
    CHECK_U64(sealed_size(0), 72 + 16);
    CHECK_U64(sealed_size(65535), 72 + 16 + 65535);
    CHECK_U64(sealed_size(65536), 72 + 32 + 65536);          // exact multiple: extra empty last block
    CHECK_U64(max_plain_for_sealed(kSealedCapCrashLog), 65448);
    CHECK_U64(sealed_size(max_plain_for_sealed(kSealedCapBootProgress)), kSealedCapBootProgress);
    CHECK(sealed_size(max_plain_for_sealed(kSealedCapDump) + 1) > kSealedCapDump, "dump cap not maximal");
    CHECK(sealed_size(max_plain_for_sealed(kSealedCapDump)) <= kSealedCapDump, "dump cap exceeded");

    ReportState st;
    st.attempts = 2; st.prompts = 1; st.created_unix = kStarted; st.next_attempt_unix = kNow; st.consent = "granted";
    ReportState back;
    CHECK(parse_report_state(serialize_report_state(st), &back), "state round trip");
    CHECK(back.attempts == 2 && back.prompts == 1 && back.created_unix == kStarted &&
          back.next_attempt_unix == kNow && back.consent == "granted", "state fields");
    CHECK(!parse_report_state("attempts=1\nconsent=maybe\ncreated_unix=5\n", &back), "unknown consent accepted");
    CHECK(!parse_report_state("attempts=1\n", &back), "state without created_unix accepted");

    ReportRecord r;
    r.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCD"; r.session_id = "abc"; r.build_id = "0.1.0+ab12cd34ef56";
    r.kind = Kind::Halt; r.hints.push_back(Kind::GuestFault); r.hints.push_back(Kind::Hang);
    r.features_json = "{\"code\":1420,\"location\":null,\"frames\":[\"Game+0x1\"]}";
    r.started_unix = kStarted; r.uptime_s = 967; r.online = true; r.fw = "3.65"; r.redactions = 4; r.dump = "none";
    ArtifactFile a; a.name = "crash_txt"; a.file = "crash_txt.txt"; a.bytes = 2210; r.artifacts.push_back(a);
    a.name = "boot_progress"; a.file = "boot_progress.txt"; a.bytes = 262144; r.artifacts.push_back(a);
    ReportRecord rb;
    CHECK(parse_report_record(serialize_report_record(r), &rb), "record round trip");
    CHECK(rb.report_id == r.report_id && rb.session_id == "abc" && rb.build_id == r.build_id && rb.kind == Kind::Halt &&
          kinds_str(rb.hints) == "guest_fault,hang" && rb.features_json == r.features_json && rb.started_unix == kStarted &&
          rb.uptime_s == 967 && rb.online && rb.fw == "3.65" && rb.redactions == 4 && rb.dump == "none", "record fields");
    CHECK(rb.artifacts.size() == 2 && rb.artifacts[1].name == "boot_progress" && rb.artifacts[1].file == "boot_progress.txt" &&
          rb.artifacts[1].bytes == 262144, "record artifacts");
}

// boot_progress of ~800 KiB: a NATIVE FAULT around 300 KiB (between the kept
// head and tail), a fake key across the 16 KiB head boundary, another one
// next to the fault line, an account name in the tail.
static std::string big_progress(size_t* fault_offset) {
    std::string t;
    char line[160];
    for (int i = 0; i < 8000; ++i) {
        if (i == 3000) {
            *fault_offset = t.size();
            t += "[ 300.00s] NATIVE FAULT thr 3 eip=0192f040 addr=0192f040\n";
            continue;
        }
        std::snprintf(line, sizeof line, "[%4d.00s] line %06d: filler text for the crash report outbox test ....\n", i / 10, i);
        t += line;
        if (t.size() >= 16 * 1024 - 8 && t.size() < 16 * 1024 + 100 && t.find("FAKE-1234") == std::string::npos) {
            t.resize(16 * 1024 - 8);
            t += "FAKE-1234-CLAS-5678\n";
        }
        if (i == 2990) t += "key near fault: fakelodkey0123456789abcdef\n";
        if (i == 7900) t += "account FAKEACCOUNT logged\n";
    }
    return t;
}

static Scenario outbox_scenario(const char* name, std::string* progress_text, size_t* fault_offset) {
    Scenario sc = make_scenario(name);
    *progress_text = big_progress(fault_offset);
    put_file(sc.progress, *progress_text);
    std::string log;
    for (int i = 0; i < 2000; ++i) { char b[96]; std::snprintf(b, sizeof b, "crashlog line %04d owner Fake Owner\n", i); log += b; }
    put_file(sc.s.write_root + "/crash.log", log);                       // ~70 KiB, over its cap
    put_file(sc.s.write_root + "/Crash.txt", fixture("crash_txt_halt1420.txt"));
    set_mtime(sc.s.write_root + "/Crash.txt", kStarted + 10);
    return sc;
}

static void t_outbox_create_full() {
    auto io = make_posix_io();
    std::string progress;
    size_t fault_off = 0;
    Scenario sc = outbox_scenario("ob_full", &progress, &fault_off);
    put_dump(sc, "first_jit.psp2dmp", kStarted - 86400);
    const SecretPatterns pats = fake_patterns();
    CollectInputs in;
    in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCD";
    in.session = sc.s;
    in.evidence = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &in.details);
    in.now_unix = kNow;
    CHECK(in.evidence.kind == Kind::HostFault, "setup: host_fault expected");
    const std::string dump_src = in.evidence.dump_path;
    const std::string root = sc.root + "/reports_outbox";
    Outbox ob(*io, root);
    ReportRecord rec;
    std::string err;
    CHECK(ob.create(in, pats, &rec, &err), "create failed: %s", err.c_str());
    const std::string d = root + "/" + in.report_id;

    // boot_progress: head, the fault window, tail; within its cap; redacted.
    const std::string bp = get_file(d + "/boot_progress.txt");
    CHECK(!bp.empty() && sealed_size(bp.size()) <= kSealedCapBootProgress, "boot_progress size %zu", bp.size());
    CHECK(bp.compare(0, 64, progress, 0, 64) == 0, "head does not start the file");
    CHECK(bp.find("NATIVE FAULT thr 3 eip=0192f040") != std::string::npos, "fault window missing");
    const std::string tail_ref = progress.substr(progress.size() - 1000);
    CHECK(bp.size() >= 1000 && bp.compare(bp.size() - 1000, 1000, tail_ref) == 0, "tail does not end the file");
    CHECK(bp.find("account XXXXXXXXXXX logged") != std::string::npos, "account name in the tail not redacted");
    CHECK(bp.find("=== d2cr:") != std::string::npos, "omission marker missing");
    CHECK(bp.find("FAKE-123") == std::string::npos && bp.find("fakelodkey") == std::string::npos &&
          bp.find("FAKEACCOUNT") == std::string::npos, "secret left in boot_progress");
    CHECK(bp.find("XXXXXXXXXXXXXXXXXXX") != std::string::npos, "redaction marks missing");
    // crash.log: its end, within cap, owner redacted.
    const std::string cl = get_file(d + "/crash_log.txt");
    CHECK(!cl.empty() && sealed_size(cl.size()) <= kSealedCapCrashLog, "crash_log size %zu", cl.size());
    CHECK(cl.find("line 1999") != std::string::npos && cl.find("line 0000") == std::string::npos, "crash_log must keep the end");
    CHECK(cl.find("Fake Owner") == std::string::npos, "owner left in crash.log copy");
    // Crash.txt copied then deleted.
    CHECK(get_file(d + "/crash_txt.txt").find("failed at (1420)") != std::string::npos, "Crash.txt copy");
    CHECK(!exists(sc.s.write_root + "/Crash.txt"), "Crash.txt must be deleted after copy");
    // The dump moved, bytes unchanged.
    CHECK(!exists(dump_src), "dump must be moved away");
    CHECK(get_file(d + "/dump.psp2dmp") == get_file(dump_path("first_jit.psp2dmp")), "moved dump differs");
    // Sources that are only copied stay.
    CHECK(exists(sc.progress) && exists(sc.s.write_root + "/crash.log"), "logs must stay in place");

    CHECK(rec.kind == Kind::HostFault && rec.dump == "included" && rec.redactions > 0, "record: kind/dump/redactions");
    CHECK_STR(rec.features_json, in.evidence.features_json);
    ReportRecord loaded;
    ReportState st;
    CHECK(ob.load(in.report_id, &loaded, &st), "load");
    CHECK(st.consent == "pending" && st.prompts == 0 && st.attempts == 0 && st.created_unix == kNow, "fresh state");
    CHECK(loaded.artifacts.size() == 4, "artifacts listed: %zu", loaded.artifacts.size());
    uint64_t on_disk = 0;
    for (const ArtifactFile& a : loaded.artifacts) {
        DirEntry e;
        CHECK(io->stat(d + "/" + a.file, &e) && e.size == a.bytes, "artifact %s size mismatch", a.name.c_str());
        on_disk += a.bytes;
    }
    CHECK(ob.report_bytes(in.report_id) >= on_disk, "report_bytes");
    CHECK(loaded.session_id == sc.s.session_id && loaded.build_id == sc.s.build_id && loaded.fw == "3.65" &&
          loaded.started_unix == kStarted, "record session fields");
    std::vector<std::string> ids = ob.list();
    CHECK(ids.size() == 1 && ids[0] == in.report_id, "list");

    // Same id again: refused, and nothing moves.
    std::string err2;
    CHECK(!ob.create(in, pats, &rec, &err2), "duplicate report id accepted");
    CHECK(ob.remove(in.report_id) && !exists(d), "remove");
}

static void t_outbox_create_small_and_dump_cases() {
    auto io = make_posix_io();
    {
        // Small log copied whole; withheld dump moved but not offered.
        Scenario sc = make_scenario("ob_withheld");
        put_file(sc.progress, fixture("progress_native_fault_block.txt"));
        put_dump(sc, "worker_withheld.psp2dmp", kStarted + 50);
        const SecretPatterns pats = fake_patterns();
        CollectInputs in;
        in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCE";
        in.session = sc.s;
        in.evidence = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &in.details);
        in.now_unix = kNow;
        Outbox ob(*io, sc.root + "/outbox");
        ReportRecord rec;
        std::string err;
        CHECK(ob.create(in, pats, &rec, &err), "create: %s", err.c_str());
        CHECK_STR(get_file(sc.root + "/outbox/" + in.report_id + "/boot_progress.txt"), fixture("progress_native_fault_block.txt"));
        CHECK_STR(rec.dump, "withheld");
        bool offered = false;
        for (const ArtifactFile& a : rec.artifacts) offered = offered || a.name == "dump";
        CHECK(!offered, "a withheld dump must not be listed as an artifact");
        CHECK(exists(sc.root + "/outbox/" + in.report_id + "/dump.psp2dmp") && !exists(in.evidence.dump_path), "withheld dump still moved");
    }
    {
        // Nothing to report: no directory is created.
        Scenario sc = make_scenario("ob_none");
        CollectInputs in;
        in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCG";
        in.session = sc.s;
        in.now_unix = kNow;
        Outbox ob(*io, sc.root + "/outbox");
        ReportRecord rec;
        std::string err;
        CHECK(!ob.create(in, SecretPatterns(), &rec, &err), "a report without evidence was created");
        CHECK(!exists(sc.root + "/outbox/" + in.report_id), "directory left behind");
    }
    {
        // A dump over its cap stays where it is.
        Scenario sc = make_scenario("ob_toolarge");
        std::string big = get_file(dump_path("first_jit.psp2dmp"));
        big.resize(2 * 1024 * 1024 + 1, '\0');
        put_file(sc.data_dir + "/psp2core-1789285000-0x1-eboot.bin.psp2dmp", big);
        CollectInputs in;
        in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYCF";
        in.session = sc.s;
        in.evidence.kind = Kind::HostFault;
        in.evidence.features_json = "{}";
        in.evidence.dump_path = sc.data_dir + "/psp2core-1789285000-0x1-eboot.bin.psp2dmp";
        in.now_unix = kNow;
        Outbox ob(*io, sc.root + "/outbox");
        ReportRecord rec;
        std::string err;
        CHECK(ob.create(in, SecretPatterns(), &rec, &err), "create: %s", err.c_str());
        CHECK_STR(rec.dump, "too_large");
        CHECK(exists(in.evidence.dump_path), "an oversized dump must stay in place");
        CHECK(rec.artifacts.empty(), "no pieces at all in this report");
    }
}

static bool all_bytes(const std::string& s, size_t pos, size_t n, char c) {
    if (pos > s.size() || s.size() - pos < n) return false;
    for (size_t i = 0; i < n; ++i) if (s[pos + i] != c) return false;
    return true;
}

// Outbox::create stays within the 1 MiB heap budget of spec 4.9: on the long
// session log (fault inside the kept tail), and on a log whose 100 lines on
// each side of a fault that falls between head and tail are 60 KiB long.
static void t_outbox_peak_memory() {
    auto io = make_posix_io();
    const SecretPatterns pats = fake_patterns();
    const char* const kFault = "NATIVE FAULT thr 3 eip=0192f040 addr=0192f040";
    for (int variant = 0; variant < 2; ++variant) {
        Scenario sc = make_scenario(variant ? "ob_peak_long_lines" : "ob_peak_long_session");
        uint64_t log_bytes = 0;
        {
            std::string text;
            if (variant == 0) {
                text = long_session_progress();
            } else {
                char line[160];
                for (int i = 0; i < 2000; ++i) {
                    std::snprintf(line, sizeof line, "[%4d.00s] head filler line %06d for the outbox heap test\n", i / 10, i);
                    text += line;
                }
                const std::string big(60 * 1024, 'y');
                for (int i = 0; i < 100; ++i) text += "[ 200.00s] " + big + "\n";
                text += "[ 201.00s] " + std::string(kFault) + "\n";
                for (int i = 0; i < 100; ++i) text += "[ 202.00s] " + big + "\n";
                for (int i = 0; i < 3000; ++i) {
                    std::snprintf(line, sizeof line, "[%4d.00s] tail filler line %06d for the outbox heap test\n", 210 + i / 10, i);
                    text += line;
                }
            }
            log_bytes = text.size();
            put_file(sc.progress, text);
        }
        CollectInputs in;
        in.report_id = variant ? "01J9Z6T4Q8M3K7V2B5N0XWAYC1" : "01J9Z6T4Q8M3K7V2B5N0XWAYC0";
        in.session = sc.s;
        in.evidence = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &in.details);
        in.now_unix = kNow;
        CHECK(in.evidence.kind == Kind::GuestFault, "variant %d: guest_fault expected (got %s)", variant, kind_name(in.evidence.kind));
        Outbox ob(*io, sc.root + "/outbox");
        ReportRecord rec;
        std::string err;
        const size_t before = g_heap_cur;
        g_heap_peak = g_heap_cur;
        const bool created = ob.create(in, pats, &rec, &err);
        const size_t peak = g_heap_peak - before;
        CHECK(created, "variant %d: create failed: %s", variant, err.c_str());
        const std::string bp = get_file(sc.root + "/outbox/" + in.report_id + "/boot_progress.txt");
        CHECK(!bp.empty() && sealed_size(bp.size()) <= kSealedCapBootProgress, "variant %d: boot_progress copy of %zu bytes", variant, bp.size());
        const size_t at = bp.find(kFault);
        CHECK(at != std::string::npos, "variant %d: fault line missing from the copy", variant);
        if (variant == 1 && at != std::string::npos) {
            // The window is centred on the fault line: ~24 KiB of the long
            // lines on each side.
            CHECK(all_bytes(bp, at - 12 - 20000, 20000, 'y'), "context before the fault missing");
            CHECK(all_bytes(bp, at + std::strlen(kFault) + 1 + 11, 20000, 'y'), "context after the fault missing");
        }
        if (kAllocCounter) {
            std::printf("    Outbox::create peak heap: %zu bytes for a %" PRIu64 "-byte boot_progress (variant %d)\n", peak, log_bytes, variant);
            CHECK(peak < 1024u * 1024u, "variant %d: Outbox::create peak heap %zu >= 1 MiB (spec 4.9)", variant, peak);
        }
    }
}

// The fault window may start in the middle of a line. A secret crossing its
// first byte must still be redacted: here the fault line ends where the
// kept tail begins, so the window is the `budget` bytes ending there, and a
// dashed UTF-16LE key (62 bytes) starts 58 bytes before that window.
static void t_outbox_window_cut_redaction() {
    auto io = make_posix_io();
    Scenario sc = make_scenario("ob_window_cut");
    const std::string fault = "[ 100.00s] NATIVE FAULT thr 3 eip=0192f040 addr=0192f040\n";
    std::string pre;
    char line[128];
    for (int i = 0; pre.size() < 700 * 1024; ++i) {
        std::snprintf(line, sizeof line, "[%4d.00s] filler line %06d before the fault window test\n", i / 100, i);
        pre += line;
    }
    std::string mid;
    while (mid.size() < 60 * 1024) mid += std::string(999, 'a') + "\n";
    std::string tail;
    for (int i = 0; tail.size() < kProgressTailBytes; ++i) {
        std::snprintf(line, sizeof line, "[%4d.00s] tail line %06d\n", 100 + i / 100, i);
        tail += line;
    }
    tail.resize(kProgressTailBytes);
    const uint64_t size = pre.size() + mid.size() + fault.size() + tail.size();
    // Budget of Outbox::create: cap - head - tail - two "\n=== d2cr: N bytes omitted ===\n" markers.
    const uint64_t marker = 30 + std::to_string((unsigned long long)size).size();
    const uint64_t budget = max_plain_for_sealed(kSealedCapBootProgress) - kProgressHeadBytes - kProgressTailBytes - 2 * marker;
    const uint64_t fault_off = pre.size() + mid.size();
    const uint64_t win_lo = fault_off + fault.size() - budget;
    const std::string key = utf16le("f-a-k-e-1-2-3-4-c-l-a-s-5-6-7-8");
    const uint64_t key_at = win_lo - 58;
    CHECK(key.size() == 62 && key_at > pre.size(), "setup");
    mid.replace((size_t)(key_at - pre.size()), key.size(), key);
    put_file(sc.progress, pre + mid + fault + tail);

    const SecretPatterns pats = fake_patterns();
    CollectInputs in;
    in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYC2";
    in.session = sc.s;
    in.evidence = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &in.details);
    in.now_unix = kNow;
    Outbox ob(*io, sc.root + "/outbox");
    ReportRecord rec;
    std::string err;
    CHECK(ob.create(in, pats, &rec, &err), "create failed: %s", err.c_str());
    const std::string bp = get_file(sc.root + "/outbox/" + in.report_id + "/boot_progress.txt");
    // The window starts right after the first omission marker, with the
    // last two code units of the key: redacted, not "7\0" "8\0".
    const std::string mark = "\n=== d2cr: " + std::to_string((unsigned long long)(win_lo - kProgressHeadBytes)) + " bytes omitted ===\n";
    const size_t at = bp.find(mark);
    CHECK(at == kProgressHeadBytes, "window marker at %zu, want %zu (bp %zu bytes)", at, (size_t)kProgressHeadBytes, bp.size());
    if (at != std::string::npos)
        CHECK(bp.compare(at + mark.size(), 4, std::string("X\0X\0", 4)) == 0, "key bytes at the window start left in clear");
    CHECK(bp.find(fault) != std::string::npos, "fault line missing");
}

static void make_report(IoApi& io, Outbox& ob, const std::string& root, const std::string& id, int64_t created, size_t bytes,
                        int prompts, const char* consent, int attempts = 0) {
    const std::string cmd = "mkdir -p '" + root + "/" + id + "'";
    if (std::system(cmd.c_str()) != 0) std::exit(2);
    ReportRecord r;
    r.report_id = id; r.session_id = "s"; r.kind = Kind::Hang; r.features_json = "{}"; r.dump = "none";
    ArtifactFile a; a.name = "boot_progress"; a.file = "boot_progress.txt"; a.bytes = bytes; r.artifacts.push_back(a);
    io.write_file(root + "/" + id + "/evidence.txt", serialize_report_record(r));
    io.write_file(root + "/" + id + "/boot_progress.txt", std::string(bytes, 'x'));
    ReportState st; st.created_unix = created; st.prompts = prompts; st.consent = consent; st.attempts = attempts;
    CHECK(ob.save_state(id, st), "save_state %s", id.c_str());
}

static void t_outbox_enforce_bounds() {
    auto io = make_posix_io();
    const std::string root = test_dir("ob_enforce");
    Outbox ob(*io, root);
    const int64_t day = 86400;
    make_report(*io, ob, root, "A_expired", kNow - 8 * day, 100, 0, "pending");
    make_report(*io, ob, root, "B_prompted", kNow - 100, 100, 3, "pending");
    make_report(*io, ob, root, "C_denied", kNow - 90, 100, 0, "denied");
    make_report(*io, ob, root, "C_failed", kNow - 85, 100, 0, "granted", 3);     // spec 4.8: dropped after 3 failures
    const std::string cmd = "mkdir -p '" + root + "/D_invalid'";
    if (std::system(cmd.c_str()) != 0) std::exit(2);                       // no state.txt
    make_report(*io, ob, root, "E1", kNow - 80, 100, 0, "pending");
    // Sent at the third dialog, first upload failed: the prompt bound no
    // longer applies once the player answered (spec 4.8: kept until 3
    // failures or 7 days).
    make_report(*io, ob, root, "E2", kNow - 70, 100, 3, "granted", 1);
    make_report(*io, ob, root, "E3", kNow - 60, 100, 0, "pending");
    CHECK_U64(ob.enforce(kNow), 5);
    std::vector<std::string> ids = ob.list();
    CHECK(ids.size() == 3 && ids[0] == "E1" && ids[1] == "E2" && ids[2] == "E3", "survivors, oldest first");
    CHECK(!exists(root + "/A_expired") && !exists(root + "/D_invalid"), "removed directories still there");

    // More than 5 reports: the oldest go.
    make_report(*io, ob, root, "E4", kNow - 50, 100, 0, "pending");
    make_report(*io, ob, root, "E5", kNow - 40, 100, 0, "pending");
    make_report(*io, ob, root, "E6", kNow - 30, 100, 0, "pending");
    CHECK_U64(ob.enforce(kNow), 1);
    ids = ob.list();
    CHECK(ids.size() == 5 && ids[0] == "E2", "count bound (first=%s)", ids.empty() ? "" : ids[0].c_str());

    // A 3 MiB report still fits (E2 goes for the count only).
    make_report(*io, ob, root, "F_big", kNow - 20, 3 * 1024 * 1024, 0, "pending");
    CHECK_U64(ob.enforce(kNow), 1);
    ids = ob.list();
    CHECK(ids.size() == 5 && ids[0] == "E3" && ids.back() == "F_big", "after F_big");
    // Another 2 MiB: E3 for the count, then E4, E5, E6 and F_big for the
    // 4 MiB bound, oldest first, until only the newest is left.
    make_report(*io, ob, root, "G_big", kNow - 10, 2 * 1024 * 1024, 0, "pending");
    CHECK_U64(ob.enforce(kNow), 5);
    ids = ob.list();
    uint64_t total = 0;
    for (const std::string& id : ids) total += ob.report_bytes(id);
    CHECK(ids.size() == 1 && ids[0] == "G_big", "size bound survivors (%zu)", ids.size());
    CHECK(total <= 4u * 1024 * 1024, "size bound: %" PRIu64 " bytes", total);
}

// save_state writes state.txt.new, removes state.txt, then renames: a stop
// between the last two steps leaves only state.txt.new, and the report must
// survive it.
static void t_outbox_state_crash_window() {
    auto io = make_posix_io();
    const std::string root = test_dir("ob_state_window");
    Outbox ob(*io, root);
    make_report(*io, ob, root, "R1", kNow - 50, 100, 1, "granted", 2);
    const std::string st_path = root + "/R1/state.txt";
    CHECK(::rename(st_path.c_str(), (st_path + ".new").c_str()) == 0, "setup rename");
    std::vector<std::string> ids = ob.list();
    CHECK(ids.size() == 1 && ids[0] == "R1", "report with only state.txt.new not listed (%zu)", ids.size());
    ReportRecord rec;
    ReportState st;
    CHECK(ob.load("R1", &rec, &st) && st.consent == "granted" && st.attempts == 2 && st.prompts == 1,
          "state.txt.new not loaded");
    CHECK_U64(ob.enforce(kNow), 0);
    CHECK(exists(st_path) && !exists(st_path + ".new"), "enforce must put state.txt back");
    CHECK(ob.load("R1", &rec, &st) && st.attempts == 2, "state after repair");
    // Stopped before the old state.txt was removed: both files exist, the
    // old one wins, and the next save goes through.
    ReportState newer = st;
    newer.attempts = 3;
    io->write_file(st_path + ".new", serialize_report_state(newer));
    CHECK(ob.load("R1", &rec, &st) && st.attempts == 2, "state.txt must win over a leftover state.txt.new");
    newer.attempts = 1;
    CHECK(ob.save_state("R1", newer), "save_state over a leftover");
    CHECK(ob.load("R1", &rec, &st) && st.attempts == 1 && !exists(st_path + ".new"), "saved state");
}

// ---------------------------------------------------------------- cr_claim

static void t_claim_ulid() {
    uint8_t r[10];
    for (int i = 0; i < 10; ++i) r[i] = (uint8_t)i;
    CHECK_STR(new_ulid(1789284000000LL, r), "01M2CT6880000G40R40M30E209");
    const uint8_t r2[10] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
    CHECK_STR(new_ulid(1789284000123LL, r2), "01M2CT68BVVTPVXVR14D2PF2DB");
    const uint8_t z[10] = {0};
    CHECK_STR(new_ulid(0, z), "00000000000000000000000000");
    uint8_t f[10];
    for (int i = 0; i < 10; ++i) f[i] = 0xff;
    CHECK_STR(new_ulid((1LL << 48) - 1, f), "7ZZZZZZZZZZZZZZZZZZZZZZZZZ");
    CHECK_STR(new_ulid(1LL << 48, f), "");
    CHECK_STR(new_ulid(-1, f), "");
    // Later milliseconds sort after earlier ones.
    CHECK(new_ulid(1789284000124LL, z) > new_ulid(1789284000123LL, f), "ULIDs must sort by time");
}

static ClaimInputs base_claim(Kind kind, const char* features) {
    ClaimInputs in;
    in.report_id = "01M2CT6880000G40R40M30E209";
    in.install_id = "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e";
    in.build_id = "0.1.0+ab12cd34ef56";
    in.channel = "release";
    in.platform_model = "vita";
    in.platform_fw = "3.65";
    in.started_unix = 1789284000;
    in.uptime_s = 967;
    in.online = false;
    in.kind = kind;
    in.features_json = features;
    return in;
}

static const char* kClaimHead =
    "{\"v\":1,\"report_id\":\"01M2CT6880000G40R40M30E209\",\"install_id\":\"4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e\","
    "\"build_id\":\"0.1.0+ab12cd34ef56\",";

static void t_claim_json_each_kind() {
    {   // halt: the example of spec §4.4, artifacts as sealed sizes.
        ClaimInputs in = base_claim(Kind::Halt, "{\"code\":1420,\"location\":null,\"frames\":[\"Game+0x1fedf4\",\"Game+0x451c23\",\"Game+0x44f570\"]}");
        in.hints.push_back(Kind::GuestFault);
        in.artifacts.push_back(ClaimArtifact{"crash_txt", sealed_size(2210)});
        in.artifacts.push_back(ClaimArtifact{"crash_log", sealed_size(3104)});
        in.artifacts.push_back(ClaimArtifact{"boot_progress", sealed_size(262144)});
        in.redactions = 0;
        CHECK_STR(build_claim_json(in), std::string(kClaimHead) +
            "\"channel\":\"release\",\"platform\":{\"model\":\"vita\",\"fw\":\"3.65\"},"
            "\"session\":{\"started_unix\":1789284000,\"uptime_s\":967,\"online\":false},\"kind\":\"halt\","
            "\"features\":{\"code\":1420,\"location\":null,\"frames\":[\"Game+0x1fedf4\",\"Game+0x451c23\",\"Game+0x44f570\"]},"
            "\"hints\":[\"guest_fault\"],\"artifacts\":[{\"name\":\"crash_txt\",\"bytes\":2298},{\"name\":\"crash_log\",\"bytes\":3192},"
            "{\"name\":\"boot_progress\",\"bytes\":262296}],\"redactions\":0}");
    }
    {   // guest_fault: unknown uptime and platform, online, no redaction count.
        ClaimInputs in = base_claim(Kind::GuestFault,
            "{\"exception\":null,\"thread\":\"worker\",\"eip\":\"Game+0x2f040\",\"frames\":[\"Game+0x51c23\"]}");
        in.platform_model = "unknown";
        in.platform_fw = "unknown";
        in.uptime_s = -1;
        in.online = true;
        in.hints.push_back(Kind::Hang);
        in.artifacts.push_back(ClaimArtifact{"boot_progress", 9742});
        CHECK_STR(build_claim_json(in), std::string(kClaimHead) +
            "\"channel\":\"release\",\"platform\":{\"model\":\"unknown\",\"fw\":\"unknown\"},"
            "\"session\":{\"started_unix\":1789284000,\"uptime_s\":null,\"online\":true},\"kind\":\"guest_fault\","
            "\"features\":{\"exception\":null,\"thread\":\"worker\",\"eip\":\"Game+0x2f040\",\"frames\":[\"Game+0x51c23\"]},"
            "\"hints\":[\"hang\"],\"artifacts\":[{\"name\":\"boot_progress\",\"bytes\":9742}]}");
    }
    {   // host_fault.
        ClaimInputs in = base_claim(Kind::HostFault,
            "{\"stop_reason\":\"0x30004\",\"thread_name\":\"DTWO00001\",\"pc\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24184\"},"
            "\"lr\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24101\"},\"guest_frames\":[\"Game+0x51c23\"],\"redaction\":\"clean\"}");
        in.channel = "dev";
        in.hints.push_back(Kind::Halt);
        in.hints.push_back(Kind::AbnormalExit);
        in.artifacts.push_back(ClaimArtifact{"dump", sealed_size(1325904)});
        in.redactions = 2;
        CHECK_STR(build_claim_json(in), std::string(kClaimHead) +
            "\"channel\":\"dev\",\"platform\":{\"model\":\"vita\",\"fw\":\"3.65\"},"
            "\"session\":{\"started_unix\":1789284000,\"uptime_s\":967,\"online\":false},\"kind\":\"host_fault\","
            "\"features\":{\"stop_reason\":\"0x30004\",\"thread_name\":\"DTWO00001\",\"pc\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24184\"},"
            "\"lr\":{\"region\":\"jit\",\"module\":\"jit\",\"offset\":\"0x24101\"},\"guest_frames\":[\"Game+0x51c23\"],\"redaction\":\"clean\"},"
            "\"hints\":[\"halt\",\"abnormal_exit\"],\"artifacts\":[{\"name\":\"dump\",\"bytes\":1326312}],\"redactions\":2}");
    }
    {   // abnormal_exit.
        ClaimInputs in = base_claim(Kind::AbnormalExit,
            "{\"reason\":\"exit_process\",\"code\":4294967295,\"import\":null,\"frames\":[\"Game+0x97be\"]}");
        in.hints.push_back(Kind::GuestFault);
        in.hints.push_back(Kind::Hang);
        const std::string j = build_claim_json(in);
        CHECK(j.find("\"kind\":\"abnormal_exit\",\"features\":{\"reason\":\"exit_process\",\"code\":4294967295,\"import\":null,"
                     "\"frames\":[\"Game+0x97be\"]},\"hints\":[\"guest_fault\",\"hang\"],\"artifacts\":[]}") != std::string::npos,
              "%s", j.c_str());
    }
    {   // hang.
        ClaimInputs in = base_claim(Kind::Hang, "{\"stalled_beats\":4,\"eip\":\"Game+0xfa60c\",\"runner_state\":\"starvation\"}");
        in.channel = "test";
        const std::string j = build_claim_json(in);
        CHECK(j.find("\"channel\":\"test\"") != std::string::npos &&
              j.find("\"kind\":\"hang\",\"features\":{\"stalled_beats\":4,\"eip\":\"Game+0xfa60c\",\"runner_state\":\"starvation\"},"
                     "\"hints\":[],\"artifacts\":[]}") != std::string::npos, "%s", j.c_str());
    }
}

static void t_claim_sanitize_and_bounds() {
    ClaimInputs in = base_claim(Kind::AbnormalExit, "{}");
    in.hints.push_back(Kind::HostFault);     // more severe: dropped
    in.hints.push_back(Kind::AbnormalExit);  // same kind: dropped
    in.hints.push_back(Kind::Hang);
    in.hints.push_back(Kind::Hang);          // duplicate: dropped
    in.hints.push_back(Kind::None);          // not a kind: dropped
    in.artifacts.push_back(ClaimArtifact{"bogus", 100});
    in.artifacts.push_back(ClaimArtifact{"crash_log", 500});
    in.artifacts.push_back(ClaimArtifact{"crash_log", 600});
    in.platform_model = "ps4";
    in.platform_fw = "3.6";
    const std::string j = build_claim_json(in);
    CHECK(j.find("\"platform\":{\"model\":\"unknown\",\"fw\":\"unknown\"}") != std::string::npos, "%s", j.c_str());
    CHECK(j.find("\"hints\":[\"hang\"],\"artifacts\":[{\"name\":\"crash_log\",\"bytes\":500}]}") != std::string::npos, "%s", j.c_str());
    in.platform_fw = "10.00";
    CHECK(build_claim_json(in).find("\"fw\":\"10.00\"") != std::string::npos, "two-digit firmware major");
    CHECK_STR(build_claim_json(base_claim(Kind::None, "{}")), "");
    ClaimInputs big = base_claim(Kind::Hang, "{}");
    big.features_json = "{\"x\":\"" + std::string(16 * 1024, 'a') + "\"}";
    CHECK_STR(build_claim_json(big), "");
    // A realistic worst case stays far below the transport limit.
    std::string frames;
    for (int i = 0; i < 16; ++i) frames += std::string(i ? "," : "") + "\"abcdefghijklmnopqrstuvwxyz012345+0xffffffff\"";
    ClaimInputs worst = base_claim(Kind::Halt, ("{\"code\":4294967295,\"location\":\"" + std::string(64, 'f') + ":4294967295\",\"frames\":[" + frames + "]}").c_str());
    for (Kind k : {Kind::AbnormalExit, Kind::GuestFault, Kind::Hang}) worst.hints.push_back(k);
    for (const char* n : {"dump", "crash_txt", "crash_log", "boot_progress"}) worst.artifacts.push_back(ClaimArtifact{n, 2097152});
    worst.redactions = 1000000;
    const std::string w = build_claim_json(worst);
    CHECK(!w.empty() && w.size() < 2048, "worst case claim is %zu bytes", w.size());
}

static void t_claim_from_record() {
    ReportRecord r;
    r.report_id = "01M2CT6880000G40R40M30E209"; r.session_id = "s"; r.build_id = "0.1.0+ab12cd34ef56";
    r.kind = Kind::Halt; r.hints.push_back(Kind::Hang);
    r.features_json = "{\"code\":1420,\"location\":null,\"frames\":[]}";
    r.started_unix = 1789284000; r.uptime_s = 967; r.online = true; r.fw = ""; r.redactions = 3; r.dump = "none";
    r.artifacts.push_back(ArtifactFile{"crash_txt", "crash_txt.txt", 2210});
    r.artifacts.push_back(ArtifactFile{"boot_progress", "boot_progress.txt", 262144});
    const ClaimInputs in = claim_inputs_from_record(r, "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e", "release", "vita");
    CHECK(in.report_id == r.report_id && in.build_id == r.build_id && in.kind == Kind::Halt && in.hints.size() == 1 &&
          in.features_json == r.features_json && in.started_unix == 1789284000 && in.uptime_s == 967 && in.online &&
          in.platform_fw == "unknown" && in.platform_model == "vita" && in.redactions == 3, "record fields");
    CHECK(in.artifacts.size() == 2 && in.artifacts[0].name == "crash_txt" && in.artifacts[0].bytes == sealed_size(2210) &&
          in.artifacts[1].bytes == sealed_size(262144), "sealed artifact sizes");
    CHECK(!build_claim_json(in).empty(), "claim from record");
}

// ---------------------------------------------------------------- claims end to end
// Address rules of contract claim.v1 (D2Vita-website contract/, commit
// f5b3847 "give every module one spelling in addresses"), copied so that
// every run checks them. run_crashreport_tests.sh validates the same claims
// against the schema itself when D2V_CONTRACT names a contract directory.

static std::string g_claims_out;

// Hex32: ^0x(0|[1-9a-f][0-9a-f]{0,7})$
static bool contract_hex32(const std::string& s) {
    if (s.size() < 3 || s.size() > 10 || s.compare(0, 2, "0x") != 0) return false;
    if (s[2] == '0') return s.size() == 3;
    for (size_t i = 2; i < s.size(); ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

// ModuleName: ^[A-Za-z0-9_.]{1,32}$
static bool contract_module_name(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) return false;
    return true;
}

// Address: ^(Game|ABS|(?!(game|abs)\+)[a-z0-9_]{1,32})\+0x(0|[1-9a-f][0-9a-f]{0,7})$
static bool contract_address(const std::string& s) {
    const size_t plus = s.find('+');
    if (plus == std::string::npos) return false;
    const std::string module = s.substr(0, plus);
    bool ok = module == "Game" || module == "ABS";
    if (!ok && !module.empty() && module.size() <= 32 && module != "game" && module != "abs") {
        ok = true;
        for (char c : module) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) ok = false;
    }
    return ok && contract_hex32(s.substr(plus + 1));
}

// Every string holding "+0x" must be an Address, and every
// {"region","module","offset"} object a HostAddress.
static std::string contract_address_problems(const std::string& claim) {
    std::string problems;
    std::vector<std::string> strs;
    for (size_t i = 0; i < claim.size(); ++i) {
        if (claim[i] != '"') continue;
        std::string s;
        size_t j = i + 1;
        for (; j < claim.size() && claim[j] != '"'; ++j) {
            if (claim[j] == '\\' && j + 1 < claim.size()) ++j;
            s += claim[j];
        }
        strs.push_back(s);
        i = j;
    }
    for (size_t k = 0; k < strs.size(); ++k) {
        if (strs[k].find("+0x") != std::string::npos && !contract_address(strs[k])) problems += " address " + strs[k];
        if (strs[k] == "region" && k + 5 < strs.size() && strs[k + 2] == "module" && strs[k + 4] == "offset") {
            const std::string& region = strs[k + 1];
            const std::string& module = strs[k + 3];
            bool ok = contract_hex32(strs[k + 5]);
            if (region == "eboot" || region == "jit" || region == "unknown") ok = ok && module == region;
            else if (region == "sysmodule") ok = ok && contract_module_name(module);
            else ok = false;
            if (!ok) problems += " host " + region + "/" + module + "+" + strs[k + 5];
        }
    }
    return problems;
}

// evidence -> outbox -> record -> claim, chained as the console will.
static std::string e2e_claim(const char* name, const Scenario& sc, const SecretPatterns& pats, Kind want) {
    auto io = make_posix_io();
    static uint8_t seq = 0;
    const uint8_t rnd[10] = {0x5e, 0x55, 0x10, 0x4e, 0, 0, 0, 0, 0, ++seq};
    CollectInputs in;
    in.report_id = new_ulid(kNow * 1000LL, rnd);
    in.session = sc.s;
    in.evidence = build_evidence(*io, sc.s, kNow, sc.data_dir, pats, &in.details);
    in.now_unix = kNow;
    CHECK(in.evidence.kind == want, "%s: %s expected, got %s", name, kind_name(want), kind_name(in.evidence.kind));
    Outbox ob(*io, sc.root + "/outbox");
    ReportRecord rec;
    ReportState st;
    std::string err;
    CHECK(ob.create(in, pats, &rec, &err), "%s: create failed: %s", name, err.c_str());
    CHECK(ob.load(in.report_id, &rec, &st), "%s: load failed", name);
    const std::string claim =
        build_claim_json(claim_inputs_from_record(rec, "4f3c9a0e8b7d6c5a4f3e2d1c0b9a8f7e", "test", "vita"));
    CHECK(!claim.empty(), "%s: no claim", name);
    const std::string problems = contract_address_problems(claim);
    CHECK(problems.empty(), "%s: contract address rules broken:%s in %s", name, problems.c_str(), claim.c_str());
    if (!g_claims_out.empty()) put_file(g_claims_out + "/" + name + ".json", claim);
    return claim;
}

static void t_claims_end_to_end() {
    if (!g_claims_out.empty()) {
        const std::string cmd = "mkdir -p '" + g_claims_out + "'";
        if (std::system(cmd.c_str()) != 0) std::exit(2);
    }
    const SecretPatterns none;
    auto crash_txt = [](const Scenario& sc, const std::string& text, int64_t mtime) {
        put_file(sc.s.write_root + "/Crash.txt", text);
        set_mtime(sc.s.write_root + "/Crash.txt", mtime);
    };
    {
        // Stalled heartbeats alongside a real Halt used to add a "hang" hint
        // (kept below Halt, the more severe kind) before automatic hang
        // detection was removed; the exact same fixture now yields no hint
        // at all — this guards against the removed branch resurfacing here
        // even though it can no longer become the PRIMARY kind (see
        // t_evidence_none_and_hang for that).
        Scenario sc = make_scenario("e2e_halt_1420");
        crash_txt(sc, fixture("crash_txt_halt1420.txt"), kStarted + 600);
        put_file(sc.progress, fixture("progress_hang_real.txt"));
        const std::string c = e2e_claim("halt_1420_no_hang_hint", sc, none, Kind::Halt);
        CHECK(c.find("\"hints\":[]") != std::string::npos, "%s", c.c_str());
    }
    {
        Scenario sc = make_scenario("e2e_halt_unflushed");
        crash_txt(sc, "", kStarted);
        e2e_claim("halt_unflushed_crash_txt", sc, none, Kind::Halt);
    }
    {
        Scenario sc = make_scenario("e2e_halt_904");
        crash_txt(sc, fixture("crash_txt_halt904_location.txt"), kStarted + 1);
        put_file(sc.s.write_root + "/crash.log", "=== D2Vita boot (build Sep 14 2026 09:54:00) ===\n");
        e2e_claim("halt_904_location", sc, none, Kind::Halt);
    }
    {
        Scenario sc = make_scenario("e2e_guest_worker");
        put_file(sc.progress, fixture("progress_native_fault_block.txt"));
        const std::string c = e2e_claim("guest_fault_worker_abs_frame", sc, none, Kind::GuestFault);
        CHECK(c.find("\"ABS+0x3012345\"") != std::string::npos, "%s", c.c_str());
    }
    {
        Scenario sc = make_scenario("e2e_guest_main");
        put_file(sc.progress, fixture("progress_native_fault_real.txt"));
        e2e_claim("guest_fault_main_without_block", sc, none, Kind::GuestFault);
    }
    {
        Scenario sc = make_scenario("e2e_host_jit");
        put_dump(sc, "first_jit.psp2dmp", kStarted + 900);
        crash_txt(sc, fixture("crash_txt_halt1420.txt"), kStarted + 890);
        const std::string c = e2e_claim("host_fault_jit_dump_offered", sc, none, Kind::HostFault);
        CHECK(c.find("{\"name\":\"dump\"") != std::string::npos, "%s", c.c_str());
    }
    {
        Scenario sc = make_scenario("e2e_host_eboot");
        put_dump(sc, "eboot_nostamp.psp2dmp", kStarted + 100);
        e2e_claim("host_fault_eboot", sc, none, Kind::HostFault);
    }
    {
        Scenario sc = make_scenario("e2e_host_sysmodule");
        put_dump(sc, "d2_sysmodule.psp2dmp", kStarted + 200);
        e2e_claim("host_fault_sysmodule_odd_lr", sc, none, Kind::HostFault);
    }
    {
        Scenario sc = make_scenario("e2e_host_unknown");
        sc.s.jit_lo = sc.s.jit_hi = 0;
        put_dump(sc, "first_jit.psp2dmp", kStarted + 900);
        e2e_claim("host_fault_unknown", sc, none, Kind::HostFault);
    }
    {
        Scenario sc = make_scenario("e2e_host_withheld");
        put_dump(sc, "worker_withheld.psp2dmp", kStarted + 50);
        put_file(sc.progress, fixture("progress_native_fault_block.txt"));
        const std::string c = e2e_claim("host_fault_withheld", sc, fake_patterns(), Kind::HostFault);
        CHECK(c.find("{\"name\":\"dump\"") == std::string::npos, "withheld dump offered: %s", c.c_str());
    }
    {
        Scenario sc = make_scenario("e2e_exit_process");
        put_file(sc.progress, fixture("progress_abnormal_exit.txt"));
        sc.s.state = "exited";
        e2e_claim("abnormal_exit_process", sc, none, Kind::AbnormalExit);
    }
    {
        Scenario sc = make_scenario("e2e_exit_unshimmed");
        sc.s.state = "exited";
        sc.s.stop_reason = "UNSHIMMED KERNEL32.dll!GetNumaHighestNodeNumber";
        e2e_claim("abnormal_exit_unshimmed", sc, none, Kind::AbnormalExit);
    }
    {
        Scenario sc = make_scenario("e2e_exit_main_fault");
        sc.s.state = "exited";
        sc.s.stop_reason = "box86 dynarec fault (unimplemented/illegal/div0)";
        sc.s.has_main_exit = true;
        sc.s.main_exit = 0xc0000005u;
        e2e_claim("abnormal_exit_main_thread_fault", sc, none, Kind::AbnormalExit);
    }
    // Two "e2e_hang_*" scenarios used to live here (the exact
    // progress_hang_real.txt and synthetic-ABS-eip fixtures, asserting
    // Kind::Hang end to end through Outbox::create + claim JSON). Now that
    // automatic hang detection has been removed, both fixtures produce
    // Kind::None instead, and e2e_claim's own "create must succeed"
    // assumption no longer holds for them (Outbox::create correctly refuses
    // a Kind::None evidence — "no evidence to report" — so there is no claim
    // left to build here). The no-evidence assertion for these exact
    // fixtures now lives in t_evidence_none_and_hang instead.
    // The checker itself refuses the old spellings.
    CHECK(!contract_address_problems("{\"eip\":\"abs+0x10e01b30\"}").empty(), "abs+ accepted");
    CHECK(!contract_address_problems("{\"eip\":\"Game.exe+0x1\"}").empty(), "Game.exe+ accepted");
    CHECK(!contract_address_problems("{\"pc\":{\"region\":\"sysmodule\",\"module\":\"Sce Odd\",\"offset\":\"0x1\"}}").empty(),
          "bad module name accepted");
    CHECK(!contract_address_problems("{\"pc\":{\"region\":\"unknown\",\"module\":\"x\",\"offset\":\"0x1\"}}").empty(),
          "unknown region with a module accepted");
    CHECK(contract_address_problems("{\"eip\":\"ABS+0x10e01b30\",\"f\":[\"glide3x+0x1a2c\"]}").empty(), "valid spellings refused");
    // The address examples table of signature-rules.v1.md §7.
    for (const char* v : {"Game+0x1fedf4", "glide3x+0x1a2c", "checkrevision+0x0", "ABS+0x2a4c1000", "games+0x1"})
        CHECK(contract_address(v), "valid address %s refused", v);
    for (const char* v : {"Game.exe+0x1fedf4", "game+0x1fedf4", "GAME+0x1fedf4", "Glide3x+0x1a2c", "glide3x.dll+0x1a2c",
                          "abs+0x2a4c1000", "Game+0x01fedf4", "Game+0x1FEDF4", "Game+0x", "+0x1", "Game+0x123456789"})
        CHECK(!contract_address(v), "invalid address %s accepted", v);
}

// --dump-facts: one dump, key=value lines (used by crashreport_oracle.py).
static int dump_facts_main(int argc, char** argv) {
    std::string path;
    SessionRecord s;
    s.session_id = "oracle";
    s.game_base = 0x01900000;
    s.arena_host_base = 0x84000000;
    DumpReadOptions opt;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-facts") && i + 1 < argc) path = argv[++i];
        else if (!std::strcmp(argv[i], "--first-thread")) opt.rule = ThreadRule::First;
        else if (!std::strcmp(argv[i], "--game-base") && i + 1 < argc) s.game_base = (uint32_t)std::strtoul(argv[++i], nullptr, 16);
        else if (!std::strcmp(argv[i], "--host-base") && i + 1 < argc) s.arena_host_base = (uint32_t)std::strtoul(argv[++i], nullptr, 16);
    }
    auto io = make_posix_io();
    g_heap_peak = g_heap_cur;
    const size_t before = g_heap_cur;
    const DumpFacts f = read_psp2dmp(*io, path, s, SecretPatterns(), opt);
    const size_t peak = g_heap_peak - before;
    DumpReadOptions stop_rule;
    const DumpFacts g = read_psp2dmp(*io, path, s, SecretPatterns(), stop_rule);
    std::printf("ok=%d\nerror=%s\ntruncated=%d\napp_title=%s\ntid=0x%x\npc=%08x\nlr=%08x\nsp=%08x\ncpsr=%08x\n",
                f.ok ? 1 : 0, f.error.c_str(), f.truncated ? 1 : 0, f.app_title.c_str(), (unsigned)f.tid,
                (unsigned)f.pc, (unsigned)f.lr, (unsigned)f.sp, (unsigned)f.cpsr);
    std::printf("chain=%s\nchain_end=%s\n", chain_str(f, s.game_base).c_str(), f.chain_end.c_str());
    std::printf("stop_rule_tid=0x%x\nstop_rule_thread=%s\nstop_reason=%s\n", (unsigned)g.tid, g.thread_name.c_str(),
                stop_reason_name(g.stop_reason).c_str());
    std::printf("decompressed=%" PRIu64 "\npeak_heap=%s\n", f.decompressed_bytes,
                kAllocCounter ? std::to_string(peak).c_str() : "n/a");
    return f.ok ? 0 : 1;
}

// --collect-peak LOG --work DIR: heap peaks of build_evidence and
// Outbox::create on a real boot_progress, read in place (no dump directory,
// an empty write root, the outbox under DIR). Exit 1 over 1 MiB.
static int collect_peak_main(int argc, char** argv) {
    std::string log, work;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--collect-peak") && i + 1 < argc) log = argv[++i];
        else if (!std::strcmp(argv[i], "--work") && i + 1 < argc) work = argv[++i];
    }
    if (log.empty() || work.empty()) { std::printf("usage: --collect-peak LOG --work DIR\n"); return 2; }
    const std::string cmd = "mkdir -p '" + work + "/save'";
    if (std::system(cmd.c_str()) != 0) return 2;
    auto io = make_posix_io();
    SessionRecord s;
    s.session_id = "0123456789abcdef0123456789abcdef";
    s.build_id = "0.1.0+ab12cd34ef56";
    s.started_unix = kStarted;
    s.write_root = work + "/save";
    s.progress_path = log;
    s.state = "running";
    s.game_base = 0x01900000;
    const SecretPatterns pats = fake_patterns();
    CollectInputs in;
    size_t before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    in.evidence = build_evidence(*io, s, kNow, "", pats, &in.details);
    const size_t evidence_peak = g_heap_peak - before;
    in.report_id = "01J9Z6T4Q8M3K7V2B5N0XWAYC9";
    in.session = s;
    in.now_unix = kNow;
    std::string outcome = kind_name(in.evidence.kind);
    if (in.evidence.kind == Kind::None) {
        // Measure the copy anyway, as if the log held evidence.
        in.evidence.kind = Kind::Hang;
        in.evidence.features_json = "{}";
        in.evidence.progress_path = log;
        outcome = "none (report forced for the measurement)";
    }
    Outbox ob(*io, work + "/outbox");
    ReportRecord rec;
    std::string err;
    before = g_heap_cur;
    g_heap_peak = g_heap_cur;
    const bool created = ob.create(in, pats, &rec, &err);
    const size_t outbox_peak = g_heap_peak - before;
    if (!created) {
        std::printf("   FAIL: Outbox::create: %s\n", err.c_str());
        return 1;
    }
    DirEntry de;
    io->stat(log, &de);
    std::printf("   %s: %" PRIu64 " bytes, evidence kind %s, boot_progress copy of %" PRIu64 " bytes\n", log.c_str(), de.size,
                outcome.c_str(), rec.artifacts.empty() ? (uint64_t)0 : rec.artifacts[0].bytes);
    if (!kAllocCounter) return 0;
    std::printf("   build_evidence peak heap %zu bytes, Outbox::create peak heap %zu bytes\n", evidence_peak, outbox_peak);
    const bool over = evidence_peak >= 1024u * 1024u || outbox_peak >= 1024u * 1024u;
    std::printf("   %s\n", over ? "FAIL: over the 1 MiB budget of spec 4.9" : "OK: under 1 MiB");
    return over ? 1 : 0;
}

// ---------------------------------------------------------------- driver

struct TestCase { const char* name; void (*fn)(); };
static const TestCase kTests[] = {
    {"kind_name", t_kind_name},
    {"format_addr", t_format_addr},
    {"parse_session", t_parse_session},
    {"serialize_session_roundtrip", t_serialize_session_roundtrip},
    {"json_writer_ordered", t_json_writer_ordered},
    {"json_escaping", t_json_escaping},
    {"posix_io_files", t_posix_io_files},
    {"posix_io_head_tail", t_posix_io_head_tail},
    {"posix_io_dirs", t_posix_io_dirs},
    {"posix_io_stream", t_posix_io_stream},
    {"guest_addr", t_guest_addr},
    {"progress_native_fault_real", t_progress_native_fault_real},
    {"progress_native_fault_block", t_progress_native_fault_block},
    {"progress_clean_exit", t_progress_clean_exit},
    {"progress_abnormal_exit", t_progress_abnormal_exit},
    {"progress_hang", t_progress_hang},
    {"progress_no_evidence", t_progress_no_evidence},
    {"progress_stop_lines_inline", t_progress_stop_lines_inline},
    {"progress_stream", t_progress_stream},
    {"crashtxt_halt1420", t_crashtxt_halt1420},
    {"crashtxt_halt904_location", t_crashtxt_halt904_location},
    {"crashtxt_access_violation", t_crashtxt_access_violation},
    {"crashtxt_empty", t_crashtxt_empty},
    {"crashtxt_inline_forms", t_crashtxt_inline_forms},
    {"redact_build_patterns", t_redact_build_patterns},
    {"redact_in_place_variants", t_redact_in_place_variants},
    {"redact_stream_scanner", t_redact_stream_scanner},
    {"redact_accounts_from_registry", t_redact_accounts_from_registry},
    {"redact_no_plaintext_in_freed_heap", t_redact_no_plaintext_in_freed_heap},
    {"psp2dmp_first_jit", t_psp2dmp_first_jit},
    {"psp2dmp_worker_withheld", t_psp2dmp_worker_withheld},
    {"psp2dmp_eboot_and_sysmodule", t_psp2dmp_eboot_and_sysmodule},
    {"psp2dmp_chain_rules_and_damage", t_psp2dmp_chain_rules_and_damage},
    {"psp2dmp_stamp_rules", t_psp2dmp_stamp_rules},
    {"psp2dmp_peak_memory", t_psp2dmp_peak_memory},
    {"evidence_none_and_hang", t_evidence_none_and_hang},
    {"evidence_guest_fault_and_abnormal", t_evidence_guest_fault_and_abnormal},
    {"evidence_halt", t_evidence_halt},
    {"evidence_host_fault", t_evidence_host_fault},
    {"evidence_peak_memory", t_evidence_peak_memory},
    {"outbox_sizes_and_codecs", t_outbox_sizes_and_codecs},
    {"outbox_create_full", t_outbox_create_full},
    {"outbox_create_small_and_dump_cases", t_outbox_create_small_and_dump_cases},
    {"outbox_peak_memory", t_outbox_peak_memory},
    {"outbox_window_cut_redaction", t_outbox_window_cut_redaction},
    {"outbox_enforce_bounds", t_outbox_enforce_bounds},
    {"outbox_state_crash_window", t_outbox_state_crash_window},
    {"claim_ulid", t_claim_ulid},
    {"claim_json_each_kind", t_claim_json_each_kind},
    {"claim_sanitize_and_bounds", t_claim_sanitize_and_bounds},
    {"claim_from_record", t_claim_from_record},
    {"claims_end_to_end", t_claims_end_to_end},
};

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump-facts")) return dump_facts_main(argc, argv);
        if (!std::strcmp(argv[i], "--collect-peak")) return collect_peak_main(argc, argv);
    }
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fixtures") && i + 1 < argc) g_fixtures = argv[++i];
        else if (!std::strcmp(argv[i], "--work") && i + 1 < argc) g_work = argv[++i];
        else if (!std::strcmp(argv[i], "--dumps") && i + 1 < argc) g_dumps = argv[++i];
        else if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!std::strcmp(argv[i], "--claims-out") && i + 1 < argc) g_claims_out = argv[++i];
    }
    if (g_work.empty()) { std::printf("usage: crashreport_test --fixtures DIR --work DIR [--filter NAME]\n"); return 2; }
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
