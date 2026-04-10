// ================================================================
// Parametric terrain generation — "Parametric Disneyland"
//
// Each biome gets a unique seed-derived:
//   - Texture pattern (10 algorithms)
//   - Color palette (HSV golden-angle separation)
//   - Terrain topology modifiers (amplitude, terracing, ridges)
//
// Combinatorial space: ~10^60+ unique worlds per seed.
// ================================================================
#include "terrain.h"
#include "types.h"
#include "variance.h"
#include "../shared/constants.h"
#include "math.h"

#include <cmath>
#include <cstdio>
#include <algorithm>

extern VarianceSampler g_variance;
extern uint32_t g_world_seed;

// ================================================================
// Deterministic parameter derivation from seed
// ================================================================

static uint32_t phash(uint32_t seed, int key, uint32_t salt) {
    uint32_t h = seed ^ ((uint32_t)(key + 10000) * 2654435761u) ^ (salt * 374761393u);
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return h;
}

static float pf(uint32_t seed, int key, uint32_t salt) {
    return (float)(phash(seed, key, salt) & 0xFFFFFF) / (float)0x1000000;
}

static int pi(uint32_t seed, int key, uint32_t salt, int n) {
    return (int)(phash(seed, key, salt) % (uint32_t)n);
}

// ================================================================
// HSV → RGB
// ================================================================

static void hsv_rgb(float h, float s, float v, float& r, float& g, float& b) {
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f);
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    if      (h <  60) { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    r += m; g += m; b += m;
}

// ================================================================
// Per-biome parametric appearance (10 unique biomes per seed)
// ================================================================

struct BC { float r, g, b; };

struct BiomeApp {
    int pattern;            // texture algorithm [0,9]
    BC col[4];              // palette: base, detail1, detail2, accent
    float hue;              // primary hue for reference
    float pscale;           // pattern frequency scale
    float pangle;           // directional pattern angle
    float contrast;         // color contrast multiplier
    float t_amp, t_freq;    // terrain amplitude & frequency modifiers
    float t_terrace, t_ridge, t_elev;  // terrain shaping
};

static BiomeApp g_ba[NUM_BIOMES];
static bool g_ba_init = false;

static BC mkc(float h, float s, float v) {
    BC c; hsv_rgb(h, s, v, c.r, c.g, c.b); return c;
}

static void init_ba() {
    if (g_ba_init) return;
    g_ba_init = true;

    // Seed-derived global hue rotation — shifts entire palette
    float base_rot = pf(g_world_seed, -1, 0) * 360.0f;

    for (int b = 0; b < NUM_BIOMES; b++) {
        BiomeApp& a = g_ba[b];

        // Pattern selection — each biome gets a different texture algorithm
        a.pattern = pi(g_world_seed, b, 100, 10);

        // Color palette — golden angle (137.5°) ensures max hue separation
        float hue = fmodf(base_rot + b * 137.508f + pf(g_world_seed, b, 200) * 25.0f, 360.0f);
        a.hue = hue;
        float sat = 0.25f + pf(g_world_seed, b, 201) * 0.55f;
        float val = 0.25f + pf(g_world_seed, b, 202) * 0.50f;
        float spr = 15.0f + pf(g_world_seed, b, 203) * 45.0f;

        a.col[0] = mkc(hue, sat, val);
        a.col[1] = mkc(hue + spr, sat * 0.8f, std::min(val * 1.15f, 0.95f));
        a.col[2] = mkc(hue - spr * 0.7f, std::min(sat * 1.2f, 0.95f), val * 0.85f);
        a.col[3] = mkc(hue + 60.0f + pf(g_world_seed, b, 204) * 120.0f,
                        0.4f + pf(g_world_seed, b, 205) * 0.5f,
                        0.5f + pf(g_world_seed, b, 206) * 0.4f);

        // Pattern modifiers
        a.pscale   = 0.6f + pf(g_world_seed, b, 300) * 0.8f;
        a.pangle   = pf(g_world_seed, b, 301) * 6.2831853f;
        a.contrast = 0.8f + pf(g_world_seed, b, 303) * 0.4f;

        // Terrain topology modifiers (per-biome)
        a.t_amp     = 0.5f + pf(g_world_seed, b, 400) * 1.5f;
        a.t_freq    = 0.6f + pf(g_world_seed, b, 401) * 0.8f;
        a.t_terrace = pf(g_world_seed, b, 402);
        a.t_ridge   = pf(g_world_seed, b, 403);
        a.t_elev    = -3.0f + pf(g_world_seed, b, 404) * 12.0f;
    }
}

