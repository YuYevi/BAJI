/**
 * @file ui_StandbyScreen.c
 * @brief 待机屏幕实现
 *
 * 布局（360x360圆形屏幕）：
 *   - 全屏：壁纸（根据模式：单图/多图切换）
 *   - 顶部：指示点（仅多图切换模式）
 *   - 底部：语音唤醒按钮（"哥哥"）
 * 交互：
 *   - 左右滑动 → 切换壁纸（仅多图切换模式）
 *   - 上滑 → 进入HomeScreen
 *   - 点击唤醒按钮 → 进入AIChatScreen
 */

#include "ui_StandbyScreen.h"
#include "../ui_media_assets.h"
#include "../ui.h"
#include "../app_common.h"
#include "../ui_runtime.h"
#include "ui_HomeScreen.h"
#include "ui_WallpaperScreen.h"
#include <stdlib.h>
#include <string.h>

extern const lv_image_dsc_t microphone;

lv_obj_t * ui_StandbyScreen;

static lv_obj_t * standby_dots_cont;
static lv_obj_t ** standby_dots;
static uint32_t standby_dot_count;
static lv_obj_t * standby_wake_btn;
static lv_obj_t * standby_swipe_overlay;
static lv_obj_t * standby_swipe_arc;
static lv_obj_t * standby_swipe_check;
static lv_timer_t * standby_carousel_timer;

static uint32_t standby_wallpaper_index;

static bool standby_pressing;
static lv_point_t standby_press_start;

static wallpaper_mode_t standby_current_mode = WALLPAPER_MODE_TRIPLE;
static const uint32_t kStandbyCarouselIntervalMs = 2000;

#define STANDBY_SWIPE_TRIGGER_PX  20
#define STANDBY_WAKE_BUTTON_WIDTH  104
#define STANDBY_WAKE_BUTTON_HEIGHT 42

static lv_opa_t standby_wake_border_opa_idle(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 30 / 100);
}

static lv_opa_t standby_wake_border_opa_pressed(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 80 / 100);
}

static lv_opa_t standby_wake_glow_opa_idle(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 12 / 100);
}

static lv_opa_t standby_wake_glow_opa_pressed(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 42 / 100);
}

