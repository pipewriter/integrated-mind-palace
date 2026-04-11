// ================================================================
// Exodia Multiplayer Server — Low-RAM Authoritative TCP server
//
// Architecture:
//   - poll()-based non-blocking I/O multiplexer
//   - Authoritative world state (all mutations go through server)
//   - Binary message protocol (shared with client via net.h)
//   - Disk-backed media: node data stored in media/ directory
//   - LRU cache keeps hot media in memory (configurable, default 128MB)
//   - Persistent world save (world.sav metadata + separate media files)
//   - Hash-based node caching (clients report cached hashes)
//
// Memory footprint: ~16MB trail map + ~100 bytes/node metadata + LRU cache
// With 1000 nodes and 128MB cache: ~145MB RSS (vs ~1GB+ with all data in RAM)
//
// Subsystems:
//   1. Trail painting + persistence (2048x2048 float map)
//   2. Media storage + LRU cache (disk-backed, hash-addressed)
//   3. World state (vector of SrvNode metadata with save/load)
//   4. Client management (connection, inventory, sync)
//   5. Message dispatch (C2S handlers)
//   6. Periodic tasks (broadcast positions, save state, RAM reporting)
//
// Build: g++ -std=c++17 -O2 src/server/main.cpp -o server
// ================================================================

#include "../shared/net.h"
#include "../shared/constants.h"
#include <csignal>
#include <set>
#include <map>
#include <list>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <cmath>
#include <ctime>

// ================================================================
// RAM usage reporting (cross-platform via platform.h)
// ================================================================

static void print_ram_usage() { plat_print_ram(); }

// ================================================================
// Trail system — server-side footprint map
// ================================================================

static std::vector<float> g_trail;
static bool g_trail_dirty = false;
static const char* TRAIL_SAVE_PATH = "trails.sav";
static constexpr uint32_t TRAIL_MAGIC = 0x5452414C; // "TRAL"

static void paint_trail_at(float wx, float wz, float intensity) {
    float half = (GRID_SIZE - 1) * GRID_SCALE * 0.5f;
    float world_to_trail = (float)(TRAIL_TEX_SIZE - 1) / (2.0f * half);

    int cx = (int)((wx + half) * world_to_trail);
    int cz = (int)((wz + half) * world_to_trail);
    int brush_cells = (int)(TRAIL_BRUSH_RADIUS * world_to_trail) + 1;
    float cell_world_size = 1.0f / world_to_trail;

    for (int dz = -brush_cells; dz <= brush_cells; dz++) {
        for (int dx = -brush_cells; dx <= brush_cells; dx++) {
            int tx = cx + dx, tz = cz + dz;
            if (tx < 0 || tx >= TRAIL_TEX_SIZE || tz < 0 || tz >= TRAIL_TEX_SIZE) continue;

            float dist = sqrtf((float)(dx*dx + dz*dz)) * cell_world_size;
            if (dist > TRAIL_BRUSH_RADIUS) continue;

            float falloff = 1.0f - dist / TRAIL_BRUSH_RADIUS;
            falloff = falloff * falloff;

            int idx = tz * TRAIL_TEX_SIZE + tx;
            g_trail[idx] = std::min(1.0f, g_trail[idx] + intensity * falloff);
        }
    }
}

static void apply_trail_walk(float prev_x, float prev_z, float curr_x, float curr_z) {
    float dx = curr_x - prev_x;
    float dz = curr_z - prev_z;
    float moved = sqrtf(dx*dx + dz*dz);
    if (moved < 0.05f) return;

    int steps = (int)(moved / 0.3f) + 1;
    float step_intensity = (moved / (float)steps) * TRAIL_PAINT_PER_UNIT;

    for (int s = 0; s <= steps; s++) {
        float t = (float)s / (float)steps;
        float sx = prev_x + dx * t;
        float sz = prev_z + dz * t;
        paint_trail_at(sx, sz, step_intensity);
    }
    g_trail_dirty = true;
}

static bool save_trails() {
    std::string tmp = std::string(TRAIL_SAVE_PATH) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "Failed to write %s\n", tmp.c_str()); return false; }

    uint32_t magic = TRAIL_MAGIC;
    uint32_t tex_size = TRAIL_TEX_SIZE;
    fwrite(&magic, 4, 1, f);
    fwrite(&tex_size, 4, 1, f);

    size_t count = (size_t)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE;
    std::vector<uint8_t> quantized(count);
    for (size_t i = 0; i < count; i++)
        quantized[i] = (uint8_t)(std::min(1.0f, g_trail[i]) * 255.0f);
    fwrite(quantized.data(), 1, count, f);

    fclose(f);
    if (rename(tmp.c_str(), TRAIL_SAVE_PATH) != 0) {
        fprintf(stderr, "Failed to rename trail save file\n");
        plat_unlink(tmp.c_str()); return false;
    }
    printf("Saved trail data (%u x %u) to %s\n", tex_size, tex_size, TRAIL_SAVE_PATH);
    g_trail_dirty = false;
    return true;
}

