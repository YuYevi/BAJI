/**
 * @file ui_AIChatScreen.h
 * @brief AI男友对话屏幕 - GIF动态背景与对话信息展示
 */

#ifndef UI_AICHATSCREEN_H
#define UI_AICHATSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t * ui_AIChatScreen;

void ui_AIChatScreen_init(void);
void ui_AIChatScreen_deinit(void);
void ui_AIChatScreen_set_status(const char * status);
void ui_AIChatScreen_set_chat_message(const char * role, const char * content);
void ui_AIChatScreen_clear_messages(void);
void ui_AIChatScreen_set_emotion(const char * emotion);

#ifdef __cplusplus
}
#endif

#endif /* UI_AICHATSCREEN_H */
