#pragma once

#include "device_state.h"
#include <string>
#include <esp_timer.h>

/**
 * Telemetry — posts device metrics to www.danscodellaro.com/esp32/xiaozhi/telemetry
 *
 * Events posted:
 *   boot              — once after activation completes
 *   heartbeat         — every 5 minutes while idle
 *   conversation_start — when device transitions to connecting/listening
 *   conversation_end   — when device returns to idle from a conversation
 *   wake_word         — each time the wake word fires
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

private:
    Telemetry() = default;
    Telemetry(const Telemetry&) = delete;
    Telemetry& operator=(const Telemetry&) = delete;

    // Spawns a short-lived FreeRTOS task to POST without blocking the main loop.
    void PostEventAsync(const char* event_type, int conversation_duration_ms = 0);

    // Builds the JSON payload string (heap-allocated, caller must free).
    char* BuildJson(const char* event_type, int conversation_duration_ms);

    int  conversation_count_  = 0;
    int  wake_word_count_     = 0;
    bool in_conversation_     = false;
    int64_t conv_start_us_    = 0;
    bool posting_             = false;   // guard: skip if previous POST still in flight

    esp_timer_handle_t heartbeat_timer_ = nullptr;

    static constexpr const char* kUrl =
        "https://www.danscodellaro.com/esp32/xiaozhi/telemetry";
    static constexpr int kHeartbeatIntervalUs = 5 * 60 * 1000000; // 5 minutes
};
