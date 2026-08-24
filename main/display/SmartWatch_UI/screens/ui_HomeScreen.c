/**
 * @file ui_HomeScreen.c
 * @brief 主页屏幕实现
 *
 * 重构目标：
 * 1. 通过静态配置表统一管理应用入口。
 * 2. 将界面拆为多个职责清晰的构建函数，降低 init 复杂度。
 * 3. 保留原有功能、交互和生命周期接口，便于外部继续复用。
 */

#include "ui_HomeScreen.h"
#include "ui_ControlCenter.h"
#include "../ui.h"
#include "../app_common.h"
#include <time.h>

extern const lv_image_dsc_t ai_love_information;
extern const lv_image_dsc_t picture;
extern const lv_image_dsc_t calendar;
extern const lv_image_dsc_t alarm_clock;
extern const lv_image_dsc_t cheering_light;

/* 主页屏幕根对象，由外部导航系统持有和切换。 */
lv_obj_t * ui_HomeScreen;

/* 应用入口的视觉和导航配置。 */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t accent_hex;
    uint32_t glow_hex;
    uint32_t icon_grad_start_hex;
    uint32_t icon_grad_end_hex;
    const void * icon_src;
    const char * label_text;
    lv_obj_t ** target_screen;
} home_app_config_t;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * plate;
    lv_obj_t * content;
} home_pressable_t;

#define HOME_HERO_PRESSED_SCALE 248
#define HOME_APP_PRESSED_SCALE  248
#define HOME_PRESSABLE_EXT_DRAW_SIZE 6

static const char * const kHomeWeekDays[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

static const home_app_config_t kHomeApps[] = {
    { 0,   0, 118, 63, 0x5eead4, 0x2dd4bf, 0x0f766e, 0x2dd4bf, &picture,        "壁纸",   &ui_WallpaperScreen },
    { 124, 0,  74, 63, 0x6ee7b7, 0x34d399, 0x065f46, 0x34d399, &calendar,       "日历",   &ui_EventsScreen },
    { 0,  69,  74, 63, 0xfcd34d, 0xfbbf24, 0x92400e, 0xfbbf24, &alarm_clock,    "闹钟",   &ui_AlarmScreen },
    { 80, 69, 118, 63, 0xe879f9, 0xf0abfc, 0x86198f, 0xf0abfc, &cheering_light, "应援灯", &ui_LightStickScreen },
};

#define HOME_APP_COUNT (sizeof(kHomeApps) / sizeof(kHomeApps[0]))

/* 统一保存页面运行时状态，避免散落的静态指针过多。 */
typedef struct {
    lv_obj_t * time_hour;
    lv_obj_t * time_colon;
    lv_obj_t * time_minute;
    lv_obj_t * date_label;
    home_pressable_t hero;
    home_pressable_t apps[HOME_APP_COUNT];
    lv_timer_t * clock_timer;
    int32_t displayed_minute_key;
    int32_t displayed_date_key;
    bool colon_bright;
} home_view_t;

static home_view_t g_home_view;
static lv_grad_dsc_t g_home_app_icon_grads[HOME_APP_COUNT];

/* 为阴影透明度动画提供统一入口。 */
static void home_set_shadow_opa(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void home_set_press_scale(void * obj, int32_t value)
{
    if(!obj) return;

    /* Only background plates are scaled. Text and icons stay at native size. */
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, value, 0);
}

static int32_t home_get_press_scale(lv_obj_t * obj)
{
    if(!obj) return LV_SCALE_NONE;
    return lv_obj_get_style_transform_scale_x_safe(obj, LV_PART_MAIN);
}

static void home_reset_press_scale(lv_obj_t * obj)
{
    if(!obj) return;

    lv_anim_del(obj, (lv_anim_exec_xcb_t)home_set_press_scale);
    home_set_press_scale(obj, LV_SCALE_NONE);
}

static lv_opa_t home_hero_bg_opa_idle(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 94 / 100);
}

static lv_opa_t home_app_card_shadow_opa_idle(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 24 / 100);
}

static lv_opa_t home_app_card_shadow_opa_pressed(void)
{
    return (lv_opa_t)(LV_OPA_COVER * 14 / 100);
}

