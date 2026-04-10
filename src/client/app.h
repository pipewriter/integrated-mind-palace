// ================================================================
// Application state — the App struct and all global state
//
// The App struct holds all Vulkan resources and rendering state.
// Global variables are declared extern here, defined in app.cpp.
// ================================================================
#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "types.h"
#include "shaders.h"
#include "variance.h"
#include "../shared/constants.h"
#include "../shared/net.h"

#include <array>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <random>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <SDL2/SDL.h>

// ----------------------------------------------------------------
// Key bindings
// ----------------------------------------------------------------

enum KeyAction {
    KEY_FORWARD, KEY_BACK, KEY_LEFT, KEY_RIGHT,
    KEY_JUMP, KEY_DESCEND, KEY_SPRINT,
    KEY_CUT, KEY_COPY, KEY_PASTE,
    KEY_RAISE, KEY_LOWER, KEY_DELETE_NODE,
    KEY_TYPE_TEXT, KEY_TOGGLE_FOG, KEY_TOGGLE_MODE,
    KEY_MENU,
    KEY_ACTION_COUNT
};

struct KeyBindings {
    int keys[KEY_ACTION_COUNT];
    const char* names[KEY_ACTION_COUNT] = {
        "Forward", "Back", "Left", "Right",
        "Jump/Up", "Descend", "Sprint",
        "Cut", "Copy", "Paste",
        "Raise Item", "Lower Item", "Delete",
        "Type Text", "Toggle Fog", "Fly/Walk",
        "Menu"
    };
    void set_defaults() {
        keys[KEY_FORWARD]     = GLFW_KEY_W;
        keys[KEY_BACK]        = GLFW_KEY_S;
        keys[KEY_LEFT]        = GLFW_KEY_A;
        keys[KEY_RIGHT]       = GLFW_KEY_D;
        keys[KEY_JUMP]        = GLFW_KEY_SPACE;
        keys[KEY_DESCEND]     = GLFW_KEY_LEFT_CONTROL;
        keys[KEY_SPRINT]      = GLFW_KEY_LEFT_SHIFT;
        keys[KEY_CUT]         = GLFW_KEY_X;
        keys[KEY_COPY]        = GLFW_KEY_C;
        keys[KEY_PASTE]       = GLFW_KEY_V;
        keys[KEY_RAISE]       = GLFW_KEY_Q;
        keys[KEY_LOWER]       = GLFW_KEY_E;
        keys[KEY_DELETE_NODE] = GLFW_KEY_DELETE;
        keys[KEY_TYPE_TEXT]   = GLFW_KEY_T;
        keys[KEY_TOGGLE_FOG]  = GLFW_KEY_F;
        keys[KEY_TOGGLE_MODE] = GLFW_KEY_TAB;
        keys[KEY_MENU]        = GLFW_KEY_ESCAPE;
    }
    void save(const char* path);
    void load(const char* path);
};

extern KeyBindings g_keys;

const char* key_name(int glfw_key);

// ----------------------------------------------------------------
// Client-only constants
// ----------------------------------------------------------------

constexpr uint32_t INIT_WIDTH  = 1280;
constexpr uint32_t INIT_HEIGHT = 720;
constexpr int      MAX_FRAMES  = 2;

// Trail depression
constexpr float TRAIL_MAX_DEPRESSION = 0.25f;
constexpr int   MESH_UPDATE_INTERVAL = 10;

// Structure wear
constexpr float STRUCT_BRUSH_RADIUS   = 2.5f;
constexpr float STRUCT_WEAR_PER_UNIT  = 0.08f;
constexpr float STRUCT_MAX_DARKEN     = 0.55f;
constexpr float STRUCT_DESAT_AMOUNT   = 0.45f;
constexpr float STRUCT_VERTICAL_REACH = 2.5f;

// Walk mode physics
constexpr float PLAYER_HEIGHT   = 5.0f;
constexpr float PLAYER_RADIUS   = 0.3f;
constexpr float STEP_HEIGHT     = 0.35f;
constexpr float WALK_GRAVITY    = 18.0f;
constexpr float JUMP_SPEED      = 7.0f;
constexpr float WALK_MOVE_SPEED = 18.0f;

// Collision spatial grid
constexpr float COL_GRID_CELL = 4.0f;
constexpr int   COL_GRID_DIM  = 256;

// Structure wear spatial grid
constexpr float STRUCT_GRID_CELL = 4.0f;
constexpr int   STRUCT_GRID_DIM  = 256;

#ifdef NDEBUG
constexpr bool VALIDATION = false;
#else
constexpr bool VALIDATION = true;
#endif
static const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

// ----------------------------------------------------------------
// The App struct — all Vulkan resources and rendering state
// ----------------------------------------------------------------

