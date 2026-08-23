/**
 * @file ui_EventsScreen.h
 * @brief 日历屏幕 - 日期浏览、点击切换和拖动吸附
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
