// ================================================================
// Parametric structure and L-system plant generation
//
// Structures: 8 architectural motifs, shuffled per-seed so each
//   biome gets a unique building style. All proportions, features,
//   and colors are seed-derived from the biome palette.
//
// Plants: 3 parametric L-system species per biome (30 total),
//   each with seed-derived branching rules, colors, and proportions.
// ================================================================
#include "structures.h"
#include "types.h"
#include "vec3.h"
#include "variance.h"
#include "terrain.h"
#include "../shared/constants.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <random>
#include <stack>
#include <map>
#include <string>

extern VarianceSampler g_variance;
extern uint32_t g_world_seed;

static const int MAX_LSYSTEM_LEN = 80000;

// ================================================================
// Deterministic parameter derivation (same hash as terrain.cpp)
// ================================================================

static uint32_t phash(uint32_t seed, int key, uint32_t salt) {
    uint32_t h = seed ^ ((uint32_t)(key + 10000) * 2654435761u) ^ (salt * 374761393u);
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    return h;
}

static float spf(uint32_t seed, int key, uint32_t salt) {
    return (float)(phash(seed, key, salt) & 0xFFFFFF) / (float)0x1000000;
}

static int spi(uint32_t seed, int key, uint32_t salt, int n) {
    return (int)(phash(seed, key, salt) % (uint32_t)n);
}

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
// StructMesh methods
// ================================================================

void StructMesh::addTri(ColorVertex a, ColorVertex b, ColorVertex c) {
    uint32_t base = (uint32_t)vertices.size();
    float ex1 = b.px-a.px, ey1 = b.py-a.py, ez1 = b.pz-a.pz;
    float ex2 = c.px-a.px, ey2 = c.py-a.py, ez2 = c.pz-a.pz;
    float nx = ey1*ez2 - ez1*ey2, ny = ez1*ex2 - ex1*ez2, nz = ex1*ey2 - ey1*ex2;
    float l = sqrtf(nx*nx + ny*ny + nz*nz);
    if (l > 1e-8f) { nx/=l; ny/=l; nz/=l; }
    a.nx=nx; a.ny=ny; a.nz=nz;
    b.nx=nx; b.ny=ny; b.nz=nz;
    c.nx=nx; c.ny=ny; c.nz=nz;
    vertices.push_back(a); vertices.push_back(b); vertices.push_back(c);
    indices.push_back(base); indices.push_back(base+1); indices.push_back(base+2);
}

void StructMesh::addQuad(ColorVertex a, ColorVertex b, ColorVertex c, ColorVertex d) {
    addTri(a, b, c);
    addTri(a, c, d);
}

void StructMesh::addTriRaw(ColorVertex a, ColorVertex b, ColorVertex c) {
    uint32_t base = (uint32_t)vertices.size();
    vertices.push_back(a); vertices.push_back(b); vertices.push_back(c);
    indices.push_back(base); indices.push_back(base+1); indices.push_back(base+2);
}

void StructMesh::addBox(float cx, float cy, float cz, float hx, float hy, float hz,
                        float cr, float cg, float cb) {
    auto v = [&](float x, float y, float z) -> ColorVertex {
        return {cx+x, cy+y, cz+z, 0,0,0, cr, cg, cb};
    };
    addQuad(v(-hx,-hy, hz), v( hx,-hy, hz), v( hx, hy, hz), v(-hx, hy, hz));
    addQuad(v( hx,-hy,-hz), v(-hx,-hy,-hz), v(-hx, hy,-hz), v( hx, hy,-hz));
    addQuad(v( hx,-hy, hz), v( hx,-hy,-hz), v( hx, hy,-hz), v( hx, hy, hz));
    addQuad(v(-hx,-hy,-hz), v(-hx,-hy, hz), v(-hx, hy, hz), v(-hx, hy,-hz));
    addQuad(v(-hx, hy, hz), v( hx, hy, hz), v( hx, hy,-hz), v(-hx, hy,-hz));
    addQuad(v(-hx,-hy,-hz), v( hx,-hy,-hz), v( hx,-hy, hz), v(-hx,-hy, hz));
}

void StructMesh::addCylinder(float cx, float cy, float cz, float r, float hh, int segs,
                             float cr, float cg, float cb) {
    float pi2 = 6.2831853f;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i / segs * pi2, a1 = (float)(i+1) / segs * pi2;
        float x0 = cosf(a0)*r, z0 = sinf(a0)*r;
        float x1 = cosf(a1)*r, z1 = sinf(a1)*r;
        ColorVertex bl = {cx+x0, cy-hh, cz+z0, 0,0,0, cr,cg,cb};
        ColorVertex br = {cx+x1, cy-hh, cz+z1, 0,0,0, cr,cg,cb};
        ColorVertex tr = {cx+x1, cy+hh, cz+z1, 0,0,0, cr,cg,cb};
        ColorVertex tl = {cx+x0, cy+hh, cz+z0, 0,0,0, cr,cg,cb};
        addQuad(tl, tr, br, bl);
        ColorVertex cen_t = {cx, cy+hh, cz, 0,0,0, cr*0.9f,cg*0.9f,cb*0.9f};
        addTri(cen_t, tr, tl);
        ColorVertex cen_b = {cx, cy-hh, cz, 0,0,0, cr*0.9f,cg*0.9f,cb*0.9f};
        addTri(cen_b, bl, br);
    }
}

void StructMesh::addCone(float cx, float cy, float cz, float r_bot, float r_top, float hh, int segs,
                         float cr, float cg, float cb) {
    float pi2 = 6.2831853f;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i / segs * pi2, a1 = (float)(i+1) / segs * pi2;
        float bx0 = cosf(a0)*r_bot, bz0 = sinf(a0)*r_bot;
        float bx1 = cosf(a1)*r_bot, bz1 = sinf(a1)*r_bot;
        float tx0 = cosf(a0)*r_top, tz0 = sinf(a0)*r_top;
        float tx1 = cosf(a1)*r_top, tz1 = sinf(a1)*r_top;
        ColorVertex bl = {cx+bx0, cy-hh, cz+bz0, 0,0,0, cr,cg,cb};
        ColorVertex br = {cx+bx1, cy-hh, cz+bz1, 0,0,0, cr,cg,cb};
        ColorVertex tr_ = {cx+tx1, cy+hh, cz+tz1, 0,0,0, cr,cg,cb};
        ColorVertex tl = {cx+tx0, cy+hh, cz+tz0, 0,0,0, cr,cg,cb};
        addQuad(tl, tr_, br, bl);
    }
}

