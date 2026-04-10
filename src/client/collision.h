#pragma once

struct ColTri;
struct WalkPlayer;
struct StructMesh;

// Build the collision spatial grid from g_col_tris
void build_col_grid();

// Get the grid cell range covering a circle at (wx,wz) with given radius
void col_grid_cell_range(float wx, float wz, float radius,
                         int& cx_min, int& cx_max, int& cz_min, int& cz_max);

// Push the player out of structure walls (wall-sliding collision response)
void col_wallSlide(WalkPlayer& pl);

// Extract collision triangles from a structure mesh
void extract_collision_tris(const StructMesh& mesh);
