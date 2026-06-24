// Game mode flag. See game_mode.h.
#include "game_mode.h"

#include <nvs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#define TAG "GameMode"
#define NS  "gamemode"
#define KEY "on"

static bool s_active = false;

void game_mode_init_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, KEY, &v) == ESP_OK && v) {
        s_active = true;
        // One-shot: clear immediately so a crash in the game-mode path can't
        // trap the device — the next boot will be normal.
        nvs_set_u8(h, KEY, 0);
        nvs_commit(h);
        ESP_LOGW(TAG, "GAME MODE this boot: BLE controller + DOOM, no WiFi/assistant");
    }
    nvs_close(h);
}

bool game_mode_active(void) { return s_active; }

void game_mode_request_and_reboot(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "Rebooting into game mode (controller)...");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
