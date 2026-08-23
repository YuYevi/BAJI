/**
 * @file ui_LightStickScreen.c
 * @brief 应援灯设置屏幕实现
 *
 * 重构目标：
 * 1. 统一管理页面状态，避免控件引用分散在多个全局变量中。
 * 2. 将“界面搭建”和“状态刷新”解耦，便于后续维护与扩展。
 * 3. 保持现有功能完整：颜色选择、预览、应用全屏纯色、点击关闭遮罩。
 */

#include "ui_LightStickScreen.h"
#include "../ui.h"
#include "../app_common.h"

extern const lv_image_dsc_t loving_heart;
extern const lv_image_dsc_t pentagram;
extern const lv_image_dsc_t four_pointed_star;

lv_obj_t * ui_LightStickScreen;

#define LIGHT_STICK_DEFAULT_COLOR  0x9A91F2u
#define LIGHT_STICK_BG_COLOR       0x09090Fu
#define LIGHT_STICK_IDLE_BORDER    0x333333u
#define LIGHT_STICK_PREVIEW_BORDER 4

static const uint32_t s_light_palette[] = {
    0xFF0000u, 0xFF6600u, 0xFFDD00u, 0x00CC44u,
    0x00BBFFu, 0x0055FFu, 0x8800FFu, 0xFFFFFFu,
};

/* 用结构体收敛页面运行期状态，后续增减控件时更容易维护。 */
typedef struct {
    lv_obj_t * screen;
    lv_obj_t * preview;
    lv_obj_t * color_dot;
    lv_obj_t * hex_label;
    lv_obj_t * featured_btn;
    lv_obj_t * palette;
    lv_obj_t * apply_btn;
    lv_obj_t * apply_icon;
    lv_obj_t * apply_label;
    lv_obj_t * overlay;
    uint32_t selected_hex;
    uint8_t saved_brightness;
    bool overlay_active;
} light_stick_screen_t;

static light_stick_screen_t s_light_stick;

static void light_color_event_cb(lv_event_t * e);
static void light_apply_btn_cb(lv_event_t * e);
static void light_overlay_event_cb(lv_event_t * e);
static void light_screen_event_cb(lv_event_t * e);

static uint32_t light_get_selected_hex(void)
{
    return s_light_stick.selected_hex;
}

static lv_color_t light_hex_to_color(uint32_t hex)
{
    return lv_color_hex(hex);
}

/* 根据亮度自动切换按钮前景色，保证浅色背景下文字仍清晰。 */
static lv_color_t light_get_contrast_color(uint32_t hex)
{
    uint32_t r = (hex >> 16) & 0xFFu;
    uint32_t g = (hex >> 8) & 0xFFu;
    uint32_t b = hex & 0xFFu;
    uint32_t luminance = (299u * r) + (587u * g) + (114u * b);

    return (luminance >= 140000u) ? lv_color_black() : lv_color_white();
}

