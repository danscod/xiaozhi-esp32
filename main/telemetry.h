#pragma once

#include "device_state.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <esp_timer.h>

class WebSocket;

struct cJSON;

struct VideoPlaybackTelemetry {
    std::string item_id;
    std::string title;
    std::string end_reason;
    int duration_ms = 0;
    int queued_video_frames = 0;
    int rendered_frames = 0;
    int dropped_frames = 0;
    int queue_overflow_drop_count = 0;
    int render_backlog_drop_count = 0;
    int max_queued_video_frames = 0;
    int http_status_code = 0;
    int stream_resume_count = 0;
    int stream_resume_failure_count = 0;
    int http_read_calls = 0;
    int http_read_short_calls = 0;
    int http_zero_reads = 0;
    int http_header_read_calls = 0;
    int http_payload_read_calls = 0;
    int http_read_stalls = 0;
    int audio_packets_seen = 0;
    int video_frames_seen = 0;
    int video_frames_decode_attempted = 0;
    int video_frames_decode_failed = 0;
    int video_frames_presented = 0;
    int render_wakeups = 0;
    int render_empty_queue_wakeups = 0;
    int max_http_read_stall_ms = 0;
    int max_http_read_ms = 0;
    int max_audio_push_block_ms = 0;
    int max_audio_push_ms = 0;
    int max_audio_packet_copy_ms = 0;
    int max_audio_output_write_ms = 0;
    int max_video_frame_copy_ms = 0;
    int max_render_queue_wait_ms = 0;
    int max_render_schedule_sleep_ms = 0;
    int max_frame_late_ms = 0;
    int max_frame_age_before_decode_ms = 0;
    int max_frame_age_after_present_ms = 0;
    int max_jpeg_decode_ms = 0;
    int max_frame_present_ms = 0;
    int http_open_time_ms = -1;
    int first_audio_packet_ms = -1;
    int first_video_frame_ms = -1;
    int playback_started_ms = -1;
    int first_frame_presented_ms = -1;
    int audio_push_block_count = 0;
    int late_frame_count = 0;
    int64_t http_read_bytes = 0;
    int64_t expected_stream_bytes = 0;
    int64_t audio_bytes_seen = 0;
    int64_t video_bytes_seen = 0;
    int64_t http_read_time_us_total = 0;
    int64_t header_read_time_us_total = 0;
    int64_t payload_read_time_us_total = 0;
    int64_t audio_packet_copy_time_us_total = 0;
    int64_t audio_output_samples_written = 0;
    int64_t audio_output_write_time_us_total = 0;
    int64_t video_frame_copy_time_us_total = 0;
    int64_t audio_push_time_us_total = 0;
    int64_t render_queue_wait_us_total = 0;
    int64_t render_schedule_sleep_us_total = 0;
    int64_t total_frame_late_us = 0;
    int64_t total_frame_age_before_decode_us = 0;
    int64_t total_frame_age_after_present_us = 0;
    int64_t jpeg_decode_time_us_total = 0;
    int64_t frame_present_time_us_total = 0;
    int audio_output_calls = 0;
    int audio_output_underrun_count = 0;
    // Per-core CPU utilization over the playback window (0-100).
    // -1 means "not measured" (e.g., snapshot capture failed).
    int cpu_core0_busy_pct = -1;
    int cpu_core1_busy_pct = -1;
    // Most CPU-heavy non-idle task during the window, and its share.
    std::string cpu_top_task_name;
    int         cpu_top_task_pct = -1;
    int         cpu_top_task_core = -1;
};

/**
 * Telemetry — posts device metrics to www.danscodellaro.com/esp32/xiaozhi/telemetry
 *
 * Events posted:
 *   boot              — once after activation completes
 *   heartbeat         — every 5 minutes while idle
 *   conversation_start — when device transitions to connecting/listening
 *   conversation_end   — when device returns to idle from a conversation
 *   wake_word         — each time the wake word fires
 *   video_playback_progress — periodic during active video playback
 *   video_playback_end — emitted when video playback stops, with transport/render stats
 */
class Telemetry {
public:
    static Telemetry& GetInstance();

    // Call once after activation completes (posts a "boot" event, starts heartbeat timer).
    void Init();

    // Call from Application::HandleStateChangedEvent().
    void OnStateChanged(DeviceState new_state);

    // Call from Application::HandleWakeWordDetectedEvent().
    void OnWakeWord(const std::string& wake_word);

    // Call from Application clock tick handler (every 5 min).
    void PostHeartbeat();
    void PostVideoPlaybackStats(const VideoPlaybackTelemetry& telemetry);
    void PostVideoPlaybackProgress(const VideoPlaybackTelemetry& telemetry);

    // Try to send a video playback summary over the persistent telemetry WS
    // ONLY (no HTTP fallback). Returns true on successful WS send. On false,
    // the caller is responsible for buffering the data (e.g., NVS-deferred
    // until next boot). Used by VideoPlayer at end-of-playback to avoid
    // creating an HttpClient during a heap-fragile moment.
    bool TrySendVideoPlaybackStats(const VideoPlaybackTelemetry& telemetry);

private:
    Telemetry() = default;
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    // Spawns a short-lived FreeRTOS task to POST without blocking the main loop.
    void PostEventAsync(const char* event_type, int conversation_duration_ms = 0,
                        std::function<void(cJSON* root)> extra_fields = {});

    // Builds the JSON payload string (heap-allocated, caller must free).
    char* BuildJson(const char* event_type, int conversation_duration_ms,
                    const std::function<void(cJSON* root)>& extra_fields = {});

    // Lazy-opens the persistent telemetry WebSocket. Returns true if the WS
    // is connected after the call. Safe to call from any task.
    bool EnsureTelemetryWs();

    // Sends one event payload over the WS as a text frame. Returns true on
    // successful send; false means the WS is unavailable or send failed (in
    // which case PostEventAsync falls back to the legacy HTTP path).
    bool TrySendViaWs(const char* json_payload);

    // Build the telemetry WS URL from Settings (namespace "net", key
    // "ws_base"). Falls back to the compiled-in default. Allows the server
    // to migrate to a new public IP by returning ws_base_url in the OTA
    // response (parsed by Ota::CheckVersion).
    std::string GetTelemetryWsUrl() const;

    int  conversation_count_  = 0;
    int  wake_word_count_     = 0;
    bool in_conversation_     = false;
    int64_t conv_start_us_    = 0;
    esp_timer_handle_t heartbeat_timer_ = nullptr;

    std::unique_ptr<WebSocket> telemetry_ws_;
    std::mutex telemetry_ws_mutex_;

    static constexpr const char* kUrl =
        "http://iot.danscodellaro.com/esp32/xiaozhi/telemetry";
    // Default WS base; can be overridden via the OTA response's ws_base_url
    // field, which is stored in NVS namespace "net", key "ws_base".
    static constexpr const char* kDefaultWsBase =
        "ws://54.253.179.134";
    static constexpr const char* kTelemetryWsPath =
        "/esp32/xiaozhi/telemetry/ws";
    static constexpr int kHeartbeatIntervalUs = 5 * 60 * 1000000; // 5 minutes
};
