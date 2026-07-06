#pragma once

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const lv_image_dsc_t * ui_media_assets_get_wallpaper_jpg(uint32_t index);
bool ui_media_assets_preload_wallpaper_jpgs(void);
// bool ui_media_assets_preload_gifs(void);
void ui_media_assets_release_wallpaper_jpg(uint32_t index);
void ui_media_assets_release_all_wallpaper_jpgs(void);
bool ui_media_assets_get_ai_chat_speaking_mjpeg(const uint8_t ** data, size_t * size);
bool ui_media_assets_get_ai_chat_idle_mjpeg(const uint8_t ** data, size_t * size);

#ifdef __cplusplus
}
#endif
