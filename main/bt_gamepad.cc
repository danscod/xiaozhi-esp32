// BLE-HID gamepad host (ShanWan Q36, HID mode). See bt_gamepad.h.
#include "bt_gamepad.h"

#include <string.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "nvs_flash.h"

#include "esp_hidh.h"
#include "esp_hid_gap.h"

#include "mcp_server.h"

#define TAG "BtGamepad"

// How long each scan window runs, and how the connect task behaves.
static const uint32_t kScanSeconds   = 5;
static const int      kScanMaxRounds = 6;   // ~30s of scanning before giving up

static std::atomic<bool> s_started{false};
static std::atomic<bool> s_connected{false};
static TaskHandle_t      s_scan_task = nullptr;
static esp_hidh_dev_t*   s_dev = nullptr;

// Latest decoded state. portMUX keeps the report-callback write and the poller
// read consistent. Decoding is best-effort until we confirm the report layout.
static portMUX_TYPE      s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bt_gamepad_state_t s_state = {};

// The specific controller, baked in (confirmed via nRF Connect):
//   HID mode:         name "Q36 for Android", MAC 03:25:00:33:AF:EB
//   ShootingPlus mode:name "ShanWan Q36",     MAC 01:25:00:33:AF:EB
// Both are standard BLE HID (HOGP, 0x1812). Manufacturer "ShanWan BM-769".
// We match by name so we never grab some other random BLE gamepad nearby.
static bool looks_like_q36(const esp_hid_scan_result_t* r) {
    if (r->transport != ESP_HID_TRANSPORT_BLE || !r->name) return false;
    return strstr(r->name, "Q36") != nullptr || strstr(r->name, "ShanWan") != nullptr;
}

// esp_hidh event callback (default event loop). C linkage for esp_event.
extern "C" void bt_gamepad_hidh_cb(void* handler_args, esp_event_base_t base,
                                   int32_t id, void* event_data) {
    (void)handler_args; (void)base;
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t* param = (esp_hidh_event_data_t*)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            s_dev = param->open.dev;
            s_connected.store(true);
            taskENTER_CRITICAL(&s_state_mux);
            s_state.connected = true;
            taskEXIT_CRITICAL(&s_state_mux);
            ESP_LOGI(TAG, "controller CONNECTED: %s", esp_hidh_dev_name_get(param->open.dev));
        } else {
            ESP_LOGW(TAG, "controller open FAILED");
        }
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        // Milestone 1: dump the raw report so we can learn the Q36's layout
        // (map index, report id, byte meaning). Decoding follows once we see it.
        ESP_LOGI(TAG, "INPUT map=%u id=%u len=%d:",
                 param->input.map_index, param->input.report_id, param->input.length);
        ESP_LOG_BUFFER_HEX(TAG, param->input.data, param->input.length);
        taskENTER_CRITICAL(&s_state_mux);
        s_state.seq++;
        // TODO(report-layout): decode buttons/dpad/sticks from param->input.data
        taskEXIT_CRITICAL(&s_state_mux);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        ESP_LOGW(TAG, "controller DISCONNECTED");
        s_connected.store(false);
        s_dev = nullptr;
        taskENTER_CRITICAL(&s_state_mux);
        s_state.connected = false;
        taskEXIT_CRITICAL(&s_state_mux);
        break;
    }
    default:
        break;
    }
}

