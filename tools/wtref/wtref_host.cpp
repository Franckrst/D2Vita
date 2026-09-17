// tools/wtref/wtref_host.cpp — runtime leg of the Authenticode oracle.
//
// Runs the real winx86 shim bodies (win32_shims_wintrust.cpp, via
// wx86_wintrust_register) on a fake flat 32-bit-memory CPU, with the same
// call sequence and arguments as tools/wtref/wtref.c (which runs under
// Wine), printing the same canonical dump. No bridge, no dynarec: this
// bench exercises only the shims and the library, not the loader.
//
//   g++ -std=gnu++17 -O2 -I third_party/winx86/src tools/wtref/wtref_host.cpp
//       third_party/winx86/src/runtime/{authenticode,win32_shims_wintrust,guest_scratch,guest_thread_ctx,cpu}.cpp
//   (see tools/oracle_authenticode.sh)
//   ./a.out [--add-root cert-file.cer]... 'Z:/path\file' ...
#include "runtime/authenticode.h"
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/win32_shims_wintrust.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace d2rt;

struct FakeCpu : Cpu {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x04000000, 0);
    uint32_t r[R_COUNT] = {0};
    bool map(uint32_t, uint32_t, const void*, int) override { return true; }
    bool protect(uint32_t, uint32_t, int) override { return true; }
    bool read(uint32_t va, void* dst, uint32_t n) override {
        if ((uint64_t)va + n > mem.size() || va < 0x1000) { std::memset(dst, 0, n); return false; }
        std::memcpy(dst, &mem[va], n); return true;
    }
    bool write(uint32_t va, const void* src, uint32_t n) override {
        if ((uint64_t)va + n > mem.size() || va < 0x1000) return false;
        std::memcpy(&mem[va], src, n); return true;
    }
    uint32_t reg(int i) override { return r[i]; }
    void set_reg(int i, uint32_t v) override { r[i] = v; }
    void set_trap(uint32_t, uint32_t, TrapFn) override {}
    bool run(uint32_t, const char**) override { return false; }
    uint32_t fault_addr() const override { return 0; }
};

// win32_shims_wintrust_install() (same translation unit) references
// Bridge::register_shim; this bench only calls wx86_wintrust_register and
// never links bridge.cpp. Sentinel definition: calling it aborts the bench.
void Bridge::register_shim(const std::string&, const std::string&, Shim) { std::abort(); }

static FakeCpu cpu;
static std::map<std::string, Shim> shims;
static uint32_t heapTop = 0x00400000;
static const uint32_t TIB = 0x00002000, STACK_TOP = 0x00300000;

static uint32_t galloc(uint32_t n) { uint32_t a = heapTop; heapTop += (n + 15) & ~15u; std::memset(&cpu.mem[a], 0, n); return a; }
static uint32_t call(const char* name, std::vector<uint32_t> args) {
    auto it = shims.find(name);
    if (it == shims.end()) { std::fprintf(stderr, "shim absent: %s\n", name); std::exit(2); }
    if (it->second.argc != args.size()) { std::fprintf(stderr, "argc %s\n", name); std::exit(2); }
    uint32_t esp = STACK_TOP - 4 * (uint32_t)(args.size() + 1);
    cpu.write_u32(esp, 0xCAFEF00D);
    for (size_t i = 0; i < args.size(); i++) cpu.write_u32(esp + 4 + 4 * (uint32_t)i, args[i]);
    cpu.set_reg(R_ESP, esp);
    return it->second.fn(cpu);
}
static void SetLastError(uint32_t v) { cpu.write_u32(TIB + 0x34, v); }
static uint32_t GetLastError() { return cpu.read_u32(TIB + 0x34); }
static uint32_t u32(uint32_t a) { return cpu.read_u32(a); }

