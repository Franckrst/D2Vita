// tools/tests/exe_identity_test.cpp -- host-side tests for src/runtime/
// exe_identity.cpp (which Diablo II Game.exe is this?).
//
// The unit reads nothing but a byte buffer, so everything here runs on a dev
// machine. The fingerprints are the two facts a PE gives without executing
// it: file size and COFF link timestamp. Synthetic buffers carry exactly
// those two; the last test, when the maintainer's reference binaries are
// reachable (D2V_REFS or ~/d2-vita-refs), repeats the checks on the real
// files -- Blizzard binaries never enter the repository, so that leg is
// SKIPPED anywhere else and says so.
// Run through tools/oracle_exe_identity.sh.
#include "runtime/exe_identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// The official builds, as measured on the maintainer's reference copies.
static const uint32_t k114dSize = 3618792, k114dStamp = 0x574ddfbc;   // 2016-05-31
static const uint32_t k114bSize = 3590120, k114bStamp = 0x56fc78a8;   // 2016-03-31
static const uint32_t k113cSize = 61440,   k113cStamp = 0x4b95ca4b;   // 2010-03-09, the split-install launcher

// A buffer that is a PE32 as far as the headers go: "MZ", e_lfanew, "PE\0\0",
// and the timestamp at COFF+8. Everything else is zero.
static std::vector<uint8_t> fake_pe(size_t size, uint32_t stamp, uint32_t e_lfanew = 0x100) {
    std::vector<uint8_t> b(size, 0);
    if (size >= 2) { b[0] = 'M'; b[1] = 'Z'; }
    if (size >= 0x40) std::memcpy(&b[0x3c], &e_lfanew, 4);
    if (size >= (size_t)e_lfanew + 12) {
        std::memcpy(&b[e_lfanew], "PE\0\0", 4);
        std::memcpy(&b[e_lfanew + 8], &stamp, 4);
    }
    return b;
}

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

static void t_114d_officielle_acceptee() {
    auto b = fake_pe(k114dSize, k114dStamp);
    d2exe::Identity id = d2exe::identify(b.data(), b.size());
    CHECK(id.pe, "1.14d : pas reconnu comme PE");
    CHECK(id.size == k114dSize, "1.14d : taille %u", id.size);
    CHECK(id.timestamp == k114dStamp, "1.14d : timestamp 0x%08x", id.timestamp);
    CHECK(id.build && std::strcmp(id.build, "1.14d") == 0, "1.14d : build=%s", id.build ? id.build : "(null)");
    CHECK(id.supported, "1.14d : devrait etre supportee");
    std::string p = d2exe::progress_line(id);
    CHECK(has(p, "install: Game.exe"), "ligne progress : prefixe manquant : %s", p.c_str());
    CHECK(has(p, "taille=3618792"), "ligne progress : taille manquante : %s", p.c_str());
    CHECK(has(p, "timestamp=0x574ddfbc"), "ligne progress : timestamp manquant : %s", p.c_str());
    CHECK(has(p, "2016-05-31"), "ligne progress : date manquante : %s", p.c_str());
    CHECK(has(p, "1.14d") && has(p, "OK"), "ligne progress : verdict manquant : %s", p.c_str());
    CHECK(!has(p, "NON SUPPORTEE"), "ligne progress : la 1.14d n'est pas refusee : %s", p.c_str());
}

static void t_114b_refusee_et_nommee() {
    auto b = fake_pe(k114bSize, k114bStamp);
    d2exe::Identity id = d2exe::identify(b.data(), b.size());
    CHECK(id.pe, "1.14b : pas reconnu comme PE");
    CHECK(id.build && std::strcmp(id.build, "1.14b") == 0, "1.14b : build=%s", id.build ? id.build : "(null)");
    CHECK(!id.supported, "1.14b : ne doit PAS etre supportee");
    std::string p = d2exe::progress_line(id);
    CHECK(has(p, "1.14b"), "ligne progress : version non nommee : %s", p.c_str());
    CHECK(has(p, "NON SUPPORTEE"), "ligne progress : refus non dit : %s", p.c_str());
    CHECK(has(p, "1.14d"), "ligne progress : ne dit pas ce qu'il faut : %s", p.c_str());
    std::string s = d2exe::screen_line(id);
    CHECK(has(s, "1.14b"), "ligne ecran : version non nommee : %s", s.c_str());
    CHECK(has(s, "3590120"), "ligne ecran : taille manquante : %s", s.c_str());
    CHECK(has(s, "2016-03-31"), "ligne ecran : date manquante : %s", s.c_str());
    CHECK(has(s, "not 1.14d"), "ligne ecran : ne dit pas 'not 1.14d' : %s", s.c_str());
}

