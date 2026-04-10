// ================================================================
// Texture management — implementation
//
// Contains all texture creation, loading, caching, and BC7 encoding.
// STB_IMAGE_IMPLEMENTATION is defined here — do NOT define it
// anywhere else in the project.
// ================================================================

#include "textures.h"
#include "app.h"
#include "terrain.h"
#include "vulkan_setup.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../../vendor/stb_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sys/stat.h>

// ----------------------------------------------------------------
// File-local helpers
// ----------------------------------------------------------------

// Simple 2x box-filter downscale (halves both dimensions)
static std::vector<unsigned char> downscale_2x(const unsigned char* src, int w, int h) {
    int nw = w / 2, nh = h / 2;
    std::vector<unsigned char> dst(nw * nh * 4);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int si = ((y*2)*w + (x*2)) * 4;
            int di = (y*nw + x) * 4;
            for (int c = 0; c < 4; c++)
                dst[di+c] = (unsigned char)(((int)src[si+c] + src[si+4+c]
                            + src[si+w*4+c] + src[si+w*4+4+c]) / 4);
        }
    return dst;
}

// Max texture dimension when loading from file (keeps GPU memory manageable)
constexpr int MAX_TEXTURE_DIM = 99999; // no downscale — full quality

// ----------------------------------------------------------------
// Texture creation from raw RGBA pixel data
// ----------------------------------------------------------------

int create_texture_from_pixels(const unsigned char* pixels, int w, int h, const char* label) {
    VkDeviceSize img_size = w * h * 4;

    // Step 1-2: staging buffer with pixel data
    VkBuffer staging; VkDeviceMemory staging_mem;
    create_buffer(img_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, staging_mem);
    void* mapped;
    vkMapMemory(app.device, staging_mem, 0, img_size, 0, &mapped);
    memcpy(mapped, pixels, img_size);
    vkUnmapMemory(app.device, staging_mem);

    // Step 3: create VkImage
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_SRGB;
    ici.extent        = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    Texture tex;
    tex.width = w; tex.height = h;
    vk_check(vkCreateImage(app.device, &ici, nullptr, &tex.image), "create texture image");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(app.device, tex.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(app.device, &ai, nullptr, &tex.memory), "alloc texture mem");
    vkBindImageMemory(app.device, tex.image, tex.memory, 0);

    // Steps 4-5-6: transition → copy → transition
    transition_image_layout(tex.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(staging, tex.image, w, h);
    transition_image_layout(tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(app.device, staging, nullptr);
    vkFreeMemory(app.device, staging_mem, nullptr);

    // Step 7: image view
    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vk_check(vkCreateImageView(app.device, &vci, nullptr, &tex.view), "create texture view");

    // Step 8: sampler
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(app.device, &sci, nullptr, &tex.sampler), "create sampler");

    // Step 9: allocate descriptor set and point it at this texture
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = app.quad_desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &app.quad_desc_layout;
    vk_check(vkAllocateDescriptorSets(app.device, &dsai, &tex.desc_set), "alloc desc set");

    VkDescriptorImageInfo img_info{};
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_info.imageView   = tex.view;
    img_info.sampler     = tex.sampler;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = tex.desc_set;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &img_info;
    vkUpdateDescriptorSets(app.device, 1, &write, 0, nullptr);

    int idx = (int)app.textures.size();
    app.textures.push_back(tex);
    printf("Loaded texture %d: %s (%dx%d)\n", idx, label, w, h);
    return idx;
}

// ----------------------------------------------------------------
// Load texture from file
// ----------------------------------------------------------------

int load_texture(const char* path) {
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) { fprintf(stderr, "Failed to load image: %s\n", path); return -1; }

    // Downscale if needed to keep GPU memory in check
    std::vector<unsigned char> scaled;
    const unsigned char* src = pixels;
    while (w > MAX_TEXTURE_DIM || h > MAX_TEXTURE_DIM) {
        scaled = downscale_2x(src, w, h);
        w /= 2; h /= 2;
        src = scaled.data();
    }

    int idx = create_texture_from_pixels(src, w, h, path);
    stbi_image_free(pixels);
    return idx;
}

// ----------------------------------------------------------------
// Checkerboard test texture
// ----------------------------------------------------------------

int create_checkerboard_texture(int size, int check_size) {
    std::vector<unsigned char> pixels(size * size * 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            bool white = ((x / check_size) + (y / check_size)) % 2 == 0;
            unsigned char c = white ? 255 : 60;
            int i = (y * size + x) * 4;
            pixels[i] = c; pixels[i+1] = c; pixels[i+2] = c; pixels[i+3] = 255;
        }
    return create_texture_from_pixels(pixels.data(), size, size, "checkerboard");
}

