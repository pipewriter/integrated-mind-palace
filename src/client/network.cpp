#include "network.h"
#include "app.h"
#include "textures.h"
#include "text.h"
#include "video.h"
#include "trail.h"
#include "terrain.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <algorithm>

bool net_connect(const char* host, uint16_t port) {
    g_net_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_valid(g_net_fd)) { perror("socket"); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(g_net_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); plat_close_socket(g_net_fd); g_net_fd = INVALID_SOCK; return false;
    }
    net_set_nonblock(g_net_fd);
    net_set_nodelay(g_net_fd);
    printf("Connected to server %s:%d\n", host, port);
    return true;
}

void net_send(uint8_t type, const void* payload, uint32_t plen) {
    if (!sock_valid(g_net_fd)) return;
    g_net_send.push(type, payload, plen);
}

void net_send_pos() {
    Camera& c = app.cam;
    float data[5] = { c.x, c.y, c.z, c.yaw, c.pitch };
    net_send(C2S_POS, data, 20);
}

void net_poll() {
    if (!sock_valid(g_net_fd)) return;
    // Non-blocking recv
    if (net_recv(g_net_fd, g_net_recv) < 0) {
        printf("Disconnected from server\n");
        plat_close_socket(g_net_fd); g_net_fd = INVALID_SOCK; return;
    }
    // Process complete messages
    uint8_t type; const uint8_t* payload; uint32_t msglen;
    while (net_try_read(g_net_recv, type, payload, msglen))
        handle_server_msg(type, payload, msglen);
    g_net_recv.compact();
    // Flush send queue
    if (g_net_send.pending()) {
        if (!g_net_send.flush(g_net_fd)) {
            printf("Send error, disconnected\n");
            plat_close_socket(g_net_fd); g_net_fd = INVALID_SOCK;
        }
    }
}

