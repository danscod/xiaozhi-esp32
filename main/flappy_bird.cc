#include "flappy_bird.h"
#include "mcp_server.h"
#include "board.h"
#include "application.h"
#include "display/display.h"
#include "assets/lang_config.h"

#include <lvgl.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cstdio>
#include <algorithm>

#define TAG "FlappyBird"

// ── Game constants ────────────────────────────────────────────────────────────

static constexpr int   SCREEN_W   = 240;
static constexpr int   SCREEN_H   = 240;
static constexpr int   FLOOR_Y    = 218;   // y of top of floor strip
static constexpr int   GAMEH      = FLOOR_Y; // playfield height

static constexpr int   BIRD_X     =  50;   // fixed horizontal position
static constexpr int   BIRD_W     =  16;
static constexpr int   BIRD_H     =  16;

static constexpr int   PIPE_W     =  26;
static constexpr int   GAP_H      =  68;   // vertical gap between pipes
static constexpr int   PIPE_MIN_Y =  20;   // minimum gap_y
static constexpr float PIPE_SPEED =  2.0f; // pixels per tick

static constexpr float GRAVITY    =  9.8f;
static constexpr float TICK_S     =  0.020f; // 20 ms → 50 fps
static constexpr float JUMP_FORCE =  3.0f;   // upward velocity on flap

static constexpr int   TICK_MS    =  20;

// ── Colours ───────────────────────────────────────────────────────────────────

static constexpr uint32_t COL_SKY      = 0x87CEEB;
static constexpr uint32_t COL_BIRD     = 0xFFD700; // golden yellow body
static constexpr uint32_t COL_BIRD_EYE = 0xFFFFFF; // white eye
static constexpr uint32_t COL_BIRD_BEK = 0xFF8C00; // dark orange beak
static constexpr uint32_t COL_PIPE     = 0x4CAF50; // pipe body
static constexpr uint32_t COL_PIPE_CAP = 0x388E3C; // darker green cap
static constexpr uint32_t COL_FLOOR    = 0xC8A96E;
static constexpr uint32_t COL_SCORE    = 0xFFFFFF;

static constexpr int PIPE_CAP_W = 34; // cap is wider than the pipe shaft
static constexpr int PIPE_CAP_H = 10; // cap height

// ── Singleton ─────────────────────────────────────────────────────────────────

FlappyBird& FlappyBird::GetInstance() {
    static FlappyBird instance;
    return instance;
}

// ── MCP tool registration ─────────────────────────────────────────────────────

void FlappyBird::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.game.flappy",
        "Launch a Flappy Bird game on the device display. "
        "The user flaps the bird by pressing the chat button. "
        "The game ends automatically when the bird hits a pipe or the ground. "
        "Call this when the user asks to play Flappy Bird or any bird/flap game.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            if (active_.load()) {
                return std::string("Game already running.");
            }
            Application::GetInstance().Schedule([this]() {
                Start();
            });
            // Return empty string — suppresses AI TTS so speech doesn't play over the game.
            return std::string("");
        }
    );

    mcp.AddTool(
        "self.game.stop",
        "Stop the currently running game. Does nothing if no game is active.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            if (!active_.load()) {
                return std::string("No game is running.");
            }
            Application::GetInstance().Schedule([this]() {
                Stop();
            });
            return std::string("Game stopped.");
        }
    );

    ESP_LOGI(TAG, "Registered MCP tools (self.game.flappy, self.game.stop)");
}

// ── Start / Stop ──────────────────────────────────────────────────────────────

void FlappyBird::Start() {
    if (active_.load()) {
        ESP_LOGW(TAG, "Start() called while already active");
        return;
    }

    // Abort any ongoing TTS so AI speech doesn't play over the game.
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);

    // Reset state
    bird_y_      = GAMEH / 2.0f;
    vel_y_       = 0.0f;
    pipe_x_      = SCREEN_W + 20.0f;
    gap_y_       = GAMEH / 2 - GAP_H / 2;
    score_       = 0;
    passed_      = false;
    game_over_.store(false);
    flap_pending_.store(false);

    auto* display = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display);

    CreateScreen();

    // Timer fires every 20 ms — GameTick is called from LVGL task with lock held.
    game_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<FlappyBird*>(lv_timer_get_user_data(t))->DoTick();
        },
        TICK_MS, this);

    active_.store(true);
    ESP_LOGI(TAG, "Game started");
}