void StructMesh::addRoof(float cx, float cy, float cz, float hw, float hd, float ph,
                         float cr, float cg, float cb) {
    auto v = [&](float x, float y, float z) -> ColorVertex {
        return {cx+x, cy+y, cz+z, 0,0,0, cr,cg,cb};
    };
    addQuad(v(-hw, 0, hd), v( hw, 0, hd), v( hw, ph, 0), v(-hw, ph, 0));
    addQuad(v( hw, 0,-hd), v(-hw, 0,-hd), v(-hw, ph, 0), v( hw, ph, 0));
    addTri(v(-hw, 0, hd), v(-hw, ph, 0), v(-hw, 0,-hd));
    addTri(v( hw, 0,-hd), v( hw, ph, 0), v( hw, 0, hd));
}

void StructMesh::merge(const StructMesh& other, float ox, float oy, float oz, float rotY) {
    uint32_t base = (uint32_t)vertices.size();
    float c = cosf(rotY), s = sinf(rotY);
    for (auto v : other.vertices) {
        float x = v.px, z = v.pz;
        v.px = x * c - z * s + ox;
        v.pz = x * s + z * c + oz;
        v.py += oy;
        float nnx = v.nx * c - v.nz * s;
        float nnz = v.nx * s + v.nz * c;
        v.nx = nnx; v.nz = nnz;
        vertices.push_back(v);
    }
    for (auto i : other.indices) indices.push_back(i + base);
}

// ================================================================
// Parametric structure archetype (seed-derived per biome)
// ================================================================

struct SArch {
    int motif;          // 0-7: tower, temple, pyramid, arch, cabin, obelisk, lighthouse, pavilion
    float w, h;         // base width/height proportions
    int sections;       // body section count
    float taper;        // section shrink factor
    int roof;           // 0=flat, 1=pitched, 2=conical, 3=dome, 4=stepped
    float roof_h;       // roof height
    bool pillars;
    int pillar_n;
    bool entrance, spire, railing, windows;
    float spire_h;
    float wr, wg, wb;   // wall color
    float rr, rg, rb;   // roof color
    float ar, ag, ab;    // accent color
    float dr, dg, db;    // detail color
};

static SArch g_sarch[NUM_BIOMES];
static bool g_sarch_init = false;

static void init_sarch() {
    if (g_sarch_init) return;
    g_sarch_init = true;

    // Shuffle motifs for maximum biome distinction
    int motifs[NUM_BIOMES];
    for (int i = 0; i < NUM_BIOMES; i++) motifs[i] = i % 8;
    uint32_t s = g_world_seed * 1664525u + 12345u;
    for (int i = NUM_BIOMES - 1; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        int j = s % (i + 1);
        int tmp = motifs[i]; motifs[i] = motifs[j]; motifs[j] = tmp;
    }

    for (int b = 0; b < NUM_BIOMES; b++) {
        SArch& a = g_sarch[b];
        a.motif    = motifs[b];
        a.w        = 1.5f + spf(g_world_seed, b, 1100) * 3.0f;
        a.h        = 1.5f + spf(g_world_seed, b, 1101) * 3.5f;
        a.sections = 1 + spi(g_world_seed, b, 1102, 4);
        a.taper    = 0.7f + spf(g_world_seed, b, 1103) * 0.25f;
        a.roof     = spi(g_world_seed, b, 1104, 5);
        a.roof_h   = 0.5f + spf(g_world_seed, b, 1105) * 2.5f;
        a.pillars  = spf(g_world_seed, b, 1106) > 0.4f;
        a.pillar_n = 4 + spi(g_world_seed, b, 1107, 9);
        a.entrance = spf(g_world_seed, b, 1108) > 0.3f;
        a.spire    = spf(g_world_seed, b, 1109) > 0.65f;
        a.spire_h  = 1.0f + spf(g_world_seed, b, 1110) * 3.0f;
        a.railing  = spf(g_world_seed, b, 1111) > 0.5f;
        a.windows  = spf(g_world_seed, b, 1112) > 0.4f;

        // Colors from biome palette
        float pal[4][3];
        get_biome_palette(b, pal);
        a.wr = std::clamp(pal[0][0] * 1.3f + 0.2f, 0.0f, 1.0f);
        a.wg = std::clamp(pal[0][1] * 1.3f + 0.2f, 0.0f, 1.0f);
        a.wb = std::clamp(pal[0][2] * 1.3f + 0.2f, 0.0f, 1.0f);
        a.rr = std::clamp(pal[1][0] * 0.9f + 0.1f, 0.0f, 1.0f);
        a.rg = std::clamp(pal[1][1] * 0.9f + 0.1f, 0.0f, 1.0f);
        a.rb = std::clamp(pal[1][2] * 0.9f + 0.1f, 0.0f, 1.0f);
        a.ar = pal[3][0]; a.ag = pal[3][1]; a.ab = pal[3][2];
        a.dr = pal[2][0]; a.dg = pal[2][1]; a.db = pal[2][2];
    }
}

// ================================================================
// 8 architectural motifs (parametric mesh builders)
// ================================================================