// ----------------------------------------------------------------
// Destroy texture
// ----------------------------------------------------------------

void destroy_texture(Texture& t) {
    if (t.sampler) vkDestroySampler(app.device, t.sampler, nullptr);
    if (t.view)    vkDestroyImageView(app.device, t.view, nullptr);
    if (t.image)   vkDestroyImage(app.device, t.image, nullptr);
    if (t.memory)  vkFreeMemory(app.device, t.memory, nullptr);
    // descriptor set is freed when the pool is destroyed
    t = {};
}

// ----------------------------------------------------------------
// Image cache — stores pixel data locally to avoid retransfer
// ----------------------------------------------------------------

void save_to_cache(uint32_t hash, uint32_t w, uint32_t h, const uint8_t* pixels) {
    char path[256], tmp[256];
    snprintf(path, sizeof(path), "image_cache/%08x.rgba", hash);
    snprintf(tmp, sizeof(tmp), "image_cache/%08x.rgba.tmp", hash);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(pixels, 1, w * h * 4, f);
    fclose(f);
    rename(tmp, path);
}

bool load_from_cache(uint32_t hash, uint32_t& w, uint32_t& h, std::vector<unsigned char>& pixels) {
    char path[256];
    snprintf(path, sizeof(path), "image_cache/%08x.rgba", hash);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1) { fclose(f); return false; }
    uint32_t sz = w * h * 4;
    pixels.resize(sz);
    if (fread(pixels.data(), 1, sz, f) != sz) { fclose(f); return false; }
    fclose(f);
    return true;
}

// ----------------------------------------------------------------
// Texture deduplication via hash
// ----------------------------------------------------------------

int get_or_create_texture(uint32_t hash, uint32_t w, uint32_t h, const unsigned char* pixels) {
    auto it = g_hash_to_tex.find(hash);
    if (it != g_hash_to_tex.end()) return it->second;
    int idx = create_texture_from_pixels(pixels, w, h, "net_image");
    g_hash_to_tex[hash] = idx;
    return idx;
}

// ----------------------------------------------------------------
// Terrain mesh cache
// ----------------------------------------------------------------

void save_terrain_cache(const Mesh& mesh) {
    mkdir("cache", 0755);
    FILE* f = fopen("cache/terrain.mesh", "wb");
    if (!f) { fprintf(stderr, "Failed to write cache/terrain.mesh\n"); return; }
    uint32_t vc = (uint32_t)mesh.vertices.size();
    uint32_t ic = (uint32_t)mesh.indices.size();
    fwrite(&vc, sizeof(uint32_t), 1, f);
    fwrite(&ic, sizeof(uint32_t), 1, f);
    fwrite(mesh.vertices.data(), sizeof(Vertex), vc, f);
    fwrite(mesh.indices.data(), sizeof(uint32_t), ic, f);
    fclose(f);
}

Mesh load_terrain_cache() {
    Mesh mesh;
    FILE* f = fopen("cache/terrain.mesh", "rb");
    if (!f) { fprintf(stderr, "Failed to read cache/terrain.mesh\n"); return mesh; }
    uint32_t vc, ic;
    fread(&vc, sizeof(uint32_t), 1, f);
    fread(&ic, sizeof(uint32_t), 1, f);
    mesh.vertices.resize(vc);
    mesh.indices.resize(ic);
    fread(mesh.vertices.data(), sizeof(Vertex), vc, f);
    fread(mesh.indices.data(), sizeof(uint32_t), ic, f);
    fclose(f);
    printf("Terrain: %d verts, %d tris (from cache)\n", (int)vc, (int)ic / 3);
    return mesh;
}

// fread terrain data directly into Vulkan staging buffers — zero intermediate CPU buffer
void upload_terrain_fread(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Failed to open %s for fread upload\n", path); return; }
    uint32_t vc, ic;
    fread(&vc, sizeof(uint32_t), 1, f);
    fread(&ic, sizeof(uint32_t), 1, f);

    VkDeviceSize vsize = vc * sizeof(Vertex);
    VkDeviceSize isize = ic * sizeof(uint32_t);

    // Vertex buffer: staging → fread → GPU copy
    {
        VkBuffer staging; VkDeviceMemory staging_mem;
        create_buffer(vsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_mem);
        void* mapped;
        vkMapMemory(app.device, staging_mem, 0, vsize, 0, &mapped);
        fread(mapped, sizeof(Vertex), vc, f);
        vkUnmapMemory(app.device, staging_mem);
        create_buffer(vsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, app.terrain_vbuf, app.terrain_vmem);
        copy_buffer(staging, app.terrain_vbuf, vsize);
        vkDestroyBuffer(app.device, staging, nullptr);
        vkFreeMemory(app.device, staging_mem, nullptr);
    }

    // Index buffer: staging → fread → GPU copy
    {
        VkBuffer staging; VkDeviceMemory staging_mem;
        create_buffer(isize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_mem);
        void* mapped;
        vkMapMemory(app.device, staging_mem, 0, isize, 0, &mapped);
        fread(mapped, sizeof(uint32_t), ic, f);
        vkUnmapMemory(app.device, staging_mem);
        create_buffer(isize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, app.terrain_ibuf, app.terrain_imem);
        copy_buffer(staging, app.terrain_ibuf, isize);
        vkDestroyBuffer(app.device, staging, nullptr);
        vkFreeMemory(app.device, staging_mem, nullptr);
    }

    app.terrain_idx_count = ic;
    fclose(f);
}