// ================================================================
// Biome palette export (for structures/plants)
// ================================================================

void get_biome_palette(int biome, float colors[4][3]) {
    init_ba();
    for (int i = 0; i < 4; i++) {
        colors[i][0] = g_ba[biome].col[i].r;
        colors[i][1] = g_ba[biome].col[i].g;
        colors[i][2] = g_ba[biome].col[i].b;
    }
}

float get_biome_hue(int biome) {
    init_ba();
    return g_ba[biome].hue;
}

// ================================================================
// Biome selection — uses variance channel 3 for huge coherent regions
// ================================================================

int biome_at(float wx, float wz) {
    double v = g_variance.sample_channel(wx, 0, wz, 3);
    int biome = (int)(remap(v, -1.0, 1.0, 0.0, (double)NUM_BIOMES - 0.001));
    return std::clamp(biome, 0, NUM_BIOMES - 1);
}

// ================================================================
// 10 texture patterns — each uses the biome's seed-derived palette
// ================================================================

// Color lerp
static BC lrp(BC a, BC b, double t) {
    return {(float)(a.r + (b.r - a.r) * t),
            (float)(a.g + (b.g - a.g) * t),
            (float)(a.b + (b.b - a.b) * t)};
}

// 0: Smooth organic FBM — flowing natural ground
static void pat_organic(float wx, float wz, uint32_t ps, const BiomeApp& a,
                        double& r, double& g, double& b) {
    float sc = 25.0f * a.pscale;
    double n1 = remap(fbm3(wx, 0, wz, ps, 5, sc), -1, 1, 0, 1);
    double n2 = remap(fbm3(wx, 0, wz, ps + 1000, 4, sc * 1.5), -1, 1, 0, 1);
    double n3 = remap(fbm3(wx, 0, wz, ps + 2000, 6, sc * 0.5), -1, 1, 0, 1);
    r = a.col[0].r * (0.6 + 0.4 * n1) + a.col[1].r * n2 * 0.3 + a.col[2].r * n3 * 0.15;
    g = a.col[0].g * (0.6 + 0.4 * n1) + a.col[1].g * n2 * 0.3 + a.col[2].g * n3 * 0.15;
    b = a.col[0].b * (0.6 + 0.4 * n1) + a.col[1].b * n2 * 0.3 + a.col[2].b * n3 * 0.15;
}

// 1: Cellular/Voronoi — cobblestone paths, flagstone
static void pat_cellular(float wx, float wz, uint32_t ps, const BiomeApp& a,
                         double& r, double& g, double& b) {
    float sc = 8.0f * a.pscale;
    float gx = floorf(wx / sc), gz = floorf(wz / sc);
    float fx = wx / sc - gx, fz = wz / sc - gz;
    float min_d = 999.0f;
    int best = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            uint32_t ch = phash(ps, (int)(gx + dx) * 7919 + (int)(gz + dy), 555);
            float px = (float)(ch & 0xFF) / 255.0f;
            float pz = (float)((ch >> 8) & 0xFF) / 255.0f;
            float ddx = dx + px - fx, ddz = dy + pz - fz;
            float d = ddx * ddx + ddz * ddz;
            if (d < min_d) { min_d = d; best = ch & 3; }
        }
    float edge = sqrtf(min_d) < 0.08f ? 0.5f : 1.0f;
    double det = remap(fbm3(wx, 0, wz, ps + 3000, 4, 6.0 * a.pscale), -1, 1, 0.85, 1.0);
    r = a.col[best].r * edge * det;
    g = a.col[best].g * edge * det;
    b = a.col[best].b * edge * det;
}

