#include "video.h"
#include "vulkan_setup.h"
#include "textures.h"
#include "terrain.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <algorithm>

// net_send is defined elsewhere in client.cpp; forward-declare until it is extracted.
extern void net_send(uint8_t type, const void* payload, uint32_t plen);

static bool sdl_audio_inited = false;

void open_media(App::VideoPlayer& vp, const char* path) {
    if (avformat_open_input(&vp.fmt_ctx, path, nullptr, nullptr) < 0) {
        fprintf(stderr, "Cannot open video: %s\n", path); return;
    }
    if (avformat_find_stream_info(vp.fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "Cannot find stream info\n"); avformat_close_input(&vp.fmt_ctx); return;
    }
    for (unsigned i = 0; i < vp.fmt_ctx->nb_streams; i++) {
        auto* par = vp.fmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && vp.vid_stream_idx < 0)
            vp.vid_stream_idx = (int)i;
        else if (par->codec_type == AVMEDIA_TYPE_AUDIO && vp.aud_stream_idx < 0)
            vp.aud_stream_idx = (int)i;
    }
    if (vp.vid_stream_idx < 0) { fprintf(stderr, "No video stream in %s\n", path); avformat_close_input(&vp.fmt_ctx); return; }

    auto* vpar = vp.fmt_ctx->streams[vp.vid_stream_idx]->codecpar;
    auto* vcodec = avcodec_find_decoder(vpar->codec_id);
    if (!vcodec) { fprintf(stderr, "Unsupported video codec\n"); return; }
    vp.vid_codec_ctx = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vp.vid_codec_ctx, vpar);
    avcodec_open2(vp.vid_codec_ctx, vcodec, nullptr);

    vp.w = vp.vid_codec_ctx->width;
    vp.h = vp.vid_codec_ctx->height;
    vp.vid_timebase = av_q2d(vp.fmt_ctx->streams[vp.vid_stream_idx]->time_base);
    AVRational avg_fr = vp.fmt_ctx->streams[vp.vid_stream_idx]->avg_frame_rate;
    vp.fps = (avg_fr.den > 0) ? av_q2d(avg_fr) : 30.0;

    vp.sws_ctx = sws_getContext(vp.w, vp.h, vp.vid_codec_ctx->pix_fmt,
                                vp.w, vp.h, AV_PIX_FMT_RGBA,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    vp.dec_frame = av_frame_alloc();
    vp.rgb_frame = av_frame_alloc();
    vp.rgb_frame->format = AV_PIX_FMT_RGBA;
    vp.rgb_frame->width = vp.w;
    vp.rgb_frame->height = vp.h;
    av_image_alloc(vp.rgb_frame->data, vp.rgb_frame->linesize, vp.w, vp.h, AV_PIX_FMT_RGBA, 32);
    vp.av_pkt = av_packet_alloc();

    if (vp.aud_stream_idx >= 0) {
        auto* apar = vp.fmt_ctx->streams[vp.aud_stream_idx]->codecpar;
        auto* acodec = avcodec_find_decoder(apar->codec_id);
        if (acodec) {
            vp.aud_codec_ctx = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(vp.aud_codec_ctx, apar);
            avcodec_open2(vp.aud_codec_ctx, acodec, nullptr);
            vp.aud_dec_frame = av_frame_alloc();
            AVChannelLayout outCh = AV_CHANNEL_LAYOUT_STEREO;
            swr_alloc_set_opts2(&vp.swr_ctx,
                &outCh, AV_SAMPLE_FMT_S16, vp.aud_codec_ctx->sample_rate,
                &vp.aud_codec_ctx->ch_layout, vp.aud_codec_ctx->sample_fmt,
                vp.aud_codec_ctx->sample_rate, 0, nullptr);
            swr_init(vp.swr_ctx);
        }
    }
    vp.active = true;
    vp.path = path;
    vp.mp_per_sec = (double)vp.w * vp.h * vp.fps / 1e6;
    printf("Opened video: %s (%dx%d, %.1f fps, %.1f MP/s)\n", path, vp.w, vp.h, vp.fps, vp.mp_per_sec);
}

void init_video_audio(App::VideoPlayer& vp) {
    if (!vp.aud_codec_ctx) return;
    if (!sdl_audio_inited) {
        if (SDL_Init(SDL_INIT_AUDIO) < 0) { fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError()); return; }
        sdl_audio_inited = true;
    }
    SDL_AudioSpec want{};
    want.freq = vp.aud_codec_ctx->sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 4096;
    vp.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (vp.audio_dev) SDL_PauseAudioDevice(vp.audio_dev, 1); // start paused, budget system controls
}