static bool load_trails() {
    FILE* f = fopen(TRAIL_SAVE_PATH, "rb");
    if (!f) return false;

    uint32_t magic, tex_size;
    if (fread(&magic, 4, 1, f) != 1 || magic != TRAIL_MAGIC) { fclose(f); return false; }
    if (fread(&tex_size, 4, 1, f) != 1 || tex_size != TRAIL_TEX_SIZE) { fclose(f); return false; }

    size_t count = (size_t)tex_size * tex_size;
    std::vector<uint8_t> quantized(count);
    if (fread(quantized.data(), 1, count, f) != count) { fclose(f); return false; }
    fclose(f);

    g_trail.resize(count);
    for (size_t i = 0; i < count; i++)
        g_trail[i] = (float)quantized[i] / 255.0f;

    printf("Loaded trail data (%u x %u) from %s\n", tex_size, tex_size, TRAIL_SAVE_PATH);
    return true;
}

// ================================================================
// Disk-backed media storage + LRU cache
//
// All node media (image pixels, video bytes) is stored on disk in
// media/<hash_hex>.dat files. An LRU cache keeps recently-accessed
// media in memory to avoid repeated disk reads.
// ================================================================

static const char* MEDIA_DIR = "media";
static constexpr size_t LRU_CACHE_MAX_BYTES = 128 * 1024 * 1024; // 128MB

static std::string media_path(uint32_t hash) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/%08x.dat", MEDIA_DIR, hash);
    return buf;
}

static bool write_media_file(uint32_t hash, const uint8_t* data, uint32_t size) {
    std::string path = media_path(hash);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Failed to write media file %s\n", path.c_str());
        return false;
    }
    if (size > 0) fwrite(data, 1, size, f);
    fclose(f);
    return true;
}

static std::vector<uint8_t> read_media_file(uint32_t hash) {
    std::string path = media_path(hash);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    if (sz > 0 && fread(data.data(), 1, sz, f) != (size_t)sz) {
        fclose(f);
        fprintf(stderr, "Warning: short read on media file %s\n", path.c_str());
        return {};
    }
    fclose(f);
    return data;
}

struct MediaCache {
    struct Entry {
        uint32_t hash;
        std::vector<uint8_t> data;
    };

    std::list<Entry> lru_list;
    std::unordered_map<uint32_t, std::list<Entry>::iterator> lookup;
    size_t current_bytes = 0;
    size_t max_bytes;

    MediaCache(size_t max = LRU_CACHE_MAX_BYTES) : max_bytes(max) {}

    std::vector<uint8_t>* get(uint32_t hash) {
        auto it = lookup.find(hash);
        if (it == lookup.end()) return nullptr;
        lru_list.splice(lru_list.begin(), lru_list, it->second);
        return &it->second->data;
    }

    void put(uint32_t hash, std::vector<uint8_t> data) {
        auto it = lookup.find(hash);
        if (it != lookup.end()) {
            current_bytes -= it->second->data.size();
            it->second->data = std::move(data);
            current_bytes += it->second->data.size();
            lru_list.splice(lru_list.begin(), lru_list, it->second);
            return;
        }
        size_t needed = data.size();
        while (current_bytes + needed > max_bytes && !lru_list.empty()) {
            auto& back = lru_list.back();
            current_bytes -= back.data.size();
            lookup.erase(back.hash);
            lru_list.pop_back();
        }
        lru_list.push_front({hash, std::move(data)});
        lookup[hash] = lru_list.begin();
        current_bytes += lru_list.front().data.size();
    }

    void remove(uint32_t hash) {
        auto it = lookup.find(hash);
        if (it == lookup.end()) return;
        current_bytes -= it->second->data.size();
        lru_list.erase(it->second);
        lookup.erase(it);
    }
};

static MediaCache g_media_cache;

// Get media data: check cache first, then disk. Returns pointer to cached
// data valid until the next cache mutation (safe in single-threaded use).
static const std::vector<uint8_t>* get_media(uint32_t hash) {
    auto* cached = g_media_cache.get(hash);
    if (cached) return cached;
    auto data = read_media_file(hash);
    if (data.empty()) return nullptr;
    g_media_cache.put(hash, std::move(data));
    return g_media_cache.get(hash);
}

// Write media to disk and insert into LRU cache
static bool store_media(uint32_t hash, const uint8_t* data, uint32_t size) {
    if (!write_media_file(hash, data, size)) return false;
    std::vector<uint8_t> vec(data, data + size);
    g_media_cache.put(hash, std::move(vec));
    return true;
}

// ================================================================
// Server-side DataNode (metadata only — media data on disk)
// ================================================================

struct SrvNode {
    NodeType node_type = NODE_IMAGE;
    float x, y, z, w, h, rot_x, rot_y, rot_z;
    uint32_t img_w, img_h;
    uint32_t data_size = 0;   // size of media data on disk (0 for TEXT nodes)
    std::string text;          // non-empty only for TEXT nodes (kept in memory)
    uint32_t hash;
};

static std::vector<SrvNode> g_world;
static const char* SAVE_PATH = "world.sav";
static constexpr uint32_t SAVE_MAGIC_V2 = 0x45584F02;
static constexpr uint32_t SAVE_MAGIC_V3 = 0x45584F03;
static constexpr uint32_t SAVE_MAGIC_V4 = 0x45584F04; // disk-backed media

// ================================================================
// Client connection state
// ================================================================

struct Client {
    socket_t fd;
    uint32_t id;
    float x = 0, y = 60, z = 0, yaw = -90, pitch = 0;
    NetBuf recv_buf;
    SendQ send_q;
    std::vector<SrvNode> inventory;
    std::set<uint32_t> known_hashes;
    bool synced = false;
};

