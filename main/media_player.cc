#include "media_player.h"
#include "mcp_server.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "audio/audio_service.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "display/display.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstring>

#define TAG "MediaPlayer"

// ── singleton ────────────────────────────────────────────────────────────────

MediaPlayer& MediaPlayer::GetInstance() {
    static MediaPlayer instance;
    return instance;
}

// ── MCP tool registration ─────────────────────────────────────────────────────

void MediaPlayer::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // ── self.media.search ────────────────────────────────────────────────────
    mcp.AddTool(
        "self.media.search",
        "Search the device media library for audio or video content to play. "
        "Returns a JSON array of matching items, each with an 'id', 'title', "
        "'description', 'type' (audio/video), and 'duration_s'. "
        "Use the returned 'id' with self.media.play to start playback. "
        "Call this first whenever the user asks to play something.",
        PropertyList({
            Property("query", kPropertyTypeString,
                     "Natural language search query, e.g. 'funny simpsons clip' "
                     "or 'relaxing music' or 'season 3 episode 1'.")
        }),
        [this](const PropertyList& props) -> ReturnValue {
            auto query = props["query"].value<std::string>();
            return FetchSearchResults(query);
        }
    );

    // ── self.media.play ──────────────────────────────────────────────────────
    mcp.AddTool(
        "self.media.play",
        "Start playing a media item on the device. "
        "Use the 'id' value returned by self.media.search. "
        "The device will download and play the item; audio plays through the "
        "speaker and the display shows the title.",
        PropertyList({
            Property("id", kPropertyTypeString,
                     "The media item ID from self.media.search results.")
        }),
        [this](const PropertyList& props) -> ReturnValue {
            auto id = props["id"].value<std::string>();
            return StartItem(id);
        }
    );

    // ── self.media.stop ──────────────────────────────────────────────────────
    mcp.AddTool(
        "self.media.stop",
        "Stop the currently playing media. Does nothing if nothing is playing.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            if (state_ == State::kIdle) {
                return std::string("Nothing is playing.");
            }
            Stop();
            return std::string("Stopped.");
        }
    );

    // ── self.media.status ────────────────────────────────────────────────────
    mcp.AddTool(
        "self.media.status",
        "Get the current media playback status.",
        PropertyList(),
        [this](const PropertyList& props) -> ReturnValue {
            cJSON* root = cJSON_CreateObject();
            const char* state_str = "idle";
            switch (state_) {
                case State::kLoading: state_str = "loading";  break;
                case State::kPlaying: state_str = "playing";  break;
                case State::kError:   state_str = "error";    break;
                default:              state_str = "idle";     break;
            }
            cJSON_AddStringToObject(root, "state",         state_str);
            cJSON_AddStringToObject(root, "current_id",    current_id_.c_str());
            cJSON_AddStringToObject(root, "current_title", current_title_.c_str());
            if (state_ == State::kError) {
                cJSON_AddStringToObject(root, "error", error_msg_.c_str());
            }
            // Returning cJSON* — MCP server serialises and frees it.
            return root;
        }
    );

    ESP_LOGI(TAG, "Registered 4 MCP tools (search, play, stop, status)");
}

// ── FetchSearchResults ────────────────────────────────────────────────────────
// Makes a GET request to the media search endpoint and returns the raw JSON
// string from the server (which the AI receives verbatim as the tool result).

