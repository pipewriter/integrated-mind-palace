#include "input.h"
#include "app.h"
#include "terrain.h"
#include "trail.h"
#include "network.h"
#include "text.h"
#include "textures.h"
#include "video.h"
#include "collision.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../vendor/stb_image.h"

// File-local trail tracking for walk mode
static float g_input_last_trail_x = 1e9f, g_input_last_trail_z = 1e9f;

// Track last pickup hash to avoid pushing picked-up items to deleted stack
uint32_t g_last_pickup_hash = 0;

// Suppress clipboard check after Ctrl+C copy
static double g_extract_clip_time = 0;

// ----------------------------------------------------------------
// PNG writer (uses zlib for compression)
// ----------------------------------------------------------------

static void write_png_file(const char* path, int w, int h, const unsigned char* rgba) {
    FILE* f = fopen(path, "wb");
    if (!f) return;

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);

    auto write_chunk = [&](const char* type, const unsigned char* data, uint32_t len) {
        uint32_t be_len = htonl(len);
        fwrite(&be_len, 1, 4, f);
        fwrite(type, 1, 4, f);
        if (len > 0) fwrite(data, 1, len, f);
        uint32_t crc = crc32(0, (const unsigned char*)type, 4);
        if (len > 0) crc = crc32(crc, data, len);
        uint32_t be_crc = htonl(crc);
        fwrite(&be_crc, 1, 4, f);
    };

    unsigned char ihdr[13];
    uint32_t be_w = htonl(w), be_h = htonl(h);
    memcpy(ihdr, &be_w, 4);
    memcpy(ihdr + 4, &be_h, 4);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    write_chunk("IHDR", ihdr, 13);

    size_t raw_size = (size_t)h * (1 + w * 4);
    std::vector<unsigned char> raw(raw_size);
    for (int y = 0; y < h; y++) {
        raw[y * (1 + w * 4)] = 0;
        memcpy(raw.data() + y * (1 + w * 4) + 1, rgba + y * w * 4, w * 4);
    }
    uLongf comp_size = compressBound(raw_size);
    std::vector<unsigned char> comp(comp_size);
    compress2(comp.data(), &comp_size, raw.data(), raw_size, Z_DEFAULT_COMPRESSION);
    write_chunk("IDAT", comp.data(), (uint32_t)comp_size);

    write_chunk("IEND", nullptr, 0);
    fclose(f);
}

// ----------------------------------------------------------------
// Toast notification
// ----------------------------------------------------------------

static void add_toast(const std::string& msg) {
    g_toasts.push_back({msg, glfwGetTime()});
    printf("Toast: %s\n", msg.c_str());
}

// ----------------------------------------------------------------
// Extraction helpers
// ----------------------------------------------------------------

static bool get_node_pixels(const DataNode& node, std::vector<unsigned char>& pixels,
                            uint32_t& w, uint32_t& h) {
    if (!node.pixels.empty() && node.img_w > 0 && node.img_h > 0) {
        pixels = node.pixels;
        w = node.img_w; h = node.img_h;
        return true;
    }
    return load_from_cache(node.hash, w, h, pixels);
}

static void extract_to_clipboard(const DataNode& node) {
    if (node.node_type == NODE_IMAGE) {
        std::vector<unsigned char> pixels;
        uint32_t w, h;
        if (get_node_pixels(node, pixels, w, h)) {
#ifdef _WIN32
            // Win32: put DIB on clipboard
            uint32_t rowBytes = w * 4;
            uint32_t dataSize = sizeof(BITMAPINFOHEADER) + rowBytes * h;
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dataSize);
            if (hMem) {
                void* ptr = GlobalLock(hMem);
                BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)ptr;
                memset(bih, 0, sizeof(*bih));
                bih->biSize = sizeof(BITMAPINFOHEADER);
                bih->biWidth = (LONG)w;
                bih->biHeight = -(LONG)h; // top-down
                bih->biPlanes = 1;
                bih->biBitCount = 32;
                bih->biCompression = BI_RGB;
                // Copy RGBA pixels (Windows CF_DIB expects BGRA)
                uint8_t* dst = (uint8_t*)ptr + sizeof(BITMAPINFOHEADER);
                for (uint32_t i = 0; i < w * h; i++) {
                    dst[i*4+0] = pixels[i*4+2]; // B
                    dst[i*4+1] = pixels[i*4+1]; // G
                    dst[i*4+2] = pixels[i*4+0]; // R
                    dst[i*4+3] = pixels[i*4+3]; // A
                }
                GlobalUnlock(hMem);
                if (OpenClipboard(NULL)) {
                    EmptyClipboard();
                    SetClipboardData(CF_DIB, hMem);
                    CloseClipboard();
                } else {
                    GlobalFree(hMem);
                }
            }
#else
            write_png_file("/tmp/exodia_clip.png", w, h, pixels.data());
            system("xclip -selection clipboard -t image/png -i /tmp/exodia_clip.png 2>/dev/null &");
#endif
            g_extract_clip_time = glfwGetTime();
            add_toast("Image copied to clipboard");
        } else {
            add_toast("No pixel data available");
        }
    } else if (node.node_type == NODE_TEXT) {
#ifdef _WIN32
        glfwSetClipboardString(app.window, node.text.c_str());
#else
        FILE* p = popen("xclip -selection clipboard 2>/dev/null", "w");
        if (p) {
            fwrite(node.text.data(), 1, node.text.size(), p);
            pclose(p);
        }
#endif
        add_toast("Text copied to clipboard");
    } else if (node.node_type == NODE_VIDEO) {
        add_toast("Cannot copy video to clipboard");
    }
}

static std::string detect_video_ext(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return ".mp4";
    unsigned char magic[12] = {};
    fread(magic, 1, 12, f);
    fclose(f);
    if (magic[0]==0x1A && magic[1]==0x45 && magic[2]==0xDF && magic[3]==0xA3) return ".mkv";
    if (memcmp(magic+4, "ftyp", 4) == 0) return ".mp4";
    if (memcmp(magic, "RIFF", 4) == 0) return ".avi";
    return ".mp4";
}

