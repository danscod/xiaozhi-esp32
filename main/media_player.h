#pragma once

#include <atomic>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * MediaPlayer — general-purpose media playback with MCP tool integration.
 *
 * Exposes four MCP tools to the AI backend:
 *   self.media.search  — search the server library with a natural-language query
 *   self.media.play    — start playback of a library item by ID
 *   self.media.stop    — stop current playback
 *   self.media.status  — report current playback state
 *
 * Phase 1: audio-only (OGG Opus served from the media library).
 * Phase 2: MJPEG video will be added as an additional media type.
 *
 * The library lives on the server — the firmware never hardcodes content.
 * The AI discovers content via search and drives playback through the tools.
 */
class MediaPlayer {
public:
    static MediaPlayer& GetInstance();

    // Called once from McpServer constructor to register the four tools.
    void RegisterMcpTools();

    // Playback states visible to MCP status tool.
    enum class State { kIdle, kLoading, kPlaying, kError };
    State GetState() const { return state_; }

    // Stop playback from any context (e.g. hardware button).
    void Stop();

    // Fetch the play URL for an item ID, then kick off the stream task. Returns
    // an error string if the item isn't found. (Also used by the demo sequencer.)
    std::string StartItem(const std::string& item_id);

    // Soft pause: freeze audio output without closing the HTTP stream.
    // Safe to call from any context.
    void TogglePause();
    bool IsPaused() const { return paused_.load(); }

private:
    MediaPlayer() = default;
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // Fetch search results from the server and return them as a JSON string
    // the AI can read. Called synchronously inside the MCP callback.
    std::string FetchSearchResults(const std::string& query);

    // Stop any in-flight stream task and reset state.
    void StopPlayback();

    // Update the device display to show now-playing info.
    void UpdateDisplay();

    // FreeRTOS task: downloads an OGG file and feeds it to AudioService.
    static void StreamTask(void* arg);

    // --- state ---
    State       state_       = State::kIdle;
    std::string current_id_;
    std::string current_title_;
    std::string stream_url_;
    std::string error_msg_;

    TaskHandle_t stream_task_handle_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};

    static constexpr const char* kSearchUrl =
        "http://iot.danscodellaro.com/esp32/xiaozhi/media/search";

};
