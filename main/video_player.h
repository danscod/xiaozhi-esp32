#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "system_info.h"

#include "telemetry.h"

/**
 * VideoPlayer — MJPEG+Opus video playback with MCP tool integration.
 *
 * Streams a pre-muxed binary container (.axv) over HTTP.
 * Video frames are JPEG decoded and rendered to the LVGL display.
 * Audio frames are Opus packets pushed to AudioService.
 *
 * Binary frame format (16-byte header per frame):
 *   [4] magic   = 0xDEADBEEF
 *   [4] type    = 0 (video/JPEG) or 1 (audio/Opus)
 *   [4] length  = payload byte count
 *   [4] ts_ms   = presentation timestamp (ms)
 *   [length bytes] payload
 *
 * Target spec: 240×240 JPEG @ 8 fps, Opus @ 24kHz mono, 60 ms frames.
 *
 * Thread safety:
 *   - Start() / Stop() acquire the display LVGL lock when modifying screen objects.
 *   - stop_requested_ is atomic — safe to set from any task / button callback.
 *   - StreamReaderTask reads/demuxes the HTTP .axv stream.
 *   - VideoRenderTask decodes JPEGs and updates LVGL.
 */
class VideoPlayer {
public:
    static VideoPlayer& GetInstance();

    // Register self.video.* MCP tools. Called from McpServer::AddCommonTools().
    void RegisterMcpTools();

    // Start playback for a library item by ID. Used by self.video.play and by
    // the generic media tools when they resolve an item of type "video".
    std::string PlayItem(const std::string& item_id);

    enum class State { kIdle, kLoading, kPlaying, kError };
    State GetState() const { return state_; }
    const std::string& GetCurrentId() const { return current_id_; }
    const std::string& GetCurrentTitle() const { return current_title_; }
    const std::string& GetError() const { return error_msg_; }

    // Stop playback from any context (button callback, MCP tool, other player).
    void Stop();

    // Start playback of a library item by id (also used by the demo sequencer).
    std::string StartItem(const std::string& item_id);

    // Soft pause: freeze video frames and silence audio without closing the stream.
    void TogglePause();
    bool IsPaused() const { return paused_.load(); }

private:
    VideoPlayer() = default;
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    std::string FetchSearchResults(const std::string& query);
    void StopPlayback();
    void CreateVideoScreen();
    void DestroyVideoScreen();
    void UpdateDisplay();
    void MaybePostPlaybackProgress();
    void MaybeFinishPlayback();
    VideoPlaybackTelemetry BuildTelemetrySnapshot(int duration_ms, int queued_video_frames) const;
    void SetPlaybackEndReason(const char* reason);

    struct QueuedVideoFrame {
        uint32_t ts_ms = 0;
        std::vector<uint8_t> jpeg;
    };

    struct PlaybackStats {
        int64_t start_us = 0;
        size_t rendered_frames = 0;
        size_t dropped_frames = 0;
        size_t queue_overflow_drop_count = 0;
        size_t render_backlog_drop_count = 0;
        size_t max_queued_video_frames = 0;
        int http_status_code = 0;
        size_t stream_resume_count = 0;
        size_t stream_resume_failure_count = 0;
        int64_t expected_stream_bytes = 0;
        size_t http_read_calls = 0;
        size_t http_read_short_calls = 0;
        size_t http_zero_reads = 0;
        size_t http_header_read_calls = 0;
        size_t http_payload_read_calls = 0;
        size_t http_read_stalls = 0;
        int64_t http_read_bytes = 0;
        int64_t http_read_time_us_total = 0;
        int64_t header_read_time_us_total = 0;
        int64_t payload_read_time_us_total = 0;
        int64_t http_open_time_us = 0;
        int64_t max_http_read_us = 0;
        int64_t max_http_read_stall_us = 0;
        size_t audio_packets_seen = 0;
        int64_t audio_bytes_seen = 0;
        int64_t audio_packet_copy_time_us_total = 0;
        int64_t max_audio_packet_copy_us = 0;
        size_t audio_push_block_count = 0;
        int64_t audio_push_time_us_total = 0;
        int64_t max_audio_push_us = 0;
        int64_t max_audio_push_block_us = 0;
        size_t audio_output_calls = 0;
        size_t audio_output_underrun_count = 0;
        int64_t audio_output_samples_written = 0;
        int64_t audio_output_write_time_us_total = 0;
        int64_t max_audio_output_write_us = 0;
        size_t video_frames_seen = 0;
        int64_t video_bytes_seen = 0;
        int64_t video_frame_copy_time_us_total = 0;
        int64_t max_video_frame_copy_us = 0;
        size_t video_frames_decode_attempted = 0;
        size_t video_frames_decode_failed = 0;
        size_t video_frames_presented = 0;
        size_t render_wakeups = 0;
        size_t render_empty_queue_wakeups = 0;
        int64_t render_queue_wait_us_total = 0;
        int64_t max_render_queue_wait_us = 0;
        int64_t render_schedule_sleep_us_total = 0;
        int64_t max_render_schedule_sleep_us = 0;
        size_t late_frame_count = 0;
        int64_t total_frame_late_us = 0;
        int64_t max_frame_late_us = 0;
        int64_t total_frame_age_before_decode_us = 0;
        int64_t max_frame_age_before_decode_us = 0;
        int64_t total_frame_age_after_present_us = 0;
        int64_t max_frame_age_after_present_us = 0;
        int64_t jpeg_decode_time_us_total = 0;
        int64_t max_jpeg_decode_us = 0;
        int64_t frame_present_time_us_total = 0;
        int64_t max_frame_present_us = 0;
        int64_t first_audio_packet_us = 0;
        int64_t first_video_frame_us = 0;
        int64_t playback_started_us = 0;
        int64_t first_frame_presented_us = 0;
        // CPU snapshot captured at playback start. End-of-playback diff
        // yields the per-core busy % over the playback window. See
        // SystemInfo::GetCoreCpuStats / CoreCpuStats.
        struct CpuSnapshot {
            bool     valid = false;
            uint64_t total_runtime = 0;
            uint64_t core0_run_time = 0;
            uint64_t core1_run_time = 0;
            uint64_t core0_idle_run_time = 0;
            uint64_t core1_idle_run_time = 0;
        };
        CpuSnapshot cpu_start;
        // Per-task runtime snapshot at playback start. End-of-playback
        // diff yields the single top CPU consumer DURING the playback
        // window (not since boot). 48 slots covers any realistic task
        // count; total ~1.2 KB stack/heap.
        static constexpr size_t kMaxTaskSnapshotEntries = 48;
        size_t cpu_start_task_count = 0;
        // The actual storage is allocated on heap via cpu_start_tasks_buf;
        // header keeps the count and the buf pointer in PlaybackStats.
        // (Defined inline as a fixed array to avoid lifecycle issues.)
        SystemInfo::TaskRunTimeEntry cpu_start_tasks[kMaxTaskSnapshotEntries];
    };

