/**
 * @file ui_LightStickScreen.h
 * @brief 应援灯设置屏幕 - 颜色选择、灯效模式、预览
 */

#ifndef UI_LIGHTSTICKSCREEN_H
#define UI_LIGHTSTICKSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_LightStickScreen;

void ui_LightStickScreen_init(void);
void ui_LightStickScreen_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LIGHTSTICKSCREEN_H */
