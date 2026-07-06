

#include "ui_ControlCenter.h"
#include "../ui.h"
#include "../app_common.h"
#include "font_awesome.h"

extern const lv_font_t font_awesome_14_1;
extern const lv_image_dsc_t luminance;
extern const lv_image_dsc_t volume;

#define HOME_CC_PANEL_W             LV_HOR_RES
#define HOME_CC_PANEL_H             LV_VER_RES
#define HOME_CC_OPEN_Y              0
#define HOME_CC_CLOSED_Y            (-HOME_CC_PANEL_H)
#define HOME_CC_PULL_ZONE_H         44
#define HOME_CC_CLOSE_ZONE_H        88
#define HOME_CC_PANEL_RADIUS        0
#define HOME_CC_PANEL_SHADOW_IDLE   0
#define HOME_CC_PANEL_SHADOW_DRAG   0
#define HOME_CC_SLIDER_W            246
#define HOME_CC_SLIDER_H            32
#define HOME_CC_SLIDER_INSET        6
#define HOME_CC_SLIDER_CAP_SIZE     6
#define HOME_CC_SLIDER_JUMP_THR     72
#define HOME_CC_OPEN_DRAG_THR       48
#define HOME_CC_CLOSE_DRAG_THR      36
#define HOME_CC_FLING_OPEN_VECT_Y   10
#define HOME_CC_FLING_CLOSE_VECT_Y  10
#define HOME_CC_FAST_CLOSE_STEP_Y   24
#define HOME_CC_FAST_CLOSE_VECT_Y   18
#define HOME_CC_FAST_CLOSE_TRAVEL_Y 22

#define HOME_CC_NETWORK_WIFI 0
#define HOME_CC_NETWORK_4G   1

typedef struct {
    lv_obj_t * btn;
    lv_obj_t * icon;
    lv_obj_t * text;
} home_cc_network_option_t;

typedef struct {
    lv_obj_t * track;
    lv_obj_t * fill;
    lv_obj_t * cap;
    lv_obj_t * value_label;
    uint8_t value;
    bool is_volume;
    bool dragging;
    int32_t last_touch_x;
} home_cc_slider_t;

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * card;
    lv_obj_t * confirm_btn;
    lv_obj_t * cancel_btn;
} home_cc_restart_confirm_t;

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * pull_zone;
    lv_obj_t * scrim;
    lv_obj_t * panel;
    lv_obj_t * close_zone;

    home_cc_network_option_t network[2];
    home_cc_slider_t brightness;
    home_cc_slider_t volume;

    lv_obj_t * auto_power_btn;
    lv_obj_t * auto_power_icon;
    lv_obj_t * auto_power_text;
    lv_obj_t * auto_power_switch;
    lv_obj_t * auto_power_knob;

    home_cc_restart_confirm_t restart_confirm;

    uint8_t network_mode;
    bool auto_power_enabled;
    bool panel_open;
    bool panel_dragging;
    bool panel_animating;
    bool drag_started_open;
    lv_opa_t scrim_opa;

    int32_t drag_start_y;
    int32_t drag_panel_start_y;
    int32_t drag_last_delta_y;
    int32_t drag_last_point_y;
    int32_t drag_last_step_y;
} home_cc_context_t;

static home_cc_context_t g_home_cc;

static lv_color_t home_cc_color_bg(void)            { return lv_color_hex(0x11111d); }
static lv_color_t home_cc_color_white(void)         { return lv_color_white(); }
static lv_color_t home_cc_color_purple(void)        { return lv_color_hex(0xA855F7); }
static lv_color_t home_cc_color_purple_border(void) { return lv_color_hex(0xC084FC); }
static lv_color_t home_cc_color_yellow(void)        { return lv_color_hex(0xFBBF24); }
static lv_color_t home_cc_color_teal(void)          { return lv_color_hex(0x0D9488); }
static lv_color_t home_cc_color_green(void)         { return lv_color_hex(0x34D399); }
static lv_color_t home_cc_color_red(void)           { return lv_color_hex(0xF87171); }
static lv_color_t home_cc_color_red_bg(void)        { return lv_color_hex(0xEF4444); }
static lv_opa_t home_cc_pct_opa(uint8_t pct)        { return (lv_opa_t)(LV_OPA_COVER * pct / 100); }

/* 初始化或重置控制中心上下文，保证重复创建时状态一致。 */
static void home_cc_reset_context(void)
{
    g_home_cc = (home_cc_context_t){ 0 };
    g_home_cc.drag_panel_start_y = HOME_CC_CLOSED_Y;
    g_home_cc.brightness.value = 100;
    g_home_cc.volume.value = 100;
    g_home_cc.volume.is_volume = true;
}

/* 递归关闭事件冒泡，避免内部点击传递到外层页面。 */
static void home_cc_clear_event_bubble_subtree(lv_obj_t * root)
{
    if(!root) return;

    lv_obj_remove_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    uint32_t child_count = lv_obj_get_child_count(root);
    for(uint32_t i = 0; i < child_count; ++i) {
        home_cc_clear_event_bubble_subtree(lv_obj_get_child(root, i));
    }
}

/* 创建不带默认样式的基础容器。 */
static lv_obj_t * home_cc_create_plain_obj(lv_obj_t * parent, int32_t width, int32_t height)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* 创建统一文本样式的标签对象。 */
static lv_obj_t * home_cc_create_label(lv_obj_t * parent,
                                       const char * text,
                                       const lv_font_t * font,
                                       lv_color_t color,
                                       lv_opa_t opa)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    ui_make_decor_hit_passthrough(label);
    return label;
}

/* 创建图标图片，并默认设置为装饰性命中透传。 */
static lv_obj_t * home_cc_create_image_icon(lv_obj_t * parent, const lv_image_dsc_t * src)
{
    lv_obj_t * img = lv_image_create(parent);
    lv_image_set_src(img, src);
    ui_make_decor_hit_passthrough(img);
    return img;
}

