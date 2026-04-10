#pragma once

#include "math.h"

struct Quad;
struct GlyphText;

// Build a model matrix for a world-space textured quad
Mat4 mat4_quad_model(const Quad& q);

// Build a model matrix for a world-space glyph text block
Mat4 mat4_glyph_text_model(const GlyphText& gt);

// Render one frame (the main render loop body)
void draw_frame();
