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

char* Telemetry::BuildJson(const char* event_type, int conversation_duration_ms,
                          const std::function<void(cJSON* root)>& extra_fields) {
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

    if (extra_fields) {
        extra_fields(root);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

// Struct passed to the POST task
struct TelemetryPostArgs {
    char* json;
};

void Telemetry::PostEventAsync(const char* event_type, int conversation_duration_ms,
                              std::function<void(cJSON* root)> extra_fields) {
    char* json = BuildJson(event_type, conversation_duration_ms, extra_fields);
    if (!json) {
        ESP_LOGE(TAG, "Failed to build JSON for %s", event_type);
        return;
    }

    auto* args = new TelemetryPostArgs{ json };

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

void Telemetry::PostVideoPlaybackStats(const VideoPlaybackTelemetry& telemetry) {
    PostEventAsync("video_playback_end", 0,
        [telemetry](cJSON* root) {
            cJSON_AddStringToObject(root, "item_id", telemetry.item_id.c_str());
            cJSON_AddStringToObject(root, "title", telemetry.title.c_str());
            cJSON_AddStringToObject(root, "end_reason", telemetry.end_reason.c_str());
            cJSON_AddNumberToObject(root, "playback_duration_ms", telemetry.duration_ms);
            cJSON_AddNumberToObject(root, "queued_video_frames", telemetry.queued_video_frames);
            cJSON_AddNumberToObject(root, "rendered_frames", telemetry.rendered_frames);
            cJSON_AddNumberToObject(root, "dropped_frames", telemetry.dropped_frames);
            cJSON_AddNumberToObject(root, "queue_overflow_drop_count", telemetry.queue_overflow_drop_count);
            cJSON_AddNumberToObject(root, "render_backlog_drop_count", telemetry.render_backlog_drop_count);
            cJSON_AddNumberToObject(root, "max_queued_video_frames", telemetry.max_queued_video_frames);
            cJSON_AddNumberToObject(root, "http_status_code", telemetry.http_status_code);
            cJSON_AddNumberToObject(root, "http_read_calls", telemetry.http_read_calls);
            cJSON_AddNumberToObject(root, "http_read_short_calls", telemetry.http_read_short_calls);
            cJSON_AddNumberToObject(root, "http_zero_reads", telemetry.http_zero_reads);
            cJSON_AddNumberToObject(root, "http_header_read_calls", telemetry.http_header_read_calls);
            cJSON_AddNumberToObject(root, "http_payload_read_calls", telemetry.http_payload_read_calls);
            cJSON_AddNumberToObject(root, "http_read_stalls", telemetry.http_read_stalls);
            cJSON_AddNumberToObject(root, "audio_packets_seen", telemetry.audio_packets_seen);
            cJSON_AddNumberToObject(root, "video_frames_seen", telemetry.video_frames_seen);
            cJSON_AddNumberToObject(root, "video_frames_decode_attempted", telemetry.video_frames_decode_attempted);
            cJSON_AddNumberToObject(root, "video_frames_decode_failed", telemetry.video_frames_decode_failed);
            cJSON_AddNumberToObject(root, "video_frames_presented", telemetry.video_frames_presented);
            cJSON_AddNumberToObject(root, "render_wakeups", telemetry.render_wakeups);
            cJSON_AddNumberToObject(root, "render_empty_queue_wakeups", telemetry.render_empty_queue_wakeups);
            cJSON_AddNumberToObject(root, "max_http_read_stall_ms", telemetry.max_http_read_stall_ms);
            cJSON_AddNumberToObject(root, "max_http_read_ms", telemetry.max_http_read_ms);
            cJSON_AddNumberToObject(root, "audio_push_block_count", telemetry.audio_push_block_count);
            cJSON_AddNumberToObject(root, "max_audio_push_block_ms", telemetry.max_audio_push_block_ms);
            cJSON_AddNumberToObject(root, "max_audio_push_ms", telemetry.max_audio_push_ms);
            cJSON_AddNumberToObject(root, "max_audio_packet_copy_ms", telemetry.max_audio_packet_copy_ms);
            cJSON_AddNumberToObject(root, "max_video_frame_copy_ms", telemetry.max_video_frame_copy_ms);
            cJSON_AddNumberToObject(root, "max_render_queue_wait_ms", telemetry.max_render_queue_wait_ms);
            cJSON_AddNumberToObject(root, "max_render_schedule_sleep_ms", telemetry.max_render_schedule_sleep_ms);
            cJSON_AddNumberToObject(root, "late_frame_count", telemetry.late_frame_count);
            cJSON_AddNumberToObject(root, "max_frame_late_ms", telemetry.max_frame_late_ms);
            cJSON_AddNumberToObject(root, "max_frame_age_before_decode_ms", telemetry.max_frame_age_before_decode_ms);
            cJSON_AddNumberToObject(root, "max_frame_age_after_present_ms", telemetry.max_frame_age_after_present_ms);
            cJSON_AddNumberToObject(root, "max_jpeg_decode_ms", telemetry.max_jpeg_decode_ms);
            cJSON_AddNumberToObject(root, "max_frame_present_ms", telemetry.max_frame_present_ms);
            cJSON_AddNumberToObject(root, "http_open_time_ms", telemetry.http_open_time_ms);
            cJSON_AddNumberToObject(root, "first_audio_packet_ms", telemetry.first_audio_packet_ms);
            cJSON_AddNumberToObject(root, "first_video_frame_ms", telemetry.first_video_frame_ms);
            cJSON_AddNumberToObject(root, "playback_started_ms", telemetry.playback_started_ms);
            cJSON_AddNumberToObject(root, "first_frame_presented_ms", telemetry.first_frame_presented_ms);
            cJSON_AddNumberToObject(root, "http_read_bytes", static_cast<double>(telemetry.http_read_bytes));
            cJSON_AddNumberToObject(root, "audio_bytes_seen", static_cast<double>(telemetry.audio_bytes_seen));
            cJSON_AddNumberToObject(root, "video_bytes_seen", static_cast<double>(telemetry.video_bytes_seen));
            cJSON_AddNumberToObject(root, "http_read_time_us_total", static_cast<double>(telemetry.http_read_time_us_total));
            cJSON_AddNumberToObject(root, "header_read_time_us_total", static_cast<double>(telemetry.header_read_time_us_total));
            cJSON_AddNumberToObject(root, "payload_read_time_us_total", static_cast<double>(telemetry.payload_read_time_us_total));
            cJSON_AddNumberToObject(root, "audio_packet_copy_time_us_total", static_cast<double>(telemetry.audio_packet_copy_time_us_total));
            cJSON_AddNumberToObject(root, "video_frame_copy_time_us_total", static_cast<double>(telemetry.video_frame_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_push_time_us_total", static_cast<double>(telemetry.audio_push_time_us_total));
            cJSON_AddNumberToObject(root, "render_queue_wait_us_total", static_cast<double>(telemetry.render_queue_wait_us_total));
            cJSON_AddNumberToObject(root, "render_schedule_sleep_us_total", static_cast<double>(telemetry.render_schedule_sleep_us_total));
            cJSON_AddNumberToObject(root, "total_frame_late_us", static_cast<double>(telemetry.total_frame_late_us));
            cJSON_AddNumberToObject(root, "total_frame_age_before_decode_us", static_cast<double>(telemetry.total_frame_age_before_decode_us));
            cJSON_AddNumberToObject(root, "total_frame_age_after_present_us", static_cast<double>(telemetry.total_frame_age_after_present_us));
            cJSON_AddNumberToObject(root, "jpeg_decode_time_us_total", static_cast<double>(telemetry.jpeg_decode_time_us_total));
            cJSON_AddNumberToObject(root, "frame_present_time_us_total", static_cast<double>(telemetry.frame_present_time_us_total));
        });
}

void Telemetry::PostVideoPlaybackProgress(const VideoPlaybackTelemetry& telemetry) {
    PostEventAsync("video_playback_progress", 0,
        [telemetry](cJSON* root) {
            cJSON_AddStringToObject(root, "item_id", telemetry.item_id.c_str());
            cJSON_AddStringToObject(root, "title", telemetry.title.c_str());
            cJSON_AddStringToObject(root, "end_reason", telemetry.end_reason.c_str());
            cJSON_AddNumberToObject(root, "playback_duration_ms", telemetry.duration_ms);
            cJSON_AddNumberToObject(root, "queued_video_frames", telemetry.queued_video_frames);
            cJSON_AddNumberToObject(root, "rendered_frames", telemetry.rendered_frames);
            cJSON_AddNumberToObject(root, "dropped_frames", telemetry.dropped_frames);
            cJSON_AddNumberToObject(root, "queue_overflow_drop_count", telemetry.queue_overflow_drop_count);
            cJSON_AddNumberToObject(root, "render_backlog_drop_count", telemetry.render_backlog_drop_count);
            cJSON_AddNumberToObject(root, "max_queued_video_frames", telemetry.max_queued_video_frames);
            cJSON_AddNumberToObject(root, "http_status_code", telemetry.http_status_code);
            cJSON_AddNumberToObject(root, "http_read_calls", telemetry.http_read_calls);
            cJSON_AddNumberToObject(root, "http_read_short_calls", telemetry.http_read_short_calls);
            cJSON_AddNumberToObject(root, "http_zero_reads", telemetry.http_zero_reads);
            cJSON_AddNumberToObject(root, "http_header_read_calls", telemetry.http_header_read_calls);
            cJSON_AddNumberToObject(root, "http_payload_read_calls", telemetry.http_payload_read_calls);
            cJSON_AddNumberToObject(root, "http_read_stalls", telemetry.http_read_stalls);
            cJSON_AddNumberToObject(root, "audio_packets_seen", telemetry.audio_packets_seen);
            cJSON_AddNumberToObject(root, "video_frames_seen", telemetry.video_frames_seen);
            cJSON_AddNumberToObject(root, "video_frames_decode_attempted", telemetry.video_frames_decode_attempted);
            cJSON_AddNumberToObject(root, "video_frames_decode_failed", telemetry.video_frames_decode_failed);
            cJSON_AddNumberToObject(root, "video_frames_presented", telemetry.video_frames_presented);
            cJSON_AddNumberToObject(root, "render_wakeups", telemetry.render_wakeups);
            cJSON_AddNumberToObject(root, "render_empty_queue_wakeups", telemetry.render_empty_queue_wakeups);
            cJSON_AddNumberToObject(root, "max_http_read_stall_ms", telemetry.max_http_read_stall_ms);
            cJSON_AddNumberToObject(root, "max_http_read_ms", telemetry.max_http_read_ms);
            cJSON_AddNumberToObject(root, "audio_push_block_count", telemetry.audio_push_block_count);
            cJSON_AddNumberToObject(root, "max_audio_push_block_ms", telemetry.max_audio_push_block_ms);
            cJSON_AddNumberToObject(root, "max_audio_push_ms", telemetry.max_audio_push_ms);
            cJSON_AddNumberToObject(root, "max_audio_packet_copy_ms", telemetry.max_audio_packet_copy_ms);
            cJSON_AddNumberToObject(root, "max_video_frame_copy_ms", telemetry.max_video_frame_copy_ms);
            cJSON_AddNumberToObject(root, "max_render_queue_wait_ms", telemetry.max_render_queue_wait_ms);
            cJSON_AddNumberToObject(root, "max_render_schedule_sleep_ms", telemetry.max_render_schedule_sleep_ms);
            cJSON_AddNumberToObject(root, "late_frame_count", telemetry.late_frame_count);
            cJSON_AddNumberToObject(root, "max_frame_late_ms", telemetry.max_frame_late_ms);
            cJSON_AddNumberToObject(root, "max_frame_age_before_decode_ms", telemetry.max_frame_age_before_decode_ms);
            cJSON_AddNumberToObject(root, "max_frame_age_after_present_ms", telemetry.max_frame_age_after_present_ms);
            cJSON_AddNumberToObject(root, "max_jpeg_decode_ms", telemetry.max_jpeg_decode_ms);
            cJSON_AddNumberToObject(root, "max_frame_present_ms", telemetry.max_frame_present_ms);
            cJSON_AddNumberToObject(root, "http_open_time_ms", telemetry.http_open_time_ms);
            cJSON_AddNumberToObject(root, "first_audio_packet_ms", telemetry.first_audio_packet_ms);
            cJSON_AddNumberToObject(root, "first_video_frame_ms", telemetry.first_video_frame_ms);
            cJSON_AddNumberToObject(root, "playback_started_ms", telemetry.playback_started_ms);
            cJSON_AddNumberToObject(root, "first_frame_presented_ms", telemetry.first_frame_presented_ms);
            cJSON_AddNumberToObject(root, "http_read_bytes", static_cast<double>(telemetry.http_read_bytes));
            cJSON_AddNumberToObject(root, "audio_bytes_seen", static_cast<double>(telemetry.audio_bytes_seen));
            cJSON_AddNumberToObject(root, "video_bytes_seen", static_cast<double>(telemetry.video_bytes_seen));
            cJSON_AddNumberToObject(root, "http_read_time_us_total", static_cast<double>(telemetry.http_read_time_us_total));
            cJSON_AddNumberToObject(root, "header_read_time_us_total", static_cast<double>(telemetry.header_read_time_us_total));
            cJSON_AddNumberToObject(root, "payload_read_time_us_total", static_cast<double>(telemetry.payload_read_time_us_total));
            cJSON_AddNumberToObject(root, "audio_packet_copy_time_us_total", static_cast<double>(telemetry.audio_packet_copy_time_us_total));
            cJSON_AddNumberToObject(root, "video_frame_copy_time_us_total", static_cast<double>(telemetry.video_frame_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_push_time_us_total", static_cast<double>(telemetry.audio_push_time_us_total));
            cJSON_AddNumberToObject(root, "render_queue_wait_us_total", static_cast<double>(telemetry.render_queue_wait_us_total));
            cJSON_AddNumberToObject(root, "render_schedule_sleep_us_total", static_cast<double>(telemetry.render_schedule_sleep_us_total));
            cJSON_AddNumberToObject(root, "total_frame_late_us", static_cast<double>(telemetry.total_frame_late_us));
            cJSON_AddNumberToObject(root, "total_frame_age_before_decode_us", static_cast<double>(telemetry.total_frame_age_before_decode_us));
            cJSON_AddNumberToObject(root, "total_frame_age_after_present_us", static_cast<double>(telemetry.total_frame_age_after_present_us));
            cJSON_AddNumberToObject(root, "jpeg_decode_time_us_total", static_cast<double>(telemetry.jpeg_decode_time_us_total));
            cJSON_AddNumberToObject(root, "frame_present_time_us_total", static_cast<double>(telemetry.frame_present_time_us_total));
        });
}
