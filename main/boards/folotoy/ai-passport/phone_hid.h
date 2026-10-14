// BLE HID (keyboard + mouse + consumer keys) so the Passport can act as a
// Bluetooth keyboard/mouse for a paired iPhone. Only built when
// CONFIG_PASSPORT_PHONE_HID is enabled (the "ai-passport-phone" variant).
//
// Adapted from lewangdev/ai-passport-iphone-agent (MIT), hardware/ai_passport/main/agent_hid.c.
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_HID_OFF = 0,      // not started or failed to start
    PHONE_HID_ADVERTISING,  // waiting for the phone (pair once in iPhone Bluetooth settings)
    PHONE_HID_CONNECTED,    // connected, link not encrypted yet
    PHONE_HID_READY,        // encrypted, reports can be sent
} phone_hid_state_t;

// Starts NimBLE and the HID service and begins advertising. Call once, after nvs_flash_init().
esp_err_t phone_hid_start(const char* name);

phone_hid_state_t phone_hid_state(void);

// Mouse report: buttons bit0 = left. dx/dy/wheel are relative, -127..127.
bool phone_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

// Keyboard: presses modifiers + one key code, then releases everything.
bool phone_hid_key_tap(uint8_t modifiers, uint8_t keycode);

// Consumer control (media / volume): presses one usage, then releases it.
bool phone_hid_consumer_tap(uint16_t usage);

// Deletes all bonds and disconnects. The iPhone must also "Forget This Device".
esp_err_t phone_hid_forget(void);

#ifdef __cplusplus
}
#endif
