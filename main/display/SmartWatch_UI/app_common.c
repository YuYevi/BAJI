#include "app_common.h"
#include "lvgl.h"
#include "font_awesome.h"
#include "ui_runtime.h"

#include <string.h>

extern const lv_font_t font_awesome_14_1;

static const char * g_network_icon = FONT_AWESOME_WIFI;
static uint8_t g_battery_percent = 100;
static bool g_battery_charging = false;

static app_status_bar_t * g_bars[16];
static uint8_t g_bar_count;

static lv_obj_t * g_status_overlay_root;
static app_status_bar_t g_status_overlay_bar;
static bool g_status_overlay_inited;
static bool g_status_overlay_visible = true;
static bool g_touch_guard_pressing;
static bool g_touch_guard_dragging;
static bool g_touch_guard_release_was_drag;
static lv_point_t g_touch_guard_press_start;
static lv_indev_t * g_touch_guard_indev;
static bool g_swipe_back_pressing;
static lv_point_t g_swipe_back_press_start;
static lv_obj_t * g_swipe_back_press_screen;
static uint32_t g_swipe_back_last_trigger_tick;

#define APP_TOUCH_DRAG_LOCK_PX 12
#define APP_SCREEN_FLAG_SWIPE_BACK LV_OBJ_FLAG_USER_1
#define APP_SWIPE_BACK_TRIGGER_PX 36
#define APP_SWIPE_BACK_DOMINANCE_PX 12
#define APP_SWIPE_BACK_COOLDOWN_MS 180

static bool touch_guard_is_pointer_indev(const lv_indev_t * indev)
{
    if(!indev) return false;

    lv_indev_type_t type = lv_indev_get_type(indev);
    return type == LV_INDEV_TYPE_POINTER || type == LV_INDEV_TYPE_BUTTON;
}

