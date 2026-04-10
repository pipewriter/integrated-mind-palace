// ================================================================
// Variance-driven procedural generation — implementation
// ================================================================

#include "variance.h"
#include <cmath>
#include <algorithm>

// ================================================================
// Simplex Noise 3D — hash-based, stateless, pure function
// ================================================================

namespace {

constexpr double GRAD3[][3] = {
    { 1, 1, 0}, {-1, 1, 0}, { 1,-1, 0}, {-1,-1, 0},
    { 1, 0, 1}, {-1, 0, 1}, { 1, 0,-1}, {-1, 0,-1},
    { 0, 1, 1}, { 0,-1, 1}, { 0, 1,-1}, { 0,-1,-1}
};

inline int fast_floor(double x) {
    int xi = static_cast<int>(x);
    return (x < xi) ? xi - 1 : xi;
}

inline uint32_t hash_3d(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed;
    h += static_cast<uint32_t>(x) * 374761393u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h += static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h += static_cast<uint32_t>(z) * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline double grad_dot(uint32_t hash, double x, double y, double z) {
    const auto& g = GRAD3[hash % 12];
    return g[0] * x + g[1] * y + g[2] * z;
}

} // namespace

double simplex3(double xin, double yin, double zin, uint32_t seed) {
    constexpr double F3 = 1.0 / 3.0;
    constexpr double G3 = 1.0 / 6.0;

    double s = (xin + yin + zin) * F3;
    int i = fast_floor(xin + s);
    int j = fast_floor(yin + s);
    int k = fast_floor(zin + s);

    double t = (i + j + k) * G3;
    double x0 = xin - (i - t);
    double y0 = yin - (j - t);
    double z0 = zin - (k - t);

    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0)      { i1=1; j1=0; k1=0; i2=1; j2=1; k2=0; }
        else if (x0 >= z0) { i1=1; j1=0; k1=0; i2=1; j2=0; k2=1; }
        else               { i1=0; j1=0; k1=1; i2=1; j2=0; k2=1; }
    } else {
        if (y0 < z0)       { i1=0; j1=0; k1=1; i2=0; j2=1; k2=1; }
        else if (x0 < z0)  { i1=0; j1=1; k1=0; i2=0; j2=1; k2=1; }
        else               { i1=0; j1=1; k1=0; i2=1; j2=1; k2=0; }
    }

    double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
    double x2 = x0 - i2 + 2.0 * G3, y2 = y0 - j2 + 2.0 * G3, z2 = z0 - k2 + 2.0 * G3;
    double x3 = x0 - 1.0 + 3.0 * G3, y3 = y0 - 1.0 + 3.0 * G3, z3 = z0 - 1.0 + 3.0 * G3;

    double n0 = 0, n1 = 0, n2 = 0, n3 = 0;

    double t0 = 0.6 - x0*x0 - y0*y0 - z0*z0;
    if (t0 > 0) { t0 *= t0; n0 = t0 * t0 * grad_dot(hash_3d(i, j, k, seed), x0, y0, z0); }

    double t1 = 0.6 - x1*x1 - y1*y1 - z1*z1;
    if (t1 > 0) { t1 *= t1; n1 = t1 * t1 * grad_dot(hash_3d(i+i1, j+j1, k+k1, seed), x1, y1, z1); }

    double t2 = 0.6 - x2*x2 - y2*y2 - z2*z2;
    if (t2 > 0) { t2 *= t2; n2 = t2 * t2 * grad_dot(hash_3d(i+i2, j+j2, k+k2, seed), x2, y2, z2); }

    double t3 = 0.6 - x3*x3 - y3*y3 - z3*z3;
    if (t3 > 0) { t3 *= t3; n3 = t3 * t3 * grad_dot(hash_3d(i+1, j+1, k+1, seed), x3, y3, z3); }

    return 32.0 * (n0 + n1 + n2 + n3);
}

// ================================================================
// Fractal Brownian Motion
// ================================================================

double fbm3(double x, double y, double z, uint32_t seed,
            int octaves, double scale, double lacunarity, double persistence) {
    double total = 0.0, amplitude = 1.0, frequency = 1.0 / scale, max_amp = 0.0;
    for (int o = 0; o < octaves; o++) {
        total += amplitude * simplex3(x * frequency, y * frequency, z * frequency, seed + o);
        max_amp += amplitude;
        frequency *= lacunarity;
        amplitude *= persistence;
    }
    return total / max_amp;
}