void process_audio_packet(App::VideoPlayer& vp) {
    if (!vp.aud_codec_ctx || !vp.swr_ctx || vp.audio_dev == 0) return;
    if (vp.audio_volume < 0.001f) {
        // Not the active audio source — still decode to keep codec state correct, but discard
        avcodec_send_packet(vp.aud_codec_ctx, vp.av_pkt);
        while (avcodec_receive_frame(vp.aud_codec_ctx, vp.aud_dec_frame) == 0) {}
        return;
    }
    avcodec_send_packet(vp.aud_codec_ctx, vp.av_pkt);
    while (avcodec_receive_frame(vp.aud_codec_ctx, vp.aud_dec_frame) == 0) {
        int maxOut = swr_get_out_samples(vp.swr_ctx, vp.aud_dec_frame->nb_samples);
        if (maxOut <= 0) continue;
        uint8_t* buf = nullptr;
        av_samples_alloc(&buf, nullptr, 2, maxOut, AV_SAMPLE_FMT_S16, 0);
        int got = swr_convert(vp.swr_ctx, &buf, maxOut,
                              (const uint8_t**)vp.aud_dec_frame->extended_data,
                              vp.aud_dec_frame->nb_samples);
        if (got > 0) {
            int16_t* samples = (int16_t*)buf;
            int total = got * 2;
            float vol = vp.audio_volume * g_master_volume;
            for (int i = 0; i < total; i++)
                samples[i] = (int16_t)(samples[i] * vol);
            SDL_QueueAudio(vp.audio_dev, buf, (uint32_t)(got * 2 * sizeof(int16_t)));
        }
        av_freep(&buf);
    }
}

bool decode_video_frame(App::VideoPlayer& vp) {
    while (true) {
        int ret = avcodec_receive_frame(vp.vid_codec_ctx, vp.dec_frame);
        if (ret == 0) {
            sws_scale(vp.sws_ctx, vp.dec_frame->data, vp.dec_frame->linesize, 0, vp.h,
                      vp.rgb_frame->data, vp.rgb_frame->linesize);
            vp.cur_pts = vp.dec_frame->pts * vp.vid_timebase;
            return true;
        }
        if (ret == AVERROR_EOF) return false;
        ret = av_read_frame(vp.fmt_ctx, vp.av_pkt);
        if (ret < 0) { avcodec_send_packet(vp.vid_codec_ctx, nullptr); continue; }
        if (vp.av_pkt->stream_index == vp.vid_stream_idx)
            avcodec_send_packet(vp.vid_codec_ctx, vp.av_pkt);
        else if (vp.av_pkt->stream_index == vp.aud_stream_idx)
            process_audio_packet(vp);
        av_packet_unref(vp.av_pkt);
    }
}

void seek_video_to_start(App::VideoPlayer& vp) {
    av_seek_frame(vp.fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vp.vid_codec_ctx);
    if (vp.aud_codec_ctx) avcodec_flush_buffers(vp.aud_codec_ctx);
    if (vp.audio_dev) SDL_ClearQueuedAudio(vp.audio_dev);
}

void create_video_texture(App::VideoPlayer& vp) {
    int w = vp.w, h = vp.h;
    VkDeviceSize img_size = (VkDeviceSize)w * h * 4;

    // Persistent staging buffer
    VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = img_size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vk_check(vkCreateBuffer(app.device, &bci, nullptr, &vp.staging_buf), "video staging buf");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(app.device, vp.staging_buf, &req);
    VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkAllocateMemory(app.device, &mai, nullptr, &vp.staging_mem), "video staging mem");
    vkBindBufferMemory(app.device, vp.staging_buf, vp.staging_mem, 0);
    vkMapMemory(app.device, vp.staging_mem, 0, img_size, 0, &vp.staging_mapped);

    Texture tex; tex.width = w; tex.height = h;
    VkImageCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_SRGB;
    ici.extent = {(uint32_t)w, (uint32_t)h, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vk_check(vkCreateImage(app.device, &ici, nullptr, &tex.image), "video image");

    vkGetImageMemoryRequirements(app.device, tex.image, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(app.device, &mai, nullptr, &tex.memory), "video image mem");
    vkBindImageMemory(app.device, tex.image, tex.memory, 0);

    VkImageViewCreateInfo vci{}; vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = tex.image; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_check(vkCreateImageView(app.device, &vci, nullptr, &tex.view), "video image view");

    VkSamplerCreateInfo sci{}; sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(app.device, &sci, nullptr, &tex.sampler), "video sampler");

    VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = app.quad_desc_pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &app.quad_desc_layout;
    vk_check(vkAllocateDescriptorSets(app.device, &dsai, &tex.desc_set), "video desc set");

    VkDescriptorImageInfo img_info{}; img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_info.imageView = tex.view; img_info.sampler = tex.sampler;
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.desc_set; write.dstBinding = 0; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1; write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(app.device, 1, &write, 0, nullptr);

    vp.tex_index = (int)app.textures.size();
    app.textures.push_back(tex);
    printf("Video texture created: %dx%d (texture index %d)\n", w, h, vp.tex_index);
}