// 2: Directional stripes — grooved terrain
static void pat_stripes(float wx, float wz, uint32_t ps, const BiomeApp& a,
                        double& r, double& g, double& b) {
    float ca = cosf(a.pangle), sa = sinf(a.pangle);
    float u = wx * ca + wz * sa;
    double warp = fbm3(wx, 0, wz, ps, 3, 40.0 * a.pscale) * 3.0;
    double stripe = 0.5 + 0.5 * sin((u + warp) / (5.0 * a.pscale) * 6.2831853);
    double det = remap(fbm3(wx, 0, wz, ps + 1000, 5, 10.0 * a.pscale), -1, 1, 0.8, 1.0);
    BC c = lrp(a.col[0], a.col[1], stripe);
    r = c.r * det; g = c.g * det; b = c.b * det;
}

// 3: Tile grid — warped masonry
static void pat_tiles(float wx, float wz, uint32_t ps, const BiomeApp& a,
                      double& r, double& g, double& b) {
    float tw = 4.0f * a.pscale, th = 3.5f * a.pscale;
    double w1 = fbm3(wx, 0, wz, ps, 3, 80.0 * a.pscale) * 10.0;
    double w2 = fbm3(wx, 0, wz, ps + 500, 3, 80.0 * a.pscale) * 10.0;
    float sx = fmodf(wx + (float)w1 + 5000.0f, tw) / tw;
    float sz = fmodf(wz + (float)w2 + 5000.0f, th) / th;
    bool gap = (sx < 0.03f || sx > 0.97f || sz < 0.03f || sz > 0.97f);
    int tid = (int)(floorf((wx + (float)w1) / tw) * 13 + floorf((wz + (float)w2) / th) * 7);
    int ci = ((uint32_t)tid ^ ps) % 3;
    double vein = remap(fbm3(wx, 0, wz, ps + 1000, 5, 15.0 * a.pscale), -1, 1, 0, 1);
    if (gap) {
        r = a.col[3].r * 0.25; g = a.col[3].g * 0.25; b = a.col[3].b * 0.25;
    } else {
        r = (a.col[ci].r + 0.05 * vein) * 0.95;
        g = (a.col[ci].g + 0.05 * vein) * 0.95;
        b = (a.col[ci].b + 0.05 * vein) * 0.95;
    }
}

// 4: Strata layers — horizontal banding
static void pat_strata(float wx, float wz, uint32_t ps, const BiomeApp& a,
                       double& r, double& g, double& b) {
    float ca = cosf(a.pangle), sa = sinf(a.pangle);
    float u = wx * ca * 0.15f + wz * sa * 0.15f;
    double warp = fbm3(wx, 0, wz, ps, 4, 100.0 * a.pscale) * 4.0;
    float layer = fmodf((u + (float)warp) * a.pscale + 1000.0f, 4.0f);
    int li = std::clamp((int)layer, 0, 3);
    float frac = layer - floorf(layer);
    float edge = (frac < 0.05f || frac > 0.95f) ? 0.7f : 1.0f;
    double det = remap(fbm3(wx, 0, wz, ps + 2000, 5, 8.0 * a.pscale), -1, 1, 0.85, 1.0);
    r = a.col[li].r * edge * det;
    g = a.col[li].g * edge * det;
    b = a.col[li].b * edge * det;
}

// 5: Spotted — speckled dots on smooth base
static void pat_spotted(float wx, float wz, uint32_t ps, const BiomeApp& a,
                        double& r, double& g, double& b) {
    double base_n = remap(fbm3(wx, 0, wz, ps, 4, 30.0 * a.pscale), -1, 1, 0, 1);
    double spots = remap(fbm3(wx, 0, wz, ps + 1000, 6, 6.0 * a.pscale), -1, 1, 0, 1);
    spots = spots > 0.7 ? 1.0 : 0.0;
    double fine = remap(fbm3(wx, 0, wz, ps + 2000, 5, 3.0 * a.pscale), -1, 1, 0, 1);
    fine = fine > 0.85 ? 1.0 : 0.0;
    r = a.col[0].r * (0.7 + 0.3 * base_n) + a.col[1].r * spots * 0.3 + a.col[3].r * fine * 0.15;
    g = a.col[0].g * (0.7 + 0.3 * base_n) + a.col[1].g * spots * 0.3 + a.col[3].g * fine * 0.15;
    b = a.col[0].b * (0.7 + 0.3 * base_n) + a.col[1].b * spots * 0.3 + a.col[3].b * fine * 0.15;
}

