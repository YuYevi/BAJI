/**
 * @file ui_AlarmScreen.h
 * @brief 闹钟/计时器屏幕 - 闹钟列表管理、倒计时器
 */

#ifndef UI_ALARMSCREEN_H
#define UI_ALARMSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_AlarmScreen;

void ui_AlarmScreen_init(void);
void ui_AlarmScreen_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_ALARMSCREEN_H */
