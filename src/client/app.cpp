// ================================================================
// Application state definitions — global variable storage
//
// All extern globals declared in app.h are defined here.
// This is the single source of truth for global state.
// ================================================================

#include "app.h"

App app;
float g_time = 0.0f;
uint32_t g_world_seed = 42;
VarianceSampler g_variance = make_default_sampler(42);

// World state
std::vector<DataNode> g_world;

// Multiplayer networking
socket_t g_net_fd = INVALID_SOCK;
uint32_t g_player_id = 0;
NetBuf g_net_recv;
SendQ g_net_send;
bool g_synced = false;
int g_inv_count = 0;
std::vector<RemotePlayer> g_remote_players;
std::map<uint32_t, int> g_hash_to_tex;
int g_marker_tex[4] = {-1, -1, -1, -1};

// Terrain mesh
std::vector<Vertex> g_terrain_verts;
std::vector<float> g_original_heights;

// Trail system
std::vector<float> g_trail;
std::vector<unsigned char> g_terrain_pixels_orig;
std::vector<unsigned char> g_terrain_pixels;
VkBuffer g_trail_tex_staging = VK_NULL_HANDLE;
VkDeviceMemory g_trail_tex_staging_mem = VK_NULL_HANDLE;
void* g_trail_tex_staging_mapped = nullptr;
bool g_trail_tex_dirty = false;
int g_trail_dirty_px_min = TRAIL_TEX_SIZE;
int g_trail_dirty_px_max = -1;
int g_trail_dirty_pz_min = TRAIL_TEX_SIZE;
int g_trail_dirty_pz_max = -1;
VkBuffer g_terrain_vert_staging = VK_NULL_HANDLE;
VkDeviceMemory g_terrain_vert_staging_mem = VK_NULL_HANDLE;
void* g_terrain_vert_staging_mapped = nullptr;
bool g_terrain_mesh_dirty = false;
int g_mesh_dirty_gz_min = 0;
int g_mesh_dirty_gz_max = GRID_SIZE - 1;

// Structure wear
std::vector<ColorVertex> g_struct_verts;
std::vector<ColorVertex> g_struct_verts_orig;
std::vector<float> g_struct_wear;
std::vector<std::vector<uint32_t>> g_struct_grid;
VkBuffer g_struct_vert_staging = VK_NULL_HANDLE;
VkDeviceMemory g_struct_vert_staging_mem = VK_NULL_HANDLE;
void* g_struct_vert_staging_mapped = nullptr;
bool g_struct_wear_dirty = false;

// Collision
std::vector<ColTri> g_col_tris;
std::vector<std::vector<uint32_t>> g_col_grid;

// Menu text
GlyphText* g_menu_text = nullptr;

// Selection state
int g_selected_world_idx = -1;
int g_selected_quad_idx = -1;
int g_selected_glyph_idx = -1;

// Client-side inventory + toast notifications
std::vector<InventoryItem> g_inventory;
std::vector<InventoryItem> g_deleted_stack;
GlyphText* g_inv_hud_text = nullptr;
GlyphText* g_toast_hud_text = nullptr;
std::vector<ToastMessage> g_toasts;

// Master volume
float g_master_volume = 1.0f;

// Key bindings
KeyBindings g_keys;

const char* key_name(int k) {
    const char* name = glfwGetKeyName(k, 0);
    if (name) return name;
    switch (k) {
        case GLFW_KEY_SPACE:         return "Space";
        case GLFW_KEY_LEFT_SHIFT:    return "LShift";
        case GLFW_KEY_RIGHT_SHIFT:   return "RShift";
        case GLFW_KEY_LEFT_CONTROL:  return "LCtrl";
        case GLFW_KEY_RIGHT_CONTROL: return "RCtrl";
        case GLFW_KEY_LEFT_ALT:      return "LAlt";
        case GLFW_KEY_RIGHT_ALT:     return "RAlt";
        case GLFW_KEY_TAB:           return "Tab";
        case GLFW_KEY_ESCAPE:        return "Esc";
        case GLFW_KEY_ENTER:         return "Enter";
        case GLFW_KEY_BACKSPACE:     return "Backspace";
        case GLFW_KEY_DELETE:        return "Delete";
        case GLFW_KEY_INSERT:        return "Insert";
        case GLFW_KEY_UP:            return "Up";
        case GLFW_KEY_DOWN:          return "Down";
        case GLFW_KEY_LEFT:          return "Left";
        case GLFW_KEY_RIGHT:         return "Right";
        case GLFW_KEY_HOME:          return "Home";
        case GLFW_KEY_END:           return "End";
        case GLFW_KEY_PAGE_UP:       return "PgUp";
        case GLFW_KEY_PAGE_DOWN:     return "PgDn";
        case GLFW_KEY_F1: return "F1"; case GLFW_KEY_F2: return "F2";
        case GLFW_KEY_F3: return "F3"; case GLFW_KEY_F4: return "F4";
        case GLFW_KEY_F5: return "F5"; case GLFW_KEY_F6: return "F6";
        case GLFW_KEY_F7: return "F7"; case GLFW_KEY_F8: return "F8";
        case GLFW_KEY_F9: return "F9"; case GLFW_KEY_F10: return "F10";
        case GLFW_KEY_F11: return "F11"; case GLFW_KEY_F12: return "F12";
        default: return "???";
    }
}

void KeyBindings::save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < KEY_ACTION_COUNT; i++)
        fprintf(f, "%s=%d\n", names[i], keys[i]);
    fclose(f);
}

void KeyBindings::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int val = atoi(eq + 1);
        for (int i = 0; i < KEY_ACTION_COUNT; i++) {
            if (strcmp(line, names[i]) == 0) {
                keys[i] = val;
                break;
            }
        }
    }
    fclose(f);
}
