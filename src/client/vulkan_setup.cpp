// ================================================================
// Vulkan initialization and setup — implementations
//
// Extracted from client.cpp: utility functions, Vulkan init,
// pipeline creation, swapchain management, and cleanup.
// ================================================================

#include "vulkan_setup.h"
#include "app.h"
#include "shaders.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

// Forward declarations for cleanup dependencies
void destroy_texture(Texture& t);
void destroy_glyph_text(GlyphText* gt);
void cleanup_video(App::VideoPlayer& vp);

// ----------------------------------------------------------------
// Utility
// ----------------------------------------------------------------

void vk_check(VkResult r, const char* msg) {
    if (r != VK_SUCCESS) { fprintf(stderr, "Vulkan error (%d): %s\n", r, msg); exit(1); }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[validation] %s\n", data->pMessage);
    return VK_FALSE;
}

// ----------------------------------------------------------------
// Shader compilation
// ----------------------------------------------------------------

std::vector<uint32_t> compile_glsl(const char* src, const char* stage, const char* label) {
    // Try loading pre-compiled SPIR-V from shaders/ directory first
    std::string spv_path = std::string("shaders/") + label + ".spv";
    {
        std::ifstream f(spv_path, std::ios::binary|std::ios::ate);
        if (f.good()) {
            size_t sz = f.tellg(); f.seekg(0);
            if (sz >= 4) {
                std::vector<uint32_t> code(sz / 4);
                f.read(reinterpret_cast<char*>(code.data()), sz);
                printf("Loaded pre-compiled shader: %s (%zu bytes)\n", spv_path.c_str(), sz);
                return code;
            }
        }
    }
    // Fall back to runtime compilation via glslc
    std::string tmpdir = plat_tmpdir();
    std::string gp = tmpdir + "/_vk_" + label + ".glsl";
    std::string sp = tmpdir + "/_vk_" + label + ".spv";
    { std::ofstream f(gp); f << src; }
    std::string cmd = std::string("glslc -fshader-stage=") + stage + " -O \"" + gp + "\" -o \"" + sp + "\" 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    char buf[256]; std::string err;
    while (fgets(buf, sizeof(buf), p)) err += buf;
    if (pclose(p) != 0) { fprintf(stderr, "Shader error (%s):\n%s\n", label, err.c_str()); exit(1); }
    std::ifstream f(sp, std::ios::binary|std::ios::ate);
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint32_t> code(sz / 4);
    f.read(reinterpret_cast<char*>(code.data()), sz);
    remove(gp.c_str()); remove(sp.c_str());
    return code;
}

// ----------------------------------------------------------------
// GLFW callbacks
// ----------------------------------------------------------------

void fb_resize_cb(GLFWwindow*, int, int) { app.fb_resized = true; }
void mouse_cb(GLFWwindow*, double mx, double my) {
    if (app.first_mouse) { app.last_mx=mx; app.last_my=my; app.first_mouse=false; return; }
    app.cam.yaw   += float(mx - app.last_mx) * app.cam.sensitivity;
    app.cam.pitch += float(app.last_my - my) * app.cam.sensitivity;
    app.last_mx=mx; app.last_my=my;
    if (app.cam.pitch> 89) app.cam.pitch= 89;
    if (app.cam.pitch<-89) app.cam.pitch=-89;
}

// ----------------------------------------------------------------
// Vulkan memory helpers
// ----------------------------------------------------------------

uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(app.phys_device, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
        if ((filter & (1<<i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    fprintf(stderr, "No suitable memory type\n"); exit(1);
}

void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags props,
                           VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(app.device, &ci, nullptr, &buffer), "create buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(app.device, buffer, &req);
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    vk_check(vkAllocateMemory(app.device, &ai, nullptr, &memory), "alloc buffer mem");
    vkBindBufferMemory(app.device, buffer, memory, 0);
}

// Run a one-shot command (used for buffer copies and image transitions)
VkCommandBuffer begin_one_shot() {
    VkCommandBufferAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = app.cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(app.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void end_one_shot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(app.gfx_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(app.gfx_queue);
    vkFreeCommandBuffers(app.device, app.cmd_pool, 1, &cmd);
}

void copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    VkCommandBuffer cmd = begin_one_shot();
    VkBufferCopy region{}; region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
    end_one_shot(cmd);
}

// Upload data to a device-local buffer via staging
void upload_buffer(const void* data, VkDeviceSize size,
                          VkBufferUsageFlags usage,
                          VkBuffer& buf, VkDeviceMemory& mem) {
    VkBuffer staging; VkDeviceMemory staging_mem;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, staging_mem);
    void* mapped;
    vkMapMemory(app.device, staging_mem, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(app.device, staging_mem);
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf, mem);
    copy_buffer(staging, buf, size);
    vkDestroyBuffer(app.device, staging, nullptr);
    vkFreeMemory(app.device, staging_mem, nullptr);
}

// ----------------------------------------------------------------
// Image helpers (new for texture support)
// ----------------------------------------------------------------

// Transition an image between layouts using a pipeline barrier.
// This tells the GPU "this image is changing from one usage to another."
void transition_image_layout(VkImage image, VkImageLayout old_layout,
                                     VkImageLayout new_layout) {
    VkCommandBuffer cmd = begin_one_shot();

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = old_layout;
    barrier.newLayout           = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags src_stage, dst_stage;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Preparing image to receive pixel data from a buffer copy
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // Image is filled -- now make it readable by shaders
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        fprintf(stderr, "Unsupported layout transition\n"); exit(1);
    }

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
    end_one_shot(cmd);
}

// Copy pixel data from a staging buffer into a VkImage
void copy_buffer_to_image(VkBuffer buffer, VkImage image, uint32_t w, uint32_t h) {
    VkCommandBuffer cmd = begin_one_shot();
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { w, h, 1 };
    vkCmdCopyBufferToImage(cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_one_shot(cmd);
}

// ----------------------------------------------------------------
// Vulkan initialization
// ----------------------------------------------------------------

void create_instance() {
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "Perlin Terrain"; ai.apiVersion = VK_API_VERSION_1_0;
    uint32_t gc = 0;
    const char** ge = glfwGetRequiredInstanceExtensions(&gc);
    std::vector<const char*> exts(ge, ge + gc);
    if (VALIDATION) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    VkInstanceCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    if (VALIDATION) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &VALIDATION_LAYER; }
    vk_check(vkCreateInstance(&ci, nullptr, &app.instance), "create instance");
    if (VALIDATION) {
        VkDebugUtilsMessengerCreateInfoEXT d{};
        d.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        d.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        d.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        d.pfnUserCallback = debug_callback;
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(app.instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) fn(app.instance, &d, nullptr, &app.debug_msg);
    }
}

void pick_physical_device() {
    uint32_t c = 0;
    vkEnumeratePhysicalDevices(app.instance, &c, nullptr);
    std::vector<VkPhysicalDevice> devs(c);
    vkEnumeratePhysicalDevices(app.instance, &c, devs.data());
    for (auto pd : devs) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qf.data());
        int g=-1, p=-1;
        for (uint32_t i=0; i<qc; i++) {
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g=i;
            VkBool32 ps=0; vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,app.surface,&ps);
            if (ps) p=i;
            if (g>=0 && p>=0) break;
        }
        if (g<0||p<0) continue;
        uint32_t ec=0; vkEnumerateDeviceExtensionProperties(pd,nullptr,&ec,nullptr);
        std::vector<VkExtensionProperties> ex(ec);
        vkEnumerateDeviceExtensionProperties(pd,nullptr,&ec,ex.data());
        bool sw=false; for (auto& e:ex) if (!strcmp(e.extensionName,VK_KHR_SWAPCHAIN_EXTENSION_NAME)) sw=true;
        if (!sw) continue;
        app.phys_device=pd; app.gfx_family=g; app.present_family=p;
        VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd,&pr);
        printf("GPU: %s\n", pr.deviceName); return;
    }
    fprintf(stderr, "No suitable GPU\n"); exit(1);
}

