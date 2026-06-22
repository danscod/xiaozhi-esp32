#include "doom_player.h"

#include <esp_log.h>

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "system_info.h"

#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps
#include <esp_heap_caps.h>
#include <vector>
#include "esp_lvgl_port.h"            // lvgl_port_stop / lvgl_port_resume (NOT the thread-bound lock)
#include "audio_codec.h"             // device codec for DOOM audio output

// PrBoom C entry points (declared in components/doom/prboom-esp32-compat/).
extern "C" {
    void  spi_lcd_init(void);
    void  xiaozhi_doom_set_panel(void* panel_handle);
    void  xiaozhi_doom_set_wad(const void* buf, size_t len);
    int   doom_main(int argc, char const* const* argv);
    void  gamepadInit(void);
}

#define TAG "DoomPlayer"

// ── DOOM audio bridge (C shim) ───────────────────────────────────────────────
// The DOOM sfx mixer (components/doom .../i_stubs_esp32.c) produces signed-16-bit
// mono PCM and hands it here; we feed the device's external audio codec. This
// replaces prboom's original ESP32 built-in-DAC output, which the S3 lacks.
extern "C" int xiaozhi_doom_audio_open(void) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return 0;
    }
    codec->EnableOutput(true);
    // Stop AudioService's power-save loop from disabling the codec output: DOOM
    // writes PCM straight to the codec (bypassing AudioService), so its
    // last_output_time_ never updates and the inactivity timeout would kill
    // DOOM audio ~1s in (observed: "Set output enable to false" at 82085).
    Application::GetInstance().GetAudioService().SetOutputKeepAlive(true);
    int rate = codec->output_sample_rate();
    ESP_LOGI(TAG, "DOOM audio: codec output rate = %d Hz", rate);
    return rate;
}

extern "C" void xiaozhi_doom_audio_write(const int16_t* pcm, int samples) {
    if (pcm == nullptr || samples <= 0) {
        return;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    std::vector<int16_t> buf(pcm, pcm + samples);
    codec->OutputData(buf);  // blocks on I2S DMA → paces the mixer task
}

extern "C" void xiaozhi_doom_audio_close(void) {
    // Re-allow normal power-save management of the codec output now that DOOM
    // is done (it will be disabled on the usual inactivity timeout).
    Application::GetInstance().GetAudioService().SetOutputKeepAlive(false);
}

// Fetch the DOOM soundtrack (pre-rendered raw s16 mono PCM at the codec rate,
// 24 kHz) into a PSRAM buffer. Returns the sample buffer + count via out params,
// or nullptr on failure. One-shot ~4.4 MB GET over the iot origin; called from
// the DOOM engine task at music start (blocking is fine — only delays the demo).
extern "C" int16_t* xiaozhi_doom_music_load(size_t* out_samples) {
    if (out_samples) *out_samples = 0;
    const char* url = "http://iot.danscodellaro.com/esp32/xiaozhi/media/files/doom_E1M1.pcm";
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());
    http->SetTimeout(20000);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "music: HTTP open failed");
        return nullptr;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "music: HTTP %d", http->GetStatusCode());
        http->Close();
        return nullptr;
    }
    size_t len = http->GetBodyLength();
    if (len < 2 || (len & 1)) {
        ESP_LOGE(TAG, "music: bad length %u", (unsigned)len);
        http->Close();
        return nullptr;
    }
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        ESP_LOGE(TAG, "music: PSRAM alloc %u failed", (unsigned)len);
        http->Close();
        return nullptr;
    }
    size_t got = 0;
    while (got < len) {
        int n = http->Read(reinterpret_cast<char*>(buf + got), len - got);
        if (n <= 0) break;
        got += n;
    }
    http->Close();
    if (got != len) {
        ESP_LOGE(TAG, "music: short read %u/%u", (unsigned)got, (unsigned)len);
        heap_caps_free(buf);
        return nullptr;
    }
    if (out_samples) *out_samples = len / 2;
    ESP_LOGI(TAG, "music: loaded %u samples (%.1f MB) to PSRAM",
             (unsigned)(len / 2), len / 1048576.0);
    return reinterpret_cast<int16_t*>(buf);
}

extern "C" void xiaozhi_doom_music_free(int16_t* buf) {
    if (buf) heap_caps_free(buf);
}

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

    // Stop the LVGL timer for the whole DOOM session so LVGL won't flush over
    // DOOM's direct-to-panel frames. We do NOT *hold* the LVGL port mutex across
    // the session (it's owner-thread-bound; Start runs on the MCP task, Stop on
    // the button task — cross-thread release deadlocked). lvgl_port_stop/resume
    // are plain timer controls callable from any task.
    //
    // ORDER MATTERS: take the lock FIRST, then stop. Acquiring the lock blocks
    // until the LVGL port task finishes any in-flight render (it renders while
    // holding this same lock), so rendering_in_progress is guaranteed false when
    // we stop the timer. Stopping BEFORE locking could freeze the timer
    // mid-render, leaving rendering_in_progress stuck true — then the next
    // lv_obj_invalidate from the main loop's status bar hits
    // LV_ASSERT_MSG(!rendering_in_progress) whose handler is while(1) → main
    // task hangs ~30s later (TWDT). Scoped guard = same-thread acquire+release.
    {
        DisplayLockGuard hold(display);
        lvgl_port_stop();
    }

    stop_requested_.store(false);
    state_.store(State::kRunning);

    // Stack in PSRAM: internal SRAM is exhausted by AFE+LVGL, so a plain
    // xTaskCreatePinnedToCore (internal stack) silently failed here and DOOM
    // never started. Self-deletes via vTaskDeleteWithCaps (below / in Stop).
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        &DoomPlayer::EngineTask,
        "doom_engine",
        kEngineTaskStackBytes,
        this,
        kEngineTaskPriority,
        &engine_task_handle_,
        kEngineTaskCore,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        error_msg_ = "Failed to spawn DOOM engine task.";
        state_.store(State::kError);
        xiaozhi_doom_set_panel(nullptr);
        lvgl_port_resume();  // we stopped it above — don't leave LVGL frozen
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
    // Best-effort: give the engine task a moment to fall out of any in-flight
    // esp_lcd_panel_draw_bitmap (which holds the SPI bus mutex) before we kill
    // it, so we don't leave the bus mutex held by a deleted task.
    if (engine_task_handle_ != nullptr) {
        ESP_LOGI(TAG, "Killing DOOM engine task");
        vTaskDelay(pdMS_TO_TICKS(40));
        vTaskDeleteWithCaps(engine_task_handle_);
        engine_task_handle_ = nullptr;
    }

    xiaozhi_doom_set_panel(nullptr);
    xiaozhi_doom_set_wad(nullptr, 0);
    // Resume the LVGL timer (any-task safe — see Start). Then repaint the normal
    // UI over DOOM's last frame on the LVGL task.
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        // Resume + repaint under the lock (same-thread scoped) so the timer
        // doesn't restart a render until LVGL state is consistent again.
        DisplayLockGuard hold(display);
        lvgl_port_resume();
        lv_obj_invalidate(lv_screen_active());
    } else {
        lvgl_port_resume();
    }
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
    vTaskDeleteWithCaps(nullptr);
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
