// ================================================================
// Texture management — loading, creation, caching, BC7 encoding
//
// All GPU texture lifecycle: create from pixels, load from file,
// checkerboard generation, destruction, and disk caching (RGBA
// image cache + BC7 texture atlas).
// ================================================================
#pragma once

#include <cstdint>
#include <vector>
#include <string>

// Forward declarations
struct Texture;
struct Mesh;

// Create a GPU texture from RGBA pixel data. Returns texture index in app.textures.
int create_texture_from_pixels(const unsigned char* pixels, int w, int h, const char* label);

// Load texture from an image file (png, jpg, bmp, etc.). Returns texture index or -1 on failure.
int load_texture(const char* path);

// Generate a checkerboard test texture (no file needed). Returns texture index.
int create_checkerboard_texture(int size, int check_size);

// Destroy a texture, freeing GPU resources.
void destroy_texture(Texture& t);

// Image cache — save/load RGBA pixel data to disk for deduplication
void save_to_cache(uint32_t hash, uint32_t w, uint32_t h, const uint8_t* pixels);
bool load_from_cache(uint32_t hash, uint32_t& w, uint32_t& h, std::vector<unsigned char>& pixels);

// Get or create a texture from hash + pixel data (deduplication via g_hash_to_tex)
int get_or_create_texture(uint32_t hash, uint32_t w, uint32_t h, const unsigned char* pixels);

// Terrain mesh cache — save/load binary mesh to disk
void save_terrain_cache(const Mesh& mesh);
Mesh load_terrain_cache();

// Upload terrain mesh directly from cache file into Vulkan staging buffers (zero-copy)
void upload_terrain_fread(const char* path);

// BC7 texture encoding
std::vector<uint8_t> encode_image_bc7(const unsigned char* pixels, int w, int h,
                                       int& out_pw, int& out_ph);

// Create a Vulkan texture from BC7 compressed data. Returns texture index.
int create_texture_from_bc7(const uint8_t* bc7_data, uint32_t bc7_size,
                             int w, int h, const char* label);

// Texture atlas save/load for fast startup (BC7 packed format)
void save_texture_cache(const std::vector<std::string>& paths);
void load_textures_from_cache(const std::vector<std::string>& image_paths);
