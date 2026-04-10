// ================================================================
// Trail / footprint system — painting, depression, structure wear
// ================================================================

#include "trail.h"
#include "app.h"
#include "terrain.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdio>

// ----------------------------------------------------------------
// File-local state
// ----------------------------------------------------------------

static float g_last_trail_x = 1e9f, g_last_trail_z = 1e9f;
static int   g_mesh_update_counter = 0;

// Forward declaration (defined in client.cpp / networking module)
void net_send(uint8_t type, const void* payload, uint32_t plen);

// ----------------------------------------------------------------
// Internal helpers
// ----------------------------------------------------------------

static uint32_t hash_pixel(int x, int z) {
    uint32_t h = (uint32_t)(x * 7919 + z * 104729);
    h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
    return h;
}

static int struct_grid_cell(float wx, float wz) {
    int cx = (int)((wx + STRUCT_GRID_DIM * STRUCT_GRID_CELL * 0.5f) / STRUCT_GRID_CELL);
    int cz = (int)((wz + STRUCT_GRID_DIM * STRUCT_GRID_CELL * 0.5f) / STRUCT_GRID_CELL);
    cx = std::clamp(cx, 0, STRUCT_GRID_DIM - 1);
    cz = std::clamp(cz, 0, STRUCT_GRID_DIM - 1);
    return cz * STRUCT_GRID_DIM + cx;
}

// Internal 4-arg version: update terrain texture pixels in a specific region
static void update_trail_texture_region_impl(int px_min, int pz_min, int px_max, int pz_max) {
    for (int z = pz_min; z <= pz_max; z++) {
        for (int x = px_min; x <= px_max; x++) {
            int idx = z * TRAIL_TEX_SIZE + x;
            float t = g_trail[idx];
            if (t < 0.001f) continue;

            int pi = idx * 4;
            float or_ = (float)g_terrain_pixels_orig[pi]     / 255.0f;
            float og  = (float)g_terrain_pixels_orig[pi + 1] / 255.0f;
            float ob  = (float)g_terrain_pixels_orig[pi + 2] / 255.0f;

            float avg = (or_ + og + ob) / 3.0f;
            float worn_r = avg * 0.55f + 0.10f;
            float worn_g = avg * 0.45f + 0.05f;
            float worn_b = avg * 0.30f + 0.03f;

            float noise = ((float)(hash_pixel(x, z) & 0xFF) / 255.0f) * 0.06f - 0.03f;
            worn_r += noise;
            worn_g += noise * 0.8f;
            worn_b += noise * 0.6f;

            float blend = t * 0.75f;
            float r = or_ * (1.0f - blend) + worn_r * blend;
            float g = og * (1.0f - blend) + worn_g * blend;
            float b = ob * (1.0f - blend) + worn_b * blend;

            float darken = 1.0f - t * 0.25f;
            r *= darken; g *= darken; b *= darken;

            g_terrain_pixels[pi]     = (unsigned char)(std::clamp(r, 0.0f, 1.0f) * 255);
            g_terrain_pixels[pi + 1] = (unsigned char)(std::clamp(g, 0.0f, 1.0f) * 255);
            g_terrain_pixels[pi + 2] = (unsigned char)(std::clamp(b, 0.0f, 1.0f) * 255);
        }
    }
}

// ----------------------------------------------------------------
// Public API
// ----------------------------------------------------------------

void trail_reset_dirty_region() {
    g_trail_dirty_px_min = TRAIL_TEX_SIZE;
    g_trail_dirty_px_max = -1;
    g_trail_dirty_pz_min = TRAIL_TEX_SIZE;
    g_trail_dirty_pz_max = -1;
}

void trail_mark_dirty_region(int px_min, int px_max, int pz_min, int pz_max) {
    g_trail_dirty_px_min = std::min(g_trail_dirty_px_min, px_min);
    g_trail_dirty_px_max = std::max(g_trail_dirty_px_max, px_max);
    g_trail_dirty_pz_min = std::min(g_trail_dirty_pz_min, pz_min);
    g_trail_dirty_pz_max = std::max(g_trail_dirty_pz_max, pz_max);
}

