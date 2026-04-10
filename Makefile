# ================================================================
# Exodia MP — Build system
#
# Targets:
#   make          Build both client and server (release)
#   make debug    Build with debug flags + Vulkan validation
#   make client   Build client only
#   make server   Build server only
#   make clean    Remove build artifacts
#   make run      Build and run server + 2 clients
# ================================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -Wno-unused-parameter
RELEASE   := -O2 -DNDEBUG
DEBUG     := -g -O0 -fsanitize=address

# Include paths
INCLUDES  := -Isrc -Ivendor

# Client libraries
CLIENT_LIBS := -lglfw -lvulkan -lavformat -lavcodec -lavutil -lswscale -lswresample -lSDL2 -lz

# Source files
CLIENT_SRCS := \
	src/client/main.cpp \
	src/client/app.cpp \
	src/client/variance.cpp \
	src/client/terrain.cpp \
	src/client/structures.cpp \
	src/client/collision.cpp \
	src/client/trail.cpp \
	src/client/sky.cpp \
	src/client/vulkan_setup.cpp \
	src/client/textures.cpp \
	src/client/video.cpp \
	src/client/text.cpp \
	src/client/input.cpp \
	src/client/network.cpp \
	src/client/renderer.cpp

SERVER_SRCS := src/server/main.cpp
CLI_SRCS    := src/cli/objectcli.cpp

# Object files (in build/ directory)
BUILD_DIR := build
CLIENT_OBJS := $(CLIENT_SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
SERVER_OBJS := $(SERVER_SRCS:src/%.cpp=$(BUILD_DIR)/%.o)
CLI_OBJS    := $(CLI_SRCS:src/%.cpp=$(BUILD_DIR)/%.o)

# CLI libraries (subset of client — no Vulkan/GLFW/SDL needed)
CLI_LIBS    := -lavformat -lavcodec -lavutil

# ================================================================
# Default target: release build
# ================================================================

.PHONY: all debug client server objectcli clean run

all: client server objectcli

client: CXXFLAGS += $(RELEASE)
client: $(BUILD_DIR)/exodia_client
	@cp $(BUILD_DIR)/exodia_client ./client

server: CXXFLAGS += $(RELEASE)
server: $(BUILD_DIR)/exodia_server
	@cp $(BUILD_DIR)/exodia_server ./server

objectcli: CXXFLAGS += $(RELEASE)
objectcli: $(BUILD_DIR)/exodia_objectcli
	@cp $(BUILD_DIR)/exodia_objectcli ./objectcli

debug: CXXFLAGS += $(DEBUG)
debug: $(BUILD_DIR)/exodia_client $(BUILD_DIR)/exodia_server
	@cp $(BUILD_DIR)/exodia_client ./client_dbg
	@cp $(BUILD_DIR)/exodia_server ./server_dbg

# ================================================================
# Linking
# ================================================================

$(BUILD_DIR)/exodia_client: $(CLIENT_OBJS)
	@echo "LINK $@"
	@$(CXX) $(CXXFLAGS) $^ -o $@ $(CLIENT_LIBS)

$(BUILD_DIR)/exodia_server: $(SERVER_OBJS)
	@echo "LINK $@"
	@$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/exodia_objectcli: $(CLI_OBJS)
	@echo "LINK $@"
	@$(CXX) $(CXXFLAGS) $^ -o $@ $(CLI_LIBS)

# ================================================================
# Compilation (pattern rule for all .cpp -> .o)
# ================================================================

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ================================================================
# Run (server + 2 clients)
# ================================================================

run: all
	@mkdir -p image_cache dump-folder
	@echo "Starting server..."
	@./server --test &
	@sleep 1
	@echo "Starting clients..."
	@./client &
	@./client &
	@echo "Press Enter to stop..."
	@read _
	@pkill -f ./server 2>/dev/null || true
	@pkill -f ./client 2>/dev/null || true

# ================================================================
# Clean
# ================================================================

clean:
	rm -rf $(BUILD_DIR)
	rm -f client server objectcli client_dbg server_dbg

# ================================================================
# Dependency tracking (auto-generated)
# ================================================================

-include $(CLIENT_OBJS:.o=.d)
-include $(SERVER_OBJS:.o=.d)

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@
