#include "passport_tools.h"

#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "lvgl_theme.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <cJSON.h>

#include <cstring>
#include <memory>
#include <vector>

#define TAG "PassportTools"

namespace {

constexpr char kPicturePrefix[] = "pic_";

bool IsPictureEmotion(const char* emotion) {
    return strncmp(emotion, kPicturePrefix, sizeof(kPicturePrefix) - 1) == 0;
}

// ---------------------------------------------------------------------------
// Reminders (kept in RAM; they are lost on reboot)

struct Reminder {
    int id = 0;
    int64_t due_us = 0;
    std::string message;
    esp_timer_handle_t timer = nullptr;
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

void DeleteReminderTimer(Reminder* reminder) {
    if (reminder->timer != nullptr) {
        esp_timer_stop(reminder->timer);
        esp_timer_delete(reminder->timer);
        reminder->timer = nullptr;
    }
}

void FireReminder(int id) {
    auto reminder = TakeReminder(id);
    if (reminder == nullptr) {
        return;
    }
    DeleteReminderTimer(reminder.get());
    ESP_LOGI(TAG, "Reminder %d: %s", id, reminder->message.c_str());

    auto& app = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetEmotion("surprised");
        display->ShowNotification("提醒：" + reminder->message, 10000);
    }
    app.PlaySound(Lang::Sounds::OGG_POPUP);
    if (app.GetDeviceState() == kDeviceStateIdle) {
        // Let the assistant say the reminder in her own words.
        app.WakeWordInvoke("（定时提醒到了，请用一两句话提醒我）" + reminder->message);
    }
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

// ---------------------------------------------------------------------------
// Pictures listed in the assets index.json:
//   "pictures": [{"name": "cat", "description": "我家的猫"}]
// Each picture image is an emoji entry named "pic_<name>".

std::string PicturesJson() {
    void* ptr = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("index.json", ptr, size)) {
        return "[]";
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<const char*>(ptr), size);
    if (root == nullptr) {
        return "[]";
    }
    cJSON* pictures = cJSON_GetObjectItem(root, "pictures");
    std::string result = "[]";
    if (cJSON_IsArray(pictures)) {
        char* text = cJSON_PrintUnformatted(pictures);
        if (text != nullptr) {
            result = text;
            cJSON_free(text);
        }
    }
    cJSON_Delete(root);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// PassportDisplay

PassportDisplay::~PassportDisplay() {
    if (picture_timer_ != nullptr) {
        esp_timer_stop(picture_timer_);
        esp_timer_delete(picture_timer_);
    }
}

void PassportDisplay::SetEmotion(const char* emotion) {
    if (picture_active_ && !IsPictureEmotion(emotion)) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_emotion_ = emotion;
        return;
    }
    LcdDisplay::SetEmotion(emotion);
}

bool PassportDisplay::ShowPicture(const std::string& name, int seconds) {
    std::string emoji_name = kPicturePrefix + name;
    auto theme = static_cast<LvglTheme*>(GetTheme());
    auto collection = theme != nullptr ? theme->emoji_collection() : nullptr;
    if (collection == nullptr || collection->GetEmojiImage(emoji_name.c_str()) == nullptr) {
        return false;
    }

    if (picture_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto self = static_cast<PassportDisplay*>(arg);
            Application::GetInstance().Schedule([self]() { self->EndPicture(); });
        };
        args.arg = this;
        args.name = "picture_timer";
        if (esp_timer_create(&args, &picture_timer_) != ESP_OK) {
            return false;
        }
    }
    esp_timer_stop(picture_timer_);

    picture_active_ = true;
    LcdDisplay::SetEmotion(emoji_name.c_str());
    esp_timer_start_once(picture_timer_, (uint64_t)seconds * 1000000ULL);
    return true;
}

void PassportDisplay::EndPicture() {
    std::string emotion;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        emotion = pending_emotion_;
        pending_emotion_ = "neutral";
    }
    picture_active_ = false;
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        emotion = "neutral";
    }
    LcdDisplay::SetEmotion(emotion.c_str());
}

// ---------------------------------------------------------------------------

void InitializePassportTools(PassportDisplay* display) {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.reminder.set",
        "Set a countdown reminder (定时提醒). When it is due the device rings and you will be "
        "asked to remind the user. Convert the user's request to seconds, e.g. "
        "'10分钟后提醒我喝水' -> seconds=600, message='喝水'. Reminders are lost if the device "
        "reboots.",
        PropertyList({Property("seconds", kPropertyTypeInteger, 5, 86400),
                      Property("message", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            int seconds = properties["seconds"].value<int>();
            auto message = properties["message"].value<std::string>();
            if (message.size() > 120) {
                message.resize(120);
            }

            auto reminder = std::make_unique<Reminder>();
            reminder->message = message;
            reminder->due_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
            {
                std::lock_guard<std::mutex> lock(g_reminders_mutex);
                if (g_reminders.size() >= kMaxReminders) {
                    return std::unexpected("Too many reminders (max 10)");
                }
                reminder->id = g_next_reminder_id++;
            }
            esp_timer_create_args_t args = {};
            args.callback = [](void* arg) {
                int id = (int)(intptr_t)arg;
                Application::GetInstance().Schedule([id]() { FireReminder(id); });
            };
            args.arg = (void*)(intptr_t)reminder->id;
            args.name = "reminder";
            if (esp_timer_create(&args, &reminder->timer) != ESP_OK) {
                return std::unexpected("Failed to create timer");
            }
            int id = reminder->id;
            esp_timer_start_once(reminder->timer, (uint64_t)seconds * 1000000ULL);
            {
                std::lock_guard<std::mutex> lock(g_reminders_mutex);
                g_reminders.push_back(std::move(reminder));
            }
            return "{\"id\":" + std::to_string(id) + "}";
        });

    mcp.AddTool("self.reminder.list",
                "List the pending countdown reminders with their id, message and seconds left.",
                PropertyList(),
                [](const PropertyList& properties) -> ToolResult { return RemindersJson(); });

    mcp.AddTool("self.reminder.cancel",
                "Cancel a countdown reminder by id. Use id 0 to cancel all reminders. Call "
                "`self.reminder.list` first if you do not know the id.",
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
                    for (auto& r : removed) {
                        DeleteReminderTimer(r.get());
                    }
                    return !removed.empty();
                });

    mcp.AddTool("self.screen.list_pictures",
                "List the pictures stored on the device (name and description) that can be shown "
                "with `self.screen.show_picture`.",
                PropertyList(),
                [](const PropertyList& properties) -> ToolResult { return PicturesJson(); });

    mcp.AddTool("self.screen.show_picture",
                "Show a stored picture on the screen in place of the avatar for a few seconds, "
                "e.g. when the user asks to see it. Call `self.screen.list_pictures` first to get "
                "the names.",
                PropertyList({Property("name", kPropertyTypeString),
                              Property("seconds", kPropertyTypeInteger, 15, 3, 300)}),
                [display](const PropertyList& properties) -> ToolResult {
                    auto name = properties["name"].value<std::string>();
                    int seconds = properties["seconds"].value<int>();
                    if (!display->ShowPicture(name, seconds)) {
                        return std::unexpected("Picture not found: " + name +
                                               ". Call self.screen.list_pictures for the names.");
                    }
                    return true;
                });
}