std::string MediaPlayer::FetchSearchResults(const std::string& query) {
    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    auto  http    = network->CreateHttp(0);

    http->SetHeader("User-Agent",  SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",   SystemInfo::GetMacAddress().c_str());

    // URL-encode the query (simple: replace spaces with +)
    std::string encoded_query;
    for (char c : query) {
        if (c == ' ') encoded_query += '+';
        else          encoded_query += c;
    }
    std::string url = std::string(kSearchUrl) + "?q=" + encoded_query;

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "search: HTTP open failed");
        return R"({"error":"network error"})";
    }
    if (http->GetStatusCode() != 200) {
        int code = http->GetStatusCode();
        http->Close();
        ESP_LOGE(TAG, "search: HTTP %d", code);
        return R"({"error":"server error"})";
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

// ── StartItem ─────────────────────────────────────────────────────────────────
// Resolves the item ID to a stream URL (via server), stops any current
// playback, then queues the stream task.

std::string MediaPlayer::StartItem(const std::string& item_id) {
    // Resolve the item URL from the server.
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
    if (!root) {
        return "Error: bad response from media server.";
    }
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

    // Stop whatever is currently playing, then start the new stream.
    // Capture url/title/id because StopPlayback() clears them.
    Application::GetInstance().Schedule([this, saved_url, saved_title, item_id]() {
        // Abort any ongoing TTS so the AI voice doesn't play over the media.
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        StopPlayback();
        // Restore after StopPlayback cleared them.
        current_title_ = saved_title;
        current_id_    = item_id;
        stream_url_    = saved_url;
        state_ = State::kLoading;
        UpdateDisplay();

        // Spawn the streaming task — it runs independently and updates state
        // when it finishes.
        xTaskCreate(StreamTask, "media_stream", 16384, this, 2,
                    &stream_task_handle_);
    });

    // Return empty string so the AI skips TTS — avoids voice overlapping media audio.
    return std::string("");
}

// ── Stop (public) ────────────────────────────────────────────────────────────
// Safe to call from any context (button callbacks, etc.).

void MediaPlayer::Stop() {
    if (state_ == State::kIdle) return;
    paused_.store(false);          // unblock stream task so it sees stop_requested_
    stop_requested_.store(true);
    // Flush audio queues immediately so playback stops without waiting for the
    // buffer to drain naturally (which can take ~2 s at 40-frame buffer depth).
    // ResetDecoder() also notifies audio_queue_cv_ so any blocked
    // PushPacketToDecodeQueue call wakes and the StreamTask can exit promptly.
    Application::GetInstance().GetAudioService().ResetDecoder();
}

void MediaPlayer::TogglePause() {
    if (state_ == State::kIdle) return;
    bool now_paused = !paused_.load();
    paused_.store(now_paused);
    if (now_paused) {
        // Clear the already-buffered audio so playback stops immediately.
        // On resume the StreamTask continues streaming from its current HTTP
        // position so no data is lost — it just skips the queued-but-unplayed
        // buffer (at most ~2 s), which is the desired behaviour.
        Application::GetInstance().GetAudioService().ResetDecoder();
    }
    ESP_LOGI(TAG, "Playback %s", now_paused ? "paused" : "resumed");
}

// ── StopPlayback ──────────────────────────────────────────────────────────────

void MediaPlayer::StopPlayback() {
    // StreamTask self-terminates when it sees stop_requested_; we just reset
    // state here.  Do NOT vTaskDelete — the task may be blocked inside
    // PushPacketToDecodeQueue holding the audio mutex, making vTaskDelete
    // unsafe and deadlock-prone.
    stop_requested_.store(false);
    paused_.store(false);
    state_         = State::kIdle;
    current_title_ = "";
    current_id_    = "";
    stream_url_    = "";
    stream_task_handle_ = nullptr;
    UpdateDisplay();
}

// ── UpdateDisplay ─────────────────────────────────────────────────────────────

void MediaPlayer::UpdateDisplay() {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) return;

    switch (state_) {
        case State::kLoading:
            display->SetChatMessage("system",
                ("Loading: " + current_title_).c_str());
            break;
        case State::kPlaying:
            display->SetChatMessage("system",
                ("\u266a " + current_title_).c_str());  // ♪
            break;
        case State::kError:
            display->SetChatMessage("system",
                ("Media error: " + error_msg_).c_str());
            break;
        case State::kIdle:
        default:
            display->SetChatMessage("system", "");
            break;
    }
}

// ── StreamTask ────────────────────────────────────────────────────────────────
// Streams OGG Opus audio directly from HTTP to the audio decode queue.
// Uses Read() in a loop (not ReadAll()) to avoid the 8 KB backpressure
// deadlock in HttpClient. Feeds each chunk to OggDemuxer on the fly and
// pushes decoded Opus packets straight to PushPacketToDecodeQueue — no
// full-file PSRAM buffer required.

void MediaPlayer::StreamTask(void* arg) {
    auto* self = static_cast<MediaPlayer*>(arg);

    if (self->stop_requested_.load()) {
        Application::GetInstance().Schedule([self]() { self->StopPlayback(); });
        self->stream_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Streaming: %s", self->stream_url_.c_str());

    auto& board   = Board::GetInstance();
    auto  network = board.GetNetwork();
    auto  http    = network->CreateHttp(0);

    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Device-Id",  SystemInfo::GetMacAddress().c_str());

    if (self->stop_requested_.load()) {
        Application::GetInstance().Schedule([self]() { self->StopPlayback(); });
        self->stream_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (!http->Open("GET", self->stream_url_)) {
        ESP_LOGE(TAG, "StreamTask: HTTP open failed");
        self->state_     = State::kError;
        self->error_msg_ = "network error";
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->stream_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "StreamTask: HTTP %d", http->GetStatusCode());
        http->Close();
        self->state_     = State::kError;
        self->error_msg_ = "HTTP " + std::to_string(http->GetStatusCode());
        Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });
        self->stream_task_handle_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Switch display to "playing" now that the connection is open.
    self->state_ = State::kPlaying;
    Application::GetInstance().Schedule([self]() { self->UpdateDisplay(); });

    // Wire up the demuxer: each Opus packet is pushed directly to the
    // audio decode queue.  wait=true throttles the download to the
    // playback rate, which is fine here on a dedicated FreeRTOS task.
    auto& audio = Application::GetInstance().GetAudioService();
    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([self, &audio](const uint8_t* data, int sample_rate, size_t size) {
        if (self->stop_requested_.load()) return;
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate    = sample_rate;
        packet->frame_duration = 60;
        packet->payload.assign(data, data + size);
        audio.PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();

    // Read HTTP body in small chunks, feeding each into the demuxer.
    // Exit early if stop_requested_ is set (e.g. button press).
    char   chunk[1024];
    int    bytes;
    size_t total = 0;
    while (!self->stop_requested_.load()) {
        // Soft-pause: stall the HTTP read loop without closing the connection.
        while (self->paused_.load() && !self->stop_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (self->stop_requested_.load()) break;

        bytes = http->Read(chunk, sizeof(chunk));
        if (bytes <= 0) break;
        demuxer->Process(reinterpret_cast<const uint8_t*>(chunk), static_cast<size_t>(bytes));
        total += bytes;
    }
    http->Close();

    ESP_LOGI(TAG, "StreamTask: streamed %zu bytes (stopped=%d)",
             total, (int)self->stop_requested_.load());

    // Whether we finished naturally or were stopped, reset via StopPlayback.
    Application::GetInstance().Schedule([self]() { self->StopPlayback(); });

    self->stream_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}
