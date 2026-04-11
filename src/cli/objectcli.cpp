// objectcli — Import/delete objects in a running Exodia MP server
//
// Usage:
//   ./objectcli x y z file.mp4          Place a video at (x,y,z)
//   ./objectcli x y z image.png         Place an image at (x,y,z)
//   ./objectcli x y z "hello world"     Place text at (x,y,z)
//   ./objectcli --delete-region x1 z1 x2 z2   Delete all nodes in region
//
// Optional flags:
//   --rot <rx> <ry> <rz>   Rotation in degrees (default: 0 0 0)
//   --size <w> <h>         World-space size (default: 10 x auto-aspect)
//   --host <ip>            Server IP (default: 127.0.0.1)

#include "../shared/net.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#define STB_IMAGE_IMPLEMENTATION
#include "../../vendor/stb_image.h"

// Cross-platform sleep in milliseconds
static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Send a complete message (blocking)
static bool send_msg(socket_t fd, uint8_t type, const void* payload, uint32_t plen) {
    uint32_t len = 1 + plen;
    uint8_t header[5];
    memcpy(header, &len, 4);
    header[4] = type;

    auto send_all = [&](const void* data, size_t n) -> bool {
        const char* p = (const char*)data;
        while (n > 0) {
            ssize_t sent = ::send(fd, p, (int)n, MSG_NOSIGNAL);
            if (sent <= 0) return false;
            p += sent; n -= sent;
        }
        return true;
    };

    if (!send_all(header, 5)) return false;
    if (plen > 0 && !send_all(payload, plen)) return false;
    return true;
}

// Read exactly n bytes (blocking)
static bool recv_all(socket_t fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t got = ::recv(fd, p, (int)n, 0);
        if (got <= 0) return false;
        p += got; n -= got;
    }
    return true;
}

// Wait for welcome message, return player ID
static bool wait_welcome(socket_t fd, uint32_t& id) {
    uint8_t buf[9]; // 4 len + 1 type + 4 id
    if (!recv_all(fd, buf, 9)) return false;
    if (buf[4] != S2C_WELCOME) return false;
    memcpy(&id, buf + 5, 4);
    return true;
}

static bool is_video_ext(const std::string& ext) {
    return ext == ".mp4" || ext == ".mkv" || ext == ".webm" || ext == ".avi" || ext == ".mov";
}

static bool is_image_ext(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp";
}

static std::string get_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (auto& ch : ext) ch = tolower(ch);
    return ext;
}

static bool probe_video(const char* path, uint32_t& w, uint32_t& h) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            w = fmt->streams[i]->codecpar->width;
            h = fmt->streams[i]->codecpar->height;
            avformat_close_input(&fmt);
            return true;
        }
    }
    avformat_close_input(&fmt);
    return false;
}

// Drain any pending server messages (non-blocking) so connection doesn't stall
static void drain_responses(socket_t fd) {
    // Temporarily set non-blocking
    plat_set_nonblock(fd);
    char tmp[65536];
    while (true) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
    }
    // Set back to blocking
#ifdef _WIN32
    u_long mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
}

// Connect to server and return socket, or INVALID_SOCK on failure
static socket_t connect_to_server(const char* host) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!sock_valid(fd)) { perror("socket"); return INVALID_SOCK; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NET_PORT);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid host: %s\n", host); plat_close_socket(fd); return INVALID_SOCK;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); plat_close_socket(fd); return INVALID_SOCK;
    }
    net_set_nodelay(fd);
    return fd;
}

