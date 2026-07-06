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

/* 统一保存页面运行时状态，避免散落的静态指针过多。 */
typedef struct {
    lv_obj_t * time_hour;
    lv_obj_t * time_colon;
    lv_obj_t * time_minute;
    lv_obj_t * date_label;
    lv_obj_t * hero_button;
    lv_timer_t * clock_timer;
} home_view_t;

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

static home_view_t g_home_view;
static lv_grad_dsc_t g_home_hero_grad;
#define HOME_APP_COUNT ((uint8_t)(sizeof(kHomeApps) / sizeof(kHomeApps[0])))
static lv_grad_dsc_t g_home_app_icon_grads[4];

static const char * const kHomeWeekDays[] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
};

static const home_app_config_t kHomeApps[] = {
    { 0,   0, 118, 63, 0x5eead4, 0x2dd4bf, 0x0f766e, 0x2dd4bf, &picture,        "壁纸",   &ui_WallpaperScreen },
    { 124, 0,  74, 63, 0x6ee7b7, 0x34d399, 0x065f46, 0x34d399, &calendar,       "日历",   &ui_EventsScreen },
    { 0,  69,  74, 63, 0xfcd34d, 0xfbbf24, 0x92400e, 0xfbbf24, &alarm_clock,    "闹钟",   &ui_AlarmScreen },
    { 80, 69, 118, 63, 0xe879f9, 0xf0abfc, 0x86198f, 0xf0abfc, &cheering_light, "应援灯", &ui_LightStickScreen },
};

/* 为文字透明度动画提供统一入口。 */
static void home_set_text_opa(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

/* 为阴影透明度动画提供统一入口。 */
static void home_set_shadow_opa(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

/* 为缩放动画提供统一入口。 */
static void home_set_transform_scale(void * obj, int32_t value)
{
    if(!obj) return;
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, value, 0);
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

/* 初始化 AI 卡片使用的渐变背景。 */
static void home_init_hero_gradient(void)
{
    const lv_color_t colors[] = { lv_color_hex(0x9d174d), lv_color_hex(0xf472b6) };
    const lv_opa_t opas[] = {
        (lv_opa_t)(LV_OPA_COVER * 94 / 100),
        (lv_opa_t)(LV_OPA_COVER * 84 / 100)
    };
    const uint8_t fracs[] = { 0, 255 };

    lv_grad_init_stops(&g_home_hero_grad, colors, opas, fracs, 2);
    lv_grad_linear_init(&g_home_hero_grad, lv_pct(8), lv_pct(18), lv_pct(92), lv_pct(82), LV_GRAD_EXTEND_PAD);
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

/* 刷新时钟和日期显示。 */
static void home_update_clock(void)
{
    if(!g_home_view.time_hour || !g_home_view.time_minute || !g_home_view.date_label) return;

    struct tm local_time;
    if(!home_get_local_time(&local_time)) return;

    char hour_buf[3];
    char minute_buf[3];
    char date_buf[48];
    const char * week_day = kHomeWeekDays[
        (local_time.tm_wday >= 0 && local_time.tm_wday <= 6) ? local_time.tm_wday : 0
    ];

    lv_snprintf(hour_buf, sizeof(hour_buf), "%02d", local_time.tm_hour);
    lv_snprintf(minute_buf, sizeof(minute_buf), "%02d", local_time.tm_min);
    lv_snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s", local_time.tm_mon + 1, local_time.tm_mday, week_day);

    lv_label_set_text(g_home_view.time_hour, hour_buf);
    lv_label_set_text(g_home_view.time_minute, minute_buf);
    lv_label_set_text(g_home_view.date_label, date_buf);
}

/* 每秒刷新一次首页时钟。 */
static void home_clock_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    home_update_clock();
}

/* 设置缩放中心，避免按压动画看起来偏移。 */
static void home_prepare_scale_pivot(lv_obj_t * obj)
{
    if(!obj) return;
    lv_obj_set_style_transform_pivot_x(obj, lv_obj_get_width(obj) / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_obj_get_height(obj) / 2, 0);
}

/* 统一判断当前点击是否应被控制中心或拖拽释放拦截。 */
static bool home_is_valid_release(lv_event_t * e)
{
    return !app_touch_event_is_drag_release(e);
}

/* 跳转到指定目标页面。 */
static void home_open_screen(lv_obj_t ** target_screen)
{
    if(!target_screen) return;
    ui_nav_push(target_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
}

/* AI 入口点击事件，负责按压反馈和页面跳转。 */
static void home_hero_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * button = lv_event_get_target(e);

    if(!button || ui_ControlCenter_is_visible()) return;

    if(code == LV_EVENT_PRESSED) {
        home_prepare_scale_pivot(button);
        home_start_anim(button, (lv_anim_exec_xcb_t)home_set_transform_scale, LV_SCALE_NONE, 242, 90);
        return;
    }

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        home_start_anim(button, (lv_anim_exec_xcb_t)home_set_transform_scale, 242, LV_SCALE_NONE, 110);
        if(code == LV_EVENT_RELEASED && home_is_valid_release(e)) {
            home_open_screen(&ui_AIChatScreen);
        }
    }
}

/* 应用入口点击事件，负责卡片动画与页面跳转。 */
static void home_app_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * item = lv_event_get_target(e);
    lv_obj_t * card = item ? lv_obj_get_child(item, 0) : NULL;

    if(!item || ui_ControlCenter_is_visible()) return;

    if(code == LV_EVENT_PRESSED) {
        if(card) {
            home_prepare_scale_pivot(card);
            home_start_anim(card, (lv_anim_exec_xcb_t)home_set_transform_scale, LV_SCALE_NONE, 238, 90);
            home_start_anim(card, (lv_anim_exec_xcb_t)home_set_shadow_opa,
                            (int32_t)(LV_OPA_COVER * 32 / 100),
                            (int32_t)(LV_OPA_COVER * 18 / 100),
                            90);
        }
        return;
    }

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(card) {
            home_start_anim(card, (lv_anim_exec_xcb_t)home_set_transform_scale, 238, LV_SCALE_NONE, 120);
            home_start_anim(card, (lv_anim_exec_xcb_t)home_set_shadow_opa,
                            (int32_t)(LV_OPA_COVER * 18 / 100),
                            (int32_t)(LV_OPA_COVER * 32 / 100),
                            120);
        }

        if(code == LV_EVENT_RELEASED && home_is_valid_release(e)) {
            uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
            if(index < HOME_APP_COUNT) {
                home_open_screen(kHomeApps[index].target_screen);
            }
        }
    }
}

