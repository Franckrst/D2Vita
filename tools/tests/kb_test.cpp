// tools/tests/kb_test.cpp — host-side oracle for the virtual keyboard.
//
// src/platform/vita_kb.h never touches VitaSDK or a syscall, so everything
// below builds and runs on a dev machine and is fully verifiable without a
// console. The one thing that still needs on-device checking is whether the
// keyboard actually renders above the frame in both presentation paths.
#include "platform/vita_kb.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("  FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const int W = 960, H = 544;
static std::vector<uint32_t> fb_new() { return std::vector<uint32_t>((size_t)W * H, 0xDEADBEEFu); }

static void t_ferme_ne_dessine_rien() {
    d2kb::State s; std::memset(&s, 0, sizeof s);
    auto fb = fb_new(); auto ref = fb;
    d2kb::draw(s, fb.data(), W, H);                 // open == 0
    CHECK(std::memcmp(fb.data(), ref.data(), fb.size() * 4) == 0, "clavier ferme : le tampon a change");
    d2kb::open_kb(s, 0);
    d2kb::draw(s, fb.data(), W, H);
    CHECK(std::memcmp(fb.data(), ref.data(), fb.size() * 4) != 0, "clavier ouvert : rien dessine");
    d2kb::close_kb(s);
    auto fb2 = fb_new(); auto ref2 = fb2;
    d2kb::draw(s, fb2.data(), W, H);
    CHECK(std::memcmp(fb2.data(), ref2.data(), fb2.size() * 4) == 0, "apres fermeture : le tampon a change");
}

static void t_alpha_blend() {
    // Pure math, no framebuffer: black under white at 50 % must land exactly
    // on mid-gray (integer truncation makes this deterministic, not a range).
    using d2kb::draw_detail::rgb;
    using d2kb::draw_detail::blend;
    CHECK(blend(rgb(0,0,0), rgb(255,255,255), 50) == rgb(127,127,127), "50%% de blanc sur noir doit donner gris moyen exact");
    CHECK(blend(rgb(10,20,30), rgb(200,150,90), 100) == rgb(200,150,90), "alpha=100 : la source remplace, sans lire la destination");
    CHECK(blend(rgb(10,20,30), rgb(200,150,90), 0) == rgb(10,20,30), "alpha=0 : la destination ne bouge pas");

    // Through the public entry point: default alpha (unspecified) must draw
    // byte-identical to before this feature existed.
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    auto fb_opaque = fb_new(); d2kb::draw(s, fb_opaque.data(), W, H);
    auto fb_explicit100 = fb_new(); d2kb::draw(s, fb_explicit100.data(), W, H, 100);
    CHECK(std::memcmp(fb_opaque.data(), fb_explicit100.data(), fb_opaque.size() * 4) == 0,
          "alpha implicite != alpha=100 explicite : le defaut a change de comportement");

    // A translucent draw must still show the game frame through the panel.
    auto fb_bg = std::vector<uint32_t>((size_t)W * H, rgb(255, 0, 0));   // saturated red "game frame"
    d2kb::draw(s, fb_bg.data(), W, H, 50);
    const int y0 = d2kb::panel_y0(d2kb::LAY_FULL, H);
    const uint32_t panel_px = fb_bg[(size_t)(y0 + 2) * W + 2];
    CHECK(panel_px != rgb(255, 0, 0), "alpha=50 : le panneau doit changer le fond (rien dessine ?)");
    CHECK((panel_px & 0xFF) > 0x40, "alpha=50 : le rouge du fond doit encore transparaitre (%02x)", panel_px & 0xFF);
    CHECK(fb_bg[0] == rgb(255, 0, 0), "alpha=50 : hors du panneau, le fond ne doit pas bouger");
}

static void t_dessin_borne_au_panneau() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    auto fb = fb_new();
    d2kb::draw(s, fb.data(), W, H);
    const int y0 = d2kb::panel_y0(d2kb::LAY_FULL, H);
    CHECK(y0 > 0 && y0 < H, "y0 hors ecran (%d)", y0);
    int hors = 0;
    for (int y = 0; y < y0; ++y)
        for (int x = 0; x < W; ++x)
            if (fb[(size_t)y * W + x] != 0xDEADBEEFu) ++hors;
    CHECK(hors == 0, "%d pixels ecrits AU-DESSUS du panneau", hors);
}