static std::map<socket_t, Client> g_clients;
static uint32_t g_next_id = 1;
static volatile bool g_running = true;
static bool g_world_dirty = false;

// ================================================================
// World persistence — V4 disk-backed format
//
// V4 stores only metadata in world.sav. Media data lives in media/
// directory as individual files named by hash. V2/V3 files are
// automatically migrated on first load.
// ================================================================

static bool save_world() {
    std::string tmp = std::string(SAVE_PATH) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "Failed to write %s\n", tmp.c_str()); return false; }

    uint32_t magic = SAVE_MAGIC_V4;
    uint32_t count = (uint32_t)g_world.size();
    fwrite(&magic, 4, 1, f);
    fwrite(&count, 4, 1, f);

    for (auto& node : g_world) {
        uint8_t nt = (uint8_t)node.node_type;
        fwrite(&nt, 1, 1, f);
        fwrite(&node.x, 4, 1, f); fwrite(&node.y, 4, 1, f); fwrite(&node.z, 4, 1, f);
        fwrite(&node.w, 4, 1, f); fwrite(&node.h, 4, 1, f);
        fwrite(&node.rot_x, 4, 1, f); fwrite(&node.rot_y, 4, 1, f); fwrite(&node.rot_z, 4, 1, f);
        fwrite(&node.img_w, 4, 1, f); fwrite(&node.img_h, 4, 1, f);
        fwrite(&node.hash, 4, 1, f);
        if (node.node_type == NODE_TEXT) {
            uint32_t tlen = (uint32_t)node.text.size();
            fwrite(&tlen, 4, 1, f);
            fwrite(node.text.data(), 1, tlen, f);
        } else {
            fwrite(&node.data_size, 4, 1, f);
            // Media data is on disk in media/<hash>.dat — not stored inline
        }
    }
    fclose(f);
    if (rename(tmp.c_str(), SAVE_PATH) != 0) {
        fprintf(stderr, "Failed to rename save file\n");
        plat_unlink(tmp.c_str()); return false;
    }
    printf("Saved %u nodes to %s (v4 disk-backed format)\n", count, SAVE_PATH);
    return true;
}

static bool load_world() {
    FILE* f = fopen(SAVE_PATH, "rb");
    if (!f) return false;
    uint32_t magic, count;
    if (fread(&magic, 4, 1, f) != 1) { fclose(f); return false; }

    bool v2 = (magic == SAVE_MAGIC_V2);
    bool v3 = (magic == SAVE_MAGIC_V3);
    bool v4 = (magic == SAVE_MAGIC_V4);
    if (!v2 && !v3 && !v4) { fclose(f); return false; }

    if (fread(&count, 4, 1, f) != 1) { fclose(f); return false; }
    g_world.clear();
    g_world.resize(count);

    bool need_migration = (v2 || v3);

    for (uint32_t i = 0; i < count; i++) {
        auto& node = g_world[i];

        if (v4 || v3) {
            uint8_t nt;
            fread(&nt, 1, 1, f);
            node.node_type = (NodeType)nt;
        } else {
            node.node_type = NODE_IMAGE;
        }

        fread(&node.x, 4, 1, f); fread(&node.y, 4, 1, f); fread(&node.z, 4, 1, f);
        fread(&node.w, 4, 1, f); fread(&node.h, 4, 1, f);
        fread(&node.rot_x, 4, 1, f); fread(&node.rot_y, 4, 1, f); fread(&node.rot_z, 4, 1, f);
        fread(&node.img_w, 4, 1, f); fread(&node.img_h, 4, 1, f);

        if (v4) {
            // V4: hash stored in file, media data on disk
            fread(&node.hash, 4, 1, f);
            if (node.node_type == NODE_TEXT) {
                uint32_t tlen;
                fread(&tlen, 4, 1, f);
                node.text.resize(tlen);
                fread(node.text.data(), 1, tlen, f);
                node.data_size = 0;
            } else {
                fread(&node.data_size, 4, 1, f);
            }
        } else if (v3) {
            // V3: inline data — read and migrate to disk
            uint32_t dsz;
            fread(&dsz, 4, 1, f);
            if (node.node_type == NODE_TEXT) {
                node.text.resize(dsz);
                fread(node.text.data(), 1, dsz, f);
                node.hash = net_fnv1a((const unsigned char*)node.text.data(), node.text.size());
                node.data_size = 0;
            } else {
                std::vector<uint8_t> data(dsz);
                fread(data.data(), 1, dsz, f);
                node.hash = net_fnv1a(data.data(), data.size());
                node.data_size = dsz;
                write_media_file(node.hash, data.data(), dsz);
            }
        } else {
            // V2: all nodes are images with inline RGBA pixels
            uint32_t pix_sz = node.img_w * node.img_h * 4;
            std::vector<uint8_t> data(pix_sz);
            fread(data.data(), 1, pix_sz, f);
            node.hash = net_fnv1a(data.data(), data.size());
            node.data_size = pix_sz;
            write_media_file(node.hash, data.data(), pix_sz);
        }
    }
    fclose(f);

    const char* fmt = v4 ? "v4" : v3 ? "v3" : "v2";
    printf("Loaded %u nodes from %s (%s format)\n", count, SAVE_PATH, fmt);

    if (need_migration) {
        printf("Migrating to v4 disk-backed format...\n");
        save_world();
    }

    return true;
}

// ================================================================
// Network helpers
// ================================================================

