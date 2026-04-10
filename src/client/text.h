// ================================================================
// Text rendering — bitmap font atlas and glyph text blocks
//
// Provides an 8x8 bitmap font atlas and per-character quad rendering
// for placing text labels in the 3D world.
// ================================================================
#pragma once

#include <cstdint>

struct GlyphText;
struct DataNode;

// Create the 8x8 bitmap font atlas texture. Returns texture index.
int create_font_atlas_texture();

// Create a new glyph text block at a world position.
// rot_y in degrees, (r,g,b) is the text tint color, max_chars limits quads.
GlyphText* create_glyph_text(float x, float y, float z, float char_size,
                              float rot_y, float r, float g, float b,
                              int max_chars);

// Rebuild vertex/index data from gt.text
void update_glyph_text(GlyphText& gt);

// Destroy a glyph text block, freeing GPU resources
void destroy_glyph_text(GlyphText* gt);

// Create a glyph text block for a world DataNode. Returns index into app.glyph_texts.
int create_glyph_for_node(DataNode& node);
