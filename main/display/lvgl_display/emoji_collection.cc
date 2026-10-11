#include "emoji_collection.h"

#include <esp_log.h>

#define TAG "EmojiCollection"

void EmojiCollection::AddEmoji(const std::string& name, LvglImage* image) {
    emoji_collection_[name] = image;
}

const LvglImage* EmojiCollection::GetEmojiImage(const char* name) {
    auto it = emoji_collection_.find(name);
    if (it != emoji_collection_.end()) {
        return it->second;
    }

    ESP_LOGW(TAG, "Emoji not found: %s", name);
    return nullptr;
}

void EmojiCollection::AddEmojiFrame(const std::string& name, const std::string& kind,
                                    LvglImage* image, int x, int y) {
    auto& frame = emoji_frames_[name][kind];
    delete frame.image;
    frame.image = image;
    frame.x = x;
    frame.y = y;
}

const EmojiFrame* EmojiCollection::GetEmojiFrame(const std::string& name,
                                                 const std::string& kind) const {
    auto it = emoji_frames_.find(name);
    if (it == emoji_frames_.end()) {
        return nullptr;
    }
    auto frame = it->second.find(kind);
    return frame != it->second.end() ? &frame->second : nullptr;
}

EmojiCollection::~EmojiCollection() {
    for (auto& [name, frames] : emoji_frames_) {
        for (auto& [kind, frame] : frames) {
            delete frame.image;
        }
    }
    emoji_frames_.clear();
    for (auto it = emoji_collection_.begin(); it != emoji_collection_.end(); ++it) {
        delete it->second;
    }
    emoji_collection_.clear();
}
