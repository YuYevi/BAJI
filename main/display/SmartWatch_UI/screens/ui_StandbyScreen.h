/**
 * @file ui_StandbyScreen.h
 * @brief 待机屏幕 - 壁纸轮播、语音唤醒入口、滑动解锁
 */

#ifndef UI_STANDBYSCREEN_H
#define UI_STANDBYSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_StandbyScreen;

void ui_StandbyScreen_init(void);
void ui_StandbyScreen_deinit(void);
void ui_StandbyScreen_apply_wallpaper(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_STANDBYSCREEN_H */