static void extract_to_folder(const DataNode& node) {
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    {
        char tmpBase[MAX_PATH];
        GetTempPathA(MAX_PATH, tmpBase);
        snprintf(tmpdir, sizeof(tmpdir), "%sexodia_%u", tmpBase, (unsigned)GetTickCount());
        if (!CreateDirectoryA(tmpdir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            add_toast("Failed to create temp folder"); return;
        }
    }
#else
    char tmpdir[] = "/tmp/exodia_XXXXXX";
    if (!mkdtemp(tmpdir)) { add_toast("Failed to create temp folder"); return; }
#endif

    if (node.node_type == NODE_IMAGE) {
        std::vector<unsigned char> pixels;
        uint32_t w, h;
        if (get_node_pixels(node, pixels, w, h)) {
            std::string path = std::string(tmpdir) + "/image.png";
            write_png_file(path.c_str(), w, h, pixels.data());
            add_toast("Opened image in folder");
        }
    } else if (node.node_type == NODE_TEXT) {
        std::string path = std::string(tmpdir) + "/text.txt";
        FILE* tf = fopen(path.c_str(), "w");
        if (tf) { fwrite(node.text.data(), 1, node.text.size(), tf); fclose(tf); }
        add_toast("Opened text in folder");
    } else if (node.node_type == NODE_VIDEO) {
        std::string src = "image_cache/vid_" + std::to_string(node.hash) + ".tmp";
        std::string ext = detect_video_ext(src.c_str());
        std::string dst = std::string(tmpdir) + "/video" + ext;
        FILE* sf = fopen(src.c_str(), "rb");
        if (sf) {
            FILE* df = fopen(dst.c_str(), "wb");
            if (df) {
                unsigned char buf[65536]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), sf)) > 0) fwrite(buf, 1, n, df);
                fclose(df);
            }
            fclose(sf);
            add_toast("Opened video in folder");
        }
    }

    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "explorer \"%s\"", tmpdir);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' 2>/dev/null &", tmpdir);
#endif
    system(cmd);
}

static InventoryItem make_inv_item(const DataNode& node, bool from_server) {
    InventoryItem item;
    item.hash = node.hash;
    item.node_type = node.node_type;
    item.img_w = node.img_w; item.img_h = node.img_h;
    item.w = node.w; item.h = node.h;
    item.text = node.text;
    item.from_server = from_server;
    return item;
}

// ----------------------------------------------------------------
// Menu
// ----------------------------------------------------------------

static constexpr int MAIN_MENU_COUNT = 8;

void build_menu_text() {
    if (!g_menu_text) return;
    char buf[2048]; char* p = buf;

    if (app.menu_page == 0) {
        p += sprintf(p, " [MENU]\n");
        p += sprintf(p, " -------------------\n");
        auto line = [&](int idx, const char* label, const char* val) {
            p += sprintf(p, " %c %-14s %s\n", (app.menu_cursor == idx) ? '>' : ' ', label, val);
        };
        char v[32];
        sprintf(v, "%s", app.fog_on ? "ON" : "OFF"); line(0, "Fog", v);
        sprintf(v, "%.1f", app.cam.speed); line(1, "Move Speed", v);
        sprintf(v, "%.3f", app.cam.sensitivity); line(2, "Sensitivity", v);
        sprintf(v, "%.0f", app.fov); line(3, "FOV", v);
        sprintf(v, "%s", app.move_mode == MODE_FLY ? "FLY" : "WALK"); line(4, "Mode", v);
        sprintf(v, "%.0f%%", g_master_volume * 100.0f); line(5, "Volume", v);
        line(6, "Controls", ">>");
        line(7, "Quit", "");
    } else {
        p += sprintf(p, " [CONTROLS] Esc=back\n");
        p += sprintf(p, " -------------------\n");
        for (int i = 0; i < KEY_ACTION_COUNT; i++) {
            char marker = (app.menu_cursor == i) ? '>' : ' ';
            if (app.rebind_action == i)
                p += sprintf(p, " %c %-14s [press key]\n", marker, g_keys.names[i]);
            else
                p += sprintf(p, " %c %-14s %s\n", marker, g_keys.names[i], key_name(g_keys.keys[i]));
        }
        p += sprintf(p, " -------------------\n");
        int back_idx = KEY_ACTION_COUNT;
        p += sprintf(p, " %c Reset Defaults\n", (app.menu_cursor == back_idx) ? '>' : ' ');
        p += sprintf(p, " %c Back\n", (app.menu_cursor == back_idx + 1) ? '>' : ' ');
    }

    g_menu_text->text = buf;
    update_glyph_text(*g_menu_text);
}

void menu_adjust(int dir) {
    if (app.menu_page == 0) {
        switch (app.menu_cursor) {
            case 0: app.fog_on = !app.fog_on; break;
            case 1: app.cam.speed = std::clamp(app.cam.speed + dir * 5.0f, 1.0f, 200.0f); break;
            case 2: app.cam.sensitivity = std::clamp(app.cam.sensitivity + dir * 0.01f, 0.01f, 1.0f); break;
            case 3: app.fov = std::clamp(app.fov + dir * 5.0f, 30.0f, 150.0f); break;
            case 4: app.move_mode = (app.move_mode == MODE_FLY) ? MODE_WALK : MODE_FLY; break;
            case 5: g_master_volume = std::clamp(g_master_volume + dir * 0.1f, 0.0f, 1.0f); break;
            case 6: app.menu_page = 1; app.menu_cursor = 0; break;
            case 7: glfwSetWindowShouldClose(app.window, 1); break;
        }
    } else {
        int total = KEY_ACTION_COUNT + 2; // actions + Reset + Back
        if (app.menu_cursor < KEY_ACTION_COUNT) {
            // Start rebinding
            app.rebind_action = app.menu_cursor;
        } else if (app.menu_cursor == KEY_ACTION_COUNT) {
            // Reset defaults
            g_keys.set_defaults();
            g_keys.save("keybinds.cfg");
        } else {
            // Back
            app.menu_page = 0;
            app.menu_cursor = 6;
        }
    }
}

static int menu_item_count() {
    if (app.menu_page == 0) return MAIN_MENU_COUNT;
    return KEY_ACTION_COUNT + 2; // actions + Reset + Back
}

// ----------------------------------------------------------------
// Text typing — char callback for live text input
// ----------------------------------------------------------------

void char_callback(GLFWwindow*, unsigned int codepoint) {
    if (!app.typing_mode || app.typing_glyph_idx < 0) return;
    if (codepoint >= 32 && codepoint < 256) {
        GlyphText* gt = app.glyph_texts[app.typing_glyph_idx];
        if (gt && (int)gt->text.size() < gt->max_chars) {
            gt->text += (char)codepoint;
            update_glyph_text(*gt);
        }
    }
}