/* 统一设置圆角按钮的基础样式。 */
static void home_cc_style_round_button(lv_obj_t * obj,
                                       int32_t radius,
                                       lv_color_t bg,
                                       lv_opa_t bg_opa,
                                       lv_color_t border,
                                       lv_opa_t border_opa)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, bg_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_opa(obj, border_opa, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* 统一设置文本透明度，便于状态刷新复用。 */
static void home_cc_set_text_opa(lv_obj_t * obj, lv_opa_t opa)
{
    if(obj) {
        lv_obj_set_style_text_opa(obj, opa, 0);
    }
}

/* 隐藏重启确认弹层。 */
static void home_cc_hide_restart_confirm(void)
{
    if(g_home_cc.restart_confirm.overlay) {
        lv_obj_add_flag(g_home_cc.restart_confirm.overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 显示重启确认弹层，并提升到面板顶层。 */
static void home_cc_show_restart_confirm(void)
{
    if(!g_home_cc.restart_confirm.overlay) return;

    lv_obj_clear_flag(g_home_cc.restart_confirm.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_home_cc.restart_confirm.overlay);
}

/* 设置滑条轨道样式。 */
static void home_cc_style_slider_track(lv_obj_t * track)
{
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, HOME_CC_SLIDER_W, HOME_CC_SLIDER_H);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(track, home_cc_pct_opa(12), 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_outline_width(track, 0, 0);
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
}

/* 设置滑条填充区域样式。 */
static void home_cc_style_slider_fill(lv_obj_t * fill, lv_color_t color)
{
    lv_obj_remove_style_all(fill);
    lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fill, color, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* 设置滑条端点样式。 */
static void home_cc_style_slider_cap(lv_obj_t * cap)
{
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, HOME_CC_SLIDER_CAP_SIZE, HOME_CC_SLIDER_CAP_SIZE);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* 根据当前数值刷新滑条填充长度与端点位置。 */
static void home_cc_update_slider_visual(home_cc_slider_t * slider)
{
    if(!slider || !slider->track || !slider->fill || !slider->cap) return;

    int32_t slider_w = lv_obj_get_width(slider->track);
    int32_t slider_h = lv_obj_get_height(slider->track);
    int32_t edge_x = ((slider_w * slider->value) + 50) / 100;
    int32_t fill_w = edge_x;
    int32_t cap_x = edge_x - HOME_CC_SLIDER_CAP_SIZE;
    int32_t cap_y = (slider_h - HOME_CC_SLIDER_CAP_SIZE) / 2;

    if(fill_w < 0) fill_w = 0;
    if(fill_w > slider_w) fill_w = slider_w;
    if(cap_x < 0) cap_x = 0;
    if(cap_x > slider_w - HOME_CC_SLIDER_CAP_SIZE) {
        cap_x = slider_w - HOME_CC_SLIDER_CAP_SIZE;
    }

    lv_obj_set_pos(slider->fill, 0, 0);
    lv_obj_set_size(slider->fill, fill_w, slider_h);
    lv_obj_set_pos(slider->cap, cap_x, cap_y);
}

/* 刷新滑条右侧的数值文本。 */
static void home_cc_update_slider_label(home_cc_slider_t * slider)
{
    if(slider && slider->value_label) {
        lv_label_set_text_fmt(slider->value_label, "%u", (unsigned int)slider->value);
    }
}

/* 设置亮度或音量值，并按需同步到设备侧。 */
static void home_cc_set_slider_value(home_cc_slider_t * slider,
                                     uint8_t value,
                                     bool apply_device,
                                     bool permanent)
{
    if(!slider) return;

    if(value > 100) {
        value = 100;
    }
    if(!slider->is_volume && value == 0) {
        value = 1;
    }

    if(apply_device) {
        if(slider->is_volume) {
            app_device_set_volume(value, permanent);
        } else {
            app_device_set_brightness(value, permanent);
        }
    }

    slider->value = value;
    home_cc_update_slider_label(slider);
    home_cc_update_slider_visual(slider);
}

/* 根据当前网络模式刷新两个切换按钮的视觉状态。 */
static void home_cc_update_network_style(void)
{
    home_cc_network_option_t * wifi = &g_home_cc.network[HOME_CC_NETWORK_WIFI];
    home_cc_network_option_t * four_g = &g_home_cc.network[HOME_CC_NETWORK_4G];
    bool wifi_active = (g_home_cc.network_mode == HOME_CC_NETWORK_WIFI);
    lv_opa_t inactive_text_opa = home_cc_pct_opa(35);
    lv_opa_t inactive_icon_opa = home_cc_pct_opa(45);

    if(!wifi->btn || !four_g->btn) return;

    lv_obj_set_style_bg_color(wifi->btn, home_cc_color_purple(), 0);
    lv_obj_set_style_bg_opa(wifi->btn, wifi_active ? LV_OPA_COVER : home_cc_pct_opa(7), 0);
    lv_obj_set_style_border_color(wifi->btn, wifi_active ? home_cc_color_purple_border() : home_cc_color_white(), 0);
    lv_obj_set_style_border_opa(wifi->btn, wifi_active ? LV_OPA_COVER : home_cc_pct_opa(10), 0);
    lv_obj_set_style_shadow_width(wifi->btn, wifi_active ? 16 : 0, 0);
    lv_obj_set_style_shadow_color(wifi->btn, home_cc_color_purple(), 0);
    lv_obj_set_style_shadow_opa(wifi->btn, home_cc_pct_opa(50), 0);
    lv_obj_set_style_text_color(wifi->icon, home_cc_color_white(), 0);
    lv_obj_set_style_text_color(wifi->text, home_cc_color_white(), 0);
    home_cc_set_text_opa(wifi->icon, wifi_active ? LV_OPA_COVER : inactive_icon_opa);
    home_cc_set_text_opa(wifi->text, wifi_active ? LV_OPA_COVER : inactive_text_opa);

    lv_obj_set_style_bg_color(four_g->btn, home_cc_color_purple(), 0);
    lv_obj_set_style_bg_opa(four_g->btn, wifi_active ? home_cc_pct_opa(7) : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(four_g->btn, wifi_active ? home_cc_color_white() : home_cc_color_purple_border(), 0);
    lv_obj_set_style_border_opa(four_g->btn, wifi_active ? home_cc_pct_opa(10) : LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(four_g->btn, wifi_active ? 0 : 16, 0);
    lv_obj_set_style_shadow_color(four_g->btn, home_cc_color_purple(), 0);
    lv_obj_set_style_shadow_opa(four_g->btn, home_cc_pct_opa(50), 0);
    lv_obj_set_style_text_color(four_g->icon, home_cc_color_white(), 0);
    lv_obj_set_style_text_color(four_g->text, home_cc_color_white(), 0);
    home_cc_set_text_opa(four_g->icon, wifi_active ? inactive_icon_opa : LV_OPA_COVER);
    home_cc_set_text_opa(four_g->text, wifi_active ? inactive_text_opa : LV_OPA_COVER);
}

/* 根据当前状态刷新自动省电按钮与开关视觉。 */
static void home_cc_update_auto_power_style(void)
{
    if(!g_home_cc.auto_power_btn || !g_home_cc.auto_power_switch || !g_home_cc.auto_power_knob) return;

    lv_color_t active_fg = home_cc_color_green();
    lv_color_t inactive_fg = home_cc_color_white();

    lv_obj_set_style_bg_color(g_home_cc.auto_power_btn,
                              g_home_cc.auto_power_enabled ? home_cc_color_teal() : home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.auto_power_btn,
                            g_home_cc.auto_power_enabled ? home_cc_pct_opa(22) : home_cc_pct_opa(7), 0);
    lv_obj_set_style_border_color(g_home_cc.auto_power_btn,
                                  g_home_cc.auto_power_enabled ? active_fg : inactive_fg, 0);
    lv_obj_set_style_border_opa(g_home_cc.auto_power_btn,
                                g_home_cc.auto_power_enabled ? home_cc_pct_opa(45) : home_cc_pct_opa(12), 0);

    lv_obj_set_style_text_color(g_home_cc.auto_power_icon,
                                g_home_cc.auto_power_enabled ? active_fg : inactive_fg, 0);
    lv_obj_set_style_text_color(g_home_cc.auto_power_text,
                                g_home_cc.auto_power_enabled ? active_fg : inactive_fg, 0);
    home_cc_set_text_opa(g_home_cc.auto_power_icon,
                         g_home_cc.auto_power_enabled ? LV_OPA_COVER : home_cc_pct_opa(45));
    home_cc_set_text_opa(g_home_cc.auto_power_text,
                         g_home_cc.auto_power_enabled ? LV_OPA_COVER : home_cc_pct_opa(50));

    lv_obj_set_style_bg_color(g_home_cc.auto_power_switch,
                              g_home_cc.auto_power_enabled ? home_cc_color_teal() : home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.auto_power_switch,
                            g_home_cc.auto_power_enabled ? LV_OPA_COVER : home_cc_pct_opa(12), 0);
    lv_obj_set_style_border_color(g_home_cc.auto_power_switch,
                                  g_home_cc.auto_power_enabled ? active_fg : home_cc_color_white(), 0);
    lv_obj_set_style_border_opa(g_home_cc.auto_power_switch,
                                g_home_cc.auto_power_enabled ? home_cc_pct_opa(50) : home_cc_pct_opa(15), 0);
    lv_obj_set_x(g_home_cc.auto_power_knob, g_home_cc.auto_power_enabled ? 14 : 2);
}

/* 从设备读取当前设置，并同步刷新控制中心显示。 */
static void home_cc_sync_from_device(void)
{
    g_home_cc.network_mode = app_device_get_network_mode_is_4g() ? HOME_CC_NETWORK_4G : HOME_CC_NETWORK_WIFI;
    g_home_cc.auto_power_enabled = app_device_get_auto_power_save_enabled();
    home_cc_set_slider_value(&g_home_cc.brightness, app_device_get_brightness(), false, false);
    home_cc_set_slider_value(&g_home_cc.volume, app_device_get_volume(), false, false);
    home_cc_update_network_style();
    home_cc_update_auto_power_style();
}

/* 限制面板 Y 坐标，避免拖出可视范围。 */
static int32_t home_cc_clamp_panel_y(int32_t y)
{
    if(y < HOME_CC_CLOSED_Y) y = HOME_CC_CLOSED_Y;
    if(y > HOME_CC_OPEN_Y) y = HOME_CC_OPEN_Y;
    return y;
}

bool ui_ControlCenter_is_visible(void)
{
    return g_home_cc.panel &&
           (g_home_cc.panel_open || g_home_cc.panel_dragging || g_home_cc.panel_animating ||
            lv_obj_get_y(g_home_cc.panel) != HOME_CC_CLOSED_Y);
}

/* 显示遮罩与底部收起热区，并调整到前景层级。 */
static void home_cc_prepare_layers(void)
{
    if(!g_home_cc.panel || !g_home_cc.scrim || !g_home_cc.close_zone) return;

    lv_obj_clear_flag(g_home_cc.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_home_cc.close_zone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_home_cc.scrim);
    lv_obj_move_foreground(g_home_cc.panel);
    lv_obj_move_foreground(g_home_cc.close_zone);
}

/* 判断触点是否落在顶部下拉热区内。 */
static bool home_cc_point_in_top_edge(const lv_point_t * p)
{
    return p && p->y >= 0 && p->y < HOME_CC_PULL_ZONE_H;
}

/* 判断触点是否落在底部收起热区内。 */
static bool home_cc_point_in_bottom_edge(const lv_point_t * p)
{
    return p && p->y >= (LV_VER_RES - HOME_CC_CLOSE_ZONE_H) && p->y < LV_VER_RES;
}

/* 根据当前显示状态判断是否允许开始拖拽控制中心。 */
static bool home_cc_should_begin_panel_drag(lv_obj_t * target, const lv_point_t * p)
{
    if(!target) return false;

    if(!ui_ControlCenter_is_visible()) {
        return target == g_home_cc.pull_zone && home_cc_point_in_top_edge(p);
    }

    if(target == g_home_cc.close_zone && home_cc_point_in_bottom_edge(p)) return true;
    if(target == g_home_cc.panel && home_cc_point_in_bottom_edge(p)) return true;
    return false;
}

/* 拖拽时切换面板阴影，预留后续扩展空间。 */
static void home_cc_set_panel_shadow(bool dragging)
{
    if(!g_home_cc.panel) return;

    lv_obj_set_style_shadow_width(g_home_cc.panel,
                                  dragging ? HOME_CC_PANEL_SHADOW_DRAG : HOME_CC_PANEL_SHADOW_IDLE, 0);
}

/* 根据面板展开进度刷新背景遮罩透明度。 */
static void home_cc_update_scrim_opa(int32_t panel_y)
{
    if(!g_home_cc.scrim) return;

    int32_t progress = panel_y - HOME_CC_CLOSED_Y;
    if(progress < 0) progress = 0;
    if(progress > HOME_CC_PANEL_H) progress = HOME_CC_PANEL_H;

    lv_opa_t opa = (lv_opa_t)((progress * (int32_t)home_cc_pct_opa(24)) / HOME_CC_PANEL_H);
    if(opa == g_home_cc.scrim_opa) return;

    g_home_cc.scrim_opa = opa;
    lv_obj_set_style_bg_opa(g_home_cc.scrim, opa, 0);
}

/* 设置面板位置，并同步更新遮罩透明度。 */
static void home_cc_set_panel_y(int32_t y)
{
    if(!g_home_cc.panel) return;

    y = home_cc_clamp_panel_y(y);
    if(lv_obj_get_y(g_home_cc.panel) != y) {
        lv_obj_set_y(g_home_cc.panel, y);
    }
    home_cc_update_scrim_opa(y);
}

/* 面板位移动画回调，统一复用位置更新逻辑。 */
static void home_cc_anim_set_panel_y(void * var, int32_t value)
{
    (void)var;
    home_cc_set_panel_y(value);
}

/* 停止面板位移动画，避免与拖拽逻辑互相干扰。 */
static void home_cc_stop_panel_anim(void)
{
    if(!g_home_cc.panel) return;

    lv_anim_del(g_home_cc.panel, (lv_anim_exec_xcb_t)home_cc_anim_set_panel_y);
    g_home_cc.panel_animating = false;
}

/* 收尾面板状态，并恢复页面返回手势开关。 */
static void home_cc_finish_panel_state(bool open)
{
    if(!g_home_cc.panel || !g_home_cc.scrim || !g_home_cc.close_zone) return;

    g_home_cc.panel_dragging = false;
    g_home_cc.panel_animating = false;
    g_home_cc.panel_open = open;
    g_home_cc.drag_last_delta_y = 0;
    g_home_cc.drag_last_step_y = 0;
    g_home_cc.drag_started_open = false;

    app_screen_set_swipe_back_enabled(g_home_cc.screen, !open);
    home_cc_set_panel_shadow(false);
    home_cc_set_panel_y(open ? HOME_CC_OPEN_Y : HOME_CC_CLOSED_Y);

    if(open) {
        home_cc_prepare_layers();
        return;
    }

    lv_obj_add_flag(g_home_cc.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_home_cc.close_zone, LV_OBJ_FLAG_HIDDEN);
    home_cc_hide_restart_confirm();
}

/* 释放时优先采用输入设备速度，让吸附判定更自然。 */
static int32_t home_cc_get_release_step_y(lv_indev_t * indev)
{
    int32_t release_step_y = g_home_cc.drag_last_step_y;

    if(indev) {
        lv_point_t vect = { 0, 0 };
        lv_indev_get_vect(indev, &vect);
        if(LV_ABS(vect.y) > LV_ABS(release_step_y)) {
            release_step_y = vect.y;
        }
    }

    return release_step_y;
}

/* 根据拖拽位移与释放速度决定最终展开还是收起。 */
static bool home_cc_should_snap_open(int32_t panel_y, int32_t drag_delta_y, int32_t release_step_y)
{
    int32_t open_progress = panel_y - HOME_CC_CLOSED_Y;

    if(release_step_y >= HOME_CC_FLING_OPEN_VECT_Y) return true;
    if(release_step_y <= -HOME_CC_FLING_CLOSE_VECT_Y) return false;

    if(g_home_cc.drag_started_open) {
        if(drag_delta_y <= -HOME_CC_CLOSE_DRAG_THR) return false;
        if(drag_delta_y >= HOME_CC_OPEN_DRAG_THR / 2) return true;
        return open_progress >= (HOME_CC_PANEL_H * 2 / 3);
    }

    if(drag_delta_y >= HOME_CC_OPEN_DRAG_THR) return true;
    if(drag_delta_y <= -(HOME_CC_CLOSE_DRAG_THR / 2)) return false;
    return open_progress >= (HOME_CC_PANEL_H * 2 / 5);
}

/* 直接切换面板显隐状态，展开前先同步设备最新值。 */
static void home_cc_show_panel(bool open)
{
    if(!g_home_cc.panel || !g_home_cc.scrim || !g_home_cc.close_zone) return;

    home_cc_stop_panel_anim();

    if(open) {
        home_cc_sync_from_device();
        home_cc_prepare_layers();
    } else {
        home_cc_hide_restart_confirm();
    }

    home_cc_finish_panel_state(open);
}

/* 处理 WiFi 与 4G 切换事件。 */
static void home_cc_network_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    uint8_t target_mode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(target_mode == g_home_cc.network_mode) return;

    if(!app_device_switch_network_mode(target_mode == HOME_CC_NETWORK_4G)) {
        home_cc_sync_from_device();
        return;
    }

    g_home_cc.network_mode = target_mode;
    home_cc_update_network_style();
}

/* 处理亮度和音量滑条事件。 */
static void home_cc_slider_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();
    home_cc_slider_t * slider = (home_cc_slider_t *)lv_event_get_user_data(e);

    if(!slider) return;

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(slider->dragging) {
            home_cc_set_slider_value(slider, slider->value, true, true);
        }
        slider->dragging = false;
        return;
    }

    if(code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    if(!indev) return;

    lv_point_t point;
    lv_area_t coords;
    lv_indev_get_point(indev, &point);

    if(code == LV_EVENT_PRESSED) {
        slider->dragging = true;
        slider->last_touch_x = point.x;
    } else {
        if(!slider->dragging) return;
        if(LV_ABS(point.x - slider->last_touch_x) > HOME_CC_SLIDER_JUMP_THR) return;
        slider->last_touch_x = point.x;
    }

    lv_obj_get_coords(slider->track, &coords);
    int32_t rel_x = point.x - coords.x1 - HOME_CC_SLIDER_INSET;
    int32_t value = (rel_x * 100) / (HOME_CC_SLIDER_W - HOME_CC_SLIDER_INSET * 2);

    if(value < 0) value = 0;
    if(value > 100) value = 100;

    home_cc_set_slider_value(slider, (uint8_t)value, true, false);
}

/* 处理自动省电开关事件。 */
static void home_cc_auto_power_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    (void)app_device_set_auto_power_save_enabled(!g_home_cc.auto_power_enabled);
    home_cc_sync_from_device();
}

/* 点击重启按钮后显示确认弹层。 */
static void home_cc_restart_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    home_cc_show_restart_confirm();
}

/* 点击确认弹层空白区域时关闭弹层。 */
static void home_cc_restart_confirm_overlay_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(lv_event_get_target_obj(e) != g_home_cc.restart_confirm.overlay) return;

    home_cc_hide_restart_confirm();
}

/* 处理重启确认弹层的取消按钮事件。 */
static void home_cc_restart_confirm_cancel_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    home_cc_hide_restart_confirm();
}

/* 处理重启确认弹层的确认按钮事件。 */
static void home_cc_restart_confirm_accept_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    home_cc_hide_restart_confirm();
    home_cc_show_panel(false);
    app_device_reboot();
}

