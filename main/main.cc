#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_bt.h>

#include "application.h"
#include "game_mode.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Read (+ clear) the game-mode flag set by "play doom with a controller".
    game_mode_init_from_nvs();

    if (game_mode_active()) {
        // GAME MODE: keep the BLE controller memory (we need BLE for the gamepad).
        // WiFi/assistant are skipped in Application::Initialize to free the RAM.
        ESP_LOGW(TAG, "game mode: keeping BLE controller memory for the gamepad");
    } else {
        // NORMAL MODE: release the BLE controller's reserved internal RAM back to
        // the heap (~tens of KB) so the assistant/audio path isn't starved. The
        // release is one-way — BLE is unusable until a reboot (into game mode).
        esp_err_t bt_rel = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        ESP_LOGI(TAG, "normal mode: released BLE controller mem (%s)", esp_err_to_name(bt_rel));
    }

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