// 6: Cracked — dry ridge lines over smooth base
static void pat_cracked(float wx, float wz, uint32_t ps, const BiomeApp& a,
                        double& r, double& g, double& b) {
    double base = remap(fbm3(wx, 0, wz, ps, 4, 25.0 * a.pscale), -1, 1, 0.5, 1.0);
    double crack_raw = fbm3(wx, 0, wz, ps + 1000, 5, 40.0 * a.pscale);
    double crack = 1.0 - 2.0 * fabs(crack_raw);
    crack = std::max(0.0, crack); crack = crack * crack * crack;
    double ox = remap(fbm3(wx, 0, wz, ps + 2000, 3, 60.0 * a.pscale), -1, 1, 0, 1);
    r = a.col[0].r * base + a.col[2].r * ox * 0.25 - crack * 0.3;
    g = a.col[0].g * base + a.col[2].g * ox * 0.25 - crack * 0.25;
    b = a.col[0].b * base + a.col[2].b * ox * 0.25 - crack * 0.15;
}

// 7: Swirl — mystical spiraling patterns
static void pat_swirl(float wx, float wz, uint32_t ps, const BiomeApp& a,
                      double& r, double& g, double& b) {
    double warp = fbm3(wx, 0, wz, ps, 5, 60.0 * a.pscale) * 6.0;
    double sw = 0.5 + 0.5 * sin(wx * 0.08 * a.pscale + wz * 0.06 * a.pscale + warp);
    double sparkle = remap(fbm3(wx, 0, wz, ps + 1000, 7, 4.0 * a.pscale), -1, 1, 0, 1);
    sparkle = sparkle > 0.92 ? 1.0 : 0.0;
    double cob = remap(fbm3(wx, 0, wz, ps + 2000, 4, 15.0 * a.pscale), -1, 1, 0.6, 1.0);
    BC c = lrp(a.col[0], a.col[1], sw);
    r = c.r * cob + a.col[3].r * sparkle * 0.4;
    g = c.g * cob + a.col[3].g * sparkle * 0.4;
    b = c.b * cob + a.col[3].b * sparkle * 0.4;
}

// 8: Mosaic — bold quantized color patches
static void pat_mosaic(float wx, float wz, uint32_t ps, const BiomeApp& a,
                       double& r, double& g, double& b) {
    double n = fbm3(wx, 0, wz, ps, 3, 40.0 * a.pscale);
    int patch = std::clamp((int)remap(n, -1, 1, 0, 3.999), 0, 3);
    BC c = a.col[patch];
    double brt = remap(fbm3(wx, 0, wz, ps + 1000, 3, 35.0 * a.pscale), -1, 1, 0, 1);
    brt = floorf(brt * 4.0) / 3.0;
    double ol_raw = fbm3(wx, 0, wz, ps + 2000, 5, 25.0 * a.pscale);
    double ol = 1.0 - 2.0 * fabs(ol_raw);
    ol = ol > 0.7 ? 0.3 : 0.0;
    float mix = 0.7f + 0.3f * (float)brt;
    r = c.r * mix - ol; g = c.g * mix - ol; b = c.b * mix - ol;
}

// 9: Woven — textile cross-hatch
static void pat_woven(float wx, float wz, uint32_t ps, const BiomeApp& a,
                      double& r, double& g, double& b) {
    float ca = cosf(a.pangle), sa = sinf(a.pangle);
    float u = wx * ca + wz * sa, v = -wx * sa + wz * ca;
    float sc = 3.0f * a.pscale;
    double wp = fbm3(wx, 0, wz, ps, 2, 50.0 * a.pscale) * 0.5;
    double tu = 0.5 + 0.5 * sin((u + wp) * 2.0 / sc);
    double tv = 0.5 + 0.5 * sin((v + wp) * 2.0 / sc);
    double weave = tu * tv, cross = tu * (1.0 - tv) + (1.0 - tu) * tv;
    double det = remap(fbm3(wx, 0, wz, ps + 1000, 4, 8.0 * a.pscale), -1, 1, 0.85, 1.0);
    r = (a.col[0].r * weave + a.col[1].r * cross) * det;
    g = (a.col[0].g * weave + a.col[1].g * cross) * det;
    b = (a.col[0].b * weave + a.col[1].b * cross) * det;
}