static StructMesh gen_motif_tower(const SArch& a) {
    StructMesh m;
    m.addCylinder(0, a.h*0.2f, 0, a.w*1.2f, a.h*0.2f, 12, a.dr*0.7f, a.dg*0.7f, a.db*0.7f);
    float y = a.h * 0.4f;
    float w = a.w;
    for (int s = 0; s < a.sections; s++) {
        float sh = a.h;
        float f = 1.0f - (float)s * 0.08f;
        m.addCylinder(0, y + sh, 0, w, sh, 12, a.wr*f, a.wg*f, a.wb*f);
        if (s < a.sections - 1)
            m.addCylinder(0, y + sh*2.0f + 0.12f, 0, w*1.08f, 0.12f, 12, a.ar, a.ag, a.ab);
        y += sh * 2.0f + 0.24f;
        w *= a.taper;
    }
    if (a.railing) {
        int nm = std::max(4, a.pillar_n);
        for (int i = 0; i < nm; i++) {
            float angle = (float)i / nm * 6.2831853f;
            float bx = cosf(angle) * w, bz = sinf(angle) * w;
            m.addBox(bx, y + 0.35f, bz, 0.25f, 0.35f, 0.12f, a.wr, a.wg, a.wb);
        }
    }
    if (a.roof == 2 || a.roof == 3)
        m.addCone(0, y + a.roof_h, 0, w*1.1f, 0.05f, a.roof_h, 12, a.rr, a.rg, a.rb);
    else if (a.roof == 0)
        m.addCylinder(0, y + 0.15f, 0, w*1.1f, 0.15f, 12, a.rr, a.rg, a.rb);
    else if (a.roof == 1)
        m.addRoof(0, y, 0, w*1.1f, w*1.1f, a.roof_h, a.rr, a.rg, a.rb);
    else {
        for (int i = 0; i < 3; i++) {
            float sw = w * (1.1f - i * 0.3f), sh = a.roof_h * 0.2f;
            m.addCylinder(0, y + i*sh*2.0f + sh, 0, sw, sh, 12,
                          a.rr*(1.0f-i*0.1f), a.rg*(1.0f-i*0.1f), a.rb*(1.0f-i*0.1f));
        }
    }
    if (a.entrance)
        m.addBox(0, a.h*0.4f + 0.7f, a.w + 0.02f, 0.35f, 0.7f, 0.06f,
                 a.dr*0.5f, a.dg*0.5f, a.db*0.5f);
    if (a.spire)
        m.addCone(0, y + a.roof_h + a.spire_h, 0, 0.15f, 0.03f, a.spire_h, 6, a.ar, a.ag, a.ab);
    return m;
}

static StructMesh gen_motif_temple(const SArch& a) {
    StructMesh m;
    for (int i = 0; i < 3; i++) {
        float pw = a.w * (1.4f - i * 0.15f), ph = 0.2f;
        m.addBox(0, i*0.4f + ph, 0, pw, ph, pw*0.7f,
                 a.wr*(1.0f-i*0.03f), a.wg*(1.0f-i*0.03f), a.wb*(1.0f-i*0.03f));
    }
    float base = 1.2f;
    m.addBox(0, base + 0.08f, 0, a.w*1.2f, 0.08f, a.w*0.65f, a.wr, a.wg, a.wb);
    int nc = std::max(4, a.pillar_n);
    float colH = a.h * 1.2f, colR = 0.18f + a.w * 0.02f;
    float spacing = a.w * 2.0f / (nc - 1);
    for (int i = 0; i < nc; i++) {
        float x = -a.w + i * spacing;
        m.addCylinder(x, base + colH,  a.w*0.55f, colR, colH, 8, a.wr*0.97f, a.wg*0.95f, a.wb*0.93f);
        m.addCylinder(x, base + colH, -a.w*0.55f, colR, colH, 8, a.wr*0.97f, a.wg*0.95f, a.wb*0.93f);
    }
    float entY = base + colH * 2.0f + 0.2f;
    m.addBox(0, entY, 0, a.w*1.2f, 0.2f, a.w*0.65f, a.wr*0.95f, a.wg*0.93f, a.wb*0.90f);
    if (a.roof == 1 || a.roof == 4)
        m.addRoof(0, entY + 0.2f, 0, a.w*1.2f, a.w*0.7f, a.roof_h, a.rr, a.rg, a.rb);
    else
        m.addBox(0, entY + 0.35f, 0, a.w*1.15f, 0.15f, a.w*0.6f, a.rr, a.rg, a.rb);
    float wallH = colH * 0.85f;
    m.addBox(0, base + wallH, -a.w*0.4f, a.w*0.8f, wallH, 0.15f, a.dr, a.dg, a.db);
    return m;
}

static StructMesh gen_motif_pyramid(const SArch& a) {
    StructMesh m;
    int steps = a.sections + 3;
    for (int i = 0; i < steps; i++) {
        float frac = (float)i / steps;
        float w = a.w * (2.0f - frac * 1.5f);
        float sh = a.h * 0.4f;
        float y = i * sh * 2.0f + sh;
        float f = 1.0f - frac * 0.15f;
        m.addBox(0, y, 0, w, sh, w, a.wr*f, a.wg*f, a.wb*f);
    }
    float topY = steps * a.h * 0.8f;
    if (a.pillars) {
        m.addBox(0, topY + 0.15f, 0, a.w*0.6f, 0.15f, a.w*0.6f, a.ar, a.ag, a.ab);
        float po = a.w*0.4f, pH = a.h*0.5f;
        m.addCylinder( po, topY+0.3f+pH,  po, 0.15f, pH, 8, a.wr, a.wg, a.wb);
        m.addCylinder(-po, topY+0.3f+pH,  po, 0.15f, pH, 8, a.wr, a.wg, a.wb);
        m.addCylinder( po, topY+0.3f+pH, -po, 0.15f, pH, 8, a.wr, a.wg, a.wb);
        m.addCylinder(-po, topY+0.3f+pH, -po, 0.15f, pH, 8, a.wr, a.wg, a.wb);
        m.addBox(0, topY+0.3f+pH*2.0f+0.15f, 0, a.w*0.5f, 0.15f, a.w*0.5f, a.rr, a.rg, a.rb);
    }
    if (a.entrance) {
        for (int i = 0; i < steps; i++) {
            float sh = a.h * 0.4f;
            float w = a.w * (2.0f - (float)i / steps * 1.5f);
            m.addBox(0, i * sh * 2.0f + sh, w + 0.1f, 1.0f, sh, 0.15f,
                     a.wr*0.9f, a.wg*0.9f, a.wb*0.9f);
        }
    }
    return m;
}

