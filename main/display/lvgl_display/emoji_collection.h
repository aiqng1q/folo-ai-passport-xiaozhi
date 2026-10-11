#ifndef EMOJI_COLLECTION_H
#define EMOJI_COLLECTION_H

#include "lvgl_image.h"

#include <lvgl.h>

#include <map>
#include <memory>
#include <string>

// An extra animation frame for an emoji, drawn on top of the emoji image at (x, y).
// Kinds used by LcdDisplay: "talk" (mouth open while speaking), "blink" (eyes closed)
// and "idle" (shown after the device has been idle for a while).
struct EmojiFrame {
    LvglImage* image = nullptr;
    int x = 0;
    int y = 0;
};

// Define interface for emoji collection
class EmojiCollection {
public:
    virtual void AddEmoji(const std::string& name, LvglImage* image);
    virtual const LvglImage* GetEmojiImage(const char* name);
    // Takes ownership of image.
    virtual void AddEmojiFrame(const std::string& name, const std::string& kind, LvglImage* image,
                               int x, int y);
    // Returns nullptr when the emoji has no frame of this kind.
    virtual const EmojiFrame* GetEmojiFrame(const std::string& name, const std::string& kind) const;
    virtual ~EmojiCollection();

private:
    std::map<std::string, LvglImage*> emoji_collection_;
    std::map<std::string, std::map<std::string, EmojiFrame>> emoji_frames_;
};

#endif