static void send_to(Client& c, uint8_t type, const void* payload, uint32_t plen) {
    c.send_q.push(type, payload, plen);
}

static void broadcast(uint8_t type, const void* payload, uint32_t plen, socket_t exclude_fd = INVALID_SOCK) {
    for (auto& [fd, c] : g_clients)
        if (fd != exclude_fd && c.synced)
            send_to(c, type, payload, plen);
}

// S2C_WORLD_META: 45 bytes per node
static void send_world_meta(Client& c) {
    uint32_t count = (uint32_t)g_world.size();
    constexpr size_t PER = 45;
    std::vector<uint8_t> payload(4 + count * PER);
    memcpy(payload.data(), &count, 4);
    for (uint32_t i = 0; i < count; i++) {
        auto& node = g_world[i];
        uint8_t* p = payload.data() + 4 + i * PER;
        memcpy(p+0,  &node.hash, 4);
        memcpy(p+4,  &node.x, 4);    memcpy(p+8,  &node.y, 4);    memcpy(p+12, &node.z, 4);
        memcpy(p+16, &node.w, 4);    memcpy(p+20, &node.h, 4);
        memcpy(p+24, &node.rot_x, 4); memcpy(p+28, &node.rot_y, 4); memcpy(p+32, &node.rot_z, 4);
        memcpy(p+36, &node.img_w, 4); memcpy(p+40, &node.img_h, 4);
        p[44] = (uint8_t)node.node_type;
    }
    send_to(c, S2C_WORLD_META, payload.data(), (uint32_t)payload.size());
}

static void send_trail_data(Client& c) {
    size_t count = (size_t)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE;
    std::vector<uint8_t> payload(4 + count);
    uint32_t tex_size = TRAIL_TEX_SIZE;
    memcpy(payload.data(), &tex_size, 4);
    for (size_t i = 0; i < count; i++)
        payload[4 + i] = (uint8_t)(std::min(1.0f, g_trail[i]) * 255.0f);
    uint32_t plen = (uint32_t)payload.size();
    uint32_t msg_len = 1 + plen;
    c.send_q.append_raw(&msg_len, 4);
    uint8_t t = S2C_TRAIL_DATA;
    c.send_q.append_raw(&t, 1);
    c.send_q.append_raw(payload.data(), plen);
    printf("Sent trail data to player %u (%u bytes)\n", c.id, plen);
}

static void send_node_data(Client& c, const SrvNode& node) {
    const uint8_t* dptr;
    uint32_t dsz;

    if (node.node_type == NODE_TEXT) {
        dptr = (const uint8_t*)node.text.data();
        dsz = (uint32_t)node.text.size();
    } else {
        const auto* media = get_media(node.hash);
        if (!media) {
            fprintf(stderr, "Warning: cannot load media for hash=%08x, skipping send\n", node.hash);
            return;
        }
        dptr = media->data();
        dsz = (uint32_t)media->size();
    }
    uint32_t total = 9 + dsz;
    uint32_t msg_len = 1 + total;
    c.send_q.append_raw(&msg_len, 4);
    uint8_t t = S2C_NODE_DATA;
    c.send_q.append_raw(&t, 1);
    c.send_q.append_raw(&node.hash, 4);
    uint8_t nt = (uint8_t)node.node_type;
    c.send_q.append_raw(&nt, 1);
    c.send_q.append_raw(&dsz, 4);
    c.send_q.append_raw(dptr, dsz);
    c.known_hashes.insert(node.hash);
}

static void broadcast_node_add(const SrvNode& node) {
    // Pre-load media data once for all clients (avoids repeated disk reads)
    const uint8_t* dptr = nullptr;
    uint32_t dsz = 0;

    if (node.node_type == NODE_TEXT) {
        dptr = (const uint8_t*)node.text.data();
        dsz = (uint32_t)node.text.size();
    } else {
        const auto* media = get_media(node.hash);
        if (media) {
            dptr = media->data();
            dsz = (uint32_t)media->size();
        }
    }

    for (auto& [fd, c] : g_clients) {
        if (!c.synced) continue;
        uint8_t header[46];
        memcpy(header+0,  &node.hash, 4);
        memcpy(header+4,  &node.x, 4);  memcpy(header+8,  &node.y, 4);  memcpy(header+12, &node.z, 4);
        memcpy(header+16, &node.w, 4);  memcpy(header+20, &node.h, 4);
        memcpy(header+24, &node.rot_x, 4); memcpy(header+28, &node.rot_y, 4); memcpy(header+32, &node.rot_z, 4);
        memcpy(header+36, &node.img_w, 4); memcpy(header+40, &node.img_h, 4);
        header[44] = (uint8_t)node.node_type;
        header[45] = (node.node_type == NODE_TEXT || !c.known_hashes.count(node.hash)) ? 1 : 0;

        if (header[45] && dptr) {
            uint32_t total = 46 + 4 + dsz;
            uint32_t msg_len = 1 + total;
            c.send_q.append_raw(&msg_len, 4);
            uint8_t t = S2C_NODE_ADD;
            c.send_q.append_raw(&t, 1);
            c.send_q.append_raw(header, 46);
            c.send_q.append_raw(&dsz, 4);
            c.send_q.append_raw(dptr, dsz);
            c.known_hashes.insert(node.hash);
        } else {
            header[45] = 0;
            send_to(c, S2C_NODE_ADD, header, 46);
        }
    }
}

