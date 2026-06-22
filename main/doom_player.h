#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class DisplayLockGuard;

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * DoomPlayer — MCP-driven attract-mode DOOM playback.
 *
 * Hosts a PrBoom engine task that renders to the device's ST7789 panel.
 * No audio, no input (vanilla DOOM's built-in attract demos run on their
 * own from the title screen). Long-press on the button exits.
 *
 * Mirrors VideoPlayer's structural pattern (singleton + MCP tool + a single
 * dedicated FreeRTOS task) so the patterns the rest of the app uses to
 * coordinate teardown/restore apply here too.
 */
class DoomPlayer {
public:
    static DoomPlayer& GetInstance();

    void RegisterMcpTools();

    enum class State { kIdle, kRunning, kError };
    State GetState() const { return state_.load(); }
    const std::string& GetError() const { return error_msg_; }

    // Start DOOM. Returns a status string suitable for MCP reply.
    std::string Start();

    // Signal DOOM to exit and wait for the engine task to drain.
    void Stop();

private:
    DoomPlayer() = default;
    DoomPlayer(const DoomPlayer&) = delete;
    DoomPlayer& operator=(const DoomPlayer&) = delete;

    static void EngineTask(void* arg);

    std::atomic<State> state_{State::kIdle};
    std::atomic<bool>  stop_requested_{false};
    std::mutex         lifecycle_mutex_;
    TaskHandle_t       engine_task_handle_ = nullptr;
    std::string        error_msg_;
    // Held for the duration of the DOOM session. Blocks LVGL refreshes so
    // they don't overwrite the frames we DMA straight to the panel.
    std::unique_ptr<DisplayLockGuard> display_lock_;

    // PrBoom needs a generous stack. The task stack now lives in PSRAM
    // (xTaskCreatePinnedToCoreWithCaps) because internal SRAM is exhausted by
    // AFE+LVGL — a plain xTaskCreatePinnedToCore for 32KB internal silently
    // FAILED (spawn returned pdFAIL) so DOOM never started. 64KB in PSRAM is
    // cheap and gives prboom's deep render recursion headroom. (ESP-IDF stack
    // size is in BYTES.)
    static constexpr uint32_t kEngineTaskStackBytes = 65536;
    // Core 1: when DOOM is active LVGL and the video player are torn down,
    // so core 1 is the better choice (core 0 still hosts WiFi for the OTA
    // poll that we leave running).
    static constexpr BaseType_t kEngineTaskCore = 1;
    // Above LVGL (1) and audio (4), below display flush (5).
    static constexpr UBaseType_t kEngineTaskPriority = 3;
};
