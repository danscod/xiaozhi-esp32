#include "doom_player.h"

#include <esp_log.h>

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"

// PrBoom C entry points (declared in components/doom/prboom-esp32-compat/).
extern "C" {
    void  spi_lcd_init(void);
    void  xiaozhi_doom_set_panel(void* panel_handle);
    void  xiaozhi_doom_set_wad(const void* buf, size_t len);
    int   doom_main(int argc, char const* const* argv);
    void  gamepadInit(void);
}

#define TAG "DoomPlayer"

DoomPlayer& DoomPlayer::GetInstance() {
    static DoomPlayer instance;
    return instance;
}

std::string DoomPlayer::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (state_.load() != State::kIdle) {
        return std::string("DOOM already running.");
    }

    auto* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        error_msg_ = "Display not available.";
        state_.store(State::kError);
        return std::string("Failed: ") + error_msg_;
    }

    // Hand the panel to the C side BEFORE spi_lcd_init walks it.
    // The display layer exposes the raw esp_lcd_panel_handle_t via
    // GetPanelHandle() — see display.h. We claim the panel here; LVGL must
    // not draw into it while DOOM is active.
    void* panel = display->GetPanelHandle();
    if (panel == nullptr) {
        error_msg_ = "Panel handle unavailable.";
        state_.store(State::kError);
        return std::string("Failed: ") + error_msg_;
    }
    xiaozhi_doom_set_panel(panel);

    // Locate doom1.wad in the assets partition. The build pipeline stages
    // it via DEFAULT_ASSETS_EXTRA_FILES; at runtime it's a memory pointer
    // into the mmap'd region — no copy, no I/O.
    void* wad_ptr = nullptr;
    size_t wad_size = 0;
    if (!Assets::GetInstance().GetAssetData("doom1.wad", wad_ptr, wad_size) ||
        wad_ptr == nullptr || wad_size < 12) {
        error_msg_ = "doom1.wad not found in assets partition.";
        state_.store(State::kError);
        xiaozhi_doom_set_panel(nullptr);
        return std::string("Failed: ") + error_msg_;
    }
    ESP_LOGI(TAG, "WAD located: %p, %u bytes", wad_ptr, (unsigned)wad_size);
    xiaozhi_doom_set_wad(wad_ptr, wad_size);

    // Suspend audio/voice subsystems so DOOM has the device to itself.
    // Existing helper used by VideoPlayer for the same purpose.
    Application::GetInstance().EndVoiceSessionForMedia();

    // Take the display lock for the whole session — blocks LVGL refreshes
    // so they don't overwrite DOOM's frames. Released in Stop().
    display_lock_ = std::make_unique<DisplayLockGuard>(display);

    stop_requested_.store(false);
    state_.store(State::kRunning);

    BaseType_t ok = xTaskCreatePinnedToCore(
        &DoomPlayer::EngineTask,
        "doom_engine",
        kEngineTaskStackBytes,
        this,
        kEngineTaskPriority,
        &engine_task_handle_,
        kEngineTaskCore);
    if (ok != pdPASS) {
        error_msg_ = "Failed to spawn DOOM engine task.";
        state_.store(State::kError);
        xiaozhi_doom_set_panel(nullptr);
        return std::string("Failed: ") + error_msg_;
    }

    ESP_LOGI(TAG, "DOOM engine task spawned on core %d", (int)kEngineTaskCore);
    return std::string("DOOM started.");
}

void DoomPlayer::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    if (state_.load() == State::kIdle) {
        return;
    }

    stop_requested_.store(true);

    // PrBoom is single-threaded C with no cooperative shutdown path. Killing
    // the task from outside leaks whatever PrBoom allocated (mostly on the
    // PSRAM heap), but: a) the buffer-backed I/O has no real file handles,
    // b) the display adapter's row buffers are small (~960 B), c) Start()
    // can be called again later — PrBoom's z_zone allocator re-bootstraps.
    // For a v1 attract-mode toy, that trade is fine.
    if (engine_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Killing DOOM engine task");
        vTaskDelete(engine_task_handle_);
        engine_task_handle_ = nullptr;
    }

    xiaozhi_doom_set_panel(nullptr);
    xiaozhi_doom_set_wad(nullptr, 0);
    display_lock_.reset();  // Releases the lock; LVGL refreshes resume.
    state_.store(State::kIdle);
    ESP_LOGI(TAG, "DOOM stopped");
}

void DoomPlayer::EngineTask(void* arg) {
    auto* self = static_cast<DoomPlayer*>(arg);
    (void)self;

    ESP_LOGI(TAG, "DOOM engine starting");
    spi_lcd_init();
    gamepadInit();

    // Argv: zero args → PrBoom auto-discovers the IWAD via I_Open, which
    // we've intercepted to return our buffer. The title screen then triggers
    // built-in DEMO1/2/3 attract-mode playback on the timeout.
    char const* argv[] = { "doom", nullptr };
    doom_main(1, argv);

    ESP_LOGI(TAG, "DOOM engine returned (this is unexpected for attract mode)");
    vTaskDelete(nullptr);
}

void DoomPlayer::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.doom.start",
        "Start DOOM in attract mode (the built-in demo lumps from the WAD "
        "play on a loop). Display switches to the game; audio and chat are "
        "suspended. Use self.doom.stop or long-press the button to exit.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            (void)props;
            return Start();
        });

    mcp.AddTool(
        "self.doom.stop",
        "Stop DOOM and return to the normal interface.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            (void)props;
            if (state_.load() == State::kIdle) {
                return std::string("DOOM is not running.");
            }
            Stop();
            return std::string("Stopped.");
        });

    mcp.AddTool(
        "self.doom.status",
        "Get DOOM playback status.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            (void)props;
            switch (state_.load()) {
                case State::kRunning: return std::string("running");
                case State::kError:   return std::string("error: ") + error_msg_;
                default:              return std::string("idle");
            }
        });
}