// Delete all nodes within an axis-aligned region
static int do_delete_region(float x1, float z1, float x2, float z2, const char* host) {
    if (x1 > x2) std::swap(x1, x2);
    if (z1 > z2) std::swap(z1, z2);

    socket_t fd = connect_to_server(host);
    if (!sock_valid(fd)) return 1;

    uint32_t player_id;
    if (!wait_welcome(fd, player_id)) {
        fprintf(stderr, "Failed to receive welcome\n"); plat_close_socket(fd); return 1;
    }

    // Drain world meta + trail data the server sends on connect
    sleep_ms(200);
    drain_responses(fd);

    printf("Connected as player %u\n", player_id);
    printf("Deleting all nodes in region (%.0f,%.0f) to (%.0f,%.0f)\n", x1, z1, x2, z2);

    // Server delete finds nearest node within 20 units.
    // Sweep the region with step < 20 to cover all positions.
    // Use multiple Y levels to catch nodes at different heights.
    float step = 15.0f;
    float y_levels[] = { 30.0f, 55.0f, 80.0f, 120.0f };
    int deleted = 0;

    // Multiple passes — each pass deletes one node per grid point,
    // so we repeat until a pass finds nothing
    for (int pass = 0; pass < 20; pass++) {
        int pass_count = 0;
        for (float y : y_levels) {
            for (float x = x1; x <= x2; x += step) {
                for (float z = z1; z <= z2; z += step) {
                    float data[3] = { x, y, z };
                    if (!send_msg(fd, C2S_DELETE, data, 12)) {
                        fprintf(stderr, "Send failed\n"); plat_close_socket(fd); return 1;
                    }
                    pass_count++;
                    deleted++;
                }
            }
        }
        // Let server process
        sleep_ms(50);
        drain_responses(fd);

        // After first pass we know the grid, if pass_count is small just do a few
        if (pass > 0) {
            printf("  Pass %d: sent %d delete requests\n", pass + 1, pass_count);
        } else {
            printf("  Pass 1: sent %d delete requests across region\n", pass_count);
        }
    }

    printf("Done — sent %d total delete requests\n", deleted);
    sleep_ms(100);
    plat_close_socket(fd);
    return 0;
}