void upload_video_to_staging(App::VideoPlayer& vp) {
    int linesize = vp.rgb_frame->linesize[0];
    if (linesize == vp.w * 4) {
        memcpy(vp.staging_mapped, vp.rgb_frame->data[0], (size_t)vp.w * vp.h * 4);
    } else {
        uint8_t* dst = (uint8_t*)vp.staging_mapped;
        uint8_t* src = vp.rgb_frame->data[0];
        int row_bytes = vp.w * 4;
        for (int y = 0; y < vp.h; y++)
            memcpy(dst + y * row_bytes, src + y * linesize, row_bytes);
    }
}

void cleanup_video(App::VideoPlayer& vp) {
    if (vp.staging_buf) { vkDestroyBuffer(app.device, vp.staging_buf, nullptr); vkFreeMemory(app.device, vp.staging_mem, nullptr); }
    if (vp.rgb_frame) { av_freep(&vp.rgb_frame->data[0]); av_frame_free(&vp.rgb_frame); }
    av_frame_free(&vp.dec_frame); av_frame_free(&vp.aud_dec_frame);
    av_packet_free(&vp.av_pkt); sws_freeContext(vp.sws_ctx); swr_free(&vp.swr_ctx);
    avcodec_free_context(&vp.vid_codec_ctx); avcodec_free_context(&vp.aud_codec_ctx);
    avformat_close_input(&vp.fmt_ctx);
    if (vp.audio_dev) SDL_CloseAudioDevice(vp.audio_dev);
}

// Start video playback from a file (for local watcher) or temp file (for network)
int start_video_player(const char* path) {
    app.video_players.emplace_back();
    auto& vp = app.video_players.back();
    open_media(vp, path);
    if (!vp.active) { app.video_players.pop_back(); return -1; }
    create_video_texture(vp);
    init_video_audio(vp);
    if (decode_video_frame(vp)) {
        upload_video_to_staging(vp);
        vp.has_new_frame = true;
    }
    vp.play_start = App::VideoPlayer::VClock::now();
    return (int)app.video_players.size() - 1;
}

// ----------------------------------------------------------------
// Decode budgeting + proximity audio
// ----------------------------------------------------------------

static float compute_audio_volume(float cam_x, float cam_y, float cam_z,
                                  float nx, float ny, float nz,
                                  float node_x, float node_y, float node_z,
                                  float distance) {
    if (distance > AUDIO_MAX_DISTANCE) return 0.0f;

    // Direction from video to camera
    float inv_d = 1.0f / fmaxf(distance, 0.001f);
    float dx = (cam_x - node_x) * inv_d;
    float dy = (cam_y - node_y) * inv_d;
    float dz = (cam_z - node_z) * inv_d;

    // Dot with surface normal = cos(angle)
    float cos_angle = dx * nx + dy * ny + dz * nz;
    if (cos_angle <= 0.0f) return 0.0f; // behind the video

    // Cone attenuation
    constexpr float D = 3.14159265f / 180.0f;
    float cos_cone = cosf(AUDIO_CONE_HALF_ANGLE * D);
    float cone_factor = (cos_angle >= cos_cone) ? 1.0f
                      : (cos_angle / cos_cone) * (cos_angle / cos_cone);

    // Distance attenuation
    float dist_factor = (distance <= AUDIO_ROLLOFF_START) ? 1.0f
                      : 1.0f - (distance - AUDIO_ROLLOFF_START) / (AUDIO_MAX_DISTANCE - AUDIO_ROLLOFF_START);

    return cone_factor * dist_factor;
}

