// ================================================================
// Exodia MP — Client entry point and main loop
//
// A networked virtual world where players explore procedurally
// generated terrain, place images/videos/text in 3D space, and
// leave persistent footprint trails on terrain and buildings.
//
// Controls:
//   WASD/Space/Ctrl  Move       Mouse   Look
//   Shift            Fast       E       Pickup nearest object
//   Q                Putdown    F       Toggle fog
//   Tab              Fly/Walk   T       Type text
//   C                Clone      Delete  Delete nearest
//   Backtick         Menu       Escape  Quit
// ================================================================

#include "app.h"
#include "terrain.h"
#include "structures.h"
#include "collision.h"
#include "trail.h"
#include "sky.h"
#include "vulkan_setup.h"
#include "textures.h"
#include "video.h"
#include "text.h"
#include "input.h"
#include "network.h"
#include "renderer.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <sys/stat.h>

// ----------------------------------------------------------------
// Cone-based selection — find the best target in front of the camera
// ----------------------------------------------------------------

static void update_selection() {
    Camera& c = app.cam;
    c.update_vectors();

    g_selected_world_idx = -1;
    g_selected_quad_idx = -1;
    g_selected_glyph_idx = -1;

    constexpr float MAX_DIST = 20.0f;
    constexpr float D = 3.14159265f / 180.0f;
    const float CONE_COS = cosf(25.0f * D); // 25-degree half-angle

    float best_score = 1e18f;

    for (int i = 0; i < (int)g_world.size(); i++) {
        auto& node = g_world[i];
        // Only select objects with a visual representation
        if (node.texture_index < 0 && node.glyph_text_index < 0) continue;

        float dx = node.x - c.x;
        float dy = node.y - c.y;
        float dz = node.z - c.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);

        if (dist > MAX_DIST || dist < 0.001f) continue;

        // Direction to object (normalized)
        float nx = dx / dist, ny = dy / dist, nz = dz / dist;
        // Dot product with camera forward = cos(angle)
        float dot = c.fx * nx + c.fy * ny + c.fz * nz;

        if (dot < CONE_COS) continue; // Outside cone

        // Score: prefer objects closer to center of cone AND closer in distance
        // angle_factor: 0 = dead center, 1 = edge of cone
        float angle_factor = 1.0f - (dot - CONE_COS) / (1.0f - CONE_COS);
        float score = dist * (1.0f + angle_factor * 2.0f);

        if (score < best_score) {
            best_score = score;
            g_selected_world_idx = i;
        }
    }

    // Compute quad and glyph indices for the renderer
    if (g_selected_world_idx >= 0) {
        auto& node = g_world[g_selected_world_idx];
        if (node.texture_index >= 0) {
            int qi = 0;
            for (int i = 0; i < g_selected_world_idx; i++) {
                if (g_world[i].texture_index >= 0) qi++;
            }
            g_selected_quad_idx = qi;
        }
        if (node.glyph_text_index >= 0) {
            g_selected_glyph_idx = node.glyph_text_index;
        }
    }
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    using Clock = std::chrono::high_resolution_clock;

    // Parse flags
    const char* server_host = "127.0.0.1";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) server_host = argv[++i];
    }

    // Create runtime directories
    mkdir("image_cache", 0755);

    // ---- Load world seed ----
    {
        FILE* sf = fopen("seed.txt", "r");
        if (sf) {
            uint32_t s = 0;
            if (fscanf(sf, "%u", &s) == 1 && s != 0) {
                g_world_seed = s;
                printf("Loaded seed: %u\n", g_world_seed);
            }
            fclose(sf);
        } else {
            // Generate and save a random seed
            g_world_seed = (uint32_t)std::random_device{}();
            if (g_world_seed == 0) g_world_seed = 1;
            FILE* wf = fopen("seed.txt", "w");
            if (wf) { fprintf(wf, "%u\n", g_world_seed); fclose(wf); }
            printf("Generated new seed: %u\n", g_world_seed);
        }
        g_variance = make_default_sampler(g_world_seed);
    }

    // ---- Generate terrain (deterministic, same on all clients) ----
    auto t0 = Clock::now();
    printf("Generating terrain...\n");
    Mesh mesh = generate_terrain();
    auto t1 = Clock::now();
    printf("Terrain: %.1f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    // ---- Key bindings ----
    g_keys.set_defaults();
    g_keys.load("keybinds.cfg");

    // ---- GLFW + Vulkan initialization ----
    if (!glfwInit()) { fprintf(stderr, "GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    app.window = glfwCreateWindow(INIT_WIDTH, INIT_HEIGHT, "Exodia MP (connecting...)", nullptr, nullptr);
    glfwSetFramebufferSizeCallback(app.window, fb_resize_cb);
    glfwSetCursorPosCallback(app.window, mouse_cb);
    glfwSetMouseButtonCallback(app.window, mouse_button_cb);
    glfwSetDropCallback(app.window, drop_cb);
    glfwSetKeyCallback(app.window, key_cb);
    glfwSetCharCallback(app.window, char_callback);
    glfwSetInputMode(app.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    create_instance();
    vk_check(glfwCreateWindowSurface(app.instance, app.window, nullptr, &app.surface), "surface");
    pick_physical_device();
    create_device();
    create_swapchain();
    create_render_pass();
    create_descriptor_resources();
    create_sky_descriptors();
    create_pipelines();
    create_depth_buffer();
    create_framebuffers();
    create_command_objects();
    create_sync();

    // ---- Keep terrain vertices for trail depression system ----
    g_terrain_verts = mesh.vertices;
    g_original_heights.resize(GRID_SIZE * GRID_SIZE);
    for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++)
        g_original_heights[i] = mesh.vertices[i].py;

    // Upload terrain mesh
    upload_buffer(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex),
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, app.terrain_vbuf, app.terrain_vmem);
    upload_buffer(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t),
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, app.terrain_ibuf, app.terrain_imem);
    app.terrain_idx_count = (uint32_t)mesh.indices.size();
    mesh.vertices.clear(); mesh.indices.clear();

    // ---- Generate structures + plants ----
    {
        printf("Generating structures...\n");
        StructMesh smesh = generate_structures();
        printf("Generating plants...\n");
        StructMesh plants = generate_plants();

        // Merge plants into structures
        uint32_t base = (uint32_t)smesh.vertices.size();
        smesh.vertices.insert(smesh.vertices.end(), plants.vertices.begin(), plants.vertices.end());
        for (auto idx : plants.indices) smesh.indices.push_back(idx + base);

        if (!smesh.vertices.empty()) {
            g_struct_verts = smesh.vertices;
            g_struct_verts_orig = smesh.vertices;
            g_struct_wear.resize(smesh.vertices.size(), 0.0f);
            build_struct_grid();

            upload_buffer(smesh.vertices.data(), smesh.vertices.size() * sizeof(ColorVertex),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, app.struct_vbuf, app.struct_vmem);
            upload_buffer(smesh.indices.data(), smesh.indices.size() * sizeof(uint32_t),
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, app.struct_ibuf, app.struct_imem);
            app.struct_idx_count = (uint32_t)smesh.indices.size();

            extract_collision_tris(smesh);
        }
    }

    // ---- Procedural terrain texture ----
    {
        auto tex_pixels = generate_terrain_texture(TRAIL_TEX_SIZE);
        g_terrain_pixels_orig = tex_pixels;
        g_terrain_pixels = tex_pixels;
        app.terrain_tex_index = create_texture_from_pixels(
            tex_pixels.data(), TRAIL_TEX_SIZE, TRAIL_TEX_SIZE, "terrain_procedural");
    }

    // ---- Initialize trail map ----
    g_trail.resize((size_t)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE, 0.0f);

    // Create persistent staging buffers for trail system
    {
        VkDeviceSize tex_size = (VkDeviceSize)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE * 4;
        create_buffer(tex_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      g_trail_tex_staging, g_trail_tex_staging_mem);
        vkMapMemory(app.device, g_trail_tex_staging_mem, 0, tex_size, 0, &g_trail_tex_staging_mapped);

        VkDeviceSize vert_size = g_terrain_verts.size() * sizeof(Vertex);
        create_buffer(vert_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      g_terrain_vert_staging, g_terrain_vert_staging_mem);
        vkMapMemory(app.device, g_terrain_vert_staging_mem, 0, vert_size, 0, &g_terrain_vert_staging_mapped);

        if (!g_struct_verts.empty()) {
            VkDeviceSize struct_size = g_struct_verts.size() * sizeof(ColorVertex);
            create_buffer(struct_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          g_struct_vert_staging, g_struct_vert_staging_mem);
            vkMapMemory(app.device, g_struct_vert_staging_mem, 0, struct_size, 0, &g_struct_vert_staging_mapped);
        }
    }

    // Upload shared quad geometry
    create_quad_geometry();

    // ---- Marker textures for remote players ----
    {
        unsigned char colors[4][3] = {{255,40,40},{40,40,255},{40,255,40},{255,255,40}};
        for (int c = 0; c < 4; c++) {
            unsigned char px[8*8*4];
            for (int i = 0; i < 8*8; i++) {
                px[i*4+0] = colors[c][0]; px[i*4+1] = colors[c][1];
                px[i*4+2] = colors[c][2]; px[i*4+3] = 255;
            }
            g_marker_tex[c] = create_texture_from_pixels(px, 8, 8, "marker");
        }
    }

    // ---- Font atlas for text nodes ----
    app.font_atlas_index = create_font_atlas_texture();
    printf("Font atlas created (texture index %d)\n", app.font_atlas_index);

    // Menu text overlay — screen-space HUD (position in pixels, char_size in pixels)
    g_menu_text = create_glyph_text(20, -30, 0, 18.0f, 0, 0.0f, 1.0f, 0.2f, 512);
    g_menu_text->is_hud = true;
    app.glyph_texts.push_back(g_menu_text);

    // Inventory HUD text (right of thumbnails, 4 rows per item with char_size=14 => 56px per slot)
    g_inv_hud_text = create_glyph_text(66, -10, 0, 14.0f, 0, 0.9f, 0.9f, 0.9f, 512);
    g_inv_hud_text->is_hud = true;
    app.glyph_texts.push_back(g_inv_hud_text);

    // Toast notification text (right side, yellow-ish)
    g_toast_hud_text = create_glyph_text(0, -40, 0, 16.0f, 0, 1.0f, 1.0f, 0.5f, 256);
    g_toast_hud_text->is_hud = true;
    app.glyph_texts.push_back(g_toast_hud_text);

    // ---- Skybox initialization ----
    app.sky_rng.seed(std::random_device{}());
    app.current_preset = generate_random_preset(app.sky_rng);
    app.display_preset = app.current_preset;
    app.target_preset = app.current_preset;
    app.sky_transition_t = 1.0f;
    printf("Skybox initialized with random preset\n");

    // ---- Connect to server ----
    printf("Connecting to %s:%d...\n", server_host, NET_PORT);
    if (!net_connect(server_host)) {
        fprintf(stderr, "Failed to connect to server!\n");
        cleanup();
        return 1;
    }

    // Blocking initial sync (welcome + world_meta + data + trail)
    {
        int flags = fcntl(g_net_fd, F_GETFL, 0);
        fcntl(g_net_fd, F_SETFL, flags & ~O_NONBLOCK);

        auto start = Clock::now();
        while (!g_synced) {
            glfwPollEvents();
            if (glfwWindowShouldClose(app.window)) break;

            uint8_t tmp[65536];
            ssize_t n = recv(g_net_fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                if (n == 0) { printf("Server closed connection during sync\n"); break; }
                if (errno == EINTR) continue;
                break;
            }
            g_net_recv.append(tmp, n);

            uint8_t type; const uint8_t* payload; uint32_t plen;
            while (net_try_read(g_net_recv, type, payload, plen))
                handle_server_msg(type, payload, plen);
            g_net_recv.compact();

            if (g_net_send.pending()) g_net_send.flush(g_net_fd);

            auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            if (elapsed > 120.0) { printf("Sync timeout\n"); break; }
        }

        net_set_nonblock(g_net_fd);
    }

    printf("\nReady! %d world images, %d textures, player %u\n",
           (int)g_world.size(), (int)app.textures.size(), g_player_id);
    printf("Controls: WASD move, Mouse look, Tab fly/walk, E pickup, Q putdown, F fog, Esc quit\n");
    printf("Clipboard images auto-detected every 2s\n\n");

    // Warm-up frames
    for (int i = 0; i < 5; i++) { glfwPollEvents(); draw_frame(); }
    vkDeviceWaitIdle(app.device);

    // ================================================================
    // Main loop
    // ================================================================
    double last = glfwGetTime(), fps_t = last, pos_t = last;
    int fps_c = 0;

    while (!glfwWindowShouldClose(app.window)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)(now - last);
        last = now;
        g_time += dt;

        // Sky preset transitions (~30s cycle)
        if (app.sky_transition_t < 1.0f) {
            app.sky_transition_t += dt * 0.033f;
            if (app.sky_transition_t >= 1.0f) {
                app.sky_transition_t = 1.0f;
                app.current_preset = app.target_preset;
            }
            app.display_preset = lerp_preset(app.current_preset, app.target_preset,
                app.sky_transition_t * app.sky_transition_t * (3.0f - 2.0f * app.sky_transition_t));
        } else {
            static float sky_timer = 0;
            sky_timer += dt;
            if (sky_timer > 30.0f) {
                sky_timer = 0;
                app.target_preset = generate_random_preset(app.sky_rng);
                app.sky_transition_t = 0.0f;
            }
        }

        // Networking: receive messages, send position at 20Hz
        net_poll();
        if (now - pos_t >= 0.05) {
            pos_t = now;
            net_send_pos();
        }

        // FPS + memory stats
        fps_c++;
        if (now - fps_t >= 1) {
            long rss = 0;
            if (FILE* f = fopen("/proc/self/statm", "r")) {
                long dummy; fscanf(f, "%ld %ld", &dummy, &rss); fclose(f);
                rss = rss * 4096L / (1024L * 1024L);
            }
            printf("FPS: %d  RSS: %ld MB  World: %d  Inv: %d  Players: %d\n",
                   fps_c, rss, (int)g_world.size(), g_inv_count, (int)g_remote_players.size() + 1);
            fps_c = 0; fps_t = now;
        }

        process_input(dt);
        update_selection();
        check_clipboard();
        check_media_folder();

        // Update video decode budget and audio selection
        update_video_budget();

        // Advance video playback (only for budgeted videos)
        for (auto& vp : app.video_players) {
            if (!vp.active || !vp.decoding) continue;
            auto elapsed = std::chrono::duration<double>(
                App::VideoPlayer::VClock::now() - vp.play_start).count();
            int decoded = 0;
            while (vp.cur_pts < elapsed && decoded < 2) {
                if (!decode_video_frame(vp)) {
                    seek_video_to_start(vp);
                    vp.play_start = App::VideoPlayer::VClock::now();
                    decode_video_frame(vp);
                    break;
                }
                decoded++;
            }
            if (decoded > 0) {
                upload_video_to_staging(vp);
                vp.has_new_frame = true;
            }
        }

        // Update menu text (centered on screen)
        if (g_menu_text) {
            if (app.menu_open) {
                build_menu_text();
                // Center menu on screen based on text dimensions
                int max_cols = 0, rows = 1;
                int col = 0;
                for (char ch : g_menu_text->text) {
                    if (ch == '\n') { rows++; col = 0; }
                    else { col++; if (col > max_cols) max_cols = col; }
                }
                float text_w = max_cols * g_menu_text->char_size;
                float text_h = rows * g_menu_text->char_size;
                float sw = (float)app.sc_extent.width;
                float sh = (float)app.sc_extent.height;
                g_menu_text->x = (sw - text_w) * 0.5f;
                g_menu_text->y = -(sh - text_h) * 0.5f;
            } else {
                g_menu_text->char_count = 0;
            }
        }

        // Update inventory HUD text
        if (g_inv_hud_text) {
            if (!g_inventory.empty()) {
                char buf[2048]; char* bp = buf;
                int count = std::min((int)g_inventory.size(), 10);
                for (int i = 0; i < count; i++) {
                    int inv_idx = (int)g_inventory.size() - 1 - i;
                    auto& item = g_inventory[inv_idx];
                    char marker = (i == 0) ? '>' : ' ';
                    if (item.node_type == NODE_IMAGE)
                        bp += sprintf(bp, "%cIMG %ux%u", marker, item.img_w, item.img_h);
                    else if (item.node_type == NODE_TEXT) {
                        std::string preview = item.text.substr(0, 10);
                        for (char& ch : preview) if (ch == '\n') ch = ' ';
                        bp += sprintf(bp, "%cTXT %s", marker, preview.c_str());
                    } else if (item.node_type == NODE_VIDEO)
                        bp += sprintf(bp, "%cVID %ux%u", marker, item.img_w, item.img_h);
                    // 4 lines per item (14px * 4 = 56px, matches thumbnail spacing)
                    bp += sprintf(bp, "\n\n\n\n");
                }
                g_inv_hud_text->text = buf;
                update_glyph_text(*g_inv_hud_text);
            } else {
                g_inv_hud_text->char_count = 0;
            }
        }

        // Update toast messages (right-justified)
        if (g_toast_hud_text) {
            // Remove old toasts (>3 seconds)
            g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(),
                [now](const ToastMessage& t) { return now - t.created_at > 3.0; }),
                g_toasts.end());
            if (!g_toasts.empty()) {
                // Find longest toast line to right-justify
                int max_len = 0;
                for (auto& t : g_toasts)
                    max_len = std::max(max_len, (int)t.text.size());
                // Right-pad shorter lines so text is right-justified
                char buf[1024]; char* bp = buf;
                for (auto& t : g_toasts) {
                    int pad = max_len - (int)t.text.size();
                    for (int i = 0; i < pad; i++) *bp++ = ' ';
                    bp += sprintf(bp, "%s\n", t.text.c_str());
                }
                float text_width = max_len * g_toast_hud_text->char_size;
                float margin = 10.0f;
                g_toast_hud_text->x = (float)app.sc_extent.width - text_width - margin;
                g_toast_hud_text->text = buf;
                update_glyph_text(*g_toast_hud_text);
            } else {
                g_toast_hud_text->char_count = 0;
            }
        }

        // Add marker quads for remote players
        size_t world_quad_count = app.quads.size();
        for (auto& rp : g_remote_players) {
            float py = terrain_height(rp.x, rp.z) + 6.0f;
            int tex = g_marker_tex[rp.id % 4];
            if (tex >= 0) app.quads.push_back({rp.x, py, rp.z, 2.0f, 8.0f, 0, 0, 0, tex});
        }

        draw_frame();

        // Remove marker quads (they're re-added each frame)
        app.quads.resize(world_quad_count);
    }

    // ---- Shutdown ----
    if (g_net_fd >= 0) close(g_net_fd);
    cleanup();
    return 0;
}
