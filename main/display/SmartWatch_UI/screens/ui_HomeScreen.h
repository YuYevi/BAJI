/**
 * @file ui_HomeScreen.h
 * @brief 主页屏幕 - 显示时钟、日期、AI男友入口、4个应用图标和下拉控制中心
 */

#ifndef UI_HOMESCREEN_H
#define UI_HOMESCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_HomeScreen;

void ui_HomeScreen_init(void);
void ui_HomeScreen_deinit(void);
bool ui_HomeScreen_dismiss_overlays(void);
void ui_HomeScreen_reset_transient_state(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_HOMESCREEN_H */