void create_device() {
    float pri = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qs;
    VkDeviceQueueCreateInfo q{}; q.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex=app.gfx_family; q.queueCount=1; q.pQueuePriorities=&pri;
    qs.push_back(q);
    if (app.present_family != app.gfx_family) { q.queueFamilyIndex=app.present_family; qs.push_back(q); }
    const char* de[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount=(uint32_t)qs.size(); ci.pQueueCreateInfos=qs.data();
    ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=de;
    VkPhysicalDeviceFeatures features{};
    ci.pEnabledFeatures = &features;
    vk_check(vkCreateDevice(app.phys_device,&ci,nullptr,&app.device), "create device");
    vkGetDeviceQueue(app.device,app.gfx_family,0,&app.gfx_queue);
    vkGetDeviceQueue(app.device,app.present_family,0,&app.present_queue);
}

void create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.phys_device, app.surface, &caps);
    uint32_t fc=0; vkGetPhysicalDeviceSurfaceFormatsKHR(app.phys_device,app.surface,&fc,nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(app.phys_device,app.surface,&fc,fmts.data());
    VkSurfaceFormatKHR ch = fmts[0];
    for (auto& f:fmts) if (f.format==VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { ch=f; break; }
    app.sc_format = ch.format;
    uint32_t pc=0; vkGetPhysicalDeviceSurfacePresentModesKHR(app.phys_device,app.surface,&pc,nullptr);
    std::vector<VkPresentModeKHR> pm(pc);
    vkGetPhysicalDeviceSurfacePresentModesKHR(app.phys_device,app.surface,&pc,pm.data());
    VkPresentModeKHR pmode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m:pm) if (m==VK_PRESENT_MODE_MAILBOX_KHR) { pmode=m; break; }
    if (caps.currentExtent.width != UINT32_MAX) app.sc_extent = caps.currentExtent;
    else { int w,h; glfwGetFramebufferSize(app.window,&w,&h);
        app.sc_extent.width=std::clamp((uint32_t)w,caps.minImageExtent.width,caps.maxImageExtent.width);
        app.sc_extent.height=std::clamp((uint32_t)h,caps.minImageExtent.height,caps.maxImageExtent.height); }
    uint32_t ic = caps.minImageCount+1;
    if (caps.maxImageCount>0 && ic>caps.maxImageCount) ic=caps.maxImageCount;
    VkSwapchainCreateInfoKHR si{}; si.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    si.surface=app.surface; si.minImageCount=ic; si.imageFormat=ch.format;
    si.imageColorSpace=ch.colorSpace; si.imageExtent=app.sc_extent; si.imageArrayLayers=1;
    si.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; si.preTransform=caps.currentTransform;
    si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; si.presentMode=pmode;
    si.clipped=VK_TRUE; si.oldSwapchain=app.swapchain;
    uint32_t fam[]={app.gfx_family,app.present_family};
    if (app.gfx_family!=app.present_family) { si.imageSharingMode=VK_SHARING_MODE_CONCURRENT; si.queueFamilyIndexCount=2; si.pQueueFamilyIndices=fam; }
    else si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkSwapchainKHR old=app.swapchain;
    vk_check(vkCreateSwapchainKHR(app.device,&si,nullptr,&app.swapchain),"create swapchain");
    if (old) vkDestroySwapchainKHR(app.device,old,nullptr);
    vkGetSwapchainImagesKHR(app.device,app.swapchain,&ic,nullptr);
    app.sc_images.resize(ic); vkGetSwapchainImagesKHR(app.device,app.swapchain,&ic,app.sc_images.data());
    app.sc_views.resize(ic);
    for (uint32_t i=0;i<ic;i++) {
        VkImageViewCreateInfo v{}; v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        v.image=app.sc_images[i]; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=app.sc_format;
        v.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; v.subresourceRange.levelCount=1; v.subresourceRange.layerCount=1;
        vk_check(vkCreateImageView(app.device,&v,nullptr,&app.sc_views[i]),"create image view");
    }
}

VkFormat find_depth_format() {
    VkFormat c[]={VK_FORMAT_D32_SFLOAT,VK_FORMAT_D32_SFLOAT_S8_UINT,VK_FORMAT_D24_UNORM_S8_UINT};
    for (auto f:c) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(app.phys_device,f,&p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f; }
    exit(1);
}

void create_depth_buffer() {
    VkFormat fmt = find_depth_format();
    VkImageCreateInfo i{}; i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; i.imageType=VK_IMAGE_TYPE_2D;
    i.format=fmt; i.extent={app.sc_extent.width,app.sc_extent.height,1};
    i.mipLevels=1; i.arrayLayers=1; i.samples=VK_SAMPLE_COUNT_1_BIT;
    i.tiling=VK_IMAGE_TILING_OPTIMAL; i.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    vk_check(vkCreateImage(app.device,&i,nullptr,&app.depth_image),"create depth");
    VkMemoryRequirements r; vkGetImageMemoryRequirements(app.device,app.depth_image,&r);
    VkMemoryAllocateInfo a{}; a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    a.allocationSize=r.size; a.memoryTypeIndex=find_memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(app.device,&a,nullptr,&app.depth_memory),"alloc depth");
    vkBindImageMemory(app.device,app.depth_image,app.depth_memory,0);
    VkImageViewCreateInfo v{}; v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v.image=app.depth_image; v.viewType=VK_IMAGE_VIEW_TYPE_2D; v.format=fmt;
    v.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; v.subresourceRange.levelCount=1; v.subresourceRange.layerCount=1;
    vk_check(vkCreateImageView(app.device,&v,nullptr,&app.depth_view),"create depth view");
}

