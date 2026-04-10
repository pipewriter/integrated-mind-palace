#include "collision.h"
#include "app.h"
#include "structures.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

// ----------------------------------------------------------------
// Collision system (for walk mode)
// ----------------------------------------------------------------

void col_grid_cell_range(float wx, float wz, float radius, int& cx_min, int& cx_max, int& cz_min, int& cz_max) {
    float half_ext = COL_GRID_DIM * COL_GRID_CELL * 0.5f;
    cx_min = std::clamp((int)((wx - radius + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
    cx_max = std::clamp((int)((wx + radius + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
    cz_min = std::clamp((int)((wz - radius + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
    cz_max = std::clamp((int)((wz + radius + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
}

void build_col_grid() {
    g_col_grid.clear();
    g_col_grid.resize((size_t)COL_GRID_DIM * COL_GRID_DIM);
    float half_ext = COL_GRID_DIM * COL_GRID_CELL * 0.5f;
    for (uint32_t i = 0; i < (uint32_t)g_col_tris.size(); i++) {
        const ColTri& t = g_col_tris[i];
        // Find AABB of triangle in XZ
        float min_x = std::min({t.v[0][0], t.v[1][0], t.v[2][0]});
        float max_x = std::max({t.v[0][0], t.v[1][0], t.v[2][0]});
        float min_z = std::min({t.v[0][2], t.v[1][2], t.v[2][2]});
        float max_z = std::max({t.v[0][2], t.v[1][2], t.v[2][2]});
        int cx0 = std::clamp((int)((min_x + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
        int cx1 = std::clamp((int)((max_x + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
        int cz0 = std::clamp((int)((min_z + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
        int cz1 = std::clamp((int)((max_z + half_ext) / COL_GRID_CELL), 0, COL_GRID_DIM - 1);
        for (int cz = cz0; cz <= cz1; cz++)
            for (int cx = cx0; cx <= cx1; cx++)
                g_col_grid[cz * COL_GRID_DIM + cx].push_back(i);
    }
}

static bool col_baryInside(float p[3], const ColTri& t, float &u, float &v, float &w) {
    float v0[3] = {t.v[1][0]-t.v[0][0], t.v[1][1]-t.v[0][1], t.v[1][2]-t.v[0][2]};
    float v1[3] = {t.v[2][0]-t.v[0][0], t.v[2][1]-t.v[0][1], t.v[2][2]-t.v[0][2]};
    float v2[3] = {p[0]-t.v[0][0], p[1]-t.v[0][1], p[2]-t.v[0][2]};
    float d00 = v0[0]*v0[0]+v0[1]*v0[1]+v0[2]*v0[2];
    float d01 = v0[0]*v1[0]+v0[1]*v1[1]+v0[2]*v1[2];
    float d11 = v1[0]*v1[0]+v1[1]*v1[1]+v1[2]*v1[2];
    float d20 = v2[0]*v0[0]+v2[1]*v0[1]+v2[2]*v0[2];
    float d21 = v2[0]*v1[0]+v2[1]*v1[1]+v2[2]*v1[2];
    float denom = d00*d11 - d01*d01;
    if (fabsf(denom) < 1e-12f) return false;
    float inv = 1.0f / denom;
    v = (d11*d20 - d01*d21) * inv;
    w = (d00*d21 - d01*d20) * inv;
    u = 1.0f - v - w;
    const float E = -1e-3f;
    return (u >= E && v >= E && w >= E);
}

static void col_closestOnSeg(float p[3], float a[3], float b[3], float out[3]) {
    float ab[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
    float ap[3] = {p[0]-a[0], p[1]-a[1], p[2]-a[2]};
    float dot_ab = ab[0]*ab[0]+ab[1]*ab[1]+ab[2]*ab[2];
    float t = (ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2]) / (dot_ab + 1e-12f);
    t = std::max(0.0f, std::min(1.0f, t));
    out[0] = a[0]+ab[0]*t; out[1] = a[1]+ab[1]*t; out[2] = a[2]+ab[2]*t;
}

static void col_closestOnTri(float p[3], const ColTri& tri, float out[3]) {
    float dist = (p[0]-tri.v[0][0])*tri.normal[0]+(p[1]-tri.v[0][1])*tri.normal[1]+(p[2]-tri.v[0][2])*tri.normal[2];
    float proj[3] = {p[0]-tri.normal[0]*dist, p[1]-tri.normal[1]*dist, p[2]-tri.normal[2]*dist};
    float u,v,w;
    if (col_baryInside(proj, tri, u, v, w)) {
        out[0]=proj[0]; out[1]=proj[1]; out[2]=proj[2]; return;
    }
    float best[3], bestD = 1e30f;
    float edges[3][2][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        edges[i][0][j] = tri.v[i][j];
        edges[i][1][j] = tri.v[(i+1)%3][j];
    }
    for (int e = 0; e < 3; e++) {
        float c[3];
        col_closestOnSeg(p, edges[e][0], edges[e][1], c);
        float dx=p[0]-c[0], dy=p[1]-c[1], dz=p[2]-c[2];
        float d2=dx*dx+dy*dy+dz*dz;
        if (d2 < bestD) { bestD=d2; best[0]=c[0]; best[1]=c[1]; best[2]=c[2]; }
    }
    out[0]=best[0]; out[1]=best[1]; out[2]=best[2];
}

void col_wallSlide(WalkPlayer& pl) {
    if (g_col_grid.empty()) return;

    float bodyBot[3] = {pl.px, pl.py + STEP_HEIGHT, pl.pz};
    float bodyTop[3] = {pl.px, pl.py + PLAYER_HEIGHT - 0.05f, pl.pz};

    // Only check triangles in nearby grid cells
    int cx_min, cx_max, cz_min, cz_max;
    col_grid_cell_range(pl.px, pl.pz, PLAYER_RADIUS + 1.0f, cx_min, cx_max, cz_min, cz_max);

    for (int cz = cz_min; cz <= cz_max; cz++) {
        for (int cx = cx_min; cx <= cx_max; cx++) {
            for (uint32_t ti : g_col_grid[cz * COL_GRID_DIM + cx]) {
                const ColTri& t = g_col_tris[ti];
                for (int s = 0; s < 3; s++) {
                    float frac = s / 2.0f;
                    float sample[3] = {
                        bodyBot[0]+(bodyTop[0]-bodyBot[0])*frac,
                        bodyBot[1]+(bodyTop[1]-bodyBot[1])*frac,
                        bodyBot[2]+(bodyTop[2]-bodyBot[2])*frac
                    };
                    float cp[3];
                    col_closestOnTri(sample, t, cp);
                    float dx = sample[0]-cp[0], dz = sample[2]-cp[2];
                    float hDist = sqrtf(dx*dx+dz*dz);
                    if (hDist < PLAYER_RADIUS && hDist > 1e-6f) {
                        if (cp[1] < pl.py + STEP_HEIGHT) continue;
                        if (cp[1] > pl.py + PLAYER_HEIGHT) continue;
                        float inv = 1.0f / hDist;
                        float pdx = dx*inv, pdz = dz*inv;
                        float push = PLAYER_RADIUS - hDist;
                        pl.px += pdx * push;
                        pl.pz += pdz * push;
                        float velDot = pl.vx*pdx + pl.vz*pdz;
                        if (velDot < 0) { pl.vx -= pdx*velDot; pl.vz -= pdz*velDot; }
                    }
                }
            }
        }
    }
}

void extract_collision_tris(const StructMesh& mesh) {
    g_col_tris.clear();
    g_col_tris.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        ColTri ct;
        for (int k = 0; k < 3; k++) {
            const ColorVertex& cv = mesh.vertices[mesh.indices[i+k]];
            ct.v[k][0] = cv.px; ct.v[k][1] = cv.py; ct.v[k][2] = cv.pz;
        }
        ct.calcNormal();
        g_col_tris.push_back(ct);
    }
    printf("Collision tris: %d extracted for walk mode\n", (int)g_col_tris.size());
    build_col_grid();
    printf("Collision grid: %dx%d cells, %.1f units/cell\n", COL_GRID_DIM, COL_GRID_DIM, COL_GRID_CELL);
}
