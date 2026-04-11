#pragma once

#include "device_state.h"
#include <string>
#include <functional>
#include <esp_timer.h>

struct cJSON;

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
    void PostVideoPlaybackStats(
        const std::string& item_id,
        const std::string& title,
        int duration_ms,
        int rendered_frames,
        int dropped_frames,
        int http_read_stalls,
        int max_http_read_stall_ms,
        int audio_push_block_count,
        int max_audio_push_block_ms,
        int late_frame_count,
        int max_frame_late_ms);
    void PostVideoPlaybackProgress(
        const std::string& item_id,
        const std::string& title,
        int duration_ms,
        int rendered_frames,
        int dropped_frames,
        int http_read_stalls,
        int max_http_read_stall_ms,
        int audio_push_block_count,
        int max_audio_push_block_ms,
        int late_frame_count,
        int max_frame_late_ms,
        int queued_video_frames);

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

    int  conversation_count_  = 0;
    int  wake_word_count_     = 0;
    bool in_conversation_     = false;
    int64_t conv_start_us_    = 0;
    esp_timer_handle_t heartbeat_timer_ = nullptr;

    static constexpr const char* kUrl =
        "https://www.danscodellaro.com/esp32/xiaozhi/telemetry";
    static constexpr int kHeartbeatIntervalUs = 5 * 60 * 1000000; // 5 minutes
};
