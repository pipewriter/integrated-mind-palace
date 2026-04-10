// ================================================================
// Variance-driven procedural generation
//
// Simplex noise, FBM, domain warping, and channel-based sampling.
// Used to control terrain character, structure placement, and plant
// distribution across the world map.
// ================================================================
#pragma once

#include <cstdint>
#include <vector>

// 3D simplex noise — hash-based, stateless
double simplex3(double xin, double yin, double zin, uint32_t seed);

// Fractal Brownian Motion — layered octaves of simplex noise
double fbm3(double x, double y, double z, uint32_t seed,
            int octaves = 6, double scale = 100.0,
            double lacunarity = 2.0, double persistence = 0.5);

// FBM with per-octave domain warping for organic distortion
double fbm_simplex_derived(double x, double z, uint32_t seed,
                           int octaves = 6, double scale = 100.0,
                           double lacunarity = 2.0, double persistence = 0.5,
                           double warp_strength = 0.5);

// Noise-driven coordinate distortion
void domain_warp(double& x, double& y, double& z,
                 uint32_t seed, double strength = 50.0, double scale = 100.0);

// Non-linear transforms
double smoothstep_d(double edge0, double edge1, double x);
double sigmoid(double x, double center, double sharpness);
double quantize(double x, int levels);
double remap(double val, double in_lo, double in_hi, double out_lo, double out_hi);
double power_curve(double x, double exponent);

// Configuration for one noise channel
struct Channel {
    uint32_t seed_offset;
    int      octaves;
    double   scale;          // world units per noise period
    double   lacunarity;     // frequency multiplier per octave
    double   persistence;    // amplitude decay per octave
    double   warp_strength;
    double   warp_scale;
};

// Multi-channel noise sampler — samples all channels at a world position
struct VarianceSampler {
    uint32_t seed;
    std::vector<Channel> channels;

    std::vector<double> sample(double x, double y, double z) const;
    double sample_channel(double x, double y, double z, int index) const;
};

// Create the default 8-channel sampler used by terrain/structures/plants
VarianceSampler make_default_sampler(uint32_t seed = 42);
