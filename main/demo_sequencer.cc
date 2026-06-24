// Self-test / demo sequencer. See demo_sequencer.h.
#include "demo_sequencer.h"

#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "mcp_server.h"
#include "media_player.h"
#include "video_player.h"
#include "flappy_bird.h"
#include "doom_player.h"

#define TAG "DemoTest"

static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stop{false};
static TaskHandle_t      s_task = nullptr;

// One-line device-health snapshot — the meat of the serial capture.
static void log_health(const char* phase) {
    ESP_LOGI(TAG, "[%s] internal: free=%u min=%u largest=%u | psram: free=%u min=%u",
             phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

// Sleep in small slices so a stop request is honoured promptly. Returns false if
// a stop was requested during the wait.
static bool dwell(int ms) {
    for (int t = 0; t < ms && !s_stop.load(); t += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !s_stop.load();
}

static void banner(int n, const char* name) {
    ESP_LOGI(TAG, "===== STEP %d: %s =====", n, name);
    log_health("before");
}

static void after_step(const char* name) {
    (void)name;
    log_health("after");
    vTaskDelay(pdMS_TO_TICKS(1500));   // settle + let memory free between activities
}

static void demo_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "##### SELF-TEST / DEMO SEQUENCE START #####");
    log_health("boot");

    // 1) Music (audio output path).
    if (s_stop.load()) goto done;
    banner(1, "Music — The Simpsons Theme (10s)");
    MediaPlayer::GetInstance().StartItem("simpsons_theme");
    dwell(10000);
    MediaPlayer::GetInstance().Stop();
    after_step("music");

    // 2) Video (decode + render + synced audio).
    if (s_stop.load()) goto done;
    banner(2, "Video — Marge vs. the Monorail (15s)");
    VideoPlayer::GetInstance().StartItem("simpsons_s04e12_24fps");
    dwell(15000);
    VideoPlayer::GetInstance().Stop();
    after_step("video");

    // 3) Flappy Bird (LVGL game).
    if (s_stop.load()) goto done;
    banner(3, "Flappy Bird (10s)");
    FlappyBird::GetInstance().Start();
    dwell(10000);
    FlappyBird::GetInstance().Stop();
    after_step("flappy");

    // 4) DOOM (engine + OPL music + SFX, demo/attract).
    if (s_stop.load()) goto done;
    banner(4, "DOOM — attract demo (20s)");
    DoomPlayer::GetInstance().Start();
    dwell(20000);
    DoomPlayer::GetInstance().Stop();
    after_step("doom");

done:
    ESP_LOGI(TAG, "##### SELF-TEST / DEMO SEQUENCE %s #####",
             s_stop.load() ? "ABORTED" : "COMPLETE");
    log_health("end");
    s_running.store(false);
    s_stop.store(false);
    s_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void demo_sequencer_register_mcp(void) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.test.run",
        "Run the device self-test / demo sequence: plays music, then a video, then "
        "Flappy Bird, then DOOM, one after another, ~10-20s each, logging device "
        "health (free RAM) between each over serial. Use to demo the device or "
        "capture a full regression snapshot. Use self.test.stop to abort.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (s_running.exchange(true)) {
                return std::string("Self-test is already running.");
            }
            s_stop.store(false);
            // PSRAM stack — this task only orchestrates; the activities have their
            // own tasks. Pinned to core 0 (app/normal side).
            xTaskCreatePinnedToCoreWithCaps(demo_task, "demo_test", 6144, nullptr, 4,
                                            &s_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            return std::string("Starting the self-test sequence — watch the screen.");
        });

    mcp.AddTool(
        "self.test.stop",
        "Abort the running device self-test / demo sequence.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (!s_running.load()) return std::string("No self-test is running.");
            s_stop.store(true);
            return std::string("Stopping the self-test.");
        });
}