void paint_trail_at(float wx, float wz, float intensity) {
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    float world_to_trail = (float)(TRAIL_TEX_SIZE - 1) / (2.0f * half);

    int cx = (int)((wx + half) * world_to_trail);
    int cz = (int)((wz + half) * world_to_trail);
    int brush_cells = (int)(TRAIL_BRUSH_RADIUS * world_to_trail) + 1;
    float cell_world_size = 1.0f / world_to_trail;

    for (int dz = -brush_cells; dz <= brush_cells; dz++) {
        for (int dx = -brush_cells; dx <= brush_cells; dx++) {
            int tx = cx + dx, tz = cz + dz;
            if (tx < 0 || tx >= TRAIL_TEX_SIZE || tz < 0 || tz >= TRAIL_TEX_SIZE) continue;

            float dist = sqrtf((float)(dx*dx + dz*dz)) * cell_world_size;
            if (dist > TRAIL_BRUSH_RADIUS) continue;

            float falloff = 1.0f - dist / TRAIL_BRUSH_RADIUS;
            falloff = falloff * falloff;

            int idx = tz * TRAIL_TEX_SIZE + tx;
            g_trail[idx] = std::min(1.0f, g_trail[idx] + intensity * falloff);
        }
    }
}

void update_trail_texture_region() {
    if (g_trail_dirty_px_min > g_trail_dirty_px_max) return;  // nothing dirty
    update_trail_texture_region_impl(g_trail_dirty_px_min, g_trail_dirty_pz_min,
                                     g_trail_dirty_px_max, g_trail_dirty_pz_max);
}

float trail_depression_at(float wx, float wz) {
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    float world_to_trail = (float)(TRAIL_TEX_SIZE - 1) / (2.0f * half);

    float tx = (wx + half) * world_to_trail;
    float tz = (wz + half) * world_to_trail;

    int ix = (int)tx, iz = (int)tz;
    if (ix < 0 || ix >= TRAIL_TEX_SIZE - 1 || iz < 0 || iz >= TRAIL_TEX_SIZE - 1)
        return 0.0f;

    float fx = tx - (float)ix, fz = tz - (float)iz;
    float t00 = g_trail[iz * TRAIL_TEX_SIZE + ix];
    float t10 = g_trail[iz * TRAIL_TEX_SIZE + ix + 1];
    float t01 = g_trail[(iz + 1) * TRAIL_TEX_SIZE + ix];
    float t11 = g_trail[(iz + 1) * TRAIL_TEX_SIZE + ix + 1];

    float t = t00 * (1-fx) * (1-fz) + t10 * fx * (1-fz) + t01 * (1-fx) * fz + t11 * fx * fz;
    return t * TRAIL_MAX_DEPRESSION;
}

float effective_terrain_height(float wx, float wz) {
    return terrain_height(wx, wz) - trail_depression_at(wx, wz);
}

void paint_struct_at(float wx, float wy, float wz, float intensity) {
    if (g_struct_verts.empty() || g_struct_grid.empty()) return;

    float r2 = STRUCT_BRUSH_RADIUS * STRUCT_BRUSH_RADIUS;
    bool any_changed = false;

    int search_cells = (int)(STRUCT_BRUSH_RADIUS / STRUCT_GRID_CELL) + 1;
    int center_cx = (int)((wx + STRUCT_GRID_DIM * STRUCT_GRID_CELL * 0.5f) / STRUCT_GRID_CELL);
    int center_cz = (int)((wz + STRUCT_GRID_DIM * STRUCT_GRID_CELL * 0.5f) / STRUCT_GRID_CELL);

    for (int dz = -search_cells; dz <= search_cells; dz++) {
        for (int dcx = -search_cells; dcx <= search_cells; dcx++) {
            int cx = center_cx + dcx, cz = center_cz + dz;
            if (cx < 0 || cx >= STRUCT_GRID_DIM || cz < 0 || cz >= STRUCT_GRID_DIM) continue;
            int cell = cz * STRUCT_GRID_DIM + cx;

            for (uint32_t i : g_struct_grid[cell]) {
                ColorVertex& v = g_struct_verts[i];
                float ddx = v.px - wx, ddz = v.pz - wz;
                float h2 = ddx*ddx + ddz*ddz;
                if (h2 > r2) continue;

                float dy = v.py - wy;
                if (dy < -0.5f || dy > STRUCT_VERTICAL_REACH) continue;
                float vert_frac = (dy < 0.0f) ? 1.0f : 1.0f - dy / STRUCT_VERTICAL_REACH;
                vert_frac = std::max(0.0f, vert_frac);

                float hdist = sqrtf(h2);
                float hfalloff = 1.0f - hdist / STRUCT_BRUSH_RADIUS;
                hfalloff = hfalloff * hfalloff;

                float wear_add = intensity * hfalloff * vert_frac;
                g_struct_wear[i] = std::min(1.0f, g_struct_wear[i] + wear_add);

                float w = g_struct_wear[i];
                const ColorVertex& orig = g_struct_verts_orig[i];

                float avg = (orig.r + orig.g + orig.b) / 3.0f;
                float desat = STRUCT_DESAT_AMOUNT * w;
                float cr = orig.r * (1.0f - desat) + avg * desat;
                float cg = orig.g * (1.0f - desat) + avg * desat;
                float cb = orig.b * (1.0f - desat) + avg * desat;

                float darken_val = 1.0f - w * STRUCT_MAX_DARKEN;
                cr *= darken_val; cg *= darken_val; cb *= darken_val;

                float warm = w * 0.06f;
                cr += warm * 0.4f;
                cg += warm * 0.1f;

                v.r = std::clamp(cr, 0.0f, 1.0f);
                v.g = std::clamp(cg, 0.0f, 1.0f);
                v.b = std::clamp(cb, 0.0f, 1.0f);
                any_changed = true;
            }
        }
    }

    if (any_changed) g_struct_wear_dirty = true;
}