/* 点击背景遮罩时直接关闭控制中心。 */
static void home_cc_scrim_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(lv_event_get_target_obj(e) != g_home_cc.scrim) return;

    home_cc_show_panel(false);
}

/* 初始化一次面板拖拽会话。 */
static bool home_cc_begin_panel_drag(lv_obj_t * target, const lv_point_t * point)
{
    if(!home_cc_should_begin_panel_drag(target, point) || !g_home_cc.panel) return false;

    app_screen_set_swipe_back_enabled(g_home_cc.screen, false);
    home_cc_stop_panel_anim();

    g_home_cc.panel_dragging = true;
    g_home_cc.panel_open = false;
    g_home_cc.drag_start_y = point->y;
    g_home_cc.drag_panel_start_y = lv_obj_get_y(g_home_cc.panel);
    g_home_cc.drag_last_delta_y = 0;
    g_home_cc.drag_last_point_y = point->y;
    g_home_cc.drag_last_step_y = 0;
    g_home_cc.drag_started_open = (g_home_cc.drag_panel_start_y == HOME_CC_OPEN_Y);

    home_cc_set_panel_shadow(true);
    home_cc_prepare_layers();
    return true;
}

/* 处理控制中心拖拽，释放时再根据位移和速度决定最终状态。 */
static void home_cc_panel_drag_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_event_get_indev(e);
    lv_obj_t * target = lv_event_get_target_obj(e);

    if(!indev) {
        indev = lv_indev_get_act();
    }
    if(!g_home_cc.panel) return;

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(!g_home_cc.panel_dragging) return;

        g_home_cc.panel_dragging = false;

        int32_t panel_y = home_cc_clamp_panel_y(g_home_cc.drag_panel_start_y + g_home_cc.drag_last_delta_y);
        int32_t release_step_y = home_cc_get_release_step_y(indev);

        if(home_cc_should_snap_open(panel_y, g_home_cc.drag_last_delta_y, release_step_y)) {
            home_cc_show_panel(true);
        } else {
            home_cc_show_panel(false);
        }
        return;
    }

    if(!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if(code == LV_EVENT_PRESSED) {
        (void)home_cc_begin_panel_drag(target, &point);
        return;
    }

    if(code == LV_EVENT_PRESSING) {
        if(!g_home_cc.panel_dragging && !home_cc_begin_panel_drag(target, &point)) {
            return;
        }

        lv_point_t vect = { 0, 0 };
        int32_t dy = point.y - g_home_cc.drag_start_y;

        g_home_cc.drag_last_step_y = point.y - g_home_cc.drag_last_point_y;
        g_home_cc.drag_last_point_y = point.y;
        g_home_cc.drag_last_delta_y = dy;

        lv_indev_get_vect(indev, &vect);

        if(g_home_cc.drag_started_open &&
           dy <= -HOME_CC_FAST_CLOSE_TRAVEL_Y &&
           (g_home_cc.drag_last_step_y <= -HOME_CC_FAST_CLOSE_STEP_Y ||
            vect.y <= -HOME_CC_FAST_CLOSE_VECT_Y)) {
            home_cc_show_panel(false);
        }
    }
}