static StructMesh gen_motif_arch(const SArch& a) {
    StructMesh m;
    float pH = a.h * 1.5f, pW = 0.5f + a.w * 0.15f, sp = a.w * 1.5f;
    m.addBox(-sp*0.5f, pH, 0, pW, pH, pW, a.wr, a.wg, a.wb);
    m.addBox( sp*0.5f, pH, 0, pW, pH, pW, a.wr, a.wg, a.wb);
    float archR = sp * 0.5f;
    int segs = 8;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i / segs * 3.14159f, a1 = (float)(i+1) / segs * 3.14159f;
        float am = (a0 + a1) * 0.5f;
        float cx = cosf(am) * archR, cy = pH*2.0f + sinf(am) * archR;
        float bw = archR * 3.14159f / segs * 0.55f;
        m.addBox(cx, cy, 0, bw, 0.35f, pW, a.rr, a.rg, a.rb);
    }
    m.addBox(0, pH*2.0f + archR + 0.2f, 0, 0.3f, 0.3f, pW*1.1f, a.ar, a.ag, a.ab);
    if (a.spire)
        m.addCone(0, pH*2.0f + archR + 0.5f + a.spire_h, 0, 0.2f, 0.03f, a.spire_h, 6,
                  a.ar, a.ag, a.ab);
    return m;
}

static StructMesh gen_motif_cabin(const SArch& a) {
    StructMesh m;
    m.addBox(0, 0.12f, 0, a.w*1.05f, 0.12f, a.w*0.85f, a.dr*0.7f, a.dg*0.7f, a.db*0.7f);
    float wallH = a.h;
    m.addBox(0, 0.24f + wallH, 0, a.w, wallH, a.w*0.8f, a.wr, a.wg, a.wb);
    for (int i = 0; i < 3; i++) {
        float y = 0.24f + 0.5f + i * 0.8f;
        m.addBox(0, y, a.w*0.81f, a.w, 0.04f, 0.04f, a.dr*0.6f, a.dg*0.5f, a.db*0.4f);
    }
    float roofBase = 0.24f + wallH * 2.0f;
    m.addRoof(0, roofBase, 0, a.w*1.1f, a.w*0.9f, a.roof_h, a.rr, a.rg, a.rb);
    if (a.entrance)
        m.addBox(0, 0.24f + 0.7f, a.w*0.81f, 0.35f, 0.7f, 0.04f,
                 a.dr*0.4f, a.dg*0.3f, a.db*0.2f);
    if (a.windows) {
        m.addBox(-a.w*0.5f, 0.24f+wallH, a.w*0.81f, 0.25f, 0.25f, 0.04f,
                 a.ar*0.6f, a.ag*0.7f, a.ab*0.8f);
        m.addBox( a.w*0.5f, 0.24f+wallH, a.w*0.81f, 0.25f, 0.25f, 0.04f,
                 a.ar*0.6f, a.ag*0.7f, a.ab*0.8f);
    }
    if (a.spire)
        m.addBox(a.w*0.5f, roofBase + a.roof_h*0.5f, -a.w*0.4f, 0.2f, a.roof_h*0.6f, 0.2f,
                 a.dr, a.dg, a.db);
    return m;
}

static StructMesh gen_motif_obelisk(const SArch& a) {
    StructMesh m;
    m.addBox(0, 0.2f, 0, a.w*1.3f, 0.2f, a.w*1.3f, a.dr*0.7f, a.dg*0.7f, a.db*0.7f);
    float w = a.w * 0.5f;
    for (int i = 0; i < 4; i++) {
        float y = 0.4f + i * a.h * 2.0f + a.h;
        float ww = w - i * 0.06f;
        float f = 1.0f - i * 0.06f;
        m.addBox(0, y, 0, ww, a.h, ww, a.wr*f, a.wg*f, a.wb*f);
    }
    for (int i = 0; i < 3; i++) {
        float y = 0.4f + (i+1) * a.h * 2.0f;
        m.addBox(0, y + 0.05f, 0, w*1.1f - i*0.05f, 0.05f, w*1.1f - i*0.05f,
                 a.ar, a.ag, a.ab);
    }
    float topY = 0.4f + 4 * a.h * 2.0f;
    float tw = w - 4 * 0.06f;
    m.addCone(0, topY + a.roof_h, 0, tw, 0.04f, a.roof_h, 4, a.rr, a.rg, a.rb);
    return m;
}

static StructMesh gen_motif_lighthouse(const SArch& a) {
    StructMesh m;
    m.addCylinder(0, 0.5f, 0, a.w*1.3f, 0.5f, 12, a.dr*0.7f, a.dg*0.7f, a.db*0.7f);
    float towerH = a.h * a.sections;
    m.addCone(0, 0.5f + towerH, 0, a.w*1.1f, a.w*0.6f, towerH, 12, a.wr, a.wg, a.wb);
    float deckY = 0.5f + towerH * 2.0f;
    m.addCylinder(0, deckY + 0.15f, 0, a.w*0.9f, 0.15f, 12, a.ar, a.ag, a.ab);
    if (a.railing) {
        for (int i = 0; i < 8; i++) {
            float angle = (float)i / 8.0f * 6.2831853f;
            float rx = cosf(angle)*a.w*0.8f, rz = sinf(angle)*a.w*0.8f;
            m.addBox(rx, deckY+0.5f, rz, 0.06f, 0.3f, 0.06f, a.wr*0.6f, a.wg*0.6f, a.wb*0.6f);
        }
    }
    m.addCylinder(0, deckY+0.3f+0.8f, 0, a.w*0.45f, 0.8f, 8, a.ar*1.1f, a.ag*1.0f, a.ab*0.6f);
    m.addCone(0, deckY+0.3f+1.6f+0.5f, 0, a.w*0.55f, 0.08f, 0.5f, 8, a.rr, a.rg, a.rb);
    if (a.entrance)
        m.addBox(0, 0.8f, a.w*1.12f, 0.3f, 0.6f, 0.05f, a.dr*0.4f, a.dg*0.3f, a.db*0.2f);
    return m;
}

