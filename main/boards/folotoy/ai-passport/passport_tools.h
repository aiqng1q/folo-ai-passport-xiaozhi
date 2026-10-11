#pragma once

#include "display/lcd_display.h"

#include <esp_timer.h>

#include <atomic>
#include <mutex>
#include <string>

// SPI LCD display that can pin a picture in the avatar area for a while.
// Pictures are emoji entries named "pic_<name>" in the assets (see make_avatar.py).
// While a picture is shown, emotions sent by the LLM are remembered and applied
// when the picture times out, so the picture is not replaced mid-sentence.
class PassportDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~PassportDisplay();

    void SetEmotion(const char* emotion) override;
    bool ShowPicture(const std::string& name, int seconds);

private:
    void EndPicture();

    std::mutex mutex_;
    std::atomic<bool> picture_active_ = false;
    std::string pending_emotion_ = "neutral";
    esp_timer_handle_t picture_timer_ = nullptr;
};

// Registers the Passport's own MCP tools (reminders and pictures).
void InitializePassportTools(PassportDisplay* display);
