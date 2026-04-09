#include "telemetry.h"
#include "system_info.h"
#include "board.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_app_desc.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "Telemetry"

Telemetry& Telemetry::GetInstance() {
    static Telemetry instance;
    return instance;
}

void Telemetry::Init() {
    // Post boot event immediately
    PostEventAsync("boot");

    // Start 5-minute heartbeat timer
    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            static_cast<Telemetry*>(arg)->PostHeartbeat();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "telemetry_heartbeat",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &heartbeat_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(heartbeat_timer_, kHeartbeatIntervalUs));
    ESP_LOGI(TAG, "Initialized (heartbeat every 5 min) → %s", kUrl);
}

void Telemetry::PostHeartbeat() {
    PostEventAsync("heartbeat");
}

void Telemetry::OnWakeWord(const std::string& wake_word) {
    wake_word_count_++;
    PostEventAsync("wake_word");
}

void Telemetry::OnStateChanged(DeviceState new_state) {
    if ((new_state == kDeviceStateConnecting || new_state == kDeviceStateListening)
            && !in_conversation_) {
        in_conversation_  = true;
        conv_start_us_    = esp_timer_get_time();
        conversation_count_++;
        PostEventAsync("conversation_start");
    } else if (new_state == kDeviceStateIdle && in_conversation_) {
        int duration_ms = (int)((esp_timer_get_time() - conv_start_us_) / 1000);
        in_conversation_ = false;
        PostEventAsync("conversation_end", duration_ms);
    }
}

char* Telemetry::BuildJson(const char* event_type, int conversation_duration_ms) {
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "event",           event_type);
    cJSON_AddStringToObject(root, "device_id",       SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "firmware",        esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "uptime_ms",       (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(root, "free_heap",       (double)SystemInfo::GetFreeHeapSize());
    cJSON_AddNumberToObject(root, "min_free_heap",   (double)SystemInfo::GetMinimumFreeHeapSize());
    cJSON_AddNumberToObject(root, "conversation_count", conversation_count_);
    cJSON_AddNumberToObject(root, "wake_word_count",    wake_word_count_);

    if (conversation_duration_ms > 0) {
        cJSON_AddNumberToObject(root, "conversation_duration_ms", conversation_duration_ms);
    }

    // Battery + charging
    int  bat_level  = 0;
    bool charging   = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(bat_level, charging, discharging)) {
        cJSON_AddNumberToObject(root, "battery",  bat_level);
        cJSON_AddBoolToObject  (root, "charging", charging);
    }

    // Temperature
    float temp = 0.0f;
    if (Board::GetInstance().GetTemperature(temp)) {
        cJSON_AddNumberToObject(root, "temperature", (double)temp);
    }

    // WiFi RSSI + IP
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    }
    esp_netif_ip_info_t ip_info = {};
    auto netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        cJSON_AddStringToObject(root, "ip", ip_str);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

// Struct passed to the POST task
struct TelemetryPostArgs {
    char* json;
    bool* posting_flag;
};

void Telemetry::PostEventAsync(const char* event_type, int conversation_duration_ms) {
    if (posting_) {
        ESP_LOGD(TAG, "Skipping %s — previous POST in flight", event_type);
        return;
    }

    char* json = BuildJson(event_type, conversation_duration_ms);
    if (!json) {
        ESP_LOGE(TAG, "Failed to build JSON for %s", event_type);
        return;
    }

    posting_ = true;

    // Capture everything the task needs
    auto* args = new TelemetryPostArgs{ json, &posting_ };

    xTaskCreate(
        [](void* param) {
            auto* a = static_cast<TelemetryPostArgs*>(param);

            auto& board   = Board::GetInstance();
            auto  network = board.GetNetwork();
            auto  http    = network->CreateHttp(0);

            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("User-Agent",   SystemInfo::GetUserAgent());
            http->SetHeader("Device-Id",    SystemInfo::GetMacAddress().c_str());
            http->SetContent(a->json);  // Http takes ownership / copies internally

            if (http->Open("POST", Telemetry::kUrl)) {
                int code = http->GetStatusCode();
                if (code != 200) {
                    ESP_LOGW(TAG, "Telemetry POST returned %d", code);
                } else {
                    ESP_LOGD(TAG, "Telemetry POST OK");
                }
                http->Close();
            } else {
                ESP_LOGW(TAG, "Telemetry POST failed (network error)");
            }

            free(a->json);
            *a->posting_flag = false;
            delete a;
            vTaskDelete(nullptr);
        },
        "telemetry_post",
        4096,
        args,
        1,
        nullptr
    );
}