/* 为指定对象统一挂载面板拖拽相关事件。 */
static void home_cc_attach_panel_drag_events(lv_obj_t * obj)
{
    if(!obj) return;

    lv_obj_add_event_cb(obj, home_cc_panel_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, home_cc_panel_drag_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, home_cc_panel_drag_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, home_cc_panel_drag_event_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* 创建滑条右侧的数值标签。 */
static lv_obj_t * home_cc_create_value_label(lv_obj_t * parent)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_set_size(label, 34, LV_SIZE_CONTENT);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(label, "100");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, home_cc_color_white(), 0);
    lv_obj_set_style_text_opa(label, home_cc_pct_opa(70), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    ui_make_decor_hit_passthrough(label);
    return label;
}

/* 创建单个网络模式切换按钮。 */
static void home_cc_build_network_option(lv_obj_t * parent,
                                         home_cc_network_option_t * option,
                                         const char * icon_text,
                                         const char * label_text,
                                         uint8_t mode)
{
    if(!parent || !option) return;

    option->btn = lv_btn_create(parent);
    home_cc_style_round_button(option->btn,
                               16,
                               home_cc_color_purple(),
                               mode == HOME_CC_NETWORK_WIFI ? LV_OPA_COVER : home_cc_pct_opa(7),
                               mode == HOME_CC_NETWORK_WIFI ? home_cc_color_purple_border() : home_cc_color_white(),
                               mode == HOME_CC_NETWORK_WIFI ? LV_OPA_COVER : home_cc_pct_opa(10));
    lv_obj_set_height(option->btn, 36);
    lv_obj_set_style_pad_left(option->btn, 14, 0);
    lv_obj_set_style_pad_right(option->btn, 16, 0);
    lv_obj_set_style_pad_gap(option->btn, 6, 0);
    lv_obj_set_flex_flow(option->btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(option->btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(option->btn, home_cc_network_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)mode);

    option->icon = home_cc_create_label(option->btn, icon_text, &font_awesome_14_1, home_cc_color_white(),
                                        mode == HOME_CC_NETWORK_WIFI ? LV_OPA_COVER : home_cc_pct_opa(45));
    option->text = home_cc_create_label(option->btn, label_text, &lv_font_montserrat_14, home_cc_color_white(),
                                        mode == HOME_CC_NETWORK_WIFI ? LV_OPA_COVER : home_cc_pct_opa(35));

    if(mode == HOME_CC_NETWORK_4G) {
        lv_obj_set_style_translate_y(option->icon, -1, 0);
    }
}

/* 构建网络模式切换区域。 */
static void home_cc_build_network_row(lv_obj_t * parent)
{
    lv_obj_t * row = home_cc_create_plain_obj(parent, 290, 36);
    lv_obj_set_pos(row, 35, 68);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    home_cc_build_network_option(row, &g_home_cc.network[HOME_CC_NETWORK_WIFI], FONT_AWESOME_WIFI, "WiFi", HOME_CC_NETWORK_WIFI);
    home_cc_build_network_option(row, &g_home_cc.network[HOME_CC_NETWORK_4G], FONT_AWESOME_SIGNAL_STRONG, "4G", HOME_CC_NETWORK_4G);
}

/* 构建亮度或音量滑条行。 */
static void home_cc_build_slider_row(lv_obj_t * parent,
                                     int32_t pos_y,
                                     home_cc_slider_t * slider,
                                     const lv_image_dsc_t * icon_src,
                                     lv_color_t icon_color,
                                     lv_color_t fill_color)
{
    if(!parent || !slider) return;

    lv_obj_t * row = home_cc_create_plain_obj(parent, 330, 40);
    lv_obj_set_pos(row, 15, pos_y);

    lv_obj_t * icon_box = home_cc_create_plain_obj(row, 26, 40);
    lv_obj_t * icon = home_cc_create_image_icon(icon_box, icon_src);
    lv_obj_set_style_image_recolor(icon, icon_color, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);

    slider->track = lv_obj_create(row);
    home_cc_style_slider_track(slider->track);
    lv_obj_set_pos(slider->track, 38, 4);

    slider->fill = lv_obj_create(slider->track);
    home_cc_style_slider_fill(slider->fill, fill_color);

    slider->cap = lv_obj_create(slider->track);
    home_cc_style_slider_cap(slider->cap);

    lv_obj_add_event_cb(slider->track, home_cc_slider_event_cb, LV_EVENT_PRESSED, slider);
    lv_obj_add_event_cb(slider->track, home_cc_slider_event_cb, LV_EVENT_PRESSING, slider);
    lv_obj_add_event_cb(slider->track, home_cc_slider_event_cb, LV_EVENT_RELEASED, slider);
    lv_obj_add_event_cb(slider->track, home_cc_slider_event_cb, LV_EVENT_PRESS_LOST, slider);

    slider->value_label = home_cc_create_value_label(row);
}

/* 构建自动省电与重启操作区域。 */
static void home_cc_build_power_row(lv_obj_t * parent)
{
    lv_obj_t * row = home_cc_create_plain_obj(parent, 330, 42);
    lv_obj_set_pos(row, 15, 224);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_home_cc.auto_power_btn = lv_btn_create(row);
    home_cc_style_round_button(g_home_cc.auto_power_btn, 12, home_cc_color_white(),
                               home_cc_pct_opa(7), home_cc_color_white(), home_cc_pct_opa(12));
    lv_obj_set_size(g_home_cc.auto_power_btn, 154, 42);
    lv_obj_set_style_pad_left(g_home_cc.auto_power_btn, 18, 0);
    lv_obj_set_style_pad_right(g_home_cc.auto_power_btn, 18, 0);
    lv_obj_set_style_pad_gap(g_home_cc.auto_power_btn, 6, 0);
    lv_obj_set_flex_flow(g_home_cc.auto_power_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_home_cc.auto_power_btn,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(g_home_cc.auto_power_btn, home_cc_auto_power_event_cb, LV_EVENT_CLICKED, NULL);

    g_home_cc.auto_power_icon = home_cc_create_label(g_home_cc.auto_power_btn,
                                                     FONT_AWESOME_BATTERY_QUARTER,
                                                     &font_awesome_14_1,
                                                     home_cc_color_white(),
                                                     home_cc_pct_opa(45));
    g_home_cc.auto_power_text = home_cc_create_label(g_home_cc.auto_power_btn,
                                                     "自动省电",
                                                     ui_builtin_text_font(),
                                                     home_cc_color_white(),
                                                     home_cc_pct_opa(50));

    g_home_cc.auto_power_switch = home_cc_create_plain_obj(g_home_cc.auto_power_btn, 34, 20);
    lv_obj_set_style_radius(g_home_cc.auto_power_switch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_home_cc.auto_power_switch, home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.auto_power_switch, home_cc_pct_opa(12), 0);
    lv_obj_set_style_border_width(g_home_cc.auto_power_switch, 1, 0);
    lv_obj_set_style_border_color(g_home_cc.auto_power_switch, home_cc_color_white(), 0);
    lv_obj_set_style_border_opa(g_home_cc.auto_power_switch, home_cc_pct_opa(15), 0);
    lv_obj_clear_flag(g_home_cc.auto_power_switch, LV_OBJ_FLAG_CLICKABLE);

    g_home_cc.auto_power_knob = home_cc_create_plain_obj(g_home_cc.auto_power_switch, 14, 14);
    lv_obj_set_style_radius(g_home_cc.auto_power_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_home_cc.auto_power_knob, home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.auto_power_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(g_home_cc.auto_power_knob, 4, 0);
    lv_obj_set_style_shadow_color(g_home_cc.auto_power_knob, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(g_home_cc.auto_power_knob, home_cc_pct_opa(30), 0);
    lv_obj_set_pos(g_home_cc.auto_power_knob, 2, 3);
    lv_obj_clear_flag(g_home_cc.auto_power_knob, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * restart_btn = lv_btn_create(row);
    home_cc_style_round_button(restart_btn,
                               12,
                               home_cc_color_red_bg(),
                               home_cc_pct_opa(14),
                               home_cc_color_red_bg(),
                               home_cc_pct_opa(30));
    lv_obj_set_size(restart_btn, 154, 42);
    lv_obj_set_style_shadow_color(restart_btn, home_cc_color_red_bg(), 0);
    lv_obj_set_style_shadow_width(restart_btn, 10, 0);
    lv_obj_set_style_shadow_opa(restart_btn, home_cc_pct_opa(20), 0);
    lv_obj_set_style_pad_gap(restart_btn, 6, 0);
    lv_obj_set_flex_flow(restart_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(restart_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(restart_btn, home_cc_restart_event_cb, LV_EVENT_CLICKED, NULL);

    (void)home_cc_create_label(restart_btn, FONT_AWESOME_POWER_OFF, &font_awesome_14_1,
                               home_cc_color_red(), LV_OPA_COVER);
    lv_obj_t * restart_text = home_cc_create_label(restart_btn, "重启", ui_builtin_text_font(),
                                                   home_cc_color_red(), LV_OPA_COVER);
    lv_obj_set_style_text_letter_space(restart_text, 1, 0);
}

/* 构建重启确认弹层及其按钮。 */
static void home_cc_build_restart_confirm(void)
{
    g_home_cc.restart_confirm.overlay = home_cc_create_plain_obj(g_home_cc.panel, HOME_CC_PANEL_W, HOME_CC_PANEL_H);
    lv_obj_set_pos(g_home_cc.restart_confirm.overlay, 0, 0);
    lv_obj_set_style_bg_color(g_home_cc.restart_confirm.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.restart_confirm.overlay, home_cc_pct_opa(55), 0);
    lv_obj_add_flag(g_home_cc.restart_confirm.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_home_cc.restart_confirm.overlay,
                        home_cc_restart_confirm_overlay_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);

    g_home_cc.restart_confirm.card = home_cc_create_plain_obj(g_home_cc.restart_confirm.overlay, 220, 132);
    lv_obj_center(g_home_cc.restart_confirm.card);
    lv_obj_set_style_radius(g_home_cc.restart_confirm.card, 18, 0);
    lv_obj_set_style_bg_color(g_home_cc.restart_confirm.card, lv_color_hex(0x161625), 0);
    lv_obj_set_style_bg_opa(g_home_cc.restart_confirm.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_home_cc.restart_confirm.card, 1, 0);
    lv_obj_set_style_border_color(g_home_cc.restart_confirm.card, home_cc_color_white(), 0);
    lv_obj_set_style_border_opa(g_home_cc.restart_confirm.card, home_cc_pct_opa(12), 0);
    lv_obj_set_style_shadow_width(g_home_cc.restart_confirm.card, 18, 0);
    lv_obj_set_style_shadow_color(g_home_cc.restart_confirm.card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(g_home_cc.restart_confirm.card, home_cc_pct_opa(35), 0);

    lv_obj_t * title = home_cc_create_label(g_home_cc.restart_confirm.card, "确认重启",
                                            ui_builtin_text_font(), home_cc_color_white(), LV_OPA_COVER);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t * icon = home_cc_create_label(g_home_cc.restart_confirm.card, FONT_AWESOME_POWER_OFF,
                                           &font_awesome_14_1, home_cc_color_red(), LV_OPA_COVER);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 48);

    g_home_cc.restart_confirm.cancel_btn = lv_btn_create(g_home_cc.restart_confirm.card);
    home_cc_style_round_button(g_home_cc.restart_confirm.cancel_btn,
                               12,
                               home_cc_color_white(),
                               home_cc_pct_opa(8),
                               home_cc_color_white(),
                               home_cc_pct_opa(12));
    lv_obj_set_size(g_home_cc.restart_confirm.cancel_btn, 82, 34);
    lv_obj_align(g_home_cc.restart_confirm.cancel_btn, LV_ALIGN_BOTTOM_LEFT, 18, -16);
    lv_obj_add_event_cb(g_home_cc.restart_confirm.cancel_btn,
                        home_cc_restart_confirm_cancel_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t * cancel_text = home_cc_create_label(g_home_cc.restart_confirm.cancel_btn, "取消",
                                                  ui_builtin_text_font(), home_cc_color_white(), home_cc_pct_opa(70));
    lv_obj_center(cancel_text);

    g_home_cc.restart_confirm.confirm_btn = lv_btn_create(g_home_cc.restart_confirm.card);
    home_cc_style_round_button(g_home_cc.restart_confirm.confirm_btn,
                               12,
                               home_cc_color_red_bg(),
                               home_cc_pct_opa(18),
                               home_cc_color_red_bg(),
                               home_cc_pct_opa(30));
    lv_obj_set_size(g_home_cc.restart_confirm.confirm_btn, 82, 34);
    lv_obj_align(g_home_cc.restart_confirm.confirm_btn, LV_ALIGN_BOTTOM_RIGHT, -18, -16);
    lv_obj_add_event_cb(g_home_cc.restart_confirm.confirm_btn,
                        home_cc_restart_confirm_accept_event_cb,
                        LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t * confirm_text = home_cc_create_label(g_home_cc.restart_confirm.confirm_btn, "确认",
                                                   ui_builtin_text_font(), home_cc_color_red(), LV_OPA_COVER);
    lv_obj_center(confirm_text);
}

/* 构建控制中心主面板。 */
static void home_cc_build_panel(void)
{
    g_home_cc.panel = home_cc_create_plain_obj(g_home_cc.screen, HOME_CC_PANEL_W, HOME_CC_PANEL_H);
    lv_obj_set_pos(g_home_cc.panel, 0, HOME_CC_CLOSED_Y);
    lv_obj_set_style_radius(g_home_cc.panel, HOME_CC_PANEL_RADIUS, 0);
    lv_obj_set_style_bg_color(g_home_cc.panel, home_cc_color_bg(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_home_cc.panel, 0, 0);
    lv_obj_set_style_shadow_width(g_home_cc.panel, HOME_CC_PANEL_SHADOW_IDLE, 0);
    lv_obj_set_style_shadow_color(g_home_cc.panel, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(g_home_cc.panel, home_cc_pct_opa(16), 0);
    lv_obj_add_flag(g_home_cc.panel, LV_OBJ_FLAG_PRESS_LOCK);
    home_cc_attach_panel_drag_events(g_home_cc.panel);

    lv_obj_t * handle = home_cc_create_plain_obj(g_home_cc.panel, 44, 4);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, 5);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(handle, home_cc_color_white(), 0);
    lv_obj_set_style_bg_opa(handle, home_cc_pct_opa(20), 0);
    ui_make_decor_hit_passthrough(handle);

    lv_obj_t * title = home_cc_create_label(g_home_cc.panel, "控制中心",
                                            ui_builtin_text_font(), home_cc_color_white(), LV_OPA_COVER);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    home_cc_build_network_row(g_home_cc.panel);
    home_cc_build_slider_row(g_home_cc.panel, 120, &g_home_cc.brightness, &luminance,
                             home_cc_color_yellow(), home_cc_color_yellow());
    home_cc_build_slider_row(g_home_cc.panel, 172, &g_home_cc.volume, &volume,
                             home_cc_color_purple_border(), home_cc_color_purple_border());
    home_cc_build_power_row(g_home_cc.panel);
    home_cc_build_restart_confirm();
}

/* 构建背景遮罩层。 */
static void home_cc_build_scrim(void)
{
    g_home_cc.scrim = home_cc_create_plain_obj(g_home_cc.screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_home_cc.scrim, 0, 0);
    lv_obj_set_style_bg_color(g_home_cc.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_home_cc.scrim, LV_OPA_0, 0);
    lv_obj_add_flag(g_home_cc.scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_home_cc.scrim, home_cc_scrim_event_cb, LV_EVENT_CLICKED, NULL);
}

/* 构建底部收起热区。 */
static void home_cc_build_close_zone(void)
{
    g_home_cc.close_zone = home_cc_create_plain_obj(g_home_cc.screen, LV_HOR_RES, HOME_CC_CLOSE_ZONE_H);
    lv_obj_align(g_home_cc.close_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_home_cc.close_zone, LV_OPA_0, 0);
    lv_obj_add_flag(g_home_cc.close_zone, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_PRESS_LOCK);
    home_cc_attach_panel_drag_events(g_home_cc.close_zone);
}

/* 构建顶部下拉热区。 */
static void home_cc_build_pull_zone(void)
{
    g_home_cc.pull_zone = home_cc_create_plain_obj(g_home_cc.screen, LV_HOR_RES, HOME_CC_PULL_ZONE_H);
    lv_obj_align(g_home_cc.pull_zone, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_home_cc.pull_zone, LV_OPA_0, 0);
    lv_obj_add_flag(g_home_cc.pull_zone, LV_OBJ_FLAG_PRESS_LOCK);
    home_cc_attach_panel_drag_events(g_home_cc.pull_zone);
}

void ui_ControlCenter_init(lv_obj_t * screen)
{
    if(!screen || g_home_cc.panel) return;

    home_cc_reset_context();
    g_home_cc.screen = screen;

    home_cc_build_scrim();
    home_cc_build_panel();
    home_cc_build_close_zone();
    home_cc_build_pull_zone();

    home_cc_sync_from_device();
    home_cc_show_panel(false);
    home_cc_clear_event_bubble_subtree(g_home_cc.scrim);
    home_cc_clear_event_bubble_subtree(g_home_cc.panel);
}

void ui_ControlCenter_deinit(void)
{
    home_cc_stop_panel_anim();
    home_cc_reset_context();
}

bool ui_ControlCenter_dismiss_overlays(void)
{
    bool restart_confirm_visible = false;
    bool panel_visible = false;

    if(g_home_cc.restart_confirm.overlay) {
        restart_confirm_visible = !lv_obj_has_flag(g_home_cc.restart_confirm.overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_home_cc.panel) {
        panel_visible = ui_ControlCenter_is_visible();
    }

    if(restart_confirm_visible) {
        home_cc_hide_restart_confirm();
    }
    if(panel_visible) {
        g_home_cc.panel_dragging = false;
        home_cc_show_panel(false);
    }

    return restart_confirm_visible || panel_visible;
}