void destroy_depth_buffer() {
    if (app.depth_view) vkDestroyImageView(app.device,app.depth_view,nullptr);
    if (app.depth_image) vkDestroyImage(app.device,app.depth_image,nullptr);
    if (app.depth_memory) vkFreeMemory(app.device,app.depth_memory,nullptr);
    app.depth_view=VK_NULL_HANDLE; app.depth_image=VK_NULL_HANDLE; app.depth_memory=VK_NULL_HANDLE;
}

void create_render_pass() {
    VkAttachmentDescription att[2]{};
    att[0].format=app.sc_format; att[0].samples=VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; att[0].finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1].format=find_depth_format(); att[1].samples=VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; att[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cr{}; cr.attachment=0; cr.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dr{}; dr.attachment=1; dr.layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=1; sub.pColorAttachments=&cr; sub.pDepthStencilAttachment=&dr;
    VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
    dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ri{}; ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ri.attachmentCount=2; ri.pAttachments=att; ri.subpassCount=1; ri.pSubpasses=&sub;
    ri.dependencyCount=1; ri.pDependencies=&dep;
    vk_check(vkCreateRenderPass(app.device,&ri,nullptr,&app.render_pass),"create render pass");
}

// ----------------------------------------------------------------
// Descriptor set layout + pool (for texture binding)
// ----------------------------------------------------------------

void create_descriptor_resources() {
    // Layout: one combined image sampler at binding 0
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings    = &binding;
    vk_check(vkCreateDescriptorSetLayout(app.device, &li, nullptr, &app.quad_desc_layout),
             "create desc layout");

    // Pool: allow up to 1300 textures (one descriptor set per texture)
    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1300;

    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &pool_size;
    pi.maxSets       = 1300;
    vk_check(vkCreateDescriptorPool(app.device, &pi, nullptr, &app.quad_desc_pool),
             "create desc pool");
}

// ----------------------------------------------------------------
// Pipeline creation
// ----------------------------------------------------------------

VkShaderModule create_shader_module(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize=code.size()*4; ci.pCode=code.data();
    VkShaderModule m; vk_check(vkCreateShaderModule(app.device,&ci,nullptr,&m),"shader module"); return m;
}

// ----------------------------------------------------------------
// Skybox: HSL, presets, UBO fill
// ----------------------------------------------------------------

void hsl_to_rgb(float h, float s, float l, float out[3]) {
    h = fmodf(h, 1.0f); if (h < 0) h += 1.0f;
    auto hue2rgb = [](float p, float q, float t) -> float {
        if (t<0.f) t+=1.f; if (t>1.f) t-=1.f;
        if (t<1.f/6.f) return p+(q-p)*6.f*t;
        if (t<1.f/2.f) return q;
        if (t<2.f/3.f) return p+(q-p)*(2.f/3.f-t)*6.f;
        return p;
    };
    if (s<=0.0001f) { out[0]=out[1]=out[2]=l; return; }
    float q=l<0.5f?l*(1.f+s):l+s-l*s; float p=2.f*l-q;
    out[0]=hue2rgb(p,q,h+1.f/3.f); out[1]=hue2rgb(p,q,h); out[2]=hue2rgb(p,q,h-1.f/3.f);
}

// Sky functions (generate_random_preset, lerp_preset, fill_ubo) are in sky.cpp

