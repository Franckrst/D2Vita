// tools/radial_gpu_check.cpp — vérifie que les QUADS destinés au GPU dessinent
// bien le même menu que le blitter CPU, avant d'aller le découvrir sur console.
//
// Le chemin GPU remplace un parcours en espace-sprite (transformation inverse
// par pixel) par deux triangles texturés. Les deux échantillonnent au plus
// proche, donc le résultat doit être quasi identique — pas au bit près (les
// règles de couverture d'un rasteriseur ne sont pas celles d'une boîte
// englobante), mais toute erreur de V inversé, de sous-rectangle d'atlas, de
// rotation ou d'ordre de composition se voit immédiatement.
//
// Ce qu'on refuse ici coûte 10 minutes de console à découvrir autrement.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "platform/radial_menu.h"

namespace {
constexpr int W = 960, H = 544;

// Deux fonds, et c'est le couple qui informe :
//  - BRUITÉ : ultra-sensible, mais un décalage d'un demi-pixel sur un bord y
//    produit un écart de canal énorme (les voisins n'ont rien à voir). Il dit
//    "il y a une différence", pas "elle est grave".
//  - UNI : un décalage de bord n'y change presque rien, puisque le fond sous
//    les deux positions est le même. Ce qui subsiste dessus est une VRAIE
//    erreur de texel ou de placement.
int g_flat = 0;
void fill_background(uint32_t* fb) {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t r, g, b;
            if (g_flat) { r = g = b = 128; }
            else { r = (uint8_t)(x * 7 + y * 3); g = (uint8_t)(x * 3 + y * 11); b = (uint8_t)(x ^ y); }
            fb[(size_t)y * W + x] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
}

float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

// Rasteriseur de référence : deux triangles par quad, échantillonnage au plus
// proche dans l'atlas, mélange alpha identique à blend_px. Il n'a pas à être
// rapide, il a à être HONNÊTE — même formule de mélange que le CPU, pour que
// l'écart mesuré ne vienne que de la géométrie.
void raster_quad(uint32_t* fb, const radial_menu::Quad& q, const unsigned char* atlas) {
    const int tri[2][3] = { {0,1,2}, {0,2,3} };
    for (int t = 0; t < 2; ++t) {
        const int i0 = tri[t][0], i1 = tri[t][1], i2 = tri[t][2];
        const float x0 = q.x[i0], y0 = q.y[i0], x1 = q.x[i1], y1 = q.y[i1], x2 = q.x[i2], y2 = q.y[i2];
        float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
        float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
        int bx0 = (int)floorf(minx), bx1 = (int)ceilf(maxx);
        int by0 = (int)floorf(miny), by1 = (int)ceilf(maxy);
        if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
        if (bx1 > W) bx1 = W; if (by1 > H) by1 = H;
        const float area = edge(x0, y0, x1, y1, x2, y2);
        if (fabsf(area) < 1e-6f) continue;
        for (int y = by0; y < by1; ++y)
            for (int x = bx0; x < bx1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                float w0 = edge(x1, y1, x2, y2, px, py) / area;
                float w1 = edge(x2, y2, x0, y0, px, py) / area;
                float w2 = edge(x0, y0, x1, y1, px, py) / area;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float u = w0 * q.u[i0] + w1 * q.u[i1] + w2 * q.u[i2];
                const float v = w0 * q.v[i0] + w1 * q.v[i1] + w2 * q.v[i2];
                int tx = (int)(u * radial_menu::RM_ATLAS_DIM);
                int ty = (int)(v * radial_menu::RM_ATLAS_DIM);
                if (tx < 0) tx = 0; if (tx >= radial_menu::RM_ATLAS_DIM) tx = radial_menu::RM_ATLAS_DIM - 1;
                if (ty < 0) ty = 0; if (ty >= radial_menu::RM_ATLAS_DIM) ty = radial_menu::RM_ATLAS_DIM - 1;
                const unsigned char* p = atlas + ((size_t)ty * radial_menu::RM_ATLAS_DIM + tx) * 4;
                radial_menu::draw_detail::blend_px(fb, W, H, x, y, p[0], p[1], p[2], p[3]);
            }
    }
}
} // namespace

