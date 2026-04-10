// ================================================================
// Skybox — procedural sky preset generation and UBO fill
//
// Generates randomized sky color/effect presets and packs them
// into the SkyUBOData structure for GPU upload.
// ================================================================
#pragma once

#include "shaders.h"
#include <random>

// Generate a random sky color preset using the given RNG
SkyPreset generate_random_preset(std::mt19937& rng);

// Linearly interpolate between two sky presets (t in [0,1])
SkyPreset lerp_preset(const SkyPreset& a, const SkyPreset& b, float t);

// Fill a SkyUBOData from a SkyPreset for GPU upload. Returns the filled struct.
SkyUBOData fill_ubo(const SkyPreset& p, float time);