static bool touch_guard_drag_threshold_reached(lv_indev_t * indev)
{
    if(!indev || !g_touch_guard_pressing || g_touch_guard_indev != indev) {
        return false;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int32_t dx = p.x - g_touch_guard_press_start.x;
    int32_t dy = p.y - g_touch_guard_press_start.y;
    return (dx * dx + dy * dy) >= (APP_TOUCH_DRAG_LOCK_PX * APP_TOUCH_DRAG_LOCK_PX);
}

static bool touch_guard_indev_has_cb(lv_indev_t * indev, lv_event_cb_t cb)
{
    if(!indev || !cb) return false;

    uint32_t event_count = lv_indev_get_event_count(indev);
    for(uint32_t i = 0; i < event_count; i++) {
        lv_event_dsc_t * dsc = lv_indev_get_event_dsc(indev, i);
        if(dsc && lv_event_dsc_get_cb(dsc) == cb) {
            return true;
        }
    }

    return false;
}

static bool app_screen_swipe_back_enabled(lv_obj_t * screen)
{
    return screen && lv_obj_has_flag(screen, APP_SCREEN_FLAG_SWIPE_BACK);
}

static void swipe_back_reset_state(void)
{
    g_swipe_back_pressing = false;
    g_swipe_back_press_screen = NULL;
}

static bool swipe_back_try_trigger(lv_indev_t * indev)
{
    uint32_t now = lv_tick_get();
    if(now - g_swipe_back_last_trigger_tick < APP_SWIPE_BACK_COOLDOWN_MS) {
        return false;
    }

    if(!smartwatch_ui_runtime_back()) {
        return false;
    }

    swipe_back_reset_state();
    g_swipe_back_last_trigger_tick = now;
    lv_indev_reset(indev, NULL);
    return true;
}

static void swipe_back_track_press(lv_indev_t * indev)
{
    lv_obj_t * screen = lv_screen_active();
    if(!app_screen_swipe_back_enabled(screen)) {
        swipe_back_reset_state();
        return;
    }

    g_swipe_back_pressing = true;
    g_swipe_back_press_screen = screen;
    lv_indev_get_point(indev, &g_swipe_back_press_start);
}

static void swipe_back_track_release(lv_indev_t * indev)
{
    lv_obj_t * screen = lv_screen_active();
    if(!g_swipe_back_pressing || g_swipe_back_press_screen != screen) {
        swipe_back_reset_state();
        return;
    }

    swipe_back_reset_state();

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - g_swipe_back_press_start.x;
    int32_t dy = p.y - g_swipe_back_press_start.y;

    if(dx >= APP_SWIPE_BACK_TRIGGER_PX &&
       dx > LV_ABS(dy) + APP_SWIPE_BACK_DOMINANCE_PX) {
        (void)swipe_back_try_trigger(indev);
    }
}

static void touch_guard_indev_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = (lv_indev_t *)lv_event_get_current_target(e);
    if(!touch_guard_is_pointer_indev(indev)) return;

    switch(code) {
        case LV_EVENT_PRESSED:
            if(g_touch_guard_pressing && g_touch_guard_indev == indev) {
                return;
            }
            g_touch_guard_pressing = true;
            g_touch_guard_dragging = false;
            g_touch_guard_release_was_drag = false;
            g_touch_guard_indev = indev;
            lv_indev_get_point(indev, &g_touch_guard_press_start);
            swipe_back_track_press(indev);
            return;

        case LV_EVENT_PRESSING:
            if(touch_guard_drag_threshold_reached(indev)) {
                g_touch_guard_dragging = true;
            }
            return;

        case LV_EVENT_PRESS_LOST:
        case LV_EVENT_CANCEL:
            if(g_touch_guard_indev == indev) {
                g_touch_guard_release_was_drag = g_touch_guard_dragging ||
                                                 touch_guard_drag_threshold_reached(indev);
                g_touch_guard_pressing = false;
                g_touch_guard_dragging = false;
                g_touch_guard_indev = NULL;
            }
            swipe_back_reset_state();
            return;

        case LV_EVENT_GESTURE:
            g_touch_guard_dragging = true;
            if(app_screen_swipe_back_enabled(lv_screen_active()) &&
               lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
                (void)swipe_back_try_trigger(indev);
            }
            return;

        case LV_EVENT_RELEASED:
            if(g_touch_guard_indev == indev) {
                g_touch_guard_release_was_drag = g_touch_guard_dragging || touch_guard_drag_threshold_reached(indev);
                g_touch_guard_pressing = false;
                g_touch_guard_dragging = false;
                g_touch_guard_indev = NULL;
            }
            swipe_back_track_release(indev);
            return;

        case LV_EVENT_SHORT_CLICKED:
        case LV_EVENT_CLICKED:
            if(g_touch_guard_release_was_drag) {
                lv_indev_stop_processing(indev);
                lv_event_stop_processing(e);
            }
            return;

        default:
            return;
    }
}

static void app_touch_guard_ensure_installed(void)
{
    lv_indev_t * indev = lv_indev_get_next(NULL);
    while(indev) {
        if(touch_guard_is_pointer_indev(indev) &&
           !touch_guard_indev_has_cb(indev, touch_guard_indev_event_cb)) {
            lv_indev_add_event_cb(indev, touch_guard_indev_event_cb, LV_EVENT_ALL, NULL);
        }
        indev = lv_indev_get_next(indev);
    }
}

bool app_touch_event_is_drag_release(lv_event_t * e)
{
    if(!e) return false;

    lv_indev_t * indev = lv_event_get_indev(e);
    if(!touch_guard_is_pointer_indev(indev)) {
        return false;
    }

    return g_touch_guard_release_was_drag;
}

static void swipe_back_mark_subtree(lv_obj_t * root)
{
    if(!root) return;

    lv_obj_add_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if(!lv_obj_has_flag(root, LV_OBJ_FLAG_CLICKABLE) &&
       !lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    } else {
        lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    uint32_t child_count = lv_obj_get_child_count(root);
    for(uint32_t i = 0; i < child_count; i++) {
        swipe_back_mark_subtree(lv_obj_get_child(root, i));
    }
}

static const char * normalize_network_icon(const char * icon)
{
    if(!icon || icon[0] == '\0') {
        return FONT_AWESOME_WIFI_SLASH;
    }
    return icon;
}

static bool is_cellular_strength_icon(const char * icon)
{
    if(!icon) return false;

    return strcmp(icon, FONT_AWESOME_SIGNAL_STRONG) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_GOOD) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_FAIR) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_WEAK) == 0;
}

