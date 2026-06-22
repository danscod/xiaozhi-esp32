#include "video_player.h"
#include "media_player.h"
#include "flappy_bird.h"
#include "mcp_server.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "audio/audio_service.h"
#include "display/display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "telemetry.h"
#include "settings.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps
#include <cJSON.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#define TAG "VideoPlayer"

namespace {

TickType_t DelayTicksForUs(int64_t delay_us) {
    if (delay_us <= 0) {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>((delay_us + 999) / 1000));
    return ticks > 0 ? ticks : 1;
}

}  // namespace

// ── Singleton ─────────────────────────────────────────────────────────────────

VideoPlayer& VideoPlayer::GetInstance() {
    static VideoPlayer instance;
    return instance;
}

std::string VideoPlayer::PlayItem(const std::string& item_id) {
    return StartItem(item_id);
}

VideoPlaybackTelemetry VideoPlayer::BuildTelemetrySnapshot(int duration_ms,
                                                           int queued_video_frames) const {
    VideoPlaybackTelemetry telemetry;
    telemetry.item_id = current_id_;
    telemetry.title = current_title_;
    telemetry.duration_ms = duration_ms;
    telemetry.queued_video_frames = queued_video_frames;

    std::lock_guard<std::mutex> lock(playback_stats_mutex_);
    telemetry.end_reason = playback_end_reason_;
    telemetry.rendered_frames = static_cast<int>(playback_stats_.rendered_frames);
    telemetry.dropped_frames = static_cast<int>(playback_stats_.dropped_frames);
    telemetry.queue_overflow_drop_count = static_cast<int>(playback_stats_.queue_overflow_drop_count);
    telemetry.render_backlog_drop_count = static_cast<int>(playback_stats_.render_backlog_drop_count);
    telemetry.max_queued_video_frames = static_cast<int>(playback_stats_.max_queued_video_frames);
    telemetry.http_status_code = playback_stats_.http_status_code;
    telemetry.stream_resume_count = static_cast<int>(playback_stats_.stream_resume_count);
    telemetry.stream_resume_failure_count = static_cast<int>(playback_stats_.stream_resume_failure_count);
    telemetry.http_read_calls = static_cast<int>(playback_stats_.http_read_calls);
    telemetry.http_read_short_calls = static_cast<int>(playback_stats_.http_read_short_calls);
    telemetry.http_zero_reads = static_cast<int>(playback_stats_.http_zero_reads);
    telemetry.http_header_read_calls = static_cast<int>(playback_stats_.http_header_read_calls);
    telemetry.http_payload_read_calls = static_cast<int>(playback_stats_.http_payload_read_calls);
    telemetry.http_read_stalls = static_cast<int>(playback_stats_.http_read_stalls);
    telemetry.audio_packets_seen = static_cast<int>(playback_stats_.audio_packets_seen);
    telemetry.video_frames_seen = static_cast<int>(playback_stats_.video_frames_seen);
    telemetry.video_frames_decode_attempted = static_cast<int>(playback_stats_.video_frames_decode_attempted);
    telemetry.video_frames_decode_failed = static_cast<int>(playback_stats_.video_frames_decode_failed);
    telemetry.video_frames_presented = static_cast<int>(playback_stats_.video_frames_presented);
    telemetry.render_wakeups = static_cast<int>(playback_stats_.render_wakeups);
    telemetry.render_empty_queue_wakeups = static_cast<int>(playback_stats_.render_empty_queue_wakeups);
    telemetry.max_http_read_stall_ms = static_cast<int>(playback_stats_.max_http_read_stall_us / 1000);
    telemetry.max_http_read_ms = static_cast<int>(playback_stats_.max_http_read_us / 1000);
    telemetry.audio_push_block_count = static_cast<int>(playback_stats_.audio_push_block_count);
    telemetry.max_audio_push_block_ms = static_cast<int>(playback_stats_.max_audio_push_block_us / 1000);
    telemetry.max_audio_push_ms = static_cast<int>(playback_stats_.max_audio_push_us / 1000);
    telemetry.max_audio_packet_copy_ms = static_cast<int>(playback_stats_.max_audio_packet_copy_us / 1000);
    telemetry.max_audio_output_write_ms =
        static_cast<int>(playback_stats_.max_audio_output_write_us / 1000);
    telemetry.max_video_frame_copy_ms = static_cast<int>(playback_stats_.max_video_frame_copy_us / 1000);
    telemetry.max_render_queue_wait_ms = static_cast<int>(playback_stats_.max_render_queue_wait_us / 1000);
    telemetry.max_render_schedule_sleep_ms =
        static_cast<int>(playback_stats_.max_render_schedule_sleep_us / 1000);
    telemetry.late_frame_count = static_cast<int>(playback_stats_.late_frame_count);
    telemetry.max_frame_late_ms = static_cast<int>(playback_stats_.max_frame_late_us / 1000);
    telemetry.max_frame_age_before_decode_ms =
        static_cast<int>(playback_stats_.max_frame_age_before_decode_us / 1000);
    telemetry.max_frame_age_after_present_ms =
        static_cast<int>(playback_stats_.max_frame_age_after_present_us / 1000);
    telemetry.max_jpeg_decode_ms = static_cast<int>(playback_stats_.max_jpeg_decode_us / 1000);
    telemetry.max_frame_present_ms = static_cast<int>(playback_stats_.max_frame_present_us / 1000);
    telemetry.http_open_time_ms = playback_stats_.http_open_time_us > 0
        ? static_cast<int>(playback_stats_.http_open_time_us / 1000)
        : -1;
    telemetry.first_audio_packet_ms = playback_stats_.first_audio_packet_us > 0
        ? static_cast<int>(playback_stats_.first_audio_packet_us / 1000)
        : -1;
    telemetry.first_video_frame_ms = playback_stats_.first_video_frame_us > 0
        ? static_cast<int>(playback_stats_.first_video_frame_us / 1000)
        : -1;
    telemetry.playback_started_ms = playback_stats_.playback_started_us > 0
        ? static_cast<int>(playback_stats_.playback_started_us / 1000)
        : -1;
    telemetry.first_frame_presented_ms = playback_stats_.first_frame_presented_us > 0
        ? static_cast<int>(playback_stats_.first_frame_presented_us / 1000)
        : -1;
    telemetry.http_read_bytes = playback_stats_.http_read_bytes;
    telemetry.expected_stream_bytes = playback_stats_.expected_stream_bytes;
    telemetry.audio_bytes_seen = playback_stats_.audio_bytes_seen;
    telemetry.video_bytes_seen = playback_stats_.video_bytes_seen;
    telemetry.http_read_time_us_total = playback_stats_.http_read_time_us_total;
    telemetry.header_read_time_us_total = playback_stats_.header_read_time_us_total;
    telemetry.payload_read_time_us_total = playback_stats_.payload_read_time_us_total;
    telemetry.audio_packet_copy_time_us_total = playback_stats_.audio_packet_copy_time_us_total;
    telemetry.audio_output_samples_written = playback_stats_.audio_output_samples_written;
    telemetry.audio_output_write_time_us_total = playback_stats_.audio_output_write_time_us_total;
    telemetry.video_frame_copy_time_us_total = playback_stats_.video_frame_copy_time_us_total;
    telemetry.audio_push_time_us_total = playback_stats_.audio_push_time_us_total;
    telemetry.render_queue_wait_us_total = playback_stats_.render_queue_wait_us_total;
    telemetry.render_schedule_sleep_us_total = playback_stats_.render_schedule_sleep_us_total;
    telemetry.total_frame_late_us = playback_stats_.total_frame_late_us;
    telemetry.total_frame_age_before_decode_us = playback_stats_.total_frame_age_before_decode_us;
    telemetry.total_frame_age_after_present_us = playback_stats_.total_frame_age_after_present_us;
    telemetry.jpeg_decode_time_us_total = playback_stats_.jpeg_decode_time_us_total;
    telemetry.frame_present_time_us_total = playback_stats_.frame_present_time_us_total;
    telemetry.audio_output_calls = static_cast<int>(playback_stats_.audio_output_calls);
    telemetry.audio_output_underrun_count =
        static_cast<int>(playback_stats_.audio_output_underrun_count);

    // Per-core CPU utilization since playback started. We snapshot at
    // playback start (PlayItem) and again here; the diff over the window
    // gives % busy per core. Pinning render to core 1 and opus to core 0
    // makes this an easy way to spot overload (one core at 95%+).
    if (playback_stats_.cpu_start.valid) {
        CoreCpuStats end_cpu{};
        if (SystemInfo::GetCoreCpuStats(&end_cpu)) {
            // Use unsigned arithmetic to gracefully handle the U32 counter
            // wrapping (~71 min boundary).
            auto delta = [](uint64_t end, uint64_t start) -> uint64_t {
                // 32-bit counter wrap: if end < start, add 2^32.
                if (end >= start) return end - start;
                return end + (uint64_t{1} << 32) - start;
            };
            uint64_t total_delta = delta(end_cpu.total_runtime,
                                         playback_stats_.cpu_start.total_runtime);
            if (total_delta > 0) {
                uint64_t idle0_delta = delta(end_cpu.core0_idle_run_time,
                                             playback_stats_.cpu_start.core0_idle_run_time);
                uint64_t idle1_delta = delta(end_cpu.core1_idle_run_time,
                                             playback_stats_.cpu_start.core1_idle_run_time);
                // Busy% per core = 100 - (idle_delta / total_delta * 100).
                // total_delta is the wall-clock interval; each core has the
                // same amount of time available.
                int busy0 = 100 - (int)((idle0_delta * 100) / total_delta);
                int busy1 = 100 - (int)((idle1_delta * 100) / total_delta);
                telemetry.cpu_core0_busy_pct = std::max(0, std::min(100, busy0));
                telemetry.cpu_core1_busy_pct = std::max(0, std::min(100, busy1));
            }
            // Top task DURING the playback window (delta of per-task
            // runtimes vs the snapshot taken at start). Much more
            // informative than the cumulative-since-boot metric.
            if (playback_stats_.cpu_start_task_count > 0 && total_delta > 0) {
                SystemInfo::TaskRunTimeEntry end_tasks[PlaybackStats::kMaxTaskSnapshotEntries];
                size_t end_n = SystemInfo::CaptureTaskRunTimes(
                    end_tasks, PlaybackStats::kMaxTaskSnapshotEntries);
                char     top_name[16] = {0};
                uint32_t top_delta    = 0;
                int      top_core     = -1;
                if (SystemInfo::FindTopTaskByDelta(
                        playback_stats_.cpu_start_tasks,
                        playback_stats_.cpu_start_task_count,
                        end_tasks, end_n,
                        top_name, sizeof(top_name),
                        &top_delta, &top_core)) {
                    telemetry.cpu_top_task_name = top_name;
                    telemetry.cpu_top_task_core = top_core;
                    telemetry.cpu_top_task_pct =
                        (int)((uint64_t(top_delta) * 100) / total_delta);
                }
            }
        }
    }

    auto audio_metrics = Application::GetInstance().GetAudioService().GetPlaybackMetrics();
    telemetry.max_audio_output_write_ms =
        std::max(telemetry.max_audio_output_write_ms, audio_metrics.max_output_write_ms);
    telemetry.audio_output_samples_written =
        std::max<int64_t>(telemetry.audio_output_samples_written, audio_metrics.output_samples_written);
    telemetry.audio_output_write_time_us_total =
        std::max<int64_t>(telemetry.audio_output_write_time_us_total,
                          audio_metrics.output_write_time_us_total);
    telemetry.audio_output_calls =
        std::max(telemetry.audio_output_calls, audio_metrics.output_calls);
    telemetry.audio_output_underrun_count =
        std::max(telemetry.audio_output_underrun_count, audio_metrics.output_underrun_count);
    return telemetry;
}

void VideoPlayer::SetPlaybackEndReason(const char* reason) {
    std::lock_guard<std::mutex> lock(playback_stats_mutex_);
    playback_end_reason_ = reason ? reason : "";
}