// Coverage: all 95 printable ASCII characters are reachable.
static void t_couverture_ascii() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    const d2kb::Layout& L = d2kb::LAY_FULL;
    bool vu[128] = { false };
    int dup = 0;
    for (int sh = 0; sh <= 1; ++sh)
        for (int r = 0; r < L.nrows; ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                const char f = d2kb::face(L, r, c, sh);
                CHECK(f >= 33 && f <= 126, "face (%d,%d,maj=%d) = 0x%02x hors ASCII imprimable", r, c, sh, (unsigned)f);
                if (f >= 33 && f <= 126) { if (vu[(int)f]) ++dup; vu[(int)f] = true; }
            }
    CHECK(dup == 0, "%d caracteres en double dans la disposition", dup);
    // space has its own dedicated key
    int ispace = -1;
    for (int i = 0; i < L.nfn; ++i) if (L.fnact[i] == d2kb::ACT_SPACE) ispace = i;
    CHECK(ispace >= 0, "aucune touche ESPACE");
    vu[32] = (ispace >= 0);
    int manquants = 0;
    for (int ch = 32; ch <= 126; ++ch)
        if (!vu[ch]) { ++manquants; std::printf("  manquant: 0x%02x '%c'\n", ch, ch); }
    CHECK(manquants == 0, "%d caracteres imprimables inatteignables", manquants);
    // and each has a non-empty glyph in the font (otherwise the key is silent)
    int vides = 0;
    for (int ch = 33; ch <= 126; ++ch) {
        const unsigned char* g = d2kb::glyph((char)ch);
        CHECK(g != 0, "pas de glyphe pour 0x%02x", ch);
        int on = 0; if (g) for (int i = 0; i < 16; ++i) on += (g[i] != 0);
        if (!on) { ++vides; std::printf("  glyphe vide: 0x%02x '%c'\n", ch, ch); }
    }
    CHECK(vides == 0, "%d glyphes vides", vides);
}

// Emission: each cell yields the expected action and character.
static void t_activation() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    const d2kb::Layout& L = d2kb::LAY_FULL;
    for (int r = 0; r < L.nrows; ++r)
        for (int c = 0; c < d2kb::cols(L, r); ++c) {
            s.shift = 0; char out = 0;
            d2kb::Act a = d2kb::activate(s, r, c, &out);
            CHECK(a == d2kb::ACT_CHAR && out == d2kb::face(L, r, c, 0), "(%d,%d) minuscule", r, c);
            s.shift = 2; out = 0;
            a = d2kb::activate(s, r, c, &out);
            CHECK(a == d2kb::ACT_CHAR && out == d2kb::face(L, r, c, 1), "(%d,%d) majuscule", r, c);
        }
    // out of bounds: never an action, never a write
    char out = 1;
    CHECK(d2kb::activate(s, -1, 0, &out) == d2kb::ACT_NONE && out == 0, "activate(-1,0)");
    CHECK(d2kb::activate(s, 9, 0, &out) == d2kb::ACT_NONE, "activate(9,0)");
    CHECK(d2kb::activate(s, 0, 99, &out) == d2kb::ACT_NONE, "activate(0,99)");
}

static void t_majuscule_trois_etats() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    CHECK(s.shift == 0, "MAJ non nulle a l'ouverture");
    d2kb::shift_cycle(s); CHECK(s.shift == 1, "1er appui MAJ");
    char out = 0;
    d2kb::activate(s, 1, 0, &out);                       // 'q' -> 'Q'
    CHECK(out == 'Q', "une seule touche : attendu Q, obtenu %c", out ? out : '?');
    CHECK(s.shift == 0, "MAJ une-seule-touche non retombee");
    d2kb::shift_cycle(s); d2kb::shift_cycle(s); CHECK(s.shift == 2, "verrouillage MAJ");
    d2kb::activate(s, 1, 0, &out); CHECK(out == 'Q', "verrouille : Q");
    CHECK(s.shift == 2, "le verrou est retombe");
    d2kb::shift_cycle(s); CHECK(s.shift == 0, "3e appui = MAJ relachee");
}