// Handle a single message from the server
void handle_server_msg(uint8_t type, const uint8_t* pl, uint32_t plen) {
    switch (type) {
    case S2C_WELCOME: {
        if (plen < 4) break;
        memcpy(&g_player_id, pl, 4);
        if (plen >= 8) {
            memcpy(&g_world_seed, pl + 4, 4);
            printf("Welcome! Player ID = %u, world seed = %u\n", g_player_id, g_world_seed);
        } else {
            printf("Welcome! Player ID = %u\n", g_player_id);
        }
        std::string title = "Exodia MP - Player " + std::to_string(g_player_id);
        glfwSetWindowTitle(app.window, title.c_str());
        break;
    }
    case S2C_WORLD_META: {
        if (plen < 4) break;
        uint32_t count;
        memcpy(&count, pl, 4);
        printf("Received world metadata: %u nodes\n", count);

        g_world.clear();
        std::vector<uint32_t> cached_hashes;

        for (uint32_t i = 0; i < count && 4 + (i+1)*45 <= plen; i++) {
            const uint8_t* p = pl + 4 + i * 45;
            DataNode wi;
            memcpy(&wi.hash, p, 4);
            memcpy(&wi.x, p+4, 4);  memcpy(&wi.y, p+8, 4);  memcpy(&wi.z, p+12, 4);
            memcpy(&wi.w, p+16, 4); memcpy(&wi.h, p+20, 4);
            memcpy(&wi.rot_x, p+24, 4); memcpy(&wi.rot_y, p+28, 4); memcpy(&wi.rot_z, p+32, 4);
            memcpy(&wi.img_w, p+36, 4); memcpy(&wi.img_h, p+40, 4);
            wi.node_type = (NodeType)p[44];

            // Try loading from local cache (images/videos only — text comes inline)
            if (wi.node_type != NODE_TEXT) {
                uint32_t cw, ch;
                std::vector<unsigned char> pixels;
                if (load_from_cache(wi.hash, cw, ch, pixels) && cw == wi.img_w && ch == wi.img_h) {
                    wi.pixels = std::move(pixels);
                    wi.texture_index = get_or_create_texture(wi.hash, wi.img_w, wi.img_h, wi.pixels.data());
                    cached_hashes.push_back(wi.hash);
                }
            }
            g_world.push_back(std::move(wi));
        }
        rebuild_quads();

        // Tell server which hashes we already have cached
        std::vector<uint8_t> msg(4 + cached_hashes.size() * 4);
        uint32_t cc = (uint32_t)cached_hashes.size();
        memcpy(msg.data(), &cc, 4);
        for (uint32_t i = 0; i < cc; i++)
            memcpy(msg.data() + 4 + i*4, &cached_hashes[i], 4);
        net_send(C2S_HAS_CACHE, msg.data(), (uint32_t)msg.size());

        printf("Loaded %u from cache, requesting %u from server\n",
               (uint32_t)cached_hashes.size(), count - (uint32_t)cached_hashes.size());
        break;
    }
    case S2C_NODE_DATA: {
        // hash(4) + node_type(1) + data_size(4) + data
        if (plen < 9) break;
        uint32_t hash;
        memcpy(&hash, pl, 4);
        NodeType nt = (NodeType)pl[4];
        uint32_t dsz;
        memcpy(&dsz, pl+5, 4);
        if (plen < 9 + dsz) break;
        const uint8_t* data = pl + 9;

        if (nt == NODE_TEXT) {
            // Store text on matching nodes and create glyph text
            std::string text((const char*)data, dsz);
            for (auto& wi : g_world) {
                if (wi.hash == hash && wi.node_type == NODE_TEXT && wi.text.empty()) {
                    wi.text = text;
                    wi.glyph_text_index = create_glyph_for_node(wi);
                }
            }
            printf("Received text data hash=%08x (%u bytes)\n", hash, dsz);
        } else {
            if (nt == NODE_VIDEO) {
                // Write to temp file, start video player
                std::string tmp = "image_cache/vid_" + std::to_string(hash) + ".tmp";
                FILE* tf = fopen(tmp.c_str(), "wb");
                if (tf) { fwrite(data, 1, dsz, tf); fclose(tf); }
                int vpi = start_video_player(tmp.c_str());
                for (auto& wi : g_world) {
                    if (wi.hash == hash && wi.node_type == NODE_VIDEO && wi.video_index < 0) {
                        wi.video_index = vpi;
                        if (vpi >= 0) wi.texture_index = app.video_players[vpi].tex_index;
                    }
                }
                printf("Received video data hash=%08x (%u bytes)\n", hash, dsz);
            } else {
                // IMAGE: data is RGBA pixels
                for (auto& wi : g_world) {
                    if (wi.hash == hash && wi.texture_index < 0) {
                        wi.pixels.assign(data, data + dsz);
                        if (wi.img_w > 0 && wi.img_h > 0) {
                            save_to_cache(hash, wi.img_w, wi.img_h, data);
                            wi.texture_index = get_or_create_texture(hash, wi.img_w, wi.img_h, data);
                        }
                    }
                }
                printf("Received image data hash=%08x (%u bytes)\n", hash, dsz);
            }
        }
        rebuild_quads();
        break;
    }
    case S2C_PLAYERS: {
        if (plen < 4) break;
        uint32_t count;
        memcpy(&count, pl, 4);
        g_remote_players.clear();
        for (uint32_t i = 0; i < count && 4 + (i+1)*24 <= plen; i++) {
            const uint8_t* p = pl + 4 + i * 24;
            RemotePlayer rp;
            memcpy(&rp.id, p, 4);
            memcpy(&rp.x, p+4, 4); memcpy(&rp.y, p+8, 4); memcpy(&rp.z, p+12, 4);
            memcpy(&rp.yaw, p+16, 4); memcpy(&rp.pitch, p+20, 4);
            if (rp.id != g_player_id)
                g_remote_players.push_back(rp);
        }
        break;
    }
    case S2C_NODE_ADD: {
        // 46-byte header: hash(4) + spatial(32) + img_w,img_h(8) + node_type(1) + has_data(1)
        // if has_data: data_size(4) + data
        if (plen < 46) break;
        DataNode wi;
        memcpy(&wi.hash, pl, 4);
        memcpy(&wi.x, pl+4, 4);  memcpy(&wi.y, pl+8, 4);  memcpy(&wi.z, pl+12, 4);
        memcpy(&wi.w, pl+16, 4); memcpy(&wi.h, pl+20, 4);
        memcpy(&wi.rot_x, pl+24, 4); memcpy(&wi.rot_y, pl+28, 4); memcpy(&wi.rot_z, pl+32, 4);
        memcpy(&wi.img_w, pl+36, 4); memcpy(&wi.img_h, pl+40, 4);
        wi.node_type = (NodeType)pl[44];
        uint8_t has_data = pl[45];

        if (has_data && plen >= 50) {
            uint32_t dsz;
            memcpy(&dsz, pl+46, 4);
            if (plen < 50 + dsz) break;
            const uint8_t* data = pl + 50;

            if (wi.node_type == NODE_TEXT) {
                wi.text.assign((const char*)data, dsz);
            } else if (wi.node_type == NODE_VIDEO) {
                wi.pixels.assign(data, data + dsz);
                std::string tmp = "image_cache/vid_" + std::to_string(wi.hash) + ".tmp";
                FILE* tf = fopen(tmp.c_str(), "wb");
                if (tf) { fwrite(data, 1, dsz, tf); fclose(tf); }
                int vpi = start_video_player(tmp.c_str());
                wi.video_index = vpi;
                if (vpi >= 0) wi.texture_index = app.video_players[vpi].tex_index;
            } else {
                wi.pixels.assign(data, data + dsz);
                if (wi.img_w > 0 && wi.img_h > 0) {
                    save_to_cache(wi.hash, wi.img_w, wi.img_h, data);
                    wi.texture_index = get_or_create_texture(wi.hash, wi.img_w, wi.img_h, data);
                }
            }
        } else if (wi.node_type == NODE_VIDEO) {
            // Reconnect to existing video player (e.g. after a move)
            std::string expected = "image_cache/vid_" + std::to_string(wi.hash) + ".tmp";
            for (int i = 0; i < (int)app.video_players.size(); i++) {
                if (app.video_players[i].active && app.video_players[i].path == expected) {
                    wi.video_index = i;
                    wi.texture_index = app.video_players[i].tex_index;
                    break;
                }
            }
            if (wi.video_index < 0) {
                // Try starting from cached temp file
                FILE* tf = fopen(expected.c_str(), "rb");
                if (tf) {
                    fclose(tf);
                    int vpi = start_video_player(expected.c_str());
                    wi.video_index = vpi;
                    if (vpi >= 0) wi.texture_index = app.video_players[vpi].tex_index;
                }
            }
        } else if (wi.node_type != NODE_TEXT) {
            // Try cache for image nodes
            auto it = g_hash_to_tex.find(wi.hash);
            if (it != g_hash_to_tex.end()) {
                wi.texture_index = it->second;
            } else {
                uint32_t cw, ch;
                std::vector<unsigned char> pixels;
                if (load_from_cache(wi.hash, cw, ch, pixels)) {
                    wi.texture_index = get_or_create_texture(wi.hash, cw, ch, pixels.data());
                    wi.pixels = std::move(pixels);
                }
            }
        }

        // Create glyph text for text nodes with inline data
        if (wi.node_type == NODE_TEXT && !wi.text.empty()) {
            wi.glyph_text_index = create_glyph_for_node(wi);
        }
        g_world.push_back(std::move(wi));
        rebuild_quads();
        printf("Node added (type=%d, hash=%08x)\n", g_world.back().node_type, g_world.back().hash);
        break;
    }
    case S2C_NODE_REMOVE: {
        if (plen < 16) break;
        uint32_t hash;
        float rx, ry, rz;
        memcpy(&hash, pl, 4);
        memcpy(&rx, pl+4, 4); memcpy(&ry, pl+8, 4); memcpy(&rz, pl+12, 4);

        // Find and remove the image with matching hash + closest position
        int best = -1;
        float best_d = 1e18f;
        for (int i = 0; i < (int)g_world.size(); i++) {
            if (g_world[i].hash == hash) {
                float dx = g_world[i].x - rx, dy = g_world[i].y - ry, dz = g_world[i].z - rz;
                float d = dx*dx + dy*dy + dz*dz;
                if (d < best_d) { best_d = d; best = i; }
            }
        }
        if (best >= 0) {
            auto& node = g_world[best];
            // Save to deleted stack for Z-key recovery (skip if this was our own pickup)
            extern uint32_t g_last_pickup_hash;
            if (node.hash != g_last_pickup_hash) {
                InventoryItem deleted;
                deleted.hash = node.hash;
                deleted.node_type = node.node_type;
                deleted.img_w = node.img_w; deleted.img_h = node.img_h;
                deleted.w = node.w; deleted.h = node.h;
                deleted.text = node.text;
                deleted.from_server = false;
                g_deleted_stack.push_back(deleted);
            }
            g_last_pickup_hash = 0;

            if (node.glyph_text_index >= 0 && node.glyph_text_index < (int)app.glyph_texts.size()) {
                destroy_glyph_text(app.glyph_texts[node.glyph_text_index]);
                app.glyph_texts[node.glyph_text_index] = nullptr;
            }
            g_world.erase(g_world.begin() + best);
            rebuild_quads();
            printf("Node removed hash=%08x\n", hash);
        }
        break;
    }
    case S2C_INV_PUSH: {
        g_inv_count++;
        printf("Inventory: %d items\n", g_inv_count);
        break;
    }
    case S2C_INV_POP: {
        if (g_inv_count > 0) g_inv_count--;
        printf("Inventory: %d items\n", g_inv_count);
        break;
    }
    case S2C_SYNC_DONE: {
        g_synced = true;
        printf("Initial sync complete! %d world nodes loaded\n", (int)g_world.size());
        // Default to walk mode on the ground
        {
            Camera& cam = app.cam;
            float ground = effective_terrain_height(cam.x, cam.z);
            cam.y = ground + PLAYER_HEIGHT;
            app.walker.px = cam.x;
            app.walker.py = ground;
            app.walker.pz = cam.z;
            app.walker.vx = app.walker.vy = app.walker.vz = 0;
            app.walker.onGround = true;
            // Trail tracking is managed by input module
            app.move_mode = MODE_WALK;
            printf("Auto-started in WALK mode on ground\n");
        }
        break;
    }
    case S2C_TRAIL_DATA: {
        if (plen < 4) break;
        uint32_t tex_size;
        memcpy(&tex_size, pl, 4);
        if (tex_size != TRAIL_TEX_SIZE) {
            printf("Trail size mismatch: server=%u, client=%d\n", tex_size, TRAIL_TEX_SIZE);
            break;
        }
        size_t count = (size_t)tex_size * tex_size;
        if (plen < 4 + count) break;
        apply_full_trail_data(pl + 4, tex_size);
        break;
    }
    case S2C_TRAIL_WALK: {
        if (plen < 20) break;
        uint32_t player_id;
        float prev_x, prev_z, curr_x, curr_z;
        memcpy(&player_id, pl, 4);
        memcpy(&prev_x, pl+4, 4); memcpy(&prev_z, pl+8, 4);
        memcpy(&curr_x, pl+12, 4); memcpy(&curr_z, pl+16, 4);
        if (player_id != g_player_id) {
            apply_remote_trail_walk(prev_x, prev_z, curr_x, curr_z);
        }
        break;
    }
    }
}

// Rebuild app.quads from g_world (call after any world mutation)
void rebuild_quads() {
    app.quads.clear();
    for (auto& wi : g_world) {
        if (wi.texture_index >= 0) {
            app.quads.push_back({wi.x, wi.y, wi.z, wi.w, wi.h, wi.rot_x, wi.rot_y, wi.rot_z, wi.texture_index});
        }
    }
}

// Upload all world images to GPU and rebuild quads
void upload_world_images() {
    for (auto& wi : g_world) {
        if (wi.texture_index < 0 && !wi.pixels.empty()) {
            wi.texture_index = create_texture_from_pixels(
                wi.pixels.data(), wi.img_w, wi.img_h, "world_image");
        }
    }
    rebuild_quads();
}