    static void StreamReaderTask(void* arg);
    static void VideoRenderTask(void* arg);

    // Playback state
    State        state_       = State::kIdle;
    std::string  current_id_;
    std::string  current_title_;
    std::string  stream_url_;
    std::string  sync_frame_url_;
    std::string  sync_audio_url_;
    std::string  ws_stream_url_;
    std::string  error_msg_;
    bool         use_sync_media_api_ = false;
    bool         use_ws_media_api_ = false;
    int          current_duration_ms_ = 0;
    int          sync_audio_packet_ms_ = 60;
    int          sync_audio_batch_packets_ = 8;
    int          sync_video_batch_frames_ = 16;
    int          sync_frame_lead_ms_ = 2500;

    TaskHandle_t stream_task_handle_ = nullptr;
    TaskHandle_t render_task_handle_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> reader_done_{false};
    std::atomic<bool> playback_started_{false};
    std::atomic<bool> render_failed_{false};

    std::mutex video_queue_mutex_;
    std::condition_variable video_queue_cv_;
    std::deque<std::unique_ptr<QueuedVideoFrame>> video_queue_;
    mutable std::mutex playback_stats_mutex_;
    int64_t playback_start_us_ = 0;
    size_t dropped_frames_ = 0;
    PlaybackStats playback_stats_;
    std::string playback_end_reason_;
    bool playback_stats_reported_ = false;
    std::atomic<int64_t> last_progress_post_us_{0};

    // PSRAM framebuffers — allocated in VideoStreamTask, freed in StopPlayback.
    uint8_t* frame_buf_a_ = nullptr;   // currently on display
    uint8_t* frame_buf_b_ = nullptr;   // being decoded into
    bool     buf_a_is_display_ = true;

    // LVGL handles
    void* video_screen_   = nullptr;   // lv_obj_t* — full-screen black bg
    void* video_img_obj_  = nullptr;   // lv_obj_t* (lv_image) — 240×240
    void* prev_screen_    = nullptr;   // lv_obj_t* — restored on exit

    static constexpr const char* kSearchUrl =
        "http://iot.danscodellaro.com/esp32/xiaozhi/media/search";

    static constexpr uint32_t kFrameMagic    = 0xDEADBEEFu;
    static constexpr uint32_t kFrameVideo    = 0u;
    static constexpr uint32_t kFrameAudio    = 1u;
    static constexpr size_t   kFrameW        = 240;
    static constexpr size_t   kFrameH        = 240;
    static constexpr size_t   kFrameBytes    = kFrameW * kFrameH * 2;  // RGB565
    static constexpr size_t   kJpegBufSize   = 32 * 1024;              // 32 KB max JPEG
    // The queue needs to cover the server's lead (~10 s) at the highest
    // expected source frame rate. At 8 fps that's 80 frames; at 24 fps
    // that's 240. 320 covers up to 13 s of 24 fps video with margin,
    // costs ~3 MB of transient PSRAM at peak (well within 8 MB available).
    // (Was 40 originally, then 120 when 8 fps was max; bumped for 24 fps.)
    static constexpr size_t   kMaxQueuedVideoFrames = 320;
    static constexpr uint32_t kStreamTaskStackWords = 8192;            // 32 KB for sync_v1 single-task decode/render
    static constexpr uint32_t kRenderTaskStackWords = 3072;            // 12 KB
    static constexpr size_t   kAudioPrebufferPackets = 4;              // 240 ms
    static constexpr int      kMaxStreamResumeAttempts = 6;
    static constexpr int      kStreamResumeBackoffMs = 250;
    static constexpr int64_t  kFrameSelectionLeadUs = 50000;           // prefer the newest frame due within 50 ms
    static constexpr int64_t  kFutureFrameRecheckUs = 40000;           // recheck pacing every 40 ms instead of long sleeps
    static constexpr int64_t  kLateFrameDropUs = 500000;               // allow a wider sync window
    static constexpr int64_t  kRenderStallResyncUs = 1500000;          // if nothing renders for 1.5 s, re-anchor video timing
    static constexpr int64_t  kResyncLeadUs = 20000;                   // re-enter playback slightly ahead of "now"
    static constexpr int64_t  kHttpReadStallWarnUs = 80000;            // >80 ms read gap
    static constexpr int64_t  kAudioPushBlockWarnUs = 20000;           // >20 ms queue wait
    static constexpr int64_t  kPlaybackProgressIntervalUs = 5000000;   // 5 s
};
