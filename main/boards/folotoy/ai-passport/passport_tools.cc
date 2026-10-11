#include "passport_tools.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TAG "PassportTools"

namespace {

// ---------------------------------------------------------------------------
// Ringtone. Everything here runs on the main task (timer callbacks only call
// Application::Schedule), so no locking is needed.
//
// The device rings locally instead of asking the server to speak the reminder:
// it works even when the conversation channel is closed, and nothing cancels it.

constexpr int kRingIntervalMs = 1500;
constexpr int kMaxRings = 20;           // about 30 seconds of ringing
constexpr int kMaxWaitTicks = 120;      // wait up to 3 minutes for the device to become idle
constexpr int kNotificationMs = 60000;  // keep the reminder text on screen for a minute

esp_timer_handle_t g_ring_timer = nullptr;
int g_rings = 0;
int g_wait_ticks = 0;

void StopRinging() {
    if (g_ring_timer != nullptr) {
        esp_timer_stop(g_ring_timer);
    }
    g_rings = 0;
    g_wait_ticks = 0;
}

void RingTick() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        // Once ringing has started, any state change means the user reacted (wake word or
        // button). Before that, the device is busy (e.g. talking): wait for it to finish.
        if (g_rings > 0 || ++g_wait_ticks > kMaxWaitTicks) {
            StopRinging();
        }
        return;
    }
    if (g_rings >= kMaxRings) {
        StopRinging();
        return;
    }
    if (g_rings == 0) {
        if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
            display->SetEmotion("surprised");
        }
    }
    g_rings++;
    // Two short chimes per ring, like a doorbell.
    app.PlaySound(Lang::Sounds::OGG_POPUP);
    app.PlaySound(Lang::Sounds::OGG_POPUP);
}

void StartRinging(const std::string& message) {
    if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
        display->ShowNotification("提醒：" + message, kNotificationMs);
    }
    if (g_ring_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { Application::GetInstance().Schedule(RingTick); };
        args.name = "reminder_ring";
        if (esp_timer_create(&args, &g_ring_timer) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ring timer");
            g_ring_timer = nullptr;
            return;
        }
    }
    StopRinging();
    esp_timer_start_periodic(g_ring_timer, kRingIntervalMs * 1000);
    RingTick();
}

// ---------------------------------------------------------------------------
// Reminders (kept in RAM; they are lost on reboot)

struct Reminder {
    int id = 0;
    int64_t due_us = 0;
    std::string message;
    esp_timer_handle_t timer = nullptr;

    ~Reminder() {
        if (timer != nullptr) {
            esp_timer_stop(timer);
            esp_timer_delete(timer);
        }
    }
};

constexpr size_t kMaxReminders = 10;
std::mutex g_reminders_mutex;
std::vector<std::unique_ptr<Reminder>> g_reminders;
int g_next_reminder_id = 1;

std::unique_ptr<Reminder> TakeReminder(int id) {
    std::lock_guard<std::mutex> lock(g_reminders_mutex);
    for (auto it = g_reminders.begin(); it != g_reminders.end(); ++it) {
        if ((*it)->id == id) {
            auto reminder = std::move(*it);
            g_reminders.erase(it);
            return reminder;
        }
    }
    return nullptr;
}

void FireReminder(int id) {
    auto reminder = TakeReminder(id);
    if (reminder == nullptr) {
        return;  // cancelled in the meantime
    }
    ESP_LOGI(TAG, "Reminder %d: %s", id, reminder->message.c_str());
    StartRinging(reminder->message);
}

std::string FormatDuration(int seconds) {
    if (seconds < 60) {
        return std::to_string(seconds) + "秒";
    }
    int minutes = (seconds + 30) / 60;
    if (minutes < 60) {
        return std::to_string(minutes) + "分钟";
    }
    int hours = minutes / 60;
    minutes %= 60;
    return std::to_string(hours) + "小时" + (minutes > 0 ? std::to_string(minutes) + "分" : "");
}

