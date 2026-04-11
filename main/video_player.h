#pragma once

#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
 *   - VideoStreamTask runs on its own FreeRTOS task; all LVGL calls inside the
 *     task are bracketed by DisplayLockGuard.
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

    // Soft pause: freeze video frames and silence audio without closing the stream.
    void TogglePause();
    bool IsPaused() const { return paused_.load(); }

private:
    VideoPlayer() = default;
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    std::string FetchSearchResults(const std::string& query);
    std::string StartItem(const std::string& item_id);
    void StopPlayback();
    void CreateVideoScreen();
    void DestroyVideoScreen();
    void UpdateDisplay();

    static void VideoStreamTask(void* arg);

    // Playback state
    State        state_       = State::kIdle;
    std::string  current_id_;
    std::string  current_title_;
    std::string  stream_url_;
    std::string  error_msg_;

    TaskHandle_t stream_task_handle_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};

    // PSRAM framebuffers — allocated in VideoStreamTask, freed in StopPlayback.
    uint8_t* frame_buf_a_ = nullptr;   // currently on display
    uint8_t* frame_buf_b_ = nullptr;   // being decoded into
    bool     buf_a_is_display_ = true;

    // LVGL handles
    void* video_screen_   = nullptr;   // lv_obj_t* — full-screen black bg
    void* video_img_obj_  = nullptr;   // lv_obj_t* (lv_image) — 240×240
    void* prev_screen_    = nullptr;   // lv_obj_t* — restored on exit

    static constexpr const char* kSearchUrl =
        "https://www.danscodellaro.com/esp32/xiaozhi/media/search";

    static constexpr uint32_t kFrameMagic    = 0xDEADBEEFu;
    static constexpr uint32_t kFrameVideo    = 0u;
    static constexpr uint32_t kFrameAudio    = 1u;
    static constexpr size_t   kFrameW        = 240;
    static constexpr size_t   kFrameH        = 240;
    static constexpr size_t   kFrameBytes    = kFrameW * kFrameH * 2;  // RGB565
    static constexpr size_t   kJpegBufSize   = 32 * 1024;              // 32 KB max JPEG
};