static StructMesh gen_motif_pavilion(const SArch& a) {
    StructMesh m;
    m.addBox(0, 0.3f, 0, a.w*1.4f, 0.3f, a.w*1.4f, a.dr*0.8f, a.dg*0.8f, a.db*0.8f);
    m.addBox(0, 0.6f+0.08f, 0, a.w*1.2f, 0.08f, a.w*1.2f, a.wr, a.wg, a.wb);
    float pilH = a.h * 1.5f, pilR = 0.15f + a.w * 0.03f;
    int np = std::max(4, a.pillar_n);
    for (int i = 0; i < np; i++) {
        float angle = (float)i / np * 6.2831853f;
        float px = cosf(angle)*a.w, pz = sinf(angle)*a.w;
        m.addCylinder(px, 0.68f + pilH, pz, pilR, pilH, 6, a.wr, a.wg, a.wb);
    }
    float roofY = 0.68f + pilH * 2.0f;
    if (a.roof <= 1)
        m.addCone(0, roofY + a.roof_h, 0, a.w*1.3f, 0.06f, a.roof_h, 12, a.rr, a.rg, a.rb);
    else if (a.roof == 2 || a.roof == 3) {
        m.addCone(0, roofY + a.roof_h*0.7f, 0, a.w*1.3f, a.w*0.4f, a.roof_h*0.7f, 12,
                  a.rr, a.rg, a.rb);
        m.addCone(0, roofY + a.roof_h, 0, a.w*0.4f, 0.03f, a.roof_h*0.3f, 12,
                  a.rr*0.9f, a.rg*0.9f, a.rb*0.9f);
    } else
        m.addBox(0, roofY + 0.15f, 0, a.w*1.25f, 0.15f, a.w*1.25f, a.rr, a.rg, a.rb);
    if (a.spire)
        m.addCone(0, roofY + a.roof_h + a.spire_h, 0, 0.12f, 0.02f, a.spire_h, 6,
                  a.ar, a.ag, a.ab);
    return m;
}

static StructMesh gen_biome_structure(int biome) {
    const SArch& a = g_sarch[biome];
    switch (a.motif) {
        case 0: return gen_motif_tower(a);
        case 1: return gen_motif_temple(a);
        case 2: return gen_motif_pyramid(a);
        case 3: return gen_motif_arch(a);
        case 4: return gen_motif_cabin(a);
        case 5: return gen_motif_obelisk(a);
        case 6: return gen_motif_lighthouse(a);
        case 7: return gen_motif_pavilion(a);
        default: return gen_motif_tower(a);
    }
}

// ================================================================
// Place structures across the terrain (biome-driven)
// ================================================================

static const char* g_motif_names[] = {
    "tower","temple","pyramid","arch","cabin","obelisk","lighthouse","pavilion"
};

StructMesh generate_structures() {
    init_sarch();

    StructMesh world;
    StructMesh templates[NUM_BIOMES];
    for (int b = 0; b < NUM_BIOMES; b++)
        templates[b] = gen_biome_structure(b);

    int counts[NUM_BIOMES] = {};
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    float margin = 80.0f;

    uint32_t seed = g_variance.seed * 2246822519u + 12345u;
    auto lcg = [&]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed & 0xFFFFFF) / (float)0xFFFFFF;
    };

    struct Placement { float x, z; };
    std::vector<Placement> placed;

    int maxStructures = 60;
    int attempts = 0;
    while ((int)placed.size() < maxStructures && attempts < 3000) {
        attempts++;
        float wx = (lcg() * 2.0f - 1.0f) * (half - margin);
        float wz = (lcg() * 2.0f - 1.0f) * (half - margin);

        double v_density = g_variance.sample_channel(wx, 0, wz, 4);
        float density_pass = (float)remap(v_density, -1, 1, 0.25, 0.85);
        if (lcg() > density_pass) continue;

        float minDist = (float)remap(v_density, -1, 1, 45.0, 18.0);
        bool too_close = false;
        for (auto& p : placed) {
            float dx = wx - p.x, dz = wz - p.z;
            if (sqrtf(dx*dx + dz*dz) < minDist) { too_close = true; break; }
        }
        if (too_close) continue;

        float h = terrain_height(wx, wz);
        float hx1 = terrain_height(wx+2, wz), hz1 = terrain_height(wx, wz+2);
        float slope = sqrtf((h-hx1)*(h-hx1) + (h-hz1)*(h-hz1));
        if (slope > 3.0f) continue;
        if (h < 3.0f) continue;

        // 80% local biome structure, 20% random biome
        int biome = biome_at(wx, wz);
        int tbi;
        if (lcg() < 0.80f)
            tbi = biome;
        else
            tbi = (int)(lcg() * NUM_BIOMES) % NUM_BIOMES;

        double v_scale = g_variance.sample_channel(wx, 0, wz, 5);
        float sf = (float)remap(v_scale, -1, 1, 0.6, 1.5);

        StructMesh scaled = templates[tbi];
        for (auto& v : scaled.vertices) {
            v.px *= sf; v.py *= sf; v.pz *= sf;
        }

        float rotY = lcg() * 6.2831853f;
        world.merge(scaled, wx, h, wz, rotY);
        placed.push_back({wx, wz});
        counts[tbi]++;
    }

    printf("Structures: %d placed (", (int)placed.size());
    for (int i = 0; i < NUM_BIOMES; i++) {
        if (i > 0) printf(", ");
        printf("%d b%d-%s", counts[i], i, g_motif_names[g_sarch[i].motif]);
    }
    printf("), %d verts, %d tris\n",
           (int)world.vertices.size(), (int)world.indices.size()/3);
    return world;
}

