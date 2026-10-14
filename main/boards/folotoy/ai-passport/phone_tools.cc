#include "phone_tools.h"

#include "sdkconfig.h"

#if CONFIG_PASSPORT_PHONE_HID

#include "mcp_server.h"
#include "phone_hid.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstring>
#include <string>

#define TAG "PhoneTools"

namespace {

// All HID traffic runs on one low-priority worker task, so MCP callbacks only enqueue and
// never block the main loop or the audio tasks. The queue is bounded on purpose.
enum class Op : uint8_t { kSwipe, kKey, kConsumer, kType, kForget };

struct Command {
    Op op;
    uint8_t modifiers;
    uint16_t code;
    char text[64];
};

constexpr int kQueueLength = 4;
constexpr int kStartDelayMs = 8000;  // let Wi-Fi and the audio channel come up first
QueueHandle_t g_queue = nullptr;

constexpr uint8_t kModShift = 0x02;
constexpr uint8_t kModGui = 0x08;  // Command key on iOS

void Delay(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void Move(int dx, int dy, int steps, int interval_ms, uint8_t buttons = 0) {
    for (int i = 0; i < steps; i++) {
        phone_hid_mouse(buttons, (int8_t)dx, (int8_t)dy, 0);
        Delay(interval_ms);
    }
}

// Relative mouse only: iOS AssistiveTouch treats a Bluetooth pointer as a mouse. The pointer
// is parked in the top-left corner first so the start of the drag is roughly known, then moved
// slowly (low pointer acceleration) to the start point. Positions are approximate.
void Swipe(char direction) {
    // Wake the pointer; the first movement after it hides only makes it visible again.
    phone_hid_mouse(0, 1, 0, 0);
    Delay(30);
    phone_hid_mouse(0, -1, 0, 0);
    Delay(30);
    Move(-127, -127, 12, 8);  // park in the top-left corner

    int start_x = 22, start_y = 42;  // steps of 8 points from the corner (centre of an SE screen)
    int dx = 0, dy = 0;
    switch (direction) {
        case 'u':  // finger moves up: next item / scroll down the page
            start_y = 56;
            dy = -10;
            break;
        case 'd':
            start_y = 22;
            dy = 10;
            break;
        case 'l':
            start_x = 38;
            dx = -10;
            break;
        case 'r':
            start_x = 6;
            dx = 10;
            break;
        default:
            return;
    }
    Move(8, 0, start_x, 10);
    Move(0, 8, start_y, 10);
    Delay(60);
    phone_hid_mouse(1, 0, 0, 0);  // press
    Delay(40);
    Move(dx, dy, 24, 12, 1);
    Delay(40);
    phone_hid_mouse(0, 0, 0, 0);  // release
}

// US keyboard usage for printable ASCII; returns false for characters it cannot type.
bool AsciiToKey(char c, uint8_t& mods, uint8_t& key) {
    static const char kShifted[] = "!@#$%^&*()";
    static const char kPunct[] = "-=[]\\;'`,./";
    static const uint8_t kPunctKeys[] = {0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33,
                                         0x34, 0x35, 0x36, 0x37, 0x38};
    static const char kPunctShifted[] = "_+{}|:\"~<>?";
    mods = 0;
    if (c >= 'a' && c <= 'z') {
        key = 0x04 + (c - 'a');
    } else if (c >= 'A' && c <= 'Z') {
        mods = kModShift;
        key = 0x04 + (c - 'A');
    } else if (c >= '1' && c <= '9') {
        key = 0x1E + (c - '1');
    } else if (c == '0') {
        key = 0x27;
    } else if (c == ' ') {
        key = 0x2C;
    } else if (c == '\n') {
        key = 0x28;
    } else if (const char* p = strchr(kShifted, c); p != nullptr && c != '\0') {
        mods = kModShift;
        key = (p - kShifted) == 9 ? 0x27 : 0x1E + (p - kShifted);
    } else if (const char* q = strchr(kPunct, c); q != nullptr && c != '\0') {
        key = kPunctKeys[q - kPunct];
    } else if (const char* r = strchr(kPunctShifted, c); r != nullptr && c != '\0') {
        mods = kModShift;
        key = kPunctKeys[r - kPunctShifted];
    } else {
        return false;
    }
    return true;
}

void LogMemory(const char* when) {
    ESP_LOGI(TAG, "[%s] free internal %u, min free %u, largest block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void Worker(void*) {
    Delay(kStartDelayMs);
    LogMemory("before BLE");
    esp_err_t err = phone_hid_start("XiaoZhi Passport");
    LogMemory("after BLE");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID failed to start: %s", esp_err_to_name(err));
    }
    Command cmd;
    while (true) {
        if (xQueueReceive(g_queue, &cmd, pdMS_TO_TICKS(60000)) != pdTRUE) {
            LogMemory("periodic");
            continue;
        }
        switch (cmd.op) {
            case Op::kSwipe:
                Swipe((char)cmd.code);
                break;
            case Op::kKey:
                phone_hid_key_tap(cmd.modifiers, (uint8_t)cmd.code);
                break;
            case Op::kConsumer:
                phone_hid_consumer_tap(cmd.code);
                break;
            case Op::kType:
                for (const char* p = cmd.text; *p; ++p) {
                    uint8_t mods, key;
                    if (AsciiToKey(*p, mods, key)) {
                        phone_hid_key_tap(mods, key);
                        Delay(20);
                    }
                }
                break;
            case Op::kForget:
                phone_hid_forget();
                break;
        }
    }
}

const char* StateText() {
    switch (phone_hid_state()) {
        case PHONE_HID_READY:
            return "ready";
        case PHONE_HID_CONNECTED:
            return "connecting";
        case PHONE_HID_ADVERTISING:
            return "not connected";
        default:
            return "bluetooth off";
    }
}

ToolResult Enqueue(const Command& cmd) {
    if (phone_hid_state() != PHONE_HID_READY && cmd.op != Op::kForget) {
        return std::unexpected(
            "The iPhone is not connected. On the iPhone open Settings > Bluetooth and tap "
            "\"XiaoZhi Passport\" (first time only), and turn on Settings > Accessibility > "
            "Touch > AssistiveTouch for swipes.");
    }
    if (xQueueSend(g_queue, &cmd, 0) != pdTRUE) {
        return std::unexpected("Busy with the previous phone action, try again in a moment.");
    }
    return true;
}

}  // namespace

void InitializePhoneTools() {
    g_queue = xQueueCreate(kQueueLength, sizeof(Command));
    xTaskCreate(Worker, "phone_hid", 3072, nullptr, 2, nullptr);

    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.phone.status",
                "Whether the user's iPhone is connected to this device over Bluetooth "
                "(experimental phone control).",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    return std::string("{\"phone\":\"") + StateText() + "\"}";
                });

    mcp.AddTool(
        "self.phone.key",
        "Press a key on the user's nearby iPhone over Bluetooth. action is one of: home (go to "
        "home screen), search (open Spotlight), play_pause, next_track, previous_track, "
        "volume_up, volume_down, mute. Use for requests like '手机回到桌面', '暂停', '下一首', "
        "'手机大声一点'.",
        PropertyList({Property("action", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            auto action = properties["action"].value<std::string>();
            Command cmd = {};
            if (action == "home") {
                cmd = {Op::kKey, kModGui, 0x0B, {}};  // Cmd+H
            } else if (action == "search") {
                cmd = {Op::kKey, kModGui, 0x2C, {}};  // Cmd+Space
            } else if (action == "play_pause") {
                cmd = {Op::kConsumer, 0, 0x00CD, {}};
            } else if (action == "next_track") {
                cmd = {Op::kConsumer, 0, 0x00B5, {}};
            } else if (action == "previous_track") {
                cmd = {Op::kConsumer, 0, 0x00B6, {}};
            } else if (action == "volume_up") {
                cmd = {Op::kConsumer, 0, 0x00E9, {}};
            } else if (action == "volume_down") {
                cmd = {Op::kConsumer, 0, 0x00EA, {}};
            } else if (action == "mute") {
                cmd = {Op::kConsumer, 0, 0x00E2, {}};
            } else {
                return std::unexpected("Unknown action: " + action);
            }
            return Enqueue(cmd);
        });

    mcp.AddTool(
        "self.phone.swipe",
        "Swipe on the user's nearby iPhone screen over Bluetooth (needs AssistiveTouch on). "
        "direction: up (next video / scroll down), down (previous video / scroll up), left, "
        "right. The touch position is approximate.",
        PropertyList({Property("direction", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            auto direction = properties["direction"].value<std::string>();
            if (direction != "up" && direction != "down" && direction != "left" &&
                direction != "right") {
                return std::unexpected("direction must be up, down, left or right");
            }
            return Enqueue({Op::kSwipe, 0, (uint16_t)direction[0], {}});
        });

    mcp.AddTool("self.phone.type",
                "Type text on the user's nearby iPhone with the Bluetooth keyboard. Only English "
                "letters, digits and common symbols (ASCII) can be typed; Chinese cannot.",
                PropertyList({Property("text", kPropertyTypeString)}),
                [](const PropertyList& properties) -> ToolResult {
                    auto text = properties["text"].value<std::string>();
                    Command cmd = {Op::kType, 0, 0, {}};
                    size_t n = 0;
                    for (char c : text) {
                        if ((unsigned char)c >= 0x80) {
                            return std::unexpected("Only ASCII text can be typed, not Chinese");
                        }
                        if (n + 1 < sizeof(cmd.text)) {
                            cmd.text[n++] = c;
                        }
                    }
                    return Enqueue(cmd);
                });

    mcp.AddTool("self.phone.forget",
                "Forget the paired iPhone (use when the user wants to pair a different phone). "
                "The user must also tap 'Forget This Device' in the iPhone Bluetooth settings.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    Command cmd = {Op::kForget, 0, 0, {}};
                    return Enqueue(cmd);
                });

    mcp.AddTool("self.device.memory",
                "Report free memory of this device (for testing the experimental phone "
                "control).",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    return "{\"free_internal\":" +
                           std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) +
                           ",\"min_free\":" + std::to_string(esp_get_minimum_free_heap_size()) +
                           ",\"largest_block\":" +
                           std::to_string(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)) +
                           "}";
                });
}

#else

void InitializePhoneTools() {}

#endif  // CONFIG_PASSPORT_PHONE_HID