struct App {
    GLFWwindow* window = nullptr;
    Camera      cam;
    double      last_mx=0, last_my=0;
    bool        first_mouse = true;
    bool        fog_on = true;
    MovementMode move_mode = MODE_FLY;
    WalkPlayer   walker;

    // Vulkan core
    VkInstance               instance       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_msg      = VK_NULL_HANDLE;
    VkSurfaceKHR             surface        = VK_NULL_HANDLE;
    VkPhysicalDevice         phys_device    = VK_NULL_HANDLE;
    VkDevice                 device         = VK_NULL_HANDLE;
    VkQueue                  gfx_queue      = VK_NULL_HANDLE;
    VkQueue                  present_queue  = VK_NULL_HANDLE;
    uint32_t                 gfx_family     = 0;
    uint32_t                 present_family = 0;

    // Swapchain
    VkSwapchainKHR           swapchain      = VK_NULL_HANDLE;
    VkFormat                 sc_format      = VK_FORMAT_UNDEFINED;
    VkExtent2D               sc_extent      = {};
    std::vector<VkImage>     sc_images;
    std::vector<VkImageView> sc_views;

    // Depth buffer
    VkImage        depth_image  = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView    depth_view   = VK_NULL_HANDLE;

    // Render pass
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    // Pipeline 1: terrain
    VkPipelineLayout terrain_pipe_layout = VK_NULL_HANDLE;
    VkPipeline       terrain_pipeline    = VK_NULL_HANDLE;
    int              terrain_tex_index   = -1;

    // Pipeline 2: textured quads
    VkDescriptorSetLayout quad_desc_layout = VK_NULL_HANDLE;
    VkDescriptorPool      quad_desc_pool   = VK_NULL_HANDLE;
    VkPipelineLayout      quad_pipe_layout = VK_NULL_HANDLE;
    VkPipeline            quad_pipeline    = VK_NULL_HANDLE;

    // Terrain mesh buffers
    VkBuffer       terrain_vbuf = VK_NULL_HANDLE;
    VkDeviceMemory terrain_vmem = VK_NULL_HANDLE;
    VkBuffer       terrain_ibuf = VK_NULL_HANDLE;
    VkDeviceMemory terrain_imem = VK_NULL_HANDLE;
    uint32_t       terrain_idx_count = 0;

    // Quad mesh (shared unit quad)
    VkBuffer       quad_vbuf = VK_NULL_HANDLE;
    VkDeviceMemory quad_vmem = VK_NULL_HANDLE;
    VkBuffer       quad_ibuf = VK_NULL_HANDLE;
    VkDeviceMemory quad_imem = VK_NULL_HANDLE;

    // Pipeline 3: structures (vertex-colored)
    VkPipelineLayout struct_pipe_layout = VK_NULL_HANDLE;
    VkPipeline       struct_pipeline    = VK_NULL_HANDLE;
    VkBuffer         struct_vbuf = VK_NULL_HANDLE;
    VkDeviceMemory   struct_vmem = VK_NULL_HANDLE;
    VkBuffer         struct_ibuf = VK_NULL_HANDLE;
    VkDeviceMemory   struct_imem = VK_NULL_HANDLE;
    uint32_t         struct_idx_count = 0;

    // Textures
    std::vector<Texture> textures;

    // Quads to draw
    std::vector<Quad> quads;

    // Glyph text
    int font_atlas_index = -1;
    std::vector<GlyphText*> glyph_texts;

    // Menu state
    bool menu_open = false;
    int menu_cursor = 0;
    int menu_page = 0;       // 0 = main, 1 = controls
    int rebind_action = -1;  // >= 0 means waiting for key press
    float fov = 70.0f;

    // Typing mode
    bool typing_mode = false;
    std::string typing_buffer;
    int typing_glyph_idx = -1;

    // Video players
    struct VideoPlayer {
        int tex_index = -1;
        VkBuffer staging_buf = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        void* staging_mapped = nullptr;
        int w = 0, h = 0;
        bool active = false;
        AVFormatContext* fmt_ctx = nullptr;
        AVCodecContext* vid_codec_ctx = nullptr;
        AVCodecContext* aud_codec_ctx = nullptr;
        int vid_stream_idx = -1, aud_stream_idx = -1;
        SwsContext* sws_ctx = nullptr;
        SwrContext* swr_ctx = nullptr;
        AVFrame* dec_frame = nullptr;
        AVFrame* rgb_frame = nullptr;
        AVFrame* aud_dec_frame = nullptr;
        AVPacket* av_pkt = nullptr;
        double vid_timebase = 0.0, fps = 0.0;
        SDL_AudioDeviceID audio_dev = 0;
        using VClock = std::chrono::steady_clock;
        VClock::time_point play_start;
        double cur_pts = 0.0;
        std::string path;

