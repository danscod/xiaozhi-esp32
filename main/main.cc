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

    // The BT (NimBLE) stack is compiled in for the controller "game mode", but
    // NORMAL mode doesn't use BLE — release the BLE controller's reserved internal
    // RAM back to the heap (~tens of KB) so the voice AFE isn't starved (the 2.3.29
    // deaf regression). VALIDATION build: if voice works now, BT can live in one
    // firmware and game mode just skips this release (+ skips the AFE). One-way:
    // BLE is unusable until reboot — which is exactly the game-mode model.
    esp_err_t bt_rel = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    ESP_LOGI(TAG, "normal mode: released BLE controller mem (%s)", esp_err_to_name(bt_rel));

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