// ═══════════════════════════════════════════════════════════════════════
//  BC7 encoding via bc7enc (richgel999) — optional texture compression
//  Enabled when bc7enc.h is available in vendor/ directory.
//  Build with -DHAS_BC7ENC to enable.
// ═══════════════════════════════════════════════════════════════════════

#ifdef HAS_BC7ENC
#include "../../vendor/bc7enc.h"

static bool g_bc7enc_initialized = false;
static bc7enc_compress_block_params g_bc7_params;

static void bc7enc_init_once() {
    if (g_bc7enc_initialized) return;
    bc7enc_compress_block_init();
    bc7enc_compress_block_params_init(&g_bc7_params);
    // Max quality: use all partitions + uber level 4
    g_bc7_params.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
    g_bc7_params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
    g_bc7_params.m_try_least_squares = BC7ENC_TRUE;
    // Linear weights (not perceptual) for pixel-accurate results
    bc7enc_compress_block_params_init_linear_weights(&g_bc7_params);
    g_bc7enc_initialized = true;
}

// Encode a full RGBA image to BC7. Dimensions are padded to multiples of 4.
std::vector<uint8_t> encode_image_bc7(const unsigned char* pixels, int w, int h,
                                              int& out_pw, int& out_ph) {
    bc7enc_init_once();

    int pw = (w + 3) & ~3;  // pad to multiple of 4
    int ph = (h + 3) & ~3;
    out_pw = pw;
    out_ph = ph;
    int bw = pw / 4, bh = ph / 4;
    std::vector<uint8_t> out(bw * bh * 16);

    uint8_t block[64];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int sx = std::min(bx*4+x, w-1);
                    int sy = std::min(by*4+y, h-1);
                    memcpy(block + (y*4+x)*4, pixels + (sy*w+sx)*4, 4);
                }
            bc7enc_compress_block(out.data() + (by*bw+bx)*16, block, &g_bc7_params);
        }
    return out;
}