static lv_opa_t home_get_shadow_opa(lv_obj_t * obj)
{
    if(!obj) return home_app_card_shadow_opa_idle();
    return lv_obj_get_style_shadow_opa(obj, LV_PART_MAIN);
}

static void home_reset_app_plate_visual(lv_obj_t * plate)
{
    if(!plate) return;

    home_reset_press_scale(plate);
    lv_anim_del(plate, (lv_anim_exec_xcb_t)home_set_shadow_opa);
    lv_obj_set_style_shadow_opa(plate, home_app_card_shadow_opa_idle(), 0);
}

static void home_reset_hero_plate_visual(lv_obj_t * plate)
{
    if(!plate) return;

    home_reset_press_scale(plate);
    lv_obj_set_style_bg_opa(plate, home_hero_bg_opa_idle(), 0);
}

static bool home_is_press_end_event(lv_event_code_t code)
{
    return code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL;
}

static void home_reset_press_visuals(void)
{
    home_reset_hero_plate_visual(g_home_view.hero.plate);

    for(uint32_t i = 0; i < HOME_APP_COUNT; ++i) {
        home_reset_app_plate_visual(g_home_view.apps[i].plate);
    }
}

/* 启动一个简单的单值动画，减少重复样板代码。 */
static void home_start_anim(lv_obj_t * obj,
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

/* 初始化小按钮图标底色渐变，恢复重构前的视觉层次。 */
static void home_init_app_icon_gradients(void)
{
    const lv_opa_t opas[] = { LV_OPA_COVER, LV_OPA_COVER };
    const uint8_t fracs[] = { 0, 255 };

    for(uint8_t i = 0; i < HOME_APP_COUNT; ++i) {
        const lv_color_t colors[] = {
            lv_color_hex(kHomeApps[i].icon_grad_start_hex),
            lv_color_hex(kHomeApps[i].icon_grad_end_hex)
        };
        lv_grad_init_stops(&g_home_app_icon_grads[i], colors, opas, fracs, 2);
        lv_grad_linear_init(&g_home_app_icon_grads[i], lv_pct(0), lv_pct(0), lv_pct(100), lv_pct(100), LV_GRAD_EXTEND_PAD);
    }
}

/* 创建基础容器，默认不参与点击与滚动。 */
static lv_obj_t * home_create_base_container(lv_obj_t * parent, int32_t width, int32_t height)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void home_extend_draw_size_event_cb(lv_event_t * e)
{
    int32_t size = (int32_t)(uintptr_t)lv_event_get_user_data(e);
    lv_event_set_ext_draw_size(e, size);
}

static void home_enable_child_overflow(lv_obj_t * obj, int32_t extra_draw_size)
{
    if(!obj) return;

    lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(obj,
                        home_extend_draw_size_event_cb,
                        LV_EVENT_REFR_EXT_DRAW_SIZE,
                        (void *)(uintptr_t)extra_draw_size);
    lv_obj_refresh_ext_draw_size(obj);
}

static home_pressable_t home_create_pressable(lv_obj_t * parent, int32_t width, int32_t height)
{
    home_pressable_t pressable = { 0 };

    pressable.root = lv_btn_create(parent);
    lv_obj_remove_style_all(pressable.root);
    lv_obj_set_size(pressable.root, width, height);
    lv_obj_set_style_bg_opa(pressable.root, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(pressable.root, 0, 0);
    lv_obj_set_style_border_width(pressable.root, 0, 0);
    lv_obj_set_style_shadow_width(pressable.root, 0, 0);
    lv_obj_clear_flag(pressable.root, LV_OBJ_FLAG_SCROLLABLE);
    home_enable_child_overflow(pressable.root, HOME_PRESSABLE_EXT_DRAW_SIZE);

    pressable.plate = home_create_base_container(pressable.root, width, height);
    lv_obj_set_style_transform_pivot_x(pressable.plate, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(pressable.plate, height / 2, 0);
    lv_obj_center(pressable.plate);

    pressable.content = home_create_base_container(pressable.root, width, height);
    lv_obj_center(pressable.content);

    return pressable;
}

/* 创建基础文字对象，统一默认样式。 */
static lv_obj_t * home_create_base_label(lv_obj_t * parent,
                                         const char * text,
                                         const lv_font_t * font,
                                         lv_color_t color)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    if(font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/* 按显示区域缩放图标，保证不同源图尺寸下视觉一致。 */
static void home_set_image_scaled(lv_obj_t * img,
                                  const void * src,
                                  int32_t box_width,
                                  int32_t box_height,
                                  int32_t visual_px)
{
    if(!img || !src) return;

    lv_image_set_src(img, src);
    lv_obj_set_size(img, box_width, box_height);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_antialias(img, true);

    lv_image_header_t header;
    if(lv_image_decoder_get_info(src, &header) == LV_RESULT_OK) {
        int32_t src_width = (int32_t)header.w;
        int32_t src_height = (int32_t)header.h;
        int32_t src_max = (src_width > src_height) ? src_width : src_height;
        int32_t scale = (int32_t)((int64_t)visual_px * LV_SCALE_NONE / (src_max > 0 ? src_max : 1));
        lv_image_set_scale(img, scale > 0 ? scale : 1);
    }
}

/* 安全获取当前本地时间，兼容 Windows 和其他平台。 */
static bool home_get_local_time(struct tm * out_time)
{
    if(!out_time) return false;

    time_t now = time(NULL);

#if defined(_WIN32)
    return localtime_s(out_time, &now) == 0;
#else
    struct tm * local_time = localtime(&now);
    if(!local_time) return false;
    *out_time = *local_time;
    return true;
#endif
}

/* 仅在分钟或日期变化时更新标签，避免无意义的文本重绘。 */
static void home_update_clock(bool force)
{
    if(!g_home_view.time_hour || !g_home_view.time_minute || !g_home_view.date_label) return;

    struct tm local_time;
    if(!home_get_local_time(&local_time)) return;

    char hour_buf[3];
    char minute_buf[3];
    char date_buf[48];
    int32_t date_key = local_time.tm_year * 366 + local_time.tm_yday;
    int32_t minute_key = ((date_key * 24 + local_time.tm_hour) * 60) + local_time.tm_min;
    const char * week_day = kHomeWeekDays[
        (local_time.tm_wday >= 0 && local_time.tm_wday <= 6) ? local_time.tm_wday : 0
    ];

    if(force || minute_key != g_home_view.displayed_minute_key) {
        lv_snprintf(hour_buf, sizeof(hour_buf), "%02d", local_time.tm_hour);
        lv_snprintf(minute_buf, sizeof(minute_buf), "%02d", local_time.tm_min);
        lv_label_set_text(g_home_view.time_hour, hour_buf);
        lv_label_set_text(g_home_view.time_minute, minute_buf);
        g_home_view.displayed_minute_key = minute_key;
    }

    if(force || date_key != g_home_view.displayed_date_key) {
        lv_snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s", local_time.tm_mon + 1, local_time.tm_mday, week_day);
        lv_label_set_text(g_home_view.date_label, date_buf);
        g_home_view.displayed_date_key = date_key;
    }
}

static void home_set_colon_bright(bool bright)
{
    if(!g_home_view.time_colon) return;

    g_home_view.colon_bright = bright;
    lv_obj_set_style_text_opa(g_home_view.time_colon,
                              bright ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 20 / 100),
                              0);
}

/* 每秒检查时钟变化，并以整秒切换冒号，避免连续逐帧动画。 */
static void home_clock_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    home_update_clock(false);
    home_set_colon_bright(!g_home_view.colon_bright);
}

/* 统一判断当前点击是否应被控制中心或拖拽释放拦截。 */
static bool home_is_valid_release(lv_event_t * e)
{
    return !app_touch_event_is_drag_release(e);
}

/* 跳转到指定目标页面。 */
static void home_open_screen(lv_obj_t ** target_screen)
{
    /* 添加时间: 2026-08-19
     * 原因: 主界面松手回调里同步切页会假死。
     * 逻辑: 仍打开原来的 target_screen，只把 ui_nav_push 延后到触摸事件结束。 */
    if(!target_screen) return;
    ui_nav_push_async(target_screen);
}

/* AI 入口点击事件，负责按压反馈和页面跳转。 */
static void home_hero_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * button = lv_event_get_current_target(e);
    lv_obj_t * plate = g_home_view.hero.plate;

    if(!button || !plate) return;

    if(home_is_press_end_event(code)) {
        bool should_open = code == LV_EVENT_RELEASED && home_is_valid_release(e) && !ui_ControlCenter_is_visible();
        if(should_open) {
            home_reset_hero_plate_visual(plate);
            home_open_screen(&ui_AIChatScreen);
        } else {
            home_start_anim(plate,
                            (lv_anim_exec_xcb_t)home_set_press_scale,
                            home_get_press_scale(plate),
                            LV_SCALE_NONE,
                            110);
        }
        return;
    }

    if(ui_ControlCenter_is_visible()) return;

    if(code == LV_EVENT_PRESSED) {
        home_start_anim(plate,
                        (lv_anim_exec_xcb_t)home_set_press_scale,
                        home_get_press_scale(plate),
                        HOME_HERO_PRESSED_SCALE,
                        90);
    }
}

/* 应用入口点击事件，负责卡片动画与页面跳转。 */
static void home_app_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * item = lv_event_get_current_target(e);
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t * plate = index < HOME_APP_COUNT ? g_home_view.apps[index].plate : NULL;

    if(!item) return;

    if(home_is_press_end_event(code)) {
        bool should_open = code == LV_EVENT_RELEASED && home_is_valid_release(e) && !ui_ControlCenter_is_visible();

        if(plate) {
            if(should_open) {
                home_reset_app_plate_visual(plate);
            } else {
                home_start_anim(plate,
                                (lv_anim_exec_xcb_t)home_set_press_scale,
                                home_get_press_scale(plate),
                                LV_SCALE_NONE,
                                120);
                home_start_anim(plate,
                                (lv_anim_exec_xcb_t)home_set_shadow_opa,
                                (int32_t)home_get_shadow_opa(plate),
                                (int32_t)home_app_card_shadow_opa_idle(),
                                120);
            }
        }

        if(should_open) {
            if(index < HOME_APP_COUNT) {
                home_open_screen(kHomeApps[index].target_screen);
            }
        }
        return;
    }

    if(ui_ControlCenter_is_visible()) return;

    if(code == LV_EVENT_PRESSED) {
        if(plate) {
            home_start_anim(plate,
                            (lv_anim_exec_xcb_t)home_set_press_scale,
                            home_get_press_scale(plate),
                            HOME_APP_PRESSED_SCALE,
                            90);
            home_start_anim(plate, (lv_anim_exec_xcb_t)home_set_shadow_opa,
                            (int32_t)home_get_shadow_opa(plate),
                            (int32_t)home_app_card_shadow_opa_pressed(),
                            90);
        }
    }
}

