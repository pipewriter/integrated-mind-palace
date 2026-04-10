#pragma once

#include <cstdint>

// Reset the dirty region tracking to "nothing dirty"
void trail_reset_dirty_region();

// Mark a region of the trail texture as dirty
void trail_mark_dirty_region(int px_min, int px_max, int pz_min, int pz_max);

// Paint trail at a world position with given intensity
void paint_trail_at(float wx, float wz, float intensity);

// Update the trail texture GPU data in the dirty region
void update_trail_texture_region();

// Get trail depression value at world coordinates (bilinear interpolation)
float trail_depression_at(float wx, float wz);

// Get effective terrain height after trail depression
float effective_terrain_height(float wx, float wz);

// Paint structure wear near a world position
void paint_struct_at(float wx, float wy, float wz, float intensity);

// Build the structure wear spatial grid
void build_struct_grid();

// Update trail + wear from player walking
void update_trail(float prev_x, float prev_z, float curr_x, float curr_z);

// Apply a remote player's trail walk
void apply_remote_trail_walk(float prev_x, float prev_z, float curr_x, float curr_z);

// Apply full trail data received from server
void apply_full_trail_data(const uint8_t* data, uint32_t tex_size);