void update_video_budget() {
    Camera& cam = app.cam;

    // Build per-video-player distance info from world nodes
    struct VidInfo {
        int vp_idx;
        float dist;
        float nx, ny, nz; // surface normal
        float wx, wy, wz; // world position
    };
    std::vector<VidInfo> infos;

    constexpr float D = 3.14159265f / 180.0f;
    for (auto& node : g_world) {
        if (node.node_type != NODE_VIDEO || node.video_index < 0) continue;
        if (node.video_index >= (int)app.video_players.size()) continue;

        float dx = node.x - cam.x, dy = node.y - cam.y, dz = node.z - cam.z;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);

        // Surface normal from rotation (column 2 of Ry * Rx)
        float cx = cosf(node.rot_x * D), sx = sinf(node.rot_x * D);
        float cy = cosf(node.rot_y * D), sy = sinf(node.rot_y * D);
        float nx = cx * sy, ny = -sx, nz = cx * cy;

        // Deduplicate: keep closest DataNode per video_index
        bool found = false;
        for (auto& info : infos) {
            if (info.vp_idx == node.video_index) {
                if (dist < info.dist) {
                    info.dist = dist;
                    info.nx = nx; info.ny = ny; info.nz = nz;
                    info.wx = node.x; info.wy = node.y; info.wz = node.z;
                }
                found = true;
                break;
            }
        }
        if (!found)
            infos.push_back({node.video_index, dist, nx, ny, nz, node.x, node.y, node.z});
    }

    // Sort by distance
    std::sort(infos.begin(), infos.end(), [](const VidInfo& a, const VidInfo& b) {
        return a.dist < b.dist;
    });

    // Save previous decoding state for resync
    std::vector<bool> was_decoding(app.video_players.size());
    for (int i = 0; i < (int)app.video_players.size(); i++)
        was_decoding[i] = app.video_players[i].decoding;

    // Mark all as not decoding, zero audio
    for (auto& vp : app.video_players) {
        vp.decoding = false;
        vp.audio_volume = 0.0f;
    }

    // Activate closest videos within budget
    double cumulative_mps = 0.0;
    int active_count = 0;
    for (auto& info : infos) {
        auto& vp = app.video_players[info.vp_idx];
        if (!vp.active) continue;

        if (active_count >= VIDEO_MAX_ACTIVE) break;
        if (info.dist > VIDEO_MAX_DISTANCE && active_count >= VIDEO_MIN_ACTIVE) break;
        if (cumulative_mps + vp.mp_per_sec > VIDEO_BUDGET_MPS && active_count >= VIDEO_MIN_ACTIVE) break;

        vp.decoding = true;
        cumulative_mps += vp.mp_per_sec;
        active_count++;
    }

    // Resync timing for videos transitioning paused → decoding
    for (int i = 0; i < (int)app.video_players.size(); i++) {
        auto& vp = app.video_players[i];
        if (vp.decoding && !was_decoding[i] && vp.active) {
            vp.play_start = App::VideoPlayer::VClock::now()
                          - std::chrono::duration_cast<App::VideoPlayer::VClock::duration>(
                                std::chrono::duration<double>(vp.cur_pts));
        }
    }

    // Audio selection: closest decoding video with audio that passes cone test
    int best_audio = -1;
    float best_vol = 0.0f;
    for (auto& info : infos) {
        auto& vp = app.video_players[info.vp_idx];
        if (!vp.decoding || vp.audio_dev == 0) continue;
        if (info.dist > AUDIO_MAX_DISTANCE) break; // sorted by distance, no point continuing

        float vol = compute_audio_volume(cam.x, cam.y, cam.z,
                                         info.nx, info.ny, info.nz,
                                         info.wx, info.wy, info.wz, info.dist);
        if (vol > best_vol) {
            best_vol = vol;
            best_audio = info.vp_idx;
        }
    }

    // Apply audio state
    for (int i = 0; i < (int)app.video_players.size(); i++) {
        auto& vp = app.video_players[i];
        if (!vp.active || vp.audio_dev == 0) continue;
        if (i == best_audio) {
            vp.audio_volume = best_vol;
            SDL_PauseAudioDevice(vp.audio_dev, 0);
        } else {
            if (vp.audio_volume > 0.0f) {
                SDL_ClearQueuedAudio(vp.audio_dev);
            }
            vp.audio_volume = 0.0f;
            SDL_PauseAudioDevice(vp.audio_dev, 1);
        }
    }
}