/* 创建时钟闪烁的冒号动画。 */
static void home_start_colon_blink_anim(void)
{
    if(!g_home_view.time_colon) return;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g_home_view.time_colon);
    lv_anim_set_values(&anim, LV_OPA_COVER, (lv_opa_t)(LV_OPA_COVER * 20 / 100));
    lv_anim_set_time(&anim, 500);
    lv_anim_set_playback_time(&anim, 500);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)home_set_text_opa);
    lv_anim_start(&anim);
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
    lv_obj_set_style_text_opa(g_home_view.time_colon, (lv_opa_t)(LV_OPA_COVER * 55 / 100), 0);
    lv_obj_set_style_text_letter_space(g_home_view.time_colon, -1, 0);

    g_home_view.time_minute = home_create_base_label(time_row, "00", &lv_font_montserrat_44, lv_color_white());
    lv_obj_set_style_text_letter_space(g_home_view.time_minute, -2, 0);

    g_home_view.date_label = home_create_base_label(clock_container, "1月1日 星期一", ui_builtin_text_font(), lv_color_white());
    lv_obj_set_style_text_opa(g_home_view.date_label, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_set_style_text_letter_space(g_home_view.date_label, 1, 0);
    lv_obj_align(g_home_view.date_label, LV_ALIGN_TOP_MID, 0, 52);

    home_start_colon_blink_anim();

    return clock_container;
}

