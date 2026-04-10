#include "renderer.h"
#include "app.h"
#include "vulkan_setup.h"
#include "sky.h"
#include "trail.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

Mat4 mat4_quad_model(const Quad& q) {
    constexpr float D = 3.14159265f / 180.0f;
    float cx = cosf(q.rot_x * D), sx = sinf(q.rot_x * D);
    float cy = cosf(q.rot_y * D), sy = sinf(q.rot_y * D);
    float cz = cosf(q.rot_z * D), sz = sinf(q.rot_z * D);
    // R = Ry * Rx * Rz, scaled: col0 *= w, col1 *= h
    Mat4 m;
    m.m[0]  = (cy*cz + sx*sy*sz) * q.w;   m.m[4]  = (-cy*sz + sx*sy*cz) * q.h;   m.m[8]  = cx*sy;   m.m[12] = q.x;
    m.m[1]  = (cx*sz) * q.w;               m.m[5]  = (cx*cz) * q.h;               m.m[9]  = -sx;     m.m[13] = q.y;
    m.m[2]  = (-sy*cz + sx*cy*sz) * q.w;   m.m[6]  = (sy*sz + sx*cy*cz) * q.h;    m.m[10] = cx*cy;   m.m[14] = q.z;
    m.m[3]  = 0;                            m.m[7]  = 0;                            m.m[11] = 0;       m.m[15] = 1;
    return m;
}

// Model matrix for world-space glyph text (position + scale + Y rotation)
Mat4 mat4_glyph_text_model(const GlyphText& t) {
    float rad = t.rot_y * 3.14159265f / 180.0f;
    float cy = cosf(rad), sy = sinf(rad);
    float s = t.char_size;
    Mat4 m;
    m.m[0]  = s * cy;  m.m[4]  = 0;   m.m[8]  = s * sy;  m.m[12] = t.x;
    m.m[1]  = 0;        m.m[5]  = s;   m.m[9]  = 0;        m.m[13] = t.y;
    m.m[2]  = -sy;       m.m[6]  = 0;   m.m[10] = cy;       m.m[14] = t.z;
    m.m[3]  = 0;         m.m[7]  = 0;   m.m[11] = 0;        m.m[15] = 1;
    return m;
}

// ----------------------------------------------------------------
// Draw frame
// ----------------------------------------------------------------

