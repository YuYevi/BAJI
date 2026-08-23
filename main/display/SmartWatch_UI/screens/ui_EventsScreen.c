/**
 * @file ui_EventsScreen.c
 * @brief 日历日期浏览屏幕实现
 *
 * 说明：
 *   - 顶部展示当前选中日期
 *   - 底部日期条支持点击切换、拖拽滚动和吸附回正
 *   - 保持右滑返回能力，并在拖动日期条时临时关闭
 */
#include "ui_EventsScreen.h"
#include "../ui.h"
#include "../app_common.h"
#include <time.h>
#include <stdlib.h>

lv_obj_t * ui_EventsScreen;

extern const lv_image_dsc_t * const event_day_digit_images[31];

#define EV_GREEN         0x10b981
#define EV_ACCENT        0x34d399
#define EV_BG            0x0a0e13
#define EV_CELL_W        44
#define EV_CELL_CNT      7
#define EV_CENTER        3
#define EV_DRAG_SNAP_MS  160
#define EV_TAP_SLOP_PX   12
#define EV_STRIP_H       80
#define EV_STRIP_Y       (-30)
#define EV_FADE_W         65
#define EV_HEADER_Y      40
#define EV_HEADER_H      220
#define EV_DATE_REFRESH_MS 60000

typedef struct {
    lv_obj_t * root;
    lv_obj_t * weekday;
    lv_obj_t * day;
    lv_obj_t * mark;
    lv_obj_t * dot;
} ev_cell_view_t;

typedef struct {
    lv_obj_t * content;
    lv_obj_t * month_year;
    lv_obj_t * day_big;
    lv_obj_t * weekday_row;
    lv_obj_t * weekday;
    lv_obj_t * today_badge;
    lv_obj_t * strip_wrap;
    lv_obj_t * strip;
    lv_obj_t * strip_touch;
    lv_timer_t * date_timer;
    ev_cell_view_t cells[EV_CELL_CNT];
    lv_grad_dsc_t fade_left;
    lv_grad_dsc_t fade_right;
} ev_view_t;

typedef struct {
    int32_t offset_days;
    bool strip_pressed;
    bool strip_dragged;
    lv_point_t press_point;
    int32_t drag_x;
    int32_t snap_delta;
    int32_t today_year;
    int32_t today_yday;
    bool today_valid;
} ev_state_t;

typedef struct {
    struct tm tm;
    bool is_today;
    bool is_weekend;
} ev_day_info_t;

static ev_view_t ev_view;
static ev_state_t ev_state;

static void ev_apply_strip_visuals(int32_t drag_x);
static void ev_refresh_view(bool play_anim);

static void ev_enable_swipe_back_async(void * user_data)
{
    lv_obj_t * screen = (lv_obj_t *)user_data;

    if(screen && screen == ui_EventsScreen) {
        app_screen_set_swipe_back_enabled(screen, true);
    }
}

static void ev_restore_swipe_back(void)
{
    if(!ui_EventsScreen) {
        return;
    }

    lv_async_call_cancel(ev_enable_swipe_back_async, ui_EventsScreen);
    (void)lv_async_call(ev_enable_swipe_back_async, ui_EventsScreen);
}

static void ev_set_translate_x(void * obj, int32_t value)
{
    if(!obj) {
        return;
    }

    lv_obj_set_style_translate_x((lv_obj_t *)obj, value, 0);
    if(obj == ev_view.strip) {
        ev_state.drag_x = value;
        ev_apply_strip_visuals(value);
    }
}

