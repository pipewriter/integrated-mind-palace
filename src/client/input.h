#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// Process keyboard/mouse input for the current frame
void process_input(float dt);

// Check clipboard for new images
void check_clipboard();

// Check dump-folder for new files to import to inventory
void check_media_folder();

// Menu functions
void menu_adjust(int dir);
void build_menu_text();

// GLFW character input callback
void char_callback(GLFWwindow* window, unsigned int codepoint);

// Typing mode functions
void start_typing();
void finalize_typing();
void cancel_typing();

// Object interaction
void pickup_nearest();
void putdown_image();

// Mouse button callback
void mouse_button_cb(GLFWwindow* window, int button, int action, int mods);

// File drop callback
void drop_cb(GLFWwindow* window, int count, const char** paths);

// Key callback (for rebinding)
void key_cb(GLFWwindow* window, int key, int scancode, int action, int mods);
