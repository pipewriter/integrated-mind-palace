#pragma once
#include <vector>
#include <string>
#include <map>
#include <cstdint>

struct ColorVertex;
struct Vec3;

struct StructMesh {
    std::vector<ColorVertex> vertices;
    std::vector<uint32_t> indices;

    void addTri(ColorVertex a, ColorVertex b, ColorVertex c);
    void addQuad(ColorVertex a, ColorVertex b, ColorVertex c, ColorVertex d);
    void addTriRaw(ColorVertex a, ColorVertex b, ColorVertex c);
    void addBox(float cx, float cy, float cz, float hx, float hy, float hz,
                float cr, float cg, float cb);
    void addCylinder(float cx, float cy, float cz, float r, float hh, int segs,
                     float cr, float cg, float cb);
    void addCone(float cx, float cy, float cz, float r_bot, float r_top, float hh, int segs,
                 float cr, float cg, float cb);
    void addRoof(float cx, float cy, float cz, float hw, float hd, float ph,
                 float cr, float cg, float cb);
    void merge(const StructMesh& other, float ox, float oy, float oz, float rotY = 0.0f);
};

// Generate all buildings (temples, houses, towers, pyramids) placed on terrain
StructMesh generate_structures();

// Generate all L-system plants placed on terrain
StructMesh generate_plants();
