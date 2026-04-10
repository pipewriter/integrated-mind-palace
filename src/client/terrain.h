#pragma once
#include <vector>
#include <cstdint>

// Forward declarations
struct Vertex;
struct VarianceSampler;

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Returns the terrain height at world coordinates (wx, wz)
float terrain_height(float wx, float wz);

// Returns the biome index (0-9) at world coordinates
constexpr int NUM_BIOMES = 10;
int biome_at(float wx, float wz);

// Generate the terrain height-field mesh (GRID_SIZE x GRID_SIZE vertices)
Mesh generate_terrain();

// Generate procedural terrain texture as RGBA pixels
std::vector<unsigned char> generate_terrain_texture(int size);

// Biome palette access (for structures/plants to match biome colors)
// colors[4][3] = { base_rgb, detail1_rgb, detail2_rgb, accent_rgb }
void get_biome_palette(int biome, float colors[4][3]);
float get_biome_hue(int biome);
