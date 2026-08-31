#include "ui_runtime.h"

#include "ui.h"
#include "app_common.h"
#include "ui_helpers.h"
#include "screens/ui_AIChatScreen.h"
#include "screens/ui_StandbyScreen.h"

static bool g_runtime_inited;
static lv_obj_t * g_top_toast_root;
static lv_obj_t * g_top_toast_label;
static lv_timer_t * g_top_toast_timer;
static lv_obj_t * g_bottom_toast_root;
static lv_obj_t * g_bottom_toast_label;
static lv_timer_t * g_bottom_toast_timer;
static const uint32_t kToastDefaultDurationMs = 2000;
/* 添加时间: 2026-08-19
 * 原因: 右滑返回要延后执行。
 * 逻辑: 同一时刻只排队一次原来的 smartwatch_ui_runtime_back，避免连滑重复退栈。 */
static bool s_pending_runtime_back;

static void smartwatch_ui_runtime_back_async_cb(void * user_data);

static void update_toast_size(lv_obj_t * root, lv_obj_t * label)
{
    if(!root || !label) return;

    lv_display_t * disp = lv_display_get_default();
    int32_t max_root_width = 180;
    if(disp) {
        max_root_width = (lv_display_get_horizontal_resolution(disp) * 76) / 100;
    }

    int32_t max_text_width = max_root_width - 32; /* Match horizontal padding. */
    if(max_text_width < 32) {
        max_text_width = 32;
    }

    lv_obj_set_style_max_width(root, max_root_width, 0);
    lv_obj_set_style_max_width(label, max_text_width, 0);
    lv_obj_update_layout(root);
}

