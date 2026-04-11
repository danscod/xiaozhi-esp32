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

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>

#define TAG "VideoPlayer"

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
    telemetry.audio_bytes_seen = playback_stats_.audio_bytes_seen;
    telemetry.video_bytes_seen = playback_stats_.video_bytes_seen;
    telemetry.http_read_time_us_total = playback_stats_.http_read_time_us_total;
    telemetry.header_read_time_us_total = playback_stats_.header_read_time_us_total;
    telemetry.payload_read_time_us_total = playback_stats_.payload_read_time_us_total;
    telemetry.audio_packet_copy_time_us_total = playback_stats_.audio_packet_copy_time_us_total;
    telemetry.video_frame_copy_time_us_total = playback_stats_.video_frame_copy_time_us_total;
    telemetry.audio_push_time_us_total = playback_stats_.audio_push_time_us_total;
    telemetry.render_queue_wait_us_total = playback_stats_.render_queue_wait_us_total;
    telemetry.render_schedule_sleep_us_total = playback_stats_.render_schedule_sleep_us_total;
    telemetry.total_frame_late_us = playback_stats_.total_frame_late_us;
    telemetry.total_frame_age_before_decode_us = playback_stats_.total_frame_age_before_decode_us;
    telemetry.total_frame_age_after_present_us = playback_stats_.total_frame_age_after_present_us;
    telemetry.jpeg_decode_time_us_total = playback_stats_.jpeg_decode_time_us_total;
    telemetry.frame_present_time_us_total = playback_stats_.frame_present_time_us_total;
    return telemetry;
}

void VideoPlayer::SetPlaybackEndReason(const char* reason) {
    std::lock_guard<std::mutex> lock(playback_stats_mutex_);
    playback_end_reason_ = reason ? reason : "";
}