// ================================================================
// Biome color dispatch
// ================================================================

typedef void (*PatFn)(float, float, uint32_t, const BiomeApp&, double&, double&, double&);
static const PatFn g_patterns[] = {
    pat_organic, pat_cellular, pat_stripes, pat_tiles, pat_strata,
    pat_spotted, pat_cracked,  pat_swirl,   pat_mosaic, pat_woven
};

static void biome_color(float wx, float wz, int biome, uint32_t tex_seed,
                        double& r, double& g, double& b) {
    init_ba();
    const BiomeApp& a = g_ba[biome];
    uint32_t ps = tex_seed + biome * 99991u;
    g_patterns[a.pattern](wx, wz, ps, a, r, g, b);
    r *= a.contrast; g *= a.contrast; b *= a.contrast;
}

// ================================================================
// Terrain height (seed-parametric with biome modifiers)
// ================================================================

float terrain_height(float wx, float wz) {
    init_ba();

    // Seed-derived global terrain character ranges
    float amp_lo  = 2.0f  + pf(g_world_seed, -1, 500) * 6.0f;
    float amp_hi  = 30.0f + pf(g_world_seed, -1, 501) * 40.0f;
    float freq_lo = 0.001f + pf(g_world_seed, -1, 502) * 0.002f;
    float freq_hi = 0.008f + pf(g_world_seed, -1, 503) * 0.010f;
    float elev_lo = -4.0f + pf(g_world_seed, -1, 504) * 4.0f;
    float elev_hi = 8.0f  + pf(g_world_seed, -1, 505) * 15.0f;
    float shp_lo  = 1.0f  + pf(g_world_seed, -1, 506) * 0.5f;
    float shp_hi  = 2.0f  + pf(g_world_seed, -1, 507) * 2.5f;

    double v_amp   = g_variance.sample_channel(wx, 0, wz, 0);
    double v_rough = g_variance.sample_channel(wx, 0, wz, 1);
    double v_freq  = g_variance.sample_channel(wx, 0, wz, 2);
    double v_elev  = g_variance.sample_channel(wx, 0, wz, 6);
    double v_shape = g_variance.sample_channel(wx, 0, wz, 7);

    float amplitude = (float)remap(v_amp,   -1, 1, amp_lo, amp_hi);
    float base_elev = (float)remap(v_elev,  -1, 1, elev_lo, elev_hi);
    float freq      = (float)remap(v_freq,  -1, 1, freq_lo, freq_hi);
    int   octaves   = (int)remap(v_rough,   -1, 1, 1.0, 5.0);
    float shape_exp = (float)remap(v_shape, -1, 1, shp_lo, shp_hi);
    float persist   = (float)remap(v_rough, -1, 1, 0.05, 0.35);
    float warp_str  = (float)remap(v_rough, -1, 1, 0.0, 0.12);

    // Per-biome terrain modification
    int biome = biome_at(wx, wz);
    const BiomeApp& ba = g_ba[biome];
    amplitude *= ba.t_amp;
    freq *= ba.t_freq;
    base_elev += ba.t_elev;

    float raw = (float)fbm_simplex_derived(wx, wz, g_variance.seed, octaves,
                                            1.0 / (double)freq, 2.0, persist, warp_str);
    float norm = (raw + 1.0f) * 0.5f;
    float shaped = powf(std::clamp(norm, 0.0f, 1.0f), shape_exp) * 2.0f - 1.0f;
    float h = shaped * amplitude + base_elev;

    // Biome-specific terracing
    if (ba.t_terrace > 0.15f) {
        float step = 4.0f;
        float level = floorf(h / step) * step;
        float frac = (h - level) / step;
        float t = frac < 0.08f ? frac / 0.08f : 1.0f;
        t = t * t * (3.0f - 2.0f * t);
        h = h * (1.0f - ba.t_terrace) + (level + t * step) * ba.t_terrace;
    }

    // Ridge emphasis
    if (ba.t_ridge > 0.15f) {
        float rn = (float)fbm3(wx, 0, wz, g_variance.seed + 9999, 4, 200.0);
        float ridge = 1.0f - 2.0f * fabsf(rn);
        ridge = std::max(0.0f, ridge);
        h += ridge * ba.t_ridge * amplitude * 0.4f;
    }

    return std::max(h, 1.0f);
}

