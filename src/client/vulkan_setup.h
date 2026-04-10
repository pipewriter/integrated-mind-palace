// ================================================================
// Vulkan initialization and setup — declarations
//
// Utility functions, Vulkan instance/device/swapchain creation,
// pipeline setup, and cleanup.
// ================================================================
#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vector>
#include <cstdint>
#include <random>

// Forward declarations for types used in signatures
struct Texture;
struct GlyphText;
struct SkyPreset;
struct SkyUBOData;

// ----------------------------------------------------------------
// Vulkan utility functions
// ----------------------------------------------------------------

void vk_check(VkResult r, const char* msg);

// Debug callback (Vulkan validation layer)
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*);

// Shader compilation: GLSL source -> SPIR-V words
std::vector<uint32_t> compile_glsl(const char* src, const char* stage, const char* label);

// Memory type selection
uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props);

// Buffer creation and upload
void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                   VkMemoryPropertyFlags props,
                   VkBuffer& buffer, VkDeviceMemory& memory);
void upload_buffer(const void* data, VkDeviceSize size,
                   VkBufferUsageFlags usage,
                   VkBuffer& buf, VkDeviceMemory& mem);

// One-shot command buffer helpers
VkCommandBuffer begin_one_shot();
void end_one_shot(VkCommandBuffer cmd);

// Buffer/image copy helpers
void copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);
void transition_image_layout(VkImage image, VkImageLayout old_layout,
                             VkImageLayout new_layout);
void copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h);

// ----------------------------------------------------------------
// GLFW callbacks
// ----------------------------------------------------------------

void fb_resize_cb(GLFWwindow*, int, int);
void mouse_cb(GLFWwindow*, double mx, double my);

// ----------------------------------------------------------------
// Color utility
// ----------------------------------------------------------------

void hsl_to_rgb(float h, float s, float l, float out[3]);

// ----------------------------------------------------------------
// Shader module helper
// ----------------------------------------------------------------

VkShaderModule create_shader_module(const std::vector<uint32_t>& code);

// ----------------------------------------------------------------
// Depth format helper
// ----------------------------------------------------------------

VkFormat find_depth_format();

// ----------------------------------------------------------------
// Vulkan initialization
// ----------------------------------------------------------------

void create_instance();
void pick_physical_device();
void create_device();
void create_swapchain();
void create_depth_buffer();
void destroy_depth_buffer();
void create_render_pass();
void create_descriptor_resources();
void create_sky_descriptors();
void create_pipelines();
void create_framebuffers();
void create_command_objects();
void create_sync();
void create_quad_geometry();

// ----------------------------------------------------------------
// Swapchain management
// ----------------------------------------------------------------

void cleanup_swapchain();
void recreate_swapchain();

// ----------------------------------------------------------------
// Full cleanup
// ----------------------------------------------------------------

void cleanup();
