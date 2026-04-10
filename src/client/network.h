#pragma once

#include <cstdint>

// Connect to the multiplayer server
bool net_connect(const char* host, uint16_t port);

// Send a raw message to the server
void net_send(uint8_t type, const void* payload, uint32_t plen);

// Send player position update to server
void net_send_pos();

// Poll for incoming messages and process them
void net_poll();

// Handle a single server message
void handle_server_msg(uint8_t type, const uint8_t* payload, uint32_t plen);

// Rebuild the drawable quad list from world state
void rebuild_quads();

// Upload world node images as textures
void upload_world_images();