static void ev_set_translate_y(void * obj, int32_t value)
{
    if(!obj) {
        return;
    }

    lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

static void ev_set_opa(void * obj, int32_t value)
{
    if(!obj) {
        return;
    }

    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void ev_init_gradients(void)
{
    lv_color_t left_colors[] = { lv_color_hex(EV_BG), lv_color_hex(EV_BG) };
    lv_opa_t left_opas[] = { LV_OPA_COVER, LV_OPA_0 };
    uint8_t left_fracs[] = { 0, 255 };

    lv_grad_init_stops(&ev_view.fade_left, left_colors, left_opas, left_fracs, 2);
    lv_grad_linear_init(&ev_view.fade_left, 0, 0, EV_FADE_W, 0, LV_GRAD_EXTEND_PAD);

    lv_color_t right_colors[] = { lv_color_hex(EV_BG), lv_color_hex(EV_BG) };
    lv_opa_t right_opas[] = { LV_OPA_0, LV_OPA_COVER };
    uint8_t right_fracs[] = { 0, 255 };

    lv_grad_init_stops(&ev_view.fade_right, right_colors, right_opas, right_fracs, 2);
    lv_grad_linear_init(&ev_view.fade_right, 0, 0, EV_FADE_W, 0, LV_GRAD_EXTEND_PAD);
}

static void ev_reset_style(lv_obj_t * obj)
{
    lv_obj_remove_style_all(obj);
}

static void ev_disable_interaction(lv_obj_t * obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static bool ev_fill_tm(time_t ts, struct tm * out)
{
#if defined(_WIN32)
    struct tm buffer;

    if(localtime_s(&buffer, &ts) == 0) {
        *out = buffer;
        return true;
    } else {
        lv_memzero(out, sizeof(*out));
        return false;
    }
#else
    struct tm * t = localtime(&ts);

    if(t) {
        *out = *t;
        return true;
    } else {
        lv_memzero(out, sizeof(*out));
        return false;
    }
#endif
}

static bool ev_get_day_info(int32_t offset_days, ev_day_info_t * out)
{
    struct tm day;

    if(!out || !ev_fill_tm(time(NULL), &day)) {
        return false;
    }

    /* Normalize at noon so calendar-day navigation is stable across DST changes. */
    day.tm_hour = 12;
    day.tm_min = 0;
    day.tm_sec = 0;
    day.tm_mday += offset_days;
    day.tm_isdst = -1;
    if(mktime(&day) == (time_t)-1) {
        return false;
    }

    out->tm = day;
    out->is_today = (offset_days == 0);
    out->is_weekend = (out->tm.tm_wday == 0 || out->tm.tm_wday == 6);
    return true;
}

static void ev_start_int_anim(lv_obj_t * obj, lv_anim_exec_xcb_t exec_cb,
                              int32_t from, int32_t to, uint32_t duration,
                              lv_anim_path_cb_t path_cb, lv_anim_completed_cb_t done_cb)
{
    lv_anim_t anim;

    if(!obj || !exec_cb) {
        return;
    }

    lv_anim_del(obj, exec_cb);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_time(&anim, duration);
    if(path_cb) {
        lv_anim_set_path_cb(&anim, path_cb);
    }
    if(done_cb) {
        lv_anim_set_completed_cb(&anim, done_cb);
    }
    lv_anim_start(&anim);
}

static void ev_play_change_animations(void)
{
    /* 日期切换后，只对顶部信息做轻量动画，保证观感同时避免拖拽时抖动。 */
    if(!ev_view.day_big || !ev_view.month_year || !ev_view.weekday_row) {
        return;
    }

    lv_obj_set_style_translate_y(ev_view.day_big, -8, 0);
    lv_obj_set_style_opa(ev_view.day_big, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_set_style_opa(ev_view.month_year, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_set_style_opa(ev_view.weekday_row, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);

    ev_start_int_anim(ev_view.day_big, (lv_anim_exec_xcb_t)ev_set_translate_y,
                      -8, 0, 250, lv_anim_path_ease_out, NULL);
    ev_start_int_anim(ev_view.day_big, (lv_anim_exec_xcb_t)ev_set_opa,
                      (int32_t)(LV_OPA_COVER * 30 / 100), LV_OPA_COVER,
                      200, NULL, NULL);
    ev_start_int_anim(ev_view.month_year, (lv_anim_exec_xcb_t)ev_set_opa,
                      (int32_t)(LV_OPA_COVER * 30 / 100), LV_OPA_COVER,
                      180, NULL, NULL);
    ev_start_int_anim(ev_view.weekday_row, (lv_anim_exec_xcb_t)ev_set_opa,
                      (int32_t)(LV_OPA_COVER * 30 / 100), LV_OPA_COVER,
                      180, NULL, NULL);

    if(ev_view.today_badge && !lv_obj_has_flag(ev_view.today_badge, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_translate_y(ev_view.today_badge, 4, 0);
        lv_obj_set_style_opa(ev_view.today_badge, LV_OPA_0, 0);
        ev_start_int_anim(ev_view.today_badge, (lv_anim_exec_xcb_t)ev_set_translate_y,
                          4, 0, 180, lv_anim_path_ease_out, NULL);
        ev_start_int_anim(ev_view.today_badge, (lv_anim_exec_xcb_t)ev_set_opa,
                          LV_OPA_0, LV_OPA_COVER, 160, NULL, NULL);
    }
}

static void ev_update_header(const ev_day_info_t * selected)
{
    static const char * weekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    char month_year[32];
    int32_t day = selected->tm.tm_mday;
    lv_color_t day_color = selected->is_today ? lv_color_hex(EV_GREEN) : lv_color_white();

    lv_snprintf(month_year, sizeof(month_year), "%d年%d月",
                selected->tm.tm_year + 1900, selected->tm.tm_mon + 1);
    lv_label_set_text(ev_view.month_year, month_year);
    if(day >= 1 && day <= 31) {
        lv_image_set_src(ev_view.day_big, event_day_digit_images[day - 1]);
    }
    lv_label_set_text(ev_view.weekday, weekdays[selected->tm.tm_wday]);

    lv_obj_set_style_image_recolor(ev_view.day_big, day_color, 0);
    lv_obj_set_style_image_recolor_opa(ev_view.day_big, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ev_view.weekday,
                                selected->is_today ? lv_color_hex(EV_GREEN) : lv_color_hex(0x808080), 0);

    if(selected->is_today) {
        lv_obj_clear_flag(ev_view.today_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ev_view.today_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ev_update_cell(int32_t index, const ev_day_info_t * info)
{
    static const char * short_weekdays[] = { "日", "一", "二", "三", "四", "五", "六" };
    ev_cell_view_t * cell = &ev_view.cells[index];
    bool is_center = (index == EV_CENTER);
    bool show_month_mark = (info->tm.tm_mday == 1 && !is_center);
    int32_t distance = LV_ABS(index - EV_CENTER);
    int32_t opa_pct = is_center ? 100 : LV_MAX(12, 100 - distance * 15);
    char day_buf[4];

    lv_snprintf(day_buf, sizeof(day_buf), "%d", info->tm.tm_mday);

    if(show_month_mark) {
        char mark_buf[8];
        lv_snprintf(mark_buf, sizeof(mark_buf), "%d月", info->tm.tm_mon + 1);
        lv_label_set_text(cell->mark, mark_buf);
        lv_obj_set_style_text_opa(cell->mark, (lv_opa_t)(LV_OPA_COVER * opa_pct / 100), 0);
        lv_obj_clear_flag(cell->mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cell->weekday, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cell->mark, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cell->weekday, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(cell->weekday, short_weekdays[info->tm.tm_wday]);
    }

    if(is_center) {
        lv_obj_set_style_text_color(cell->weekday,
                                    info->is_today ? lv_color_hex(EV_GREEN) : lv_color_hex(0xb3b3b3), 0);
        lv_obj_set_style_text_opa(cell->weekday, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(cell->day, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(cell->day,
                                    info->is_today ? lv_color_hex(EV_GREEN) : lv_color_white(), 0);
        lv_obj_set_style_text_opa(cell->day, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(cell->weekday,
                                    info->is_weekend ? lv_color_hex(EV_ACCENT) : lv_color_hex(0x737373), 0);
        lv_obj_set_style_text_opa(cell->weekday,
                                  (lv_opa_t)(LV_OPA_COVER * opa_pct / 100), 0);
        lv_obj_set_style_text_font(cell->day, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cell->day,
                                    info->is_today ? lv_color_hex(EV_GREEN) :
                                    info->is_weekend ? lv_color_hex(EV_ACCENT) : lv_color_hex(0x8a8a8a), 0);
        lv_obj_set_style_text_opa(cell->day,
                                  (lv_opa_t)(LV_OPA_COVER * opa_pct / 100), 0);
    }

    lv_label_set_text(cell->day, day_buf);

    if(info->is_today && !is_center) {
        lv_obj_clear_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ev_refresh_cells(void)
{
    ev_day_info_t info;

    for(int32_t i = 0; i < EV_CELL_CNT; ++i) {
        if(ev_get_day_info(ev_state.offset_days + (i - EV_CENTER), &info)) {
            ev_update_cell(i, &info);
        }
    }
}

static void ev_refresh_view(bool play_anim)
{
    ev_day_info_t selected;

    /* 顶部主日期和底部日期条始终由同一份状态驱动，避免显示不同步。 */
    if(!ev_get_day_info(ev_state.offset_days, &selected)) {
        return;
    }
    ev_update_header(&selected);
    ev_refresh_cells();

    if(play_anim) {
        ev_play_change_animations();
    }
}

static void ev_shift_days(int32_t delta_days)
{
    if(delta_days == 0) {
        return;
    }

    ev_state.offset_days += delta_days;
    ev_refresh_view(!ev_state.strip_pressed);
}

static void ev_apply_strip_visuals(int32_t drag_x)
{
    static const uint8_t opa_table[] = { 100, 85, 70, 55, 40 };
    const int32_t max_index = (int32_t)(sizeof(opa_table) / sizeof(opa_table[0])) - 1;
    int32_t center_q8 = (EV_CENTER << 8) - (drag_x << 8) / EV_CELL_W;

    /* 拖拽时只插值透明度，保持文字始终以原生字号清晰渲染。 */
    for(int32_t i = 0; i < EV_CELL_CNT; ++i) {
        ev_cell_view_t * cell = &ev_view.cells[i];
        int32_t dist_q8 = LV_ABS((i << 8) - center_q8);
        int32_t dist_i = dist_q8 >> 8;
        int32_t dist_f = dist_q8 & 0xFF;
        uint8_t opa_a;
        uint8_t opa_b;
        int32_t opa_pct;
        lv_opa_t opa;

        if(dist_i > max_index) {
            dist_i = max_index;
            dist_f = 0;
        }

        opa_a = opa_table[dist_i];
        opa_b = opa_table[dist_i < max_index ? dist_i + 1 : max_index];

        opa_pct = opa_a + ((int32_t)(opa_b - opa_a) * dist_f) / 256;
        opa_pct = LV_CLAMP(12, opa_pct, 100);

        opa = (lv_opa_t)(LV_OPA_COVER * opa_pct / 100);

        lv_obj_set_style_text_opa(cell->weekday, opa, 0);
        lv_obj_set_style_text_opa(cell->day, opa, 0);
        lv_obj_set_style_text_opa(cell->mark, opa, 0);
        lv_obj_set_style_bg_opa(cell->dot, (lv_opa_t)(opa * 55 / 100), 0);
    }
}

static void ev_finish_strip_snap(void)
{
    if(ev_view.strip) {
        lv_obj_set_style_translate_x(ev_view.strip, 0, 0);
    }

    ev_state.drag_x = 0;
    ev_apply_strip_visuals(0);

    if(ev_state.snap_delta != 0) {
        /* 吸附动画结束后再真正提交日期偏移，避免中途刷新造成跳动。 */
        int32_t delta = ev_state.snap_delta;
        ev_state.snap_delta = 0;
        ev_shift_days(delta);
    }
}

static void ev_strip_snap_ready_cb(lv_anim_t * anim)
{
    (void)anim;
    ev_finish_strip_snap();
    ev_restore_swipe_back();
}

static void ev_stop_strip_motion(void)
{
    if(ev_view.strip) {
        lv_anim_del(ev_view.strip, (lv_anim_exec_xcb_t)ev_set_translate_x);
    }
    ev_state.snap_delta = 0;

    if(ev_view.strip) {
        lv_obj_set_style_translate_x(ev_view.strip, 0, 0);
    }

    ev_state.drag_x = 0;
    ev_apply_strip_visuals(0);
}

static void ev_reset_runtime_state(void)
{
    ev_state.strip_pressed = false;
    ev_state.strip_dragged = false;
    ev_state.drag_x = 0;
    ev_state.snap_delta = 0;
}

static void ev_cancel_strip_drag(void)
{
    ev_state.strip_pressed = false;
    ev_state.strip_dragged = false;
    ev_stop_strip_motion();
    ev_restore_swipe_back();
}

static bool ev_strip_point_to_index(const lv_point_t * point, int32_t * out_index)
{
    lv_area_t area;
    lv_coord_t local_x;
    lv_coord_t local_y;
    lv_coord_t strip_w;
    lv_coord_t left_pad;

    if(!ev_view.strip_wrap || !point || !out_index) {
        return false;
    }

    lv_obj_get_coords(ev_view.strip_wrap, &area);
    local_x = point->x - area.x1 - ev_state.drag_x;
    local_y = point->y - area.y1;
    strip_w = EV_CELL_W * EV_CELL_CNT;
    left_pad = (lv_obj_get_width(ev_view.strip_wrap) - strip_w) / 2;

    if(local_y < 0 || local_y >= lv_obj_get_height(ev_view.strip_wrap)) {
        return false;
    }
    if(local_x < left_pad || local_x >= left_pad + strip_w) {
        return false;
    }

    *out_index = (int32_t)((local_x - left_pad) / EV_CELL_W);
    return *out_index >= 0 && *out_index < EV_CELL_CNT;
}

static void ev_begin_strip_drag(lv_indev_t * indev)
{
    lv_anim_del(ev_view.strip, (lv_anim_exec_xcb_t)ev_set_translate_x);
    ev_finish_strip_snap();

    ev_state.strip_pressed = true;
    ev_state.strip_dragged = false;
    lv_indev_get_point(indev, &ev_state.press_point);

    lv_async_call_cancel(ev_enable_swipe_back_async, ui_EventsScreen);
    app_screen_set_swipe_back_enabled(ui_EventsScreen, false);
}

static void ev_consume_drag_steps(void)
{
    /* 拖动跨过一个单元宽度时，立即滚动一天并重置参考点，实现连续滑动。 */
    while(ev_state.drag_x <= -EV_CELL_W) {
        ev_state.offset_days += 1;
        ev_state.drag_x += EV_CELL_W;
        ev_state.press_point.x -= EV_CELL_W;
        ev_state.strip_dragged = true;
        ev_refresh_view(false);
    }

    while(ev_state.drag_x >= EV_CELL_W) {
        ev_state.offset_days -= 1;
        ev_state.drag_x -= EV_CELL_W;
        ev_state.press_point.x += EV_CELL_W;
        ev_state.strip_dragged = true;
        ev_refresh_view(false);
    }
}

static void ev_update_strip_drag(lv_indev_t * indev)
{
    lv_point_t point;

    if(!ev_state.strip_pressed) {
        return;
    }

    lv_indev_get_point(indev, &point);
    ev_state.drag_x = point.x - ev_state.press_point.x;
    ev_consume_drag_steps();

    if(LV_ABS(ev_state.drag_x) >= EV_TAP_SLOP_PX) {
        ev_state.strip_dragged = true;
    }

    ev_set_translate_x(ev_view.strip, ev_state.drag_x);
}

static int32_t ev_calc_snap_delta(void)
{
    int32_t snap_delta;

    if(ev_state.drag_x <= 0) {
        snap_delta = (-ev_state.drag_x + EV_CELL_W / 2) / EV_CELL_W;
    } else {
        snap_delta = -((ev_state.drag_x + EV_CELL_W / 2) / EV_CELL_W);
    }

    return LV_CLAMP(-1, snap_delta, 1);
}

static void ev_handle_strip_tap(const lv_point_t * point)
{
    int32_t index = -1;

    if(ev_strip_point_to_index(point, &index) && index != EV_CENTER) {
        ev_shift_days(index - EV_CENTER);
    }

    ev_state.drag_x = 0;
    ev_set_translate_x(ev_view.strip, 0);
    ev_restore_swipe_back();
}

static void ev_start_strip_snap(int32_t snap_delta)
{
    int32_t target_x = -snap_delta * EV_CELL_W;

    ev_state.snap_delta = snap_delta;

    ev_start_int_anim(ev_view.strip, (lv_anim_exec_xcb_t)ev_set_translate_x,
                      ev_state.drag_x, target_x, EV_DRAG_SNAP_MS,
                      lv_anim_path_ease_out, ev_strip_snap_ready_cb);
}

static void ev_end_strip_drag(lv_indev_t * indev)
{
    lv_point_t point;
    int32_t total_dx;

    if(!ev_state.strip_pressed) {
        ev_restore_swipe_back();
        return;
    }

    lv_indev_get_point(indev, &point);
    total_dx = point.x - ev_state.press_point.x;

    if(!ev_state.strip_dragged && LV_ABS(total_dx) < EV_TAP_SLOP_PX) {
        ev_handle_strip_tap(&point);
    } else {
        ev_start_strip_snap(ev_calc_snap_delta());
    }

    ev_state.strip_pressed = false;
    ev_state.strip_dragged = false;
}

static void ev_strip_event_cb(lv_event_t * e)
{
    lv_event_code_t code;
    lv_indev_t * indev;

    if(!ui_EventsScreen) {
        return;
    }

    code = lv_event_get_code(e);
    if(code == LV_EVENT_CANCEL) {
        ev_cancel_strip_drag();
        return;
    }

    indev = lv_event_get_indev(e);
    if(!indev) {
        if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            ev_cancel_strip_drag();
        }
        return;
    }

    if(code == LV_EVENT_PRESSED) {
        ev_begin_strip_drag(indev);
        return;
    }

    if(code == LV_EVENT_PRESSING) {
        ev_update_strip_drag(indev);
        return;
    }

    if(code == LV_EVENT_RELEASED) {
        ev_end_strip_drag(indev);
        return;
    }

    if(code == LV_EVENT_PRESS_LOST) {
        ev_cancel_strip_drag();
    }
}

static void ev_remember_today(void)
{
    ev_day_info_t today;

    ev_state.today_valid = ev_get_day_info(0, &today);
    if(ev_state.today_valid) {
        ev_state.today_year = today.tm.tm_year;
        ev_state.today_yday = today.tm.tm_yday;
    }
}

static void ev_date_timer_cb(lv_timer_t * timer)
{
    ev_day_info_t today;

    (void)timer;
    if(!ui_EventsScreen || lv_screen_active() != ui_EventsScreen ||
       !ev_get_day_info(0, &today)) {
        return;
    }

    if(!ev_state.today_valid ||
       ev_state.today_year != today.tm.tm_year ||
       ev_state.today_yday != today.tm.tm_yday) {
        ev_state.today_valid = true;
        ev_state.today_year = today.tm.tm_year;
        ev_state.today_yday = today.tm.tm_yday;
        ev_refresh_view(false);
    }
}

static void ev_set_date_timer_running(bool running)
{
    if(!ev_view.date_timer) {
        return;
    }

    if(running) {
        lv_timer_reset(ev_view.date_timer);
        lv_timer_resume(ev_view.date_timer);
    } else {
        lv_timer_pause(ev_view.date_timer);
    }
}

static void ev_sync_screen_state(void)
{
    /* 页面进入/离开时统一收口运行态，避免残留动画和手势状态。 */
    lv_async_call_cancel(ev_enable_swipe_back_async, ui_EventsScreen);
    ev_reset_runtime_state();
    ev_stop_strip_motion();
    app_screen_set_swipe_back_enabled(ui_EventsScreen, true);
}

static void ev_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_SCREEN_LOADED) {
        ev_state.offset_days = 0;
        ev_sync_screen_state();
        ev_remember_today();
        ev_refresh_view(true);
        ev_set_date_timer_running(true);
    } else if(code == LV_EVENT_SCREEN_UNLOADED) {
        ev_set_date_timer_running(false);
        ev_sync_screen_state();
    }
}

static lv_obj_t * ev_create_label(lv_obj_t * parent, const char * text)
{
    lv_obj_t * label = lv_label_create(parent);

    ev_reset_style(label);
    lv_label_set_text(label, text);
    return label;
}

static void ev_build_header(lv_obj_t * parent)
{
    lv_obj_t * today_text;

    /* 顶部区域只负责展示当前选中日期信息。 */
    ev_view.content = lv_obj_create(parent);
    ev_reset_style(ev_view.content);
    lv_obj_set_size(ev_view.content, LV_PCT(100), EV_HEADER_H);
    lv_obj_align(ev_view.content, LV_ALIGN_TOP_MID, 0, EV_HEADER_Y);
    lv_obj_set_flex_flow(ev_view.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ev_view.content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ev_view.content, 0, 0);
    ev_disable_interaction(ev_view.content);

    ev_view.month_year = ev_create_label(ev_view.content, "");
    lv_obj_set_style_text_font(ev_view.month_year, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(ev_view.month_year, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ev_view.month_year, (lv_opa_t)(LV_OPA_COVER * 58 / 100), 0);
    lv_obj_set_style_text_letter_space(ev_view.month_year, 0, 0);

    ev_view.day_big = lv_image_create(ev_view.content);
    ev_reset_style(ev_view.day_big);
    lv_image_set_src(ev_view.day_big, event_day_digit_images[0]);
    lv_image_set_antialias(ev_view.day_big, false);
    lv_obj_set_style_image_recolor(ev_view.day_big, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(ev_view.day_big, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_top(ev_view.day_big, 10, 0);
    ev_disable_interaction(ev_view.day_big);

    ev_view.weekday_row = lv_obj_create(ev_view.content);
    ev_reset_style(ev_view.weekday_row);
    lv_obj_set_size(ev_view.weekday_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ev_view.weekday_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ev_view.weekday_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(ev_view.weekday_row, 8, 0);
    lv_obj_set_style_pad_top(ev_view.weekday_row, 16, 0);
    ev_disable_interaction(ev_view.weekday_row);

    ev_view.weekday = ev_create_label(ev_view.weekday_row, "");
    lv_obj_set_style_text_font(ev_view.weekday, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(ev_view.weekday, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ev_view.weekday, (lv_opa_t)(LV_OPA_COVER * 68 / 100), 0);
    lv_obj_set_style_text_letter_space(ev_view.weekday, 0, 0);

    ev_view.today_badge = lv_obj_create(ev_view.weekday_row);
    ev_reset_style(ev_view.today_badge);
    lv_obj_set_size(ev_view.today_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(ev_view.today_badge, 20, 0);
    lv_obj_set_style_bg_color(ev_view.today_badge, lv_color_hex(EV_GREEN), 0);
    lv_obj_set_style_bg_opa(ev_view.today_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(ev_view.today_badge, 9, 0);
    lv_obj_set_style_pad_right(ev_view.today_badge, 9, 0);
    lv_obj_set_style_pad_top(ev_view.today_badge, 2, 0);
    lv_obj_set_style_pad_bottom(ev_view.today_badge, 2, 0);
    ev_disable_interaction(ev_view.today_badge);
    lv_obj_add_flag(ev_view.today_badge, LV_OBJ_FLAG_HIDDEN);

    today_text = ev_create_label(ev_view.today_badge, "今天");
    lv_obj_set_style_text_font(today_text, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(today_text, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(today_text, 0, 0);
}

static void ev_build_strip_cell(lv_obj_t * parent, int32_t index)
{
    ev_cell_view_t * cell = &ev_view.cells[index];

    cell->root = lv_obj_create(parent);
    ev_reset_style(cell->root);
    lv_obj_set_size(cell->root, EV_CELL_W, EV_STRIP_H);
    lv_obj_set_style_radius(cell->root, 0, 0);
    lv_obj_set_style_bg_opa(cell->root, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(cell->root, 0, 0);
    ev_disable_interaction(cell->root);

    cell->mark = ev_create_label(cell->root, "");
    lv_obj_set_style_text_font(cell->mark, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(cell->mark, lv_color_hex(EV_ACCENT), 0);
    lv_obj_set_style_text_opa(cell->mark, (lv_opa_t)(LV_OPA_COVER * 47 / 100), 0);
    lv_obj_align(cell->mark, LV_ALIGN_TOP_MID, 0, 6);
    ui_make_decor_hit_passthrough(cell->mark);
    lv_obj_add_flag(cell->mark, LV_OBJ_FLAG_HIDDEN);

    cell->weekday = ev_create_label(cell->root, "");
    lv_obj_set_style_text_font(cell->weekday, ui_builtin_text_font(), 0);
    lv_obj_align(cell->weekday, LV_ALIGN_TOP_MID, 0, 6);
    ui_make_decor_hit_passthrough(cell->weekday);

    cell->day = ev_create_label(cell->root, "");
    lv_obj_set_style_text_font(cell->day, &lv_font_montserrat_16, 0);
    lv_obj_align(cell->day, LV_ALIGN_CENTER, 0, 4);
    ui_make_decor_hit_passthrough(cell->day);

    cell->dot = lv_obj_create(cell->root);
    ev_reset_style(cell->dot);
    lv_obj_set_size(cell->dot, 3, 3);
    lv_obj_set_style_radius(cell->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cell->dot, lv_color_hex(EV_GREEN), 0);
    lv_obj_set_style_bg_opa(cell->dot, (lv_opa_t)(LV_OPA_COVER * 53 / 100), 0);
    lv_obj_align(cell->dot, LV_ALIGN_BOTTOM_MID, 0, -8);
    ev_disable_interaction(cell->dot);
    lv_obj_add_flag(cell->dot, LV_OBJ_FLAG_HIDDEN);
}

static void ev_build_strip(lv_obj_t * parent)
{
    lv_obj_t * capsule;

    /* 中间高亮胶囊固定不动，真正滑动的是整条日期序列。 */
    ev_view.strip_wrap = lv_obj_create(parent);
    ev_reset_style(ev_view.strip_wrap);
    lv_obj_set_size(ev_view.strip_wrap, LV_PCT(100), EV_STRIP_H);
    lv_obj_align(ev_view.strip_wrap, LV_ALIGN_BOTTOM_MID, 0, EV_STRIP_Y);
    ev_disable_interaction(ev_view.strip_wrap);
    lv_obj_add_flag(ev_view.strip_wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    capsule = lv_obj_create(ev_view.strip_wrap);
    ev_reset_style(capsule);
    lv_obj_set_size(capsule, EV_CELL_W - 6, 68);
    lv_obj_set_style_radius(capsule, 14, 0);
    lv_obj_set_style_bg_color(capsule, lv_color_hex(EV_GREEN), 0);
    lv_obj_set_style_bg_opa(capsule, (lv_opa_t)(LV_OPA_COVER * 9 / 100), 0);
    lv_obj_set_style_border_width(capsule, 2, 0);
    lv_obj_set_style_border_color(capsule, lv_color_hex(EV_GREEN), 0);
    lv_obj_set_style_border_opa(capsule, (lv_opa_t)(LV_OPA_COVER * 33 / 100), 0);
    lv_obj_align(capsule, LV_ALIGN_CENTER, 0, 0);
    ev_disable_interaction(capsule);

    ev_view.strip = lv_obj_create(ev_view.strip_wrap);
    ev_reset_style(ev_view.strip);
    lv_obj_set_size(ev_view.strip, EV_CELL_W * EV_CELL_CNT, EV_STRIP_H);
    lv_obj_align(ev_view.strip, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(ev_view.strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ev_view.strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ev_view.strip, LV_OBJ_FLAG_SCROLLABLE);

    for(int32_t i = 0; i < EV_CELL_CNT; ++i) {
        ev_build_strip_cell(ev_view.strip, i);
    }
}

static void ev_build_fades(lv_obj_t * parent)
{
    lv_obj_t * fade_left;
    lv_obj_t * fade_right;

    fade_left = lv_obj_create(parent);
    ev_reset_style(fade_left);
    lv_obj_set_size(fade_left, EV_FADE_W, EV_STRIP_H);
    lv_obj_set_style_bg_opa(fade_left, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(fade_left, &ev_view.fade_left, 0);
    lv_obj_align(fade_left, LV_ALIGN_BOTTOM_LEFT, 0, EV_STRIP_Y);
    ev_disable_interaction(fade_left);

    fade_right = lv_obj_create(parent);
    ev_reset_style(fade_right);
    lv_obj_set_size(fade_right, EV_FADE_W, EV_STRIP_H);
    lv_obj_set_style_bg_opa(fade_right, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(fade_right, &ev_view.fade_right, 0);
    lv_obj_align(fade_right, LV_ALIGN_BOTTOM_RIGHT, 0, EV_STRIP_Y);
    ev_disable_interaction(fade_right);
}

static void ev_build_touch_layer(lv_obj_t * parent)
{
    ev_view.strip_touch = lv_obj_create(parent);
    ev_reset_style(ev_view.strip_touch);
    lv_obj_set_size(ev_view.strip_touch, LV_PCT(100), EV_STRIP_H);
    lv_obj_align(ev_view.strip_touch, LV_ALIGN_BOTTOM_MID, 0, EV_STRIP_Y);
    lv_obj_set_style_bg_opa(ev_view.strip_touch, LV_OPA_0, 0);
    lv_obj_add_flag(ev_view.strip_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ev_view.strip_touch, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(ev_view.strip_touch, ev_strip_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ev_view.strip_touch, ev_strip_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ev_view.strip_touch, ev_strip_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ev_view.strip_touch, ev_strip_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(ev_view.strip_touch, ev_strip_event_cb, LV_EVENT_CANCEL, NULL);
}

static void ev_build_screen(void)
{
    ui_EventsScreen = lv_obj_create(NULL);
    ev_reset_style(ui_EventsScreen);
    lv_obj_set_size(ui_EventsScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_EventsScreen, lv_color_hex(EV_BG), 0);
    lv_obj_set_style_bg_opa(ui_EventsScreen, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_EventsScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_EventsScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_EventsScreen, ev_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_EventsScreen, ev_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    ev_build_header(ui_EventsScreen);
    ev_build_strip(ui_EventsScreen);
    ev_build_fades(ui_EventsScreen);
    ev_build_touch_layer(ui_EventsScreen);
}

void ui_EventsScreen_init(void)
{
    if(ui_EventsScreen) {
        return;
    }

    lv_memzero(&ev_view, sizeof(ev_view));
    lv_memzero(&ev_state, sizeof(ev_state));
    ev_init_gradients();
    ev_build_screen();
    ev_view.date_timer = lv_timer_create(ev_date_timer_cb, EV_DATE_REFRESH_MS, NULL);
    if(ev_view.date_timer) {
        lv_timer_pause(ev_view.date_timer);
    }
    ev_refresh_view(false);
    app_screen_enable_swipe_back(ui_EventsScreen);
}

void ui_EventsScreen_deinit(void)
{
    if(ui_EventsScreen) {
        lv_async_call_cancel(ev_enable_swipe_back_async, ui_EventsScreen);
    }

    if(ev_view.date_timer) {
        lv_timer_delete(ev_view.date_timer);
        ev_view.date_timer = NULL;
    }

    if(ui_EventsScreen) {
        lv_obj_delete(ui_EventsScreen);
        ui_EventsScreen = NULL;
    }

    lv_memzero(&ev_view, sizeof(ev_view));
    lv_memzero(&ev_state, sizeof(ev_state));
}