void create_sky_descriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding=0; binding.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount=1; binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount=1; li.pBindings=&binding;
    vk_check(vkCreateDescriptorSetLayout(app.device,&li,nullptr,&app.sky_desc_layout),"sky desc layout");

    VkDescriptorPoolSize pool_size{}; pool_size.type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount=MAX_FRAMES;
    VkDescriptorPoolCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount=1; pi.pPoolSizes=&pool_size; pi.maxSets=MAX_FRAMES;
    vk_check(vkCreateDescriptorPool(app.device,&pi,nullptr,&app.sky_desc_pool),"sky desc pool");

    VkDescriptorSetLayout layouts[MAX_FRAMES];
    for (int i=0;i<MAX_FRAMES;i++) layouts[i]=app.sky_desc_layout;
    VkDescriptorSetAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool=app.sky_desc_pool; ai.descriptorSetCount=MAX_FRAMES; ai.pSetLayouts=layouts;
    vk_check(vkAllocateDescriptorSets(app.device,&ai,app.sky_desc_sets.data()),"sky desc sets");

    for (int i=0;i<MAX_FRAMES;i++) {
        create_buffer(sizeof(SkyUBOData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      app.sky_ubo_bufs[i], app.sky_ubo_mems[i]);
        vkMapMemory(app.device, app.sky_ubo_mems[i], 0, sizeof(SkyUBOData), 0, &app.sky_ubo_mapped[i]);

        VkDescriptorBufferInfo buf_info{}; buf_info.buffer=app.sky_ubo_bufs[i]; buf_info.range=sizeof(SkyUBOData);
        VkWriteDescriptorSet write{}; write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet=app.sky_desc_sets[i]; write.dstBinding=0; write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo=&buf_info;
        vkUpdateDescriptorSets(app.device,1,&write,0,nullptr);
    }
}