// Create a Vulkan texture from BC7 compressed data
int create_texture_from_bc7(const uint8_t* bc7_data, uint32_t bc7_size,
                                    int w, int h, const char* label) {
    VkDeviceSize img_size = bc7_size;

    // Staging buffer with BC7 data
    VkBuffer staging; VkDeviceMemory staging_mem;
    create_buffer(img_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, staging_mem);
    void* mapped;
    vkMapMemory(app.device, staging_mem, 0, img_size, 0, &mapped);
    memcpy(mapped, bc7_data, img_size);
    vkUnmapMemory(app.device, staging_mem);

    // Create VkImage with BC7 format
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_BC7_SRGB_BLOCK;
    ici.extent        = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    Texture tex;
    tex.width = w; tex.height = h;
    vk_check(vkCreateImage(app.device, &ici, nullptr, &tex.image), "create BC7 texture image");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(app.device, tex.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(app.device, &ai, nullptr, &tex.memory), "alloc BC7 texture mem");
    vkBindImageMemory(app.device, tex.image, tex.memory, 0);

    // Transition → copy → transition
    transition_image_layout(tex.image, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_buffer_to_image(staging, tex.image, w, h);
    transition_image_layout(tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(app.device, staging, nullptr);
    vkFreeMemory(app.device, staging_mem, nullptr);

    // Image view — BC7 format
    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = VK_FORMAT_BC7_SRGB_BLOCK;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    vk_check(vkCreateImageView(app.device, &vci, nullptr, &tex.view), "create BC7 texture view");

    // Sampler
    VkSamplerCreateInfo sci{};
    sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter    = VK_FILTER_NEAREST;
    sci.minFilter    = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(app.device, &sci, nullptr, &tex.sampler), "create sampler");

    // Descriptor set
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = app.quad_desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &app.quad_desc_layout;
    vk_check(vkAllocateDescriptorSets(app.device, &dsai, &tex.desc_set), "alloc desc set");

    VkDescriptorImageInfo img_info{};
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_info.imageView   = tex.view;
    img_info.sampler     = tex.sampler;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = tex.desc_set;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &img_info;
    vkUpdateDescriptorSets(app.device, 1, &write, 0, nullptr);

    int idx = (int)app.textures.size();
    app.textures.push_back(tex);
    printf("Loaded BC7 texture %d: %s (%dx%d, %.1f KB)\n", idx, label, w, h, bc7_size/1024.0f);
    return idx;
}

#else // !HAS_BC7ENC

std::vector<uint8_t> encode_image_bc7(const unsigned char*, int, int, int&, int&) {
    return {};
}

int create_texture_from_bc7(const uint8_t*, uint32_t, int, int, const char*) {
    return -1;
}

#endif // HAS_BC7ENC

// ═══════════════════════════════════════════════════════════════════════
// Texture cache — BC7 packed atlas for fast startup
// ═══════════════════════════════════════════════════════════════════════

struct TextureCacheEntry {
    uint32_t width, height, offset, size;  // width/height are padded (mult of 4), size is BC7 bytes
};

void save_texture_cache(const std::vector<std::string>& paths) {
    mkdir("cache", 0755);
    FILE* f = fopen("cache/textures.pack", "wb");
    if (!f) { fprintf(stderr, "Failed to write cache/textures.pack\n"); return; }

    uint32_t count = (uint32_t)paths.size();
    std::vector<TextureCacheEntry> entries;
    std::vector<uint8_t> packed_data;
    uint32_t offset = 0;

    for (uint32_t i = 0; i < count; i++) {
        int w, h, channels;
        unsigned char* pixels = stbi_load(paths[i].c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels) {
            fprintf(stderr, "Warning: failed to load %s for caching, skipping\n", paths[i].c_str());
            entries.push_back({ 0, 0, offset, 0 });
            continue;
        }
        std::vector<unsigned char> scaled;
        const unsigned char* src = pixels;
        while (w > MAX_TEXTURE_DIM || h > MAX_TEXTURE_DIM) {
            scaled = downscale_2x(src, w, h);
            w /= 2; h /= 2;
            src = scaled.data();
        }

        // BC7 encode — padded dimensions stored in entry
        int pw, ph;
        auto bc7 = encode_image_bc7(src, w, h, pw, ph);
        entries.push_back({ (uint32_t)pw, (uint32_t)ph, offset, (uint32_t)bc7.size() });
        packed_data.insert(packed_data.end(), bc7.begin(), bc7.end());
        offset += (uint32_t)bc7.size();
        stbi_image_free(pixels);

        if ((i+1) % 50 == 0) printf("  [cook] %u/%u textures encoded\n", i+1, count);
    }

    fwrite(&count, sizeof(uint32_t), 1, f);
    fwrite(entries.data(), sizeof(TextureCacheEntry), count, f);
    fwrite(packed_data.data(), 1, packed_data.size(), f);
    fclose(f);
    printf("Cached %u textures as BC7 (%.1f MB packed — 25%% of raw RGBA)\n",
           count, packed_data.size() / (1024.0*1024.0));
}

void load_textures_from_cache(const std::vector<std::string>& image_paths) {
    FILE* f = fopen("cache/textures.pack", "rb");
    if (!f) { fprintf(stderr, "Failed to read cache/textures.pack\n"); return; }

    uint32_t count;
    fread(&count, sizeof(uint32_t), 1, f);
    std::vector<TextureCacheEntry> entries(count);
    fread(entries.data(), sizeof(TextureCacheEntry), count, f);

    // Calculate total data size and fread it all at once
    uint32_t total_data = 0;
    for (uint32_t i = 0; i < count; i++) total_data += entries[i].size;
    std::vector<unsigned char> data(total_data);
    fread(data.data(), 1, total_data, f);
    fclose(f);

    // Grid layout (same as non-cached path)
    const float spacing = 15.0f;
    const float quad_size = 10.0f;
    int cols = (int)std::ceil(std::sqrt((double)count));
    float x_offset = -(cols * spacing) / 2.0f;
    float z_offset = -(cols * spacing) / 2.0f;

    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].size == 0) continue; // skipped during cook
        const char* label = (i < image_paths.size()) ? image_paths[i].c_str() : "cached";
        int idx = create_texture_from_bc7(data.data() + entries[i].offset,
                                           entries[i].size,
                                           entries[i].width, entries[i].height, label);
        if (idx < 0) continue;
        int col = (int)i % cols;
        int row = (int)i / cols;
        float px = x_offset + col * spacing;
        float pz = z_offset + row * spacing;
        float py = terrain_height(px, pz) + quad_size * 0.5f + 1.0f;
        app.quads.push_back({ px, py, pz, quad_size, quad_size, 0, idx });
    }
}
