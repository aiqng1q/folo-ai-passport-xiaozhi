#include "lvgl_image.h"
#include <cbin_font.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstring>

#define TAG "LvglImage"

LvglRawImage::LvglRawImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));

    // Pre-decoded LVGL image (lv_image_header_t followed by pixel data), e.g. an
    // RGB565 emoji stored in the memory-mapped assets partition. LVGL draws it
    // straight from flash, so no RAM is needed to decode it. This lets boards
    // without PSRAM show emojis larger than a PNG could be decoded at.
    if (size > sizeof(lv_image_header_t)) {
        lv_image_header_t header;
        memcpy(&header, data, sizeof(header));
        uint32_t pixel_bytes = (uint32_t)header.stride * header.h;
        if (header.magic == LV_IMAGE_HEADER_MAGIC && header.w > 0 && header.h > 0 &&
            header.stride > 0 && pixel_bytes == size - sizeof(header)) {
            image_dsc_.header = header;
            image_dsc_.data_size = pixel_bytes;
            image_dsc_.data = static_cast<uint8_t*>(data) + sizeof(header);
            pre_decoded_ = true;
            return;
        }
    }

    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    image_dsc_.header.w = 0;
    image_dsc_.header.h = 0;
}

bool LvglRawImage::IsGif() const {
    if (pre_decoded_) {
        return false;
    }
    auto ptr = (const uint8_t*)image_dsc_.data;
    return ptr[0] == 'G' && ptr[1] == 'I' && ptr[2] == 'F';
}

LvglCBinImage::LvglCBinImage(void* data) {
    image_dsc_ = cbin_img_dsc_create(static_cast<uint8_t*>(data));
}

LvglCBinImage::~LvglCBinImage() {
    if (image_dsc_ != nullptr) {
        cbin_img_dsc_delete(image_dsc_);
    }
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);

    if (lv_image_decoder_get_info(&image_dsc_, &image_dsc_.header) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to get image info, data: %p size: %u", data, size);
        return;
    }
    valid_ = true;
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size, int width, int height, int stride,
                                       int color_format) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
    valid_ = data != nullptr && size > 0 && width > 0 && height > 0;
}

LvglAllocatedImage::~LvglAllocatedImage() {
    if (image_dsc_.data) {
        heap_caps_free((void*)image_dsc_.data);
        image_dsc_.data = nullptr;
    }
}