void VideoPlayer::MaybePostPlaybackProgress() {
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
            Telemetry::GetInstance().PostVideoPlaybackStats(telemetry);
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
    if (!cJSON_IsString(title_j) || !cJSON_IsString(url_j)) {
        cJSON_Delete(root);
        return "Error: item metadata incomplete.";
    }
    std::string saved_title = title_j->valuestring;
    std::string saved_url   = url_j->valuestring;
    cJSON_Delete(root);

    current_title_ = saved_title;
    stream_url_    = saved_url;
    current_id_    = item_id;

    Application::GetInstance().Schedule([this, saved_url, saved_title, item_id]() {
        // Stop any conflicting players.
        MediaPlayer::GetInstance().Stop();
        FlappyBird::GetInstance().Stop();

        StopPlayback();

        current_title_ = saved_title;
        current_id_    = item_id;
        stream_url_    = saved_url;
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
        }
        playback_stats_reported_ = false;
        last_progress_post_us_.store(0);
        state_ = State::kLoading;
        UpdateDisplay();
        Application::GetInstance().EndVoiceSessionForMedia();

        xTaskCreate(StreamReaderTask, "video_stream", 20480, this, 4,
                    &stream_task_handle_);
        xTaskCreate(VideoRenderTask, "video_render", 20480, this, 2,
                    &render_task_handle_);
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
        vTaskDelete(nullptr);
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
        vTaskDelete(nullptr);
        return;
    }

    // Disable wake word detection to prevent the video's speaker audio from
    // feeding back through the microphone and triggering state changes that
    // call ResetDecoder(), which breaks the audio backpressure clock.
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);

    ESP_LOGI(TAG, "Streaming video: %s", self->stream_url_.c_str());

    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    auto  http    = network->CreateHttp(0);

    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());

    int64_t http_open_start_us = esp_timer_get_time();
    bool http_opened = http->Open("GET", self->stream_url_);
    int64_t http_open_elapsed_us = esp_timer_get_time() - http_open_start_us;
    {
        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
        self->playback_stats_.http_open_time_us = http_open_elapsed_us;
        if (http_open_elapsed_us >= kHttpReadStallWarnUs) {
            self->playback_stats_.http_read_stalls++;
            self->playback_stats_.max_http_read_stall_us =
                std::max(self->playback_stats_.max_http_read_stall_us, http_open_elapsed_us);
        }
    }

    if (!http_opened) {
        ESP_LOGE(TAG, "StreamReaderTask: HTTP open failed");
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
        vTaskDelete(nullptr);
        return;
    }
    int status_code = http->GetStatusCode();
    {
        std::lock_guard<std::mutex> lock(self->playback_stats_mutex_);
        self->playback_stats_.http_status_code = status_code;
    }
    if (status_code != 200) {
        int code = status_code;
        http->Close();
        ESP_LOGE(TAG, "StreamReaderTask: HTTP %d", code);
        heap_caps_free(jpeg_buf);
        self->state_ = State::kError;
        self->error_msg_ = "HTTP " + std::to_string(code);
        self->SetPlaybackEndReason("stream_http_status_error");
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        self->stream_task_handle_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
            self->video_queue_cv_.notify_all();
        }
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->MaybeFinishPlayback();
        vTaskDelete(nullptr);
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
            if (n <= 0) goto stream_done;
            got += n;
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
            if (n <= 0) goto stream_done;
            read += (uint32_t)n;
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
    http->Close();
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
    vTaskDelete(nullptr);
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
        vTaskDelete(nullptr);
        return;
    }
    self->buf_a_is_display_ = true;

    lv_image_dsc_t frame_dsc = {};
    frame_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    frame_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    frame_dsc.header.w      = (uint16_t)kFrameW;
    frame_dsc.header.h      = (uint16_t)kFrameH;
    frame_dsc.header.stride = (uint16_t)(kFrameW * 2);
    frame_dsc.data_size     = kFrameBytes;

    bool screen_created = false;
    int64_t playback_base_us = 0;
    size_t rendered_frames = 0;

    while (!self->stop_requested_.load()) {
        while (self->paused_.load() && !self->stop_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (self->stop_requested_.load()) {
            break;
        }

        std::unique_ptr<QueuedVideoFrame> frame;
        int backlog_drops = 0;
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
            if (playback_base_us == 0) {
                playback_base_us = self->playback_start_us_;
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

            if (self->video_queue_.size() > 1) {
                backlog_drops = static_cast<int>(self->video_queue_.size() - 1);
                self->dropped_frames_ += backlog_drops;
            }
            frame = std::move(self->video_queue_.back());
            self->video_queue_.clear();
        }
        if (backlog_drops > 0) {
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.render_backlog_drop_count += backlog_drops;
            self->playback_stats_.dropped_frames = self->dropped_frames_;
        }

        if (!screen_created) {
            auto* display = Board::GetInstance().GetDisplay();
            DisplayLockGuard lock(display);
            self->CreateVideoScreen();
            self->state_ = State::kPlaying;
            screen_created = true;
        }

        int64_t target_us = playback_base_us + (static_cast<int64_t>(frame->ts_ms) * 1000);
        int64_t now_us = esp_timer_get_time();
        if (target_us > now_us) {
            int64_t sleep_us = target_us - now_us;
            {
                std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
                self->playback_stats_.render_schedule_sleep_us_total += sleep_us;
                self->playback_stats_.max_render_schedule_sleep_us =
                    std::max(self->playback_stats_.max_render_schedule_sleep_us, sleep_us);
            }
            vTaskDelay(pdMS_TO_TICKS((sleep_us + 999) / 1000));
        } else if (target_us + kLateFrameDropUs < now_us) {
            int64_t late_us = now_us - target_us;
            std::lock_guard<std::mutex> stats_lock(self->playback_stats_mutex_);
            self->playback_stats_.late_frame_count++;
            self->playback_stats_.total_frame_late_us += late_us;
            self->playback_stats_.max_frame_late_us =
                std::max(self->playback_stats_.max_frame_late_us, late_us);
            ESP_LOGD(TAG, "Rendering late frame immediately (late=%lld ms)",
                     (long long)(late_us / 1000));
        }

        int64_t decode_start_us = esp_timer_get_time();
        int64_t age_before_decode_us = std::max<int64_t>(0, decode_start_us - target_us);
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
        bool frame_presented = false;
        {
            DisplayLockGuard lock(display);
            if (self->video_img_obj_) {
                self->buf_a_is_display_ = !self->buf_a_is_display_;
                frame_dsc.data = decode_buf;
                lv_image_set_src(static_cast<lv_obj_t*>(self->video_img_obj_), &frame_dsc);
                lv_obj_invalidate(static_cast<lv_obj_t*>(self->video_img_obj_));
                rendered_frames++;
                frame_presented = true;
            }
        }
        int64_t present_elapsed_us = esp_timer_get_time() - present_start_us;
        int64_t presented_at_us = esp_timer_get_time();
        int64_t age_after_present_us = std::max<int64_t>(0, presented_at_us - target_us);
        if (frame_presented) {
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
    vTaskDelete(nullptr);
}