static void t_113c_lanceur_nomme() {
    auto b = fake_pe(k113cSize, k113cStamp);
    d2exe::Identity id = d2exe::identify(b.data(), b.size());
    CHECK(id.build && std::strcmp(id.build, "1.13c") == 0, "1.13c : build=%s", id.build ? id.build : "(null)");
    CHECK(!id.supported, "1.13c : ne doit pas etre supportee");
}

static void t_bonne_taille_mauvais_timestamp_inconnue() {
    auto b = fake_pe(k114dSize, k114dStamp + 1);   // right size, one second off: not the official file
    d2exe::Identity id = d2exe::identify(b.data(), b.size());
    CHECK(id.pe, "inconnue : pas reconnu comme PE");
    CHECK(id.build == nullptr, "inconnue : build=%s au lieu de null", id.build);
    CHECK(!id.supported, "inconnue : ne doit pas etre supportee");
    std::string p = d2exe::progress_line(id);
    CHECK(has(p, "inconnue"), "ligne progress : 'inconnue' absent : %s", p.c_str());
    CHECK(has(p, "NON SUPPORTEE"), "ligne progress : refus non dit : %s", p.c_str());
    CHECK(has(p, "timestamp=0x574ddfbd"), "ligne progress : le timestamp reel n'est pas montre : %s", p.c_str());
    std::string s = d2exe::screen_line(id);
    CHECK(has(s, "unknown build"), "ligne ecran : 'unknown build' absent : %s", s.c_str());
    CHECK(has(s, "3618792"), "ligne ecran : taille manquante : %s", s.c_str());
}

static void t_bon_timestamp_mauvaise_taille_inconnue() {
    auto b = fake_pe(k114dSize + 1, k114dStamp);   // a byte appended: not the official file either
    d2exe::Identity id = d2exe::identify(b.data(), b.size());
    CHECK(id.build == nullptr, "taille+1 : build=%s au lieu de null", id.build);
    CHECK(!id.supported, "taille+1 : ne doit pas etre supportee");
    CHECK(id.size == k114dSize + 1, "taille+1 : taille rapportee %u", id.size);
}

static void t_pas_un_pe() {
    // Empty, too short for e_lfanew, "MZ" alone, e_lfanew pointing past the end,
    // e_lfanew inside the buffer but without room for the COFF header, and a
    // wrong signature: none is a PE, none may read out of bounds (ASan watches).
    { d2exe::Identity id = d2exe::identify(nullptr, 0);
      CHECK(!id.pe && !id.supported && id.build == nullptr, "vide : pe=%d", id.pe); }
    { std::vector<uint8_t> b = {'M', 'Z'};
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      CHECK(!id.pe && !id.supported, "MZ seul : pe=%d", id.pe); CHECK(id.size == 2, "MZ seul : taille %u", id.size); }
    { auto b = fake_pe(0x40, 0, 0x1000);   // e_lfanew far beyond the 64 bytes
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      CHECK(!id.pe && !id.supported, "e_lfanew hors tampon : pe=%d", id.pe); }
    { auto b = fake_pe(0x108, 0, 0x100);   // "PE\0\0" fits, the timestamp does not
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      CHECK(!id.pe && !id.supported, "COFF tronque : pe=%d", id.pe); }
    { auto b = fake_pe(k114dSize, k114dStamp); b[0x101] = 'X';   // "PX\0\0"
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      CHECK(!id.pe && !id.supported && id.build == nullptr, "signature fausse : pe=%d build=%s", id.pe, id.build ? id.build : "(null)"); }
    { auto b = fake_pe(k114dSize, k114dStamp); b[0] = 'Z';        // "ZZ"
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      CHECK(!id.pe && !id.supported, "MZ absent : pe=%d", id.pe); }
    // A non-PE still gets a line a player can act on.
    { std::vector<uint8_t> b(100, 0);
      d2exe::Identity id = d2exe::identify(b.data(), b.size());
      std::string p = d2exe::progress_line(id);
      CHECK(has(p, "pas un executable") && has(p, "NON SUPPORTEE"), "non-PE : ligne progress : %s", p.c_str());
      std::string s = d2exe::screen_line(id);
      CHECK(has(s, "not a Windows executable"), "non-PE : ligne ecran : %s", s.c_str()); }
}