/* 页面进入或离开时清理没有完成的按压反馈。 */
static void home_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_SCREEN_LOAD_START) {
        home_reset_press_visuals();
        home_update_clock(true);
        home_set_colon_bright(true);
        if(g_home_view.clock_timer) {
            lv_timer_reset(g_home_view.clock_timer);
            lv_timer_resume(g_home_view.clock_timer);
        }
    } else if(code == LV_EVENT_SCREEN_UNLOAD_START) {
        home_reset_press_visuals();
        if(g_home_view.clock_timer) {
            lv_timer_pause(g_home_view.clock_timer);
        }
    }
}

/* 构建顶部时钟和日期区域。 */
static lv_obj_t * home_build_clock_section(lv_obj_t * parent)
{
    lv_obj_t * clock_container = home_create_base_container(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(clock_container, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t * time_row = home_create_base_container(clock_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(time_row, 2, 0);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(time_row, LV_ALIGN_TOP_MID, 0, 0);

    g_home_view.time_hour = home_create_base_label(time_row, "00", &lv_font_montserrat_44, lv_color_white());
    lv_obj_set_style_text_letter_space(g_home_view.time_hour, -2, 0);

    g_home_view.time_colon = home_create_base_label(time_row, ":", &lv_font_montserrat_32, lv_color_white());
    lv_obj_set_style_text_opa(g_home_view.time_colon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(g_home_view.time_colon, -1, 0);

    g_home_view.time_minute = home_create_base_label(time_row, "00", &lv_font_montserrat_44, lv_color_white());
    lv_obj_set_style_text_letter_space(g_home_view.time_minute, -2, 0);

    g_home_view.date_label = home_create_base_label(clock_container, "1月1日 星期一", ui_builtin_text_font(), lv_color_white());
    lv_obj_set_style_text_opa(g_home_view.date_label, (lv_opa_t)(LV_OPA_COVER * 50 / 100), 0);
    lv_obj_set_style_text_letter_space(g_home_view.date_label, 1, 0);
    lv_obj_align(g_home_view.date_label, LV_ALIGN_TOP_MID, 0, 52);

    return clock_container;
}

/* 构建 AI 男友入口卡片。 */
static lv_obj_t * home_build_hero_button(lv_obj_t * parent, lv_obj_t * anchor)
{
    home_pressable_t pressable = home_create_pressable(parent, 198, 66);
    lv_obj_t * button = pressable.root;
    lv_obj_t * plate = pressable.plate;
    lv_obj_t * content = pressable.content;

    lv_obj_align_to(button, anchor, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_set_style_radius(plate, 22, 0);
    lv_obj_set_style_bg_color(plate, lv_color_hex(0xb24d72), 0);
    lv_obj_set_style_bg_opa(plate, home_hero_bg_opa_idle(), 0);
    lv_obj_set_style_border_width(plate, 2, 0);
    lv_obj_set_style_border_color(plate, lv_color_white(), 0);
    lv_obj_set_style_border_opa(plate, (lv_opa_t)(LV_OPA_COVER * 12 / 100), 0);
    lv_obj_set_style_shadow_width(plate, 5, 0);
    lv_obj_set_style_shadow_color(plate, lv_color_hex(0x681f42), 0);
    lv_obj_set_style_shadow_opa(plate, (lv_opa_t)(LV_OPA_COVER * 20 / 100), 0);
    lv_obj_set_style_shadow_ofs_y(plate, 2, 0);

    lv_obj_set_style_pad_hor(content, 20, 0);
    lv_obj_set_style_pad_gap(content, 16, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * icon_box = home_create_base_container(content, 40, 40);
    lv_obj_set_style_radius(icon_box, 12, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xfbcfe8), 0);
    lv_obj_set_style_bg_opa(icon_box, (lv_opa_t)(LV_OPA_COVER * 20 / 100), 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(icon_box, (lv_opa_t)(LV_OPA_COVER * 16 / 100), 0);

    lv_obj_t * icon = lv_image_create(icon_box);
    home_set_image_scaled(icon, &ai_love_information, 20, 20, 20);
    lv_obj_center(icon);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    ui_make_decor_hit_passthrough(icon);

    lv_obj_t * title = home_create_base_label(content, "专属陪伴\n随时陪着你", ui_builtin_text_font(), lv_color_white());
    lv_obj_set_style_text_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_style_text_line_space(title, 2, 0);
    ui_make_decor_hit_passthrough(title);

    g_home_view.hero = pressable;
    return button;
}

/* 构建单个应用入口卡片。 */
static lv_obj_t * home_build_app_item(lv_obj_t * parent, const home_app_config_t * config, uint8_t index)
{
    if(!config) return NULL;

    lv_color_t accent = lv_color_hex(config->accent_hex);
    lv_color_t glow = lv_color_hex(config->glow_hex);

    home_pressable_t pressable = home_create_pressable(parent, config->w, config->h);
    lv_obj_t * item = pressable.root;
    lv_obj_t * plate = pressable.plate;
    lv_obj_t * content = pressable.content;

    lv_obj_set_pos(item, config->x, config->y);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_CANCEL, (void *)(uintptr_t)index);

    lv_obj_set_style_radius(plate, 15, 0);
    lv_obj_set_style_bg_color(plate, accent, 0);
    lv_obj_set_style_bg_opa(plate, (lv_opa_t)(LV_OPA_COVER * 28 / 100), 0);
    lv_obj_set_style_border_width(plate, 2, 0);
    lv_obj_set_style_border_color(plate, accent, 0);
    lv_obj_set_style_border_opa(plate, (lv_opa_t)(LV_OPA_COVER * 18 / 100), 0);
    lv_obj_set_style_shadow_width(plate, 5, 0);
    lv_obj_set_style_shadow_color(plate, glow, 0);
    lv_obj_set_style_shadow_opa(plate, (lv_opa_t)(LV_OPA_COVER * 24 / 100), 0);
    lv_obj_set_style_shadow_ofs_y(plate, 1, 0);

    lv_obj_set_style_pad_left(content, 12, 0);
    lv_obj_set_style_pad_right(content, 12, 0);
    lv_obj_set_style_pad_gap(content, 5, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * icon_box = home_create_base_container(content, 28, 28);
    lv_obj_set_style_radius(icon_box, 9, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(config->icon_grad_start_hex), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(icon_box, &g_home_app_icon_grads[index], 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_set_style_border_color(icon_box, accent, 0);
    lv_obj_set_style_border_opa(icon_box, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);

    lv_obj_t * icon = lv_image_create(icon_box);
    /* 小图标资源已改为原生 16x16，这里直接按原尺寸显示，避免再次缩放。 */
    lv_image_set_src(icon, config->icon_src);
    lv_obj_set_size(icon, 16, 16);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_antialias(icon, false);
    lv_obj_center(icon);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    ui_make_decor_hit_passthrough(icon);

    lv_obj_t * label = home_create_base_label(content, config->label_text, ui_builtin_text_font(), accent);
    lv_obj_set_style_text_opa(label, (lv_opa_t)(LV_OPA_COVER * 78 / 100), 0);
    ui_make_decor_hit_passthrough(label);

    g_home_view.apps[index] = pressable;
    return item;
}

/* 批量构建底部应用入口区域。 */
static lv_obj_t * home_build_app_grid(lv_obj_t * parent, lv_obj_t * anchor)
{
    lv_obj_t * grid = home_create_base_container(parent, 198, 132);
    home_enable_child_overflow(grid, HOME_PRESSABLE_EXT_DRAW_SIZE);
    lv_obj_align_to(grid, anchor, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    for(uint8_t i = 0; i < HOME_APP_COUNT; ++i) {
        (void)home_build_app_item(grid, &kHomeApps[i], i);
    }

    return grid;
}

/* 初始化首页根屏幕基础样式。 */
static void home_build_screen_root(void)
{
    ui_HomeScreen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui_HomeScreen);
    lv_obj_set_size(ui_HomeScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_HomeScreen, lv_color_hex(0x09090f), 0);
    lv_obj_set_style_bg_opa(ui_HomeScreen, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_HomeScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_HomeScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_HomeScreen, home_screen_event_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
    lv_obj_add_event_cb(ui_HomeScreen, home_screen_event_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
}

/* 清理运行时缓存指针，便于页面销毁后安全重建。 */
static void home_reset_runtime_state(void)
{
    g_home_view = (home_view_t) { 0 };
    g_home_view.displayed_minute_key = -1;
    g_home_view.displayed_date_key = -1;
    g_home_view.colon_bright = true;
}

void ui_HomeScreen_init(void)
{
    if(ui_HomeScreen) return;

    home_reset_runtime_state();
    home_init_app_icon_gradients();
    home_build_screen_root();

    lv_obj_t * clock_section = home_build_clock_section(ui_HomeScreen);
    lv_obj_t * hero_button = home_build_hero_button(ui_HomeScreen, clock_section);
    (void)home_build_app_grid(ui_HomeScreen, hero_button);

    ui_ControlCenter_init(ui_HomeScreen);
    home_update_clock(true);
    g_home_view.clock_timer = lv_timer_create(home_clock_timer_cb, 1000, NULL);
    if(g_home_view.clock_timer) {
        lv_timer_pause(g_home_view.clock_timer);
    }
    app_screen_enable_swipe_back(ui_HomeScreen);
}

void ui_HomeScreen_deinit(void)
{
    if(g_home_view.clock_timer) {
        lv_timer_delete(g_home_view.clock_timer);
        g_home_view.clock_timer = NULL;
    }

    ui_ControlCenter_deinit();

    if(ui_HomeScreen) {
        lv_obj_delete(ui_HomeScreen);
        ui_HomeScreen = NULL;
    }

    home_reset_runtime_state();
}

bool ui_HomeScreen_dismiss_overlays(void)
{
    return ui_ControlCenter_dismiss_overlays();
}

void ui_HomeScreen_reset_transient_state(void)
{
    home_reset_press_visuals();
    /* 恢复缓存页上的按压反馈和覆盖层状态。 */
    (void)ui_HomeScreen_dismiss_overlays();
}