static void standby_set_border_opa(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void standby_set_shadow_opa(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void standby_start_anim(lv_obj_t * obj,
                               lv_anim_exec_xcb_t exec_cb,
                               int32_t from,
                               int32_t to,
                               uint32_t duration)
{
    if(!obj || !exec_cb) return;

    lv_anim_del(obj, exec_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_time(&anim, duration);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_start(&anim);
}

static void standby_reset_wake_button_visual(void)
{
    if(!standby_wake_btn) return;

    lv_anim_del(standby_wake_btn, (lv_anim_exec_xcb_t)standby_set_border_opa);
    lv_anim_del(standby_wake_btn, (lv_anim_exec_xcb_t)standby_set_shadow_opa);
    standby_set_border_opa(standby_wake_btn, standby_wake_border_opa_idle());
    standby_set_shadow_opa(standby_wake_btn, standby_wake_glow_opa_idle());
}

static uint32_t standby_carousel_interval_ms(wallpaper_mode_t mode)
{
    if(mode == WALLPAPER_MODE_TRIPLE &&
       smartwatch_ui_runtime_wallpaper_preview_is_remote((uint8_t)mode)) {
        uint32_t interval_ms = smartwatch_ui_runtime_wallpaper_preview_interval_ms((uint8_t)mode);
        return interval_ms >= 500 ? interval_ms : kStandbyCarouselIntervalMs;
    }
    return kStandbyCarouselIntervalMs;
}

static uint32_t standby_wallpaper_count(wallpaper_mode_t mode)
{
    if(smartwatch_ui_runtime_wallpaper_preview_is_remote((uint8_t)mode)) {
        uint32_t count = smartwatch_ui_runtime_wallpaper_preview_count((uint8_t)mode);
        if(count == 0) return 1;
        return count;
    }
    return 3;
}

static void standby_rebuild_dots(uint32_t count)
{
    if(!standby_dots_cont) return;
    if(count == standby_dot_count && (count == 0 || standby_dots != NULL)) return;

    lv_obj_clean(standby_dots_cont);
    free(standby_dots);
    standby_dots = NULL;
    standby_dot_count = 0;

    if(count == 0) return;

    standby_dots = (lv_obj_t **)calloc(count, sizeof(*standby_dots));
    if(!standby_dots) return;

    standby_dot_count = count;
    for(uint32_t i = 0; i < standby_dot_count; i++) {
        standby_dots[i] = lv_obj_create(standby_dots_cont);
        lv_obj_remove_style_all(standby_dots[i]);
        lv_obj_set_height(standby_dots[i], 6);
        lv_obj_set_width(standby_dots[i], 6);
        lv_obj_set_style_radius(standby_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(standby_dots[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(standby_dots[i], (lv_opa_t)(LV_OPA_COVER * 2 / 5), 0);
        lv_obj_clear_flag(standby_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

static void standby_set_dot_state(uint32_t i, bool selected)
{
    if(i >= standby_dot_count) return;
    lv_obj_set_width(standby_dots[i], selected ? 16 : 6);
    lv_obj_set_style_bg_opa(standby_dots[i], selected ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 2 / 5), 0);
}

static void standby_update_dots(uint32_t index)
{
    uint32_t count = standby_wallpaper_count(standby_current_mode);
    standby_rebuild_dots(count);
    for(uint32_t i = 0; i < standby_dot_count; i++) {
        standby_set_dot_state(i, i == index);
    }
}

static void standby_clear_wallpaper(void)
{
    smartwatch_ui_runtime_wallpaper_clear();
}

static void standby_wallpaper_apply(wallpaper_mode_t mode, uint32_t index)
{
    uint32_t count = standby_wallpaper_count(mode);
    if(count == 0) count = 1;
    if(index >= count) index = 0;

    const lv_image_dsc_t * dsc = NULL;
    if(smartwatch_ui_runtime_wallpaper_preview_is_remote((uint8_t)mode)) {
        dsc = smartwatch_ui_runtime_wallpaper_preview_get((uint8_t)mode, index);
    }
    else {
        dsc = ui_media_assets_get_wallpaper_jpg(index);
    }
    if(!dsc) {
        standby_clear_wallpaper();
        return;
    }

    smartwatch_ui_runtime_wallpaper_set_static(dsc);
}

static void standby_wallpaper_show(uint32_t index)
{
    uint32_t count = standby_wallpaper_count(standby_current_mode);
    if(count == 0) count = 1;
    if(index >= count) index = 0;
    standby_wallpaper_apply(standby_current_mode, index);
    standby_wallpaper_index = index;
    standby_update_dots(index);
}

static void standby_update_carousel_timer(void)
{
    if(!standby_carousel_timer) return;

    if(standby_current_mode == WALLPAPER_MODE_TRIPLE &&
       ui_StandbyScreen &&
       lv_screen_active() == ui_StandbyScreen &&
       standby_wallpaper_count(standby_current_mode) > 1 &&
       !standby_pressing) {
        lv_timer_set_period(standby_carousel_timer, standby_carousel_interval_ms(standby_current_mode));
        lv_timer_resume(standby_carousel_timer);
        lv_timer_reset(standby_carousel_timer);
    }
    else {
        lv_timer_pause(standby_carousel_timer);
    }
}

static void standby_carousel_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    if(!ui_StandbyScreen ||
       lv_screen_active() != ui_StandbyScreen ||
       standby_current_mode != WALLPAPER_MODE_TRIPLE ||
       standby_wallpaper_count(standby_current_mode) <= 1 ||
       standby_pressing) {
        standby_update_carousel_timer();
        return;
    }

    standby_wallpaper_show((standby_wallpaper_index + 1) % standby_wallpaper_count(standby_current_mode));
}

static void standby_go_home(void)
{
    /* 添加时间: 2026-08-19
     * 原因: 上滑进主界面发生在松手事件里，同步切页会打乱触摸状态。
     * 逻辑: 仍隐藏待机壁纸并进入主界面；init/切页由原来的 ui_nav_push 在事件结束后完成。 */
    smartwatch_ui_runtime_wallpaper_set_visible(false);
    ui_nav_push_async(&ui_HomeScreen);
}

static void standby_go_chat(void)
{
    /* 添加时间: 2026-08-19
     * 原因: 点击唤醒进陪伴页同样不能在触摸事件里同步切页。
     * 逻辑: 目标仍是陪伴页，只延后执行原 ui_nav_push。 */
    smartwatch_ui_runtime_wallpaper_set_visible(false);
    ui_nav_push_async(&ui_AIChatScreen);
}

static void standby_swipe_overlay_set_progress(int32_t dy)
{
    int32_t p = (-dy * 100) / STANDBY_SWIPE_TRIGGER_PX;
    if(p < 0) p = 0;
    if(p > 100) p = 100;

    lv_arc_set_value(standby_swipe_arc, p);
    if(p >= 100) lv_obj_clear_flag(standby_swipe_check, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(standby_swipe_check, LV_OBJ_FLAG_HIDDEN);
}

static void standby_clear_swipe_feedback(void)
{
    if(standby_swipe_overlay) {
        lv_obj_add_flag(standby_swipe_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if(standby_swipe_check) {
        lv_obj_add_flag(standby_swipe_check, LV_OBJ_FLAG_HIDDEN);
    }
    if(standby_swipe_arc) {
        lv_arc_set_value(standby_swipe_arc, 0);
    }
}

static void standby_reset_interaction_state(void)
{
    standby_pressing = false;
    standby_clear_swipe_feedback();
    standby_reset_wake_button_visual();
}

static void standby_root_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_CANCEL || code == LV_EVENT_PRESS_LOST) {
        standby_reset_interaction_state();
        standby_update_carousel_timer();
        return;
    }

    lv_indev_t * indev = lv_event_get_indev(e);
    if(!indev) indev = lv_indev_get_act();
    if(!indev) return;

    lv_obj_t * target = (lv_obj_t *)lv_event_get_target(e);
    while(target) {
        if(target == standby_wake_btn) return;
        target = lv_obj_get_parent(target);
    }

    if(code == LV_EVENT_PRESSED) {
        standby_pressing = true;
        standby_update_carousel_timer();
        lv_indev_get_point(indev, &standby_press_start);
        standby_clear_swipe_feedback();
        return;
    }

    if(code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - standby_press_start.x;
        int32_t dy = p.y - standby_press_start.y;

        if(LV_ABS(dy) > LV_ABS(dx) && dy < -8) {
            lv_obj_clear_flag(standby_swipe_overlay, LV_OBJ_FLAG_HIDDEN);
            standby_swipe_overlay_set_progress(dy);
        }
        else {
            standby_clear_swipe_feedback();
        }
        return;
    }

    if(code == LV_EVENT_RELEASED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - standby_press_start.x;
        int32_t dy = p.y - standby_press_start.y;

        standby_reset_interaction_state();
        standby_update_carousel_timer();

        if(LV_ABS(dy) > LV_ABS(dx)) {
            if(dy <= -STANDBY_SWIPE_TRIGGER_PX) {
                standby_go_home();
                return;
            }
        }
        else {
            if(LV_ABS(dx) > 20 && standby_current_mode == WALLPAPER_MODE_TRIPLE) {
                uint32_t count = standby_wallpaper_count(standby_current_mode);
                if(count <= 1) return;
                if(dx < 0) standby_wallpaper_show((standby_wallpaper_index + 1) % count);
                else standby_wallpaper_show((standby_wallpaper_index + count - 1) % count);
                standby_update_carousel_timer();
                return;
            }
        }
    }
}

static void standby_wake_btn_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * button = lv_event_get_current_target(e);

    if(!button) return;

    if(code == LV_EVENT_PRESSED) {
        standby_start_anim(button,
                           (lv_anim_exec_xcb_t)standby_set_border_opa,
                           lv_obj_get_style_border_opa(button, LV_PART_MAIN),
                           standby_wake_border_opa_pressed(),
                           90);
        standby_start_anim(button,
                           (lv_anim_exec_xcb_t)standby_set_shadow_opa,
                           lv_obj_get_style_shadow_opa(button, LV_PART_MAIN),
                           standby_wake_glow_opa_pressed(),
                           90);
        return;
    }

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL) {
        standby_start_anim(button,
                           (lv_anim_exec_xcb_t)standby_set_border_opa,
                           lv_obj_get_style_border_opa(button, LV_PART_MAIN),
                           standby_wake_border_opa_idle(),
                           120);
        standby_start_anim(button,
                           (lv_anim_exec_xcb_t)standby_set_shadow_opa,
                           lv_obj_get_style_shadow_opa(button, LV_PART_MAIN),
                           standby_wake_glow_opa_idle(),
                           120);
        return;
    }

    if(code == LV_EVENT_CLICKED) {
        standby_reset_wake_button_visual();
        standby_go_chat();
    }
}

static void standby_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_SCREEN_LOADED) {
        standby_reset_interaction_state();
        ui_StandbyScreen_apply_wallpaper();
    }
    else if(code == LV_EVENT_SCREEN_UNLOADED) {
        standby_reset_interaction_state();
        standby_update_carousel_timer();
        smartwatch_ui_runtime_wallpaper_set_visible(false);
    }
}

void ui_StandbyScreen_apply_wallpaper(void)
{
    if(!ui_StandbyScreen) return;

    wallpaper_mode_t mode = g_wallpaper_config.mode;
    uint32_t idx = g_wallpaper_config.selected_index;
    uint32_t count = standby_wallpaper_count(mode);
    if(count == 0) count = 1;
    if(idx >= count) idx = 0;
    standby_current_mode = mode;

    if(mode == WALLPAPER_MODE_SINGLE || count <= 1) {
        standby_rebuild_dots(0);
        standby_wallpaper_apply(mode, idx);
        lv_obj_add_flag(standby_dots_cont, LV_OBJ_FLAG_HIDDEN);

        standby_wallpaper_index = idx;
    }
    else {
        standby_wallpaper_apply(mode, idx);
        lv_obj_clear_flag(standby_dots_cont, LV_OBJ_FLAG_HIDDEN);

        standby_wallpaper_index = idx;
        standby_update_dots(idx);
    }

    smartwatch_ui_runtime_wallpaper_set_visible(true);
    standby_update_carousel_timer();
}

void ui_StandbyScreen_init(void) {
    if(ui_StandbyScreen) return;

    ui_StandbyScreen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui_StandbyScreen);
    lv_obj_set_size(ui_StandbyScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_StandbyScreen, lv_color_hex(0x09090f), 0);
    lv_obj_set_style_bg_opa(ui_StandbyScreen, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(ui_StandbyScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_StandbyScreen, LV_OBJ_FLAG_SCROLLABLE);

    standby_wallpaper_index = 0;
    standby_carousel_timer = lv_timer_create(standby_carousel_timer_cb, kStandbyCarouselIntervalMs, NULL);
    if(standby_carousel_timer) {
        lv_timer_pause(standby_carousel_timer);
    }

    standby_dots_cont = lv_obj_create(ui_StandbyScreen);
    lv_obj_remove_style_all(standby_dots_cont);
    lv_obj_set_size(standby_dots_cont, LV_SIZE_CONTENT, 6);
    lv_obj_set_style_pad_gap(standby_dots_cont, 6, 0);
    lv_obj_set_flex_flow(standby_dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(standby_dots_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(standby_dots_cont, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_clear_flag(standby_dots_cont, LV_OBJ_FLAG_CLICKABLE);
    standby_rebuild_dots(3);
    standby_update_dots(0);

    standby_swipe_overlay = lv_obj_create(ui_StandbyScreen);
    lv_obj_remove_style_all(standby_swipe_overlay);
    lv_obj_set_size(standby_swipe_overlay, 110, 110);
    lv_obj_set_style_radius(standby_swipe_overlay, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(standby_swipe_overlay, LV_OPA_0, 0);
    lv_obj_align(standby_swipe_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(standby_swipe_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(standby_swipe_overlay, LV_OBJ_FLAG_CLICKABLE);

    standby_swipe_arc = lv_arc_create(standby_swipe_overlay);
    lv_obj_remove_style_all(standby_swipe_arc);
    lv_obj_set_size(standby_swipe_arc, 110, 110);
    lv_arc_set_range(standby_swipe_arc, 0, 100);
    lv_arc_set_value(standby_swipe_arc, 0);
    lv_arc_set_rotation(standby_swipe_arc, 270);
    lv_arc_set_bg_angles(standby_swipe_arc, 0, 360);
    lv_obj_set_style_arc_width(standby_swipe_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(standby_swipe_arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(standby_swipe_arc, (lv_opa_t)(LV_OPA_COVER * 15 / 100), LV_PART_MAIN);
    lv_obj_set_style_arc_width(standby_swipe_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(standby_swipe_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(standby_swipe_arc, (lv_opa_t)(LV_OPA_COVER * 85 / 100), LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(standby_swipe_arc, 0, 0);
    lv_obj_center(standby_swipe_arc);

    standby_swipe_check = lv_label_create(standby_swipe_overlay);
    lv_obj_remove_style_all(standby_swipe_check);
    lv_label_set_text(standby_swipe_check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(standby_swipe_check, lv_color_white(), 0);
    lv_obj_set_style_text_font(standby_swipe_check, LV_FONT_DEFAULT, 0);
    lv_obj_center(standby_swipe_check);
    lv_obj_add_flag(standby_swipe_check, LV_OBJ_FLAG_HIDDEN);

    standby_wake_btn = lv_btn_create(ui_StandbyScreen);
    lv_obj_remove_style_all(standby_wake_btn);
    lv_obj_set_size(standby_wake_btn, STANDBY_WAKE_BUTTON_WIDTH, STANDBY_WAKE_BUTTON_HEIGHT);
    lv_obj_set_style_radius(standby_wake_btn, STANDBY_WAKE_BUTTON_HEIGHT / 2, 0);
    lv_obj_set_style_bg_color(standby_wake_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(standby_wake_btn, (lv_opa_t)(LV_OPA_COVER * 55 / 100), 0);
    lv_obj_set_style_border_width(standby_wake_btn, 2, 0);
    lv_obj_set_style_border_color(standby_wake_btn, lv_color_hex(0xf9a8d4), 0);
    lv_obj_set_style_border_opa(standby_wake_btn, standby_wake_border_opa_idle(), 0);
    lv_obj_set_style_shadow_width(standby_wake_btn, 16, 0);
    lv_obj_set_style_shadow_color(standby_wake_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_shadow_opa(standby_wake_btn, standby_wake_glow_opa_idle(), 0);
    lv_obj_set_style_shadow_spread(standby_wake_btn, 0, 0);
    lv_obj_set_style_shadow_ofs_x(standby_wake_btn, 0, 0);
    lv_obj_set_style_shadow_ofs_y(standby_wake_btn, 0, 0);
    lv_obj_set_style_pad_hor(standby_wake_btn, 20, 0);
    lv_obj_set_style_pad_ver(standby_wake_btn, 10, 0);
    lv_obj_set_style_pad_gap(standby_wake_btn, 8, 0);
    lv_obj_set_flex_flow(standby_wake_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(standby_wake_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(standby_wake_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_remove_flag(standby_wake_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(standby_wake_btn, standby_wake_btn_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(standby_wake_btn, standby_wake_btn_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(standby_wake_btn, standby_wake_btn_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(standby_wake_btn, standby_wake_btn_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(standby_wake_btn, standby_wake_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * wake_icon = lv_image_create(standby_wake_btn);
    lv_image_set_src(wake_icon, &microphone);
    lv_obj_set_size(wake_icon, 16, 16);
    lv_image_set_inner_align(wake_icon, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(wake_icon, LV_SCALE_NONE);
    lv_image_set_antialias(wake_icon, false);
    lv_obj_set_style_image_recolor(wake_icon, lv_color_hex(0xf9a8d4), 0);
    lv_obj_set_style_image_recolor_opa(wake_icon, LV_OPA_COVER, 0);
    ui_make_decor_hit_passthrough(wake_icon);

    lv_obj_t * wake_text = lv_label_create(standby_wake_btn);
    lv_label_set_text(wake_text, "哥哥");
    lv_obj_set_style_text_color(wake_text, lv_color_white(), 0);
    lv_obj_set_style_text_opa(wake_text, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(wake_text, ui_builtin_text_font(), 0);
    ui_make_decor_hit_passthrough(wake_text);

    lv_obj_add_event_cb(ui_StandbyScreen, standby_root_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_root_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_root_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_root_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_root_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_StandbyScreen, standby_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    ui_HomeScreen_init();
}

void ui_StandbyScreen_deinit(void) {
    standby_reset_interaction_state();
    smartwatch_ui_runtime_wallpaper_set_visible(false);
    if(standby_carousel_timer) {
        lv_timer_delete(standby_carousel_timer);
        standby_carousel_timer = NULL;
    }

    if(ui_StandbyScreen) {
        lv_obj_delete(ui_StandbyScreen);
        ui_StandbyScreen = NULL;
    }

    standby_dots_cont = NULL;
    free(standby_dots);
    standby_dots = NULL;
    standby_dot_count = 0;
    standby_wake_btn = NULL;
    standby_swipe_overlay = NULL;
    standby_swipe_arc = NULL;
    standby_swipe_check = NULL;
    standby_wallpaper_index = 0;
    standby_pressing = false;
    standby_current_mode = WALLPAPER_MODE_TRIPLE;
}
