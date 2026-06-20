#include "telemetry.h"
#include "system_info.h"
#include "board.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_core_dump.h>
#include <cJSON.h>
#include "settings.h"
#include "board.h"
#include "web_socket.h"
#include "network_interface.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#define TAG "Telemetry"

Telemetry& Telemetry::GetInstance() {
    static Telemetry instance;
    return instance;
}

void Telemetry::Init() {
    // Pre-read coredump (if present) before sending boot event, so the boot
    // payload can include the panic summary from the previous crash. Erased
    // after read so it doesn't keep showing up on subsequent boots.
    esp_core_dump_summary_t* dump_summary = nullptr;
    if (esp_core_dump_image_check() == ESP_OK) {
        dump_summary = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
        if (dump_summary && esp_core_dump_get_summary(dump_summary) != ESP_OK) {
            free(dump_summary);
            dump_summary = nullptr;
        }
    }

    // Post boot event immediately, including reset/wake reason for diagnostics
    PostEventAsync("boot", 0, [dump_summary](cJSON* root) {
        const char* reset = "unknown";
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:    reset = "poweron";    break;
            case ESP_RST_EXT:        reset = "ext_pin";    break;
            case ESP_RST_SW:         reset = "sw";         break;
            case ESP_RST_PANIC:      reset = "panic";      break;
            case ESP_RST_INT_WDT:    reset = "int_wdt";    break;
            case ESP_RST_TASK_WDT:   reset = "task_wdt";   break;
            case ESP_RST_WDT:        reset = "wdt";        break;
            case ESP_RST_DEEPSLEEP:  reset = "deepsleep";  break;
            case ESP_RST_BROWNOUT:   reset = "brownout";   break;
            case ESP_RST_SDIO:       reset = "sdio";       break;
            default:                 reset = "unknown";    break;
        }
        cJSON_AddStringToObject(root, "reset_reason", reset);

        if (dump_summary) {
            cJSON* cd = cJSON_CreateObject();
            cJSON_AddStringToObject(cd, "task", dump_summary->exc_task);
            char hex[16];
            snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)dump_summary->exc_pc);
            cJSON_AddStringToObject(cd, "pc", hex);
            cJSON* bt = cJSON_CreateArray();
            uint32_t depth = dump_summary->exc_bt_info.depth;
            if (depth > 16) depth = 16;
            for (uint32_t i = 0; i < depth; i++) {
                snprintf(hex, sizeof(hex), "0x%08lx",
                         (unsigned long)dump_summary->exc_bt_info.bt[i]);
                cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
            }
            cJSON_AddItemToObject(cd, "backtrace", bt);
            cJSON_AddBoolToObject(cd, "bt_corrupted",
                                  dump_summary->exc_bt_info.corrupted);
            cJSON_AddItemToObject(root, "coredump", cd);
            free((void*)dump_summary);
        }
    });

    // Erase the coredump so the next panic gets a fresh dump.
    if (dump_summary != nullptr || esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_image_erase();
    }

    // NVS-deferred playback stats: if the previous session ended a video
    // playback, the stats were saved to NVS (avoiding an in-playback HTTP
    // POST that would risk crashing on corrupted heap). Send them now in a
    // clean boot-time heap state, then erase.
    {
        Settings s("pbstats", false);
        if (s.GetInt("present", 0) == 1) {
            int duration_ms = s.GetInt("duration_ms");
            int rendered    = s.GetInt("rendered");
            int dropped     = s.GetInt("dropped");
            int audio_n     = s.GetInt("audio_pkts");
            int video_n     = s.GetInt("video_pkts");
            int overflow    = s.GetInt("overflow");
            std::string reason = s.GetString("end_reason", "unknown");
            PostEventAsync(
                "video_playback_summary", duration_ms,
                [rendered, dropped, audio_n, video_n, overflow, reason](cJSON* root) {
                    cJSON_AddNumberToObject(root, "rendered_frames", rendered);
                    cJSON_AddNumberToObject(root, "dropped_frames",  dropped);
                    cJSON_AddNumberToObject(root, "audio_packets_seen", audio_n);
                    cJSON_AddNumberToObject(root, "video_frames_seen",  video_n);
                    cJSON_AddNumberToObject(root, "queue_overflow_drop_count", overflow);
                    cJSON_AddStringToObject(root, "end_reason", reason.c_str());
                    cJSON_AddBoolToObject  (root, "deferred", true);
                });
            ESP_LOGI(TAG, "Sent deferred playback summary: R=%d D=%d reason=%s",
                     rendered, dropped, reason.c_str());
        }
    }
    {
        Settings s("pbstats", true);
        s.EraseAll();
    }

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

