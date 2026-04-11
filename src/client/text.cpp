// ================================================================
// Text rendering — bitmap font atlas and glyph text blocks
// ================================================================

#include "text.h"
#include "app.h"
#include "types.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

// ----------------------------------------------------------------
// Vulkan helpers (declared in client.cpp, will move to vulkan_setup.h)
// ----------------------------------------------------------------
void vk_check(VkResult r, const char* msg);
uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props);
int create_texture_from_pixels(const unsigned char* pixels, int w, int h, const char* label);

// ----------------------------------------------------------------
// Font data
// ----------------------------------------------------------------
#include "../../vendor/font8x8_data.inc"

// ----------------------------------------------------------------
// create_font_atlas_texture
// ----------------------------------------------------------------

int create_font_atlas_texture() {
    constexpr int ATLAS_W = 128, ATLAS_H = 128;
    std::vector<unsigned char> pixels(ATLAS_W * ATLAS_H * 4, 0);

    for (int ch = 0; ch < 256; ch++) {
        int grid_x = ch % 16;
        int grid_y = ch / 16;
        for (int row = 0; row < 8; row++) {
            unsigned char bits = font8x8[ch][row];
            for (int col = 0; col < 8; col++) {
                bool on = (bits >> col) & 1;
                int px = grid_x * 8 + col;
                int py = grid_y * 8 + row;
                int i = (py * ATLAS_W + px) * 4;
                unsigned char v = on ? 255 : 0;
                pixels[i+0] = v;
                pixels[i+1] = v;
                pixels[i+2] = v;
                pixels[i+3] = on ? 255 : 0;
            }
        }
    }
    return create_texture_from_pixels(pixels.data(), ATLAS_W, ATLAS_H, "font_atlas_16x16");
}

// ----------------------------------------------------------------
// create_glyph_text
// ----------------------------------------------------------------

GlyphText* create_glyph_text(float x, float y, float z, float char_size,
                                     float rot_y, float r, float g, float b, int max_chars) {
    GlyphText* gt = new GlyphText{};
    gt->x = x; gt->y = y; gt->z = z;
    gt->char_size = char_size;
    gt->rot_x = 0; gt->rot_y = rot_y; gt->rot_z = 0;
    gt->tint_r = r; gt->tint_g = g; gt->tint_b = b;
    gt->max_chars = max_chars;
    gt->char_count = 0;

    VkDeviceSize vsize = max_chars * 4 * sizeof(QuadVertex);
    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = vsize; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vk_check(vkCreateBuffer(app.device, &bci, nullptr, &gt->vbuf), "glyph vbuf");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(app.device, gt->vbuf, &req);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(app.device, &ai, nullptr, &gt->vmem), "glyph vmem");
    vkBindBufferMemory(app.device, gt->vbuf, gt->vmem, 0);
    vkMapMemory(app.device, gt->vmem, 0, vsize, 0, &gt->mapped_verts);

    VkDeviceSize isize = max_chars * 6 * sizeof(uint32_t);
    bci.size = isize; bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vk_check(vkCreateBuffer(app.device, &bci, nullptr, &gt->ibuf), "glyph ibuf");
    vkGetBufferMemoryRequirements(app.device, gt->ibuf, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(app.device, &ai, nullptr, &gt->imem), "glyph imem");
    vkBindBufferMemory(app.device, gt->ibuf, gt->imem, 0);
    vkMapMemory(app.device, gt->imem, 0, isize, 0, &gt->mapped_indices);

    return gt;
}

// ----------------------------------------------------------------
// update_glyph_text
// ----------------------------------------------------------------

void update_glyph_text(GlyphText& gt) {
    QuadVertex* verts = (QuadVertex*)gt.mapped_verts;
    uint32_t* indices = (uint32_t*)gt.mapped_indices;
    int vi = 0, ii = 0;
    int col = 0, row = 0;

    for (size_t i = 0; i < gt.text.size(); i++) {
        char ch = gt.text[i];
        if (ch == '\n') { row++; col = 0; continue; }
        if (vi / 4 >= gt.max_chars) break;

        float left   = (float)col;
        float right  = (float)(col + 1);
        float top    = -(float)row;
        float bottom = -(float)(row + 1);

        int ax = (unsigned char)ch % 16;
        int ay = (unsigned char)ch / 16;
        float u0 = ax * (1.0f / 16.0f);
        float v0 = ay * (1.0f / 16.0f);
        float u1 = (ax + 1) * (1.0f / 16.0f);
        float v1 = (ay + 1) * (1.0f / 16.0f);

        uint32_t base = (uint32_t)vi;
        verts[vi++] = { left,  bottom, 0, u0, v1 };
        verts[vi++] = { right, bottom, 0, u1, v1 };
        verts[vi++] = { right, top,    0, u1, v0 };
        verts[vi++] = { left,  top,    0, u0, v0 };

        indices[ii++] = base;
        indices[ii++] = base + 1;
        indices[ii++] = base + 2;
        indices[ii++] = base + 2;
        indices[ii++] = base + 3;
        indices[ii++] = base;

        col++;
    }
    gt.char_count = vi / 4;
}

// ----------------------------------------------------------------
// destroy_glyph_text
// ----------------------------------------------------------------

void destroy_glyph_text(GlyphText* gt) {
    if (!gt) return;
    // Wait for GPU to finish using these buffers before destroying them
    vkDeviceWaitIdle(app.device);
    if (gt->mapped_verts)   vkUnmapMemory(app.device, gt->vmem);
    if (gt->mapped_indices) vkUnmapMemory(app.device, gt->imem);
    if (gt->vbuf) vkDestroyBuffer(app.device, gt->vbuf, nullptr);
    if (gt->vmem) vkFreeMemory(app.device, gt->vmem, nullptr);
    if (gt->ibuf) vkDestroyBuffer(app.device, gt->ibuf, nullptr);
    if (gt->imem) vkFreeMemory(app.device, gt->imem, nullptr);
    delete gt;
}

// ----------------------------------------------------------------
// create_glyph_for_node
// ----------------------------------------------------------------

int create_glyph_for_node(DataNode& node) {
    Camera& c = app.cam;
    GlyphText* gt = create_glyph_text(node.x, node.y, node.z, node.w > 0 ? node.w * 0.1f : 0.5f,
                                       node.rot_y, 1.0f, 1.0f, 1.0f, 512);
    gt->rot_x = node.rot_x;
    gt->rot_z = node.rot_z;
    gt->text = node.text;
    update_glyph_text(*gt);
    int idx = (int)app.glyph_texts.size();
    app.glyph_texts.push_back(gt);
    return idx;
}
