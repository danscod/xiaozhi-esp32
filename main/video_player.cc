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

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
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

void VideoPlayer::MaybeFinishPlayback() {
    bool should_finish = false;
    {
        std::lock_guard<std::mutex> lock(video_queue_mutex_);
        should_finish = (stream_task_handle_ == nullptr &&
                         render_task_handle_ == nullptr);
    }
    if (should_finish) {
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
            cJSON_AddStringToObject(root, "state",         state_str);
            cJSON_AddStringToObject(root, "current_id",    current_id_.c_str());
            cJSON_AddStringToObject(root, "current_title", current_title_.c_str());
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
        // Abort any ongoing TTS.
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);

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
        state_ = State::kLoading;
        UpdateDisplay();

        xTaskCreate(StreamReaderTask, "video_stream", 20480, this, 2,
                    &stream_task_handle_);
        xTaskCreate(VideoRenderTask, "video_render", 20480, this, 2,
                    &render_task_handle_);
    });

    return std::string("");
}

// ── Stop ─────────────────────────────────────────────────────────────────────

void VideoPlayer::Stop() {
    if (state_ == State::kIdle) return;
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
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->stream_task_handle_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
            self->reader_done_.store(true);
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

    if (!http->Open("GET", self->stream_url_)) {
        ESP_LOGE(TAG, "StreamReaderTask: HTTP open failed");
        heap_caps_free(jpeg_buf);
        self->state_ = State::kError;
        self->error_msg_ = "network error";
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
    if (http->GetStatusCode() != 200) {
        int code = http->GetStatusCode();
        http->Close();
        ESP_LOGE(TAG, "StreamReaderTask: HTTP %d", code);
        heap_caps_free(jpeg_buf);
        self->state_ = State::kError;
        self->error_msg_ = "HTTP " + std::to_string(code);
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
            int n = http->Read(reinterpret_cast<char*>(hdr + got), 16 - got);
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
            break;
        }
        if (flen == 0 || flen > kJpegBufSize) {
            ESP_LOGE(TAG, "Bad frame length %" PRIu32, flen);
            break;
        }

        // Read payload.
        uint32_t read = 0;
        while (read < flen && !self->stop_requested_.load()) {
            int n = http->Read(reinterpret_cast<char*>(jpeg_buf + read), (int)(flen - read));
            if (n <= 0) goto stream_done;
            read += (uint32_t)n;
        }
        if (self->stop_requested_.load()) break;

        if (ftype == kFrameVideo) {
            auto frame = std::make_unique<QueuedVideoFrame>();
            frame->ts_ms = ts_ms;
            frame->jpeg.assign(jpeg_buf, jpeg_buf + flen);

            {
                std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                if (self->video_queue_.size() >= kMaxQueuedVideoFrames) {
                    self->video_queue_.pop_front();
                    self->dropped_frames_++;
                }
                self->video_queue_.push_back(std::move(frame));
                self->video_queue_cv_.notify_all();
            }
            queued_frames++;
        } else if (ftype == kFrameAudio) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate    = 24000;
            packet->frame_duration = 60;
            packet->timestamp      = ts_ms;
            packet->payload.assign(jpeg_buf, jpeg_buf + flen);
            audio.PushPacketToDecodeQueue(std::move(packet), true);

            {
                std::lock_guard<std::mutex> lock(self->video_queue_mutex_);
                if (!playback_anchor_set) {
                    self->playback_start_us_ = esp_timer_get_time() - (static_cast<int64_t>(ts_ms) * 1000);
                    playback_anchor_set = true;
                }
                if (!self->playback_started_.load()) {
                    audio_packets_buffered++;
                    if (audio_packets_buffered >= kAudioPrebufferPackets) {
                        self->playback_started_.store(true);
                        self->video_queue_cv_.notify_all();
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Unknown frame type %" PRIu32 ", skipping", ftype);
        }
    }

stream_done:
    http->Close();
    heap_caps_free(jpeg_buf);

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
    self->MaybeFinishPlayback();
    vTaskDelete(nullptr);
}

// ── VideoRenderTask ───────────────────────────────────────────────────────────
// FreeRTOS task: waits for playback to be primed, then decodes queued JPEGs and
// uses the frame timestamps to drop late frames instead of blocking audio.

void VideoPlayer::VideoRenderTask(void* arg) {
    auto* self = static_cast<VideoPlayer*>(arg);

    self->frame_buf_a_ = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    self->frame_buf_b_ = static_cast<uint8_t*>(
        heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!self->frame_buf_a_ || !self->frame_buf_b_) {
        ESP_LOGE(TAG, "Render buffers allocation failed");
        if (self->frame_buf_a_) { heap_caps_free(self->frame_buf_a_); self->frame_buf_a_ = nullptr; }
        if (self->frame_buf_b_) { heap_caps_free(self->frame_buf_b_); self->frame_buf_b_ = nullptr; }
        self->state_ = State::kError;
        self->error_msg_ = "out of memory";
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
        {
            std::unique_lock<std::mutex> lock(self->video_queue_mutex_);
            self->video_queue_cv_.wait(lock, [self]() {
                return self->stop_requested_.load() ||
                       (self->playback_started_.load() && !self->video_queue_.empty()) ||
                       self->reader_done_.load();
            });

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
                if (self->reader_done_.load()) {
                    break;
                }
                continue;
            }
            frame = std::move(self->video_queue_.front());
            self->video_queue_.pop_front();
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
        if (target_us + kLateFrameDropUs < now_us) {
            self->dropped_frames_++;
            continue;
        }
        if (target_us > now_us) {
            int64_t sleep_us = target_us - now_us;
            vTaskDelay(pdMS_TO_TICKS((sleep_us + 999) / 1000));
        }

        uint8_t* decode_buf = self->buf_a_is_display_ ? self->frame_buf_b_ : self->frame_buf_a_;
        uint8_t* decoded = nullptr;
        size_t dec_len = 0;
        size_t w = 0;
        size_t h = 0;
        size_t stride = 0;
        esp_err_t ret = jpeg_to_image(frame->jpeg.data(), frame->jpeg.size(),
                                      &decoded, &dec_len, &w, &h, &stride);
        if (ret != ESP_OK || decoded == nullptr || dec_len > kFrameBytes) {
            if (decoded) {
                heap_caps_free(decoded);
            }
            ESP_LOGE(TAG, "JPEG decode FAILED frame %zu: ret=%d flen=%zu dec_len=%zu",
                     rendered_frames, ret, frame->jpeg.size(), dec_len);
            continue;
        }

        memcpy(decode_buf, decoded, dec_len);
        heap_caps_free(decoded);

        auto* display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        if (self->video_img_obj_) {
            self->buf_a_is_display_ = !self->buf_a_is_display_;
            frame_dsc.data = decode_buf;
            lv_image_set_src(static_cast<lv_obj_t*>(self->video_img_obj_), &frame_dsc);
            lv_obj_invalidate(static_cast<lv_obj_t*>(self->video_img_obj_));
            rendered_frames++;
        }
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