void finalize_typing() {
    if (app.typing_glyph_idx < 0) return;
    GlyphText* gt = app.glyph_texts[app.typing_glyph_idx];
    if (!gt || gt->text.empty()) {
        // Cancel empty text
        app.typing_mode = false;
        app.typing_glyph_idx = -1;
        return;
    }

    // Send to server as C2S_ADD_NODE with NODE_TEXT
    Camera& c = app.cam;
    uint32_t text_len = (uint32_t)gt->text.size();
    // node_type(1) + img_w(4) + img_h(4) + x,y,z,w,h,rot_x,rot_y,rot_z(32) + data_size(4) + data
    std::vector<uint8_t> msg(45 + text_len);
    msg[0] = (uint8_t)NODE_TEXT;
    uint32_t zero = 0;
    memcpy(msg.data()+1, &zero, 4); memcpy(msg.data()+5, &zero, 4); // img_w=0, img_h=0
    memcpy(msg.data()+9, &gt->x, 4); memcpy(msg.data()+13, &gt->y, 4); memcpy(msg.data()+17, &gt->z, 4);
    float tw = gt->char_size * 10.0f; // default world width
    float th = gt->char_size * 5.0f;
    memcpy(msg.data()+21, &tw, 4); memcpy(msg.data()+25, &th, 4);
    memcpy(msg.data()+29, &gt->rot_x, 4); memcpy(msg.data()+33, &gt->rot_y, 4); memcpy(msg.data()+37, &gt->rot_z, 4);
    memcpy(msg.data()+41, &text_len, 4);
    memcpy(msg.data()+45, gt->text.data(), text_len);
    net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());

    printf("Text node sent: \"%s\"\n", gt->text.c_str());

    // Destroy the local typing glyph — the server will broadcast S2C_NODE_ADD
    // which creates the proper glyph linked to the DataNode
    destroy_glyph_text(app.glyph_texts[app.typing_glyph_idx]);
    app.glyph_texts[app.typing_glyph_idx] = nullptr;

    app.typing_mode = false;
    app.typing_glyph_idx = -1;
}

void cancel_typing() {
    if (app.typing_glyph_idx >= 0 && app.typing_glyph_idx < (int)app.glyph_texts.size()) {
        destroy_glyph_text(app.glyph_texts[app.typing_glyph_idx]);
        app.glyph_texts[app.typing_glyph_idx] = nullptr;
    }
    app.typing_mode = false;
    app.typing_glyph_idx = -1;
}

void start_typing() {
    Camera& c = app.cam;
    c.update_vectors();
    float hx = c.fx, hz = c.fz;
    float hl = sqrtf(hx*hx + hz*hz);
    if (hl > 0.001f) { hx /= hl; hz /= hl; }
    float px = c.x + hx * 10.0f;
    float pz = c.z + hz * 10.0f;
    float py = terrain_height(px, pz) + 4.0f;
    float rot_y = -(c.yaw + 90.0f);

    GlyphText* gt = create_glyph_text(px, py, pz, 0.5f, rot_y, 1.0f, 1.0f, 1.0f, 512);
    app.typing_glyph_idx = (int)app.glyph_texts.size();
    app.glyph_texts.push_back(gt);
    app.typing_mode = true;
    printf("Typing mode ON — press T again to place, Escape to cancel\n");
}

// ----------------------------------------------------------------
// Resize — left click grows, right click shrinks nearest node
// ----------------------------------------------------------------

void key_cb(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS) return;
    if (app.rebind_action >= 0) {
        g_keys.keys[app.rebind_action] = key;
        g_keys.save("keybinds.cfg");
        app.rebind_action = -1;
    }
}

void mouse_button_cb(GLFWwindow* window, int button, int action, int mods) {
    if (action != GLFW_PRESS) return;
    if (app.menu_open || app.typing_mode) return;
    if (g_selected_world_idx < 0 || g_selected_world_idx >= (int)g_world.size()) return;

    float factor = 0.0f;
    if (button == GLFW_MOUSE_BUTTON_LEFT) factor = 1.2f;
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) factor = 1.0f / 1.2f;
    else return;

    Camera& c = app.cam;
    uint32_t hash = g_world[g_selected_world_idx].hash;
    uint8_t data[20];
    memcpy(data, &hash, 4);
    memcpy(data+4, &c.x, 4); memcpy(data+8, &c.y, 4); memcpy(data+12, &c.z, 4);
    memcpy(data+16, &factor, 4);
    net_send(C2S_RESIZE, data, 20);
}

// ----------------------------------------------------------------
// File drop — drag images/videos onto the window to import
// ----------------------------------------------------------------

static bool is_video_ext(const char* path) {
    std::string p(path);
    auto dot = p.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot);
    for (auto& ch : ext) ch = tolower(ch);
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov";
}

static bool is_image_ext(const char* path) {
    std::string p(path);
    auto dot = p.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = p.substr(dot);
    for (auto& ch : ext) ch = tolower(ch);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp";
}

// Compute placement position in front of camera, offset horizontally for multiple drops
static void place_in_front(float offset_right, float& px, float& py, float& pz,
                           float& rot_x, float& rot_y, float& rot_z) {
    Camera& c = app.cam;
    c.update_vectors();
    float hx = c.fx, hz = c.fz;
    float hl = sqrtf(hx*hx + hz*hz);
    if (hl > 0.001f) { hx /= hl; hz /= hl; }
    px = c.x + hx * 15.0f + c.rx * offset_right;
    pz = c.z + hz * 15.0f + c.rz * offset_right;
    py = terrain_height(px, pz) + 6.0f;
    rot_x = c.pitch;
    rot_y = -(c.yaw + 90.0f);
    rot_z = 0.0f;
}

static void send_add_image(const char* path, float offset) {
    int iw, ih, channels;
    unsigned char* pixels = stbi_load(path, &iw, &ih, &channels, STBI_rgb_alpha);
    if (!pixels) { fprintf(stderr, "Failed to load image: %s\n", path); return; }

    float px, py, pz, rx, ry, rz;
    place_in_front(offset, px, py, pz, rx, ry, rz);
    float ww = 10.0f, wh = 10.0f * (float)ih / (float)iw;

    uint32_t pix_sz = iw * ih * 4;
    std::vector<uint8_t> msg(45 + pix_sz);
    msg[0] = (uint8_t)NODE_IMAGE;
    uint32_t uw = iw, uh = ih;
    memcpy(msg.data()+1, &uw, 4); memcpy(msg.data()+5, &uh, 4);
    memcpy(msg.data()+9, &px, 4); memcpy(msg.data()+13, &py, 4); memcpy(msg.data()+17, &pz, 4);
    memcpy(msg.data()+21, &ww, 4); memcpy(msg.data()+25, &wh, 4);
    memcpy(msg.data()+29, &rx, 4); memcpy(msg.data()+33, &ry, 4); memcpy(msg.data()+37, &rz, 4);
    memcpy(msg.data()+41, &pix_sz, 4);
    memcpy(msg.data()+45, pixels, pix_sz);
    stbi_image_free(pixels);
    net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());
    printf("Dropped image: %s (%dx%d)\n", path, iw, ih);
}