/* 构建 AI 男友入口卡片。 */
static lv_obj_t * home_build_hero_button(lv_obj_t * parent, lv_obj_t * anchor)
{
    lv_obj_t * button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 198, 66);
    lv_obj_set_style_radius(button, 22, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xbe185d), 0);
    lv_obj_set_style_bg_opa(button, (lv_opa_t)(LV_OPA_COVER * 94 / 100), 0);
    lv_obj_set_style_bg_grad(button, &g_home_hero_grad, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_white(), 0);
    lv_obj_set_style_border_opa(button, (lv_opa_t)(LV_OPA_COVER * 12 / 100), 0);
    lv_obj_set_style_pad_hor(button, 20, 0);
    lv_obj_set_style_pad_gap(button, 16, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(button, anchor, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(button, home_hero_event_cb, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t * icon_box = home_create_base_container(button, 40, 40);
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

    lv_obj_t * title = home_create_base_label(button, "专属陪伴\n随时陪着你", ui_builtin_text_font(), lv_color_white());
    lv_obj_set_style_text_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_style_text_line_space(title, 2, 0);
    ui_make_decor_hit_passthrough(title);

    g_home_view.hero_button = button;
    return button;
}

/* 构建单个应用入口卡片。 */
static lv_obj_t * home_build_app_item(lv_obj_t * parent, const home_app_config_t * config, uint8_t index)
{
    if(!config) return NULL;

    lv_color_t accent = lv_color_hex(config->accent_hex);
    lv_color_t glow = lv_color_hex(config->glow_hex);

    lv_obj_t * item = lv_btn_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, config->w, config->h);
    lv_obj_set_pos(item, config->x, config->y);
    lv_obj_set_style_bg_opa(item, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_shadow_width(item, 0, 0);
    lv_obj_set_style_radius(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)index);
    lv_obj_add_event_cb(item, home_app_event_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)index);

    lv_obj_t * card = home_create_base_container(item, config->w, config->h);
    lv_obj_set_style_radius(card, 15, 0);
    lv_obj_set_style_bg_color(card, accent, 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)(LV_OPA_COVER * 28 / 100), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, accent, 0);
    lv_obj_set_style_border_opa(card, (lv_opa_t)(LV_OPA_COVER * 26 / 100), 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_color(card, glow, 0);
    lv_obj_set_style_shadow_opa(card, (lv_opa_t)(LV_OPA_COVER * 32 / 100), 0);
    lv_obj_set_style_shadow_ofs_y(card, 2, 0);
    lv_obj_set_style_pad_left(card, 12, 0);
    lv_obj_set_style_pad_right(card, 12, 0);
    lv_obj_set_style_pad_gap(card, 5, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * icon_box = home_create_base_container(card, 28, 28);
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

    lv_obj_t * label = home_create_base_label(card, config->label_text, ui_builtin_text_font(), accent);
    lv_obj_set_style_text_opa(label, (lv_opa_t)(LV_OPA_COVER * 78 / 100), 0);
    ui_make_decor_hit_passthrough(label);

    return item;
}

/* 批量构建底部应用入口区域。 */
static lv_obj_t * home_build_app_grid(lv_obj_t * parent, lv_obj_t * anchor)
{
    lv_obj_t * grid = home_create_base_container(parent, 198, 132);
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
}

/* 清理运行时缓存指针，便于页面销毁后安全重建。 */
static void home_reset_runtime_state(void)
{
    g_home_view.time_hour = NULL;
    g_home_view.time_colon = NULL;
    g_home_view.time_minute = NULL;
    g_home_view.date_label = NULL;
    g_home_view.hero_button = NULL;
    g_home_view.clock_timer = NULL;
}

void ui_HomeScreen_init(void)
{
    if(ui_HomeScreen) return;

    home_reset_runtime_state();
    home_init_hero_gradient();
    home_init_app_icon_gradients();
    home_build_screen_root();

    lv_obj_t * clock_section = home_build_clock_section(ui_HomeScreen);
    lv_obj_t * hero_button = home_build_hero_button(ui_HomeScreen, clock_section);
    (void)home_build_app_grid(ui_HomeScreen, hero_button);

    ui_ControlCenter_init(ui_HomeScreen);
    home_update_clock();
    g_home_view.clock_timer = lv_timer_create(home_clock_timer_cb, 1000, NULL);
    app_screen_enable_swipe_back(ui_HomeScreen);
}

void ui_HomeScreen_deinit(void)
{
    if(g_home_view.clock_timer) {
        lv_timer_delete(g_home_view.clock_timer);
        g_home_view.clock_timer = NULL;
    }

    if(g_home_view.time_colon) {
        lv_anim_del(g_home_view.time_colon, (lv_anim_exec_xcb_t)home_set_text_opa);
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
    /* 当前首页的瞬时状态只有控制中心弹层。 */
    (void)ui_HomeScreen_dismiss_overlays();
}
