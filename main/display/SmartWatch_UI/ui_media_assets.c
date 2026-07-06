#include "ui_media_assets.h"

#include <stdbool.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "jpg/jpeg_to_image.h"
#include "ui_runtime.h"

#define TAG "ui_media_assets"
typedef struct {
    const char * name;
    const uint8_t * data;
    size_t size;
    bool loaded;
    bool failed;
} ui_binary_asset_t;

typedef struct {
    ui_binary_asset_t raw;
    lv_image_dsc_t dsc;
    bool ready;
    bool failed;
} ui_jpeg_asset_t;

static ui_jpeg_asset_t s_wallpaper_jpgs[] = {
    {
        .raw.name = "ui_img_1.jpg",
    },
    {
        .raw.name = "ui_img_2.jpg",
    },
    {
        .raw.name = "ui_img_3.jpg",
    },
};

static ui_binary_asset_t s_ai_chat_speaking_mjpeg_asset = {
    .name = "speaking.mjpeg",
};

static ui_binary_asset_t s_ai_chat_idle_mjpeg_asset = {
    .name = "standby.mjpeg",
};

static bool ui_media_assets_load_raw(ui_binary_asset_t * asset)
{
    if(asset->loaded) {
        return true;
    }

    if(asset->failed) {
        return false;
    }

    if(!smartwatch_ui_runtime_get_asset(asset->name, &asset->data, &asset->size) ||
       asset->data == NULL || asset->size == 0) {
        asset->failed = true;
        ESP_LOGW(TAG, "Failed to load asset: %s", asset->name);
        return false;
    }

    asset->loaded = true;
    return true;
}

static void ui_media_assets_release_jpg_asset(ui_jpeg_asset_t * asset)
{
    if(asset->ready && asset->dsc.data != NULL) {
        heap_caps_free((void *)asset->dsc.data);
    }

    memset(&asset->dsc, 0, sizeof(asset->dsc));
    asset->ready = false;
    asset->failed = false;
}

static const lv_image_dsc_t * ui_media_assets_decode_jpg(ui_jpeg_asset_t * asset)
{
    uint8_t * decoded = NULL;
    size_t decoded_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;

    if(asset->ready) {
        return &asset->dsc;
    }

    if(asset->failed) {
        return NULL;
    }

    if(!ui_media_assets_load_raw(&asset->raw)) {
        asset->failed = true;
        return NULL;
    }

    if(jpeg_to_image(asset->raw.data, asset->raw.size, &decoded, &decoded_len, &width, &height, &stride) != ESP_OK ||
       decoded == NULL || decoded_len == 0 || width == 0 || height == 0) {
        asset->failed = true;
        if(decoded) {
            heap_caps_free(decoded);
        }
        ESP_LOGW(TAG, "Failed to decode JPG asset: %s, size=%u", asset->raw.name, (unsigned)asset->raw.size);
        return NULL;
    }

    uint8_t * psram_data = heap_caps_malloc(decoded_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(psram_data != NULL) {
        memcpy(psram_data, decoded, decoded_len);
        heap_caps_free(decoded);
        decoded = psram_data;
    }

    memset(&asset->dsc, 0, sizeof(asset->dsc));
    asset->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    asset->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    asset->dsc.header.w = (uint32_t)width;
    asset->dsc.header.h = (uint32_t)height;
    asset->dsc.header.stride = (uint32_t)stride;
    asset->dsc.data = decoded;
    asset->dsc.data_size = (uint32_t)decoded_len;
    asset->ready = true;
    return &asset->dsc;
}

const lv_image_dsc_t * ui_media_assets_get_wallpaper_jpg(uint32_t index)
{
    if(index >= (sizeof(s_wallpaper_jpgs) / sizeof(s_wallpaper_jpgs[0]))) {
        return NULL;
    }

    return ui_media_assets_decode_jpg(&s_wallpaper_jpgs[index]);
}

bool ui_media_assets_preload_wallpaper_jpgs(void)
{
    for(uint32_t i = 0; i < (sizeof(s_wallpaper_jpgs) / sizeof(s_wallpaper_jpgs[0])); i++) {
        if(ui_media_assets_decode_jpg(&s_wallpaper_jpgs[i]) == NULL) {
            return false;
        }
    }

    return true;
}

static bool ui_media_assets_get_raw_asset(ui_binary_asset_t * asset, const uint8_t ** data, size_t * size)
{
    if(data == NULL || size == NULL) {
        return false;
    }

    if(!ui_media_assets_load_raw(asset)) {
        return false;
    }

    *data = asset->data;
    *size = asset->size;
    return true;
}

void ui_media_assets_release_wallpaper_jpg(uint32_t index)
{
    if(index >= (sizeof(s_wallpaper_jpgs) / sizeof(s_wallpaper_jpgs[0]))) {
        return;
    }

    ui_media_assets_release_jpg_asset(&s_wallpaper_jpgs[index]);
}

void ui_media_assets_release_all_wallpaper_jpgs(void)
{
    for(uint32_t i = 0; i < (sizeof(s_wallpaper_jpgs) / sizeof(s_wallpaper_jpgs[0])); i++) {
        ui_media_assets_release_jpg_asset(&s_wallpaper_jpgs[i]);
    }
}

bool ui_media_assets_get_ai_chat_speaking_mjpeg(const uint8_t ** data, size_t * size)
{
    return ui_media_assets_get_raw_asset(&s_ai_chat_speaking_mjpeg_asset, data, size);
}

bool ui_media_assets_get_ai_chat_idle_mjpeg(const uint8_t ** data, size_t * size)
{
    return ui_media_assets_get_raw_asset(&s_ai_chat_idle_mjpeg_asset, data, size);
}