// ================================================================
// L-system infrastructure (unchanged)
// ================================================================

struct LRule {
    std::string replacement;
    float probability;
};

struct PlantSpecies {
    std::string name;
    std::string axiom;
    std::map<char, std::vector<LRule>> rules;
    int iterations;
    float angle;
    float length;
    float lengthScale;
    float thickness;
    float thicknessScale;
    Vec3 trunkColor;
    Vec3 leafColor;
    float leafSize;
    float tropism;
    float variance;
};

static std::string generateLSystem(const PlantSpecies& sp, std::mt19937& rng) {
    std::string current = sp.axiom;
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (int iter = 0; iter < sp.iterations; iter++) {
        std::string next;
        next.reserve(current.size() * 5);
        for (char c : current) {
            auto it = sp.rules.find(c);
            if (it != sp.rules.end()) {
                const auto& rl = it->second;
                float r = dist(rng);
                float cum = 0.0f;
                for (const auto& rule : rl) {
                    cum += rule.probability;
                    if (r <= cum) { next += rule.replacement; break; }
                }
            } else {
                next += c;
            }
            if ((int)next.size() > MAX_LSYSTEM_LEN) break;
        }
        current = std::move(next);
        if ((int)current.size() > MAX_LSYSTEM_LEN) break;
    }
    return current;
}