static void t_navigation() {
    for (int simple = 0; simple <= 1; ++simple) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
        const d2kb::Layout& L = d2kb::layout(s);
        const int nr = d2kb::nrows_total(L);
        // exhaustive: from every cell, all 4 directions stay in bounds
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                const int d[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
                for (int k = 0; k < 4; ++k) {
                    s.r = r; s.c = c;
                    d2kb::nav(s, d[k][0], d[k][1]);
                    CHECK(s.r >= 0 && s.r < nr, "nav: rangee %d hors [0,%d[", s.r, nr);
                    CHECK(s.c >= 0 && s.c < d2kb::cols(L, s.r),
                          "nav: colonne %d hors [0,%d[ (rangee %d)", s.c, d2kb::cols(L, s.r), s.r);
                }
            }
        // horizontal wraparound
        s.r = 1; s.c = 0; d2kb::nav(s, 0, -1);
        CHECK(s.c == d2kb::cols(L, 1) - 1, "pas de bouclage a gauche");
        d2kb::nav(s, 0, 1); CHECK(s.c == 0, "pas de bouclage a droite");
        // vertical wraparound
        s.r = 0; s.c = 0; d2kb::nav(s, -1, 0); CHECK(s.r == nr - 1, "pas de bouclage en haut");
        d2kb::nav(s, 1, 0); CHECK(s.r == 0, "pas de bouclage en bas");
    }
}

// Drawn keys and tappable keys must be the exact same cells.
static void t_tactile() {
    for (int simple = 0; simple <= 1; ++simple) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
        const d2kb::Layout& L = d2kb::layout(s);
        for (int r = 0; r < d2kb::nrows_total(L); ++r)
            for (int c = 0; c < d2kb::cols(L, r); ++c) {
                int x, y, w, h; d2kb::cell_rect(L, r, c, W, H, &x, &y, &w, &h);
                CHECK(x >= 0 && y >= 0 && x + w <= W && y + h <= H,
                      "cellule (%d,%d) hors ecran: %d,%d %dx%d", r, c, x, y, w, h);
                int rr = -1, cc = -1;
                CHECK(d2kb::hit(L, W, H, x + w / 2, y + h / 2, &rr, &cc) && rr == r && cc == c,
                      "centre de (%d,%d) touche (%d,%d)", r, c, rr, cc);
            }
        // above the panel: no key
        int rr, cc;
        CHECK(!d2kb::hit(L, W, H, W / 2, d2kb::panel_y0(L, H) - 4, &rr, &cc), "touche au-dessus du panneau");
    }
}

// MASK mode: rendering must depend only on length, never on the letters.
static void t_masque() {
    auto rendu = [](const char* txt, int mask) {
        d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
        s.mask = mask;
        for (int i = 0; txt[i]; ++i) d2kb::echo_push(s, txt[i]);
        auto fb = fb_new();
        d2kb::draw(s, fb.data(), W, H);
        return fb;
    };
    auto a = rendu("motdepasse", 1), b = rendu("ZZZZZZZZZZ", 1);
    CHECK(a == b, "mode masque : deux textes differents rendent des images differentes");
    auto c = rendu("motdepasse", 0);
    CHECK(!(a == c), "mode masque : identique au mode clair");
    auto d = rendu("motdepassX", 1);
    CHECK(a == d, "mode masque : la derniere lettre transparait");
    // different length => different image (dot count is visible)
    auto e = rendu("motdepass", 1);
    CHECK(!(a == e), "mode masque : la longueur ne se voit pas");
}

