// src/runtime/cdkeys_file.cpp — CD keys from a text file (keys.txt).
//
// WHERE DIABLO II 1.14d KEEPS ITS KEYS (docs-site/en-ligne.md): NOT in the
// registry (BLIZZARDKEY there is an RSA PUBLIC key used to sign MPQs).
// Game.exe loads three small encrypted files from the MPQ chain, written by
// the installer:
//     classic key   font\font.gid   else data\global\sfx\cursor\wavindx.wav
//     owner name    font\font.clh   else data\global\sfx\cursor\curindx.wav
//     LoD key       data\global\chars\am\cof\amblxbow.cof
// then passes each buffer to Game+0x1232b0 (ecx=buffer, edi=size), which
// decrypts it IN PLACE and checks its integrity. The cipher depends on NO
// machine-specific data: password drawn from srand(0x150b), IDEA key and
// SHA-0 state (signed-shift variant) derived from srand(0x4fa7). The encoder
// below is the transcription of the exact inverse of 0x523060 (dead code in
// the binary), PROVEN byte-for-byte against the game's own code running under
// Wine (KAT vector below, produced by Game.exe itself).
//
// FAITHFUL APPROACH: no special-casing. At the entry of 0x1232b0 (translation
// hook, .text untouched), if keys.txt supplies the value, the buffer read
// from the MPQ is REPLACED with the blob the installer would have written for
// that key, then the game resumes and decrypts it itself, through its own
// code path. Without keys.txt: nothing is installed, behavior is unchanged.
//
// HYGIENE: the log only ever says "present/absent/applied/refused" — never a
// value, a key length, a blob, or a hash. Host-side copies are wiped after
// use. (The plaintext key then lives in guest memory, same as on PC: never
// attach a memory dump to a report.)
#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace d2rt;

const char* d2_keystore_file();   // win32_shims_advapi32_d2.cpp: sets the secret directory

