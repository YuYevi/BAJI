#ifndef SMARTWATCH_UI_RUNTIME_H
#define SMARTWATCH_UI_RUNTIME_H

#ifdef __cplusplus
#include <memory>
#include <vector>
class LvglImage;
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct smartwatch_ui_runtime_mjpeg_player smartwatch_ui_runtime_mjpeg_player_t;

void smartwatch_ui_runtime_init(void);
void smartwatch_ui_runtime_deinit(void);
bool smartwatch_ui_runtime_is_initialized(void);
bool smartwatch_ui_runtime_is_ai_chat_active(void);
bool smartwatch_ui_runtime_is_ai_speaking(void);
void smartwatch_ui_runtime_refresh_wake_word_detection(void);
void smartwatch_ui_runtime_exit_ai_chat_to_standby(void);
const lv_font_t * smartwatch_ui_runtime_get_text_font(void);

bool smartwatch_ui_runtime_back(void);
/* 添加时间: 2026-08-19
 * 原因: 右滑返回发生在触摸事件中，同步切页会假死。
 * 逻辑: 仍走原来的 smartwatch_ui_runtime_back（关浮层/退栈/回待机），只是延后到事件结束后执行。 */
void smartwatch_ui_runtime_back_async(void);
void smartwatch_ui_runtime_show_standby(void);
void smartwatch_ui_runtime_show_home(void);
void smartwatch_ui_runtime_show_ai_chat(void);
void smartwatch_ui_runtime_show_wallpaper(void);

void smartwatch_ui_runtime_set_status(const char * status);
void smartwatch_ui_runtime_set_chat_message(const char * role, const char * content);
void smartwatch_ui_runtime_clear_chat_messages(void);
void smartwatch_ui_runtime_set_emotion(const char * emotion);

void smartwatch_ui_runtime_set_network_icon(const char * icon);
void smartwatch_ui_runtime_set_battery(uint8_t percent, bool charging, bool full);
void smartwatch_ui_runtime_show_notification(const char * text, uint32_t duration_ms);
void smartwatch_ui_runtime_show_top_notification(const char * text);
bool smartwatch_ui_runtime_is_sound_idle(void);
void smartwatch_ui_runtime_play_alarm_sound(void);
void smartwatch_ui_runtime_stop_sound(void);
bool smartwatch_ui_runtime_get_asset(const char * name, const uint8_t ** data, size_t * size);
bool smartwatch_ui_runtime_get_remote_ai_chat_mjpeg(bool speaking, const uint8_t ** data,
                                                     size_t * size);
void smartwatch_ui_runtime_reset_remote_ai_chat_mjpeg_cache(void);
void smartwatch_ui_runtime_reload_ai_chat_mjpeg(void);

void smartwatch_ui_runtime_wallpaper_set_visible(bool visible);
void smartwatch_ui_runtime_wallpaper_set_static(const lv_image_dsc_t * image);
void smartwatch_ui_runtime_wallpaper_clear(void);
void smartwatch_ui_runtime_wallpaper_reset(void);
void smartwatch_ui_runtime_wallpaper_preview_reload_remote(void);
uint32_t smartwatch_ui_runtime_wallpaper_preview_count(uint8_t mode);
const lv_image_dsc_t * smartwatch_ui_runtime_wallpaper_preview_get(uint8_t mode, uint32_t index);
bool smartwatch_ui_runtime_wallpaper_preview_is_remote(uint8_t mode);
uint32_t smartwatch_ui_runtime_wallpaper_preview_interval_ms(uint8_t mode);
uint8_t smartwatch_ui_runtime_wallpaper_preview_last_mode(void);

smartwatch_ui_runtime_mjpeg_player_t * smartwatch_ui_runtime_mjpeg_player_create(lv_obj_t * target);
void smartwatch_ui_runtime_mjpeg_player_destroy(smartwatch_ui_runtime_mjpeg_player_t * player);
bool smartwatch_ui_runtime_mjpeg_player_set_src(smartwatch_ui_runtime_mjpeg_player_t * player,
                                                const uint8_t * data, size_t size);
void smartwatch_ui_runtime_mjpeg_player_restart(smartwatch_ui_runtime_mjpeg_player_t * player);
void smartwatch_ui_runtime_mjpeg_player_set_visible(smartwatch_ui_runtime_mjpeg_player_t * player, bool visible);
bool smartwatch_ui_runtime_mjpeg_player_is_loaded(const smartwatch_ui_runtime_mjpeg_player_t * player);

#ifdef __cplusplus
}

void smartwatch_ui_runtime_wallpaper_preview_set_images(std::vector<std::unique_ptr<LvglImage>> images,
                                                        uint8_t mode, uint32_t interval_ms);
#endif

#endif