static lv_obj_t * light_create_clean_obj(lv_obj_t * parent)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t * light_create_row(lv_obj_t * parent, lv_coord_t gap)
{
    lv_obj_t * row = light_create_clean_obj(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_gap(row, gap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void light_enable_press_glow(lv_obj_t * obj, lv_color_t color)
{
    if(!obj) return;

    /* The selected swatch already uses a border; a second pressed outline
     * makes it look double-ringed. Keep the feedback as a soft halo only. */
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_pad(obj, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(obj, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(obj, 10, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(obj, 1, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(obj, color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)(LV_OPA_COVER * 38 / 100), LV_STATE_PRESSED);

    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(parent) {
        lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
}

/* 颜色按钮和特色胶囊都复用同一套 user_data 方案，便于统一刷新选中态。 */
static lv_obj_t * light_create_color_button(lv_obj_t * parent, uint32_t color_hex, lv_coord_t size)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, light_hex_to_color(color_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, light_hex_to_color(color_hex), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(LIGHT_STICK_IDLE_BORDER), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_shadow_color(btn, light_hex_to_color(color_hex), 0);
    lv_obj_set_style_shadow_opa(btn, 0, 0);
    lv_obj_set_user_data(btn, (void *)(uintptr_t)color_hex);
    lv_obj_add_event_cb(btn, light_color_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color_hex);
    light_enable_press_glow(btn, light_hex_to_color(color_hex));
    return btn;
}

static void light_set_overlay_visible(bool visible)
{
    if(!s_light_stick.overlay) {
        return;
    }

    if(s_light_stick.overlay_active == visible) {
        return;
    }

    if(visible) {
        lv_obj_clear_flag(s_light_stick.overlay, LV_OBJ_FLAG_HIDDEN);
        s_light_stick.overlay_active = true;
        s_light_stick.saved_brightness = app_device_get_brightness();
        app_device_set_brightness(100, false);
        app_status_overlay_set_visible(false);
        app_screen_set_swipe_back_enabled(s_light_stick.screen, false);
        lv_obj_move_foreground(s_light_stick.overlay);
    }
    else {
        lv_obj_add_flag(s_light_stick.overlay, LV_OBJ_FLAG_HIDDEN);
        s_light_stick.overlay_active = false;
        app_device_set_brightness(s_light_stick.saved_brightness > 0 ? s_light_stick.saved_brightness : 1, false);
        app_status_overlay_set_visible(true);
        app_screen_enable_swipe_back(s_light_stick.screen);
    }
}

static void light_refresh_info_row(void)
{
    char color_text[16];

    if(s_light_stick.color_dot) {
        lv_obj_set_style_bg_color(s_light_stick.color_dot, light_hex_to_color(light_get_selected_hex()), 0);
        lv_obj_set_style_bg_opa(s_light_stick.color_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_light_stick.color_dot, light_hex_to_color(light_get_selected_hex()), 0);
        lv_obj_set_style_shadow_width(s_light_stick.color_dot, 0, 0);
        lv_obj_set_style_shadow_opa(s_light_stick.color_dot, 0, 0);
    }

    if(s_light_stick.hex_label) {
        lv_snprintf(color_text, sizeof(color_text), "#%06X", (unsigned)light_get_selected_hex());
        lv_label_set_text(s_light_stick.hex_label, color_text);
    }
}

static void light_refresh_preview(void)
{
    if(s_light_stick.preview) {
        lv_obj_set_style_bg_color(s_light_stick.preview, light_hex_to_color(light_get_selected_hex()), 0);
        lv_obj_set_style_bg_opa(s_light_stick.preview, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_light_stick.preview, LIGHT_STICK_PREVIEW_BORDER, 0);
        lv_obj_set_style_border_color(s_light_stick.preview, light_hex_to_color(light_get_selected_hex()), 0);
        lv_obj_set_style_shadow_width(s_light_stick.preview, 0, 0);
        lv_obj_set_style_shadow_opa(s_light_stick.preview, 0, 0);
    }

    if(s_light_stick.overlay) {
        lv_obj_set_style_bg_color(s_light_stick.overlay, light_hex_to_color(light_get_selected_hex()), 0);
        lv_obj_set_style_bg_opa(s_light_stick.overlay, LV_OPA_COVER, 0);
    }
}

static void light_refresh_featured_button(void)
{
    bool is_selected;

    if(!s_light_stick.featured_btn) {
        return;
    }

    is_selected = ((uint32_t)(uintptr_t)lv_obj_get_user_data(s_light_stick.featured_btn) == light_get_selected_hex());
    lv_obj_set_style_border_width(s_light_stick.featured_btn, is_selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(
        s_light_stick.featured_btn,
        is_selected ? lv_color_white() : lv_color_hex(LIGHT_STICK_IDLE_BORDER),
        0);
    lv_obj_set_style_border_opa(
        s_light_stick.featured_btn,
        is_selected ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 20 / 100),
        0);
    lv_obj_set_style_outline_width(s_light_stick.featured_btn, 0, 0);
    lv_obj_set_style_outline_pad(s_light_stick.featured_btn, 0, 0);
    lv_obj_set_style_outline_color(s_light_stick.featured_btn, lv_color_white(), 0);
    lv_obj_set_style_outline_opa(s_light_stick.featured_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_light_stick.featured_btn, 0, 0);
    lv_obj_set_style_shadow_color(s_light_stick.featured_btn, light_hex_to_color(light_get_selected_hex()), 0);
    lv_obj_set_style_shadow_opa(s_light_stick.featured_btn, 0, 0);
    lv_obj_set_style_bg_color(s_light_stick.featured_btn, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_light_stick.featured_btn,
                            (lv_opa_t)(LV_OPA_COVER * 6 / 100),
                            LV_STATE_PRESSED);
    light_enable_press_glow(s_light_stick.featured_btn,
                            light_hex_to_color(LIGHT_STICK_DEFAULT_COLOR));
}

static void light_refresh_palette(void)
{
    uint32_t selected_hex;
    uint32_t child_count;
    uint32_t i;

    if(!s_light_stick.palette) {
        return;
    }

    selected_hex = light_get_selected_hex();
    child_count = lv_obj_get_child_count(s_light_stick.palette);

    for(i = 0; i < child_count; i++) {
        lv_obj_t * btn = lv_obj_get_child(s_light_stick.palette, i);
        uint32_t btn_hex;
        bool is_selected;

        if(!btn) {
            continue;
        }

        btn_hex = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);
        is_selected = (btn_hex == selected_hex);

        lv_obj_set_style_border_width(btn, is_selected ? 3 : 2, 0);
        lv_obj_set_style_border_color(
            btn,
            is_selected ? lv_color_white() : lv_color_hex(LIGHT_STICK_IDLE_BORDER),
            0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_shadow_color(btn, light_hex_to_color(btn_hex), 0);
        lv_obj_set_style_shadow_opa(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_pad(btn, 0, 0);
        lv_obj_set_style_outline_color(btn, lv_color_white(), 0);
        lv_obj_set_style_outline_opa(btn, LV_OPA_TRANSP, 0);
    }
}

static void light_refresh_apply_button(void)
{
    lv_color_t foreground_color;

    if(!s_light_stick.apply_btn) {
        return;
    }

    foreground_color = light_get_contrast_color(light_get_selected_hex());

    lv_obj_set_style_bg_color(s_light_stick.apply_btn, light_hex_to_color(light_get_selected_hex()), 0);
    lv_obj_set_style_bg_opa(s_light_stick.apply_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_light_stick.apply_btn,
                              light_hex_to_color(light_get_selected_hex()),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_light_stick.apply_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_light_stick.apply_btn, 1, 0);
    lv_obj_set_style_border_color(s_light_stick.apply_btn, foreground_color, 0);
    lv_obj_set_style_shadow_color(s_light_stick.apply_btn, light_hex_to_color(light_get_selected_hex()), 0);
    lv_obj_set_style_shadow_width(s_light_stick.apply_btn, 0, 0);
    lv_obj_set_style_shadow_opa(s_light_stick.apply_btn, 0, 0);
    light_enable_press_glow(s_light_stick.apply_btn,
                            light_hex_to_color(light_get_selected_hex()));

    if(s_light_stick.apply_label) {
        lv_obj_set_style_text_color(s_light_stick.apply_label, foreground_color, 0);
    }

    if(s_light_stick.apply_icon) {
        lv_obj_set_style_image_recolor(s_light_stick.apply_icon, foreground_color, 0);
        lv_obj_set_style_image_recolor_opa(s_light_stick.apply_icon, LV_OPA_COVER, 0);
    }
}

static void light_refresh_all(void)
{
    light_refresh_info_row();
    light_refresh_preview();
    light_refresh_featured_button();
    light_refresh_palette();
    light_refresh_apply_button();
}

static void light_select_color(uint32_t color_hex)
{
    color_hex &= 0xFFFFFFu;
    if(s_light_stick.selected_hex == color_hex) {
        return;
    }

    s_light_stick.selected_hex = color_hex;
    light_refresh_all();
}

static void light_build_header(lv_obj_t * parent)
{
    lv_obj_t * header = light_create_row(parent, 0);
    lv_obj_t * title_box;
    lv_obj_t * title_icon;
    lv_obj_t * title;

    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 47);

    title_box = light_create_row(header, 6);
    lv_obj_set_style_radius(title_box, 999, 0);
    lv_obj_set_style_bg_color(title_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(title_box, (lv_opa_t)(LV_OPA_COVER * 6 / 100), 0);
    lv_obj_set_style_pad_hor(title_box, 16, 0);
    lv_obj_set_style_pad_ver(title_box, 8, 0);
    lv_obj_clear_flag(title_box, LV_OBJ_FLAG_CLICKABLE);

    title_icon = lv_image_create(title_box);
    lv_image_set_src(title_icon, &loving_heart);
    lv_obj_set_size(title_icon, 16, 16);
    lv_image_set_inner_align(title_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_clear_flag(title_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(title_box);
    lv_obj_remove_style_all(title);
    lv_label_set_text(title, "应援灯");
    lv_obj_set_style_text_font(title, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_opa(title, (lv_opa_t)(LV_OPA_COVER * 75 / 100), 0);
}

static void light_build_preview(lv_obj_t * parent)
{
    s_light_stick.preview = light_create_clean_obj(parent);
    lv_obj_set_size(s_light_stick.preview, 92, 92);
    lv_obj_set_style_radius(s_light_stick.preview, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_light_stick.preview, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_clear_flag(s_light_stick.preview, LV_OBJ_FLAG_CLICKABLE);
}

static void light_build_info_row(lv_obj_t * parent)
{
    lv_obj_t * info_row = light_create_row(parent, 2);

    lv_obj_align(info_row, LV_ALIGN_TOP_MID, 0, 180);

    s_light_stick.color_dot = light_create_clean_obj(info_row);
    lv_obj_set_size(s_light_stick.color_dot, 10, 10);
    lv_obj_set_style_radius(s_light_stick.color_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_light_stick.color_dot, 1, 0);
    lv_obj_set_style_border_color(s_light_stick.color_dot, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_light_stick.color_dot, (lv_opa_t)(LV_OPA_COVER * 25 / 100), 0);
    lv_obj_clear_flag(s_light_stick.color_dot, LV_OBJ_FLAG_CLICKABLE);

    s_light_stick.hex_label = lv_label_create(info_row);
    lv_obj_remove_style_all(s_light_stick.hex_label);
    lv_label_set_text(s_light_stick.hex_label, "#9A91F2");
    lv_obj_set_style_text_font(s_light_stick.hex_label, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(s_light_stick.hex_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_light_stick.hex_label, (lv_opa_t)(LV_OPA_COVER * 40 / 100), 0);
    lv_obj_set_style_text_letter_space(s_light_stick.hex_label, 0, 0);
}

static void light_build_featured_button(lv_obj_t * parent)
{
    lv_obj_t * color_ball;
    lv_obj_t * star_icon;

    s_light_stick.featured_btn = lv_btn_create(parent);
    lv_obj_remove_style_all(s_light_stick.featured_btn);
    lv_obj_set_size(s_light_stick.featured_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_light_stick.featured_btn, 999, 0);
    lv_obj_set_style_bg_color(s_light_stick.featured_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_light_stick.featured_btn, (lv_opa_t)(LV_OPA_COVER * 6 / 100), 0);
    lv_obj_set_style_pad_hor(s_light_stick.featured_btn, 16, 0);
    lv_obj_set_style_pad_ver(s_light_stick.featured_btn, 8, 0);
    lv_obj_set_style_pad_gap(s_light_stick.featured_btn, 6, 0);
    lv_obj_set_flex_flow(s_light_stick.featured_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_light_stick.featured_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_light_stick.featured_btn, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_clear_flag(s_light_stick.featured_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(s_light_stick.featured_btn, (void *)(uintptr_t)LIGHT_STICK_DEFAULT_COLOR);
    lv_obj_add_event_cb(
        s_light_stick.featured_btn,
        light_color_event_cb,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)LIGHT_STICK_DEFAULT_COLOR);
    light_enable_press_glow(s_light_stick.featured_btn,
                            light_hex_to_color(LIGHT_STICK_DEFAULT_COLOR));

    color_ball = light_create_clean_obj(s_light_stick.featured_btn);
    lv_obj_set_size(color_ball, 12, 12);
    lv_obj_set_style_radius(color_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(color_ball, lv_color_hex(LIGHT_STICK_DEFAULT_COLOR), 0);
    lv_obj_set_style_bg_opa(color_ball, LV_OPA_COVER, 0);
    lv_obj_clear_flag(color_ball, LV_OBJ_FLAG_CLICKABLE);

    star_icon = lv_image_create(s_light_stick.featured_btn);
    lv_image_set_src(star_icon, &pentagram);
    lv_obj_set_size(star_icon, 16, 16);
    lv_image_set_inner_align(star_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_clear_flag(star_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void light_build_palette(lv_obj_t * parent)
{
    lv_obj_t * palette_section = light_create_clean_obj(parent);
    uint32_t i;

    lv_obj_set_size(palette_section, LV_PCT(100), 48);
    lv_obj_set_style_pad_left(palette_section, 9, 0);
    lv_obj_set_style_pad_right(palette_section, 9, 0);
    lv_obj_align(palette_section, LV_ALIGN_TOP_MID, 0, 234);
    lv_obj_add_flag(palette_section, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    s_light_stick.palette = light_create_row(palette_section, 2);
    lv_obj_set_size(s_light_stick.palette, LV_PCT(100), 38);
    lv_obj_align(s_light_stick.palette, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_light_stick.palette, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    for(i = 0; i < (sizeof(s_light_palette) / sizeof(s_light_palette[0])); i++) {
        (void)light_create_color_button(s_light_stick.palette, s_light_palette[i], 38);
    }
}

static void light_build_apply_button(lv_obj_t * parent)
{
    s_light_stick.apply_btn = lv_btn_create(parent);
    lv_obj_remove_style_all(s_light_stick.apply_btn);
    lv_obj_set_size(s_light_stick.apply_btn, 150, 42);
    lv_obj_set_style_radius(s_light_stick.apply_btn, 21, 0);
    lv_obj_align(s_light_stick.apply_btn, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_set_style_border_opa(s_light_stick.apply_btn, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_set_style_pad_hor(s_light_stick.apply_btn, 16, 0);
    lv_obj_set_style_pad_gap(s_light_stick.apply_btn, 6, 0);
    lv_obj_set_flex_flow(s_light_stick.apply_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_light_stick.apply_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_light_stick.apply_btn, light_apply_btn_cb, LV_EVENT_CLICKED, NULL);
    light_enable_press_glow(s_light_stick.apply_btn,
                            light_hex_to_color(LIGHT_STICK_DEFAULT_COLOR));

    s_light_stick.apply_icon = lv_image_create(s_light_stick.apply_btn);
    lv_image_set_src(s_light_stick.apply_icon, &four_pointed_star);
    lv_obj_set_size(s_light_stick.apply_icon, 16, 16);
    lv_image_set_inner_align(s_light_stick.apply_icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_clear_flag(s_light_stick.apply_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_light_stick.apply_label = lv_label_create(s_light_stick.apply_btn);
    lv_obj_remove_style_all(s_light_stick.apply_label);
    lv_label_set_text(s_light_stick.apply_label, "应用");
    lv_obj_set_style_text_font(s_light_stick.apply_label, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(s_light_stick.apply_label, lv_color_white(), 0);
}

static void light_build_overlay(lv_obj_t * parent)
{
    s_light_stick.overlay = light_create_clean_obj(parent);
    lv_obj_set_size(s_light_stick.overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(s_light_stick.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_light_stick.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_light_stick.overlay, light_overlay_event_cb, LV_EVENT_CLICKED, NULL);
}

static void light_build_screen(void)
{
    s_light_stick.screen = light_create_clean_obj(NULL);
    lv_obj_set_size(s_light_stick.screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_light_stick.screen, lv_color_hex(LIGHT_STICK_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_light_stick.screen, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_light_stick.screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_light_stick.screen, light_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(s_light_stick.screen, light_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    light_build_header(s_light_stick.screen);
    light_build_preview(s_light_stick.screen);
    light_build_info_row(s_light_stick.screen);
    light_build_featured_button(s_light_stick.screen);
    light_build_palette(s_light_stick.screen);
    light_build_apply_button(s_light_stick.screen);
    light_build_overlay(s_light_stick.screen);
}

static void light_reset_state(void)
{
    uint32_t selected_hex = s_light_stick.selected_hex;
    s_light_stick = (light_stick_screen_t){0};
    s_light_stick.selected_hex = selected_hex != 0 ? selected_hex : LIGHT_STICK_DEFAULT_COLOR;
}

static void light_color_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    light_select_color((uint32_t)(uintptr_t)lv_event_get_user_data(e));
}

static void light_apply_btn_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    light_set_overlay_visible(true);
}

static void light_overlay_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    light_set_overlay_visible(false);
}

static void light_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_SCREEN_LOADED) {
        light_set_overlay_visible(false);
    }
    else if(code == LV_EVENT_SCREEN_UNLOADED) {
        light_set_overlay_visible(false);
    }
}

void ui_LightStickScreen_init(void)
{
    if(ui_LightStickScreen) {
        return;
    }

    light_reset_state();
    light_build_screen();
    ui_LightStickScreen = s_light_stick.screen;
    light_refresh_all();
    app_screen_enable_swipe_back(ui_LightStickScreen);
}

void ui_LightStickScreen_deinit(void)
{
    light_set_overlay_visible(false);

    if(ui_LightStickScreen) {
        lv_obj_delete(ui_LightStickScreen);
    }

    ui_LightStickScreen = NULL;
    light_reset_state();
}