// ================================================================
// Procedural terrain texture (biome-driven)
// ================================================================

std::vector<unsigned char> generate_terrain_texture(int size) {
    std::vector<unsigned char> pixels(size * size * 4);
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    uint32_t tex_seed = g_world_seed * 2654435761u;

    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float wx = (float)x / (size - 1) * 2.0f * half - half;
            float wz = (float)y / (size - 1) * 2.0f * half - half;
            int biome = biome_at(wx, wz);
            double r = 0, g = 0, b = 0;
            biome_color(wx, wz, biome, tex_seed, r, g, b);
            unsigned char* p = &pixels[(y * size + x) * 4];
            p[0] = (unsigned char)(std::clamp(r, 0.0, 1.0) * 255);
            p[1] = (unsigned char)(std::clamp(g, 0.0, 1.0) * 255);
            p[2] = (unsigned char)(std::clamp(b, 0.0, 1.0) * 255);
            p[3] = 255;
        }
    printf("Terrain texture: %dx%d generated (%d biomes, %d patterns)\n",
           size, size, NUM_BIOMES, 10);
    return pixels;
}

// ================================================================
// Mesh generation
// ================================================================

Mesh generate_terrain() {
    Mesh mesh;
    int N = GRID_SIZE;
    float half = (N - 1) * GRID_SCALE * 0.5f;
    uint32_t tex_seed = g_world_seed * 2654435761u;

    mesh.vertices.resize(N * N);
    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float wx = x * GRID_SCALE - half, wz = z * GRID_SCALE - half;
            Vertex& v = mesh.vertices[z * N + x];
            v.px = wx; v.py = terrain_height(wx, wz); v.pz = wz;
            v.nx = 0; v.ny = 1; v.nz = 0;
            int biome = biome_at(wx, wz);
            double r = 0, g = 0, b = 0;
            biome_color(wx, wz, biome, tex_seed, r, g, b);
            v.cr = (float)std::clamp(r, 0.0, 1.0);
            v.cg = (float)std::clamp(g, 0.0, 1.0);
            v.cb = (float)std::clamp(b, 0.0, 1.0);
        }

    // Compute normals from height differences
    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float h  = mesh.vertices[z * N + x].py;
            float hx = (x < N - 1) ? mesh.vertices[z * N + x + 1].py : h;
            float hz = (z < N - 1) ? mesh.vertices[(z + 1) * N + x].py : h;
            float nx = h - hx, ny = GRID_SCALE, nz = h - hz;
            float l = sqrtf(nx * nx + ny * ny + nz * nz);
            Vertex& v = mesh.vertices[z * N + x];
            v.nx = nx / l; v.ny = ny / l; v.nz = nz / l;
        }

    // Triangle indices
    mesh.indices.reserve((N - 1) * (N - 1) * 6);
    for (int z = 0; z < N - 1; z++)
        for (int x = 0; x < N - 1; x++) {
            uint32_t tl = z * N + x, tr = tl + 1, bl = (z + 1) * N + x, br = bl + 1;
            mesh.indices.push_back(tl); mesh.indices.push_back(bl); mesh.indices.push_back(tr);
            mesh.indices.push_back(tr); mesh.indices.push_back(bl); mesh.indices.push_back(br);
        }

    printf("Terrain: %d verts, %d tris\n",
           (int)mesh.vertices.size(), (int)mesh.indices.size() / 3);
    return mesh;
}