std::string Telemetry::GetTelemetryWsUrl() const {
    Settings s("net", false);
    std::string base = s.GetString("ws_base", kDefaultWsBase);
    if (base.empty()) base = kDefaultWsBase;
    return base + kTelemetryWsPath;
}

bool Telemetry::EnsureTelemetryWs() {
    std::lock_guard<std::mutex> lock(telemetry_ws_mutex_);
    if (telemetry_ws_ && telemetry_ws_->IsConnected()) {
        return true;
    }
    telemetry_ws_.reset();

    auto network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    auto ws = network->CreateWebSocket(3);  // slot 3: chat=1, video=2
    if (!ws) return false;
    ws->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    ws->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    ws->OnDisconnected([this]() {
        std::lock_guard<std::mutex> lock(telemetry_ws_mutex_);
        // Mark for re-open on next event. We don't reset the pointer here to
        // avoid destroying the WS from inside its own callback.
    });
    std::string url = GetTelemetryWsUrl();
    if (!ws->Connect(url.c_str())) {
        ESP_LOGW(TAG, "Telemetry WS connect failed err=%d url=%s",
                 ws->GetLastError(), url.c_str());
        return false;
    }
    telemetry_ws_ = std::move(ws);
    ESP_LOGI(TAG, "Telemetry WS connected → %s", url.c_str());
    return true;
}

bool Telemetry::TrySendViaWs(const char* json_payload) {
    if (!json_payload) return false;
    if (!EnsureTelemetryWs()) return false;
    std::lock_guard<std::mutex> lock(telemetry_ws_mutex_);
    if (!telemetry_ws_ || !telemetry_ws_->IsConnected()) return false;
    size_t len = strlen(json_payload);
    bool sent = telemetry_ws_->Send(json_payload, len, /*binary=*/false, /*fin=*/true);
    if (!sent) {
        ESP_LOGW(TAG, "Telemetry WS send failed; dropping connection for next-event retry");
        telemetry_ws_.reset();
    }
    return sent;
}

void Telemetry::PostEventAsync(const char* event_type, int conversation_duration_ms,
                              std::function<void(cJSON* root)> extra_fields) {
    char* json = BuildJson(event_type, conversation_duration_ms, extra_fields);
    if (!json) {
        ESP_LOGE(TAG, "Failed to build JSON for %s", event_type);
        return;
    }

    // Fast WS path: only attempt if the connection is ALREADY open (a Connect
    // call blocks up to ~10s, which would stall the caller — e.g. the video
    // task during playback). The persistent connection is maintained by a
    // background task; if it's not up right now, fall through to the async
    // HTTP path so the event still gets through.
    {
        std::lock_guard<std::mutex> lock(telemetry_ws_mutex_);
        if (telemetry_ws_ && telemetry_ws_->IsConnected()) {
            size_t len = strlen(json);
            bool sent = telemetry_ws_->Send(json, len, /*binary=*/false, /*fin=*/true);
            if (sent) {
                free(json);
                return;
            }
            ESP_LOGW(TAG, "Telemetry WS send failed; falling back to HTTP");
            telemetry_ws_.reset();
        }
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
            // Now that we're in a background task with the network warmed up,
            // try to open the persistent telemetry WS so the NEXT event can
            // use the fast WS path instead of spawning another HTTP cycle.
            Telemetry::GetInstance().EnsureTelemetryWs();
            vTaskDelete(nullptr);
        },
        "telemetry_post",
        4096,
        args,
        1,
        nullptr
    );
}

