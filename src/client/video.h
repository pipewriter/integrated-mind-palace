#pragma once

#include "app.h"

// Decode budgeting constants
constexpr double VIDEO_BUDGET_MPS      = 300.0;   // max total megapixels/sec to decode
constexpr float  VIDEO_MAX_DISTANCE    = 200.0f;  // never decode beyond this
constexpr int    VIDEO_MIN_ACTIVE      = 5;       // always decode at least this many
constexpr int    VIDEO_MAX_ACTIVE      = 15;      // never decode more than this

// Proximity audio constants
constexpr float  AUDIO_CONE_HALF_ANGLE = 70.0f;   // degrees, wide cone in front of video
constexpr float  AUDIO_MAX_DISTANCE    = 40.0f;   // short range
constexpr float  AUDIO_ROLLOFF_START   = 5.0f;    // full volume up to this distance

// Open a media file and initialize decoding contexts
void open_media(App::VideoPlayer& vp, const char* path);

// Initialize audio output device for a video player
void init_video_audio(App::VideoPlayer& vp);

// Process one audio packet (decode + queue to SDL audio)
void process_audio_packet(App::VideoPlayer& vp);

// Decode one video frame. Returns false at end of stream.
bool decode_video_frame(App::VideoPlayer& vp);

// Seek video back to start for looping
void seek_video_to_start(App::VideoPlayer& vp);

// Create GPU texture + staging buffer for video frames
void create_video_texture(App::VideoPlayer& vp);

// Upload decoded frame from staging buffer to GPU texture
void upload_video_to_staging(App::VideoPlayer& vp);

// Clean up a video player's resources
void cleanup_video(App::VideoPlayer& vp);

// Start a new video player for a given file path
int start_video_player(const char* path);

// Update decode budgeting and audio selection based on camera proximity
void update_video_budget();