namespace {

// ------------------------------------------------------------------ MSVC rand
struct MsRand { uint32_t s; explicit MsRand(uint32_t seed) : s(seed) {}
    uint32_t next() { s = s * 0x343FDu + 0x269EC3u; return (s >> 16) & 0x7FFF; } };

// ---------------------------------------- SHA-0 variant (Game+0x123af0/0x124200)
inline uint32_t sar(uint32_t x, int n) { return (uint32_t)((int32_t)x >> n); }
inline uint32_t rol5(uint32_t x)  { return (x << 5)  | sar(x, 27); }   // SIGNED shift, matching the binary
inline uint32_t rol30(uint32_t x) { return (x << 30) | sar(x, 2); }

void sha_compress(uint32_t st[5], const uint8_t* blk) {
    uint32_t W[80];
    for (int t = 0; t < 16; ++t)
        W[t] = (uint32_t)blk[4*t] | (uint32_t)blk[4*t+1] << 8 | (uint32_t)blk[4*t+2] << 16 | (uint32_t)blk[4*t+3] << 24;
    for (int t = 16; t < 80; ++t) W[t] = W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16];   // no rotation: SHA-0
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int t = 0; t < 80; ++t) {
        uint32_t f, k;
        if (t < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (t < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t tmp = rol5(a) + f + W[t] + e + k;
        e = d; d = c; c = rol30(b); b = a; a = tmp;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}
void sha_bytes(const uint32_t st[5], uint8_t out[20]) {
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[4*i+j] = (uint8_t)(st[i] >> (8*j));
}
const uint32_t SHA_IV[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

// ---------------------------------------------------- IDEA, little-endian words
inline uint16_t idea_mul(uint32_t a, uint32_t b) {
    a &= 0xFFFF; b &= 0xFFFF;
    if (!a) return (uint16_t)(1 - b);
    if (!b) return (uint16_t)(1 - a);
    uint32_t p = a * b, lo = p & 0xFFFF, hi = p >> 16;
    return (uint16_t)(lo - hi + (lo < hi));
}
void idea_expand(const uint8_t key[16], uint16_t ek[52]) {           // Game+0x124360
    for (int i = 0; i < 8; ++i) ek[i] = (uint16_t)(key[2*i] | key[2*i+1] << 8);
    for (int i = 8; i < 52; ++i) ek[i] = 0;
    int base = 0, i = 0;
    for (int j = 8; j < 52; ++j) {
        ++i;
        ek[base + i + 7] = (uint16_t)((ek[base + (i & 7)] << 9) | (ek[base + ((i + 1) & 7)] >> 7));
        base += i & 8; i &= 7;
    }
}
void idea_block(const uint16_t k[52], uint8_t* b8) {                 // Game+0x124540
    uint32_t x1 = b8[0] | b8[1] << 8, x2 = b8[2] | b8[3] << 8, x3 = b8[4] | b8[5] << 8, x4 = b8[6] | b8[7] << 8;
    int i = 0;
    for (int r = 0; r < 8; ++r, i += 6) {
        x1 = idea_mul(x1, k[i]); x2 = (x2 + k[i+1]) & 0xFFFF;
        x3 = (x3 + k[i+2]) & 0xFFFF; x4 = idea_mul(x4, k[i+3]);
        uint32_t t0 = idea_mul(x3 ^ x1, k[i+4]);
        uint32_t t1 = idea_mul(((x4 ^ x2) + t0) & 0xFFFF, k[i+5]);
        t0 = (t0 + t1) & 0xFFFF;
        x1 ^= t1; x4 ^= t0;
        uint32_t nx2 = x3 ^ t1, nx3 = x2 ^ t0; x2 = nx2; x3 = nx3;
    }
    uint16_t o[4] = { idea_mul(x1, k[48]), (uint16_t)(x3 + k[49]), (uint16_t)(x2 + k[50]), idea_mul(x4, k[51]) };
    for (int j = 0; j < 4; ++j) { b8[2*j] = (uint8_t)o[j]; b8[2*j+1] = (uint8_t)(o[j] >> 8); }
}

// ------------------------------------------------------- encryption (0x523060)
std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plain) {
    // Password (Game+0x122bc0): srand(0x150b), 19 non-null bytes, NUL.
    uint8_t pw[20]; { MsRand r(0x150B); int n = 0; while (n < 19) { uint8_t v = (uint8_t)r.next(); if (v) pw[n++] = v; } pw[19] = 0; }
    // Key derivation (Game+0x122c30).
    uint8_t R[112]; { MsRand r(0x4FA7); for (int j = 0; j < 112; ++j) R[j] = (uint8_t)r.next(); }
    uint8_t blk[64]; for (int j = 0, i = 0; j < 64; ++j) { if (!pw[i]) i = 0; blk[j] = pw[i++]; }
    uint32_t st[5]; std::memcpy(st, SHA_IV, sizeof st); sha_compress(st, blk);
    uint8_t d[20]; sha_bytes(st, d);
    for (int j = 0; j < 112; ++j) R[j] ^= d[j % 20];
    uint16_t ek[52]; idea_expand(R, ek);
    std::memcpy(st, SHA_IV, sizeof st); sha_compress(st, R + 48);

    std::vector<uint8_t> out; uint32_t last = 0;
    for (size_t b = 0; b < plain.size(); b += 64) {
        size_t n = plain.size() - b < 64 ? plain.size() - b : 64; last = (uint32_t)n;
        uint8_t P[64] = {0}; std::memcpy(P, plain.data() + b, n);
        uint8_t K[20]; sha_bytes(st, K);
        const size_t idx = b / 64;
        if ((idx & 0xF) == 0) sha_compress(st, P);
        uint8_t C[64]; for (int j = 0; j < 64; ++j) C[j] = (uint8_t)(P[j] + K[(63 - j) % 20]);
        if ((idx & 7) == 0) for (int j = 0; j < 64; j += 8) idea_block(ek, C + j);
        out.insert(out.end(), C, C + 64);
        std::memset(P, 0, sizeof P);
    }
    uint8_t T[20]; sha_bytes(st, T);
    const uint8_t tr[8] = {T[0], T[1], T[2], T[3], 0, (uint8_t)last, 0, 0};
    out.insert(out.end(), tr, tr + 8);
    return out;
}

// KAT vector: blob produced by the GAME'S OWN CODE (Game.exe 0x523060 under
// Wine, via the re_cdkey harness) for the FAKE string "FAUSSECLEFAUSSECLEFAUSSECL".
bool self_test() {
    static const char* PLAIN = "FAUSSECLEFAUSSECLEFAUSSECL";
    static const uint8_t KAT[72] = {
        0xa3,0x2f,0x90,0xf7,0xde,0x18,0x2a,0x16,0x25,0xb6,0x0e,0x75,0x8f,0x1a,0x12,0x43,
        0x67,0x7f,0xff,0xb0,0xdb,0x59,0xd8,0x0b,0x14,0x06,0x6f,0x2e,0x41,0xe2,0x43,0xf5,
        0x52,0x7f,0xb6,0x80,0xe1,0x00,0xc0,0x84,0x68,0xd5,0x62,0x38,0x4c,0xe8,0xc5,0x39,
        0x6f,0x83,0x97,0x02,0x60,0x95,0x15,0x1a,0xca,0xb9,0xd7,0x64,0xb3,0x75,0x18,0x77,
        0x13,0xa1,0xd1,0x6b,0x00,0x1b,0x00,0x00 };
    std::vector<uint8_t> p(PLAIN, PLAIN + std::strlen(PLAIN) + 1);
    std::vector<uint8_t> b = encrypt(p);
    return b.size() == sizeof KAT && std::memcmp(b.data(), KAT, sizeof KAT) == 0;
}

// ------------------------------------------------------------------- keys.txt
enum Kind { K_CLASSIC = 0, K_OWNER = 1, K_LOD = 2, K_N = 3 };
std::string g_val[K_N];           // wiped after encoding
std::vector<uint8_t> g_blob[K_N];
bool g_have[K_N] = {false, false, false};

void wipe(std::string& s) { if (!s.empty()) std::memset(&s[0], 0, s.size()); s.clear(); }

bool load_keys_txt() {
    const std::string ks = d2_keystore_file();
    size_t sl = ks.find_last_of("/\\");
    const std::string path = (sl == std::string::npos ? std::string(".") : ks.substr(0, sl)) + "/keys.txt";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { jpline("keys.txt: absent (cles du MPQ inchangees)"); return false; }
    char line[256]; int ln = 0, bad = 0;
    std::string owners[3];   // owner, classic_owner, lod_owner
    while (std::fgets(line, sizeof line, f)) {
        ++ln;
        std::string s(line); std::memset(line, 0, sizeof line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos || s[a] == '#') { wipe(s); continue; }
        size_t eq = s.find('=');
        if (eq == std::string::npos) { ++bad; jpline("keys.txt: ligne %d ignoree (attendu nom=valeur)", ln); wipe(s); continue; }
        std::string name = s.substr(a, eq - a), v = s.substr(eq + 1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        for (auto& ch : name) ch = (char)std::tolower((unsigned char)ch);
        // One owner name per key (classic_owner / lod_owner), or a shared `owner`.
        // The GAME only has ONE: a single file (font.clh / curindx.wav), a single
        // field in SID_AUTH_CHECK — none of the 5 calls to Game+0x1232b0 read an
        // LoD-specific owner. Picked once the file is fully read.
        int osl = name == "owner" ? 0 : name == "classic_owner" ? 1 : name == "lod_owner" ? 2 : -1;
        int k = name == "classic" ? K_CLASSIC : name == "lod" ? K_LOD : osl >= 0 ? K_OWNER : -1;
        if (k < 0) { ++bad; jpline("keys.txt: ligne %d : nom inconnu (classic|lod|classic_owner|lod_owner|owner)", ln); wipe(s); wipe(v); continue; }
        std::string clean;
        if (k == K_OWNER) {                                   // free text, leading/trailing spaces trimmed
            size_t b0 = v.find_first_not_of(" \t"), b1 = v.find_last_not_of(" \t");
            if (b0 != std::string::npos) clean = v.substr(b0, b1 - b0 + 1);
            bool ok = !clean.empty() && clean.size() <= 63;
            for (char ch : clean) if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) ok = false;
            if (!ok) { ++bad; jpline("keys.txt: ligne %d : owner invalide (1 a 63 caracteres ASCII)", ln); wipe(s); wipe(v); wipe(clean); continue; }
        } else {                                              // dashes/spaces ignored, uppercased
            for (char ch : v) if (ch != '-' && ch != ' ' && ch != '\t') clean += (char)std::toupper((unsigned char)ch);
            bool ok = clean.size() == 26 || clean.size() == 16;   // lengths accepted by Game+0x11dfc0
            for (char ch : clean) if (!std::isalnum((unsigned char)ch)) ok = false;
            if (!ok) { ++bad; jpline("keys.txt: ligne %d : cle mal formee (16 ou 26 caracteres alphanumeriques)", ln); wipe(s); wipe(v); wipe(clean); continue; }
        }
        if (k == K_OWNER) { wipe(owners[osl]); owners[osl] = clean; }
        else { wipe(g_val[k]); g_val[k] = clean; g_have[k] = true; }
        wipe(s); wipe(v); wipe(clean);
    }
    std::fclose(f);
    {   // shared owner > classic_owner > lod_owner
        int pick = !owners[0].empty() ? 0 : !owners[1].empty() ? 1 : !owners[2].empty() ? 2 : -1;
        if (pick >= 0) { wipe(g_val[K_OWNER]); g_val[K_OWNER] = owners[pick]; g_have[K_OWNER] = true; }
        if (!owners[1].empty() && !owners[2].empty() && owners[1] != owners[2])
            jpline("keys.txt: classic_owner et lod_owner different — le jeu n'envoie qu'UN proprietaire : %s retenu",
                   pick == 0 ? "owner" : "classic_owner");
        for (auto& o : owners) wipe(o);
    }
    if (!self_test()) {
        jpline("keys.txt: AUTO-TEST DE L'ENCODEUR EN ECHEC — cles NON appliquees (MPQ inchange)");
        for (auto& v : g_val) wipe(v);
        for (auto& h : g_have) h = false;
        return false;
    }
    int n = 0;
    for (int k = 0; k < K_N; ++k) if (g_have[k]) {
        std::vector<uint8_t> p(g_val[k].begin(), g_val[k].end()); p.push_back(0);
        g_blob[k] = encrypt(p);
        std::memset(p.data(), 0, p.size()); wipe(g_val[k]); ++n;
    }
    jpline("keys.txt: present — classic=%s lod=%s owner=%s (%d ligne(s) refusee(s))",
           g_have[K_CLASSIC] ? "oui" : "non", g_have[K_LOD] ? "oui" : "non", g_have[K_OWNER] ? "oui" : "non", bad);
    return n > 0;
}

} // namespace

void cdkeys_hooks_install(Cpu* cpu, Bridge& br) {
    if (!g_114 || !g_d2base) return;
    if (!load_keys_txt()) return;
    static const uint32_t ENTRY_RVA = 0x1232b0;
    static uint32_t s_entry = 0; s_entry = g_d2base + ENTRY_RVA;
    Shim s; s.argc = 0; s.stdcall_cleanup = false; s.tag = "native!cdkey_file";
    s.fn = [&br](Cpu& c) -> uint32_t {
        const uint32_t E = c.reg(R_ESP);
        const uint32_t ret = c.read_u32(E) - g_d2base;
        int k = -1; const char* what = "?";
        switch (ret) {                                        // calling site -> file being processed
            case 0x1234b1: case 0x123656: k = K_CLASSIC; what = "classic"; break;
            case 0x1236d1: case 0x12380d: k = K_OWNER;   what = "owner";   break;
            case 0x123942:                k = K_LOD;     what = "lod";     break;
        }
        if (k >= 0 && g_have[k]) {
            const uint32_t buf = c.reg(R_ECX), size = c.reg(R_EDI);
            if (buf && size == g_blob[k].size()) {
                c.write(buf, g_blob[k].data(), size);
                jpline("keys.txt: %s applique (le jeu dechiffre lui-meme)", what);
            } else {
                jpline("keys.txt: %s NON applique — taille du fichier MPQ inattendue", what);
            }
        }
        c.write_u32(E - 4, c.reg(R_EBP));   // faithful fallback: `push ebp`
        c.set_reg(R_ESP, E - 8);            // the handler's +4 lands back on E-4
        br.redirect_next(s_entry + 1);      // resumes at `mov ebp,esp`
        return c.reg(R_EAX); };             // EAX unchanged
    br.register_shim("native.hook", "cdkey_file", s);
    cpu->set_alternate(s_entry, br.shim_trap("native.hook", "cdkey_file"));
}
