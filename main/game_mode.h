// "Game mode" — a reboot-selected boot personality for playing DOOM with the
// BLE controller. The ESP32-S3 can't fit the voice assistant (WiFi + audio) AND
// the BLE controller + DOOM in internal RAM at once, and the BLE controller
// memory release at boot is one-way. So "play doom with a controller" sets an
// NVS flag and reboots: game mode skips WiFi/network/assistant (freeing the RAM)
// and keeps the BLE controller, then auto-launches DOOM + connects the gamepad.
// Exiting DOOM reboots back to normal.
//
// Safety: the flag is one-shot — cleared at boot BEFORE the game-mode path runs,
// so if game mode ever crashes, the next boot is automatically normal.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Call once early in app_main (after nvs_flash_init). Reads + clears the flag.
void game_mode_init_from_nvs(void);

// True if this boot was requested as game mode.
bool game_mode_active(void);

// Set the flag and reboot into game mode (used by self.doom.start_controller).
void game_mode_request_and_reboot(void);

#ifdef __cplusplus
}
#endif