void create_pipelines() {
    // --- Shared state ---
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{}; dyn.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount=2; dyn.pDynamicStates=dyn_states;
    VkPipelineViewportStateCreateInfo vps{}; vps.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount=1; vps.scissorCount=1;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{}; ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo bl{}; bl.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    bl.attachmentCount=1; bl.pAttachments=&ba;

    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc_range.size = sizeof(PushConstants);

    // ========== TERRAIN PIPELINE ==========
    {
        auto vs = compile_glsl(TERRAIN_VERT_SRC, "vertex", "t_vert");
        auto fs = compile_glsl(TERRAIN_FRAG_SRC, "fragment", "t_frag");
        VkShaderModule vm = create_shader_module(vs), fm = create_shader_module(fs);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vm; stages[0].pName="main";
        stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fm; stages[1].pName="main";

        VkVertexInputBindingDescription bind{}; bind.stride=sizeof(Vertex); bind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[3]{};
        attrs[0].location=0; attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset=offsetof(Vertex,px);
        attrs[1].location=1; attrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset=offsetof(Vertex,nx);
        attrs[2].location=2; attrs[2].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[2].offset=offsetof(Vertex,cr);
        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
        vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_BACK_BIT;
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;

        // Terrain pipeline layout: push constants + texture descriptor set
        VkPipelineLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount=1; li.pSetLayouts=&app.quad_desc_layout;
        li.pushConstantRangeCount=1; li.pPushConstantRanges=&pc_range;
        vk_check(vkCreatePipelineLayout(app.device,&li,nullptr,&app.terrain_pipe_layout),"terrain layout");

        VkGraphicsPipelineCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vps; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms;
        pi.pDepthStencilState=&ds; pi.pColorBlendState=&bl; pi.pDynamicState=&dyn;
        pi.layout=app.terrain_pipe_layout; pi.renderPass=app.render_pass;
        vk_check(vkCreateGraphicsPipelines(app.device,VK_NULL_HANDLE,1,&pi,nullptr,&app.terrain_pipeline),"terrain pipeline");
        vkDestroyShaderModule(app.device,fm,nullptr); vkDestroyShaderModule(app.device,vm,nullptr);
    }

    // ========== TEXTURED QUAD PIPELINE ==========
    {
        auto vs = compile_glsl(QUAD_VERT_SRC, "vertex", "q_vert");
        auto fs = compile_glsl(QUAD_FRAG_SRC, "fragment", "q_frag");
        VkShaderModule vm = create_shader_module(vs), fm = create_shader_module(fs);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vm; stages[0].pName="main";
        stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fm; stages[1].pName="main";

        // Quad vertex format: position (vec3) + UV (vec2)
        VkVertexInputBindingDescription bind{}; bind.stride=sizeof(QuadVertex); bind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[2]{};
        attrs[0].location=0; attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset=offsetof(QuadVertex,px);
        attrs[1].location=1; attrs[1].format=VK_FORMAT_R32G32_SFLOAT;    attrs[1].offset=offsetof(QuadVertex,u);
        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
        vi.vertexAttributeDescriptionCount=2; vi.pVertexAttributeDescriptions=attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE;  // quads visible from both sides
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;

        // Quad pipeline layout: push constants + one descriptor set (texture)
        VkPipelineLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount=1; li.pSetLayouts=&app.quad_desc_layout;
        li.pushConstantRangeCount=1; li.pPushConstantRanges=&pc_range;
        vk_check(vkCreatePipelineLayout(app.device,&li,nullptr,&app.quad_pipe_layout),"quad layout");

        VkGraphicsPipelineCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vps; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms;
        pi.pDepthStencilState=&ds; pi.pColorBlendState=&bl; pi.pDynamicState=&dyn;
        pi.layout=app.quad_pipe_layout; pi.renderPass=app.render_pass;
        vk_check(vkCreateGraphicsPipelines(app.device,VK_NULL_HANDLE,1,&pi,nullptr,&app.quad_pipeline),"quad pipeline");
        vkDestroyShaderModule(app.device,fm,nullptr); vkDestroyShaderModule(app.device,vm,nullptr);
    }

    // ========== STRUCTURE PIPELINE (vertex-colored) ==========
    {
        auto vs = compile_glsl(STRUCT_VERT_SRC, "vertex", "s_vert");
        auto fs = compile_glsl(STRUCT_FRAG_SRC, "fragment", "s_frag");
        VkShaderModule vm = create_shader_module(vs), fm = create_shader_module(fs);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vm; stages[0].pName="main";
        stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fm; stages[1].pName="main";

        VkVertexInputBindingDescription bind{}; bind.stride=sizeof(ColorVertex); bind.inputRate=VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[3]{};
        attrs[0].location=0; attrs[0].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset=offsetof(ColorVertex,px);
        attrs[1].location=1; attrs[1].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset=offsetof(ColorVertex,nx);
        attrs[2].location=2; attrs[2].format=VK_FORMAT_R32G32B32_SFLOAT; attrs[2].offset=offsetof(ColorVertex,r);
        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount=1; vi.pVertexBindingDescriptions=&bind;
        vi.vertexAttributeDescriptionCount=3; vi.pVertexAttributeDescriptions=attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_BACK_BIT;
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;

        VkPipelineLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.pushConstantRangeCount=1; li.pPushConstantRanges=&pc_range;
        vk_check(vkCreatePipelineLayout(app.device,&li,nullptr,&app.struct_pipe_layout),"struct layout");

        VkGraphicsPipelineCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vps; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms;
        pi.pDepthStencilState=&ds; pi.pColorBlendState=&bl; pi.pDynamicState=&dyn;
        pi.layout=app.struct_pipe_layout; pi.renderPass=app.render_pass;
        vk_check(vkCreateGraphicsPipelines(app.device,VK_NULL_HANDLE,1,&pi,nullptr,&app.struct_pipeline),"struct pipeline");
        vkDestroyShaderModule(app.device,fm,nullptr); vkDestroyShaderModule(app.device,vm,nullptr);
    }

    // ========== SKYBOX PIPELINE (no depth, fullscreen triangle) ==========
    {
        auto vs = compile_glsl(SKY_VERT_SRC, "vertex", "sky_vert");
        auto fs = compile_glsl(SKY_FRAG_SRC, "fragment", "sky_frag");
        VkShaderModule vm = create_shader_module(vs), fm = create_shader_module(fs);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vm; stages[0].pName="main";
        stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fm; stages[1].pName="main";

        VkPipelineVertexInputStateCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineRasterizationStateCreateInfo rs{}; rs.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE;
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;
        VkPipelineDepthStencilStateCreateInfo sky_ds{};
        sky_ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        sky_ds.depthTestEnable=VK_FALSE; sky_ds.depthWriteEnable=VK_FALSE;

        VkPushConstantRange sky_pc{};
        sky_pc.stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
        sky_pc.size=sizeof(SkyPushConstants);
        VkPipelineLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount=1; li.pSetLayouts=&app.sky_desc_layout;
        li.pushConstantRangeCount=1; li.pPushConstantRanges=&sky_pc;
        vk_check(vkCreatePipelineLayout(app.device,&li,nullptr,&app.sky_pipe_layout),"sky layout");

        VkGraphicsPipelineCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vps; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms;
        pi.pDepthStencilState=&sky_ds; pi.pColorBlendState=&bl; pi.pDynamicState=&dyn;
        pi.layout=app.sky_pipe_layout; pi.renderPass=app.render_pass;
        vk_check(vkCreateGraphicsPipelines(app.device,VK_NULL_HANDLE,1,&pi,nullptr,&app.sky_pipeline),"sky pipeline");
        vkDestroyShaderModule(app.device,fm,nullptr); vkDestroyShaderModule(app.device,vm,nullptr);
    }
}