void VideoPlayer::MaybePostPlaybackProgress() {
    // Skip during WebSocket streaming — concurrent telemetry POSTs hit a
    // use-after-free bug in HttpClient::OnTcpDisconnected (the managed
    // esp-ml307 component does not synchronize its destructor with the TCP
    // receive task). The end-of-playback event still fires from MaybeFinishPlayback
    // which runs after the WS is fully closed.
    if (use_ws_media_api_) {
        return;
    }
    int64_t start_us = 0;
    {
        std::lock_guard<std::mutex> lock(playback_stats_mutex_);
        start_us = playback_stats_.start_us;
    }
    if (playback_stats_reported_ || start_us <= 0 || current_id_.empty()) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    int64_t last_post_us = last_progress_post_us_.load();
    if (last_post_us != 0 && now_us - last_post_us < kPlaybackProgressIntervalUs) {
        return;
    }
    last_progress_post_us_.store(now_us);

    int queued_video_frames = 0;
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        queued_video_frames = static_cast<int>(video_queue_.size());
    }

    int duration_ms = static_cast<int>((now_us - start_us) / 1000);
    Telemetry::GetInstance().PostVideoPlaybackProgress(
        BuildTelemetrySnapshot(duration_ms, queued_video_frames));
}

void VideoPlayer::MaybeFinishPlayback() {
    bool should_finish = false;
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        should_finish = (stream_task_handle_ == nullptr &&
                         render_task_handle_ == nullptr);
    }
    if (should_finish) {
        if (!playback_stats_reported_) {
            playback_stats_reported_ = true;
            int64_t start_us = 0;
            {
                std::lock_guard<std::mutex> lock(playback_stats_mutex_);
                start_us = playback_stats_.start_us;
            }
            int duration_ms = start_us > 0
                ? static_cast<int>((esp_timer_get_time() - start_us) / 1000)
                : 0;
            VideoPlaybackTelemetry telemetry = BuildTelemetrySnapshot(duration_ms, 0);
            if (use_ws_media_api_) {
                // Prefer the persistent telemetry WS (no HttpClient created,
                // no destructor races, instant delivery). If the WS isn't
                // currently up, fall back to NVS so the summary is sent on
                // the next boot. This avoids ever creating an HttpClient in
                // a heap-fragile post-playback moment.
                bool sent = Telemetry::GetInstance().TrySendVideoPlaybackStats(telemetry);
                if (sent) {
                    ESP_LOGI(TAG, "Playback summary sent via telemetry WS");
                } else {
                    Settings s("pbstats", true);
                    s.SetInt("duration_ms", duration_ms);
                    s.SetInt("rendered",    telemetry.rendered_frames);
                    s.SetInt("dropped",     telemetry.dropped_frames);
                    s.SetInt("audio_pkts",  telemetry.audio_packets_seen);
                    s.SetInt("video_pkts",  telemetry.video_frames_seen);
                    s.SetInt("overflow",    telemetry.queue_overflow_drop_count);
                    s.SetString("end_reason", playback_end_reason_);
                    s.SetInt("present", 1);
                    ESP_LOGI(TAG, "Playback stats saved to NVS (WS not up; will send on next boot)");
                }
            } else {
                Telemetry::GetInstance().PostVideoPlaybackStats(telemetry);
            }
            ESP_LOGI(TAG,
                     "Playback stats: rendered=%d dropped=%d http_calls=%d http_stalls=%d "
                     "decode_attempts=%d decode_failures=%d present=%d max_decode_ms=%d "
                     "max_present_ms=%d end_reason=%s",
                     telemetry.rendered_frames,
                     telemetry.dropped_frames,
                     telemetry.http_read_calls,
                     telemetry.http_read_stalls,
                     telemetry.video_frames_decode_attempted,
                     telemetry.video_frames_decode_failed,
                     telemetry.video_frames_presented,
                     telemetry.max_jpeg_decode_ms,
                     telemetry.max_frame_present_ms,
                     telemetry.end_reason.c_str());
        }
        Application::GetInstance().Schedule([this]() { StopPlayback(); });
    }
}

// ── MCP tool registration ─────────────────────────────────────────────────────

void VideoPlayer::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // ── self.video.search ────────────────────────────────────────────────────
    mcp.AddTool(
        "self.video.search",
        "Search the device video library for video content to play. "
        "Returns a JSON array of matching items, each with 'id', 'title', "
        "'description', 'type' (video), and 'duration_s'. "
        "Use the returned 'id' with self.video.play to start playback.",
        PropertyList({
            Property("query", kPropertyTypeString,
                     "Natural language search query, e.g. 'test video' or 'Big Buck Bunny'.")
        }),
        [this](const PropertyList& props) -> ReturnValue {
            auto query = props["query"].value<std::string>();
            return FetchSearchResults(query);
        }
    );

    // ── self.video.play ──────────────────────────────────────────────────────
    mcp.AddTool(
        "self.video.play",
        "Start playing a video item on the device display with synchronised audio. "
        "Use the 'id' value returned by self.video.search. "
        "Video renders at 240×240 on the device screen; audio plays through the speaker.",
        PropertyList({
            Property("id", kPropertyTypeString,
                     "The video item ID from self.video.search results.")
        }),
        [this](const PropertyList& props) -> ReturnValue {
            auto id = props["id"].value<std::string>();
            return StartItem(id);
        }
    );

    // ── self.video.stop ──────────────────────────────────────────────────────
    mcp.AddTool(
        "self.video.stop",
        "Stop the currently playing video. Does nothing if no video is playing.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_ == State::kIdle) {
                return std::string("Nothing is playing.");
            }
            Stop();
            return std::string("Stopped.");
        }
    );

    // ── self.video.status ────────────────────────────────────────────────────
    mcp.AddTool(
        "self.video.status",
        "Get the current video playback status.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            const char* state_str = "idle";
            switch (state_) {
                case State::kLoading: state_str = "loading"; break;
                case State::kPlaying: state_str = "playing"; break;
                case State::kError:   state_str = "error";   break;
                default:              state_str = "idle";    break;
            }
            int queued_video_frames = 0;
            {
                std::lock_guard<std::mutex> lock(video_queue_mutex_);
                queued_video_frames = static_cast<int>(video_queue_.size());
            }
            int64_t start_us = 0;
            {
                std::lock_guard<std::mutex> lock(playback_stats_mutex_);
                start_us = playback_stats_.start_us;
            }
            int duration_ms = 0;
            if (start_us > 0) {
                duration_ms = static_cast<int>((esp_timer_get_time() - start_us) / 1000);
            }
            VideoPlaybackTelemetry telemetry = BuildTelemetrySnapshot(duration_ms, queued_video_frames);
            cJSON_AddStringToObject(root, "state",         state_str);
            cJSON_AddStringToObject(root, "current_id",    current_id_.c_str());
            cJSON_AddStringToObject(root, "current_title", current_title_.c_str());
            cJSON_AddStringToObject(root, "end_reason", telemetry.end_reason.c_str());
            cJSON_AddNumberToObject(root, "playback_duration_ms", telemetry.duration_ms);
            cJSON_AddNumberToObject(root, "queued_video_frames", telemetry.queued_video_frames);
            cJSON_AddNumberToObject(root, "max_queued_video_frames", telemetry.max_queued_video_frames);
            cJSON_AddNumberToObject(root, "rendered_frames", telemetry.rendered_frames);
            cJSON_AddNumberToObject(root, "dropped_frames", telemetry.dropped_frames);
            cJSON_AddNumberToObject(root, "queue_overflow_drop_count", telemetry.queue_overflow_drop_count);
            cJSON_AddNumberToObject(root, "render_backlog_drop_count", telemetry.render_backlog_drop_count);
            cJSON_AddNumberToObject(root, "http_read_stalls", telemetry.http_read_stalls);
            cJSON_AddNumberToObject(root, "http_read_calls", telemetry.http_read_calls);
            cJSON_AddNumberToObject(root, "http_read_bytes", static_cast<double>(telemetry.http_read_bytes));
            cJSON_AddNumberToObject(root, "max_http_read_stall_ms", telemetry.max_http_read_stall_ms);
            cJSON_AddNumberToObject(root, "max_http_read_ms", telemetry.max_http_read_ms);
            cJSON_AddNumberToObject(root, "audio_push_block_count", telemetry.audio_push_block_count);
            cJSON_AddNumberToObject(root, "max_audio_push_block_ms", telemetry.max_audio_push_block_ms);
            cJSON_AddNumberToObject(root, "max_audio_push_ms", telemetry.max_audio_push_ms);
            cJSON_AddNumberToObject(root, "video_frames_decode_attempted", telemetry.video_frames_decode_attempted);
            cJSON_AddNumberToObject(root, "video_frames_decode_failed", telemetry.video_frames_decode_failed);
            cJSON_AddNumberToObject(root, "video_frames_presented", telemetry.video_frames_presented);
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
            if (state_ == State::kError) {
                cJSON_AddStringToObject(root, "error", error_msg_.c_str());
            }
            return root;
        }
    );

    ESP_LOGI(TAG, "Registered 4 MCP tools (search, play, stop, status)");
}

// ── FetchSearchResults ────────────────────────────────────────────────────────