void draw_frame() {
    uint32_t f = app.frame;
    vkWaitForFences(app.device,1,&app.fences[f],VK_TRUE,UINT64_MAX);
    uint32_t img;
    VkResult r = vkAcquireNextImageKHR(app.device,app.swapchain,UINT64_MAX,app.sem_available[f],VK_NULL_HANDLE,&img);
    if (r==VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
    else if (r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR) vk_check(r,"acquire");
    vkResetFences(app.device,1,&app.fences[f]);

    VkCommandBuffer cmd = app.cmd_bufs[f];
    vkResetCommandBuffer(cmd,0);
    VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd,&bi);

    // --- Trail texture upload (before render pass) ---
    if (g_trail_tex_dirty && app.terrain_tex_index >= 0) {
        VkImage terrImg = app.textures[app.terrain_tex_index].image;

        VkImageMemoryBarrier bar{};
        bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcAccessMask       = 0;
        bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image               = terrImg;
        bar.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        // Upload only the dirty sub-rectangle
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        if (g_trail_dirty_px_max >= 0) {
            int rx = g_trail_dirty_px_min;
            int rz = g_trail_dirty_pz_min;
            int rw = g_trail_dirty_px_max - rx + 1;
            int rh = g_trail_dirty_pz_max - rz + 1;
            region.bufferOffset    = ((size_t)rz * TRAIL_TEX_SIZE + rx) * 4;
            region.bufferRowLength = (uint32_t)TRAIL_TEX_SIZE;
            region.imageOffset     = {rx, rz, 0};
            region.imageExtent     = {(uint32_t)rw, (uint32_t)rh, 1};
        } else {
            region.imageExtent = {(uint32_t)TRAIL_TEX_SIZE, (uint32_t)TRAIL_TEX_SIZE, 1};
        }
        vkCmdCopyBufferToImage(cmd, g_trail_tex_staging, terrImg,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bar);

        trail_reset_dirty_region();
        g_trail_tex_dirty = false;
    }

    // --- Trail mesh depression upload (before render pass) ---
    if (g_terrain_mesh_dirty && g_mesh_dirty_gz_max >= 0) {
        int N = GRID_SIZE;
        VkBufferCopy bregion{};
        bregion.srcOffset = (VkDeviceSize)g_mesh_dirty_gz_min * N * sizeof(Vertex);
        bregion.dstOffset = bregion.srcOffset;
        bregion.size = (VkDeviceSize)(g_mesh_dirty_gz_max - g_mesh_dirty_gz_min + 1) * N * sizeof(Vertex);
        vkCmdCopyBuffer(cmd, g_terrain_vert_staging, app.terrain_vbuf, 1, &bregion);

        VkBufferMemoryBarrier bbar{};
        bbar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bbar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bbar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        bbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bbar.buffer = app.terrain_vbuf;
        bbar.offset = bregion.dstOffset;
        bbar.size   = bregion.size;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 0, nullptr, 1, &bbar, 0, nullptr);

        g_mesh_dirty_gz_min = GRID_SIZE;
        g_mesh_dirty_gz_max = -1;
        g_terrain_mesh_dirty = false;
    }

    // --- Structure vertex wear upload (before render pass) ---
    if (g_struct_wear_dirty && g_struct_vert_staging != VK_NULL_HANDLE && app.struct_vbuf != VK_NULL_HANDLE) {
        VkBufferCopy bregion{};
        bregion.size = g_struct_verts.size() * sizeof(ColorVertex);
        vkCmdCopyBuffer(cmd, g_struct_vert_staging, app.struct_vbuf, 1, &bregion);

        VkBufferMemoryBarrier bbar{};
        bbar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bbar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bbar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        bbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bbar.buffer = app.struct_vbuf;
        bbar.size   = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0, 0, nullptr, 1, &bbar, 0, nullptr);

        g_struct_wear_dirty = false;
    }

    // Upload video frames to GPU textures (only for videos with new frame data)
    for (auto& vp : app.video_players) {
        if (!vp.active || vp.tex_index < 0 || !vp.has_new_frame) continue;
        VkImage vidImg = app.textures[vp.tex_index].image;
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = vidImg;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {(uint32_t)vp.w, (uint32_t)vp.h, 1};
        vkCmdCopyBufferToImage(cmd, vp.staging_buf, vidImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);
        vp.has_new_frame = false;
    }

    VkClearValue clears[2]{}; clears[0].color={{0.0f,0.0f,0.0f,1.0f}}; clears[1].depthStencil={1,0};
    VkRenderPassBeginInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass=app.render_pass; rp.framebuffer=app.framebuffers[img];
    rp.renderArea.extent=app.sc_extent; rp.clearValueCount=2; rp.pClearValues=clears;
    vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{}; vp.width=(float)app.sc_extent.width; vp.height=(float)app.sc_extent.height; vp.maxDepth=1;
    vkCmdSetViewport(cmd,0,1,&vp);
    VkRect2D sc{}; sc.extent=app.sc_extent;
    vkCmdSetScissor(cmd,0,1,&sc);

    // Build view-projection matrix (shared by all pipelines)
    Camera& c = app.cam; c.update_vectors();
    float aspect = (float)app.sc_extent.width / (float)app.sc_extent.height;
    Mat4 view = mat4_look_at(c.x,c.y,c.z, c.x+c.fx,c.y+c.fy,c.z+c.fz, 0,1,0);
    Mat4 proj = mat4_perspective(app.fov, aspect, 0.5f, 1500);
    Mat4 vp_mat = mat4_mul(proj, view);

    // ---- Draw skybox (first, no depth) ----
    if (app.sky_pipeline) {
        // Update UBO
        SkyUBOData ubo = fill_ubo(app.display_preset, g_time);
        memcpy(app.sky_ubo_mapped[f], &ubo, sizeof(ubo));

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.sky_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.sky_pipe_layout,
                                0, 1, &app.sky_desc_sets[f], 0, nullptr);

        // Camera up = cross(right, forward)
        float ux = c.ry*c.fz - c.rz*c.fy;
        float uy = c.rz*c.fx - c.rx*c.fz;
        float uz = c.rx*c.fy - c.ry*c.fx;

        SkyPushConstants sky_pc{};
        sky_pc.cam_fwd[0]=c.fx; sky_pc.cam_fwd[1]=c.fy; sky_pc.cam_fwd[2]=c.fz;
        sky_pc.cam_right[0]=c.rx; sky_pc.cam_right[1]=c.ry; sky_pc.cam_right[2]=c.rz;
        sky_pc.cam_up[0]=ux; sky_pc.cam_up[1]=uy; sky_pc.cam_up[2]=uz;
        sky_pc.params[0]=aspect;
        sky_pc.params[1]=tanf(app.fov * 3.14159265f / 360.0f);
        sky_pc.params[2]=g_time;
        vkCmdPushConstants(cmd, app.sky_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(sky_pc), &sky_pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    // ---- Draw terrain ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.terrain_pipeline);
    if (app.terrain_tex_index >= 0) {
        VkDescriptorSet ds = app.textures[app.terrain_tex_index].desc_set;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                app.terrain_pipe_layout, 0, 1, &ds, 0, nullptr);
    }
    VkBuffer tb[] = { app.terrain_vbuf }; VkDeviceSize to[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, tb, to);
    vkCmdBindIndexBuffer(cmd, app.terrain_ibuf, 0, VK_INDEX_TYPE_UINT32);

    PushConstants pc{};
    memcpy(pc.mvp, vp_mat.m, 64);
    pc.cam_pos[0]=c.x; pc.cam_pos[1]=c.y; pc.cam_pos[2]=c.z;
    pc.fog_on = app.fog_on ? 1.0f : 0.0f;
    pc.highlight = 0.0f;
    vkCmdPushConstants(cmd, app.terrain_pipe_layout,
                       VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDrawIndexed(cmd, app.terrain_idx_count, 1, 0, 0, 0);

    // ---- Draw structures ----
    if (app.struct_idx_count > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.struct_pipeline);
        VkBuffer sb[] = { app.struct_vbuf }; VkDeviceSize so[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, sb, so);
        vkCmdBindIndexBuffer(cmd, app.struct_ibuf, 0, VK_INDEX_TYPE_UINT32);

        PushConstants spc{};
        memcpy(spc.mvp, vp_mat.m, 64);
        spc.cam_pos[0]=c.x; spc.cam_pos[1]=c.y; spc.cam_pos[2]=c.z;
        spc.fog_on = app.fog_on ? 1.0f : 0.0f;
        spc.highlight = 0.0f;
        vkCmdPushConstants(cmd, app.struct_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(spc), &spc);
        vkCmdDrawIndexed(cmd, app.struct_idx_count, 1, 0, 0, 0);
    }

    // ---- Draw textured quads ----
    if (!app.quads.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.quad_pipeline);
        VkBuffer qb[] = { app.quad_vbuf }; VkDeviceSize qo[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, qb, qo);
        vkCmdBindIndexBuffer(cmd, app.quad_ibuf, 0, VK_INDEX_TYPE_UINT32);

        for (int qi = 0; qi < (int)app.quads.size(); qi++) {
            auto& q = app.quads[qi];
            if (q.texture_index < 0 || q.texture_index >= (int)app.textures.size()) continue;
            Texture& tex = app.textures[q.texture_index];

            // Bind this quad's texture via its descriptor set
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app.quad_pipe_layout, 0, 1, &tex.desc_set, 0, nullptr);

            // Build MVP = viewproj * model for this quad
            Mat4 model = mat4_quad_model(q);
            Mat4 mvp = mat4_mul(vp_mat, model);
            PushConstants qpc{};
            memcpy(qpc.mvp, mvp.m, 64);
            qpc.cam_pos[0]=c.x; qpc.cam_pos[1]=c.y; qpc.cam_pos[2]=c.z;
            qpc.fog_on = app.fog_on ? 1.0f : 0.0f;
            qpc.highlight = (qi == g_selected_quad_idx) ? 1.0f : 0.0f;
            vkCmdPushConstants(cmd, app.quad_pipe_layout,
                               VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(qpc), &qpc);

            vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
        }
    }

    // ---- Draw world-space glyph text (TEXT nodes) ----
    if (app.font_atlas_index >= 0 && !app.glyph_texts.empty()) {
        // Reuse quad pipeline, bind font atlas
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.quad_pipeline);
        Texture& atlas = app.textures[app.font_atlas_index];
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                app.quad_pipe_layout, 0, 1, &atlas.desc_set, 0, nullptr);

        for (int gi = 0; gi < (int)app.glyph_texts.size(); gi++) {
            auto* gt = app.glyph_texts[gi];
            if (!gt || gt->char_count == 0 || gt->is_hud) continue;

            VkBuffer vb[] = { gt->vbuf }; VkDeviceSize off[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
            vkCmdBindIndexBuffer(cmd, gt->ibuf, 0, VK_INDEX_TYPE_UINT32);

            Mat4 model = mat4_glyph_text_model(*gt);
            Mat4 mvp = mat4_mul(vp_mat, model);
            PushConstants tpc{};
            memcpy(tpc.mvp, mvp.m, 64);
            tpc.cam_pos[0]=c.x; tpc.cam_pos[1]=c.y; tpc.cam_pos[2]=c.z;
            tpc.fog_on = app.fog_on ? 1.0f : 0.0f;
            tpc.highlight = (gi == g_selected_glyph_idx) ? 2.0f : 0.0f;
            vkCmdPushConstants(cmd, app.quad_pipe_layout,
                               VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(tpc), &tpc);

            vkCmdDrawIndexed(cmd, gt->char_count * 6, 1, 0, 0, 0);
        }

        // ---- Draw HUD (menu, inventory, toasts) — orthographic, on top of everything ----
        bool has_hud = false;
        for (auto* gt : app.glyph_texts) {
            if (gt && gt->char_count > 0 && gt->is_hud) { has_hud = true; break; }
        }
        bool has_inv_thumbs = !g_inventory.empty();
        if (has_hud || has_inv_thumbs) {
            VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            ca.clearValue.depthStencil = {1.0f, 0};
            VkClearRect cr{}; cr.rect.extent = app.sc_extent; cr.layerCount = 1;
            vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);

            float sw = (float)app.sc_extent.width;
            float sh = (float)app.sc_extent.height;
            Mat4 hud_proj = mat4_ortho(0, sw, -sh, 0, -1, 1);

            // HUD glyph texts (menu, inventory labels, toasts)
            if (has_hud) {
                for (auto* gt : app.glyph_texts) {
                    if (!gt || gt->char_count == 0 || !gt->is_hud) continue;

                    VkBuffer vb[] = { gt->vbuf }; VkDeviceSize off[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
                    vkCmdBindIndexBuffer(cmd, gt->ibuf, 0, VK_INDEX_TYPE_UINT32);

                    Mat4 model{};
                    float s = gt->char_size;
                    model.m[0]  = s;    model.m[5]  = s;   model.m[10] = 1;
                    model.m[12] = gt->x; model.m[13] = gt->y; model.m[14] = 0;
                    model.m[15] = 1;
                    Mat4 mvp = mat4_mul(hud_proj, model);
                    PushConstants tpc{};
                    memcpy(tpc.mvp, mvp.m, 64);
                    tpc.cam_pos[0]=c.x; tpc.cam_pos[1]=c.y; tpc.cam_pos[2]=c.z;
                    tpc.fog_on = 0.0f;
                    tpc.highlight = 0.0f;
                    vkCmdPushConstants(cmd, app.quad_pipe_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(tpc), &tpc);
                    vkCmdDrawIndexed(cmd, gt->char_count * 6, 1, 0, 0, 0);
                }
            }

            // HUD inventory thumbnail quads
            if (has_inv_thumbs) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, app.quad_pipeline);
                VkBuffer qb2[] = { app.quad_vbuf }; VkDeviceSize qo2[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, qb2, qo2);
                vkCmdBindIndexBuffer(cmd, app.quad_ibuf, 0, VK_INDEX_TYPE_UINT32);

                constexpr float THUMB = 48.0f, START_X = 10.0f, START_Y = 10.0f, SPACING = 56.0f;
                int count = std::min((int)g_inventory.size(), 10);
                for (int i = 0; i < count; i++) {
                    int inv_idx = (int)g_inventory.size() - 1 - i;
                    auto& item = g_inventory[inv_idx];
                    auto it = g_hash_to_tex.find(item.hash);
                    if (it == g_hash_to_tex.end() || it->second < 0) continue;
                    int ti = it->second;
                    if (ti >= (int)app.textures.size()) continue;

                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            app.quad_pipe_layout, 0, 1,
                                            &app.textures[ti].desc_set, 0, nullptr);

                    float tw = THUMB, th = THUMB;
                    if (item.img_w > 0 && item.img_h > 0) {
                        float ar = (float)item.img_w / (float)item.img_h;
                        if (ar > 1.0f) th = tw / ar; else tw = th * ar;
                    }
                    float cx = START_X + tw * 0.5f;
                    float cy = START_Y + i * SPACING + th * 0.5f;

                    Mat4 model{};
                    model.m[0]=tw; model.m[5]=th; model.m[10]=1;
                    model.m[12]=cx; model.m[13]=-cy; model.m[15]=1;
                    Mat4 mvp = mat4_mul(hud_proj, model);
                    PushConstants qpc{};
                    memcpy(qpc.mvp, mvp.m, 64);
                    qpc.cam_pos[0]=c.x; qpc.cam_pos[1]=c.y; qpc.cam_pos[2]=c.z;
                    qpc.fog_on = 0.0f;
                    qpc.highlight = (i == 0) ? 1.0f : 0.0f;
                    vkCmdPushConstants(cmd, app.quad_pipe_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(qpc), &qpc);
                    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
                }
            }
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount=1; si.pWaitSemaphores=&app.sem_available[f]; si.pWaitDstStageMask=&ws;
    si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    si.signalSemaphoreCount=1; si.pSignalSemaphores=&app.sem_finished[f];
    vk_check(vkQueueSubmit(app.gfx_queue,1,&si,app.fences[f]),"submit");

    VkPresentInfoKHR pi{}; pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&app.sem_finished[f];
    pi.swapchainCount=1; pi.pSwapchains=&app.swapchain; pi.pImageIndices=&img;
    r = vkQueuePresentKHR(app.present_queue,&pi);
    if (r==VK_ERROR_OUT_OF_DATE_KHR || r==VK_SUBOPTIMAL_KHR || app.fb_resized) {
        app.fb_resized=false; recreate_swapchain();
    } else if (r!=VK_SUCCESS) vk_check(r,"present");
    app.frame = (f+1) % MAX_FRAMES;
}