// ----------------------------------------------------------------
// Framebuffers, command pool, sync
// ----------------------------------------------------------------

void create_framebuffers() {
    app.framebuffers.resize(app.sc_views.size());
    for (size_t i=0; i<app.sc_views.size(); i++) {
        VkImageView views[] = { app.sc_views[i], app.depth_view };
        VkFramebufferCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass=app.render_pass; fi.attachmentCount=2; fi.pAttachments=views;
        fi.width=app.sc_extent.width; fi.height=app.sc_extent.height; fi.layers=1;
        vk_check(vkCreateFramebuffer(app.device,&fi,nullptr,&app.framebuffers[i]),"framebuffer");
    }
}

void create_command_objects() {
    VkCommandPoolCreateInfo pi{}; pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex=app.gfx_family;
    vk_check(vkCreateCommandPool(app.device,&pi,nullptr,&app.cmd_pool),"cmd pool");
    app.cmd_bufs.resize(MAX_FRAMES);
    VkCommandBufferAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool=app.cmd_pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=MAX_FRAMES;
    vk_check(vkAllocateCommandBuffers(app.device,&ai,app.cmd_bufs.data()),"alloc cmd bufs");
}

void create_sync() {
    VkSemaphoreCreateInfo si{}; si.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{}; fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i=0; i<MAX_FRAMES; i++) {
        vk_check(vkCreateSemaphore(app.device,&si,nullptr,&app.sem_available[i]),"sem");
        vk_check(vkCreateSemaphore(app.device,&si,nullptr,&app.sem_finished[i]),"sem");
        vk_check(vkCreateFence(app.device,&fi,nullptr,&app.fences[i]),"fence");
    }
}

// ----------------------------------------------------------------
// Upload quad geometry (a unit quad: 4 vertices, 6 indices)
// ----------------------------------------------------------------

void create_quad_geometry() {
    // Unit quad centered at origin in the XY plane, facing +Z
    // Each quad instance is positioned/rotated via push constants
    QuadVertex verts[] = {
        { -0.5f, -0.5f, 0, 0, 1 },  // bottom-left
        {  0.5f, -0.5f, 0, 1, 1 },  // bottom-right
        {  0.5f,  0.5f, 0, 1, 0 },  // top-right
        { -0.5f,  0.5f, 0, 0, 0 },  // top-left
    };
    uint32_t indices[] = { 0,1,2, 2,3,0 };

    upload_buffer(verts, sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  app.quad_vbuf, app.quad_vmem);
    upload_buffer(indices, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  app.quad_ibuf, app.quad_imem);
}

// ----------------------------------------------------------------
// Swapchain recreation
// ----------------------------------------------------------------

void cleanup_swapchain() {
    destroy_depth_buffer();
    for (auto fb : app.framebuffers) vkDestroyFramebuffer(app.device,fb,nullptr);
    for (auto iv : app.sc_views) vkDestroyImageView(app.device,iv,nullptr);
    app.framebuffers.clear(); app.sc_views.clear();
}

void recreate_swapchain() {
    int w=0,h=0; glfwGetFramebufferSize(app.window,&w,&h);
    while (w==0||h==0) { glfwGetFramebufferSize(app.window,&w,&h); glfwWaitEvents(); }
    vkDeviceWaitIdle(app.device);
    cleanup_swapchain(); create_swapchain(); create_depth_buffer(); create_framebuffers();
}

// ----------------------------------------------------------------
// Full cleanup
// ----------------------------------------------------------------

