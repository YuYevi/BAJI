#ifndef UI_CONTROLCENTER_H
#define UI_CONTROLCENTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>

void ui_ControlCenter_init(lv_obj_t * screen);
void ui_ControlCenter_deinit(void);
bool ui_ControlCenter_is_visible(void);
bool ui_ControlCenter_dismiss_overlays(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_CONTROLCENTER_H */