static void top_toast_hide_cb(lv_timer_t * timer)
{
    (void)timer;
    if(g_top_toast_root) {
        lv_obj_add_flag(g_top_toast_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bottom_toast_hide_cb(lv_timer_t * timer)
{
    (void)timer;
    if(g_bottom_toast_root) {
        lv_obj_add_flag(g_bottom_toast_root, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ensure_toast_overlay(bool align_top)
{
    lv_obj_t ** root_ptr = align_top ? &g_top_toast_root : &g_bottom_toast_root;
    lv_obj_t ** label_ptr = align_top ? &g_top_toast_label : &g_bottom_toast_label;
    lv_timer_t ** timer_ptr = align_top ? &g_top_toast_timer : &g_bottom_toast_timer;
    if(*root_ptr) return;

    lv_obj_t * layer = lv_layer_top();
    *root_ptr = lv_obj_create(layer);
    lv_obj_remove_style_all(*root_ptr);
    lv_obj_set_size(*root_ptr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(*root_ptr, 18, 0);
    lv_obj_set_style_bg_color(*root_ptr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(*root_ptr, (lv_opa_t)(LV_OPA_COVER * 4 / 5), 0);
    lv_obj_set_style_pad_hor(*root_ptr, 16, 0);
    lv_obj_set_style_pad_ver(*root_ptr, 10, 0);
    lv_obj_set_style_shadow_width(*root_ptr, 18, 0);
    lv_obj_set_style_shadow_color(*root_ptr, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(*root_ptr, (lv_opa_t)(LV_OPA_COVER / 4), 0);
    lv_obj_align(*root_ptr, align_top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, align_top ? 64 : -64);
    lv_obj_clear_flag(*root_ptr, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(*root_ptr, LV_OBJ_FLAG_HIDDEN);

    *label_ptr = lv_label_create(*root_ptr);
    lv_obj_remove_style_all(*label_ptr);
    lv_obj_set_size(*label_ptr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    const lv_font_t * text_font = smartwatch_ui_runtime_get_text_font();
    if(text_font) {
        lv_obj_set_style_text_font(*label_ptr, text_font, 0);
    }
    lv_obj_set_style_text_color(*label_ptr, lv_color_white(), 0);
    lv_obj_set_style_text_align(*label_ptr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*label_ptr, LV_LABEL_LONG_WRAP);
    lv_label_set_text(*label_ptr, "");
    lv_obj_center(*label_ptr);
    update_toast_size(*root_ptr, *label_ptr);

    *timer_ptr = lv_timer_create(align_top ? top_toast_hide_cb : bottom_toast_hide_cb,
                                 kToastDefaultDurationMs, NULL);
    lv_timer_pause(*timer_ptr);
}

void smartwatch_ui_runtime_init(void)
{
    if(g_runtime_inited) return;

    /* Keep rounded borders anti-aliased on RGB565 panels.  The LVGL default
     * is enabled for 16-bit color, but set it before creating any UI because
     * display ports may override the default during initialization. */
    lv_display_t * display = lv_display_get_default();
    if(display) {
        lv_display_set_antialiasing(display, true);
    }
    ui_init();
    ensure_toast_overlay(true);
    ensure_toast_overlay(false);
    g_runtime_inited = true;
}

void smartwatch_ui_runtime_deinit(void)
{
    if(!g_runtime_inited) return;

    /* 添加时间: 2026-08-19
     * 原因: 界面销毁后若异步返回还在队列里，会切到已释放对象。
     * 逻辑: 先取消未执行的异步返回和导航，再走原来的销毁流程。 */
    lv_async_call_cancel(smartwatch_ui_runtime_back_async_cb, NULL);
    s_pending_runtime_back = false;
    ui_nav_reset();

    if(g_top_toast_timer) {
        lv_timer_delete(g_top_toast_timer);
        g_top_toast_timer = NULL;
    }
    if(g_bottom_toast_timer) {
        lv_timer_delete(g_bottom_toast_timer);
        g_bottom_toast_timer = NULL;
    }
    if(g_top_toast_root) {
        lv_obj_delete(g_top_toast_root);
        g_top_toast_root = NULL;
        g_top_toast_label = NULL;
    }
    if(g_bottom_toast_root) {
        lv_obj_delete(g_bottom_toast_root);
        g_bottom_toast_root = NULL;
        g_bottom_toast_label = NULL;
    }
    smartwatch_ui_runtime_wallpaper_reset();
    ui_destroy();
    smartwatch_ui_runtime_reset_remote_ai_chat_mjpeg_cache();
    g_runtime_inited = false;
}

bool smartwatch_ui_runtime_is_initialized(void)
{
    return g_runtime_inited;
}

bool smartwatch_ui_runtime_is_ai_chat_active(void)
{
    if(!g_runtime_inited) return false;
    return lv_screen_active() == ui_AIChatScreen;
}

static void smartwatch_ui_runtime_show_standby_internal(bool exiting_ai_chat)
{
    /* 添加时间: 2026-08-19
     * 原因: 进待机时若还排队着右滑返回，会在待机页再退一次栈。
     * 逻辑: 先取消异步返回，再执行原来的清栈和切待机。 */
    lv_async_call_cancel(smartwatch_ui_runtime_back_async_cb, NULL);
    s_pending_runtime_back = false;

    if(lv_screen_active() == ui_StandbyScreen) return;
    if(exiting_ai_chat) {
        smartwatch_ui_runtime_exit_ai_chat_to_standby();
    }

    ui_HomeScreen_reset_transient_state();

    ui_nav_reset();
    _ui_screen_change(&ui_StandbyScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0, ui_StandbyScreen_init);
    smartwatch_ui_runtime_wallpaper_set_visible(true);
}

static bool smartwatch_ui_runtime_exit_ai_chat_if_active(void)
{
    if(lv_screen_active() != ui_AIChatScreen) {
        return false;
    }

    smartwatch_ui_runtime_exit_ai_chat_to_standby();
    return true;
}

bool smartwatch_ui_runtime_back(void)
{
    if(!g_runtime_inited) return false;
    if(lv_screen_active() == ui_HomeScreen && ui_HomeScreen_dismiss_overlays()) {
        return true;
    }
    if(smartwatch_ui_runtime_exit_ai_chat_if_active()) {
        if(ui_nav_back(LV_SCR_LOAD_ANIM_NONE, 0, 0)) {
            return true;
        }
        smartwatch_ui_runtime_show_standby_internal(false);
        return true;
    }
    if(ui_nav_back(LV_SCR_LOAD_ANIM_NONE, 0, 0)) {
        return true;
    }
    return false;
}

static void smartwatch_ui_runtime_back_async_cb(void * user_data)
{
    (void)user_data;
    s_pending_runtime_back = false;
    (void)smartwatch_ui_runtime_back();
}

void smartwatch_ui_runtime_back_async(void)
{
    /* 添加时间: 2026-08-19
     * 原因: 右滑发生在触摸事件中。
     * 逻辑: 排队调用原来的 smartwatch_ui_runtime_back（关浮层、退栈、陪伴回待机），连滑只执行一次。 */
    if(!g_runtime_inited) return;
    if(s_pending_runtime_back) return;
    s_pending_runtime_back = true;
    lv_async_call(smartwatch_ui_runtime_back_async_cb, NULL);
}

void smartwatch_ui_runtime_show_standby(void)
{
    if(!g_runtime_inited) return;
    smartwatch_ui_runtime_show_standby_internal(lv_screen_active() == ui_AIChatScreen);
}

void smartwatch_ui_runtime_show_home(void)
{
    if(!g_runtime_inited) return;
    if(lv_screen_active() == ui_HomeScreen) return;

    (void)smartwatch_ui_runtime_exit_ai_chat_if_active();
    ui_HomeScreen_reset_transient_state();
    smartwatch_ui_runtime_wallpaper_set_visible(false);
    ui_nav_push(&ui_HomeScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
}

void smartwatch_ui_runtime_show_ai_chat(void)
{
    if(!g_runtime_inited) return;
    if(lv_screen_active() == ui_AIChatScreen) return;

    smartwatch_ui_runtime_wallpaper_set_visible(false);
    ui_nav_push(&ui_AIChatScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
}

void smartwatch_ui_runtime_reload_ai_chat_mjpeg(void)
{
    if(!g_runtime_inited) return;
    ui_AIChatScreen_reload_mjpeg();
}

void smartwatch_ui_runtime_show_wallpaper(void)
{
    if(!g_runtime_inited) return;

    ui_WallpaperScreen_reload_previews();
    (void)smartwatch_ui_runtime_exit_ai_chat_if_active();
    smartwatch_ui_runtime_wallpaper_set_visible(false);
    if(lv_screen_active() == ui_WallpaperScreen) return;
    ui_nav_push(&ui_WallpaperScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
}

void smartwatch_ui_runtime_set_status(const char * status)
{
    if(!g_runtime_inited) return;
    ui_AIChatScreen_set_status(status);
}

void smartwatch_ui_runtime_set_chat_message(const char * role, const char * content)
{
    if(!g_runtime_inited) return;
    ui_AIChatScreen_set_chat_message(role, content);
}

void smartwatch_ui_runtime_clear_chat_messages(void)
{
    if(!g_runtime_inited) return;
    ui_AIChatScreen_clear_messages();
}

void smartwatch_ui_runtime_set_emotion(const char * emotion)
{
    if(!g_runtime_inited) return;
    ui_AIChatScreen_set_emotion(emotion);
}

void smartwatch_ui_runtime_set_network_icon(const char * icon)
{
    if(!g_runtime_inited) return;
    app_status_set_network_icon(icon);
}

void smartwatch_ui_runtime_set_battery(uint8_t percent, bool charging, bool full)
{
    if(!g_runtime_inited) return;
    app_status_set_battery(percent, charging, full);
}

void smartwatch_ui_runtime_set_low_battery_warning(const char * text, bool visible)
{
    if(!g_runtime_inited) return;
    // Reuse the original top notification bar. A zero duration pauses its
    // auto-hide timer, so the warning remains visible until this method is
    // called again with visible=false.
    smartwatch_ui_runtime_show_top_notification(
        visible && text && text[0] != '\0' ? text : NULL);
}

static void smartwatch_ui_runtime_show_notification_internal(const char * text,
                                                             uint32_t duration_ms,
                                                             bool align_top)
{
    if(!g_runtime_inited) return;
    ensure_toast_overlay(align_top);

    lv_obj_t * root = align_top ? g_top_toast_root : g_bottom_toast_root;
    lv_obj_t * label = align_top ? g_top_toast_label : g_bottom_toast_label;
    lv_timer_t * timer = align_top ? g_top_toast_timer : g_bottom_toast_timer;

    if(!text || text[0] == '\0') {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(timer);
        return;
    }

    lv_obj_align(root, align_top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, align_top ? 64 : -64);
    lv_label_set_text(label, text);
    update_toast_size(root, label);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);

    lv_timer_pause(timer);
    if(duration_ms == 0) {
        return;
    }

    lv_timer_set_period(timer, duration_ms);
    lv_timer_resume(timer);
    lv_timer_reset(timer);
}

void smartwatch_ui_runtime_show_notification(const char * text, uint32_t duration_ms)
{
    smartwatch_ui_runtime_show_notification_internal(text, duration_ms, duration_ms != 0);
}

void smartwatch_ui_runtime_show_top_notification(const char * text)
{
    smartwatch_ui_runtime_show_notification_internal(text, 0, true);
}