void build_struct_grid() {
    g_struct_grid.clear();
    g_struct_grid.resize(STRUCT_GRID_DIM * STRUCT_GRID_DIM);
    for (uint32_t i = 0; i < (uint32_t)g_struct_verts_orig.size(); i++) {
        int cell = struct_grid_cell(g_struct_verts_orig[i].px, g_struct_verts_orig[i].pz);
        g_struct_grid[cell].push_back(i);
    }
}

void update_trail(float prev_x, float prev_z, float curr_x, float curr_z) {
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    if (fabsf(curr_x) > half - 5.0f || fabsf(curr_z) > half - 5.0f) return;

    float dx = curr_x - prev_x;
    float dz = curr_z - prev_z;
    float moved = sqrtf(dx*dx + dz*dz);

    if (moved < 0.05f) return;

    // Send trail walk to server for persistence + broadcast
    float trail_msg[4] = { prev_x, prev_z, curr_x, curr_z };
    net_send(C2S_TRAIL_WALK, trail_msg, 16);

    int steps = (int)(moved / 0.3f) + 1;
    float step_intensity = (moved / (float)steps) * TRAIL_PAINT_PER_UNIT;
    float struct_step_intensity = (moved / (float)steps) * STRUCT_WEAR_PER_UNIT;

    for (int s = 0; s <= steps; s++) {
        float t = (float)s / (float)steps;
        float sx = prev_x + dx * t;
        float sz = prev_z + dz * t;
        paint_trail_at(sx, sz, step_intensity);
        float sy = effective_terrain_height(sx, sz);
        paint_struct_at(sx, sy, sz, struct_step_intensity);
    }

    float world_to_trail = (float)(TRAIL_TEX_SIZE - 1) / (2.0f * half);
    int margin = (int)(TRAIL_BRUSH_RADIUS * world_to_trail) + 2;
    float min_x = std::min(prev_x, curr_x);
    float max_x = std::max(prev_x, curr_x);
    float min_z = std::min(prev_z, curr_z);
    float max_z = std::max(prev_z, curr_z);

    int px_min = std::max(0, (int)((min_x + half) * world_to_trail) - margin);
    int px_max = std::min(TRAIL_TEX_SIZE - 1, (int)((max_x + half) * world_to_trail) + margin);
    int pz_min = std::max(0, (int)((min_z + half) * world_to_trail) - margin);
    int pz_max = std::min(TRAIL_TEX_SIZE - 1, (int)((max_z + half) * world_to_trail) + margin);

    update_trail_texture_region_impl(px_min, pz_min, px_max, pz_max);
    trail_mark_dirty_region(px_min, px_max, pz_min, pz_max);

    // Copy only dirty rows to staging buffer
    if (g_trail_tex_staging_mapped) {
        uint8_t* dst = (uint8_t*)g_trail_tex_staging_mapped;
        for (int z = pz_min; z <= pz_max; z++) {
            size_t row_off = (size_t)z * TRAIL_TEX_SIZE * 4 + (size_t)px_min * 4;
            size_t row_len = (size_t)(px_max - px_min + 1) * 4;
            memcpy(dst + row_off, g_terrain_pixels.data() + row_off, row_len);
        }
    }
    g_trail_tex_dirty = true;

    g_mesh_update_counter++;
    if (g_mesh_update_counter >= MESH_UPDATE_INTERVAL) {
        g_mesh_update_counter = 0;

        int N = GRID_SIZE;
        float grid_to_world = GRID_SCALE;
        float grid_half = (N - 1) * grid_to_world * 0.5f;

        // Convert trail pixel dirty region to grid vertex range (with margin for normals)
        float trail_to_world = (2.0f * grid_half) / (float)(TRAIL_TEX_SIZE - 1);
        float world_min_x = g_trail_dirty_px_min * trail_to_world - grid_half;
        float world_max_x = g_trail_dirty_px_max * trail_to_world - grid_half;
        float world_min_z = g_trail_dirty_pz_min * trail_to_world - grid_half;
        float world_max_z = g_trail_dirty_pz_max * trail_to_world - grid_half;

        int gx_min = std::max(0, (int)((world_min_x + grid_half) / grid_to_world) - 1);
        int gx_max = std::min(N - 1, (int)((world_max_x + grid_half) / grid_to_world) + 2);
        int gz_min = std::max(0, (int)((world_min_z + grid_half) / grid_to_world) - 1);
        int gz_max = std::min(N - 1, (int)((world_max_z + grid_half) / grid_to_world) + 2);

        for (int z = gz_min; z <= gz_max; z++) {
            for (int x = gx_min; x <= gx_max; x++) {
                float wx = x * grid_to_world - grid_half;
                float wz = z * grid_to_world - grid_half;
                float depression = trail_depression_at(wx, wz);
                g_terrain_verts[z * N + x].py = g_original_heights[z * N + x] - depression;
            }
        }

        for (int z = gz_min; z <= gz_max; z++)
            for (int x = gx_min; x <= gx_max; x++) {
                float h  = g_terrain_verts[z*N+x].py;
                float hx = (x < N-1) ? g_terrain_verts[z*N+x+1].py : h;
                float hz = (z < N-1) ? g_terrain_verts[(z+1)*N+x].py : h;
                float nx = h-hx, ny = GRID_SCALE, nz = h-hz;
                float l = sqrtf(nx*nx+ny*ny+nz*nz);
                Vertex& v = g_terrain_verts[z*N+x];
                v.nx = nx/l; v.ny = ny/l; v.nz = nz/l;
            }

        // Only copy dirty rows to staging
        if (g_terrain_vert_staging_mapped) {
            uint8_t* dst = (uint8_t*)g_terrain_vert_staging_mapped;
            const uint8_t* src = (const uint8_t*)g_terrain_verts.data();
            size_t row_bytes = (size_t)N * sizeof(Vertex);
            for (int z = gz_min; z <= gz_max; z++) {
                size_t off = (size_t)z * row_bytes;
                memcpy(dst + off, src + off, row_bytes);
            }
        }
        g_mesh_dirty_gz_min = std::min(g_mesh_dirty_gz_min, gz_min);
        g_mesh_dirty_gz_max = std::max(g_mesh_dirty_gz_max, gz_max);
        g_terrain_mesh_dirty = true;
    }

    if (g_struct_wear_dirty && g_struct_vert_staging_mapped && !g_struct_verts.empty()) {
        memcpy(g_struct_vert_staging_mapped, g_struct_verts.data(),
               g_struct_verts.size() * sizeof(ColorVertex));
    }

    g_last_trail_x = curr_x;
    g_last_trail_z = curr_z;
}