static void t_echo_bornes() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    for (int i = 0; i < 500; ++i) d2kb::echo_push(s, (char)('a' + (i % 26)));
    CHECK(s.echo_n == d2kb::ECHO_MAX, "echo_n=%d attendu %d", (int)s.echo_n, (int)d2kb::ECHO_MAX);
    CHECK(s.echo[d2kb::ECHO_MAX] == 0, "echo non termine par 0");
    for (int i = 0; i < 500; ++i) d2kb::echo_pop(s);
    CHECK(s.echo_n == 0, "echo_n=%d apres vidage", (int)s.echo_n);
    // corrupted state (render/game thread race): no write outside the buffer
    for (int i = 0; i < 60; ++i) d2kb::echo_push(s, 'x');
    s.echo_n = 12345; d2kb::echo_push(s, 'y'); CHECK(s.echo_n >= 0 && s.echo_n <= d2kb::ECHO_MAX, "echo_n non borne apres corruption");
    s.echo_n = -7;    d2kb::echo_pop(s);       CHECK(s.echo_n == 0, "echo_n negatif non ramene a 0");
    // closing wipes everything (password must not linger in memory)
    for (int i = 0; i < 20; ++i) d2kb::echo_push(s, 's');
    d2kb::close_kb(s);
    char zero[sizeof s.echo]; std::memset(zero, 0, sizeof zero);
    CHECK(std::memcmp((const void*)s.echo, zero, sizeof zero) == 0, "echo non efface a la fermeture");
    CHECK(s.mask == 0 && s.shift == 0, "modificateurs non remis a zero");
}

// draw() must never read out of bounds regardless of state, even
// inconsistent state (the render thread reads state written by the game thread).
static void t_etat_incoherent() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 0);
    auto fb = fb_new();
    const int rs[] = { -5, 0, 3, 4, 99 }, cs[] = { -3, 0, 11, 50 };
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) {
        s.r = rs[i]; s.c = cs[j]; s.echo_n = (i * 7 + j) * 13 - 20; s.shift = i % 3; s.mask = j % 2;
        d2kb::draw(s, fb.data(), W, H);       // must neither crash nor overflow (ASAN)
    }
    ++g_checks;
}

static void t_disposition_simple() {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, 1);
    CHECK(std::strcmp(d2kb::layout(s).name, "simple") == 0, "D2_KBSIMPLE ne rend pas la disposition simple");
    const d2kb::Layout& L = d2kb::layout(s);
    // legacy layout: 4x10 + 4 wide keys, uppercase only
    CHECK(d2kb::cols(L, 0) == 10 && d2kb::cols(L, 4) == 4, "geometrie historique changee");
    char out = 0; d2kb::activate(s, 1, 0, &out); CHECK(out == 'A', "simple: (1,0) != A");
}

// --ppm <file>: renders a keyboard image (visual preview only, not a test).
static int dump_ppm(const char* path, int simple, int mask) {
    d2kb::State s; std::memset(&s, 0, sizeof s); d2kb::open_kb(s, simple);
    s.mask = mask; s.shift = 1; s.r = 2; s.c = 3;
    const char* demo = mask ? "motdepasse" : "Compte-D2 2026!";
    for (int i = 0; demo[i]; ++i) d2kb::echo_push(s, demo[i]);
    std::vector<uint32_t> fb((size_t)W * H, 0xFF203040u);
    d2kb::draw(s, fb.data(), W, H);
    FILE* f = std::fopen(path, "wb"); if (!f) return 1;
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < fb.size(); ++i) {
        const uint32_t v = fb[i];
        const unsigned char rgb[3] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF), (unsigned char)((v >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f); return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--ppm"))
        return dump_ppm(argv[2], argc > 3 ? std::atoi(argv[3]) : 0, argc > 4 ? std::atoi(argv[4]) : 0);
    t_ferme_ne_dessine_rien();
    t_alpha_blend();
    t_dessin_borne_au_panneau();
    t_couverture_ascii();
    t_activation();
    t_majuscule_trois_etats();
    t_navigation();
    t_tactile();
    t_masque();
    t_echo_bornes();
    t_etat_incoherent();
    t_disposition_simple();
    std::printf("%s: %d verifications, %d echecs\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