static void t_date_utc() {
    CHECK(d2exe::date_utc(k114dStamp) == "2016-05-31", "date 1.14d : %s", d2exe::date_utc(k114dStamp).c_str());
    CHECK(d2exe::date_utc(k114bStamp) == "2016-03-31", "date 1.14b : %s", d2exe::date_utc(k114bStamp).c_str());
    CHECK(d2exe::date_utc(k113cStamp) == "2010-03-09", "date 1.13c : %s", d2exe::date_utc(k113cStamp).c_str());
    CHECK(d2exe::date_utc(0) == "1970-01-01", "date 0 : %s", d2exe::date_utc(0).c_str());
    CHECK(d2exe::date_utc(951782400) == "2000-02-29", "date bissextile : %s", d2exe::date_utc(951782400).c_str());
    CHECK(d2exe::date_utc(0xffffffffu) == "2106-02-07", "date max : %s", d2exe::date_utc(0xffffffffu).c_str());
}

// On the maintainer's machine only: the real files must be told apart the
// same way the synthetic ones are. Never fails elsewhere -- it says SKIPPED.
static bool slurp(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f); return ok;
}
static void t_fichiers_reels() {
    std::string refs = std::getenv("D2V_REFS") ? std::getenv("D2V_REFS") : "";
    if (refs.empty() && std::getenv("HOME")) refs = std::string(std::getenv("HOME")) + "/d2-vita-refs";
    std::vector<uint8_t> b;
    int seen = 0;
    if (slurp(refs + "/1.14d/Game.exe", b)) { ++seen;
        d2exe::Identity id = d2exe::identify(b.data(), b.size());
        CHECK(id.supported && id.build && std::strcmp(id.build, "1.14d") == 0,
              "vrai 1.14d : build=%s supportee=%d taille=%u ts=0x%08x", id.build ? id.build : "(null)", id.supported, id.size, id.timestamp); }
    if (slurp(refs + "/1.14b/Game.exe", b)) { ++seen;
        d2exe::Identity id = d2exe::identify(b.data(), b.size());
        CHECK(!id.supported && id.build && std::strcmp(id.build, "1.14b") == 0,
              "vrai 1.14b : build=%s supportee=%d taille=%u ts=0x%08x", id.build ? id.build : "(null)", id.supported, id.size, id.timestamp); }
    if (slurp(refs + "/1.13c/Game.exe", b)) { ++seen;
        d2exe::Identity id = d2exe::identify(b.data(), b.size());
        CHECK(!id.supported && id.build && std::strcmp(id.build, "1.13c") == 0,
              "vrai 1.13c : build=%s supportee=%d taille=%u ts=0x%08x", id.build ? id.build : "(null)", id.supported, id.size, id.timestamp); }
    if (!seen) std::printf("  SKIPPED: fichiers reels (aucun Game.exe sous %s)\n", refs.c_str());
    else std::printf("  fichiers reels : %d binaire(s) verifie(s) sous %s\n", seen, refs.c_str());
}

int main() {
    t_114d_officielle_acceptee();
    t_114b_refusee_et_nommee();
    t_113c_lanceur_nomme();
    t_bonne_taille_mauvais_timestamp_inconnue();
    t_bon_timestamp_mauvaise_taille_inconnue();
    t_pas_un_pe();
    t_date_utc();
    t_fichiers_reels();
    std::printf("%s: exe_identity_test (%d verifications, %d echec(s))\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