        // Decode budgeting
        bool   decoding = true;
        bool   has_new_frame = false;
        double mp_per_sec = 0.0;

        // Proximity audio
        float  audio_volume = 0.0f;
    };
    std::vector<VideoPlayer> video_players;

    // Skybox
    VkDescriptorSetLayout sky_desc_layout = VK_NULL_HANDLE;
    VkDescriptorPool sky_desc_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES> sky_desc_sets = {};
    std::array<VkBuffer, MAX_FRAMES> sky_ubo_bufs = {};
    std::array<VkDeviceMemory, MAX_FRAMES> sky_ubo_mems = {};
    std::array<void*, MAX_FRAMES> sky_ubo_mapped = {};
    VkPipelineLayout sky_pipe_layout = VK_NULL_HANDLE;
    VkPipeline sky_pipeline = VK_NULL_HANDLE;
    SkyPreset current_preset{};
    SkyPreset target_preset{};
    SkyPreset display_preset{};
    float sky_transition_t = 1.0f;
    std::mt19937 sky_rng;

    // Command + sync
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmd_bufs;
    std::array<VkSemaphore, MAX_FRAMES> sem_available = {};
    std::array<VkSemaphore, MAX_FRAMES> sem_finished  = {};
    std::array<VkFence,     MAX_FRAMES> fences        = {};
    uint32_t frame = 0;
    bool fb_resized = false;
};

// ================================================================
// Global state — extern declarations (defined in app.cpp)
// ================================================================

extern App app;
extern float g_time;
extern uint32_t g_world_seed;
extern VarianceSampler g_variance;

// World state
extern std::vector<DataNode> g_world;

// Multiplayer networking
extern int g_net_fd;
extern uint32_t g_player_id;
extern NetBuf g_net_recv;
extern SendQ g_net_send;
extern bool g_synced;
extern int g_inv_count;
extern std::vector<RemotePlayer> g_remote_players;
extern std::map<uint32_t, int> g_hash_to_tex;
extern int g_marker_tex[4];

// Terrain mesh (kept for trail depression)
extern std::vector<Vertex> g_terrain_verts;
extern std::vector<float> g_original_heights;

// Trail system
extern std::vector<float> g_trail;
extern std::vector<unsigned char> g_terrain_pixels_orig;
extern std::vector<unsigned char> g_terrain_pixels;
extern VkBuffer g_trail_tex_staging;
extern VkDeviceMemory g_trail_tex_staging_mem;
extern void* g_trail_tex_staging_mapped;
extern bool g_trail_tex_dirty;
extern int g_trail_dirty_px_min, g_trail_dirty_px_max;
extern int g_trail_dirty_pz_min, g_trail_dirty_pz_max;
extern VkBuffer g_terrain_vert_staging;
extern VkDeviceMemory g_terrain_vert_staging_mem;
extern void* g_terrain_vert_staging_mapped;
extern bool g_terrain_mesh_dirty;
extern int g_mesh_dirty_gz_min, g_mesh_dirty_gz_max;

// Structure wear
extern std::vector<ColorVertex> g_struct_verts;
extern std::vector<ColorVertex> g_struct_verts_orig;
extern std::vector<float> g_struct_wear;
extern std::vector<std::vector<uint32_t>> g_struct_grid;
extern VkBuffer g_struct_vert_staging;
extern VkDeviceMemory g_struct_vert_staging_mem;
extern void* g_struct_vert_staging_mapped;
extern bool g_struct_wear_dirty;

// Collision
extern std::vector<ColTri> g_col_tris;
extern std::vector<std::vector<uint32_t>> g_col_grid;

// Menu text
extern GlyphText* g_menu_text;

// Selection state (cone-based, updated each frame)
extern int g_selected_world_idx;
extern int g_selected_quad_idx;
extern int g_selected_glyph_idx;

// ----------------------------------------------------------------
// Client-side inventory + toast notifications
// ----------------------------------------------------------------

struct InventoryItem {
    uint32_t hash;
    NodeType node_type;
    uint32_t img_w, img_h;
    float w, h;
    std::string text;           // For text nodes
    bool from_server = true;    // false = Z-recovered, paste sends C2S_ADD_NODE
};

struct ToastMessage {
    std::string text;
    double created_at;
};

extern std::vector<InventoryItem> g_inventory;
extern std::vector<InventoryItem> g_deleted_stack;
extern GlyphText* g_inv_hud_text;
extern GlyphText* g_toast_hud_text;
extern std::vector<ToastMessage> g_toasts;

// Master volume (0.0 to 1.0)
extern float g_master_volume;
