/**
 * @file ui_WallpaperScreen.h
 * @brief 壁纸库屏幕 - 壁纸缩略图浏览、模式切换、应用壁纸
 */

#ifndef UI_WALLPAPERSCREEN_H
#define UI_WALLPAPERSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WALLPAPER_MODE_SINGLE = 0,
    WALLPAPER_MODE_TRIPLE = 1,
} wallpaper_mode_t;

typedef struct {
    wallpaper_mode_t mode;
    uint32_t selected_index;
} wallpaper_config_t;

extern wallpaper_config_t g_wallpaper_config;

extern lv_obj_t * ui_WallpaperScreen;

void ui_WallpaperScreen_init(void);
void ui_WallpaperScreen_deinit(void);
void ui_WallpaperScreen_reload_previews(void);
void ui_WallpaperScreen_release_preview_images(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WALLPAPERSCREEN_H */