static void send_add_video(const char* path, float offset) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> file_data(fsize);
    fread(file_data.data(), 1, fsize, f);
    fclose(f);

    int vpi = start_video_player(path);
    if (vpi < 0) { fprintf(stderr, "Failed to open video: %s\n", path); return; }
    auto& vp = app.video_players[vpi];

    float px, py, pz, rx, ry, rz;
    place_in_front(offset, px, py, pz, rx, ry, rz);
    float ww = 10.0f, wh = 10.0f * (float)vp.h / (float)vp.w;

    uint32_t dsz = (uint32_t)file_data.size();
    std::vector<uint8_t> msg(45 + dsz);
    msg[0] = (uint8_t)NODE_VIDEO;
    uint32_t uw = (uint32_t)vp.w, uh = (uint32_t)vp.h;
    memcpy(msg.data()+1, &uw, 4); memcpy(msg.data()+5, &uh, 4);
    memcpy(msg.data()+9, &px, 4); memcpy(msg.data()+13, &py, 4); memcpy(msg.data()+17, &pz, 4);
    memcpy(msg.data()+21, &ww, 4); memcpy(msg.data()+25, &wh, 4);
    memcpy(msg.data()+29, &rx, 4); memcpy(msg.data()+33, &ry, 4); memcpy(msg.data()+37, &rz, 4);
    memcpy(msg.data()+41, &dsz, 4);
    memcpy(msg.data()+45, file_data.data(), dsz);
    net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());
    printf("Dropped video: %s (%ux%u, %zu bytes)\n", path, uw, uh, file_data.size());
}

void drop_cb(GLFWwindow* window, int count, const char** paths) {
    if (!sock_valid(g_net_fd)) return;
    float spacing = 12.0f;
    float start_offset = -(count - 1) * spacing * 0.5f;

    for (int i = 0; i < count; i++) {
        float offset = start_offset + i * spacing;
        if (is_video_ext(paths[i]))
            send_add_video(paths[i], offset);
        else if (is_image_ext(paths[i]))
            send_add_image(paths[i], offset);
        else
            printf("Skipping unsupported file: %s\n", paths[i]);
    }
}

// ----------------------------------------------------------------
// Pickup / putdown — send requests to authoritative server
// ----------------------------------------------------------------

void pickup_nearest() {
    if (g_selected_world_idx < 0 || g_selected_world_idx >= (int)g_world.size()) return;
    auto& node = g_world[g_selected_world_idx];
    g_inventory.push_back(make_inv_item(node, true));
    g_last_pickup_hash = node.hash;
    Camera& c = app.cam;
    uint32_t hash = node.hash;
    uint8_t data[16];
    memcpy(data, &hash, 4);
    memcpy(data+4, &c.x, 4); memcpy(data+8, &c.y, 4); memcpy(data+12, &c.z, 4);
    net_send(C2S_PICKUP, data, 16);
    printf("Pickup request sent (hash=%08x)\n", hash);
}

void putdown_image() {
    if (g_inventory.empty()) { printf("Inventory empty\n"); return; }
    InventoryItem item = g_inventory.back();
    g_inventory.pop_back();

    Camera& c = app.cam;
    c.update_vectors();
    float hx = c.fx, hz = c.fz;
    float hl = sqrtf(hx*hx + hz*hz);
    if (hl > 0.001f) { hx /= hl; hz /= hl; }
    float px = c.x + hx * 15.0f;
    float pz = c.z + hz * 15.0f;
    float py = terrain_height(px, pz) + 6.0f;
    float rot_x = c.pitch;
    float rot_y = -(c.yaw + 90.0f);
    float rot_z = 0.0f;

    if (item.from_server) {
        float data[6] = { px, py, pz, rot_x, rot_y, rot_z };
        net_send(C2S_PUTDOWN, data, 24);
    } else {
        // Z-recovered item: re-create via C2S_ADD_NODE
        float ww = item.w > 0 ? item.w : 10.0f;
        float wh = item.h > 0 ? item.h : 5.0f;
        if (item.node_type == NODE_TEXT) {
            uint32_t text_len = (uint32_t)item.text.size();
            std::vector<uint8_t> msg(45 + text_len);
            msg[0] = (uint8_t)NODE_TEXT;
            uint32_t zero = 0;
            memcpy(msg.data()+1, &zero, 4); memcpy(msg.data()+5, &zero, 4);
            memcpy(msg.data()+9, &px, 4); memcpy(msg.data()+13, &py, 4); memcpy(msg.data()+17, &pz, 4);
            memcpy(msg.data()+21, &ww, 4); memcpy(msg.data()+25, &wh, 4);
            memcpy(msg.data()+29, &rot_x, 4); memcpy(msg.data()+33, &rot_y, 4); memcpy(msg.data()+37, &rot_z, 4);
            memcpy(msg.data()+41, &text_len, 4);
            memcpy(msg.data()+45, item.text.data(), text_len);
            net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());
        } else if (item.node_type == NODE_IMAGE) {
            uint32_t cw, ch;
            std::vector<unsigned char> pixels;
            if (load_from_cache(item.hash, cw, ch, pixels)) {
                uint32_t pix_sz = cw * ch * 4;
                std::vector<uint8_t> msg(45 + pix_sz);
                msg[0] = (uint8_t)NODE_IMAGE;
                memcpy(msg.data()+1, &cw, 4); memcpy(msg.data()+5, &ch, 4);
                memcpy(msg.data()+9, &px, 4); memcpy(msg.data()+13, &py, 4); memcpy(msg.data()+17, &pz, 4);
                if (cw > 0) wh = ww * (float)ch / (float)cw;
                memcpy(msg.data()+21, &ww, 4); memcpy(msg.data()+25, &wh, 4);
                memcpy(msg.data()+29, &rot_x, 4); memcpy(msg.data()+33, &rot_y, 4); memcpy(msg.data()+37, &rot_z, 4);
                memcpy(msg.data()+41, &pix_sz, 4);
                memcpy(msg.data()+45, pixels.data(), pix_sz);
                net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());
            }
        } else if (item.node_type == NODE_VIDEO) {
            std::string vpath = "image_cache/vid_" + std::to_string(item.hash) + ".tmp";
            FILE* vf = fopen(vpath.c_str(), "rb");
            if (vf) {
                fseek(vf, 0, SEEK_END); long fsize = ftell(vf); fseek(vf, 0, SEEK_SET);
                std::vector<uint8_t> file_data(fsize);
                fread(file_data.data(), 1, fsize, vf); fclose(vf);
                uint32_t dsz = (uint32_t)file_data.size();
                std::vector<uint8_t> msg(45 + dsz);
                msg[0] = (uint8_t)NODE_VIDEO;
                memcpy(msg.data()+1, &item.img_w, 4); memcpy(msg.data()+5, &item.img_h, 4);
                memcpy(msg.data()+9, &px, 4); memcpy(msg.data()+13, &py, 4); memcpy(msg.data()+17, &pz, 4);
                if (item.img_w > 0) wh = ww * (float)item.img_h / (float)item.img_w;
                memcpy(msg.data()+21, &ww, 4); memcpy(msg.data()+25, &wh, 4);
                memcpy(msg.data()+29, &rot_x, 4); memcpy(msg.data()+33, &rot_y, 4); memcpy(msg.data()+37, &rot_z, 4);
                memcpy(msg.data()+41, &dsz, 4);
                memcpy(msg.data()+45, file_data.data(), dsz);
                net_send(C2S_ADD_NODE, msg.data(), (uint32_t)msg.size());
            }
        }
    }
    printf("Putdown request sent\n");
}

