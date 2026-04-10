// ================================================================
// Vec3 helpers — lightweight 3D vector math for L-system plants
// ================================================================
#pragma once

#include <cmath>

static const float PI_F = 3.14159265358979323846f;

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }

inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float vlen(Vec3 v) { return sqrtf(dot3(v, v)); }

inline Vec3 vnorm(Vec3 v) {
    float l = vlen(v);
    return l > 1e-8f ? v * (1.0f / l) : Vec3(0, 1, 0);
}

inline Vec3 rotateAround(Vec3 v, Vec3 axis, float angle) {
    float c = cosf(angle), s = sinf(angle);
    return v * c + cross3(axis, v) * s + axis * dot3(axis, v) * (1.0f - c);
}