static unsigned long long fnv(const uint8_t* p, uint32_t n) {
    unsigned long long h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
static uint8_t* gp(uint32_t a) { return a ? &cpu.mem[a] : nullptr; }
static void hex(uint32_t p, uint32_t n) { for (uint32_t i = 0; i < n; i++) std::printf("%02x", cpu.mem[p + i]); }
static void blob(const char* tag, uint32_t pb, uint32_t cb) {
    std::printf(" %s=%lu:%016llx", tag, (unsigned long)cb, pb && cb ? fnv(gp(pb), cb) : 0ULL);
}
static std::string cstr(uint32_t p) { return p ? std::string((const char*)gp(p)) : std::string("(null)"); }
static void alg(const char* tag, uint32_t a) {                   // CRYPT_ALGORITHM_IDENTIFIER
    std::printf(" %s=%s", tag, cstr(u32(a)).c_str());
    blob("par", u32(a + 8), u32(a + 4));
}
static void attrs(const char* tag, uint32_t a) {                 // CRYPT_ATTRIBUTES
    uint32_t n = u32(a), rg = u32(a + 4);
    std::printf("SI %s n=%lu\n", tag, (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = rg + 12 * i, cv = u32(e + 4), rv = u32(e + 8);
        std::printf("SI %s[%lu] %s vals=%lu", tag, (unsigned long)i, cstr(u32(e)).c_str(), (unsigned long)cv);
        for (uint32_t j = 0; j < cv; j++) blob("v", u32(rv + 8 * j + 4), u32(rv + 8 * j));
        std::printf("\n");
    }
}
static void utf8w(uint32_t p) {
    for (;; p += 2) { uint16_t w = (uint16_t)(cpu.mem[p] | (cpu.mem[p + 1] << 8)); if (!w) break;
        if (w >= 32 && w < 127) std::putchar((char)w); else std::printf("\\u%04x", w); }
}
static uint32_t wstr(const std::string& s) {
    uint32_t a = galloc((uint32_t)(s.size() + 1) * 2);
    for (size_t i = 0; i < s.size(); i++) { cpu.mem[a + 2 * i] = (uint8_t)s[i]; }
    return a;
}
static uint32_t astr(const char* s) { uint32_t a = galloc((uint32_t)std::strlen(s) + 1); std::strcpy((char*)gp(a), s); return a; }

static void wvt(uint32_t path, bool cr) {
    uint32_t act = galloc(16);
    static const uint8_t G[16] = {0x6b,0xc5,0xaa,0x00,0x44,0xcd,0xd0,0x11,0x8c,0xc2,0x00,0xc0,0x4f,0xc2,0x95,0xee};
    std::memcpy(gp(act), G, 16);
    uint32_t fi = galloc(0x10), wd = galloc(0x30);
    cpu.write_u32(fi, 0x10); cpu.write_u32(fi + 4, path);
    cpu.write_u32(wd, 0x30); cpu.write_u32(wd + 0x0c, 2); cpu.write_u32(wd + 0x10, 0);
    cpu.write_u32(wd + 0x14, 1); cpu.write_u32(wd + 0x18, fi); cpu.write_u32(wd + 0x1c, 0);
    cpu.write_u32(wd + 0x28, cr ? 0x100 : 0);
    SetLastError(0xdeadbeef);
    uint32_t r = call("WinVerifyTrust", { cr ? 0xFFFFFFFFu : 0u, act, wd });
    std::printf("WVT %s hr=0x%08lx le=0x%08lx\n", cr ? "checkrevision" : "game", (unsigned long)r, (unsigned long)GetLastError());
}

static void run(const std::string& guestPath) {
    uint32_t path = wstr(guestPath);
    wvt(path, false);
    wvt(path, true);
    uint32_t pst = galloc(4), pmsg = galloc(4);
    SetLastError(0xdeadbeef);
    uint32_t ok = call("CryptQueryObject", {1, path, 0x400, 2, 0, 0, 0, 0, pst, pmsg, 0});
    uint32_t st = u32(pst), msg = u32(pmsg);
    std::printf("CQO ret=%d le=0x%08lx store=%d msg=%d\n", (int)ok, (unsigned long)(ok ? 0 : GetLastError()), st != 0, msg != 0);
    if (!ok) return;
    { uint32_t enc = galloc(4), ct = galloc(4), fmt = galloc(4), s2 = galloc(4), m2 = galloc(4);
      ok = call("CryptQueryObject", {1, path, 0x400, 2, 0, enc, ct, fmt, s2, m2, 0});
      std::printf("CQO2 ret=%d enc=0x%lx ct=%lu fmt=%lu\n", (int)ok, (unsigned long)u32(enc), (unsigned long)u32(ct), (unsigned long)u32(fmt));
      if (u32(m2)) call("CryptMsgClose", {u32(m2)});
      if (u32(s2)) call("CertCloseStore", {u32(s2), 0}); }
    uint32_t pn = galloc(4), pcb = galloc(4);
    cpu.write_u32(pcb, 4); ok = call("CryptMsgGetParam", {msg, 5, 0, pn, pcb});
    std::printf("MSG signers ret=%d n=%lu\n", (int)ok, (unsigned long)u32(pn));
    cpu.write_u32(pcb, 4); ok = call("CryptMsgGetParam", {msg, 11, 0, pn, pcb});
    uint32_t n = u32(pn);
    std::printf("MSG certs ret=%d n=%lu\n", (int)ok, (unsigned long)n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t buf = galloc(8192); cpu.write_u32(pcb, 8192);
        ok = call("CryptMsgGetParam", {msg, 12, i, buf, pcb});
        std::printf("MSG cert[%lu] ret=%d", (unsigned long)i, (int)ok); blob("der", buf, ok ? u32(pcb) : 0); std::printf("\n");
    }
    SetLastError(0xdeadbeef);
    ok = call("CryptMsgGetParam", {msg, 99, 0, 0, pcb});
    std::printf("MSG param99 ret=%d le=0x%08lx\n", (int)ok, (unsigned long)(ok ? 0 : GetLastError()));
    SetLastError(0xdeadbeef);
    ok = call("CryptMsgGetParam", {msg, 6, 5, 0, pcb});
    std::printf("MSG signer5 ret=%d le=0x%08lx\n", (int)ok, (unsigned long)(ok ? 0 : GetLastError()));

    cpu.write_u32(pcb, 0); ok = call("CryptMsgGetParam", {msg, 6, 0, 0, pcb});
    std::printf("SI size ret=%d\n", (int)ok);
    uint32_t cb = u32(pcb), si = galloc(cb);
    { uint32_t psmall = galloc(4); cpu.write_u32(psmall, 8); SetLastError(0xdeadbeef);
      ok = call("CryptMsgGetParam", {msg, 6, 0, si, psmall});
      std::printf("SI small ret=%d le=0x%08lx grown=%d\n", (int)ok, (unsigned long)(ok ? 0 : GetLastError()), u32(psmall) == cb); }
    ok = call("CryptMsgGetParam", {msg, 6, 0, si, pcb});
    std::printf("SI ret=%d ver=%lu", (int)ok, (unsigned long)u32(si));
    blob("issuer", u32(si + 8), u32(si + 4));
    std::printf(" serial="); hex(u32(si + 0x10), u32(si + 0x0c));
    alg("hash", si + 0x14); alg("enc", si + 0x20);
    blob("encdig", u32(si + 0x30), u32(si + 0x2c));
    std::printf("\n");
    attrs("auth", si + 0x34); attrs("unauth", si + 0x3c);

    uint32_t ci = galloc(0x70);
    cpu.write_u32(ci + 0x18, u32(si + 4)); cpu.write_u32(ci + 0x1c, u32(si + 8));     // Issuer
    cpu.write_u32(ci + 0x04, u32(si + 0x0c)); cpu.write_u32(ci + 0x08, u32(si + 0x10)); // SerialNumber
    SetLastError(0xdeadbeef);
    uint32_t ctx = call("CertFindCertificateInStore", {st, 0x10001, 0, 0xB0000, ci, 0});
    std::printf("FIND ret=%d le=0x%08lx\n", ctx != 0, (unsigned long)(ctx ? 0 : GetLastError()));
    if (ctx) {
        uint32_t x = u32(ctx + 12);
        std::printf("CTX enc=%lu", (unsigned long)u32(ctx)); blob("der", u32(ctx + 4), u32(ctx + 8));
        std::printf(" storeMatch=%d\n", u32(ctx + 16) == st);
        std::printf("CI ver=%lu serial=", (unsigned long)u32(x)); hex(u32(x + 8), u32(x + 4));
        alg("sig", x + 0x0c); blob("issuer", u32(x + 0x1c), u32(x + 0x18));
        std::printf(" nb=%08lx%08lx na=%08lx%08lx", (unsigned long)u32(x + 0x24), (unsigned long)u32(x + 0x20),
                    (unsigned long)u32(x + 0x2c), (unsigned long)u32(x + 0x28));
        blob("subject", u32(x + 0x34), u32(x + 0x30));
        alg("spki", x + 0x38);
        blob("pk", u32(x + 0x48), u32(x + 0x44));
        std::printf(" unused=%lu pkhead=", (unsigned long)u32(x + 0x4c));
        hex(u32(x + 0x48), u32(x + 0x44) > 12 ? 12 : u32(x + 0x44));
        uint32_t ne = u32(x + 0x68), rg = u32(x + 0x6c);
        std::printf(" ext=%lu\n", (unsigned long)ne);
        for (uint32_t k = 0; k < ne; k++) {
            uint32_t e = rg + 16 * k;
            std::printf("CI ext[%lu] %s crit=%d", (unsigned long)k, cstr(u32(e)).c_str(), (int)u32(e + 4));
            blob("val", u32(e + 12), u32(e + 8)); std::printf("\n");
        }
        uint32_t name = galloc(1024), O = astr("2.5.4.10"), CN = astr("2.5.4.3"), T = astr("2.5.4.12");
        uint32_t nn = call("CertGetNameStringW", {ctx, 3, 0, O, 0, 0});
        std::printf("NAME O size=%lu", (unsigned long)nn);
        nn = call("CertGetNameStringW", {ctx, 3, 0, O, name, 512});
        std::printf(" ret=%lu \"", (unsigned long)nn); utf8w(name); std::printf("\"\n");
        nn = call("CertGetNameStringW", {ctx, 3, 0, O, name, 9});
        std::printf("NAME O trunc ret=%lu \"", (unsigned long)nn); utf8w(name); std::printf("\"\n");
        nn = call("CertGetNameStringW", {ctx, 4, 0, 0, name, 512});
        std::printf("NAME simple ret=%lu \"", (unsigned long)nn); utf8w(name); std::printf("\"\n");
        nn = call("CertGetNameStringW", {ctx, 3, 1, CN, name, 512});
        std::printf("NAME issuerCN ret=%lu \"", (unsigned long)nn); utf8w(name); std::printf("\"\n");
        nn = call("CertGetNameStringW", {ctx, 3, 0, T, name, 512});
        std::printf("NAME absent ret=%lu \"", (unsigned long)nn); utf8w(name); std::printf("\"\n");
        std::printf("FREE ret=%d\n", (int)call("CertFreeCertificateContext", {ctx}));
    }
    int a = (int)call("CertCloseStore", {st, 0});
    int b = (int)call("CryptMsgClose", {msg});
    std::printf("CLOSE store=%d msg=%d\n", a, b);
}

static void observer(const char* line) { if (std::getenv("WTREF_TRACE")) std::fprintf(stderr, "  [wintrust] %s\n", line); }

int main(int argc, char** argv) {
    wx86_set_main_tib(TIB);
    wx86_scratch_init(0x02000000, 0x01000000);
    wx86_wintrust_register([](const char*, const char* name, const Shim& s) { shims[name] = s; });
    wx86_wintrust_set_observer(observer);
    // Guest path "Z:/dir\file" (Wine convention) -> host file.
    wx86_wintrust_set_file_source([](const std::string& g, std::vector<uint8_t>& out) {
        std::string h = g.size() > 2 && g[1] == ':' ? g.substr(2) : g;
        for (char& ch : h) if (ch == '\\') ch = '/';
        std::ifstream f(h, std::ios::binary); if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {}); return true;
    });
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--add-root") && i + 1 < argc) {
            std::ifstream f(argv[++i], std::ios::binary); std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
            if (!wx86::ac::add_trusted_root(d.data(), d.size())) { std::fprintf(stderr, "racine illisible: %s\n", argv[i]); return 2; }
            continue;
        }
        std::string p = argv[i];
        size_t s = p.find_last_of('\\'); std::string base = s == std::string::npos ? p : p.substr(s + 1);
        std::printf("== %s\n", base.c_str());
        run(p);
    }
    return 0;
}