void apply_remote_trail_walk(float prev_x, float prev_z, float curr_x, float curr_z) {
    float dx = curr_x - prev_x;
    float dz = curr_z - prev_z;
    float moved = sqrtf(dx*dx + dz*dz);
    if (moved < 0.05f) return;

    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    int steps = (int)(moved / 0.3f) + 1;
    float step_intensity = (moved / (float)steps) * TRAIL_PAINT_PER_UNIT;
    float struct_step_intensity = (moved / (float)steps) * STRUCT_WEAR_PER_UNIT;

    for (int s = 0; s <= steps; s++) {
        float t = (float)s / (float)steps;
        float sx = prev_x + dx * t;
        float sz = prev_z + dz * t;
        paint_trail_at(sx, sz, step_intensity);
        float sy = effective_terrain_height(sx, sz);
        paint_struct_at(sx, sy, sz, struct_step_intensity);
    }

    float world_to_trail = (float)(TRAIL_TEX_SIZE - 1) / (2.0f * half);
    int margin = (int)(TRAIL_BRUSH_RADIUS * world_to_trail) + 2;
    float min_x = std::min(prev_x, curr_x);
    float max_x = std::max(prev_x, curr_x);
    float min_z = std::min(prev_z, curr_z);
    float max_z = std::max(prev_z, curr_z);

    int px_min = std::max(0, (int)((min_x + half) * world_to_trail) - margin);
    int px_max = std::min(TRAIL_TEX_SIZE - 1, (int)((max_x + half) * world_to_trail) + margin);
    int pz_min = std::max(0, (int)((min_z + half) * world_to_trail) - margin);
    int pz_max = std::min(TRAIL_TEX_SIZE - 1, (int)((max_z + half) * world_to_trail) + margin);

    update_trail_texture_region_impl(px_min, pz_min, px_max, pz_max);
    trail_mark_dirty_region(px_min, px_max, pz_min, pz_max);

    // Copy only dirty rows to staging buffer
    if (g_trail_tex_staging_mapped) {
        uint8_t* dst = (uint8_t*)g_trail_tex_staging_mapped;
        for (int z = pz_min; z <= pz_max; z++) {
            size_t row_off = (size_t)z * TRAIL_TEX_SIZE * 4 + (size_t)px_min * 4;
            size_t row_len = (size_t)(px_max - px_min + 1) * 4;
            memcpy(dst + row_off, g_terrain_pixels.data() + row_off, row_len);
        }
    }
    g_trail_tex_dirty = true;
}

