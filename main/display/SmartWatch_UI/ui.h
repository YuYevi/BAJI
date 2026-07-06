#ifndef _SQUARELINE_PROJECT_UI_H
#define _SQUARELINE_PROJECT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "ui_helpers.h"

/* 屏幕头文件 */

#include "screens/ui_HomeScreen.h"
#include "screens/ui_StandbyScreen.h"
#include "screens/ui_AIChatScreen.h"
#include "screens/ui_WallpaperScreen.h"
#include "screens/ui_EventsScreen.h"
#include "screens/ui_AlarmScreen.h"
#include "screens/ui_LightStickScreen.h"

/* 字体 */
extern const lv_font_t baji_font_14;
extern const lv_font_t font_puhui_14_1;
//const lv_font_t * smartwatch_ui_runtime_get_text_font(void);

static inline const lv_font_t * ui_builtin_text_font(void)
{
    //const lv_font_t * font = smartwatch_ui_runtime_get_text_font();
    const lv_font_t * font = &baji_font_14;
    return font ? font : &font_puhui_14_1;
}

/* UI 生命周期接口 */
void ui_init(void);
void ui_destroy(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