std::string VideoPlayer::FetchSearchResults(const std::string& query) {
    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    auto  http    = network->CreateHttp(0);

    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());

    std::string encoded_query;
    for (char c : query) {
        if (c == ' ') encoded_query += '+';
        else          encoded_query += c;
    }
    std::string url = std::string(kSearchUrl) + "?q=" + encoded_query + "&type=video";

    if (!http->Open("GET", url)) {
        return R"({"error":"network error"})";
    }
    if (http->GetStatusCode() != 200) {
        http->Close();
        return R"({"error":"server error"})";
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

// ── StartItem ─────────────────────────────────────────────────────────────────

std::string VideoPlayer::StartItem(const std::string& item_id) {
    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    auto  http    = network->CreateHttp(0);

    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());

    std::string url = std::string(kSearchUrl) + "/item/" + item_id;
    if (!http->Open("GET", url)) {
        return "Error: could not reach media server.";
    }
    if (http->GetStatusCode() != 200) {
        http->Close();
        return "Error: item '" + item_id + "' not found.";
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return "Error: bad response from media server.";

    auto* title_j = cJSON_GetObjectItem(root, "title");
    auto* url_j   = cJSON_GetObjectItem(root, "url");
    auto* playback_mode_j = cJSON_GetObjectItem(root, "playback_mode");
    auto* sync_frame_url_j = cJSON_GetObjectItem(root, "sync_frame_url");
    auto* sync_audio_url_j = cJSON_GetObjectItem(root, "sync_audio_url");
    auto* ws_stream_url_j  = cJSON_GetObjectItem(root, "ws_stream_url");
    auto* duration_s_j = cJSON_GetObjectItem(root, "duration_s");
    auto* sync_audio_packet_ms_j = cJSON_GetObjectItem(root, "sync_audio_packet_ms");
    auto* sync_audio_batch_packets_j = cJSON_GetObjectItem(root, "sync_audio_batch_packets");
    auto* sync_video_batch_frames_j = cJSON_GetObjectItem(root, "sync_video_batch_frames");
    auto* sync_frame_lead_ms_j = cJSON_GetObjectItem(root, "sync_frame_lead_ms");
    if (!cJSON_IsString(title_j) || !cJSON_IsString(url_j)) {
        cJSON_Delete(root);
        return "Error: item metadata incomplete.";
    }
    std::string saved_title = title_j->valuestring;
    std::string saved_url   = url_j->valuestring;
    // Prefer WebSocket streaming when offered (one persistent connection,
    // avoids per-batch HTTP cycle and the http_client mutex contention bug).
    bool use_ws_media_api = cJSON_IsString(ws_stream_url_j) &&
                            ws_stream_url_j->valuestring[0] != '\0';
    std::string saved_ws_stream_url = use_ws_media_api ? ws_stream_url_j->valuestring : "";
    ESP_LOGI(TAG, "PlayItem '%s': ws_stream_url_j=%p is_str=%d use_ws=%d body_len=%zu",
             item_id.c_str(),
             (void*)ws_stream_url_j,
             ws_stream_url_j ? cJSON_IsString(ws_stream_url_j) : 0,
             (int)use_ws_media_api,
             body.size());
    if (use_ws_media_api) {
        ESP_LOGI(TAG, "  ws_url=%s", ws_stream_url_j->valuestring);
    }
    bool use_sync_media_api =
        !use_ws_media_api &&
        cJSON_IsString(playback_mode_j) &&
        std::string(playback_mode_j->valuestring) == "sync_v1" &&
        cJSON_IsString(sync_frame_url_j) &&
        cJSON_IsString(sync_audio_url_j);
    std::string saved_sync_frame_url = use_sync_media_api ? sync_frame_url_j->valuestring : "";
    std::string saved_sync_audio_url = use_sync_media_api ? sync_audio_url_j->valuestring : "";
    int saved_duration_ms =
        cJSON_IsNumber(duration_s_j) ? static_cast<int>(duration_s_j->valuedouble * 1000.0) : 0;
    int saved_sync_audio_packet_ms =
        cJSON_IsNumber(sync_audio_packet_ms_j) ? sync_audio_packet_ms_j->valueint : 60;
    int saved_sync_audio_batch_packets =
        cJSON_IsNumber(sync_audio_batch_packets_j) ? sync_audio_batch_packets_j->valueint : 96;
    int saved_sync_video_batch_frames =
        cJSON_IsNumber(sync_video_batch_frames_j) ? sync_video_batch_frames_j->valueint : 16;
    int saved_sync_frame_lead_ms =
        cJSON_IsNumber(sync_frame_lead_ms_j) ? sync_frame_lead_ms_j->valueint : 2500;
    cJSON_Delete(root);

    current_title_ = saved_title;
    stream_url_    = saved_url;
    current_id_    = item_id;
    sync_frame_url_ = saved_sync_frame_url;
    sync_audio_url_ = saved_sync_audio_url;
    ws_stream_url_  = saved_ws_stream_url;
    use_sync_media_api_ = use_sync_media_api;
    use_ws_media_api_   = use_ws_media_api;
    current_duration_ms_ = saved_duration_ms;
    sync_audio_packet_ms_ = saved_sync_audio_packet_ms;
    sync_audio_batch_packets_ = saved_sync_audio_batch_packets;
    sync_video_batch_frames_ = saved_sync_video_batch_frames;
    sync_frame_lead_ms_ = saved_sync_frame_lead_ms;

    Application::GetInstance().Schedule([this, saved_url, saved_title, item_id,
                                         use_sync_media_api, use_ws_media_api,
                                         saved_sync_frame_url, saved_sync_audio_url,
                                         saved_ws_stream_url, saved_duration_ms,
                                         saved_sync_audio_packet_ms,
                                         saved_sync_audio_batch_packets,
                                         saved_sync_video_batch_frames,
                                         saved_sync_frame_lead_ms]() {
        // Stop any conflicting players.
        MediaPlayer::GetInstance().Stop();
        FlappyBird::GetInstance().Stop();

        StopPlayback();

        current_title_ = saved_title;
        current_id_    = item_id;
        stream_url_    = saved_url;
        sync_frame_url_ = saved_sync_frame_url;
        sync_audio_url_ = saved_sync_audio_url;
        ws_stream_url_  = saved_ws_stream_url;
        use_sync_media_api_ = use_sync_media_api;
        use_ws_media_api_   = use_ws_media_api;
        current_duration_ms_ = saved_duration_ms;
        sync_audio_packet_ms_ = saved_sync_audio_packet_ms;
        sync_audio_batch_packets_ = saved_sync_audio_batch_packets;
        sync_video_batch_frames_ = saved_sync_video_batch_frames;
        sync_frame_lead_ms_ = saved_sync_frame_lead_ms;
        reader_done_.store(false);
        playback_started_.store(false);
        render_failed_.store(false);
        dropped_frames_ = 0;
        playback_start_us_ = 0;
        {
            std::lock_guard<std::mutex> lock(playback_stats_mutex_);
            playback_stats_ = {};
            playback_stats_.start_us = esp_timer_get_time();
            playback_end_reason_ = "in_progress";
            // CPU snapshot at playback start. The end-of-playback summary
            // diffs against this to compute per-core busy % AND the single
            // top CPU consumer over the playback window.
            CoreCpuStats start_cpu{};
            if (SystemInfo::GetCoreCpuStats(&start_cpu)) {
                playback_stats_.cpu_start.valid               = true;
                playback_stats_.cpu_start.total_runtime       = start_cpu.total_runtime;
                playback_stats_.cpu_start.core0_run_time      = start_cpu.core0_run_time;
                playback_stats_.cpu_start.core1_run_time      = start_cpu.core1_run_time;
                playback_stats_.cpu_start.core0_idle_run_time = start_cpu.core0_idle_run_time;
                playback_stats_.cpu_start.core1_idle_run_time = start_cpu.core1_idle_run_time;
            }
            playback_stats_.cpu_start_task_count = SystemInfo::CaptureTaskRunTimes(
                playback_stats_.cpu_start_tasks,
                PlaybackStats::kMaxTaskSnapshotEntries);
        }
        playback_stats_reported_ = false;
        last_progress_post_us_.store(0);
        state_ = State::kLoading;
        UpdateDisplay();
        Application::GetInstance().EndVoiceSessionForMedia();

        stream_task_handle_ = nullptr;
        render_task_handle_ = nullptr;

        // Stream task on core 0 alongside the network stack. With the WS
        // OnData callback now doing all parsing & dispatch on the tcp_receive
        // task, this task mostly just sleeps waiting for ws_done.
        //
        // Stack MUST go in PSRAM: with AFE audio + LVGL loaded, internal SRAM
        // is nearly exhausted and the plain xTaskCreatePinnedToCore (internal
        // stack) failed -> end_reason "stream_task_create_failed", 0 frames,
        // no playback. PSRAM has megabytes free here. WithCaps stack is sized
        // in BYTES (the classic API uses words), so multiply. WithCaps tasks
        // are static under the hood -> the task self-deletes with
        // vTaskDeleteWithCaps (see StreamReaderTask) or the PSRAM stack leaks.
        BaseType_t stream_task_ok = xTaskCreatePinnedToCoreWithCaps(
                                        StreamReaderTask, "video_stream",
                                        kStreamTaskStackWords * sizeof(StackType_t), this, 3,
                                        &stream_task_handle_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stream_task_ok != pdPASS || stream_task_handle_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create video stream task");
            state_ = State::kError;
            error_msg_ = "stream task create failed";
            SetPlaybackEndReason("stream_task_create_failed");
            reader_done_.store(true);
            playback_started_.store(true);
            video_queue_cv_.notify_all();
            UpdateDisplay();
            MaybeFinishPlayback();
            return;
        }

        {
            // Render task pinned to core 1: isolated from Wi-Fi/TCP work
            // (which are on core 0). JPEG decode + LCD push are the heaviest
            // CPU loads; giving them a dedicated core removes contention
            // with the network receive path. Now created for sync_v1 too (not
            // just WS): decoupling decode/present from the fetch loop is what
            // unlocks ~24fps — the old single-task sync path presented one
            // frame per fetch iteration (~11fps) and dropped the rest.
            // Stack in PSRAM (WithCaps, BYTES) — internal SRAM is exhausted
            // with AFE+LVGL; self-deletes via vTaskDeleteWithCaps.
            BaseType_t render_task_ok = xTaskCreatePinnedToCoreWithCaps(
                                            VideoRenderTask, "video_render",
                                            kRenderTaskStackWords * sizeof(StackType_t), this, 3,
                                            &render_task_handle_, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (render_task_ok != pdPASS || render_task_handle_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create video render task");
                state_ = State::kError;
                error_msg_ = "render task create failed";
                SetPlaybackEndReason("render_task_create_failed");
                stop_requested_.store(true);
                reader_done_.store(true);
                playback_started_.store(true);
                video_queue_cv_.notify_all();
                UpdateDisplay();
                return;
            }
        }
    });

    return std::string("");
}

// ── Stop ─────────────────────────────────────────────────────────────────────

void VideoPlayer::Stop() {
    if (state_ == State::kIdle) return;
    SetPlaybackEndReason("stop_requested");
    paused_.store(false);          // unblock stream task so it sees stop_requested_
    stop_requested_.store(true);
    video_queue_cv_.notify_all();
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void VideoPlayer::TogglePause() {
    if (state_ == State::kIdle) return;
    bool now_paused = !paused_.load();
    paused_.store(now_paused);
    if (now_paused) {
        Application::GetInstance().GetAudioService().ResetDecoder();
    }
    ESP_LOGI(TAG, "Playback %s", now_paused ? "paused" : "resumed");
}

// ── StopPlayback ──────────────────────────────────────────────────────────────

void VideoPlayer::StopPlayback() {
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        video_queue_.clear();
        playback_start_us_ = 0;
        dropped_frames_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(playback_stats_mutex_);
        playback_stats_ = {};
        playback_end_reason_.clear();
        playback_stats_reported_ = false;
    }
    last_progress_post_us_.store(0);
    stop_requested_.store(false);
    paused_.store(false);
    reader_done_.store(false);
    playback_started_.store(false);
    render_failed_.store(false);
    state_         = State::kIdle;
    current_title_ = "";
    current_id_    = "";
    stream_url_    = "";
    sync_frame_url_ = "";
    sync_audio_url_ = "";
    ws_stream_url_  = "";
    use_sync_media_api_ = false;
    use_ws_media_api_   = false;
    current_duration_ms_ = 0;
    sync_audio_packet_ms_ = 60;
    sync_audio_batch_packets_ = 96;
    sync_video_batch_frames_ = 16;
    sync_frame_lead_ms_ = 2500;
    error_msg_     = "";
    stream_task_handle_ = nullptr;
    render_task_handle_ = nullptr;

    // Free PSRAM framebuffers if allocated.
    if (frame_buf_a_) { heap_caps_free(frame_buf_a_); frame_buf_a_ = nullptr; }
    if (frame_buf_b_) { heap_caps_free(frame_buf_b_); frame_buf_b_ = nullptr; }

    // Tear down the video screen (requires LVGL lock).
    auto* display = Board::GetInstance().GetDisplay();
    if (display) {
        DisplayLockGuard lock(display);
        DestroyVideoScreen();
    }
    UpdateDisplay();
}

// ── CreateVideoScreen ─────────────────────────────────────────────────────────
// Must be called with LVGL lock held.

void VideoPlayer::CreateVideoScreen() {
    prev_screen_ = lv_scr_act();

    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, kFrameW, kFrameH);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    video_screen_ = screen;

    // Image object — will display each decoded JPEG frame.
    lv_obj_t* img = lv_image_create(screen);
    lv_obj_set_size(img, kFrameW, kFrameH);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    video_img_obj_ = img;

    lv_scr_load(screen);
}

// ── DestroyVideoScreen ────────────────────────────────────────────────────────
// Must be called with LVGL lock held.

void VideoPlayer::DestroyVideoScreen() {
    video_img_obj_ = nullptr;
    if (prev_screen_) {
        lv_scr_load_anim(static_cast<lv_obj_t*>(prev_screen_),
                         LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
        prev_screen_  = nullptr;
        video_screen_ = nullptr;  // freed by auto_del
    } else if (video_screen_) {
        lv_obj_del(static_cast<lv_obj_t*>(video_screen_));
        video_screen_ = nullptr;
    }
}

// ── UpdateDisplay ─────────────────────────────────────────────────────────────

void VideoPlayer::UpdateDisplay() {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) return;

    switch (state_) {
        case State::kLoading:
            display->SetChatMessage("system",
                ("Loading: " + current_title_).c_str());
            break;
        case State::kError:
            display->SetChatMessage("system",
                ("Video error: " + error_msg_).c_str());
            break;
        case State::kIdle:
        default:
            display->SetChatMessage("system", "");
            break;
    }
    // kPlaying: display is the video screen itself — no chat message needed.
}

// ── StreamReaderTask ──────────────────────────────────────────────────────────
// FreeRTOS task: opens the .axv HTTP stream, pushes audio packets into
// AudioService and enqueues compressed JPEG frames for the render task.

void VideoPlayer::StreamReaderTask(void* arg) {
    auto* self = static_cast<VideoPlayer*>(arg);

    if (self->stop_requested_.load()) {
        self->stream_task_handle_ = nullptr;
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    // ── WebSocket streaming (ws_v1) ─────────────────────────────────────────
    // Single persistent connection. Server pushes interleaved binary frames:
    //   [1B type][4B ts_ms BE][payload]
    //   type 0x01 = video JPEG, 0x02 = opus audio packet
    // Text control frames: {"type":"hello"|"eos"|"error"}
    //
    // Parsing & dispatch happens DIRECTLY in the WS OnData callback (which
    // runs on the tcp_receive task). Audio push is blocking — when the
    // decode queue fills, the WS callback blocks, kernel TCP buffer fills,
    // server sees a zero-window and naturally stops sending. This is real
    // TCP backpressure with zero data loss.
    //
    // (Previous design used an intermediate raw_queue drained by this task.
    // That auto-dropped the OLDEST frames on overflow, which silently
    // discarded data whenever audio push blocked even briefly. Eliminating
    // the intermediate queue removes the silent-drop point and lets TCP
    // do its job.)
    if (self->use_ws_media_api_) {
        auto& audio = Application::GetInstance().GetAudioService();
        audio.EnableWakeWordDetection(false);

        auto network = Board::GetInstance().GetNetwork();
        auto ws = network->CreateWebSocket(2);
        if (!ws) {
            ESP_LOGE(TAG, "WS: CreateWebSocket failed");
            self->SetPlaybackEndReason("ws_create_failed");
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            self->playback_started_.store(true);
            self->stream_task_handle_ = nullptr;
            self->video_queue_cv_.notify_all();
            self->MaybeFinishPlayback();
            vTaskDeleteWithCaps(nullptr);
            return;
        }

        ws->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
        ws->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());

        std::atomic<bool> ws_done{false};
        std::atomic<bool> ws_error{false};
        std::atomic<size_t> ws_queued_frames{0};
        std::atomic<bool>   playback_anchor_set{false};
        std::atomic<size_t> audio_packets_buffered{0};

        ws->OnData([&](const char* data, size_t len, bool binary) {
            if (self->stop_requested_.load()) return;

            if (!binary) {
                cJSON* root = cJSON_ParseWithLength(data, len);
                if (!root) return;
                auto* type_j = cJSON_GetObjectItem(root, "type");
                if (cJSON_IsString(type_j)) {
                    if (strcmp(type_j->valuestring, "eos") == 0) {
                        ws_done.store(true);
                    } else if (strcmp(type_j->valuestring, "error") == 0) {
                        ws_error.store(true);
                        ws_done.store(true);
                    }
                }
                cJSON_Delete(root);
                return;
            }

            if (len < 5) return;
            const uint8_t* d = reinterpret_cast<const uint8_t*>(data);
            uint8_t type = d[0];
            uint32_t ts_ms = (static_cast<uint32_t>(d[1]) << 24) |
                             (static_cast<uint32_t>(d[2]) << 16) |
                             (static_cast<uint32_t>(d[3]) << 8)  |
                             (static_cast<uint32_t>(d[4]));
            const uint8_t* payload = d + 5;
            size_t payload_len = len - 5;
            int64_t now_us = esp_timer_get_time();

            if (type == 0x01) {
                auto frame = std::make_unique<QueuedVideoFrame>();
                frame->ts_ms = ts_ms;
                frame->jpeg.assign(payload, payload + payload_len);
                int queue_depth = 0;
                bool queue_overflow = false;
                {
                    std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                    if (self->video_queue_.size() >= kMaxQueuedVideoFrames) {
                        self->video_queue_.pop_front();
                        self->dropped_frames_++;
                        queue_overflow = true;
                    }
                    self->video_queue_.push_back(std::move(frame));
                    queue_depth = static_cast<int>(self->video_queue_.size());
                    self->video_queue_cv_.notify_all();
                }
                {
                    std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                    self->playback_stats_.video_frames_seen++;
                    self->playback_stats_.video_bytes_seen += payload_len;
                    self->playback_stats_.max_queued_video_frames =
                        std::max(self->playback_stats_.max_queued_video_frames,
                                 static_cast<size_t>(queue_depth));
                    if (queue_overflow) self->playback_stats_.queue_overflow_drop_count++;
                    self->playback_stats_.dropped_frames = self->dropped_frames_;
                    if (self->playback_stats_.first_video_frame_us == 0 &&
                        self->playback_stats_.start_us > 0) {
                        self->playback_stats_.first_video_frame_us =
                            now_us - self->playback_stats_.start_us;
                    }
                }
                ws_queued_frames++;
            } else if (type == 0x02) {
                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate    = 24000;
                packet->frame_duration = 60;
                packet->timestamp      = ts_ms;
                packet->payload.assign(payload, payload + payload_len);
                // BLOCKING push: this is where TCP backpressure happens.
                // When the decode queue is full, this WS callback blocks,
                // the kernel TCP buffer fills (~24 KB), server gets a
                // zero-window ACK and pauses. No data loss.
                audio.PushPacketToDecodeQueue(std::move(packet), true);

                bool playback_started_now = false;
                {
                    std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                    if (!playback_anchor_set.load()) {
                        self->playback_start_us_ = now_us - (static_cast<int64_t>(ts_ms) * 1000);
                        playback_anchor_set.store(true);
                    }
                    if (!self->playback_started_.load()) {
                        size_t buffered = audio_packets_buffered.fetch_add(1) + 1;
                        constexpr size_t kWsAudioPrebufferPackets = 50;  // 50*60ms = 3.0s
                        constexpr size_t kWsVideoPrebufferFrames  = 20;  // ~2.5s at 8fps
                        if (buffered >= kWsAudioPrebufferPackets &&
                            self->video_queue_.size() >= kWsVideoPrebufferFrames) {
                            self->playback_started_.store(true);
                            playback_started_now = true;
                            self->video_queue_cv_.notify_all();
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                    self->playback_stats_.audio_packets_seen++;
                    self->playback_stats_.audio_bytes_seen += payload_len;
                    if (self->playback_stats_.first_audio_packet_us == 0 &&
                        self->playback_stats_.start_us > 0) {
                        self->playback_stats_.first_audio_packet_us =
                            now_us - self->playback_stats_.start_us;
                    }
                    if (playback_started_now &&
                        self->playback_stats_.playback_started_us == 0 &&
                        self->playback_stats_.start_us > 0) {
                        self->playback_stats_.playback_started_us =
                            now_us - self->playback_stats_.start_us;
                    }
                }
            }
        });

        ws->OnDisconnected([&]() {
            ws_done.store(true);
        });

        ESP_LOGI(TAG, "WS streaming from %s", self->ws_stream_url_.c_str());
        int64_t connect_start_us = esp_timer_get_time();
        bool connected = ws->Connect(self->ws_stream_url_.c_str());
        int64_t connect_elapsed_us = esp_timer_get_time() - connect_start_us;
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.http_open_time_us = connect_elapsed_us;
        }
        if (!connected) {
            ESP_LOGE(TAG, "WS connect failed err=%d", ws->GetLastError());
            self->SetPlaybackEndReason("ws_connect_failed");
            ws_error.store(true);
            ws_done.store(true);
        }

        // Stream task just waits for done/stop. Parsing & dispatch happens
        // in the WS OnData callback above (on the tcp_receive task).
        while (!self->stop_requested_.load() && !ws_done.load()) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ws->Close();
        ws.reset();

        if (!self->stop_requested_.load() && !ws_error.load()) {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            if (self->playback_end_reason_.empty() || self->playback_end_reason_ == "in_progress") {
                self->playback_end_reason_ = "ws_eos";
            }
        }

        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            if (!self->playback_started_.load()) {
                self->playback_started_.store(true);
            }
            self->stream_task_handle_ = nullptr;
            self->video_queue_cv_.notify_all();
        }
        ESP_LOGI(TAG, "WS stream done: queued=%zu dropped=%zu stopped=%d",
                 static_cast<size_t>(ws_queued_frames.load()), self->dropped_frames_,
                 static_cast<int>(self->stop_requested_.load()));
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.dropped_frames = self->dropped_frames_;
        }
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    if (self->use_sync_media_api_) {
        auto& audio = Application::GetInstance().GetAudioService();

        // Disable wake word detection to prevent speaker audio from
        // feeding back through the microphone and resetting decode state.
        audio.EnableWakeWordDetection(false);

        ESP_LOGI(TAG, "Streaming video via sync API: frame=%s audio=%s",
                 self->sync_frame_url_.c_str(), self->sync_audio_url_.c_str());

        auto& board = Board::GetInstance();
        auto network = board.GetNetwork();
        constexpr int kSyncFrameIntervalMs = 125;
        constexpr int kSyncLoopDelayMs = 10;
        constexpr int kSyncStartupSettleDelayMs = 200;
        constexpr int kSyncTargetBufferedBatches = 2;
        constexpr int kSyncStartupPrebufferPackets = 24;
        constexpr int kSyncTargetBufferedFrameBatches = 2;

        size_t queued_frames = 0;
        size_t audio_packets_buffered = 0;
        uint32_t next_audio_ts_ms = 0;
        uint32_t next_video_fetch_ts_ms = 0;
        std::deque<QueuedVideoFrame> pending_video_frames;
        int64_t last_sync_activity_us = esp_timer_get_time();
        std::string sync_failure_reason = "sync_audio_fetch_failed";
        bool audio_finished = false;
        bool fatal_error = false;

        auto record_http_open = [&](int64_t elapsed_us) {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            if (self->playback_stats_.http_open_time_us == 0) {
                self->playback_stats_.http_open_time_us = elapsed_us;
            }
            if (elapsed_us >= kHttpReadStallWarnUs) {
                self->playback_stats_.http_read_stalls++;
                self->playback_stats_.max_http_read_stall_us =
                    std::max(self->playback_stats_.max_http_read_stall_us, elapsed_us);
            }
        };

        auto parse_timestamp_header = [](const std::string& value, uint32_t* out) -> bool {
            if (!out || value.empty()) {
                return false;
            }
            char* end = nullptr;
            unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (end == value.c_str() || (end && *end != '\0')) {
                return false;
            }
            *out = static_cast<uint32_t>(parsed);
            return true;
        };

        auto fetch_binary = [&](const std::string& url,
                                std::vector<uint8_t>& body,
                                std::string* frame_ts_header,
                                std::string* next_ts_header) -> bool {
            constexpr int kSyncHttpTimeoutMs = 15000;
            constexpr int kSyncHttpMaxAttempts = 3;

            for (int attempt = 1; attempt <= kSyncHttpMaxAttempts; ++attempt) {
                auto http = network->CreateHttp(0);
                http->SetTimeout(kSyncHttpTimeoutMs);
                http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
                http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
                http->SetHeader("Connection", "close");
                http->SetHeader("Cache-Control", "no-cache");

                int64_t http_open_start_us = esp_timer_get_time();
                bool opened = http->Open("GET", url);
                int64_t http_open_elapsed_us = esp_timer_get_time() - http_open_start_us;
                record_http_open(http_open_elapsed_us);
                if (!opened) {
                    ESP_LOGE(TAG, "Sync API HTTP open failed (attempt %d/%d): %s err=%d",
                             attempt, kSyncHttpMaxAttempts, url.c_str(), http->GetLastError());
                    sync_failure_reason = "sync_http_open_failed";
                    {
                        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                        self->playback_stats_.http_status_code = -1;
                    }
                    http->Close();
                    if (attempt < kSyncHttpMaxAttempts && !self->stop_requested_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(40));
                        continue;
                    }
                    return false;
                }

                int status_code = http->GetStatusCode();
                {
                    std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                    self->playback_stats_.http_status_code = status_code;
                }
                if (status_code != 200 && status_code != 204) {
                    ESP_LOGE(TAG, "Sync API HTTP %d (attempt %d/%d) for %s err=%d",
                             status_code, attempt, kSyncHttpMaxAttempts, url.c_str(),
                             http->GetLastError());
                    sync_failure_reason = status_code < 0
                        ? "sync_http_status_invalid"
                        : "sync_http_status_unexpected";
                    http->Close();
                    if (attempt < kSyncHttpMaxAttempts && status_code < 0 &&
                        !self->stop_requested_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(40));
                        continue;
                    }
                    return false;
                }

                if (frame_ts_header != nullptr) {
                    *frame_ts_header = http->GetResponseHeader("X-Frame-Timestamp-Ms");
                }
                if (next_ts_header != nullptr) {
                    std::string next_header = http->GetResponseHeader("X-Video-Next-Timestamp-Ms");
                    if (next_header.empty()) {
                        next_header = http->GetResponseHeader("X-Audio-Next-Timestamp-Ms");
                    }
                    *next_ts_header = next_header;
                }
                size_t expected_length = http->GetBodyLength();
                body.clear();
                if (expected_length > 0) {
                    body.reserve(expected_length);
                }

                char read_buf[1024];
                bool read_failed = false;
                while (!self->stop_requested_.load()) {
                    int64_t read_start_us = esp_timer_get_time();
                    int n = http->Read(read_buf, sizeof(read_buf));
                    int64_t read_elapsed_us = esp_timer_get_time() - read_start_us;
                    {
                        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                        self->playback_stats_.http_read_calls++;
                        self->playback_stats_.http_payload_read_calls++;
                        self->playback_stats_.payload_read_time_us_total += read_elapsed_us;
                        self->playback_stats_.http_read_time_us_total += read_elapsed_us;
                        self->playback_stats_.max_http_read_us =
                            std::max(self->playback_stats_.max_http_read_us, read_elapsed_us);
                        if (read_elapsed_us >= kHttpReadStallWarnUs) {
                            self->playback_stats_.http_read_stalls++;
                            self->playback_stats_.max_http_read_stall_us =
                                std::max(self->playback_stats_.max_http_read_stall_us, read_elapsed_us);
                        }
                        if (n > 0) {
                            self->playback_stats_.http_read_bytes += n;
                            if (expected_length > 0 &&
                                n < static_cast<int>(std::min(sizeof(read_buf),
                                                              expected_length - body.size()))) {
                                self->playback_stats_.http_read_short_calls++;
                            }
                        } else if (n == 0) {
                            self->playback_stats_.http_zero_reads++;
                        }
                    }

                    if (n > 0) {
                        body.insert(body.end(), read_buf, read_buf + n);
                        if (expected_length > 0 && body.size() >= expected_length) {
                            break;
                        }
                        continue;
                    }

                    if (n == 0) {
                        break;
                    }

                    if (!body.empty() && expected_length == 0) {
                        ESP_LOGW(TAG, "Sync API read ended after buffered timeout: %s", url.c_str());
                        break;
                    }

                    read_failed = true;
                    break;
                }

                http->Close();
                if (read_failed) {
                    ESP_LOGE(TAG, "Sync API read failed (attempt %d/%d) for %s after %u bytes err=%d",
                             attempt, kSyncHttpMaxAttempts, url.c_str(),
                             static_cast<unsigned>(body.size()), http->GetLastError());
                    sync_failure_reason = "sync_http_read_failed";
                    if (attempt < kSyncHttpMaxAttempts && !self->stop_requested_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(40));
                        continue;
                    }
                    return false;
                }
                if (expected_length > 0 && body.size() < expected_length) {
                    ESP_LOGE(TAG, "Sync API short body (attempt %d/%d) for %s (got=%u expected=%u)",
                             attempt, kSyncHttpMaxAttempts, url.c_str(),
                             static_cast<unsigned>(body.size()),
                             static_cast<unsigned>(expected_length));
                    sync_failure_reason = "sync_http_short_body";
                    if (attempt < kSyncHttpMaxAttempts && !self->stop_requested_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(40));
                        continue;
                    }
                    return false;
                }
                if (expected_length > 0 && body.empty()) {
                    ESP_LOGE(TAG, "Sync API empty body (attempt %d/%d) for %s (expected=%u)",
                             attempt, kSyncHttpMaxAttempts, url.c_str(),
                             static_cast<unsigned>(expected_length));
                    sync_failure_reason = "sync_http_empty_body";
                    if (attempt < kSyncHttpMaxAttempts && !self->stop_requested_.load()) {
                        vTaskDelay(pdMS_TO_TICKS(40));
                        continue;
                    }
                    return false;
                }
                return true;
            }
            return false;
        };

        auto fetch_audio_batch = [&](uint32_t start_ms) -> bool {
            std::vector<uint8_t> body;
            std::string next_ts_header;
            std::string url = self->sync_audio_url_ + "/" + std::to_string(start_ms) + "/" +
                              std::to_string(self->sync_audio_batch_packets_);
            if (!fetch_binary(url, body, nullptr, &next_ts_header)) {
                if (sync_failure_reason.empty()) {
                    sync_failure_reason = "sync_audio_fetch_failed";
                }
                return false;
            }
            if (body.empty()) {
                audio_finished = true;
                return true;
            }

            size_t offset = 0;
            while (offset + 16 <= body.size()) {
                uint32_t magic = ((uint32_t)body[offset] << 24) |
                                 ((uint32_t)body[offset + 1] << 16) |
                                 ((uint32_t)body[offset + 2] << 8) |
                                 (uint32_t)body[offset + 3];
                uint32_t frame_type = ((uint32_t)body[offset + 4] << 24) |
                                      ((uint32_t)body[offset + 5] << 16) |
                                      ((uint32_t)body[offset + 6] << 8) |
                                      (uint32_t)body[offset + 7];
                uint32_t payload_len = ((uint32_t)body[offset + 8] << 24) |
                                       ((uint32_t)body[offset + 9] << 16) |
                                       ((uint32_t)body[offset + 10] << 8) |
                                       (uint32_t)body[offset + 11];
                uint32_t ts_ms = ((uint32_t)body[offset + 12] << 24) |
                                 ((uint32_t)body[offset + 13] << 16) |
                                 ((uint32_t)body[offset + 14] << 8) |
                                 (uint32_t)body[offset + 15];
                offset += 16;

                if (magic != kFrameMagic || frame_type != kFrameAudio ||
                    offset + payload_len > body.size()) {
                    ESP_LOGE(TAG, "Malformed sync audio batch");
                    sync_failure_reason = "sync_audio_batch_malformed";
                    return false;
                }

                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate = 24000;
                packet->frame_duration = self->sync_audio_packet_ms_;
                packet->timestamp = ts_ms;
                int64_t copy_start_us = esp_timer_get_time();
                packet->payload.assign(body.begin() + offset, body.begin() + offset + payload_len);
                int64_t copy_elapsed_us = esp_timer_get_time() - copy_start_us;

                int64_t push_start_us = esp_timer_get_time();
                bool pushed = audio.PushPacketToDecodeQueue(std::move(packet), false);
                int64_t push_elapsed_us = esp_timer_get_time() - push_start_us;
                int64_t now_us = esp_timer_get_time();
                bool playback_started_now = false;
                if (!pushed) {
                    ESP_LOGE(TAG, "Sync audio queue full at ts=%u", ts_ms);
                    sync_failure_reason = "sync_audio_queue_full";
                    return false;
                }

                {
                    std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                    if (!self->playback_started_.load()) {
                        audio_packets_buffered++;
                        if (audio_packets_buffered >=
                            static_cast<size_t>(std::max(
                                kSyncStartupPrebufferPackets,
                                std::min(self->sync_audio_batch_packets_, 24)))) {
                            self->playback_started_.store(true);
                            playback_started_now = true;
                            self->video_queue_cv_.notify_all();
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                    self->playback_stats_.audio_packets_seen++;
                    self->playback_stats_.audio_bytes_seen += payload_len;
                    self->playback_stats_.audio_packet_copy_time_us_total += copy_elapsed_us;
                    self->playback_stats_.max_audio_packet_copy_us =
                        std::max(self->playback_stats_.max_audio_packet_copy_us, copy_elapsed_us);
                    self->playback_stats_.audio_push_time_us_total += push_elapsed_us;
                    self->playback_stats_.max_audio_push_us =
                        std::max(self->playback_stats_.max_audio_push_us, push_elapsed_us);
                    if (push_elapsed_us >= kAudioPushBlockWarnUs) {
                        self->playback_stats_.audio_push_block_count++;
                        self->playback_stats_.max_audio_push_block_us =
                            std::max(self->playback_stats_.max_audio_push_block_us, push_elapsed_us);
                    }
                    if (self->playback_stats_.first_audio_packet_us == 0 &&
                        self->playback_stats_.start_us > 0) {
                        self->playback_stats_.first_audio_packet_us =
                            now_us - self->playback_stats_.start_us;
                    }
                    if (playback_started_now &&
                        self->playback_stats_.playback_started_us == 0 &&
                        self->playback_stats_.start_us > 0) {
                        self->playback_stats_.playback_started_us =
                            now_us - self->playback_stats_.start_us;
                    }
                }

                next_audio_ts_ms = ts_ms + self->sync_audio_packet_ms_;
                offset += payload_len;
                last_sync_activity_us = now_us;
            }

            if (offset != body.size()) {
                sync_failure_reason = "sync_audio_batch_trailing_bytes";
                return false;
            }
            uint32_t next_ts_from_header = 0;
            if (parse_timestamp_header(next_ts_header, &next_ts_from_header)) {
                next_audio_ts_ms = next_ts_from_header;
            }
            return true;
        };

        uint8_t* render_buf_a = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint8_t* render_buf_b = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!render_buf_a || !render_buf_b) {
            if (render_buf_a) {
                heap_caps_free(render_buf_a);
            }
            if (render_buf_b) {
                heap_caps_free(render_buf_b);
            }
            ESP_LOGE(TAG, "Sync render buffers allocation failed");
            self->state_ = State::kError;
            self->error_msg_ = "out of memory";
            self->SetPlaybackEndReason("render_buffer_alloc_failed");
            Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
            audio.EnableWakeWordDetection(true);
            self->stream_task_handle_ = nullptr;
            self->MaybeFinishPlayback();
            vTaskDeleteWithCaps(nullptr);
            return;
        }

        bool screen_created = false;
        bool buf_a_is_display = true;
        size_t rendered_frames = 0;
        int64_t last_rendered_frame_ts_ms = -1;

        lv_image_dsc_t frame_dsc = {};
        frame_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        frame_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        frame_dsc.header.w      = static_cast<uint16_t>(kFrameW);
        frame_dsc.header.h      = static_cast<uint16_t>(kFrameH);
        frame_dsc.header.stride = static_cast<uint16_t>(kFrameW * 2);
        frame_dsc.data_size     = kFrameBytes;

        auto fetch_video_batch = [&](uint32_t start_ms) -> bool {
            std::vector<uint8_t> body;
            std::string next_ts_header;
            std::string url = self->sync_frame_url_ + "/" + std::to_string(start_ms) + "/" +
                              std::to_string(self->sync_video_batch_frames_);
            if (!fetch_binary(url, body, nullptr, &next_ts_header)) {
                return false;
            }
            if (body.empty()) {
                uint32_t next_ts_from_header = 0;
                if (parse_timestamp_header(next_ts_header, &next_ts_from_header)) {
                    next_video_fetch_ts_ms = next_ts_from_header;
                } else {
                    next_video_fetch_ts_ms =
                        start_ms + (self->sync_video_batch_frames_ * kSyncFrameIntervalMs);
                }
                return true;
            }

            size_t offset = 0;
            while (offset + 16 <= body.size()) {
                uint32_t magic = ((uint32_t)body[offset] << 24) |
                                 ((uint32_t)body[offset + 1] << 16) |
                                 ((uint32_t)body[offset + 2] << 8) |
                                 (uint32_t)body[offset + 3];
                uint32_t frame_type = ((uint32_t)body[offset + 4] << 24) |
                                      ((uint32_t)body[offset + 5] << 16) |
                                      ((uint32_t)body[offset + 6] << 8) |
                                      (uint32_t)body[offset + 7];
                uint32_t payload_len = ((uint32_t)body[offset + 8] << 24) |
                                       ((uint32_t)body[offset + 9] << 16) |
                                       ((uint32_t)body[offset + 10] << 8) |
                                       (uint32_t)body[offset + 11];
                uint32_t ts_ms = ((uint32_t)body[offset + 12] << 24) |
                                 ((uint32_t)body[offset + 13] << 16) |
                                 ((uint32_t)body[offset + 14] << 8) |
                                 (uint32_t)body[offset + 15];
                offset += 16;

                if (magic != kFrameMagic || frame_type != kFrameVideo ||
                    offset + payload_len > body.size()) {
                    ESP_LOGE(TAG, "Malformed sync video batch");
                    sync_failure_reason = "sync_video_batch_malformed";
                    return false;
                }

                if (ts_ms > static_cast<uint32_t>(std::max<int64_t>(0, last_rendered_frame_ts_ms)) &&
                    (pending_video_frames.empty() || ts_ms > pending_video_frames.back().ts_ms)) {
                    QueuedVideoFrame frame;
                    frame.ts_ms = ts_ms;
                    frame.jpeg.assign(body.begin() + offset, body.begin() + offset + payload_len);
                    pending_video_frames.push_back(std::move(frame));

                    int64_t now_us = esp_timer_get_time();
                    {
                        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                        self->playback_stats_.video_frames_seen++;
                        self->playback_stats_.video_bytes_seen += payload_len;
                        self->playback_stats_.max_queued_video_frames =
                            std::max(self->playback_stats_.max_queued_video_frames,
                                     pending_video_frames.size());
                        if (self->playback_stats_.first_video_frame_us == 0 &&
                            self->playback_stats_.start_us > 0) {
                            self->playback_stats_.first_video_frame_us =
                                now_us - self->playback_stats_.start_us;
                        }
                    }
                    queued_frames++;
                    last_sync_activity_us = now_us;
                }

                next_video_fetch_ts_ms = ts_ms + kSyncFrameIntervalMs;
                offset += payload_len;
            }

            if (offset != body.size()) {
                sync_failure_reason = "sync_video_batch_trailing_bytes";
                return false;
            }
            uint32_t next_ts_from_header = 0;
            if (parse_timestamp_header(next_ts_header, &next_ts_from_header)) {
                next_video_fetch_ts_ms = next_ts_from_header;
            }
            return true;
        };

        auto present_video_frame = [&](uint32_t frame_ts_ms,
                                       const std::vector<uint8_t>& jpeg,
                                       int64_t audio_clock_ms) -> bool {
            if (jpeg.empty()) {
                return true;
            }

            if (!screen_created) {
                auto* display = Board::GetInstance().GetDisplay();
                DisplayLockGuard lock(display);
                self->CreateVideoScreen();
                self->state_ = State::kPlaying;
                screen_created = true;
            }

            int64_t frame_timestamp_us = static_cast<int64_t>(frame_ts_ms) * 1000;
            int64_t age_before_decode_us =
                std::max<int64_t>(0, (audio_clock_ms * 1000) - frame_timestamp_us);
            {
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.video_frames_decode_attempted++;
                self->playback_stats_.total_frame_age_before_decode_us += age_before_decode_us;
                self->playback_stats_.max_frame_age_before_decode_us =
                    std::max(self->playback_stats_.max_frame_age_before_decode_us, age_before_decode_us);
            }

            uint8_t* decode_buf = buf_a_is_display ? render_buf_b : render_buf_a;
            size_t dec_len = 0;
            size_t w = 0;
            size_t h = 0;
            size_t stride = 0;
            int64_t decode_begin_us = esp_timer_get_time();
            esp_err_t ret = jpeg_to_image_into(jpeg.data(), jpeg.size(),
                                               decode_buf, kFrameBytes, &dec_len, &w, &h, &stride);
            int64_t decode_elapsed_us = esp_timer_get_time() - decode_begin_us;
            {
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.jpeg_decode_time_us_total += decode_elapsed_us;
                self->playback_stats_.max_jpeg_decode_us =
                    std::max(self->playback_stats_.max_jpeg_decode_us, decode_elapsed_us);
            }
            if (ret != ESP_OK || dec_len > kFrameBytes || w != kFrameW || h != kFrameH ||
                stride != (kFrameW * 2)) {
                ESP_LOGE(TAG, "Sync JPEG decode FAILED: ret=%d flen=%zu dec_len=%zu",
                         ret, jpeg.size(), dec_len);
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.video_frames_decode_failed++;
                sync_failure_reason = "frame_decode_failed";
                return false;
            }

            auto* display = Board::GetInstance().GetDisplay();
            int64_t present_start_us = esp_timer_get_time();
            bool frame_presented = display->PresentVideoFrameRGB565(decode_buf, dec_len, w, h, stride);
            if (!frame_presented) {
                DisplayLockGuard lock(display);
                if (self->video_img_obj_) {
                    buf_a_is_display = !buf_a_is_display;
                    frame_dsc.data = decode_buf;
                    lv_image_set_src(static_cast<lv_obj_t*>(self->video_img_obj_), &frame_dsc);
                    lv_obj_invalidate(static_cast<lv_obj_t*>(self->video_img_obj_));
                    frame_presented = true;
                }
            }
            int64_t present_elapsed_us = esp_timer_get_time() - present_start_us;
            int64_t presented_at_us = esp_timer_get_time();
            int64_t audio_after_present_ms = audio.GetPlaybackPositionMs();
            if (audio_after_present_ms < 0) {
                audio_after_present_ms = audio_clock_ms;
            }
            int64_t age_after_present_us =
                std::max<int64_t>(0, (audio_after_present_ms * 1000) - frame_timestamp_us);

            if (frame_presented) {
                rendered_frames++;
                last_rendered_frame_ts_ms = frame_ts_ms;
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.frame_present_time_us_total += present_elapsed_us;
                self->playback_stats_.max_frame_present_us =
                    std::max(self->playback_stats_.max_frame_present_us, present_elapsed_us);
                self->playback_stats_.video_frames_presented = rendered_frames;
                self->playback_stats_.rendered_frames = rendered_frames;
                self->playback_stats_.total_frame_age_after_present_us += age_after_present_us;
                self->playback_stats_.max_frame_age_after_present_us =
                    std::max(self->playback_stats_.max_frame_age_after_present_us, age_after_present_us);
                if (self->playback_stats_.first_frame_presented_us == 0 &&
                    self->playback_stats_.start_us > 0) {
                    self->playback_stats_.first_frame_presented_us =
                        presented_at_us - self->playback_stats_.start_us;
                }
            }

            return true;
        };

        if (kSyncStartupSettleDelayMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(kSyncStartupSettleDelayMs));
        }

        while (!self->stop_requested_.load()) {
            while (self->paused_.load() && !self->stop_requested_.load()) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (self->stop_requested_.load()) {
                break;
            }

            int64_t audio_clock_ms = audio.GetPlaybackPositionMs();
            int64_t frame_clock_ms = audio_clock_ms;
            if (frame_clock_ms < 0 && self->playback_started_.load() && next_audio_ts_ms > 0) {
                frame_clock_ms = std::max<int64_t>(
                    0, static_cast<int64_t>(next_audio_ts_ms) - self->sync_audio_packet_ms_);
            }
            int video_fetch_lead_ms = std::max(self->sync_frame_lead_ms_, 2500);
            {
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                int observed_stall_ms =
                    static_cast<int>(self->playback_stats_.max_http_read_stall_us / 1000);
                if (observed_stall_ms > 0) {
                    video_fetch_lead_ms = std::max(video_fetch_lead_ms, observed_stall_ms + 750);
                }
            }
            video_fetch_lead_ms = std::max(
                video_fetch_lead_ms,
                self->sync_video_batch_frames_ * kSyncFrameIntervalMs);
            video_fetch_lead_ms = std::min(video_fetch_lead_ms, 8000);

            if (self->playback_started_.load() && frame_clock_ms >= 0) {
                // Hand fetched frames to VideoRenderTask (core 1) via the shared
                // queue; it decodes + presents them timed to the audio clock and
                // drops late frames itself. We no longer decode/present inline —
                // that serialized fetch with render and capped playback at ~11fps
                // (one present per fetch iteration). Drop oldest only if the queue
                // is somehow saturated (render far behind).
                size_t buffered_now = 0;
                {
                    std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                    while (!pending_video_frames.empty()) {
                        if (self->video_queue_.size() >= kMaxQueuedVideoFrames) {
                            self->video_queue_.pop_front();
                            self->dropped_frames_++;
                        }
                        self->video_queue_.push_back(std::make_unique<QueuedVideoFrame>(
                            std::move(pending_video_frames.front())));
                        pending_video_frames.pop_front();
                    }
                    buffered_now = self->video_queue_.size();
                    self->video_queue_cv_.notify_all();
                }

                int target_buffered_frames = std::max(
                    self->sync_video_batch_frames_,
                    self->sync_video_batch_frames_ * kSyncTargetBufferedFrameBatches);
                if (buffered_now < static_cast<size_t>(target_buffered_frames)) {
                    int64_t desired_fetch_start_ms = frame_clock_ms + video_fetch_lead_ms;
                    int64_t batch_span_ms =
                        static_cast<int64_t>(self->sync_video_batch_frames_) * kSyncFrameIntervalMs;
                    if (next_video_fetch_ts_ms == 0 ||
                        static_cast<int64_t>(next_video_fetch_ts_ms) <
                            desired_fetch_start_ms - batch_span_ms) {
                        next_video_fetch_ts_ms =
                            static_cast<uint32_t>(std::max<int64_t>(0, desired_fetch_start_ms));
                    }
                    uint32_t fetch_start_ms = static_cast<uint32_t>(std::max<int64_t>(
                        0, std::max<int64_t>(next_video_fetch_ts_ms, desired_fetch_start_ms)));
                    if (self->current_duration_ms_ <= 0 ||
                        fetch_start_ms <= static_cast<uint32_t>(self->current_duration_ms_)) {
                        if (!fetch_video_batch(fetch_start_ms)) {
                            fatal_error = true;
                            self->SetPlaybackEndReason(sync_failure_reason.c_str());
                            break;
                        }
                    }
                }
                if (fatal_error) {
                    break;
                }
            }

            int64_t reference_ms = audio_clock_ms >= 0 ? audio_clock_ms : 0;
            int target_buffered_packets = std::max(
                kSyncStartupPrebufferPackets,
                self->sync_audio_batch_packets_ * kSyncTargetBufferedBatches);
            int64_t desired_audio_buffer_until_ms =
                reference_ms + static_cast<int64_t>(target_buffered_packets *
                                                    self->sync_audio_packet_ms_);

            if (!audio_finished && next_audio_ts_ms < desired_audio_buffer_until_ms) {
                if (!fetch_audio_batch(next_audio_ts_ms)) {
                    fatal_error = true;
                    self->SetPlaybackEndReason(sync_failure_reason.c_str());
                }
            }
            if (fatal_error) {
                break;
            }

            if (audio_finished && self->playback_started_.load()) {
                int64_t finished_at_ms = self->current_duration_ms_ > 0
                    ? self->current_duration_ms_
                    : static_cast<int64_t>(next_audio_ts_ms);
                if (audio_clock_ms >= finished_at_ms - self->sync_audio_packet_ms_) {
                    break;
                }
            }

            if (self->playback_started_.load() &&
                (audio_clock_ms >= 0 || !pending_video_frames.empty())) {
                self->MaybePostPlaybackProgress();
            }
            if (self->playback_started_.load() &&
                pending_video_frames.empty() &&
                (esp_timer_get_time() - last_sync_activity_us) > 2000000) {
                fatal_error = true;
                self->SetPlaybackEndReason("sync_start_stalled");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(kSyncLoopDelayMs));
        }

        if (screen_created) {
            Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        }
        heap_caps_free(render_buf_a);
        heap_caps_free(render_buf_b);
        if (!self->stop_requested_.load()) {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            if (self->playback_end_reason_.empty() || self->playback_end_reason_ == "in_progress") {
                self->playback_end_reason_ = fatal_error ? "sync_api_failed" : "stream_read_ended";
            }
        }

        audio.EnableWakeWordDetection(true);
        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            if (!self->playback_started_.load()) {
                self->playback_started_.store(true);
            }
            self->stream_task_handle_ = nullptr;
            self->video_queue_cv_.notify_all();
        }
        ESP_LOGI(TAG, "SyncMediaTask: rendered=%zu fetched=%zu stopped=%d",
                 rendered_frames, queued_frames, (int)self->stop_requested_.load());
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.dropped_frames = self->dropped_frames_;
        }
        if (fatal_error && self->state_ != State::kError) {
            self->state_ = State::kError;
            self->error_msg_ = "network error";
            Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        }
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    uint8_t* jpeg_buf  = static_cast<uint8_t*>(
        heap_caps_malloc(kJpegBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!jpeg_buf) {
        ESP_LOGE(TAG, "JPEG buffer allocation failed");
        self->state_ = State::kError;
        self->error_msg_ = "out of memory";
        self->SetPlaybackEndReason("jpeg_buffer_alloc_failed");
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->stream_task_handle_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            self->playback_started_.store(true);
            self->video_queue_cv_.notify_all();
        }
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    // Disable wake word detection to prevent the video's speaker audio from
    // feeding back through the microphone and triggering state changes that
    // call ResetDecoder(), which breaks the audio backpressure clock.
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);

    ESP_LOGI(TAG, "Streaming video: %s", self->stream_url_.c_str());

    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    std::unique_ptr<Http> http;
    int64_t stream_offset = 0;
    int64_t expected_stream_bytes = 0;

    auto parse_content_range_total = [](const std::string& value) -> int64_t {
        size_t slash = value.find('/');
        if (slash == std::string::npos || slash + 1 >= value.size()) {
            return 0;
        }
        return static_cast<int64_t>(std::strtoll(value.c_str() + slash + 1, nullptr, 10));
    };

    auto open_stream = [&](int64_t offset, bool is_resume) -> bool {
        http = network->CreateHttp(0);
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        if (offset > 0) {
            http->SetHeader("Range", "bytes=" + std::to_string(offset) + "-");
        }

        int64_t http_open_start_us = esp_timer_get_time();
        bool http_opened = http->Open("GET", self->stream_url_);
        int64_t http_open_elapsed_us = esp_timer_get_time() - http_open_start_us;
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            if (!is_resume && self->playback_stats_.http_open_time_us == 0) {
                self->playback_stats_.http_open_time_us = http_open_elapsed_us;
            }
            if (http_open_elapsed_us >= kHttpReadStallWarnUs) {
                self->playback_stats_.http_read_stalls++;
                self->playback_stats_.max_http_read_stall_us =
                    std::max(self->playback_stats_.max_http_read_stall_us, http_open_elapsed_us);
            }
        }

        if (!http_opened) {
            ESP_LOGE(TAG, "StreamReaderTask: HTTP open failed at offset=%lld",
                     static_cast<long long>(offset));
            return false;
        }

        int status_code = http->GetStatusCode();
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.http_status_code = status_code;
        }

        const int expected_status = offset > 0 ? 206 : 200;
        if (status_code != expected_status) {
            ESP_LOGE(TAG, "StreamReaderTask: HTTP %d at offset=%lld (expected %d)",
                     status_code, static_cast<long long>(offset), expected_status);
            http->Close();
            return false;
        }

        int64_t body_length = static_cast<int64_t>(http->GetBodyLength());
        if (offset == 0 && body_length > 0) {
            expected_stream_bytes = body_length;
        } else if (offset > 0) {
            int64_t range_total = parse_content_range_total(http->GetResponseHeader("Content-Range"));
            if (range_total > 0) {
                expected_stream_bytes = range_total;
            } else if (expected_stream_bytes > 0 && body_length > 0) {
                expected_stream_bytes = std::max(expected_stream_bytes, offset + body_length);
            }
        }
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.expected_stream_bytes = expected_stream_bytes;
        }

        return true;
    };

    if (!open_stream(0, false)) {
        heap_caps_free(jpeg_buf);
        self->state_ = State::kError;
        self->error_msg_ = "network error";
        self->SetPlaybackEndReason("stream_http_open_failed");
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        self->stream_task_handle_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            self->video_queue_cv_.notify_all();
        }
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    auto read_with_stats = [self, &http](uint8_t* dst, int len, bool is_header) -> int {
        int64_t read_start_us = esp_timer_get_time();
        int n = http->Read(reinterpret_cast<char*>(dst), len);
        int64_t read_elapsed_us = esp_timer_get_time() - read_start_us;
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.http_read_calls++;
            if (is_header) {
                self->playback_stats_.http_header_read_calls++;
                self->playback_stats_.header_read_time_us_total += read_elapsed_us;
            } else {
                self->playback_stats_.http_payload_read_calls++;
                self->playback_stats_.payload_read_time_us_total += read_elapsed_us;
            }
            self->playback_stats_.http_read_time_us_total += read_elapsed_us;
            self->playback_stats_.max_http_read_us =
                std::max(self->playback_stats_.max_http_read_us, read_elapsed_us);
            if (n > 0) {
                self->playback_stats_.http_read_bytes += n;
            } else {
                self->playback_stats_.http_zero_reads++;
            }
            if (n > 0 && n < len) {
                self->playback_stats_.http_read_short_calls++;
            }
            if (read_elapsed_us >= kHttpReadStallWarnUs) {
                self->playback_stats_.http_read_stalls++;
                self->playback_stats_.max_http_read_stall_us =
                    std::max(self->playback_stats_.max_http_read_stall_us, read_elapsed_us);
            }
        }
        return n;
    };

    int resume_attempts = 0;
    auto try_resume = [&]() -> bool {
        if (self->stop_requested_.load()) {
            return false;
        }
        if (resume_attempts >= kMaxStreamResumeAttempts) {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.stream_resume_failure_count++;
            return false;
        }

        resume_attempts++;
        {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.stream_resume_count++;
        }
        ESP_LOGW(TAG, "StreamReaderTask: resuming at offset=%lld attempt=%d",
                 static_cast<long long>(stream_offset), resume_attempts);

        if (http) {
            http->Close();
        }
        vTaskDelay(pdMS_TO_TICKS(kStreamResumeBackoffMs));

        if (!open_stream(stream_offset, true)) {
            std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
            self->playback_stats_.stream_resume_failure_count++;
            return false;
        }
        return true;
    };

    // Binary frame header: 4× uint32_t big-endian.
    uint8_t hdr[16];
    size_t  queued_frames = 0;
    size_t  audio_packets_buffered = 0;
    bool    playback_anchor_set = false;

    while (!self->stop_requested_.load()) {
        // Soft-pause: stall without closing the HTTP connection.
        while (self->paused_.load() && !self->stop_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (self->stop_requested_.load()) break;

        // Read 16-byte frame header.
        int got = 0;
        while (got < 16 && !self->stop_requested_.load()) {
            int n = read_with_stats(hdr + got, 16 - got, true);
            if (n < 0 || (n == 0 && expected_stream_bytes > 0 && stream_offset < expected_stream_bytes)) {
                if (try_resume()) continue;
                self->SetPlaybackEndReason("stream_resume_failed");
                goto stream_done;
            }
            if (n == 0) goto stream_done;
            got += n;
            stream_offset += n;
        }
        if (self->stop_requested_.load()) break;

        // Decode header (big-endian).
        uint32_t magic  = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                          ((uint32_t)hdr[2] <<  8) |  (uint32_t)hdr[3];
        uint32_t ftype  = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) |
                          ((uint32_t)hdr[6] <<  8) |  (uint32_t)hdr[7];
        uint32_t flen   = ((uint32_t)hdr[8] << 24) | ((uint32_t)hdr[9] << 16) |
                          ((uint32_t)hdr[10]<<  8) |  (uint32_t)hdr[11];
        uint32_t ts_ms  = ((uint32_t)hdr[12] << 24) | ((uint32_t)hdr[13] << 16) |
                          ((uint32_t)hdr[14]<<  8) |  (uint32_t)hdr[15];

        if (magic != kFrameMagic) {
            ESP_LOGE(TAG, "Frame sync lost (magic=0x%08" PRIx32 ")", magic);
            self->SetPlaybackEndReason("stream_bad_magic");
            break;
        }
        if (flen == 0 || flen > kJpegBufSize) {
            ESP_LOGE(TAG, "Bad frame length %" PRIu32, flen);
            self->SetPlaybackEndReason("stream_bad_frame_length");
            break;
        }

        // Read payload.
        uint32_t read = 0;
        while (read < flen && !self->stop_requested_.load()) {
            int n = read_with_stats(jpeg_buf + read, (int)(flen - read), false);
            if (n < 0 || (n == 0 && expected_stream_bytes > 0 && stream_offset < expected_stream_bytes)) {
                if (try_resume()) continue;
                self->SetPlaybackEndReason("stream_resume_failed");
                goto stream_done;
            }
            if (n == 0) goto stream_done;
            read += (uint32_t)n;
            stream_offset += n;
        }
        if (self->stop_requested_.load()) break;

        if (ftype == kFrameVideo) {
            auto frame = std::make_unique<QueuedVideoFrame>();
            frame->ts_ms = ts_ms;
            int64_t copy_start_us = esp_timer_get_time();
            frame->jpeg.assign(jpeg_buf, jpeg_buf + flen);
            int64_t copy_elapsed_us = esp_timer_get_time() - copy_start_us;
            int queue_depth = 0;
            bool queue_overflow = false;
            int64_t now_us = esp_timer_get_time();

            {
                std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                if (self->video_queue_.size() >= kMaxQueuedVideoFrames) {
                    self->video_queue_.pop_front();
                    self->dropped_frames_++;
                    queue_overflow = true;
                }
                self->video_queue_.push_back(std::move(frame));
                queue_depth = static_cast<int>(self->video_queue_.size());
                self->video_queue_cv_.notify_all();
            }
            {
                std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                self->playback_stats_.video_frames_seen++;
                self->playback_stats_.video_bytes_seen += flen;
                self->playback_stats_.video_frame_copy_time_us_total += copy_elapsed_us;
                self->playback_stats_.max_video_frame_copy_us =
                    std::max(self->playback_stats_.max_video_frame_copy_us, copy_elapsed_us);
                self->playback_stats_.max_queued_video_frames =
                    std::max(self->playback_stats_.max_queued_video_frames, static_cast<size_t>(queue_depth));
                if (queue_overflow) {
                    self->playback_stats_.queue_overflow_drop_count++;
                }
                self->playback_stats_.dropped_frames = self->dropped_frames_;
                if (self->playback_stats_.first_video_frame_us == 0 && self->playback_stats_.start_us > 0) {
                    self->playback_stats_.first_video_frame_us = now_us - self->playback_stats_.start_us;
                }
            }
            queued_frames++;
        } else if (ftype == kFrameAudio) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate    = 24000;
            packet->frame_duration = 60;
            packet->timestamp      = ts_ms;
            int64_t copy_start_us = esp_timer_get_time();
            packet->payload.assign(jpeg_buf, jpeg_buf + flen);
            int64_t copy_elapsed_us = esp_timer_get_time() - copy_start_us;
            int64_t push_start_us = esp_timer_get_time();
            audio.PushPacketToDecodeQueue(std::move(packet), true);
            int64_t push_elapsed_us = esp_timer_get_time() - push_start_us;
            int64_t now_us = esp_timer_get_time();
            bool playback_started_now = false;

            {
                std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                if (!playback_anchor_set) {
                    self->playback_start_us_ = now_us - (static_cast<int64_t>(ts_ms) * 1000);
                    playback_anchor_set = true;
                }
                if (!self->playback_started_.load()) {
                    audio_packets_buffered++;
                    if (audio_packets_buffered >= kAudioPrebufferPackets) {
                        self->playback_started_.store(true);
                        playback_started_now = true;
                        self->video_queue_cv_.notify_all();
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
                self->playback_stats_.audio_packets_seen++;
                self->playback_stats_.audio_bytes_seen += flen;
                self->playback_stats_.audio_packet_copy_time_us_total += copy_elapsed_us;
                self->playback_stats_.max_audio_packet_copy_us =
                    std::max(self->playback_stats_.max_audio_packet_copy_us, copy_elapsed_us);
                self->playback_stats_.audio_push_time_us_total += push_elapsed_us;
                self->playback_stats_.max_audio_push_us =
                    std::max(self->playback_stats_.max_audio_push_us, push_elapsed_us);
                if (push_elapsed_us >= kAudioPushBlockWarnUs) {
                    self->playback_stats_.audio_push_block_count++;
                    self->playback_stats_.max_audio_push_block_us =
                        std::max(self->playback_stats_.max_audio_push_block_us, push_elapsed_us);
                }
                if (self->playback_stats_.first_audio_packet_us == 0 && self->playback_stats_.start_us > 0) {
                    self->playback_stats_.first_audio_packet_us = now_us - self->playback_stats_.start_us;
                }
                if (playback_started_now && self->playback_stats_.playback_started_us == 0 &&
                    self->playback_stats_.start_us > 0) {
                    self->playback_stats_.playback_started_us = now_us - self->playback_stats_.start_us;
                }
            }
        } else {
            ESP_LOGW(TAG, "Unknown frame type %" PRIu32 ", skipping", ftype);
        }

        if (self->playback_started_.load()) {
            self->MaybePostPlaybackProgress();
        }
    }

stream_done:
    if (http) {
        http->Close();
    }
    heap_caps_free(jpeg_buf);

    if (!self->stop_requested_.load()) {
        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
        if (self->playback_end_reason_.empty() || self->playback_end_reason_ == "in_progress") {
            self->playback_end_reason_ = "stream_read_ended";
        }
    }

    {
        std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
        self->reader_done_.store(true);
        if (!self->playback_started_.load()) {
            self->playback_started_.store(true);
        }
        self->stream_task_handle_ = nullptr;
        self->video_queue_cv_.notify_all();
    }
    ESP_LOGI(TAG, "StreamReaderTask: queued=%zu dropped=%zu stopped=%d",
             queued_frames, self->dropped_frames_, (int)self->stop_requested_.load());
    {
        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
        self->playback_stats_.dropped_frames = self->dropped_frames_;
    }
    self->MaybeFinishPlayback();
    vTaskDeleteWithCaps(nullptr);
}