void cleanup() {
    vkDeviceWaitIdle(app.device);
    for (int i=0;i<MAX_FRAMES;i++) {
        vkDestroySemaphore(app.device,app.sem_available[i],nullptr);
        vkDestroySemaphore(app.device,app.sem_finished[i],nullptr);
        vkDestroyFence(app.device,app.fences[i],nullptr);
    }
    vkDestroyCommandPool(app.device,app.cmd_pool,nullptr);

    // Textures
    for (auto& vp : app.video_players) cleanup_video(vp);
    app.video_players.clear();
    for (auto* gt : app.glyph_texts) destroy_glyph_text(gt);
    app.glyph_texts.clear();

    // Sky cleanup
    for (int i=0;i<MAX_FRAMES;i++) {
        if (app.sky_ubo_mapped[i]) vkUnmapMemory(app.device, app.sky_ubo_mems[i]);
        if (app.sky_ubo_bufs[i]) { vkDestroyBuffer(app.device,app.sky_ubo_bufs[i],nullptr); vkFreeMemory(app.device,app.sky_ubo_mems[i],nullptr); }
    }
    if (app.sky_pipeline) vkDestroyPipeline(app.device,app.sky_pipeline,nullptr);
    if (app.sky_pipe_layout) vkDestroyPipelineLayout(app.device,app.sky_pipe_layout,nullptr);
    if (app.sky_desc_pool) vkDestroyDescriptorPool(app.device,app.sky_desc_pool,nullptr);
    if (app.sky_desc_layout) vkDestroyDescriptorSetLayout(app.device,app.sky_desc_layout,nullptr);

    for (auto& t : app.textures) destroy_texture(t);

    // Trail staging buffers
    if (g_trail_tex_staging_mapped) vkUnmapMemory(app.device, g_trail_tex_staging_mem);
    if (g_trail_tex_staging) { vkDestroyBuffer(app.device, g_trail_tex_staging, nullptr); vkFreeMemory(app.device, g_trail_tex_staging_mem, nullptr); }
    if (g_terrain_vert_staging_mapped) vkUnmapMemory(app.device, g_terrain_vert_staging_mem);
    if (g_terrain_vert_staging) { vkDestroyBuffer(app.device, g_terrain_vert_staging, nullptr); vkFreeMemory(app.device, g_terrain_vert_staging_mem, nullptr); }
    if (g_struct_vert_staging_mapped) vkUnmapMemory(app.device, g_struct_vert_staging_mem);
    if (g_struct_vert_staging) { vkDestroyBuffer(app.device, g_struct_vert_staging, nullptr); vkFreeMemory(app.device, g_struct_vert_staging_mem, nullptr); }

    // Quad geometry
    vkDestroyBuffer(app.device,app.quad_vbuf,nullptr); vkFreeMemory(app.device,app.quad_vmem,nullptr);
    vkDestroyBuffer(app.device,app.quad_ibuf,nullptr); vkFreeMemory(app.device,app.quad_imem,nullptr);

    // Structure geometry
    if (app.struct_vbuf) { vkDestroyBuffer(app.device,app.struct_vbuf,nullptr); vkFreeMemory(app.device,app.struct_vmem,nullptr); }
    if (app.struct_ibuf) { vkDestroyBuffer(app.device,app.struct_ibuf,nullptr); vkFreeMemory(app.device,app.struct_imem,nullptr); }

    // Terrain geometry
    vkDestroyBuffer(app.device,app.terrain_vbuf,nullptr); vkFreeMemory(app.device,app.terrain_vmem,nullptr);
    vkDestroyBuffer(app.device,app.terrain_ibuf,nullptr); vkFreeMemory(app.device,app.terrain_imem,nullptr);

    cleanup_swapchain();
    vkDestroyPipeline(app.device,app.struct_pipeline,nullptr);
    vkDestroyPipelineLayout(app.device,app.struct_pipe_layout,nullptr);
    vkDestroyPipeline(app.device,app.terrain_pipeline,nullptr);
    vkDestroyPipelineLayout(app.device,app.terrain_pipe_layout,nullptr);
    vkDestroyPipeline(app.device,app.quad_pipeline,nullptr);
    vkDestroyPipelineLayout(app.device,app.quad_pipe_layout,nullptr);
    vkDestroyDescriptorPool(app.device,app.quad_desc_pool,nullptr);
    vkDestroyDescriptorSetLayout(app.device,app.quad_desc_layout,nullptr);
    vkDestroyRenderPass(app.device,app.render_pass,nullptr);
    vkDestroySwapchainKHR(app.device,app.swapchain,nullptr);
    vkDestroyDevice(app.device,nullptr);
    vkDestroySurfaceKHR(app.instance,app.surface,nullptr);
    if (app.debug_msg) {
        auto fn=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(app.instance,"vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(app.instance,app.debug_msg,nullptr);
    }
    vkDestroyInstance(app.instance,nullptr);
    glfwDestroyWindow(app.window); glfwTerminate();
}