// Shared field filler used by both PostVideoPlaybackStats (HTTP fallback OK)
// and TrySendVideoPlaybackStats (WS-only, no fallback).
static std::function<void(cJSON*)> FillVideoPlaybackFields(const VideoPlaybackTelemetry& telemetry) {
    return [telemetry](cJSON* root) {
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
            cJSON_AddNumberToObject(root, "stream_resume_count", telemetry.stream_resume_count);
            cJSON_AddNumberToObject(root, "stream_resume_failure_count", telemetry.stream_resume_failure_count);
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
            cJSON_AddNumberToObject(root, "max_audio_output_write_ms", telemetry.max_audio_output_write_ms);
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
            cJSON_AddNumberToObject(root, "expected_stream_bytes", static_cast<double>(telemetry.expected_stream_bytes));
            cJSON_AddNumberToObject(root, "audio_bytes_seen", static_cast<double>(telemetry.audio_bytes_seen));
            cJSON_AddNumberToObject(root, "video_bytes_seen", static_cast<double>(telemetry.video_bytes_seen));
            cJSON_AddNumberToObject(root, "http_read_time_us_total", static_cast<double>(telemetry.http_read_time_us_total));
            cJSON_AddNumberToObject(root, "header_read_time_us_total", static_cast<double>(telemetry.header_read_time_us_total));
            cJSON_AddNumberToObject(root, "payload_read_time_us_total", static_cast<double>(telemetry.payload_read_time_us_total));
            cJSON_AddNumberToObject(root, "audio_packet_copy_time_us_total", static_cast<double>(telemetry.audio_packet_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_output_samples_written", static_cast<double>(telemetry.audio_output_samples_written));
            cJSON_AddNumberToObject(root, "audio_output_write_time_us_total", static_cast<double>(telemetry.audio_output_write_time_us_total));
            cJSON_AddNumberToObject(root, "video_frame_copy_time_us_total", static_cast<double>(telemetry.video_frame_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_push_time_us_total", static_cast<double>(telemetry.audio_push_time_us_total));
            cJSON_AddNumberToObject(root, "render_queue_wait_us_total", static_cast<double>(telemetry.render_queue_wait_us_total));
            cJSON_AddNumberToObject(root, "render_schedule_sleep_us_total", static_cast<double>(telemetry.render_schedule_sleep_us_total));
            cJSON_AddNumberToObject(root, "total_frame_late_us", static_cast<double>(telemetry.total_frame_late_us));
            cJSON_AddNumberToObject(root, "total_frame_age_before_decode_us", static_cast<double>(telemetry.total_frame_age_before_decode_us));
            cJSON_AddNumberToObject(root, "total_frame_age_after_present_us", static_cast<double>(telemetry.total_frame_age_after_present_us));
            cJSON_AddNumberToObject(root, "jpeg_decode_time_us_total", static_cast<double>(telemetry.jpeg_decode_time_us_total));
            cJSON_AddNumberToObject(root, "frame_present_time_us_total", static_cast<double>(telemetry.frame_present_time_us_total));
            cJSON_AddNumberToObject(root, "audio_output_calls", telemetry.audio_output_calls);
            cJSON_AddNumberToObject(root, "audio_output_underrun_count", telemetry.audio_output_underrun_count);
            cJSON_AddNumberToObject(root, "cpu_core0_busy_pct", telemetry.cpu_core0_busy_pct);
            cJSON_AddNumberToObject(root, "cpu_core1_busy_pct", telemetry.cpu_core1_busy_pct);
            if (!telemetry.cpu_top_task_name.empty()) {
                cJSON_AddStringToObject(root, "cpu_top_task_name", telemetry.cpu_top_task_name.c_str());
                cJSON_AddNumberToObject(root, "cpu_top_task_pct", telemetry.cpu_top_task_pct);
                cJSON_AddNumberToObject(root, "cpu_top_task_core", telemetry.cpu_top_task_core);
            }
    };
}

void Telemetry::PostVideoPlaybackStats(const VideoPlaybackTelemetry& telemetry) {
    PostEventAsync("video_playback_end", 0, FillVideoPlaybackFields(telemetry));
}

bool Telemetry::TrySendVideoPlaybackStats(const VideoPlaybackTelemetry& telemetry) {
    // WS-only: never falls back to HTTP. Returns false if the persistent WS
    // is not currently up — caller can then save to NVS for next-boot send.
    char* json = BuildJson("video_playback_end", 0, FillVideoPlaybackFields(telemetry));
    if (!json) return false;
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(telemetry_ws_mutex_);
        if (telemetry_ws_ && telemetry_ws_->IsConnected()) {
            sent = telemetry_ws_->Send(json, strlen(json), /*binary=*/false, /*fin=*/true);
            if (!sent) {
                ESP_LOGW(TAG, "video_playback_end WS send failed");
                telemetry_ws_.reset();
            }
        }
    }
    free(json);
    return sent;
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
            cJSON_AddNumberToObject(root, "stream_resume_count", telemetry.stream_resume_count);
            cJSON_AddNumberToObject(root, "stream_resume_failure_count", telemetry.stream_resume_failure_count);
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
            cJSON_AddNumberToObject(root, "max_audio_output_write_ms", telemetry.max_audio_output_write_ms);
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
            cJSON_AddNumberToObject(root, "expected_stream_bytes", static_cast<double>(telemetry.expected_stream_bytes));
            cJSON_AddNumberToObject(root, "audio_bytes_seen", static_cast<double>(telemetry.audio_bytes_seen));
            cJSON_AddNumberToObject(root, "video_bytes_seen", static_cast<double>(telemetry.video_bytes_seen));
            cJSON_AddNumberToObject(root, "http_read_time_us_total", static_cast<double>(telemetry.http_read_time_us_total));
            cJSON_AddNumberToObject(root, "header_read_time_us_total", static_cast<double>(telemetry.header_read_time_us_total));
            cJSON_AddNumberToObject(root, "payload_read_time_us_total", static_cast<double>(telemetry.payload_read_time_us_total));
            cJSON_AddNumberToObject(root, "audio_packet_copy_time_us_total", static_cast<double>(telemetry.audio_packet_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_output_samples_written", static_cast<double>(telemetry.audio_output_samples_written));
            cJSON_AddNumberToObject(root, "audio_output_write_time_us_total", static_cast<double>(telemetry.audio_output_write_time_us_total));
            cJSON_AddNumberToObject(root, "video_frame_copy_time_us_total", static_cast<double>(telemetry.video_frame_copy_time_us_total));
            cJSON_AddNumberToObject(root, "audio_push_time_us_total", static_cast<double>(telemetry.audio_push_time_us_total));
            cJSON_AddNumberToObject(root, "render_queue_wait_us_total", static_cast<double>(telemetry.render_queue_wait_us_total));
            cJSON_AddNumberToObject(root, "render_schedule_sleep_us_total", static_cast<double>(telemetry.render_schedule_sleep_us_total));
            cJSON_AddNumberToObject(root, "total_frame_late_us", static_cast<double>(telemetry.total_frame_late_us));
            cJSON_AddNumberToObject(root, "total_frame_age_before_decode_us", static_cast<double>(telemetry.total_frame_age_before_decode_us));
            cJSON_AddNumberToObject(root, "total_frame_age_after_present_us", static_cast<double>(telemetry.total_frame_age_after_present_us));
            cJSON_AddNumberToObject(root, "jpeg_decode_time_us_total", static_cast<double>(telemetry.jpeg_decode_time_us_total));
            cJSON_AddNumberToObject(root, "frame_present_time_us_total", static_cast<double>(telemetry.frame_present_time_us_total));
            cJSON_AddNumberToObject(root, "audio_output_calls", telemetry.audio_output_calls);
            cJSON_AddNumberToObject(root, "audio_output_underrun_count", telemetry.audio_output_underrun_count);
        });
}
