#pragma once

// Registers the experimental "control the paired iPhone" MCP tools and starts the
// BLE HID worker. Does nothing unless CONFIG_PASSPORT_PHONE_HID is enabled.
void InitializePhoneTools();
