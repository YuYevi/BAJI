/**
 * @file ui_EventsScreen.h
 * @brief 事件提醒屏幕 - 日历日期选择、事件卡片列表
 */

#ifndef UI_EVENTSSCREEN_H
#define UI_EVENTSSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_EventsScreen;

void ui_EventsScreen_init(void);
void ui_EventsScreen_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_EVENTSSCREEN_H */
