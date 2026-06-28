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
#include "host/ble_hs.h"        // ble_hs_synced, ble_hs_cfg
#include "host/ble_gap.h"       // ble_gap_conn_find_by_addr, ble_gap_security_initiate
#include "host/ble_sm.h"        // BLE_SM_PAIR_KEY_DIST_*
#include "host/util/util.h"     // ble_hs_util_ensure_addr
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

extern "C" void ble_store_config_init(void);   // NVS-backed bonding key store

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

// Latest RAW HID input report (for the on-device controller test screen).
static uint8_t  s_raw[32] = {0};
static size_t   s_raw_len = 0;

// The specific controller, baked in (confirmed via nRF Connect):
//   HID mode:         name "Q36 for Android", MAC 03:25:00:33:AF:EB
//   ShootingPlus mode:name "ShanWan Q36",     MAC 01:25:00:33:AF:EB
// The Q36 doesn't put its name in the BLE advertisement (only readable via GATT
// after connecting), so name-matching alone never finds it — match by MAC too.
static const uint8_t kQ36HidMac[6] = {0x03, 0x25, 0x00, 0x33, 0xAF, 0xEB};
static const uint8_t kQ36SpMac[6]  = {0x01, 0x25, 0x00, 0x33, 0xAF, 0xEB};

// Match a scanned address against a known MAC in EITHER byte order (the stored
// order vs human MSB-first is ambiguous across the esp_hid/NimBLE boundary).
static bool bda_matches(const uint8_t* bda, const uint8_t* mac) {
    bool fwd = true, rev = true;
    for (int i = 0; i < 6; i++) {
        if (bda[i] != mac[i])     fwd = false;
        if (bda[i] != mac[5 - i]) rev = false;
    }
    return fwd || rev;
}

static bool looks_like_q36(const esp_hid_scan_result_t* r) {
    if (r->transport != ESP_HID_TRANSPORT_BLE) return false;
    if (r->name && (strstr(r->name, "Q36") || strstr(r->name, "ShanWan"))) return true;
    return bda_matches(r->bda, kQ36HidMac) || bda_matches(r->bda, kQ36SpMac);
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
        size_t n = param->input.length;
        if (n > sizeof(s_raw)) n = sizeof(s_raw);
        memcpy(s_raw, param->input.data, n);
        s_raw_len = n;
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

// The NimBLE host event loop. Runs until nimble_port_stop().
static void ble_host_task(void* param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// esp_hidh_dev_open() BLOCKS until GATT discovery completes or fails, and esp_hidh
// never pairs — so the encrypted HID report reads fail ("insufficient auth") and
// the link drops. This task runs CONCURRENTLY with that blocked open: as soon as
// the link is up it initiates security, so the protected reads succeed in time.
// Only the Q36 is connected in game mode, so we find the connection by handle.
static void pair_task(void* arg) {
    (void)arg;
    for (int t = 0; t < 120; t++) {          // ~6s window
        for (uint16_t h = 0; h <= 8; h++) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(h, &desc) == 0) {
                if (!desc.sec_state.encrypted) {
                    int sr = ble_gap_security_initiate(h);
                    ESP_LOGI(TAG, "pairing: security_initiate conn=%u rc=%d", h, sr);
                } else {
                    ESP_LOGI(TAG, "pairing: conn=%u already encrypted", h);
                }
                vTaskDelete(nullptr);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "pairing: no connection appeared to secure");
    vTaskDelete(nullptr);
}

// Scan for the Q36 and open it. Exits once connected or after kScanMaxRounds.
static void scan_task(void* arg) {
    (void)arg;

    // NimBLE needs an identity address before a scan can infer own_addr_type;
    // without one the scan derefs NULL and panics (LoadProhibited). Wait for the
    // host to sync FIRST (abort cleanly if it never does — no crash), then ensure
    // an address. Use prefer_random=1: ensure_addr(0) prefers the PUBLIC address,
    // which isn't configured here and NULL-derefs — a generated random static
    // address is fine for a scanning/connecting central.
    for (int i = 0; i < 200 && !ble_hs_synced(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!ble_hs_synced()) {
        ESP_LOGE(TAG, "BLE host never synced — aborting scan");
        s_scan_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    int addr_rc = ble_hs_util_ensure_addr(1);   // 1 = prefer random static address
    ESP_LOGI(TAG, "BLE host synced; ensure_addr(random)=%d", addr_rc);

    for (int round = 0; round < kScanMaxRounds && !s_connected.load() && s_started.load(); round++) {
        size_t num = 0;
        esp_hid_scan_result_t* results = nullptr;
        ESP_LOGI(TAG, "scanning for controller (round %d/%d)...", round + 1, kScanMaxRounds);
        esp_hid_scan(kScanSeconds, &num, &results);

        esp_hid_scan_result_t* match = nullptr;
        for (esp_hid_scan_result_t* r = results; r; r = r->next) {
            ESP_LOGI(TAG, "  %s %02x:%02x:%02x:%02x:%02x:%02x rssi=%d usage=%s name=%s",
                     (r->transport == ESP_HID_TRANSPORT_BLE) ? "BLE" : "BT",
                     r->bda[0], r->bda[1], r->bda[2], r->bda[3], r->bda[4], r->bda[5],
                     r->rssi, esp_hid_usage_str(r->usage), r->name ? r->name : "");
            if (!match && looks_like_q36(r)) match = r;
        }

        if (match) {
            // Copy the address out before freeing the results list (open uses it,
            // and esp_hidh_dev_open blocks for the whole connection).
            uint8_t bda[6];
            memcpy(bda, match->bda, sizeof(bda));
            esp_hid_transport_t transport = match->transport;
            uint8_t addr_type = match->ble.addr_type;
            ESP_LOGI(TAG, "opening controller %s (addr_type=%d)",
                     match->name ? match->name : "?", addr_type);
            if (results) esp_hid_scan_results_free(results);

            // esp_hidh_dev_open BLOCKS until discovery finishes — spawn the pairing
            // task FIRST so it can encrypt the link while open is mid-discovery.
            xTaskCreate(pair_task, "bt_gp_pair", 3072, nullptr, 6, nullptr);
            esp_hidh_dev_open(bda, transport, addr_type);
            break;  // OPEN/INPUT arrive via the callback
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

    // Security Manager: the HID report characteristics require an encrypted/
    // bonded link (reads otherwise fail "insufficient authentication" and the
    // controller drops the connection). Just-works pairing (no input/output),
    // NVS-backed key storage so the bond persists across reboots.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    // RUN the NimBLE host. esp_hid_gap_init only port-inits the host and
    // esp_hidh_init only registers ble_hs_cfg.sync_cb — nobody starts the host
    // event-loop task, so the controller never syncs ("BLE host never synced").
    // Start it here; it processes the sync event so ble_hs_synced() becomes true.
    nimble_port_freertos_init(ble_host_task);

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

bool bt_gamepad_get_raw(uint8_t* buf, size_t buflen, size_t* out_len, uint32_t* out_seq) {
    if (!buf || !out_len) return false;
    taskENTER_CRITICAL(&s_state_mux);
    size_t n = s_raw_len < buflen ? s_raw_len : buflen;
    memcpy(buf, s_raw, n);
    *out_len = n;
    if (out_seq) *out_seq = s_state.seq;
    taskEXIT_CRITICAL(&s_state_mux);
    return n > 0;
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