void apply_full_trail_data(const uint8_t* data, uint32_t tex_size) {
    // Decode uint8 trail data into g_trail float array
    size_t count = (size_t)tex_size * tex_size;
    for (size_t i = 0; i < count; i++)
        g_trail[i] = (float)data[i] / 255.0f;

    printf("Received trail data (%ux%u)\n", tex_size, tex_size);

    // Rebuild the entire terrain texture from trail map
    update_trail_texture_region_impl(0, 0, TRAIL_TEX_SIZE - 1, TRAIL_TEX_SIZE - 1);

    if (g_trail_tex_staging_mapped)
        memcpy(g_trail_tex_staging_mapped, g_terrain_pixels.data(),
               (size_t)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE * 4);
    g_trail_tex_dirty = true;

    // Rebuild terrain mesh depression
    int N = GRID_SIZE;
    float grid_to_world = GRID_SCALE;
    float grid_half = (N - 1) * grid_to_world * 0.5f;

    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float wx = x * grid_to_world - grid_half;
            float wz = z * grid_to_world - grid_half;
            float depression = trail_depression_at(wx, wz);
            g_terrain_verts[z * N + x].py = g_original_heights[z * N + x] - depression;
        }

    for (int z = 0; z < N; z++)
        for (int x = 0; x < N; x++) {
            float h  = g_terrain_verts[z*N+x].py;
            float hx = (x < N-1) ? g_terrain_verts[z*N+x+1].py : h;
            float hz = (z < N-1) ? g_terrain_verts[(z+1)*N+x].py : h;
            float nx = h-hx, ny = GRID_SCALE, nz = h-hz;
            float l = sqrtf(nx*nx+ny*ny+nz*nz);
            Vertex& v = g_terrain_verts[z*N+x];
            v.nx = nx/l; v.ny = ny/l; v.nz = nz/l;
        }

    if (g_terrain_vert_staging_mapped)
        memcpy(g_terrain_vert_staging_mapped, g_terrain_verts.data(),
               g_terrain_verts.size() * sizeof(Vertex));
    g_mesh_dirty_gz_min = 0;
    g_mesh_dirty_gz_max = GRID_SIZE - 1;
    g_terrain_mesh_dirty = true;
}