void FlappyBird::Stop() {
    if (!active_.load()) return;
    active_.store(false);

    auto* display = Board::GetInstance().GetDisplay();
    DisplayLockGuard lock(display);
    DestroyScreen();
    ESP_LOGI(TAG, "Game stopped");
}

// ── Flap ──────────────────────────────────────────────────────────────────────

void FlappyBird::Flap() {
    if (game_over_.load()) {
        // On the game-over screen: single press = restart.
        restart_pending_.store(true);
    } else {
        flap_pending_.store(true);
    }
}

// ── CreateScreen ─────────────────────────────────────────────────────────────
// Must be called with LVGL lock held.

void FlappyBird::CreateScreen() {
    prev_screen_ = lv_scr_act();

    game_screen_ = lv_obj_create(nullptr);
    lv_obj_set_size(static_cast<lv_obj_t*>(game_screen_), SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(game_screen_),
                              lv_color_hex(COL_SKY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(game_screen_), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(static_cast<lv_obj_t*>(game_screen_), LV_OBJ_FLAG_SCROLLABLE);

    // Floor
    floor_obj_ = lv_obj_create(static_cast<lv_obj_t*>(game_screen_));
    lv_obj_set_size(static_cast<lv_obj_t*>(floor_obj_), SCREEN_W, SCREEN_H - FLOOR_Y);
    lv_obj_set_pos(static_cast<lv_obj_t*>(floor_obj_), 0, FLOOR_Y);
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(floor_obj_),
                              lv_color_hex(COL_FLOOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(floor_obj_), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(static_cast<lv_obj_t*>(floor_obj_), 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(floor_obj_), 0, LV_PART_MAIN);
    lv_obj_set_style_radius(static_cast<lv_obj_t*>(floor_obj_), 0, LV_PART_MAIN);

    // ── Pipe shafts ──────────────────────────────────────────────────────────────
    auto make_pipe_shaft = [&](void*& obj) {
        obj = lv_obj_create(static_cast<lv_obj_t*>(game_screen_));
        lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(obj),
                                  lv_color_hex(COL_PIPE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(static_cast<lv_obj_t*>(obj), 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(obj), 0, LV_PART_MAIN);
        lv_obj_set_style_radius(static_cast<lv_obj_t*>(obj), 0, LV_PART_MAIN);
    };
    make_pipe_shaft(pipe_top_);
    make_pipe_shaft(pipe_bot_);

    // ── Pipe caps (darker, wider rectangles at the gap edges) ────────────────────
    auto make_pipe_cap = [&](void*& obj) {
        obj = lv_obj_create(static_cast<lv_obj_t*>(game_screen_));
        lv_obj_set_size(static_cast<lv_obj_t*>(obj), PIPE_CAP_W, PIPE_CAP_H);
        lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(obj),
                                  lv_color_hex(COL_PIPE_CAP), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(static_cast<lv_obj_t*>(obj), 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(obj), 0, LV_PART_MAIN);
        lv_obj_set_style_radius(static_cast<lv_obj_t*>(obj), 2, LV_PART_MAIN);
    };
    make_pipe_cap(pipe_top_cap_);
    make_pipe_cap(pipe_bot_cap_);

    // ── Bird body (circular) ──────────────────────────────────────────────────────
    bird_obj_ = lv_obj_create(static_cast<lv_obj_t*>(game_screen_));
    lv_obj_set_size(static_cast<lv_obj_t*>(bird_obj_), BIRD_W, BIRD_H);
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(bird_obj_),
                              lv_color_hex(COL_BIRD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(bird_obj_), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(static_cast<lv_obj_t*>(bird_obj_),
                                  lv_color_hex(0xCC8800), LV_PART_MAIN);
    lv_obj_set_style_border_width(static_cast<lv_obj_t*>(bird_obj_), 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(bird_obj_), 0, LV_PART_MAIN);
    lv_obj_set_style_radius(static_cast<lv_obj_t*>(bird_obj_), BIRD_W / 2, LV_PART_MAIN); // circle
    lv_obj_clear_flag(static_cast<lv_obj_t*>(bird_obj_), LV_OBJ_FLAG_SCROLLABLE);

    // Eye — small white circle, upper-right quadrant
    bird_eye_ = lv_obj_create(static_cast<lv_obj_t*>(bird_obj_));
    lv_obj_set_size(static_cast<lv_obj_t*>(bird_eye_), 5, 5);
    lv_obj_set_pos(static_cast<lv_obj_t*>(bird_eye_), 8, 3);
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(bird_eye_),
                              lv_color_hex(COL_BIRD_EYE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(bird_eye_), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(static_cast<lv_obj_t*>(bird_eye_),
                                  lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(static_cast<lv_obj_t*>(bird_eye_), 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(bird_eye_), 0, LV_PART_MAIN);
    lv_obj_set_style_radius(static_cast<lv_obj_t*>(bird_eye_), 3, LV_PART_MAIN); // circle

    // Beak — small orange rectangle, right side, mid-height
    bird_beak_ = lv_obj_create(static_cast<lv_obj_t*>(bird_obj_));
    lv_obj_set_size(static_cast<lv_obj_t*>(bird_beak_), 6, 4);
    lv_obj_set_pos(static_cast<lv_obj_t*>(bird_beak_), 11, 8);
    lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(bird_beak_),
                              lv_color_hex(COL_BIRD_BEK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(bird_beak_), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(static_cast<lv_obj_t*>(bird_beak_), 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(static_cast<lv_obj_t*>(bird_beak_), 0, LV_PART_MAIN);
    lv_obj_set_style_radius(static_cast<lv_obj_t*>(bird_beak_), 1, LV_PART_MAIN);

    // Score label
    score_label_ = lv_label_create(static_cast<lv_obj_t*>(game_screen_));
    lv_obj_set_style_text_color(static_cast<lv_obj_t*>(score_label_),
                                lv_color_hex(COL_SCORE), LV_PART_MAIN);
    lv_label_set_text(static_cast<lv_obj_t*>(score_label_), "0");
    lv_obj_align(static_cast<lv_obj_t*>(score_label_), LV_ALIGN_TOP_MID, 0, 6);

    UpdatePositions();
    lv_scr_load(static_cast<lv_obj_t*>(game_screen_));
}

// ── DestroyScreen ─────────────────────────────────────────────────────────────
// Must be called with LVGL lock held.

void FlappyBird::DestroyScreen() {
    if (game_timer_) {
        lv_timer_del(static_cast<lv_timer_t*>(game_timer_));
        game_timer_ = nullptr;
    }
    if (prev_screen_) {
        // Fade back to chat UI over 400 ms; auto_del=true frees game_screen_ after transition.
        lv_scr_load_anim(static_cast<lv_obj_t*>(prev_screen_),
                         LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
        prev_screen_ = nullptr;
        game_screen_ = nullptr; // will be freed by lv_scr_load_anim auto_del
    } else if (game_screen_) {
        lv_obj_del(static_cast<lv_obj_t*>(game_screen_));
        game_screen_       = nullptr;
        bird_obj_          = nullptr;
        bird_eye_          = nullptr;
        bird_beak_         = nullptr;
        pipe_top_          = nullptr;
        pipe_top_cap_      = nullptr;
        pipe_bot_          = nullptr;
        pipe_bot_cap_      = nullptr;
        floor_obj_         = nullptr;
        score_label_       = nullptr;
        game_over_overlay_ = nullptr;
    }
}

// ── UpdatePositions ───────────────────────────────────────────────────────────
// Sync LVGL object positions to game state. Called with lock held.

void FlappyBird::UpdatePositions() {
    // Bird
    lv_obj_set_pos(static_cast<lv_obj_t*>(bird_obj_),
                   BIRD_X,
                   static_cast<int>(bird_y_));

    // Upper pipe: from y=0 to y=gap_y_
    int px = static_cast<int>(pipe_x_);
    int cap_ox = (PIPE_CAP_W - PIPE_W) / 2; // horizontal offset so cap is centred on shaft
    lv_obj_set_size(static_cast<lv_obj_t*>(pipe_top_), PIPE_W, std::max(0, gap_y_ - PIPE_CAP_H));
    lv_obj_set_pos(static_cast<lv_obj_t*>(pipe_top_), px, 0);

    // Top-pipe cap: sits at the bottom of the top pipe shaft, facing downward into gap
    lv_obj_set_pos(static_cast<lv_obj_t*>(pipe_top_cap_), px - cap_ox, gap_y_ - PIPE_CAP_H);

    // Lower pipe: from y=gap_y_+GAP_H to FLOOR_Y
    int lower_y = gap_y_ + GAP_H;
    int lower_h = FLOOR_Y - lower_y - PIPE_CAP_H;
    if (lower_h < 0) lower_h = 0;
    lv_obj_set_size(static_cast<lv_obj_t*>(pipe_bot_), PIPE_W, lower_h);
    lv_obj_set_pos(static_cast<lv_obj_t*>(pipe_bot_), px, lower_y + PIPE_CAP_H);

    // Bottom-pipe cap: sits at the top of the bottom pipe shaft, facing upward into gap
    lv_obj_set_pos(static_cast<lv_obj_t*>(pipe_bot_cap_), px - cap_ox, lower_y);

    // Score
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", score_);
    lv_label_set_text(static_cast<lv_obj_t*>(score_label_), buf);
    lv_obj_align(static_cast<lv_obj_t*>(score_label_), LV_ALIGN_TOP_MID, 0, 6);
}

// ── DoTick ────────────────────────────────────────────────────────────────────
// Called from LVGL timer task every 20 ms. LVGL lock already held.

void FlappyBird::DoTick() {
    if (!active_.load()) return;

    // ── Game-over state: wait for restart or exit ─────────────────────────────
    if (game_over_.load()) {
        if (restart_pending_.exchange(false)) {
            // Remove overlay
            if (game_over_overlay_) {
                lv_obj_del(static_cast<lv_obj_t*>(game_over_overlay_));
                game_over_overlay_ = nullptr;
            }
            // Reset physics & state
            bird_y_   = GAMEH / 2.0f;
            vel_y_    = 0.0f;
            pipe_x_   = SCREEN_W + 20.0f;
            gap_y_    = GAMEH / 2 - GAP_H / 2;
            score_    = 0;
            passed_   = false;
            flap_pending_.store(false);
            game_over_.store(false);
            UpdatePositions();
        }
        return;
    }

    // Input
    if (flap_pending_.exchange(false)) {
        vel_y_ = -JUMP_FORCE;
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
    }

    // Physics
    vel_y_  += GRAVITY * TICK_S;
    bird_y_ += vel_y_;

    // Pipe movement
    pipe_x_ -= PIPE_SPEED;
    if (pipe_x_ < -PIPE_W) {
        // Pipe left screen — reset with new random gap
        pipe_x_ = static_cast<float>(SCREEN_W);
        int max_gap_y = GAMEH - GAP_H - PIPE_MIN_Y;
        gap_y_ = PIPE_MIN_Y + static_cast<int>(esp_random() % (max_gap_y - PIPE_MIN_Y));
        passed_ = false;
    }

    // Score: bird cleared the pipe
    int px = static_cast<int>(pipe_x_);
    if (!passed_ && px + PIPE_W < BIRD_X) {
        passed_ = true;
        score_++;
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
    }

    // Collision: floor / ceiling
    int by = static_cast<int>(bird_y_);
    bool dead = false;
    if (by + BIRD_H >= FLOOR_Y || by < 0) {
        dead = true;
    }

    // Collision: pipe (AABB)
    if (!dead) {
        int bx  = BIRD_X;
        int bx2 = bx + BIRD_W;
        int by2 = by + BIRD_H;
        int px2 = px + PIPE_W;
        if (bx2 > px && bx < px2) {
            // Horizontally overlapping with pipe column
            if (by < gap_y_ || by2 > gap_y_ + GAP_H) {
                dead = true;
            }
        }
    }

    UpdatePositions();

    if (dead) {
        ESP_LOGI(TAG, "Game over — score %d", score_);
        game_over_.store(true);
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        // Timer keeps running so DoTick() can handle restart input.

        // Show game over overlay — stays until press (restart) or long-press (exit).
        lv_obj_t* overlay = lv_obj_create(static_cast<lv_obj_t*>(game_screen_));
        lv_obj_set_size(overlay, 180, 110);
        lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_80, LV_PART_MAIN);
        lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(overlay, 8, LV_PART_MAIN);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        game_over_overlay_ = overlay;

        lv_obj_t* lbl = lv_label_create(overlay);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF4444), LV_PART_MAIN);
        lv_label_set_text_fmt(lbl,
            "GAME OVER\nScore: %d\n\n"
            "\xe2\x97\x8f press  restart\n"
            "\xe2\x97\x8f hold   exit",
            score_);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, 160);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }
}

// ── GameTick (static trampoline) ─────────────────────────────────────────────

void FlappyBird::GameTick(void* timer) {
    // Not used — lambda registered in Start() instead.
    (void)timer;
}