// ── VideoRenderTask ───────────────────────────────────────────────────────────
// FreeRTOS task: waits for playback to be primed, then decodes queued JPEGs and
// uses the frame timestamps to drop late frames instead of blocking audio.

void VideoPlayer::VideoRenderTask(void* arg) {
    auto* self = static_cast<VideoPlayer*>(arg);

    self->frame_buf_a_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    self->frame_buf_b_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!self->frame_buf_a_ || !self->frame_buf_b_) {
        ESP_LOGE(TAG, "Render buffers allocation failed");
        if (self->frame_buf_a_) { heap_caps_free(self->frame_buf_a_); self->frame_buf_a_ = nullptr; }
        if (self->frame_buf_b_) { heap_caps_free(self->frame_buf_b_); self->frame_buf_b_ = nullptr; }
        self->state_ = State::kError;
        self->error_msg_ = "out of memory";
        self->SetPlaybackEndReason("render_buffer_alloc_failed");
        self->render_task_handle_ = nullptr;
        self->render_failed_.store(true);
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->MaybeFinishPlayback();
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    self->buf_a_is_display_ = true;
    auto& audio_service = Application::GetInstance().GetAudioService();

    lv_image_dsc_t frame_dsc = {};
    frame_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    frame_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    frame_dsc.header.w      = (uint16_t)kFrameW;
    frame_dsc.header.h      = (uint16_t)kFrameH;
    frame_dsc.header.stride = (uint16_t)(kFrameW * 2);
    frame_dsc.data_size     = kFrameBytes;

    bool screen_created = false;
    size_t rendered_frames = 0;
    // Stall-resync: if no frame has been rendered for kRenderStallResyncUs,
    // assume audio_clock has drifted past the queued frames (WS hiccup, decoder
    // catch-up, etc.) and force-render the newest frame instead of waiting.
    int64_t last_present_us = 0;

    while (!self->stop_requested_.load()) {
        while (self->paused_.load() && !self->stop_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (self->stop_requested_.load()) {
            break;
        }

        std::unique_ptr<QueuedVideoFrame> frame;
        int backlog_drops = 0;
        int64_t future_wait_us = 0;
        int64_t frame_timestamp_us = 0;
        int64_t audio_clock_ms = -1;
        {
            std::unique_lock<std::mutex> lock(self->video_queue_mutex_);
            int64_t wait_start_us = esp_timer_get_time();
            self->video_queue_cv_.wait(lock, [self]() {
                return self->stop_requested_.load() ||
                       (self->playback_started_.load() && !self->video_queue_.empty()) ||
                       self->reader_done_.load();
            });
            int64_t wait_elapsed_us = esp_timer_get_time() - wait_start_us;
            {
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.render_wakeups++;
                self->playback_stats_.render_queue_wait_us_total += wait_elapsed_us;
                self->playback_stats_.max_render_queue_wait_us =
                    std::max(self->playback_stats_.max_render_queue_wait_us, wait_elapsed_us);
            }

            if (self->stop_requested_.load()) {
                break;
            }
            if (!self->playback_started_.load()) {
                continue;
            }
            if (self->video_queue_.empty()) {
                {
                    std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                    self->playback_stats_.render_empty_queue_wakeups++;
                }
                if (self->reader_done_.load()) {
                    break;
                }
                continue;
            }

            audio_clock_ms = audio_service.GetPlaybackPositionMs();
            if (audio_clock_ms < 0) {
                // Audio playback not yet running — wait briefly. Release the
                // lock first (same spin-loop bug fix as below).
                lock.unlock();
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            // STALL-RESYNC: if we've been stuck for kRenderStallResyncUs (1.5s)
            // with no successful render, drop the entire queue except the newest
            // frame and force-render that one. This unblocks the "video frozen,
            // audio continued" case the user observed.
            int64_t stall_now_us = esp_timer_get_time();
            if (last_present_us > 0 &&
                (stall_now_us - last_present_us) > kRenderStallResyncUs &&
                !self->video_queue_.empty()) {
                size_t popped = self->video_queue_.size() - 1;
                while (self->video_queue_.size() > 1) {
                    self->video_queue_.pop_front();
                    self->dropped_frames_++;
                }
                backlog_drops += static_cast<int>(popped);
                ESP_LOGW(TAG, "Render stalled %lld ms — resync, dropped %zu frames",
                         (long long)((stall_now_us - last_present_us) / 1000), popped);
                // Anchor audio_clock to the surviving frame so the selector
                // picks it (it's "future" from the previous audio_clock but we
                // want it rendered NOW).
                audio_clock_ms =
                    static_cast<int64_t>(self->video_queue_.front()->ts_ms) -
                    (kResyncLeadUs / 1000);
            } else {
                while (!self->video_queue_.empty()) {
                    int64_t front_timestamp_us =
                        static_cast<int64_t>(self->video_queue_.front()->ts_ms) * 1000;
                    if (front_timestamp_us + kLateFrameDropUs < (audio_clock_ms * 1000)) {
                        self->video_queue_.pop_front();
                        backlog_drops++;
                        self->dropped_frames_++;
                        continue;
                    }
                    break;
                }
            }
            if (self->video_queue_.empty()) {
                {
                    std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                    self->playback_stats_.render_empty_queue_wakeups++;
                    self->playback_stats_.render_backlog_drop_count += backlog_drops;
                    self->playback_stats_.dropped_frames = self->dropped_frames_;
                }
                if (self->reader_done_.load()) {
                    break;
                }
                continue;
            }

            int selected_index = -1;
            for (size_t i = 0; i < self->video_queue_.size(); ++i) {
                int64_t queued_timestamp_us =
                    static_cast<int64_t>(self->video_queue_[i]->ts_ms) * 1000;
                if (queued_timestamp_us <= (audio_clock_ms * 1000) + kFrameSelectionLeadUs) {
                    selected_index = static_cast<int>(i);
                } else {
                    break;
                }
            }

            if (selected_index < 0) {
                int64_t front_timestamp_us =
                    static_cast<int64_t>(self->video_queue_.front()->ts_ms) * 1000;
                future_wait_us = std::max<int64_t>(1000, std::min<int64_t>(
                    front_timestamp_us - (audio_clock_ms * 1000), kFutureFrameRecheckUs));
                if (backlog_drops > 0) {
                    std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                    self->playback_stats_.render_backlog_drop_count += backlog_drops;
                    self->playback_stats_.dropped_frames = self->dropped_frames_;
                }
                if (future_wait_us > 0) {
                    std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                    self->playback_stats_.render_schedule_sleep_us_total += future_wait_us;
                    self->playback_stats_.max_render_schedule_sleep_us =
                        std::max(self->playback_stats_.max_render_schedule_sleep_us, future_wait_us);
                }
                // SLEEP HERE before continue — the outer-loop vTaskDelay below
                // is unreachable from this branch (continue jumps over it),
                // which previously left this code in a tight spin loop on
                // core 1 burning ~30k spins/sec when frames were not yet
                // due to render. Release the lock first so producer (WS
                // OnData) isn't blocked on us while we wait.
                lock.unlock();
                if (future_wait_us > 0) {
                    TickType_t delay_ticks = DelayTicksForUs(future_wait_us);
                    if (delay_ticks > 0) vTaskDelay(delay_ticks);
                }
                continue;
            }

            backlog_drops += selected_index;
            while (selected_index-- > 0) {
                self->video_queue_.pop_front();
                self->dropped_frames_++;
            }
            frame_timestamp_us = static_cast<int64_t>(self->video_queue_.front()->ts_ms) * 1000;
            frame = std::move(self->video_queue_.front());
            self->video_queue_.pop_front();
        }
        if (backlog_drops > 0) {
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.render_backlog_drop_count += backlog_drops;
            self->playback_stats_.dropped_frames = self->dropped_frames_;
        }
        // Note: a previous branch (selected_index < 0) sleeps inline before
        // `continue`. When we reach here, a frame WAS selected and we just
        // popped it from the queue under lock — proceed straight to decode
        // and present.

        if (!screen_created) {
            auto* display = Board::GetInstance().GetDisplay();
            DisplayLockGuard lock(display);
            self->CreateVideoScreen();
            self->state_ = State::kPlaying;
            screen_created = true;
        }

        int64_t age_before_decode_us = std::max<int64_t>(0, (audio_clock_ms * 1000) - frame_timestamp_us);
        {
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.video_frames_decode_attempted++;
            self->playback_stats_.total_frame_age_before_decode_us += age_before_decode_us;
            self->playback_stats_.max_frame_age_before_decode_us =
                std::max(self->playback_stats_.max_frame_age_before_decode_us, age_before_decode_us);
        }
        uint8_t* decode_buf = self->buf_a_is_display_ ? self->frame_buf_b_ : self->frame_buf_a_;
        size_t dec_len = 0;
        size_t w = 0;
        size_t h = 0;
        size_t stride = 0;
        int64_t decode_begin_us = esp_timer_get_time();
        esp_err_t ret = jpeg_to_image_into(frame->jpeg.data(), frame->jpeg.size(),
                                           decode_buf, kFrameBytes, &dec_len, &w, &h, &stride);
        int64_t decode_elapsed_us = esp_timer_get_time() - decode_begin_us;
        {
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.jpeg_decode_time_us_total += decode_elapsed_us;
            self->playback_stats_.max_jpeg_decode_us =
                std::max(self->playback_stats_.max_jpeg_decode_us, decode_elapsed_us);
        }
        if (ret != ESP_OK || dec_len > kFrameBytes || w != kFrameW || h != kFrameH ||
            stride != (kFrameW * 2)) {
            ESP_LOGE(TAG, "JPEG decode FAILED frame %zu: ret=%d flen=%zu dec_len=%zu",
                     rendered_frames, ret, frame->jpeg.size(), dec_len);
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.video_frames_decode_failed++;
            continue;
        }

        auto* display = Board::GetInstance().GetDisplay();
        int64_t present_start_us = esp_timer_get_time();
        bool frame_presented = display->PresentVideoFrameRGB565(decode_buf, dec_len, w, h, stride);
        if (!frame_presented) {
            DisplayLockGuard lock(display);
            if (self->video_img_obj_) {
                self->buf_a_is_display_ = !self->buf_a_is_display_;
                frame_dsc.data = decode_buf;
                lv_image_set_src(static_cast<lv_obj_t*>(self->video_img_obj_), &frame_dsc);
                lv_obj_invalidate(static_cast<lv_obj_t*>(self->video_img_obj_));
                frame_presented = true;
            }
        }
        int64_t present_elapsed_us = esp_timer_get_time() - present_start_us;
        int64_t presented_at_us = esp_timer_get_time();
        int64_t audio_after_present_ms = audio_service.GetPlaybackPositionMs();
        if (audio_after_present_ms < 0) {
            audio_after_present_ms = audio_clock_ms;
        }
        int64_t age_after_present_us = std::max<int64_t>(0, (audio_after_present_ms * 1000) - frame_timestamp_us);
        if (frame_presented) {
            last_present_us = presented_at_us;  // for stall-resync detection
            rendered_frames++;
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.frame_present_time_us_total += present_elapsed_us;
            self->playback_stats_.max_frame_present_us =
                std::max(self->playback_stats_.max_frame_present_us, present_elapsed_us);
            self->playback_stats_.video_frames_presented = rendered_frames;
            self->playback_stats_.rendered_frames = rendered_frames;
            self->playback_stats_.total_frame_age_after_present_us += age_after_present_us;
            self->playback_stats_.max_frame_age_after_present_us =
                std::max(self->playback_stats_.max_frame_age_after_present_us, age_after_present_us);
            if (self->playback_stats_.first_frame_presented_us == 0 && self->playback_stats_.start_us > 0) {
                self->playback_stats_.first_frame_presented_us =
                    presented_at_us - self->playback_stats_.start_us;
            }
        }

        self->MaybePostPlaybackProgress();
    }

    ESP_LOGI(TAG, "VideoRenderTask: rendered=%zu dropped=%zu stopped=%d",
             rendered_frames, self->dropped_frames_, (int)self->stop_requested_.load());

    // Re-enable wake word detection that was suppressed during video playback.
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);

    {
        std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
        self->render_task_handle_ = nullptr;
        self->video_queue_cv_.notify_all();
    }
    self->MaybeFinishPlayback();
    vTaskDeleteWithCaps(nullptr);
}