// ----------------------------------------------------------------
// Clipboard scanning — sends new images to server
// ----------------------------------------------------------------

static uint32_t g_last_clip_hash = 0;
static double   g_last_clip_time = 0;

static uint32_t fnv1a(const unsigned char* data, size_t len) {
    uint32_t h = 0x811c9dc5;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x01000193; }
    return h;
}

void check_clipboard() {
    if (!sock_valid(g_net_fd)) return;
    double now = glfwGetTime();
    if (now - g_extract_clip_time < 5.0) return;  // Suppress after Ctrl+C
    if (now - g_last_clip_time < 2.0) return;
    g_last_clip_time = now;

    std::vector<unsigned char> data;

#ifdef _WIN32
    // Win32 clipboard: try multiple image formats in priority order.
    // Snipping Tool, Discord, browsers use PNG; Paint uses CF_DIB; others use CF_BITMAP.
    if (!OpenClipboard(NULL)) return;

    // 1. PNG registered format — used by Snipping Tool, Discord, browsers, etc.
    static UINT cf_png = RegisterClipboardFormatA("PNG");
    if (data.empty()) {
        HANDLE hPng = GetClipboardData(cf_png);
        if (hPng) {
            void* ptr = GlobalLock(hPng);
            if (ptr) {
                SIZE_T sz = GlobalSize(hPng);
                data.assign((unsigned char*)ptr, (unsigned char*)ptr + sz);
                GlobalUnlock(hPng);
            }
        }
    }

    // 2. CF_DIBV5 — enhanced DIB with alpha, used by some modern apps
    if (data.empty()) {
        HANDLE hDib5 = GetClipboardData(CF_DIBV5);
        if (hDib5) {
            void* dibData = GlobalLock(hDib5);
            if (dibData) {
                SIZE_T dibSize = GlobalSize(hDib5);
                uint32_t fileSize = (uint32_t)(14 + dibSize);
                data.resize(fileSize);
                data[0] = 'B'; data[1] = 'M';
                memcpy(data.data() + 2, &fileSize, 4);
                memset(data.data() + 6, 0, 4);
                BITMAPV5HEADER* bih = (BITMAPV5HEADER*)dibData;
                uint32_t pixOffset = 14 + bih->bV5Size;
                memcpy(data.data() + 10, &pixOffset, 4);
                memcpy(data.data() + 14, dibData, dibSize);
                GlobalUnlock(hDib5);
            }
        }
    }

    // 3. CF_DIB — standard device-independent bitmap (Paint, older apps)
    if (data.empty()) {
        HANDLE hDib = GetClipboardData(CF_DIB);
        if (hDib) {
            void* dibData = GlobalLock(hDib);
            if (dibData) {
                SIZE_T dibSize = GlobalSize(hDib);
                uint32_t fileSize = (uint32_t)(14 + dibSize);
                data.resize(fileSize);
                data[0] = 'B'; data[1] = 'M';
                memcpy(data.data() + 2, &fileSize, 4);
                memset(data.data() + 6, 0, 4);
                BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)dibData;
                uint32_t pixOffset = 14 + bih->biSize;
                if (bih->biBitCount <= 8) {
                    uint32_t colors = bih->biClrUsed ? bih->biClrUsed : (1u << bih->biBitCount);
                    pixOffset += colors * 4;
                }
                memcpy(data.data() + 10, &pixOffset, 4);
                memcpy(data.data() + 14, dibData, dibSize);
                GlobalUnlock(hDib);
            }
        }
    }

    // 4. CF_BITMAP — device-dependent bitmap, convert via GDI
    if (data.empty()) {
        HBITMAP hBmp = (HBITMAP)GetClipboardData(CF_BITMAP);
        if (hBmp) {
            BITMAP bm;
            GetObject(hBmp, sizeof(bm), &bm);
            int w = bm.bmWidth, h = bm.bmHeight;
            if (w > 0 && h > 0) {
                BITMAPINFOHEADER bi = {};
                bi.biSize = sizeof(bi);
                bi.biWidth = w;
                bi.biHeight = h; // bottom-up for BMP file
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                bi.biCompression = BI_RGB;
                uint32_t rowBytes = w * 4;
                uint32_t pixelSize = rowBytes * h;
                uint32_t headerSize = 14 + sizeof(BITMAPINFOHEADER);
                uint32_t fileSize = headerSize + pixelSize;
                data.resize(fileSize);
                data[0] = 'B'; data[1] = 'M';
                memcpy(data.data() + 2, &fileSize, 4);
                memset(data.data() + 6, 0, 4);
                memcpy(data.data() + 10, &headerSize, 4);
                memcpy(data.data() + 14, &bi, sizeof(bi));
                HDC hdc = GetDC(NULL);
                BITMAPINFOHEADER biQuery = bi;
                biQuery.biHeight = -h; // top-down for GetDIBits
                GetDIBits(hdc, hBmp, 0, h, data.data() + headerSize,
                          (BITMAPINFO*)&biQuery, DIB_RGB_COLORS);
                ReleaseDC(NULL, hdc);
            }
        }
    }

    CloseClipboard();
    if (data.empty()) return;
#else
    FILE* p = popen("xclip -selection clipboard -t image/png -o 2>/dev/null", "r");
    if (!p) return;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
        data.insert(data.end(), buf, buf + n);
    int status = pclose(p);
    if (status != 0 || data.size() < 8) return;
