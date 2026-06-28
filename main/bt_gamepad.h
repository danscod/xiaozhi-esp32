// BLE-HID gamepad host for the ShanWan Q36 (HID mode = standard HID-over-GATT).
//
// The ESP32-S3 is BLE-only; the Q36's "HID mode" advertises service 0x1812
// (HOGP), so we act as a BLE HID *host*: scan -> connect -> subscribe to input
// reports. NimBLE + esp_hidh do the heavy lifting. BLE is brought up on demand
// (not at boot) so the memory cost doesn't touch the normal chat/video path.
//
// Milestone 1: connect + log raw input reports (so we can learn the Q36's report
// layout). Decoding into bt_gamepad_state_t / DOOM events comes next.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decoded controller state, polled by the consumer (DOOM). Layout is refined
// once we see real reports; for now buttons/axes are best-effort.
typedef struct {
    uint32_t buttons;          // bitmask of pressed buttons (assignment TBD from reports)
    uint8_t  dpad;             // hat switch 0..7 (clockwise from up), 0x0f = centred
    int16_t  lx, ly, rx, ry;   // analog sticks, centred ~0
    uint32_t seq;              // bumped on every input report (freshness / change detect)
    bool     connected;
} bt_gamepad_state_t;

// Bring up NimBLE + esp_hidh and start scanning for / connecting to the Q36.
// Idempotent: a second call while already running is a no-op. Returns ESP_OK if
// the host came up (connection happens asynchronously via the scan task).
esp_err_t bt_gamepad_start(void);

// Disconnect and tear down the BLE host, freeing the controller memory.
void bt_gamepad_stop(void);

bool bt_gamepad_connected(void);

// Copy the latest decoded state. Safe to call from any task.
void bt_gamepad_get_state(bt_gamepad_state_t *out);

// Copy the latest RAW HID input report (for the on-device controller test
// screen). Returns true if a report is available; *out_len = bytes copied,
// *out_seq increments per report (for change detection). out_seq may be NULL.
bool bt_gamepad_get_raw(uint8_t *buf, size_t buflen, size_t *out_len, uint32_t *out_seq);

// HID report ID of the most recent input report (e.g. the gamepad report vs a
// separate consumer/system report for Home).
uint8_t bt_gamepad_raw_report_id(void);

// Register the self.controller.* MCP tools (pair / unpair / status).
void bt_gamepad_register_mcp(void);

#ifdef __cplusplus
}
#endif
