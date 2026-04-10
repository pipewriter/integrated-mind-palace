# Exodia MP — Multiplayer 3D World

## What This Is

A networked multiplayer 3D virtual world where players explore procedurally generated terrain with buildings and plants, place images/videos/text in 3D space, and leave persistent footprint trails on terrain and structures. Built with Vulkan, C++17, and custom TCP networking.

## Build

```bash
make            # Build client + server (release)
make debug      # Build with AddressSanitizer + Vulkan validation
make run        # Build and launch server + 2 clients
make clean      # Remove build artifacts
```

**Dependencies**: g++, glfw3, vulkan-sdk, ffmpeg (libavformat/libavcodec/libavutil/libswscale/libswresample), SDL2, glslc (for shader compilation at runtime)

## Architecture

### Client-Server Model
- **Server** (`src/server/main.cpp`): Authoritative TCP server using `poll()`. Single source of truth for world state. Manages DataNodes, trail map, client connections, inventory, and persistence.
- **Client** (`src/client/`): Vulkan rendering engine with procedural world generation, real-time networking, and user interaction. Connects to server on port 9998.

### Wire Protocol
Binary TCP on port 9998. Format: `[uint32_t len][uint8_t type][payload...]`. Defined in `src/shared/net.h`. Message types prefixed C2S_ (client-to-server) and S2C_ (server-to-client).

### Client Module Map

| Module | Files | Purpose |
|--------|-------|---------|
| **App State** | `app.h`, `app.cpp` | App struct (all Vulkan resources), global state declarations/definitions |
| **Types** | `types.h` | Vertex, Quad, Texture, Camera, DataNode, ColTri, etc. |
| **Shaders** | `shaders.h` | GLSL source strings for all 4 pipelines + sky data structs |
| **Math** | `math.h` | Mat4 operations, CPU Perlin noise (all inline) |
| **Vec3** | `vec3.h` | 3D vector math for L-system plants (inline) |
| **Variance** | `variance.h/cpp` | Simplex noise, FBM, domain warping, channel-based terrain sampling |
| **Terrain** | `terrain.h/cpp` | Height-field generation, terrain texture, mesh construction |
| **Structures** | `structures.h/cpp` | Procedural buildings (temple/house/tower/pyramid) + L-system plants |
| **Collision** | `collision.h/cpp` | Walk-mode physics, spatial grid, wall-sliding |
| **Trail** | `trail.h/cpp` | Footprint depression, terrain deformation, structure wear |
| **Sky** | `sky.h/cpp` | Procedural sky presets, UBO data, smooth transitions |
| **Vulkan Setup** | `vulkan_setup.h/cpp` | Instance/device/swapchain/pipeline creation, buffer helpers, cleanup |
| **Textures** | `textures.h/cpp` | Texture loading, GPU upload, hash-based caching, stb_image |
| **Video** | `video.h/cpp` | FFmpeg video decoding, SDL2 audio, video player lifecycle |
| **Text** | `text.h/cpp` | 8x8 bitmap font atlas, glyph text rendering |
| **Input** | `input.h/cpp` | Keyboard/mouse handling, menu, typing mode, clipboard monitoring |
| **Network** | `network.h/cpp` | Client networking, server message handling, world sync |
| **Renderer** | `renderer.h/cpp` | draw_frame (main render loop), model matrix construction |
| **Main** | `main.cpp` | Entry point, initialization sequence, main loop |

### Shared Code
- `src/shared/net.h` — Network protocol (NodeType, MsgType, NetBuf, SendQ, FNV1a hash)
- `src/shared/constants.h` — Values that must match between client and server (GRID_SIZE, TRAIL_TEX_SIZE, etc.)

### Vendor
- `vendor/stb_image.h` — PNG/JPG image loading (implementation in textures.cpp)
- `vendor/font8x8_data.inc` — 8x8 bitmap font glyph data

## Key Design Decisions

### Global State
All mutable global state is declared `extern` in `app.h` and defined in `app.cpp`. The `App` struct holds all Vulkan resources. Other globals are prefixed `g_` (e.g., `g_world`, `g_trail`, `g_terrain_verts`).

### Rendering Pipeline (4 pipelines in one render pass)
1. **Sky** — Fullscreen triangle, procedural sky via UBO (no depth write)
2. **Terrain** — 512x512 vertex grid, height-mapped, textured via procedural noise
3. **Structures** — Vertex-colored buildings + L-system plants with wear darkening
4. **Quads** — Per-texture descriptor set, world-space images/videos
5. **Text** — Font atlas, per-character quads (same pipeline as quads)

### World Generation (deterministic)
All procedural content uses fixed seeds — every client generates identical terrain, structures, and plants without server involvement. The `VarianceSampler` (8 noise channels) drives regional variation across the map.

### Trail System
Players leave footprints that depress terrain and darken structures. The trail is a 2048x2048 float map, synced from server on connect, updated in real-time via C2S_TRAIL_WALK messages. Dirty-region tracking minimizes GPU uploads.

### Persistence
- **world.sav** — All DataNodes (v3 format: type + position + rotation + data)
- **trails.sav** — Quantized trail map (float→uint8)
- **image_cache/** — Client-side hash-based texture cache
- Both use atomic writes (write to .tmp, rename)

## Controls

| Key | Action |
|-----|--------|
| WASD | Move |
| Mouse | Look |
| Space/Ctrl | Up/Down (fly) or Jump (walk) |
| Shift | Sprint |
| Tab | Toggle fly/walk mode |
| E | Pick up nearest object |
| Q | Put down held object |
| T | Type text node |
| C | Clone nearest object |
| Delete | Delete nearest object |
| F | Toggle fog |
| Backtick | Open menu |
| Escape | Quit |

## Runtime Files
- `image_cache/` — Cached textures (auto-created)
- `dump-folder/` — Drop images/videos/text here to import to inventory
- `world.sav` — Server world state (auto-saved every 30s)
- `trails.sav` — Server trail data (auto-saved every 30s)
