#pragma once

#include <atomic>

/**
 * FlappyBird — a simple Flappy Bird clone running on the LVGL display.
 *
 * Launched via MCP tool self.game.flappy — the AI triggers it on request.
 * The BOOT button (GPIO 0) flaps the bird while the game is active.
 * Game ends automatically when the bird hits a pipe or the floor/ceiling.
 *
 * Thread safety:
 *   - Start() / Stop() acquire the display LVGL lock (lvgl_port_lock).
 *   - Flap() only sets an atomic flag — safe to call from any task.
 *   - GameTick() runs inside the LVGL timer handler (lock already held).
 */
class FlappyBird {
public:
    static FlappyBird& GetInstance();

    // Register self.game.flappy MCP tool. Called from McpServer::AddCommonTools().
    void RegisterMcpTools();

    // Start or stop the game. Safe to call from any task.
    void Start();
    void Stop();

    // Signal a flap (bird jumps). Called from button callback; sets atomic flag.
    void Flap();

    bool IsActive() const { return active_.load(); }

private:
    FlappyBird() = default;
    FlappyBird(const FlappyBird&) = delete;
    FlappyBird& operator=(const FlappyBird&) = delete;

    // LVGL timer callback — runs every 20 ms inside lvgl_port task.
    static void GameTick(void* timer);
    void DoTick();

    // Create / destroy the LVGL screen and objects. Must be called with lock held.
    void CreateScreen();
    void DestroyScreen();
    void UpdatePositions();

    // Physics / state (only touched from GameTick, no extra locking needed).
    float    bird_y_     = 120.0f;
    float    vel_y_      =   0.0f;
    float    pipe_x_     = 240.0f;
    int      gap_y_      =  80;       // y of top edge of the gap
    int      score_      =   0;
    bool     passed_     = false;     // have we cleared the current pipe?

    // LVGL handles
    void*    game_screen_      = nullptr; // lv_obj_t*
    void*    bird_obj_         = nullptr;
    void*    bird_eye_         = nullptr; // child of bird_obj_
    void*    bird_beak_        = nullptr; // child of bird_obj_
    void*    pipe_top_         = nullptr;
    void*    pipe_top_cap_     = nullptr; // wider cap at gap edge of top pipe
    void*    pipe_bot_         = nullptr;
    void*    pipe_bot_cap_     = nullptr; // wider cap at gap edge of bottom pipe
    void*    floor_obj_        = nullptr;
    void*    score_label_      = nullptr;
    void*    game_timer_       = nullptr; // lv_timer_t*
    void*    game_over_overlay_= nullptr; // semi-transparent overlay shown on death
    void*    prev_screen_      = nullptr; // lv_obj_t* — restored on exit

    std::atomic<bool> active_          {false};
    std::atomic<bool> game_over_       {false}; // true = game-over overlay visible
    std::atomic<bool> flap_pending_    {false};
    std::atomic<bool> restart_pending_ {false}; // set by Flap() during game-over
};