static const char * battery_symbol_for(uint8_t percent, bool charging)
{
    (void)charging;
    if(percent >= 80) return LV_SYMBOL_BATTERY_FULL;
    if(percent >= 60) return LV_SYMBOL_BATTERY_3;
    if(percent >= 40) return LV_SYMBOL_BATTERY_2;
    if(percent >= 20) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void status_bar_apply(app_status_bar_t * bar)
{
    if(!bar || !bar->cont) return;
    const char * network_icon = normalize_network_icon(g_network_icon);
    lv_label_set_text(bar->wifi, network_icon);
    // Cellular strength glyphs sit one pixel below the Wi-Fi glyph in the
    // status-bar font. Keep the online network indicators centered; the
    // slash/off glyphs already share the Wi-Fi slash baseline.
    lv_obj_set_style_translate_y(bar->wifi, is_cellular_strength_icon(network_icon) ? -1 : 0, 0);
    lv_label_set_text(bar->battery, battery_symbol_for(g_battery_percent, g_battery_charging));
    
    if(g_battery_charging) {
        lv_obj_remove_flag(bar->charging, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bar->charging, LV_OBJ_FLAG_HIDDEN);
    }

    if(bar->battery_pct) {
        char buf[8];
        lv_snprintf(buf, sizeof(buf), "%u%%", (unsigned)g_battery_percent);
        lv_label_set_text(bar->battery_pct, buf);
    }
}

void app_status_bar_init(app_status_bar_t * bar, lv_obj_t * parent)
{
    if(!bar) return;

    bar->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(bar->cont);
    lv_obj_set_size(bar->cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bar->cont, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar->cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar->cont, (lv_opa_t)(LV_OPA_COVER / 4), 0);
    lv_obj_set_style_pad_hor(bar->cont, 18, 0);
    lv_obj_set_style_pad_ver(bar->cont, 6, 0);
    lv_obj_set_style_pad_gap(bar->cont, 10, 0);
    lv_obj_set_style_shadow_width(bar->cont, 8, 0);
    lv_obj_set_style_shadow_color(bar->cont, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(bar->cont, (lv_opa_t)(LV_OPA_COVER / 8), 0);
    lv_obj_set_flex_flow(bar->cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(bar->cont, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(bar->cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    bar->wifi = lv_label_create(bar->cont);
    lv_obj_remove_style_all(bar->wifi);
    lv_obj_set_style_text_color(bar->wifi, lv_color_white(), 0);
    lv_obj_set_style_text_opa(bar->wifi, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(bar->wifi, &font_awesome_14_1, 0);
    lv_obj_clear_flag(bar->wifi, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * battery_cont = lv_obj_create(bar->cont);
    lv_obj_remove_style_all(battery_cont);
    lv_obj_set_size(battery_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(battery_cont, 4, 0);
    lv_obj_set_flex_flow(battery_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(battery_cont, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    bar->charging = lv_label_create(battery_cont);
    lv_obj_remove_style_all(bar->charging);
    lv_label_set_text(bar->charging, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(bar->charging, lv_color_hex(0x22C55E), 0); // Green lightning
    lv_obj_set_style_text_font(bar->charging, LV_FONT_DEFAULT, 0);
    lv_obj_add_flag(bar->charging, LV_OBJ_FLAG_HIDDEN);

    bar->battery = lv_label_create(battery_cont);
    lv_obj_remove_style_all(bar->battery);
    lv_obj_set_style_text_color(bar->battery, lv_color_white(), 0);
    lv_obj_set_style_text_opa(bar->battery, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(bar->battery, LV_FONT_DEFAULT, 0);
    lv_obj_clear_flag(bar->battery, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    bar->battery_pct = lv_label_create(battery_cont);
    lv_obj_remove_style_all(bar->battery_pct);
    lv_obj_set_style_text_color(bar->battery_pct, lv_color_white(), 0);
    lv_obj_set_style_text_opa(bar->battery_pct, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(bar->battery_pct, LV_FONT_DEFAULT, 0);
    lv_obj_clear_flag(bar->battery_pct, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(bar->battery_pct, 34);
    lv_label_set_long_mode(bar->battery_pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(bar->battery_pct, LV_TEXT_ALIGN_RIGHT, 0);

    if(g_bar_count < (uint8_t)(sizeof(g_bars) / sizeof(g_bars[0]))) {
        g_bars[g_bar_count++] = bar;
    }

    status_bar_apply(bar);
}

void app_status_bar_deinit(app_status_bar_t * bar)
{
    if(!bar) return;

    for(uint8_t i = 0; i < g_bar_count; i++) {
        if(g_bars[i] == bar) {
            for(uint8_t j = i + 1; j < g_bar_count; j++) {
                g_bars[j - 1] = g_bars[j];
            }
            g_bar_count--;
            break;
        }
    }

    bar->cont = NULL;
    bar->wifi = NULL;
    bar->charging = NULL;
    bar->battery = NULL;
    bar->battery_pct = NULL;
}

void app_status_set_network_icon(const char * icon)
{
    g_network_icon = normalize_network_icon(icon);
    for(uint8_t i = 0; i < g_bar_count; i++) {
        status_bar_apply(g_bars[i]);
    }
}

void app_status_set_battery(uint8_t percent, bool charging)
{
    g_battery_percent = percent;
    g_battery_charging = charging;
    for(uint8_t i = 0; i < g_bar_count; i++) {
        status_bar_apply(g_bars[i]);
    }
}

void app_status_overlay_init(void)
{
    if(g_status_overlay_inited) return;

    app_touch_guard_ensure_installed();

    lv_obj_t * layer = lv_layer_top();
    g_status_overlay_root = lv_obj_create(layer);
    lv_obj_remove_style_all(g_status_overlay_root);
    lv_obj_set_size(g_status_overlay_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_status_overlay_root, 0, 0);
    lv_obj_set_style_bg_opa(g_status_overlay_root, LV_OPA_0, 0);
    lv_obj_clear_flag(g_status_overlay_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    app_status_bar_init(&g_status_overlay_bar, g_status_overlay_root);
    if(!g_status_overlay_visible) {
        lv_obj_add_flag(g_status_overlay_root, LV_OBJ_FLAG_HIDDEN);
    }
    g_status_overlay_inited = true;
}

void app_status_overlay_set_visible(bool visible)
{
    g_status_overlay_visible = visible;
    if(!g_status_overlay_root) return;

    if(visible) {
        lv_obj_remove_flag(g_status_overlay_root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_status_overlay_root, LV_OBJ_FLAG_HIDDEN);
    }
}

void app_status_overlay_deinit(void)
{
    if(!g_status_overlay_inited) return;

    app_status_bar_deinit(&g_status_overlay_bar);
    if(g_status_overlay_root) {
        lv_obj_delete(g_status_overlay_root);
        g_status_overlay_root = NULL;
    }
    g_status_overlay_visible = true;
    g_status_overlay_inited = false;
}

void app_swipe_back_refresh_subtree(lv_obj_t * root)
{
    swipe_back_mark_subtree(root);
}

void app_screen_enable_swipe_back(lv_obj_t * screen)
{
    if(!screen) return;
    app_touch_guard_ensure_installed();
    lv_obj_add_flag(screen, APP_SCREEN_FLAG_SWIPE_BACK);
    swipe_back_mark_subtree(screen);
}

void app_screen_set_swipe_back_enabled(lv_obj_t * screen, bool enabled)
{
    if(!screen) return;
    app_touch_guard_ensure_installed();

    if(enabled) {
        lv_obj_add_flag(screen, APP_SCREEN_FLAG_SWIPE_BACK);
        swipe_back_mark_subtree(screen);
        return;
    }

    lv_obj_remove_flag(screen, APP_SCREEN_FLAG_SWIPE_BACK);
    if(g_swipe_back_press_screen == screen) {
        swipe_back_reset_state();
    }
}
