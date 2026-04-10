// ================================================================
// Exodia Multiplayer — Shared network protocol + helpers
//
// Wire format: [uint32_t len][uint8_t type][payload...]
// len = 1 + payload_size (includes the type byte)
//
// Used by both client and server for all network communication.
// ================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstdio>

static constexpr uint16_t NET_PORT = 9998;

// Node types for DataNode system
enum NodeType : uint8_t {
    NODE_IMAGE = 0,
    NODE_VIDEO = 1,
    NODE_TEXT  = 2,
};

// Message types — all communication between client and server
enum MsgType : uint8_t {
    // Client -> Server
    C2S_POS        = 1,   // float x,y,z,yaw,pitch (20 bytes)
    C2S_PICKUP     = 2,   // float x,y,z (12 bytes)
    C2S_PUTDOWN    = 3,   // float x,y,z,rot_x,rot_y,rot_z (24 bytes)
    C2S_ADD_NODE   = 4,   // node_type(1)+img_w(4)+img_h(4)+pos/dims/rot(36)+data_size(4)+data
    C2S_REQ_IMG    = 5,   // uint32 hash (4 bytes)
    C2S_HAS_CACHE  = 6,   // uint32 count + uint32[] hashes
    C2S_TRAIL_WALK = 7,   // float prev_x, prev_z, curr_x, curr_z (16 bytes)
    C2S_DELETE     = 8,   // float x,y,z (12 bytes)
    C2S_CLONE      = 9,   // float x,y,z (12 bytes)
    C2S_RESIZE     = 21,  // float x,y,z (12 bytes) + float factor (4 bytes)
    C2S_COPY       = 22,  // float x,y,z (12 bytes) — copy nearest to inventory (original stays)
    C2S_MOVE_Y     = 23,  // float x,y,z (12 bytes) + float dy (4 bytes) — move nearest node vertically

    // Server -> Client
    S2C_WELCOME    = 10,  // uint32 player_id
    S2C_WORLD_META = 11,  // uint32 count + per-node metadata (45 bytes each)
    S2C_NODE_DATA  = 12,  // uint32 hash + uint8 node_type + uint32 data_size + data
    S2C_PLAYERS    = 13,  // uint32 count + per-player (id,x,y,z,yaw,pitch = 24 bytes)
    S2C_NODE_ADD   = 14,  // node metadata + has_data(1) + optional data
    S2C_NODE_REMOVE= 15,  // uint32 hash + float x,y,z (16 bytes)
    S2C_INV_PUSH   = 16,  // uint32 hash + node_type(1) + img_w(4) + img_h(4) + float w,h (21 bytes)
    S2C_INV_POP    = 17,  // no payload
    S2C_SYNC_DONE  = 18,  // no payload
    S2C_TRAIL_DATA = 19,  // uint32 tex_size + uint8[] quantized trail map
    S2C_TRAIL_WALK = 20,  // uint32 player_id + float prev_x, prev_z, curr_x, curr_z (20 bytes)
};

// FNV1a hash — used for data deduplication
inline uint32_t net_fnv1a(const unsigned char* data, size_t len) {
    uint32_t h = 0x811c9dc5;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x01000193; }
    return h;
}

// Receive buffer — accumulates incoming bytes, extracts complete messages
struct NetBuf {
    std::vector<uint8_t> data;
    size_t rpos = 0;

    void append(const void* p, size_t n) {
        auto* b = (const uint8_t*)p;
        data.insert(data.end(), b, b + n);
    }
    size_t avail() const { return data.size() - rpos; }
    const uint8_t* ptr() const { return data.data() + rpos; }
    void advance(size_t n) { rpos += n; }
    void compact() {
        if (rpos > 0) { data.erase(data.begin(), data.begin() + rpos); rpos = 0; }
    }
};

// Send queue — buffers outgoing messages with non-blocking flush
struct SendQ {
    std::vector<uint8_t> buf;
    size_t spos = 0;

    void push(uint8_t type, const void* payload, uint32_t plen) {
        uint32_t len = 1 + plen;
        append_raw(&len, 4);
        append_raw(&type, 1);
        if (plen > 0) append_raw(payload, plen);
    }

    void append_raw(const void* d, size_t n) {
        auto* p = (const uint8_t*)d;
        buf.insert(buf.end(), p, p + n);
    }

    bool flush(int fd) {
        while (spos < buf.size()) {
            ssize_t n = ::send(fd, buf.data() + spos, buf.size() - spos, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                return false;
            }
            if (n == 0) return false;
            spos += n;
        }
        if (spos == buf.size()) { buf.clear(); spos = 0; }
        else if (spos > 65536) { buf.erase(buf.begin(), buf.begin() + spos); spos = 0; }
        return true;
    }

    bool pending() const { return spos < buf.size(); }
};

// Extract one complete message from buffer. Returns false if incomplete.
inline bool net_try_read(NetBuf& buf, uint8_t& type, const uint8_t*& payload, uint32_t& plen) {
    if (buf.avail() < 5) return false;
    uint32_t len;
    memcpy(&len, buf.ptr(), 4);
    if (len < 1 || len > 200000000) return false; // 200MB sanity limit
    if (buf.avail() < 4 + len) return false;
    type = buf.ptr()[4];
    payload = buf.ptr() + 5;
    plen = len - 1;
    buf.advance(4 + len);
    return true;
}

// Non-blocking recv into buffer. Returns: >0=bytes, 0=would-block, -1=closed/error
inline int net_recv(int fd, NetBuf& buf) {
    uint8_t tmp[65536];
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n > 0) { buf.append(tmp, n); return (int)n; }
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

inline void net_set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

inline void net_set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}