#endif

    uint32_t h = fnv1a(data.data(), data.size());
    if (h == g_last_clip_hash) return;
    g_last_clip_hash = h;

    int iw, ih, channels;
    unsigned char* pixels = stbi_load_from_memory(data.data(), (int)data.size(),
                                                   &iw, &ih, &channels, STBI_rgb_alpha);
    if (!pixels) return;

    printf("New clipboard image: %dx%d -> adding to inventory\n", iw, ih);

    // Hash the decoded pixels for cache/dedup
    uint32_t pix_sz = iw * ih * 4;
    uint32_t pix_hash = fnv1a(pixels, pix_sz);

    // Save to local cache and create texture so it shows in inventory HUD
    save_to_cache(pix_hash, (uint32_t)iw, (uint32_t)ih, pixels);
    get_or_create_texture(pix_hash, (uint32_t)iw, (uint32_t)ih, pixels);
    stbi_image_free(pixels);

    // Add to inventory (not to world)
    InventoryItem item;
    item.hash = pix_hash;
    item.node_type = NODE_IMAGE;
    item.img_w = (uint32_t)iw;
    item.img_h = (uint32_t)ih;
    item.w = 10.0f;
    item.h = 10.0f * (float)ih / (float)iw;
    item.from_server = false;
    g_inventory.push_back(item);
    add_toast("Clipboard image added to inventory");
}

// ----------------------------------------------------------------
// Dump folder watcher — imports images/videos/txt to inventory
// ----------------------------------------------------------------

static double g_last_media_check = 0;

static bool is_media_image_ext(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
           ext == ".gif" || ext == ".tga" || ext == ".ppm" || ext == ".pgm" ||
           ext == ".pbm" || ext == ".tiff" || ext == ".tif" || ext == ".webp" ||
           ext == ".ico" || ext == ".exr" || ext == ".hdr" || ext == ".psd" ||
           ext == ".svg" || ext == ".j2k" || ext == ".jp2";
}

static bool is_media_video_ext(const std::string& ext) {
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" ||
           ext == ".mov" || ext == ".flv" || ext == ".wmv" || ext == ".m4v" ||
           ext == ".3gp" || ext == ".ts" || ext == ".mts" || ext == ".vob" ||
           ext == ".ogv" || ext == ".mpg" || ext == ".mpeg" || ext == ".f4v";
}

static bool is_media_text_ext(const std::string& ext) {
    return ext == ".txt";
}

void check_media_folder() {
    if (!sock_valid(g_net_fd)) return;
    double now = glfwGetTime();
    if (now - g_last_media_check < 1.0) return;
    g_last_media_check = now;

    const char* dir = "dump-folder";
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directory(dir);
        return;
    }

    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (auto& ch : ext) ch = tolower(ch);
        std::string path = entry.path().string();

        if (is_media_image_ext(ext)) {
            // Load image with stbi
            int iw, ih, channels;
            unsigned char* pixels = stbi_load(path.c_str(), &iw, &ih, &channels, STBI_rgb_alpha);
            if (!pixels) {
                // Try ffmpeg as fallback for formats stbi can't handle
                // For now just skip
                fprintf(stderr, "Dump: failed to load image: %s\n", path.c_str());
                std::filesystem::remove(path);
                continue;
            }

            uint32_t pix_sz = iw * ih * 4;
            uint32_t hash = fnv1a(pixels, pix_sz);

            save_to_cache(hash, (uint32_t)iw, (uint32_t)ih, pixels);
            get_or_create_texture(hash, (uint32_t)iw, (uint32_t)ih, pixels);
            stbi_image_free(pixels);

            InventoryItem item;
            item.hash = hash;
            item.node_type = NODE_IMAGE;
            item.img_w = (uint32_t)iw;
            item.img_h = (uint32_t)ih;
            item.w = 10.0f;
            item.h = 10.0f * (float)ih / (float)iw;
            item.from_server = false;
            g_inventory.push_back(item);
            add_toast("Dump: image added to inventory");
            printf("Dump folder: imported image %s (%dx%d)\n", path.c_str(), iw, ih);

        } else if (is_media_video_ext(ext)) {
            // Read video file into memory
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) { std::filesystem::remove(path); continue; }
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> file_data(fsize);
            fread(file_data.data(), 1, fsize, f);
            fclose(f);

            uint32_t hash = fnv1a(file_data.data(), file_data.size());

            // Save video to cache
            {
                std::string cache_path = "image_cache/vid_" + std::to_string(hash) + ".tmp";
                FILE* cf = fopen(cache_path.c_str(), "wb");
                if (cf) { fwrite(file_data.data(), 1, file_data.size(), cf); fclose(cf); }
            }

            // Start video player from cache (not dump-folder original — Windows can't
            // delete a file with an open handle, and FFmpeg keeps it open)
            std::string cache_vid = "image_cache/vid_" + std::to_string(hash) + ".tmp";
            int vpi = start_video_player(cache_vid.c_str());
            int vw = 320, vh = 240;
            if (vpi >= 0) {
                vw = app.video_players[vpi].w;
                vh = app.video_players[vpi].h;
            }

            InventoryItem item;
            item.hash = hash;
            item.node_type = NODE_VIDEO;
            item.img_w = (uint32_t)vw;
            item.img_h = (uint32_t)vh;
            item.w = 10.0f;
            item.h = 10.0f * (float)vh / (float)vw;
            item.from_server = false;
            g_inventory.push_back(item);
            add_toast("Dump: video added to inventory");
            printf("Dump folder: imported video %s (%dx%d, %ld bytes)\n", path.c_str(), vw, vh, fsize);

        } else if (is_media_text_ext(ext)) {
            // Read text file
            FILE* f = fopen(path.c_str(), "r");
            if (!f) { std::filesystem::remove(path); continue; }
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::string text(fsize, '\0');
            fread(text.data(), 1, fsize, f);
            fclose(f);

            uint32_t hash = fnv1a((const unsigned char*)text.data(), text.size());

            InventoryItem item;
            item.hash = hash;
            item.node_type = NODE_TEXT;
            item.img_w = 0;
            item.img_h = 0;
            item.w = 10.0f;
            item.h = 5.0f;
            item.text = text;
            item.from_server = false;
            g_inventory.push_back(item);
            add_toast("Dump: text added to inventory");
            printf("Dump folder: imported text %s (%ld bytes)\n", path.c_str(), fsize);

        } else {
            // Unknown extension, skip but don't delete
            continue;
        }

        // Delete the file after importing
        std::filesystem::remove(path);
    }
}

// ----------------------------------------------------------------
// Input
// ----------------------------------------------------------------