// ================================================================
// Hash-based target lookup — find world node by hash, closest to player
// ================================================================

static int find_by_hash(uint32_t hash, float px, float py, float pz) {
    float best = 1e18f;
    int idx = -1;
    for (int i = 0; i < (int)g_world.size(); i++) {
        if (g_world[i].hash != hash) continue;
        float dx = g_world[i].x - px, dy = g_world[i].y - py, dz = g_world[i].z - pz;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best) { best = d2; idx = i; }
    }
    return idx;
}

// ================================================================
// Message handling — dispatch incoming client messages
// ================================================================

static void handle_msg(Client& c, uint8_t type, const uint8_t* pl, uint32_t plen) {
    switch (type) {
    case C2S_POS: {
        if (plen < 20) break;
        memcpy(&c.x, pl, 4); memcpy(&c.y, pl+4, 4); memcpy(&c.z, pl+8, 4);
        memcpy(&c.yaw, pl+12, 4); memcpy(&c.pitch, pl+16, 4);
        break;
    }
    case C2S_TRAIL_WALK: {
        if (plen < 16) break;
        float prev_x, prev_z, curr_x, curr_z;
        memcpy(&prev_x, pl, 4); memcpy(&prev_z, pl+4, 4);
        memcpy(&curr_x, pl+8, 4); memcpy(&curr_z, pl+12, 4);
        apply_trail_walk(prev_x, prev_z, curr_x, curr_z);
        uint8_t bcast[20];
        memcpy(bcast, &c.id, 4);
        memcpy(bcast+4, pl, 16);
        broadcast(S2C_TRAIL_WALK, bcast, 20, c.fd);
        break;
    }
    case C2S_PICKUP: {
        if (plen < 16) break;
        uint32_t hash;
        float px, py, pz;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        printf("Player %u picked up node %d (type=%d, hash=%08x)\n",
               c.id, idx, g_world[idx].node_type, g_world[idx].hash);

        uint8_t rem[16];
        memcpy(rem, &g_world[idx].hash, 4);
        memcpy(rem+4, &g_world[idx].x, 4);
        memcpy(rem+8, &g_world[idx].y, 4);
        memcpy(rem+12, &g_world[idx].z, 4);
        broadcast(S2C_NODE_REMOVE, rem, 16);

        c.inventory.push_back(std::move(g_world[idx]));
        g_world.erase(g_world.begin() + idx);

        auto& inv = c.inventory.back();
        uint8_t inv_data[21];
        memcpy(inv_data, &inv.hash, 4);
        inv_data[4] = (uint8_t)inv.node_type;
        memcpy(inv_data+5, &inv.img_w, 4);
        memcpy(inv_data+9, &inv.img_h, 4);
        memcpy(inv_data+13, &inv.w, 4);
        memcpy(inv_data+17, &inv.h, 4);
        send_to(c, S2C_INV_PUSH, inv_data, 21);

        g_world_dirty = true;
        break;
    }
    case C2S_PUTDOWN: {
        if (plen < 24 || c.inventory.empty()) break;
        float px, py, pz, rx, ry, rz;
        memcpy(&px, pl, 4); memcpy(&py, pl+4, 4); memcpy(&pz, pl+8, 4);
        memcpy(&rx, pl+12, 4); memcpy(&ry, pl+16, 4); memcpy(&rz, pl+20, 4);

        auto node = std::move(c.inventory.back());
        c.inventory.pop_back();
        node.x = px; node.y = py; node.z = pz;
        node.rot_x = rx; node.rot_y = ry; node.rot_z = rz;

        printf("Player %u put down node (type=%d, hash=%08x)\n", c.id, node.node_type, node.hash);
        g_world.push_back(std::move(node));
        broadcast_node_add(g_world.back());

        send_to(c, S2C_INV_POP, nullptr, 0);
        g_world_dirty = true;
        break;
    }
    case C2S_ADD_NODE: {
        if (plen < 45) break;
        SrvNode node;
        node.node_type = (NodeType)pl[0];
        memcpy(&node.img_w, pl+1, 4); memcpy(&node.img_h, pl+5, 4);
        memcpy(&node.x, pl+9, 4);   memcpy(&node.y, pl+13, 4);  memcpy(&node.z, pl+17, 4);
        memcpy(&node.w, pl+21, 4);  memcpy(&node.h, pl+25, 4);
        memcpy(&node.rot_x, pl+29, 4); memcpy(&node.rot_y, pl+33, 4); memcpy(&node.rot_z, pl+37, 4);
        uint32_t dsz;
        memcpy(&dsz, pl+41, 4);
        if (plen < 45 + dsz) break;

        if (node.node_type == NODE_TEXT) {
            node.text.assign((const char*)(pl + 45), dsz);
            node.hash = net_fnv1a((const unsigned char*)node.text.data(), node.text.size());
            node.data_size = 0;
        } else {
            node.hash = net_fnv1a(pl + 45, dsz);
            node.data_size = dsz;
            store_media(node.hash, pl + 45, dsz);
        }

        const char* type_name = node.node_type == NODE_IMAGE ? "image" :
                                node.node_type == NODE_VIDEO ? "video" : "text";
        printf("Player %u added %s node: %ux%u, %u bytes (hash=%08x)\n",
               c.id, type_name, node.img_w, node.img_h, dsz, node.hash);
        g_world.push_back(std::move(node));
        broadcast_node_add(g_world.back());
        g_world_dirty = true;
        break;
    }
    case C2S_REQ_IMG: {
        if (plen < 4) break;
        uint32_t hash;
        memcpy(&hash, pl, 4);
        for (auto& node : g_world) {
            if (node.hash == hash) {
                send_node_data(c, node);
                printf("Sent node data hash=%08x to player %u\n", hash, c.id);
                break;
            }
        }
        break;
    }
    case C2S_HAS_CACHE: {
        if (plen < 4) break;
        uint32_t count;
        memcpy(&count, pl, 4);
        for (uint32_t i = 0; i < count && 4 + (i+1)*4 <= plen; i++) {
            uint32_t hash;
            memcpy(&hash, pl + 4 + i * 4, 4);
            c.known_hashes.insert(hash);
        }
        printf("Player %u has %u cached nodes\n", c.id, count);

        int sent = 0;
        for (auto& node : g_world) {
            if (!c.known_hashes.count(node.hash)) {
                send_node_data(c, node);
                sent++;
            }
        }
        printf("Sending %d nodes to player %u\n", sent, c.id);

        send_trail_data(c);
        c.synced = true;
        send_to(c, S2C_SYNC_DONE, nullptr, 0);
        printf("Player %u sync complete (%d world nodes, trail %dx%d)\n",
               c.id, (int)g_world.size(), TRAIL_TEX_SIZE, TRAIL_TEX_SIZE);
        print_ram_usage();
        break;
    }
    case C2S_DELETE: {
        if (plen < 16) break;
        uint32_t hash;
        float px, py, pz;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        printf("Player %u deleted node %d (type=%d, hash=%08x)\n",
               c.id, idx, g_world[idx].node_type, g_world[idx].hash);

        uint8_t rem[16];
        memcpy(rem, &g_world[idx].hash, 4);
        memcpy(rem+4, &g_world[idx].x, 4);
        memcpy(rem+8, &g_world[idx].y, 4);
        memcpy(rem+12, &g_world[idx].z, 4);
        broadcast(S2C_NODE_REMOVE, rem, 16);

        g_world.erase(g_world.begin() + idx);
        g_world_dirty = true;
        break;
    }
    case C2S_CLONE: {
        if (plen < 16) break;
        uint32_t hash;
        float px, py, pz;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        SrvNode clone;
        clone.node_type = g_world[idx].node_type;
        clone.x = g_world[idx].x + 3.0f;
        clone.y = g_world[idx].y;
        clone.z = g_world[idx].z;
        clone.w = g_world[idx].w;
        clone.h = g_world[idx].h;
        clone.rot_x = g_world[idx].rot_x;
        clone.rot_y = g_world[idx].rot_y;
        clone.rot_z = g_world[idx].rot_z;
        clone.img_w = g_world[idx].img_w;
        clone.img_h = g_world[idx].img_h;
        clone.data_size = g_world[idx].data_size;
        clone.text = g_world[idx].text;

        // Compute hash from data, then salt to make unique
        if (clone.node_type == NODE_TEXT) {
            clone.hash = net_fnv1a((const unsigned char*)clone.text.data(), clone.text.size());
        } else {
            const auto* media = get_media(g_world[idx].hash);
            clone.hash = media ? net_fnv1a(media->data(), media->size()) : g_world[idx].hash;
        }
        uint32_t salt = (uint32_t)(clone.x * 1000.0f) ^ (uint32_t)(clone.z * 1000.0f) ^ g_next_id;
        clone.hash ^= salt;

        // Copy media file under the new hash
        if (clone.node_type != NODE_TEXT) {
            const auto* media = get_media(g_world[idx].hash);
            if (media) {
                store_media(clone.hash, media->data(), (uint32_t)media->size());
            }
        }

        printf("Player %u cloned node %d -> hash=%08x\n", c.id, idx, clone.hash);
        g_world.push_back(std::move(clone));
        broadcast_node_add(g_world.back());
        g_world_dirty = true;
        break;
    }
    case C2S_RESIZE: {
        if (plen < 20) break;
        uint32_t hash;
        float px, py, pz, factor;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);
        memcpy(&factor, pl+16, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        g_world[idx].w *= factor;
        g_world[idx].h *= factor;

        printf("Player %u resized node %d (hash=%08x) by %.2f -> %.1fx%.1f\n",
               c.id, idx, g_world[idx].hash, factor, g_world[idx].w, g_world[idx].h);

        // Broadcast: remove old, add updated
        uint8_t rem[16];
        memcpy(rem, &g_world[idx].hash, 4);
        memcpy(rem+4, &g_world[idx].x, 4);
        memcpy(rem+8, &g_world[idx].y, 4);
        memcpy(rem+12, &g_world[idx].z, 4);
        broadcast(S2C_NODE_REMOVE, rem, 16);
        broadcast_node_add(g_world[idx]);

        g_world_dirty = true;
        break;
    }
    case C2S_COPY: {
        if (plen < 16) break;
        uint32_t hash;
        float px, py, pz;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        SrvNode copy;
        copy.node_type = g_world[idx].node_type;
        copy.x = g_world[idx].x; copy.y = g_world[idx].y; copy.z = g_world[idx].z;
        copy.w = g_world[idx].w; copy.h = g_world[idx].h;
        copy.rot_x = g_world[idx].rot_x; copy.rot_y = g_world[idx].rot_y; copy.rot_z = g_world[idx].rot_z;
        copy.img_w = g_world[idx].img_w; copy.img_h = g_world[idx].img_h;
        copy.data_size = g_world[idx].data_size;
        copy.text = g_world[idx].text;
        copy.hash = g_world[idx].hash;

        printf("Player %u copied node %d (hash=%08x) to inventory\n", c.id, idx, copy.hash);

        c.inventory.push_back(std::move(copy));
        auto& inv = c.inventory.back();
        uint8_t inv_data[21];
        memcpy(inv_data, &inv.hash, 4);
        inv_data[4] = (uint8_t)inv.node_type;
        memcpy(inv_data+5, &inv.img_w, 4);
        memcpy(inv_data+9, &inv.img_h, 4);
        memcpy(inv_data+13, &inv.w, 4);
        memcpy(inv_data+17, &inv.h, 4);
        send_to(c, S2C_INV_PUSH, inv_data, 21);
        break;
    }
    case C2S_MOVE_Y: {
        if (plen < 20) break;
        uint32_t hash;
        float px, py, pz, dy;
        memcpy(&hash, pl, 4);
        memcpy(&px, pl+4, 4); memcpy(&py, pl+8, 4); memcpy(&pz, pl+12, 4);
        memcpy(&dy, pl+16, 4);

        int idx = find_by_hash(hash, px, py, pz);
        if (idx < 0) break;

        // Broadcast remove at old position
        uint8_t rem[16];
        memcpy(rem, &g_world[idx].hash, 4);
        memcpy(rem+4, &g_world[idx].x, 4);
        memcpy(rem+8, &g_world[idx].y, 4);
        memcpy(rem+12, &g_world[idx].z, 4);
        broadcast(S2C_NODE_REMOVE, rem, 16);

        g_world[idx].y += dy;

        printf("Player %u moved node %d (hash=%08x) y %+.1f -> %.1f\n",
               c.id, idx, g_world[idx].hash, dy, g_world[idx].y);

        broadcast_node_add(g_world[idx]);
        g_world_dirty = true;
        break;
    }
    }
}