// ================================================================
// Simplex-derived FBM — each octave warped by unique simplex fields
// ================================================================

double fbm_simplex_derived(double x, double z, uint32_t seed,
                           int octaves, double scale,
                           double lacunarity, double persistence,
                           double warp_strength) {
    double total = 0.0, amplitude = 1.0, frequency = 1.0 / scale, max_amp = 0.0;
    for (int o = 0; o < octaves; o++) {
        double wx = simplex3(x * frequency * 0.7, z * frequency * 0.7, 0.0,
                             seed + o * 1000 + 100);
        double wz = simplex3(x * frequency * 0.7, z * frequency * 0.7, 0.0,
                             seed + o * 1000 + 200);
        double val = simplex3(x * frequency + wx * warp_strength,
                              z * frequency + wz * warp_strength,
                              0.0, seed + o);
        total += amplitude * val;
        max_amp += amplitude;
        frequency *= lacunarity;
        amplitude *= persistence;
    }
    return total / max_amp;
}

// ================================================================
// Domain Warping
// ================================================================

void domain_warp(double& x, double& y, double& z,
                 uint32_t seed, double strength, double scale) {
    double wx = fbm3(x, y, z, seed + 1000, 3, scale);
    double wy = fbm3(x, y, z, seed + 2000, 3, scale);
    double wz = fbm3(x, y, z, seed + 3000, 3, scale);
    x += wx * strength;
    y += wy * strength;
    z += wz * strength;
}

// ================================================================
// Non-linear Transforms
// ================================================================

double smoothstep_d(double edge0, double edge1, double x) {
    double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double sigmoid(double x, double center, double sharpness) {
    return 1.0 / (1.0 + std::exp(-sharpness * (x - center)));
}

double quantize(double x, int levels) {
    if (levels < 2) return x;
    double c = std::clamp(x, 0.0, 1.0);
    int bin = std::min(static_cast<int>(c * levels), levels - 1);
    return static_cast<double>(bin) / (levels - 1);
}

double remap(double val, double in_lo, double in_hi, double out_lo, double out_hi) {
    double t = std::clamp((val - in_lo) / (in_hi - in_lo), 0.0, 1.0);
    return out_lo + t * (out_hi - out_lo);
}

double power_curve(double x, double exponent) {
    return std::pow(std::clamp(x, 0.0, 1.0), exponent);
}

// ================================================================
// Channel Sampler
// ================================================================

std::vector<double> VarianceSampler::sample(double x, double y, double z) const {
    std::vector<double> values;
    values.reserve(channels.size());
    for (int i = 0; i < static_cast<int>(channels.size()); i++)
        values.push_back(sample_channel(x, y, z, i));
    return values;
}

double VarianceSampler::sample_channel(double x, double y, double z, int index) const {
    const auto& ch = channels[index];
    double wx = x, wy = y, wz = z;
    if (ch.warp_strength > 0.0)
        domain_warp(wx, wy, wz, seed + ch.seed_offset + 5000,
                    ch.warp_strength, ch.warp_scale);
    return fbm3(wx, wy, wz, seed + ch.seed_offset,
                 ch.octaves, ch.scale, ch.lacunarity, ch.persistence);
}

VarianceSampler make_default_sampler(uint32_t seed) {
    return {seed, {
        //  seed_off  oct   scale    lac   pers   warp_str  warp_scl
        {   0,        8,    400.0,  2.0,  0.55,   80.0,    200.0 },
        { 100,        4,    800.0,  2.0,  0.45,   40.0,    400.0 },
        { 200,        5,    500.0,  2.0,  0.50,   60.0,    250.0 },
        { 300,        3,   1200.0,  2.5,  0.35,  120.0,    600.0 },
        { 400,        6,    200.0,  2.0,  0.50,   30.0,    100.0 },
        { 500,        7,    300.0,  2.0,  0.48,   50.0,    150.0 },
        { 600,        4,   1000.0,  2.0,  0.40,    0.0,      0.0 },
        { 700,        9,    150.0,  2.0,  0.52,   20.0,     75.0 },
    }};
}