static void usage() {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  objectcli <x> <y> <z> <file-or-text> [options]   Add a node\n");
    fprintf(stderr, "  objectcli --delete-region <x1> <z1> <x2> <z2>    Delete all nodes in region\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --rot <rx> <ry> <rz>   Rotation degrees (default: 0 0 0)\n");
    fprintf(stderr, "  --size <w> <h>          World size (default: 10 x auto)\n");
    fprintf(stderr, "  --host <ip>             Server IP (default: 127.0.0.1)\n");
    exit(1);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    WinsockInit _wsa;
#endif
    if (argc < 2) usage();

    // Check for --delete-region mode
    if (strcmp(argv[1], "--delete-region") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: objectcli --delete-region <x1> <z1> <x2> <z2> [--host <ip>]\n");
            return 1;
        }
        float x1 = atof(argv[2]), z1 = atof(argv[3]);
        float x2 = atof(argv[4]), z2 = atof(argv[5]);
        const char* host = "127.0.0.1";
        for (int i = 6; i < argc; i++) {
            if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        }
        return do_delete_region(x1, z1, x2, z2, host);
    }

    if (argc < 5) usage();

    float px = atof(argv[1]);
    float py = atof(argv[2]);
    float pz = atof(argv[3]);
    std::string arg4 = argv[4];

    float rot_x = 0, rot_y = 0, rot_z = 0;
    float size_w = 10.0f, size_h = -1.0f; // -1 = auto aspect
    const char* host = "127.0.0.1";

    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--rot") == 0 && i + 3 < argc) {
            rot_x = atof(argv[++i]); rot_y = atof(argv[++i]); rot_z = atof(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            size_w = atof(argv[++i]); size_h = atof(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]); usage();
        }
    }

    // Determine what we're sending
    NodeType node_type;
    uint32_t img_w = 0, img_h = 0;
    std::vector<uint8_t> data;

    std::string ext = get_ext(arg4);
    bool is_file = is_video_ext(ext) || is_image_ext(ext);

    if (is_video_ext(ext)) {
        node_type = NODE_VIDEO;
        if (!probe_video(arg4.c_str(), img_w, img_h)) {
            fprintf(stderr, "Failed to probe video: %s\n", arg4.c_str()); return 1;
        }
        FILE* f = fopen(arg4.c_str(), "rb");
        if (!f) { fprintf(stderr, "Cannot open: %s\n", arg4.c_str()); return 1; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize(fsize);
        fread(data.data(), 1, fsize, f);
        fclose(f);
        if (size_h < 0) size_h = size_w * (float)img_h / (float)img_w;
        printf("Video: %s (%ux%u, %zu bytes)\n", arg4.c_str(), img_w, img_h, data.size());

    } else if (is_image_ext(ext)) {
        node_type = NODE_IMAGE;
        int iw, ih, channels;
        unsigned char* pixels = stbi_load(arg4.c_str(), &iw, &ih, &channels, 4);
        if (!pixels) { fprintf(stderr, "Cannot load image: %s\n", arg4.c_str()); return 1; }
        img_w = iw; img_h = ih;
        data.assign(pixels, pixels + (size_t)iw * ih * 4);
        stbi_image_free(pixels);
        if (size_h < 0) size_h = size_w * (float)img_h / (float)img_w;
        printf("Image: %s (%ux%u, %zu bytes RGBA)\n", arg4.c_str(), img_w, img_h, data.size());

    } else if (is_file) {
        fprintf(stderr, "Unsupported file type: %s\n", ext.c_str()); return 1;

    } else {
        // Text node
        node_type = NODE_TEXT;
        img_w = 0; img_h = 0;
        data.assign(arg4.begin(), arg4.end());
        if (size_h < 0) size_h = size_w;
        printf("Text: \"%s\" (%zu bytes)\n", arg4.c_str(), data.size());
    }

    // Connect to server
    socket_t fd = connect_to_server(host);
    if (!sock_valid(fd)) return 1;

    // Wait for welcome
    uint32_t player_id;
    if (!wait_welcome(fd, player_id)) {
        fprintf(stderr, "Failed to receive welcome\n"); plat_close_socket(fd); return 1;
    }
    printf("Connected as player %u\n", player_id);

    // Build C2S_ADD_NODE payload:
    // node_type(1) + img_w(4) + img_h(4) + x(4) + y(4) + z(4) + w(4) + h(4) +
    // rot_x(4) + rot_y(4) + rot_z(4) + data_size(4) + data
    uint32_t dsz = (uint32_t)data.size();
    std::vector<uint8_t> msg(45 + dsz);
    msg[0] = (uint8_t)node_type;
    memcpy(msg.data()+1, &img_w, 4);
    memcpy(msg.data()+5, &img_h, 4);
    memcpy(msg.data()+9, &px, 4);
    memcpy(msg.data()+13, &py, 4);
    memcpy(msg.data()+17, &pz, 4);
    memcpy(msg.data()+21, &size_w, 4);
    memcpy(msg.data()+25, &size_h, 4);
    memcpy(msg.data()+29, &rot_x, 4);
    memcpy(msg.data()+33, &rot_y, 4);
    memcpy(msg.data()+37, &rot_z, 4);
    memcpy(msg.data()+41, &dsz, 4);
    if (dsz > 0) memcpy(msg.data()+45, data.data(), dsz);

    if (!send_msg(fd, C2S_ADD_NODE, msg.data(), (uint32_t)msg.size())) {
        fprintf(stderr, "Failed to send node\n"); plat_close_socket(fd); return 1;
    }

    printf("Sent %s node at (%.1f, %.1f, %.1f) size %.1fx%.1f\n",
           node_type == NODE_VIDEO ? "video" : node_type == NODE_IMAGE ? "image" : "text",
           px, py, pz, size_w, size_h);

    // Brief pause to let server process before we disconnect
    sleep_ms(100);
    plat_close_socket(fd);
    return 0;
}
