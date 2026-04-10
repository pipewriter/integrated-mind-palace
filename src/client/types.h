// ================================================================
// Core type definitions — vertex formats, world entities, camera
//
// Defines the data structures shared across all client subsystems.
// ================================================================
#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>

#include "../shared/net.h"

// ----------------------------------------------------------------
// Vertex formats (GPU-side vertex attributes)
// ----------------------------------------------------------------

// Terrain vertex: position + normal + per-vertex color
struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float cr, cg, cb;
};

// Textured quad vertex: position + UV coordinates
struct QuadVertex {
    float px, py, pz;
    float u, v;
};

// Structure/plant vertex: position + normal + RGB color
struct ColorVertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
};

// ----------------------------------------------------------------
// Push constants — shared by terrain, quad, and structure pipelines
// ----------------------------------------------------------------

// MVP(64) + camera_pos(16) + fog_on(4) + highlight(4) = 88 bytes
struct PushConstants {
    float mvp[16];
    float cam_pos[4];
    float fog_on;       // 1.0 = fog enabled, 0.0 = disabled
    float highlight;    // 0.0 = normal, 1.0 = quad highlight, 2.0 = text highlight
};

// ----------------------------------------------------------------
// Texture handle — GPU image + descriptor set ready to bind
// ----------------------------------------------------------------

struct Texture {
    VkImage        image    = VK_NULL_HANDLE;
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    VkImageView    view     = VK_NULL_HANDLE;
    VkSampler      sampler  = VK_NULL_HANDLE;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    int width = 0, height = 0;
};

// ----------------------------------------------------------------
// World-space textured quad (images placed in the world)
// ----------------------------------------------------------------

struct Quad {
    float x, y, z;              // center position
    float w, h;                 // width, height in world units
    float rot_x, rot_y, rot_z;  // Euler angles (degrees): applied as Ry * Rx * Rz
    int   texture_index;         // which texture to use
};

// ----------------------------------------------------------------
// Glyph text block — per-character quads with atlas UVs
// ----------------------------------------------------------------

struct GlyphText {
    float x, y, z;                  // world position
    float char_size;                // world-space character cell size
    float rot_x, rot_y, rot_z;     // rotation (degrees)
    float tint_r, tint_g, tint_b;  // text color
    bool is_hud = false;            // true = render in screen space

    VkBuffer       vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkBuffer       ibuf = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    void*          mapped_verts   = nullptr;
    void*          mapped_indices = nullptr;
    int            max_chars = 0;
    int            char_count = 0;

    std::string    text;
};

// ----------------------------------------------------------------
// Camera
// ----------------------------------------------------------------

struct Camera {
    float x = 0, y = 60, z = 0;
    float yaw = -90.0f, pitch = -10.0f;
    float speed = 25.0f, sensitivity = 0.08f;
    float fx, fy, fz, rx, ry, rz;

    void update_vectors() {
        constexpr float D = 3.14159265f / 180.0f;
        float cy=cosf(yaw*D), sy=sinf(yaw*D), cp=cosf(pitch*D), sp=sinf(pitch*D);
        fx=cp*cy; fy=sp; fz=cp*sy; rx=-sy; ry=0; rz=cy;
    }
};

// ----------------------------------------------------------------
// Walk mode types
// ----------------------------------------------------------------

struct WalkPlayer {
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    bool  onGround = false;
};

enum MovementMode { MODE_FLY = 0, MODE_WALK = 1 };

// ----------------------------------------------------------------
// Collision triangle (for walk-mode wall sliding)
// ----------------------------------------------------------------

struct ColTri {
    float v[3][3];
    float normal[3];
    void calcNormal() {
        float e1[3] = {v[1][0]-v[0][0], v[1][1]-v[0][1], v[1][2]-v[0][2]};
        float e2[3] = {v[2][0]-v[0][0], v[2][1]-v[0][1], v[2][2]-v[0][2]};
        normal[0] = e1[1]*e2[2] - e1[2]*e2[1];
        normal[1] = e1[2]*e2[0] - e1[0]*e2[2];
        normal[2] = e1[0]*e2[1] - e1[1]*e2[0];
        float l = sqrtf(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
        if (l > 1e-8f) { normal[0]/=l; normal[1]/=l; normal[2]/=l; }
    }
};

// ----------------------------------------------------------------
// World DataNode — an image, video, or text placed in the world
// ----------------------------------------------------------------

struct DataNode {
    NodeType node_type = NODE_IMAGE;
    float x, y, z, w, h, rot_x, rot_y, rot_z;
    uint32_t img_w, img_h;             // pixel dims (0 for TEXT)
    std::vector<unsigned char> pixels;  // IMAGE: RGBA, VIDEO: compressed bytes
    std::string text;                   // non-empty for TEXT nodes
    int texture_index = -1;
    int glyph_text_index = -1;
    int video_index = -1;
    uint32_t hash = 0;
};

// ----------------------------------------------------------------
// Remote multiplayer player
// ----------------------------------------------------------------

struct RemotePlayer {
    uint32_t id;
    float x, y, z, yaw, pitch;
};