// Scan for the Q36 and open it. Exits once connected or after kScanMaxRounds.
static void scan_task(void* arg) {
    (void)arg;
    for (int round = 0; round < kScanMaxRounds && !s_connected.load() && s_started.load(); round++) {
        size_t num = 0;
        esp_hid_scan_result_t* results = nullptr;
        ESP_LOGI(TAG, "scanning for controller (round %d/%d)...", round + 1, kScanMaxRounds);
        esp_hid_scan(kScanSeconds, &num, &results);

        esp_hid_scan_result_t* match = nullptr;
        for (esp_hid_scan_result_t* r = results; r; r = r->next) {
            ESP_LOGI(TAG, "  %s rssi=%d usage=%s name=%s",
                     (r->transport == ESP_HID_TRANSPORT_BLE) ? "BLE" : "BT",
                     r->rssi, esp_hid_usage_str(r->usage), r->name ? r->name : "");
            if (!match && looks_like_q36(r)) match = r;
        }

        if (match) {
            ESP_LOGI(TAG, "opening controller %s (addr_type=%d)",
                     match->name ? match->name : "?", match->ble.addr_type);
            esp_hidh_dev_open(match->bda, match->transport, match->ble.addr_type);
            if (results) esp_hid_scan_results_free(results);
            break;  // OPEN/INPUT now arrive via the callback
        }
        if (results) esp_hid_scan_results_free(results);
    }
    if (!s_connected.load()) ESP_LOGW(TAG, "no controller found");
    s_scan_task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t bt_gamepad_start(void) {
    if (s_started.exchange(true)) {
        ESP_LOGI(TAG, "already started");
        return ESP_OK;
    }

    // NVS is needed for BLE bonding storage. The app normally inits it already;
    // tolerate that (ESP_ERR_NVS_NO_FREE_PAGES etc. handled by the app at boot).
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }

    // Brings up the BT controller + NimBLE host in BLE-only mode.
    ret = esp_hid_gap_init(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(ret));
        s_started.store(false);
        return ret;
    }

    esp_hidh_config_t config = {};
    config.callback = bt_gamepad_hidh_cb;
    config.event_stack_size = 4096;
    config.callback_arg = nullptr;
    ret = esp_hidh_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed: %s", esp_err_to_name(ret));
        s_started.store(false);
        return ret;
    }

    ESP_LOGI(TAG, "BLE HID host up — scanning for the Q36 (HID mode)");
    xTaskCreate(scan_task, "bt_gp_scan", 4096, nullptr, 5, &s_scan_task);
    return ESP_OK;
}

void bt_gamepad_stop(void) {
    if (!s_started.exchange(false)) return;
    // Best-effort teardown. Close the device; the NimBLE host/controller stay
    // resident for now (a clean nimble_port deinit will be added once the basic
    // connection path is verified — doing it wrong risks a crash on teardown).
    if (s_dev) {
        esp_hidh_dev_close(s_dev);
        s_dev = nullptr;
    }
    s_connected.store(false);
    taskENTER_CRITICAL(&s_state_mux);
    s_state.connected = false;
    taskEXIT_CRITICAL(&s_state_mux);
    ESP_LOGI(TAG, "controller host stopped");
}

bool bt_gamepad_connected(void) { return s_connected.load(); }

void bt_gamepad_get_state(bt_gamepad_state_t* out) {
    if (!out) return;
    taskENTER_CRITICAL(&s_state_mux);
    *out = s_state;
    taskEXIT_CRITICAL(&s_state_mux);
}

void bt_gamepad_register_mcp(void) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.controller.pair",
        "Put the device into Bluetooth pairing mode to connect a BLE game "
        "controller (e.g. the ShanWan Q36 in HID mode) for playing DOOM. Turn the "
        "controller on in HID mode first. Returns once scanning has started; "
        "connection happens in the background.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            esp_err_t r = bt_gamepad_start();
            return r == ESP_OK ? std::string("Scanning for a controller…")
                               : std::string("Failed to start Bluetooth.");
        });

    mcp.AddTool(
        "self.controller.status",
        "Report whether a Bluetooth game controller is currently connected.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return std::string(bt_gamepad_connected() ? "Controller connected."
                                                      : "No controller connected.");
        });

    mcp.AddTool(
        "self.controller.unpair",
        "Disconnect the Bluetooth game controller and shut the Bluetooth radio down.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            bt_gamepad_stop();
            return std::string("Controller disconnected.");
        });
}