// ================================================================
// Signal handler
// ================================================================

static void sighandler(int) { g_running = false; }

// ================================================================
// Main server loop
// ================================================================

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
#ifdef _WIN32
    WinsockInit _wsa;
#endif
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    uint16_t port = NET_PORT;
    const char* datadir = nullptr;
    size_t cache_mb = 128;
    uint32_t world_seed = 42;
    bool test_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) test_mode = true;
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--datadir") == 0 && i + 1 < argc) datadir = argv[++i];
        else if (strcmp(argv[i], "--cache-mb") == 0 && i + 1 < argc) cache_mb = (size_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) world_seed = (uint32_t)atol(argv[++i]);
    }

    if (datadir) {
        if (plat_chdir(datadir) != 0) { perror("chdir"); return 1; }
        printf("Data directory: %s\n", datadir);
    }
    g_media_cache.max_bytes = cache_mb * 1024 * 1024;

    // Ensure media directory exists
    plat_mkdir(MEDIA_DIR);

    if (!load_world())
        printf("No world.sav found, starting with empty world\n");

    g_trail.resize((size_t)TRAIL_TEX_SIZE * TRAIL_TEX_SIZE, 0.0f);
    if (!load_trails())
        printf("No trails.sav found, starting with pristine terrain\n");

    // Create test images if world is empty and --test flag given
    if (g_world.empty() && test_mode) {
        printf("Creating test images...\n");
        for (int i = 0; i < 5; i++) {
            SrvNode node;
            node.node_type = NODE_IMAGE;
            node.x = i * 20.0f - 40.0f;
            node.y = 20.0f;
            node.z = -30.0f;
            node.w = 10.0f;
            node.h = 10.0f;
            node.rot_x = 0; node.rot_y = 0; node.rot_z = 0;
            node.img_w = 64;
            node.img_h = 64;

            uint32_t pix_sz = 64 * 64 * 4;
            std::vector<uint8_t> data(pix_sz);
            uint8_t r = (uint8_t)((i * 60 + 100) % 256);
            uint8_t g = (uint8_t)((i * 80 + 50) % 256);
            uint8_t b = (uint8_t)((i * 100 + 200) % 256);
            for (int p = 0; p < 64 * 64; p++) {
                data[p*4+0] = r; data[p*4+1] = g;
                data[p*4+2] = b; data[p*4+3] = 255;
            }
            node.hash = net_fnv1a(data.data(), data.size());
            node.data_size = pix_sz;
            store_media(node.hash, data.data(), pix_sz);
            g_world.push_back(std::move(node));
        }
        save_world();
    }

    printf("Server starting with %d nodes, trail %dx%d\n",
           (int)g_world.size(), TRAIL_TEX_SIZE, TRAIL_TEX_SIZE);
    printf("LRU media cache: %zu MB max\n", g_media_cache.max_bytes / (1024 * 1024));
    print_ram_usage();

    // ---- Set up listening socket ----
    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_valid(listen_fd)) { perror("socket"); return 1; }
    int opt = 1;
    plat_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 4) < 0) { perror("listen"); return 1; }
    net_set_nonblock(listen_fd);

    printf("Listening on port %d (poll-based selector)\n", port);

    uint64_t last_broadcast = plat_now_ms();
    uint64_t last_trail_save = plat_now_ms();
    uint64_t last_world_save = plat_now_ms();
    uint64_t last_ram_print = plat_now_ms();

    // ---- Main event loop ----
    while (g_running) {
        // Build poll descriptor set
        std::vector<pollfd> fds;
        { pollfd pf = {}; pf.fd = listen_fd; pf.events = POLLIN; fds.push_back(pf); }
        std::vector<socket_t> client_fds;
        for (auto& [fd, c] : g_clients) {
            pollfd pf = {};
            pf.fd = fd;
            pf.events = POLLIN;
            if (c.send_q.pending()) pf.events |= POLLOUT;
            fds.push_back(pf);
            client_fds.push_back(fd);
        }

        poll(fds.data(), (unsigned long)fds.size(), 20);

        // Accept new connections
        if (fds[0].revents & POLLIN) {
            sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            socket_t cfd = accept(listen_fd, (sockaddr*)&ca, &cl);
            if (sock_valid(cfd)) {
                net_set_nonblock(cfd);
                net_set_nodelay(cfd);
                uint32_t id = g_next_id++;
                Client& c = g_clients[cfd];
                c.fd = cfd; c.id = id;
                uint8_t welcome[8];
                memcpy(welcome, &id, 4);
                memcpy(welcome + 4, &world_seed, 4);
                send_to(c, S2C_WELCOME, welcome, 8);
                send_world_meta(c);
                printf("Player %u connected (fd=%lld)\n", id, (long long)cfd);
            }
        }

        // Process client I/O
        std::vector<socket_t> to_remove;
        for (size_t i = 0; i < client_fds.size(); i++) {
            int fd = client_fds[i];
            auto it = g_clients.find(fd);
            if (it == g_clients.end()) continue;
            Client& c = it->second;
            auto& pfd = fds[i + 1];

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                to_remove.push_back(fd); continue;
            }
            if (pfd.revents & POLLIN) {
                if (net_recv(fd, c.recv_buf) < 0) {
                    to_remove.push_back(fd); continue;
                }
                uint8_t type; const uint8_t* payload; uint32_t plen;
                while (net_try_read(c.recv_buf, type, payload, plen))
                    handle_msg(c, type, payload, plen);
                c.recv_buf.compact();
            }
            if (pfd.revents & POLLOUT) {
                if (!c.send_q.flush(fd)) { to_remove.push_back(fd); continue; }
            }
        }

        // Flush remaining send queues
        for (auto& [fd, c] : g_clients) {
            if (c.send_q.pending()) {
                if (!c.send_q.flush(fd) && std::find(to_remove.begin(), to_remove.end(), fd) == to_remove.end())
                    to_remove.push_back(fd);
            }
        }

        // Handle disconnections
        for (socket_t fd : to_remove) {
            auto it = g_clients.find(fd);
            if (it == g_clients.end()) continue;
            printf("Player %u disconnected\n", it->second.id);
            for (auto& node : it->second.inventory) {
                printf("  Returning inventory node hash=%08x to world\n", node.hash);
                g_world.push_back(std::move(node));
            }
            if (!it->second.inventory.empty())
                g_world_dirty = true;
            g_clients.erase(it);
            plat_close_socket(fd);
        }

        // Broadcast player positions every 50ms
        uint64_t now = plat_now_ms();
        if (now - last_broadcast >= 50 && !g_clients.empty()) {
            last_broadcast = now;
            uint32_t count = (uint32_t)g_clients.size();
            std::vector<uint8_t> payload(4 + count * 24);
            memcpy(payload.data(), &count, 4);
            int j = 0;
            for (auto& [fd, c] : g_clients) {
                uint8_t* p = payload.data() + 4 + j * 24;
                memcpy(p, &c.id, 4);
                memcpy(p+4, &c.x, 4); memcpy(p+8, &c.y, 4); memcpy(p+12, &c.z, 4);
                memcpy(p+16, &c.yaw, 4); memcpy(p+20, &c.pitch, 4);
                j++;
            }
            for (auto& [fd, c] : g_clients)
                if (c.synced)
                    send_to(c, S2C_PLAYERS, payload.data(), (uint32_t)payload.size());
        }

        // Auto-save every 30 seconds
        if (now - last_trail_save >= 30000 && g_trail_dirty) {
            last_trail_save = now;
            save_trails();
        }
        if (now - last_world_save >= 30000 && g_world_dirty) {
            last_world_save = now;
            save_world();
            g_world_dirty = false;
        }
        // Periodic RAM usage report every 30 seconds
        if (now - last_ram_print >= 30000) {
            last_ram_print = now;
            printf("[RAM] LRU cache: %.1f MB / %zu MB (%zu entries)\n",
                   g_media_cache.current_bytes / (1024.0 * 1024.0),
                   g_media_cache.max_bytes / (1024 * 1024),
                   g_media_cache.lookup.size());
            print_ram_usage();
        }
    }

    // ---- Graceful shutdown ----
    printf("\nShutting down...\n");
    for (auto& [fd, c] : g_clients) {
        for (auto& node : c.inventory) {
            printf("  Returning inventory node hash=%08x to world\n", node.hash);
            g_world.push_back(std::move(node));
        }
        plat_close_socket(fd);
    }
    g_clients.clear();
    save_world();
    if (g_trail_dirty) save_trails();
    plat_close_socket(listen_fd);
    print_ram_usage();
    printf("Server stopped. %d nodes saved. Trail %s.\n",
           (int)g_world.size(), g_trail_dirty ? "saved" : "unchanged");
    return 0;
}