void process_input(float dt) {
    GLFWwindow* w = app.window; Camera& c = app.cam;
    c.update_vectors();

    // Shift+Escape: quit immediately (Ctrl+Escape opens Windows Start Menu)
    if (glfwGetKey(w,GLFW_KEY_ESCAPE)==GLFW_PRESS &&
        (glfwGetKey(w,GLFW_KEY_LEFT_SHIFT)==GLFW_PRESS || glfwGetKey(w,GLFW_KEY_RIGHT_SHIFT)==GLFW_PRESS)) {
        glfwSetWindowShouldClose(w,1);
        return;
    }

    // Menu key: toggle menu (skip if Ctrl held to avoid conflict)
    static bool menu_key_was = false;
    if (glfwGetKey(w, g_keys.keys[KEY_MENU])==GLFW_PRESS) {
        if (!menu_key_was && !app.typing_mode && app.rebind_action < 0) {
            if (app.menu_open && app.menu_page == 1) {
                // Escape in controls submenu goes back to main
                app.menu_page = 0;
                app.menu_cursor = 6;
            } else {
                app.menu_open = !app.menu_open;
                app.menu_page = 0;
                glfwSetInputMode(w, GLFW_CURSOR, app.menu_open ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                if (!app.menu_open) app.first_mouse = true;
            }
        }
        menu_key_was = true;
    } else menu_key_was = false;

    // Menu navigation
    if (app.menu_open) {
        if (app.rebind_action >= 0) return; // waiting for key press in key_cb
        int count = menu_item_count();
        static bool up_was=false, dn_was=false, lt_was=false, rt_was=false, enter_was=false;
        if (glfwGetKey(w,GLFW_KEY_UP)==GLFW_PRESS) {
            if (!up_was) app.menu_cursor = (app.menu_cursor - 1 + count) % count;
            up_was = true;
        } else up_was = false;
        if (glfwGetKey(w,GLFW_KEY_DOWN)==GLFW_PRESS) {
            if (!dn_was) app.menu_cursor = (app.menu_cursor + 1) % count;
            dn_was = true;
        } else dn_was = false;
        if (glfwGetKey(w,GLFW_KEY_LEFT)==GLFW_PRESS) { if (!lt_was) menu_adjust(-1); lt_was=true; } else lt_was=false;
        if (glfwGetKey(w,GLFW_KEY_RIGHT)==GLFW_PRESS) { if (!rt_was) menu_adjust(1); rt_was=true; } else rt_was=false;
        if (glfwGetKey(w,GLFW_KEY_ENTER)==GLFW_PRESS) { if (!enter_was) menu_adjust(1); enter_was=true; } else enter_was=false;
        return; // Block all other input while menu is open
    }

    // Type text key
    static bool t_was_pressed = false;
    if (glfwGetKey(w, g_keys.keys[KEY_TYPE_TEXT])==GLFW_PRESS) {
        if (!t_was_pressed && !app.typing_mode) {
            start_typing();
        }
        t_was_pressed = true;
    } else t_was_pressed = false;

    // Handle typing mode input
    if (app.typing_mode) {
        // Menu key finalizes typing
        if (glfwGetKey(w, g_keys.keys[KEY_MENU])==GLFW_PRESS) {
            finalize_typing();
            return;
        }
        // Enter adds newline
        static bool enter_was_pressed = false;
        if (glfwGetKey(w,GLFW_KEY_ENTER)==GLFW_PRESS) {
            if (!enter_was_pressed && app.typing_glyph_idx >= 0) {
                GlyphText* gt = app.glyph_texts[app.typing_glyph_idx];
                if (gt) { gt->text += '\n'; update_glyph_text(*gt); }
            }
            enter_was_pressed = true;
        } else enter_was_pressed = false;
        // Backspace
        static bool bs_was_pressed = false;
        if (glfwGetKey(w,GLFW_KEY_BACKSPACE)==GLFW_PRESS) {
            if (!bs_was_pressed && app.typing_glyph_idx >= 0) {
                GlyphText* gt = app.glyph_texts[app.typing_glyph_idx];
                if (gt && !gt->text.empty()) {
                    gt->text.pop_back();
                    update_glyph_text(*gt);
                }
            }
            bs_was_pressed = true;
        } else bs_was_pressed = false;
        return; // Block all other input while typing
    }

    // Single-press action helper
    // Single-press action helper
    static bool action_was[KEY_ACTION_COUNT] = {};
    auto key_pressed = [&](KeyAction a) -> bool {
        bool down = glfwGetKey(w, g_keys.keys[a]) == GLFW_PRESS;
        bool triggered = down && !action_was[a];
        action_was[a] = down;
        return triggered;
    };

    if (key_pressed(KEY_TOGGLE_FOG)) {
        app.fog_on = !app.fog_on;
        printf("Fog: %s\n", app.fog_on ? "ON" : "OFF");
    }

    if (key_pressed(KEY_CUT)) pickup_nearest();

    if (key_pressed(KEY_COPY)) {
        if (g_selected_world_idx >= 0 && g_selected_world_idx < (int)g_world.size()) {
            bool ctrl = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL)==GLFW_PRESS ||
                        glfwGetKey(w, GLFW_KEY_RIGHT_CONTROL)==GLFW_PRESS;
            bool alt  = glfwGetKey(w, GLFW_KEY_LEFT_ALT)==GLFW_PRESS ||
                        glfwGetKey(w, GLFW_KEY_RIGHT_ALT)==GLFW_PRESS;
            auto& node = g_world[g_selected_world_idx];
            if (ctrl) {
                extract_to_clipboard(node);
            } else if (alt) {
                extract_to_folder(node);
            } else {
                // Copy to inventory
                g_inventory.push_back(make_inv_item(node, true));
                Camera& cam = app.cam;
                uint32_t hash = node.hash;
                uint8_t data[16];
                memcpy(data, &hash, 4);
                memcpy(data+4, &cam.x, 4); memcpy(data+8, &cam.y, 4); memcpy(data+12, &cam.z, 4);
                net_send(C2S_COPY, data, 16);
            }
        }
    }

    if (key_pressed(KEY_PASTE)) putdown_image();

    // Z key: recover deleted objects to inventory
    static bool z_was_pressed = false;
    if (glfwGetKey(w, GLFW_KEY_Z)==GLFW_PRESS) {
        if (!z_was_pressed && !g_deleted_stack.empty()) {
            auto item = g_deleted_stack.back();
            g_deleted_stack.pop_back();
            g_inventory.push_back(item);
            add_toast("Recovered to inventory");
            printf("Z-recovered hash=%08x to inventory\n", item.hash);
        }
        z_was_pressed = true;
    } else z_was_pressed = false;

    if (key_pressed(KEY_RAISE)) {
        if (g_selected_world_idx >= 0 && g_selected_world_idx < (int)g_world.size()) {
            Camera& cam = app.cam;
            uint32_t hash = g_world[g_selected_world_idx].hash;
            float dy = 2.0f;
            uint8_t data[20];
            memcpy(data, &hash, 4);
            memcpy(data+4, &cam.x, 4); memcpy(data+8, &cam.y, 4); memcpy(data+12, &cam.z, 4);
            memcpy(data+16, &dy, 4);
            net_send(C2S_MOVE_Y, data, 20);
        }
    }

    if (key_pressed(KEY_LOWER)) {
        if (g_selected_world_idx >= 0 && g_selected_world_idx < (int)g_world.size()) {
            Camera& cam = app.cam;
            uint32_t hash = g_world[g_selected_world_idx].hash;
            float dy = -2.0f;
            uint8_t data[20];
            memcpy(data, &hash, 4);
            memcpy(data+4, &cam.x, 4); memcpy(data+8, &cam.y, 4); memcpy(data+12, &cam.z, 4);
            memcpy(data+16, &dy, 4);
            net_send(C2S_MOVE_Y, data, 20);
        }
    }

    if (key_pressed(KEY_DELETE_NODE)) {
        if (!g_inventory.empty()) {
            // Delete top of inventory stack (recoverable with Z)
            auto item = g_inventory.back();
            g_inventory.pop_back();
            item.from_server = false;
            g_deleted_stack.push_back(item);
            add_toast("Deleted from inventory");
        } else if (g_selected_world_idx >= 0 && g_selected_world_idx < (int)g_world.size()) {
            Camera& cam = app.cam;
            uint32_t hash = g_world[g_selected_world_idx].hash;
            uint8_t data[16];
            memcpy(data, &hash, 4);
            memcpy(data+4, &cam.x, 4); memcpy(data+8, &cam.y, 4); memcpy(data+12, &cam.z, 4);
            net_send(C2S_DELETE, data, 16);
        }
    }

    // Toggle fly/walk mode
    static bool tab_was_pressed = false;
    if (glfwGetKey(w, g_keys.keys[KEY_TOGGLE_MODE])==GLFW_PRESS) {
        if (!tab_was_pressed) {
            if (app.move_mode == MODE_FLY) {
                app.move_mode = MODE_WALK;
                float ground = effective_terrain_height(c.x, c.z);
                app.walker.px = c.x;
                app.walker.py = ground;
                app.walker.pz = c.z;
                app.walker.vx = app.walker.vy = app.walker.vz = 0;
                app.walker.onGround = true;
                g_input_last_trail_x = c.x;
                g_input_last_trail_z = c.z;
                printf("Mode: WALK — your footsteps will leave desire paths\n");
            } else {
                app.move_mode = MODE_FLY;
                printf("Mode: FLY (WASD/Space/Ctrl, Shift=fast, Tab to walk)\n");
            }
        }
        tab_was_pressed = true;
    } else tab_was_pressed = false;

    if (app.move_mode == MODE_FLY) {
        float spd = c.speed * dt;
        if (glfwGetKey(w, g_keys.keys[KEY_SPRINT])==GLFW_PRESS) spd *= 3;
        float hx=c.fx, hz=c.fz, l=sqrtf(hx*hx+hz*hz);
        if (l>0.001f) { hx/=l; hz/=l; }
        if (glfwGetKey(w, g_keys.keys[KEY_FORWARD])==GLFW_PRESS) { c.x+=hx*spd; c.z+=hz*spd; }
        if (glfwGetKey(w, g_keys.keys[KEY_BACK])==GLFW_PRESS)    { c.x-=hx*spd; c.z-=hz*spd; }
        if (glfwGetKey(w, g_keys.keys[KEY_LEFT])==GLFW_PRESS)    { c.x-=c.rx*spd; c.z-=c.rz*spd; }
        if (glfwGetKey(w, g_keys.keys[KEY_RIGHT])==GLFW_PRESS)   { c.x+=c.rx*spd; c.z+=c.rz*spd; }
        if (glfwGetKey(w, g_keys.keys[KEY_JUMP])==GLFW_PRESS)    c.y+=spd;
        if (glfwGetKey(w, g_keys.keys[KEY_DESCEND])==GLFW_PRESS) c.y-=spd;
    } else {
        WalkPlayer& pl = app.walker;

        constexpr float D = 3.14159265f / 180.0f;
        float fwd_x = cosf(c.yaw * D), fwd_z = sinf(c.yaw * D);
        float right_x = -sinf(c.yaw * D), right_z = cosf(c.yaw * D);

        float wx = 0, wz = 0;
        if (glfwGetKey(w, g_keys.keys[KEY_FORWARD])==GLFW_PRESS) { wx += fwd_x; wz += fwd_z; }
        if (glfwGetKey(w, g_keys.keys[KEY_BACK])==GLFW_PRESS)    { wx -= fwd_x; wz -= fwd_z; }
        if (glfwGetKey(w, g_keys.keys[KEY_LEFT])==GLFW_PRESS)    { wx -= right_x; wz -= right_z; }
        if (glfwGetKey(w, g_keys.keys[KEY_RIGHT])==GLFW_PRESS)   { wx += right_x; wz += right_z; }
        float wl = sqrtf(wx*wx + wz*wz);
        if (wl > 0.01f) { wx /= wl; wz /= wl; }

        float speed = WALK_MOVE_SPEED;
        if (glfwGetKey(w, g_keys.keys[KEY_SPRINT])==GLFW_PRESS) speed *= 2.0f;
        pl.vx = wx * speed;
        pl.vz = wz * speed;

        if (glfwGetKey(w, g_keys.keys[KEY_JUMP])==GLFW_PRESS && pl.onGround) {
            pl.vy = JUMP_SPEED;
            pl.onGround = false;
        }

        if (!pl.onGround) pl.vy -= WALK_GRAVITY * dt;

        pl.px += pl.vx * dt;
        pl.py += pl.vy * dt;
        pl.pz += pl.vz * dt;

        float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
        pl.px = std::clamp(pl.px, -half + 1.0f, half - 1.0f);
        pl.pz = std::clamp(pl.pz, -half + 1.0f, half - 1.0f);

        float groundY = effective_terrain_height(pl.px, pl.pz);
        if (pl.py <= groundY + 0.05f && pl.vy <= 0.0f) {
            pl.py = groundY;
            pl.vy = 0;
            pl.onGround = true;
        } else {
            pl.onGround = false;
        }

        col_wallSlide(pl);

        if (pl.py < -30.0f) {
            pl.px = 0; pl.py = terrain_height(0,0) + 2.0f; pl.pz = 0;
            pl.vx = pl.vy = pl.vz = 0;
        }

        if (pl.onGround) {
            update_trail(g_input_last_trail_x, g_input_last_trail_z, pl.px, pl.pz);
            g_input_last_trail_x = pl.px;
            g_input_last_trail_z = pl.pz;
        }

        c.x = pl.px;
        c.y = pl.py + PLAYER_HEIGHT;
        c.z = pl.pz;
    }
}