int main() {
    static unsigned char atlas[radial_menu::RM_ATLAS_DIM * radial_menu::RM_ATLAS_DIM * 4];
    radial_menu::fill_atlas(atlas);

    // 1) l'atlas contient-il EXACTEMENT les sprites d'origine ?
    int bad = 0;
    auto check = [&](const unsigned char* src, int sw, int sh, radial_menu::AtlasRect a, const char* nm) {
        for (int y = 0; y < sh && bad < 5; ++y)
            for (int x = 0; x < sw * 4; ++x)
                if (atlas[((size_t)(a.y + y) * radial_menu::RM_ATLAS_DIM + a.x) * 4 + x] !=
                    src[((size_t)y * sw) * 4 + x]) {
                    std::printf("atlas: %s DIFFERE en ligne %d\n", nm, y); ++bad; break; }
    };
    check(g_rm_wedge_normal, RM_WEDGE_W, RM_WEDGE_H, radial_menu::atlas_wedge(false), "wedge_normal");
    check(g_rm_wedge_glow,   RM_WEDGE_W, RM_WEDGE_H, radial_menu::atlas_wedge(true),  "wedge_glow");
    for (int i = 0; i < RM_N; ++i) {
        char nm[24]; std::snprintf(nm, sizeof nm, "icone %d", i);
        check(g_rm_slots[i].icon, RM_ICON_SIZE, RM_ICON_SIZE, radial_menu::atlas_icon(i), nm);
    }
    std::printf("atlas: %s\n", bad ? "INCORRECT" : "conforme aux sprites d'origine");

    // 2) le rendu par quads colle-t-il au rendu CPU, pour chaque état ?
    uint32_t* a = (uint32_t*)std::malloc((size_t)W * H * 4);
    uint32_t* b = (uint32_t*)std::malloc((size_t)W * H * 4);
  for (g_flat = 0; g_flat <= 1; ++g_flat) {
    std::printf("\n--- fond %s ---\n", g_flat ? "UNI (gris 128)" : "BRUITE");
    int worst_pct = 0, worst_max = 0;
    for (int st = 0; st <= RM_N; ++st) {
        radial_menu::State s{1, st ? 1 : 0, st ? st - 1 : 0};
        fill_background(a); radial_menu::draw(s, a, W, H);
        fill_background(b);
        radial_menu::Quad q[2 * RM_N];
        const int n = radial_menu::build_quads(s, W, H, q, 2 * RM_N);
        for (int k = 0; k < n; ++k) raster_quad(b, q[k], atlas);

        long diff = 0, touched = 0, maxdev = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            const uint32_t va = a[i], vb = b[i];
            if (va != vb) {
                ++diff;
                for (int c = 0; c < 3; ++c) {
                    long d = labs((long)((va >> (8*c)) & 0xFF) - (long)((vb >> (8*c)) & 0xFF));
                    if (d > maxdev) maxdev = d;
                }
            }
        }
        // pixels réellement couverts par le menu = ceux que le CPU a changés
        uint32_t* bg = (uint32_t*)std::malloc((size_t)W * H * 4);
        fill_background(bg);
        for (size_t i = 0; i < (size_t)W * H; ++i) if (a[i] != bg[i]) ++touched;
        std::free(bg);
        // RM_DUMP=1 : sort les deux images brutes (RGBA 960x544) pour les
        // regarder cote a cote. C'est ce qui a montre que les 7 181 pixels
        // faux du premier jet etaient les ICONES, pas les quartiers — un
        // pourcentage ne dit jamais OU.
        if (st == 3 && getenv("RM_DUMP")) {
            FILE* fa = fopen("/tmp/rm_cpu.raw", "wb"); if (fa) { fwrite(a, 4, (size_t)W * H, fa); fclose(fa); }
            FILE* fb2 = fopen("/tmp/rm_gpu.raw", "wb"); if (fb2) { fwrite(b, 4, (size_t)W * H, fb2); fclose(fb2); }
        }
        const int pct = touched ? (int)(diff * 100 / touched) : 0;
        if (pct > worst_pct) worst_pct = pct;
        if (maxdev > worst_max) worst_max = (int)maxdev;
        if (st == 0 || st == 3)
            std::printf("  etat %d: %ld pixels dessines | %ld differents (%d%%) | ecart max %ld\n",
                        st, touched, diff, pct, maxdev);
    }
    std::printf("  PIRE CAS: %d%% de pixels differents, ecart max %d\n", worst_pct, worst_max);
  }
    std::free(a); std::free(b);
    return bad ? 1 : 0;
}