std::string RemindersJson() {
    std::lock_guard<std::mutex> lock(g_reminders_mutex);
    int64_t now = esp_timer_get_time();
    cJSON* list = cJSON_CreateArray();
    for (const auto& r : g_reminders) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r->id);
        cJSON_AddStringToObject(item, "message", r->message.c_str());
        cJSON_AddNumberToObject(item, "seconds_left", (double)((r->due_us - now) / 1000000));
        cJSON_AddItemToArray(list, item);
    }
    char* text = cJSON_PrintUnformatted(list);
    std::string result = text != nullptr ? text : "[]";
    cJSON_free(text);
    cJSON_Delete(list);
    return result;
}

}  // namespace

void InitializePassportTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.reminder.set",
        "Set a countdown reminder / alarm on this device (定时提醒、闹钟、倒计时). ALWAYS call "
        "this "
        "tool when the user asks to be reminded later; do not just reply in words. When it is due "
        "the device rings and shows the message on screen. Convert the request to seconds, e.g. "
        "'10分钟后提醒我喝水' -> seconds=600, message='喝水'. Reminders are lost if the device "
        "reboots.",
        PropertyList({Property("seconds", kPropertyTypeInteger, 5, 86400),
                      Property("message", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            int seconds = properties["seconds"].value<int>();
            auto message = properties["message"].value<std::string>();
            if (message.empty()) {
                message = "时间到了";
            } else if (message.size() > 120) {
                message.resize(120);  // may cut a UTF-8 character; the display tolerates it
            }

            auto reminder = std::make_unique<Reminder>();
            reminder->message = message;
            reminder->due_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;

            std::lock_guard<std::mutex> lock(g_reminders_mutex);
            if (g_reminders.size() >= kMaxReminders) {
                return std::unexpected("Too many reminders (max 10). Cancel one first.");
            }
            reminder->id = g_next_reminder_id++;

            esp_timer_create_args_t args = {};
            args.callback = [](void* arg) {
                int id = (int)(intptr_t)arg;
                Application::GetInstance().Schedule([id]() { FireReminder(id); });
            };
            args.arg = (void*)(intptr_t)reminder->id;
            args.name = "reminder";
            if (esp_timer_create(&args, &reminder->timer) != ESP_OK) {
                reminder->timer = nullptr;
                return std::unexpected("Failed to create timer");
            }
            esp_timer_start_once(reminder->timer, (uint64_t)seconds * 1000000ULL);
            int id = reminder->id;
            g_reminders.push_back(std::move(reminder));

            auto when = FormatDuration(seconds);
            if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
                display->ShowNotification("已设提醒：" + when + "后 " + message, 5000);
            }
            ESP_LOGI(TAG, "Reminder %d set in %d s: %s", id, seconds, message.c_str());
            return "{\"id\":" + std::to_string(id) + ",\"rings_in\":\"" + when +
                   "\",\"note\":\"The device will ring by itself when it is due.\"}";
        });

    mcp.AddTool("self.reminder.list",
                "List the pending countdown reminders with their id, message and seconds left.",
                PropertyList(),
                [](const PropertyList& properties) -> ToolResult { return RemindersJson(); });

    mcp.AddTool("self.reminder.cancel",
                "Cancel a countdown reminder by id and stop the ringing. Use id 0 to cancel all "
                "reminders. Call `self.reminder.list` first if you do not know the id.",
                PropertyList({Property("id", kPropertyTypeInteger, 0, 1000000)}),
                [](const PropertyList& properties) -> ToolResult {
                    int id = properties["id"].value<int>();
                    std::vector<std::unique_ptr<Reminder>> removed;
                    {
                        std::lock_guard<std::mutex> lock(g_reminders_mutex);
                        for (auto it = g_reminders.begin(); it != g_reminders.end();) {
                            if (id == 0 || (*it)->id == id) {
                                removed.push_back(std::move(*it));
                                it = g_reminders.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    // Timers are deleted by ~Reminder outside the lock.
                    Application::GetInstance().Schedule(StopRinging);
                    return !removed.empty();
                });
}