static void plant_addCylinder(StructMesh& mesh, Vec3 p0, Vec3 p1, float r0, float r1,
                               Vec3 col, int sides = 5) {
    Vec3 d = p1 - p0;
    float l = vlen(d);
    if (l < 1e-6f) return;
    d = d * (1.0f / l);
    Vec3 arb = fabsf(d.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 right = vnorm(cross3(d, arb));
    Vec3 up = vnorm(cross3(right, d));
    for (int i = 0; i < sides; i++) {
        float a0 = 2.0f * PI_F * i / sides;
        float a1 = 2.0f * PI_F * (i + 1) / sides;
        Vec3 n0 = right * cosf(a0) + up * sinf(a0);
        Vec3 n1 = right * cosf(a1) + up * sinf(a1);
        Vec3 v00 = p0 + n0 * r0, v01 = p0 + n1 * r0;
        Vec3 v10 = p1 + n0 * r1, v11 = p1 + n1 * r1;
        mesh.addTriRaw(
            {v00.x,v00.y,v00.z, n0.x,n0.y,n0.z, col.x,col.y,col.z},
            {v10.x,v10.y,v10.z, n0.x,n0.y,n0.z, col.x,col.y,col.z},
            {v01.x,v01.y,v01.z, n1.x,n1.y,n1.z, col.x,col.y,col.z});
        mesh.addTriRaw(
            {v01.x,v01.y,v01.z, n1.x,n1.y,n1.z, col.x,col.y,col.z},
            {v10.x,v10.y,v10.z, n0.x,n0.y,n0.z, col.x,col.y,col.z},
            {v11.x,v11.y,v11.z, n1.x,n1.y,n1.z, col.x,col.y,col.z});
    }
}

static void plant_addLeaf(StructMesh& mesh, Vec3 pos, Vec3 dir, Vec3 right, float size,
                           Vec3 col) {
    Vec3 ld = vnorm(dir);
    Vec3 lr = vnorm(right);
    Vec3 n = vnorm(cross3(ld, lr));
    Vec3 tip = pos + ld * size;
    Vec3 mid = pos + ld * (size * 0.45f);
    Vec3 lp = mid - lr * (size * 0.25f);
    Vec3 rp = mid + lr * (size * 0.25f);
    mesh.addTriRaw({pos.x,pos.y,pos.z, n.x,n.y,n.z, col.x,col.y,col.z},
                   {lp.x,lp.y,lp.z, n.x,n.y,n.z, col.x,col.y,col.z},
                   {tip.x,tip.y,tip.z, n.x,n.y,n.z, col.x,col.y,col.z});
    mesh.addTriRaw({pos.x,pos.y,pos.z, n.x,n.y,n.z, col.x,col.y,col.z},
                   {tip.x,tip.y,tip.z, n.x,n.y,n.z, col.x,col.y,col.z},
                   {rp.x,rp.y,rp.z, n.x,n.y,n.z, col.x,col.y,col.z});
    Vec3 bn = Vec3(0,0,0) - n;
    mesh.addTriRaw({pos.x,pos.y,pos.z, bn.x,bn.y,bn.z, col.x,col.y,col.z},
                   {tip.x,tip.y,tip.z, bn.x,bn.y,bn.z, col.x,col.y,col.z},
                   {lp.x,lp.y,lp.z, bn.x,bn.y,bn.z, col.x,col.y,col.z});
    mesh.addTriRaw({pos.x,pos.y,pos.z, bn.x,bn.y,bn.z, col.x,col.y,col.z},
                   {rp.x,rp.y,rp.z, bn.x,bn.y,bn.z, col.x,col.y,col.z},
                   {tip.x,tip.y,tip.z, bn.x,bn.y,bn.z, col.x,col.y,col.z});
}

struct TurtleState {
    Vec3 pos, heading, left, up;
    float thickness, segLength;
    int depth;
};

static void interpretLSystem(const std::string& lstr, const PlantSpecies& sp,
                              Vec3 origin, float scale, std::mt19937& rng,
                              StructMesh& mesh) {
    TurtleState st;
    st.pos = origin;
    st.heading = Vec3(0, 1, 0);
    st.left = Vec3(-1, 0, 0);
    st.up = Vec3(0, 0, 1);
    st.thickness = sp.thickness * scale;
    st.segLength = sp.length * scale;
    st.depth = 0;

    std::stack<TurtleState> stk;
    std::uniform_real_distribution<float> vdist(-1.0f, 1.0f);
    float baseAngle = sp.angle * PI_F / 180.0f;

    for (size_t i = 0; i < lstr.size(); i++) {
        char c = lstr[i];
        float angleVar = vdist(rng) * sp.variance * PI_F / 180.0f;
        float a = baseAngle + angleVar;

        float dr = (float)st.depth / (float)(sp.iterations + 1);
        Vec3 col = sp.trunkColor * (1.0f - dr * 0.5f) + sp.leafColor * (dr * 0.5f);

        switch (c) {
        case 'F': case 'G': {
            Vec3 end = st.pos + st.heading * st.segLength;
            if (sp.tropism != 0.0f) {
                Vec3 grav(0, -1, 0);
                Vec3 axis = cross3(st.heading, grav);
                float al = vlen(axis);
                if (al > 0.01f) {
                    axis = axis * (1.0f / al);
                    float bend = sp.tropism * st.segLength;
                    st.heading = vnorm(rotateAround(st.heading, axis, bend));
                    st.left = vnorm(rotateAround(st.left, axis, bend));
                    st.up = vnorm(rotateAround(st.up, axis, bend));
                    end = st.pos + st.heading * st.segLength;
                }
            }
            plant_addCylinder(mesh, st.pos, end, st.thickness, st.thickness * 0.85f, col);
            st.pos = end;
            break;
        }
        case 'f': st.pos = st.pos + st.heading * st.segLength; break;
        case '+': st.heading = rotateAround(st.heading, st.up, a);
                  st.left = rotateAround(st.left, st.up, a); break;
        case '-': st.heading = rotateAround(st.heading, st.up, -a);
                  st.left = rotateAround(st.left, st.up, -a); break;
        case '&': st.heading = rotateAround(st.heading, st.left, a);
                  st.up = rotateAround(st.up, st.left, a); break;
        case '^': st.heading = rotateAround(st.heading, st.left, -a);
                  st.up = rotateAround(st.up, st.left, -a); break;
        case '\\': st.left = rotateAround(st.left, st.heading, a);
                   st.up = rotateAround(st.up, st.heading, a); break;
        case '/': st.left = rotateAround(st.left, st.heading, -a);
                  st.up = rotateAround(st.up, st.heading, -a); break;
        case '|': st.heading = Vec3(0,0,0) - st.heading; st.left = Vec3(0,0,0) - st.left; break;
        case '[': stk.push(st); st.depth++; st.thickness *= sp.thicknessScale;
                  st.segLength *= sp.lengthScale; break;
        case ']': if (!stk.empty()) { st = stk.top(); stk.pop(); } break;
        case 'L': plant_addLeaf(mesh, st.pos, st.heading, st.left,
                                sp.leafSize * scale, sp.leafColor); break;
        default: break;
        }
    }
}

// ================================================================
// Parametric plant species (3 per biome = 30 total, seed-derived)
// ================================================================

static std::vector<PlantSpecies> g_plant_species;
static bool g_plants_init = false;

static void init_plant_species() {
    if (g_plants_init) return;
    g_plants_init = true;
    g_plant_species.clear();

    for (int b = 0; b < NUM_BIOMES; b++) {
        float pal[4][3];
        get_biome_palette(b, pal);
        float biome_hue = get_biome_hue(b);

        for (int v = 0; v < 3; v++) {
            int key = b * 100 + v;
            PlantSpecies sp;
            sp.name = "B" + std::to_string(b) + "P" + std::to_string(v);

            // Branching style (6 types)
            int style = spi(g_world_seed, key, 2000, 6);

            // Parameters from seed
            sp.angle         = 15.0f + spf(g_world_seed, key, 2001) * 50.0f;
            sp.length        = 0.1f  + spf(g_world_seed, key, 2002) * 0.55f;
            sp.lengthScale   = 0.55f + spf(g_world_seed, key, 2003) * 0.35f;
            sp.thickness     = 0.01f + spf(g_world_seed, key, 2004) * 0.17f;
            sp.thicknessScale= 0.5f  + spf(g_world_seed, key, 2005) * 0.45f;
            sp.iterations    = 2 + spi(g_world_seed, key, 2006, 4);
            sp.tropism       = -0.02f + spf(g_world_seed, key, 2007) * 0.14f;
            sp.variance      = 3.0f  + spf(g_world_seed, key, 2008) * 12.0f;
            sp.leafSize      = 0.05f + spf(g_world_seed, key, 2009) * 0.45f;

            // Trunk color: desaturated/darkened biome tint
            float trunk_h = biome_hue + spf(g_world_seed, key, 2100) * 40.0f - 20.0f;
            float trunk_s = 0.15f + spf(g_world_seed, key, 2101) * 0.25f;
            float trunk_v = 0.15f + spf(g_world_seed, key, 2102) * 0.3f;
            float tr, tg, tb;
            hsv_rgb(trunk_h, trunk_s, trunk_v, tr, tg, tb);
            sp.trunkColor = Vec3(tr, tg, tb);

            // Leaf color: biome-influenced, sometimes pushed toward green
            float leaf_h = biome_hue + spf(g_world_seed, key, 2200) * 60.0f - 30.0f;
            if (spf(g_world_seed, key, 2210) > 0.4f)
                leaf_h = leaf_h * 0.5f + 60.0f;  // blend toward green
            float leaf_s = 0.4f + spf(g_world_seed, key, 2201) * 0.5f;
            float leaf_v = 0.3f + spf(g_world_seed, key, 2202) * 0.5f;
            float lr, lg, lb;
            hsv_rgb(leaf_h, leaf_s, leaf_v, lr, lg, lb);
            sp.leafColor = Vec3(lr, lg, lb);

            // Generate L-system rules from branching style
            switch (style) {
            case 0: // Forking (deciduous)
                sp.axiom = "FFFA";
                sp.rules['A'] = {{"[&+FAL]/////[&+FAL]/////[&+FAL]", 0.6f},
                                  {"[&-FAL]////[&-FAL]////[&+FAL]", 0.4f}};
                sp.rules['F'] = {{"FF", 0.5f}, {"F", 0.5f}};
                break;
            case 1: // Whorled (conifer)
                sp.axiom = "FFFFFA";
                sp.rules['A'] = {{"F[&&+FL][&&-FL][&&/FL][&&\\FL]A", 1.0f}};
                break;
            case 2: // Spiral (tropical)
                sp.axiom = "FFFFA";
                sp.rules['A'] = {{"F[&&+FAL]//[&&+FAL]//[&&+FAL]A", 0.7f},
                                  {"F[&&-FAL]//[&&-FAL]A", 0.3f}};
                sp.rules['F'] = {{"FF", 0.4f}, {"F", 0.6f}};
                break;
            case 3: // Fan (palm-like)
                sp.axiom = "FFFFFFA";
                sp.rules['A'] = {{"[&&B]/[&&B]/[&&B]/[&&B]/[&&B]/[&&B]", 1.0f}};
                sp.rules['B'] = {{"F[-L][+L]F[-L][+L]FL", 1.0f}};
                sp.iterations = std::min(sp.iterations, 2);
                break;
            case 4: // Bushy (shrub)
                sp.axiom = "FA";
                sp.rules['A'] = {{"[&+AL]F[&-AL]F[&/AL]", 0.5f},
                                  {"[&-AL]F[&+AL]F[&\\AL]", 0.5f}};
                sp.rules['F'] = {{"FF", 0.3f}, {"F", 0.7f}};
                break;
            case 5: // Drooping (willow/weeping)
                sp.axiom = "FFFA";
                sp.rules['A'] = {{"[&&+FFAL]/////[&&+FFAL]/////[&&+FFAL]", 1.0f}};
                sp.rules['F'] = {{"FF", 0.5f}, {"F", 0.5f}};
                sp.tropism = std::max(sp.tropism, 0.06f);
                break;
            }

            g_plant_species.push_back(sp);
        }
    }
}

// ================================================================
// Place L-system plants across the terrain (biome-driven)
// ================================================================

StructMesh generate_plants() {
    init_plant_species();

    StructMesh world;
    std::mt19937 rng(g_variance.seed * 1664525u + 98765u);
    std::uniform_real_distribution<float> scaleDist(0.5f, 1.6f);
    std::uniform_real_distribution<float> colorVar(0.7f, 1.35f);

    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    float margin = 60.0f;

    uint32_t seed = g_variance.seed * 1103515245u + 54321u;
    auto lcg = [&]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return (float)(seed & 0xFFFFFF) / (float)0xFFFFFF;
    };

    struct Placement { float x, z; };
    std::vector<Placement> placed;

    int maxPlants = 300;
    int attempts = 0;
    int total_species = (int)g_plant_species.size();
    std::vector<int> counts(total_species, 0);

    while ((int)placed.size() < maxPlants && attempts < 6000) {
        attempts++;
        float wx = (lcg() * 2.0f - 1.0f) * (half - margin);
        float wz = (lcg() * 2.0f - 1.0f) * (half - margin);

        double v_density = g_variance.sample_channel(wx, 0, wz, 4);
        float density_pass = (float)remap(v_density, -1, 1, 0.2, 0.75);
        if (lcg() > density_pass) continue;

        float minDist = (float)remap(v_density, -1, 1, 30.0, 12.0);
        bool too_close = false;
        for (auto& p : placed) {
            float dx = wx - p.x, dz = wz - p.z;
            if (sqrtf(dx*dx + dz*dz) < minDist) { too_close = true; break; }
        }
        if (too_close) continue;

        float h = terrain_height(wx, wz);
        float hx1 = terrain_height(wx+2, wz), hz1 = terrain_height(wx, wz+2);
        float slope = sqrtf((h-hx1)*(h-hx1) + (h-hz1)*(h-hz1));
        if (slope > 4.0f) continue;
        if (h < 2.0f) continue;

        // 80% from this biome's 3 species, 20% random
        int biome = biome_at(wx, wz);
        int si;
        if (lcg() < 0.80f) {
            si = biome * 3 + (int)(lcg() * 3) % 3;
        } else {
            si = (int)(lcg() * total_species);
            if (si >= total_species) si = total_species - 1;
        }

        float baseScale = 4.0f;
        float scale = baseScale * scaleDist(rng);

        PlantSpecies inst = g_plant_species[si];

        // Color variation
        float cv = colorVar(rng);
        inst.trunkColor = Vec3(
            std::clamp(inst.trunkColor.x * cv, 0.0f, 1.0f),
            std::clamp(inst.trunkColor.y * cv, 0.0f, 1.0f),
            std::clamp(inst.trunkColor.z * cv, 0.0f, 1.0f));

        float lcv = colorVar(rng);
        if (lcg() < 0.05f) {
            // 5% wild color mutation
            float wr = lcg(), wg = lcg(), wb = lcg();
            inst.leafColor = Vec3(
                std::clamp(inst.leafColor.x * 0.3f + wr * 0.7f, 0.0f, 1.0f),
                std::clamp(inst.leafColor.y * 0.3f + wg * 0.7f, 0.0f, 1.0f),
                std::clamp(inst.leafColor.z * 0.3f + wb * 0.7f, 0.0f, 1.0f));
        } else {
            inst.leafColor = Vec3(
                std::clamp(inst.leafColor.x * lcv, 0.0f, 1.0f),
                std::clamp(inst.leafColor.y * lcv, 0.0f, 1.0f),
                std::clamp(inst.leafColor.z * lcv, 0.0f, 1.0f));
        }

        std::string lstr = generateLSystem(inst, rng);
        interpretLSystem(lstr, inst, Vec3(wx, h, wz), scale, rng, world);
        placed.push_back({wx, wz});
        counts[si]++;
    }

    printf("Plants: %d placed (", (int)placed.size());
    bool first = true;
    for (int i = 0; i < total_species; i++) {
        if (counts[i] > 0) {
            if (!first) printf(", ");
            printf("%d %s", counts[i], g_plant_species[i].name.c_str());
            first = false;
        }
    }
    printf("), %d verts, %d tris\n",
           (int)world.vertices.size(), (int)world.indices.size()/3);
    return world;
}
