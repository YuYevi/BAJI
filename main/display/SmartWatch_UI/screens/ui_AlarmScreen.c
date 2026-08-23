/**
 * @file ui_AlarmScreen.c
 * @brief 闹钟/倒计时页面
 *
 * 重构目标：
 * 1. 统一收敛页面状态与视图引用，减少分散的全局变量。
 * 2. 把“数据逻辑、状态刷新、UI 创建”拆分成更清晰的模块。
 * 3. 保留闹钟列表、编辑面板、倒计时、响铃弹窗、贪睡提醒等完整能力。
 */

#include "ui_AlarmScreen.h"

#include "../ui.h"
#include "../app_common.h"
#include "../ui_runtime.h"

#include <string.h>
#include <time.h>

lv_obj_t * ui_AlarmScreen = NULL;

#define ALARM_AMBER                   lv_color_hex(0xf59e0b)
#define ALARM_TEAL                    lv_color_hex(0x2dd4bf)
#define ALARM_RED                     lv_color_hex(0xef4444)
#define ALARM_BG                      lv_color_hex(0x0e0e16)
#define ALARM_CARD_OFF_TEXT           lv_color_white()
#define ALARM_CARD_ON_TEXT            lv_color_hex(0xfbbf24)
#define ALARM_TAB_DARK_TEXT           lv_color_hex(0x1a1200)
#define ALARM_TIMER_DARK_TEXT         lv_color_hex(0x013330)
#define ALARM_ALERT_CARD_BG           lv_color_hex(0x161625)

#define ALARM_MAX_ITEMS               8U
#define ALARM_RING_REPEAT_COUNT       3U
#define ALARM_RING_GAP_AFTER_PLAY_MS  5000U
#define ALARM_RING_TIMER_STEP_MS      100U
#define ALARM_RING_SOUND_START_TIMEOUT_MS 1500U
#define ALARM_CD_ARC_SIZE             148
#define ALARM_PANEL_TOP_Y             52
#define ALARM_CARD_WIDTH              260
#define ALARM_CARD_HEIGHT             54
#define ALARM_CARD_DELETE_REVEAL      88
#define ALARM_CARD_DELETE_BTN_WIDTH   76
#define ALARM_CARD_DELETE_BTN_HEIGHT  54
#define ALARM_CARD_SWIPE_THRESHOLD    ((ALARM_CARD_WIDTH * 3) / 10)

typedef enum {
    CD_IDLE = 0,
    CD_RUNNING,
    CD_PAUSED,
    CD_DONE
} cd_state_t;

typedef enum {
    ALARM_ALERT_NONE = 0,
    ALARM_ALERT_ALARM,
    ALARM_ALERT_COUNTDOWN
} alarm_alert_kind_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    bool on;
} alarm_item_t;

typedef struct {
    lv_obj_t * btn;
    lv_obj_t * icon;
    lv_obj_t * label;
} alarm_tab_btn_view_t;

typedef struct {
    lv_obj_t * row;
    lv_obj_t * delete_slot;
    lv_obj_t * delete_btn;
    lv_obj_t * delete_icon;
    lv_obj_t * card;
    lv_obj_t * accent;
    lv_obj_t * time_label;
    lv_obj_t * toggle;
    bool press_started;
    bool swiping;
    bool moved;
} alarm_card_view_t;

typedef struct {
    lv_obj_t * idle_cont;
    lv_obj_t * roller_min;
    lv_obj_t * roller_sec;
    lv_obj_t * start_btn;
    lv_obj_t * start_icon;
    lv_obj_t * start_label;

    lv_obj_t * running_cont;
    lv_obj_t * arc;
    lv_obj_t * time_label;
    lv_obj_t * status_label;
    lv_obj_t * play_btn;
    lv_obj_t * play_icon;
    lv_obj_t * reset_btn;
    lv_obj_t * total_label;
} alarm_countdown_view_t;

typedef struct {
    lv_obj_t * panel;
    lv_obj_t * title;
    lv_obj_t * close_btn;
    lv_obj_t * roller_hour;
    lv_obj_t * roller_min;
    lv_obj_t * delete_btn;
    lv_obj_t * ok_btn;
} alarm_edit_view_t;

typedef struct {
    lv_obj_t * overlay;
    lv_obj_t * card;
    lv_obj_t * title;
    lv_obj_t * message;
    lv_obj_t * left_btn;
    lv_obj_t * right_btn;
    lv_obj_t * left_label;
    lv_obj_t * right_label;
} alarm_alert_view_t;

typedef struct {
    lv_obj_t * root;
    lv_obj_t * header;

    alarm_tab_btn_view_t tab_alarm;
    alarm_tab_btn_view_t tab_timer;

    lv_obj_t * alarm_cont;
    lv_obj_t * timer_cont;

    lv_obj_t * next_pill;
    lv_obj_t * next_dot;
    lv_obj_t * next_label;
    lv_obj_t * list_cont;
    lv_obj_t * fab_btn;

    alarm_card_view_t cards[ALARM_MAX_ITEMS];
    alarm_countdown_view_t cd;
    alarm_edit_view_t edit;
    alarm_alert_view_t alert;
} alarm_view_t;

typedef struct {
    uint8_t tab;
    alarm_item_t items[ALARM_MAX_ITEMS];
    uint8_t count;

    cd_state_t cd_state;
    int32_t cd_seconds;
    int32_t cd_total;
    uint32_t cd_remaining_ms;
    uint32_t cd_deadline_tick;

    bool editing_new;
    uint8_t editing_idx;

    alarm_alert_kind_t alert_kind;
    time_t snooze_until;

    int32_t last_ring_minute_key;
    int32_t last_hint_minute_key;

    lv_timer_t * service_timer;
    uint32_t alarm_generation;
    bool runtime_initialized;

    uint32_t ring_wait_started_tick;
    uint8_t ring_play_count;
    bool ring_active;
    bool ring_waiting_for_sound_start;
    bool ring_waiting_for_sound_finish;

    int16_t expanded_card_idx;
    int16_t suppress_click_idx;
} alarm_state_t;

static alarm_view_t g_alarm_view;
static alarm_state_t g_alarm_state;

static const char * const ALARM_HOUR_OPTIONS_24 =
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";

static const char * const ALARM_MIN_SEC_OPTIONS_60 =
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59";

/* ----------------------------- 前置声明 ----------------------------- */

static void alarm_apply_tab_style(void);
static void alarm_refresh_next_hint(void);
static void alarm_rebuild_list(void);
static void alarm_apply_countdown_state(void);
static void alarm_update_countdown_total_label(void);
static void alarm_stop_ring(void);
static void alarm_hide_alert_popup(void);
static void alarm_close_edit_panel(void);
static void alarm_refresh_swipe_back_state(void);
static void alarm_sync_items_if_changed(bool force);
static void alarm_enable_press_glow(lv_obj_t * obj, lv_color_t color);
static void alarm_start_countdown_deadline(uint32_t remaining_ms);
static lv_obj_t * alarm_create_circle_button(lv_obj_t * parent,
                                             int32_t size,
                                             lv_color_t bg_color,
                                             lv_opa_t bg_opa,
                                             lv_color_t border_color,
                                             lv_opa_t border_opa);

static void alarm_tab_event_cb(lv_event_t * e);
static void alarm_fab_event_cb(lv_event_t * e);
static void alarm_card_click_cb(lv_event_t * e);
static void alarm_toggle_cb(lv_event_t * e);
static void alarm_card_swipe_event_cb(lv_event_t * e);
static void alarm_card_delete_event_cb(lv_event_t * e);
static void alarm_blank_area_click_cb(lv_event_t * e);
static void alarm_edit_close_event_cb(lv_event_t * e);
static void alarm_edit_delete_event_cb(lv_event_t * e);
static void alarm_edit_ok_event_cb(lv_event_t * e);
static void alarm_alert_left_event_cb(lv_event_t * e);
static void alarm_alert_right_event_cb(lv_event_t * e);
static void alarm_cd_start_cb(lv_event_t * e);
static void alarm_cd_toggle_cb(lv_event_t * e);
static void alarm_cd_reset_cb(lv_event_t * e);

static void alarm_service_timer_cb(lv_timer_t * timer);

/* ----------------------------- 通用工具 ----------------------------- */

static lv_opa_t alarm_pct_opa(uint8_t pct)
{
    return (lv_opa_t)(LV_OPA_COVER * pct / 100U);
}

static void alarm_set_hidden(lv_obj_t * obj, bool hidden)
{
    if(!obj) return;
    if(hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void alarm_make_passthrough(lv_obj_t * obj)
{
    if(obj) {
        ui_make_decor_hit_passthrough(obj);
    }
}

static lv_obj_t * alarm_create_clean_container(lv_obj_t * parent)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

static lv_obj_t * alarm_create_clean_button(lv_obj_t * parent)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    return btn;
}

static lv_obj_t * alarm_create_label(lv_obj_t * parent,
                                     const char * text,
                                     const lv_font_t * font,
                                     lv_color_t color,
                                     lv_opa_t opa)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text ? text : "");
    if(font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    return label;
}

static void alarm_format_hhmm(char * buf, size_t size, uint8_t hour, uint8_t minute)
{
    lv_snprintf(buf, size, "%02u:%02u", (unsigned int)hour, (unsigned int)minute);
}

static bool alarm_obj_is_descendant(lv_obj_t * obj, lv_obj_t * ancestor)
{
    while(obj) {
        if(obj == ancestor) {
            return true;
        }
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

static bool alarm_get_now(time_t * out_ts, struct tm * out_tm)
{
    time_t now = time(NULL);

    if(out_ts) {
        *out_ts = now;
    }

    if(!out_tm) {
        return true;
    }

#if defined(_WIN32)
    return localtime_s(out_tm, &now) == 0;
#else
    struct tm * current = localtime(&now);
    if(!current) {
        return false;
    }
    *out_tm = *current;
    return true;
#endif
}

static int32_t alarm_get_minute_key(const struct tm * tm_info)
{
    if(!tm_info) {
        return -1;
    }
    return (int32_t)tm_info->tm_yday * 1440 + tm_info->tm_hour * 60 + tm_info->tm_min;
}

/* ----------------------------- 数据层 ----------------------------- */

static void alarm_save_items(void)
{
    for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
        app_device_set_alarm_item(i,
                                  g_alarm_state.items[i].hour,
                                  g_alarm_state.items[i].minute,
                                  g_alarm_state.items[i].on);
    }
    app_device_set_alarm_count(g_alarm_state.count);
    g_alarm_state.alarm_generation = app_device_get_alarm_generation();
}

static void alarm_save_item(uint8_t idx)
{
    if(idx >= g_alarm_state.count) {
        return;
    }

    const alarm_item_t * item = &g_alarm_state.items[idx];
    app_device_set_alarm_item(idx, item->hour, item->minute, item->on);
    g_alarm_state.alarm_generation = app_device_get_alarm_generation();
}

static void alarm_load_items(void)
{
    /* 没有存储数据时沿用原页面的默认配置。 */
    g_alarm_state.count = 3;
    g_alarm_state.items[0] = (alarm_item_t){ 6, 0, false };
    g_alarm_state.items[1] = (alarm_item_t){ 0, 0, false };
    g_alarm_state.items[2] = (alarm_item_t){ 0, 0, false };

    int32_t saved_count = app_device_get_alarm_count();
    if(saved_count < 0) {
        return;
    }
    if(saved_count > (int32_t)ALARM_MAX_ITEMS) {
        saved_count = (int32_t)ALARM_MAX_ITEMS;
    }

    g_alarm_state.count = (uint8_t)saved_count;
    for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
        uint8_t hour = 0;
        uint8_t minute = 0;
        bool enabled = false;
        if(app_device_get_alarm_item(i, &hour, &minute, &enabled)) {
            g_alarm_state.items[i] = (alarm_item_t){ hour, minute, enabled };
        } else {
            g_alarm_state.items[i] = (alarm_item_t){ 0, 0, false };
        }
    }
}

static void alarm_sync_items_if_changed(bool force)
{
    uint32_t generation = app_device_get_alarm_generation();
    if(!force && generation == g_alarm_state.alarm_generation) {
        return;
    }

    alarm_load_items();
    g_alarm_state.alarm_generation = app_device_get_alarm_generation();

    if(g_alarm_view.list_cont) {
        alarm_close_edit_panel();
        alarm_rebuild_list();
    }
}

static void alarm_add_item(uint8_t hour, uint8_t minute)
{
    if(g_alarm_state.count >= ALARM_MAX_ITEMS) {
        smartwatch_ui_runtime_show_notification("闹钟已达上限", 2500);
        return;
    }

    uint8_t idx = g_alarm_state.count;
    g_alarm_state.items[idx] = (alarm_item_t){ hour, minute, true };
    g_alarm_state.count++;
    alarm_save_item(idx);
    app_device_set_alarm_count(g_alarm_state.count);
    g_alarm_state.alarm_generation = app_device_get_alarm_generation();
    alarm_rebuild_list();
}

static void alarm_delete_item(uint8_t idx)
{
    if(idx >= g_alarm_state.count) {
        return;
    }

    for(uint8_t i = idx; i + 1 < g_alarm_state.count; ++i) {
        g_alarm_state.items[i] = g_alarm_state.items[i + 1];
    }
    g_alarm_state.count--;
    alarm_save_items();
    alarm_rebuild_list();
}

/* ----------------------------- 响铃与弹窗 ----------------------------- */

static void alarm_hide_alert_popup(void)
{
    g_alarm_state.alert_kind = ALARM_ALERT_NONE;
    alarm_set_hidden(g_alarm_view.alert.overlay, true);
}

static void alarm_show_alert_popup(alarm_alert_kind_t kind, const char * title)
{
    g_alarm_state.alert_kind = kind;
    if(!g_alarm_view.alert.overlay) {
        return;
    }

    lv_label_set_text(g_alarm_view.alert.title, title ? title : "");

    if(kind == ALARM_ALERT_ALARM) {
        lv_label_set_text(g_alarm_view.alert.message, "请选择处理方式");
        lv_label_set_text(g_alarm_view.alert.left_label, "停止");
        lv_label_set_text(g_alarm_view.alert.right_label, "稍后提醒");

        lv_obj_set_style_bg_color(g_alarm_view.alert.right_btn, ALARM_AMBER, 0);
        lv_obj_set_style_bg_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(18), 0);
        lv_obj_set_style_border_color(g_alarm_view.alert.right_btn, ALARM_AMBER, 0);
        lv_obj_set_style_border_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(30), 0);
        lv_obj_set_style_text_color(g_alarm_view.alert.right_label, ALARM_CARD_ON_TEXT, 0);
        alarm_enable_press_glow(g_alarm_view.alert.right_btn, ALARM_AMBER);
    } else {
        lv_label_set_text(g_alarm_view.alert.message, "是否重新开始本次倒计时");
        lv_label_set_text(g_alarm_view.alert.left_label, "重新开始");
        lv_label_set_text(g_alarm_view.alert.right_label, "停止");

        lv_obj_set_style_bg_color(g_alarm_view.alert.right_btn, ALARM_RED, 0);
        lv_obj_set_style_bg_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(18), 0);
        lv_obj_set_style_border_color(g_alarm_view.alert.right_btn, ALARM_RED, 0);
        lv_obj_set_style_border_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(30), 0);
        lv_obj_set_style_text_color(g_alarm_view.alert.right_label, ALARM_RED, 0);
        alarm_enable_press_glow(g_alarm_view.alert.right_btn, ALARM_RED);
    }

    alarm_set_hidden(g_alarm_view.alert.overlay, false);
    lv_obj_move_foreground(g_alarm_view.alert.overlay);
}

static void alarm_schedule_snooze_10min(void)
{
    g_alarm_state.snooze_until = time(NULL) + 10 * 60;
}

static void alarm_stop_ring(void)
{
    if(g_alarm_state.ring_active) {
        smartwatch_ui_runtime_stop_sound();
    }

    g_alarm_state.ring_active = false;
    g_alarm_state.ring_wait_started_tick = 0;
    g_alarm_state.ring_play_count = 0;
    g_alarm_state.ring_waiting_for_sound_start = false;
    g_alarm_state.ring_waiting_for_sound_finish = false;

    if(g_alarm_state.service_timer) {
        lv_timer_set_period(g_alarm_state.service_timer, 1000);
        lv_timer_reset(g_alarm_state.service_timer);
    }
}

static void alarm_start_next_ring_playback(void)
{
    if(!g_alarm_state.ring_active || g_alarm_state.ring_play_count >= ALARM_RING_REPEAT_COUNT) {
        return;
    }

    smartwatch_ui_runtime_play_alarm_sound();
    g_alarm_state.ring_play_count++;
    g_alarm_state.ring_wait_started_tick = lv_tick_get();
    g_alarm_state.ring_waiting_for_sound_start = true;
    g_alarm_state.ring_waiting_for_sound_finish = false;
}

static void alarm_ring_service_step(void)
{
    if(!g_alarm_state.ring_active) {
        return;
    }

    uint32_t now_tick = lv_tick_get();
    bool sound_idle = smartwatch_ui_runtime_is_sound_idle();

    if(g_alarm_state.ring_waiting_for_sound_start) {
        if(!sound_idle) {
            g_alarm_state.ring_waiting_for_sound_start = false;
            g_alarm_state.ring_waiting_for_sound_finish = true;
        } else if(lv_tick_elaps(g_alarm_state.ring_wait_started_tick) >= ALARM_RING_SOUND_START_TIMEOUT_MS) {
            /* 音频启动失败时也要退出等待，避免 100ms 定时器永久运行。 */
            g_alarm_state.ring_waiting_for_sound_start = false;
            if(g_alarm_state.ring_play_count >= ALARM_RING_REPEAT_COUNT) {
                alarm_hide_alert_popup();
                alarm_stop_ring();
            } else {
                g_alarm_state.ring_wait_started_tick = now_tick;
            }
        }
        return;
    }

    if(g_alarm_state.ring_waiting_for_sound_finish) {
        if(sound_idle) {
            g_alarm_state.ring_waiting_for_sound_finish = false;
            if(g_alarm_state.ring_play_count >= ALARM_RING_REPEAT_COUNT) {
                alarm_hide_alert_popup();
                alarm_stop_ring();
            } else {
                g_alarm_state.ring_wait_started_tick = now_tick;
            }
        }
        return;
    }

    if(!sound_idle) {
        /* 兼容音频在启动超时之后才开始播放的情况。 */
        g_alarm_state.ring_wait_started_tick = 0;
        g_alarm_state.ring_waiting_for_sound_finish = true;
        return;
    }

    if(g_alarm_state.ring_wait_started_tick != 0 &&
       lv_tick_elaps(g_alarm_state.ring_wait_started_tick) >= ALARM_RING_GAP_AFTER_PLAY_MS) {
        alarm_start_next_ring_playback();
    }
}

static void alarm_play_ring(const char * text, alarm_alert_kind_t kind)
{
    alarm_stop_ring();
    g_alarm_state.ring_active = true;

    if(lv_screen_active() != ui_AlarmScreen) {
        ui_nav_push(&ui_AlarmScreen, LV_SCR_LOAD_ANIM_NONE, 0, 0);
    }

    if(g_alarm_state.service_timer) {
        lv_timer_set_period(g_alarm_state.service_timer, ALARM_RING_TIMER_STEP_MS);
        lv_timer_reset(g_alarm_state.service_timer);
    }

    if(text && text[0] != '\0') {
        smartwatch_ui_runtime_show_notification(text, 3000);
    }

    alarm_show_alert_popup(kind, text);
    alarm_start_next_ring_playback();
}

/* ----------------------------- 闹钟刷新 ----------------------------- */

static void alarm_refresh_next_hint(void)
{
    if(!g_alarm_view.next_label) {
        return;
    }

    time_t now_ts = 0;
    struct tm now_tm;
    if(!alarm_get_now(&now_ts, &now_tm)) {
        return;
    }

    int32_t now_minutes = now_tm.tm_hour * 60 + now_tm.tm_min;
    int32_t best_diff = -1;

    /* 贪睡提醒也视作下一次有效响铃时间。 */
    if(g_alarm_state.snooze_until > now_ts) {
        time_t diff_sec = g_alarm_state.snooze_until - now_ts;
        int32_t diff_min = (int32_t)((diff_sec + 59) / 60);
        if(diff_min <= 0) {
            diff_min = 1;
        }
        best_diff = diff_min;
    }

    for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
        if(!g_alarm_state.items[i].on) {
            continue;
        }

        int32_t alarm_minutes = g_alarm_state.items[i].hour * 60 + g_alarm_state.items[i].minute;
        int32_t diff = alarm_minutes - now_minutes;
        if(diff <= 0) {
            diff += 1440;
        }

        if(best_diff < 0 || diff < best_diff) {
            best_diff = diff;
        }
    }

    if(best_diff < 0) {
        lv_label_set_text(g_alarm_view.next_label, "暂无开启的闹钟");
        alarm_set_hidden(g_alarm_view.next_dot, true);
        return;
    }

    int32_t hours = best_diff / 60;
    int32_t minutes = best_diff % 60;
    char buf[32];

    if(hours <= 0) {
        lv_snprintf(buf, sizeof(buf), "%d 分钟后响铃", (int)minutes);
    } else if(minutes == 0) {
        lv_snprintf(buf, sizeof(buf), "%d 小时后响铃", (int)hours);
    } else {
        lv_snprintf(buf, sizeof(buf), "%d 小时 %d 分钟后响铃", (int)hours, (int)minutes);
    }

    lv_label_set_text(g_alarm_view.next_label, buf);
    alarm_set_hidden(g_alarm_view.next_dot, false);
}

static void alarm_check_due_alarms(time_t now_ts, const struct tm * now_tm)
{
    int32_t minute_key = alarm_get_minute_key(now_tm);
    if(minute_key < 0) {
        return;
    }

    if(g_alarm_state.snooze_until > 0 && now_ts >= g_alarm_state.snooze_until) {
        g_alarm_state.snooze_until = 0;
        alarm_refresh_next_hint();

        if(minute_key != g_alarm_state.last_ring_minute_key) {
            g_alarm_state.last_ring_minute_key = minute_key;
            alarm_play_ring("闹钟响了", ALARM_ALERT_ALARM);
        }
        return;
    }

    if(minute_key == g_alarm_state.last_ring_minute_key) {
        return;
    }

    for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
        const alarm_item_t * item = &g_alarm_state.items[i];
        if(!item->on) {
            continue;
        }
        if(item->hour == now_tm->tm_hour && item->minute == now_tm->tm_min) {
            g_alarm_state.last_ring_minute_key = minute_key;
            alarm_play_ring("闹钟响了", ALARM_ALERT_ALARM);
            return;
        }
    }
}

/* ----------------------------- 倒计时逻辑 ----------------------------- */

static void alarm_update_countdown_time_label(void)
{
    if(!g_alarm_view.cd.time_label) {
        return;
    }

    if(g_alarm_state.cd_state == CD_DONE) {
        lv_label_set_text(g_alarm_view.cd.time_label, "时间到！");
        lv_obj_set_style_text_font(g_alarm_view.cd.time_label, ui_builtin_text_font(), 0);
        lv_obj_set_style_text_color(g_alarm_view.cd.time_label, ALARM_RED, 0);
        lv_obj_set_style_text_opa(g_alarm_view.cd.time_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_letter_space(g_alarm_view.cd.time_label, 0, 0);
        return;
    }

    char buf[16];
    int32_t mm = g_alarm_state.cd_seconds / 60;
    int32_t ss = g_alarm_state.cd_seconds % 60;
    lv_snprintf(buf, sizeof(buf), "%02d:%02d", (int)mm, (int)ss);
    lv_label_set_text(g_alarm_view.cd.time_label, buf);
    lv_obj_set_style_text_font(g_alarm_view.cd.time_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(g_alarm_view.cd.time_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(g_alarm_view.cd.time_label,
                              g_alarm_state.cd_state == CD_PAUSED ? alarm_pct_opa(50) : LV_OPA_COVER,
                              0);
    lv_obj_set_style_text_letter_space(g_alarm_view.cd.time_label, 0, 0);
}

static void alarm_update_countdown_total_label(void)
{
    if(!g_alarm_view.cd.total_label) {
        return;
    }

    if(g_alarm_state.cd_total <= 0 || g_alarm_state.cd_state == CD_IDLE) {
        lv_label_set_text(g_alarm_view.cd.total_label, "");
        return;
    }

    char buf[24];
    int32_t mm = g_alarm_state.cd_total / 60;
    int32_t ss = g_alarm_state.cd_total % 60;
    lv_snprintf(buf, sizeof(buf), "共 %02d:%02d", (int)mm, (int)ss);
    lv_label_set_text(g_alarm_view.cd.total_label, buf);
}

static void alarm_update_countdown_progress(void)
{
    if(!g_alarm_view.cd.arc) {
        return;
    }

    int32_t value = 0;
    if(g_alarm_state.cd_total > 0) {
        value = 100 - (g_alarm_state.cd_seconds * 100) / g_alarm_state.cd_total;
        if(value < 0) value = 0;
        if(value > 100) value = 100;
    }
    lv_arc_set_value(g_alarm_view.cd.arc, value);
}

static void alarm_start_countdown_deadline(uint32_t remaining_ms)
{
    g_alarm_state.cd_remaining_ms = remaining_ms;
    g_alarm_state.cd_deadline_tick = remaining_ms > 0 ? lv_tick_get() + remaining_ms : 0;
    g_alarm_state.cd_seconds = (int32_t)((remaining_ms + 999U) / 1000U);
}

static bool alarm_update_countdown_from_clock(void)
{
    if(g_alarm_state.cd_state != CD_RUNNING) {
        return false;
    }

    uint32_t now_tick = lv_tick_get();
    int32_t delta = (int32_t)(g_alarm_state.cd_deadline_tick - now_tick);
    uint32_t remaining_ms = delta > 0 ? (uint32_t)delta : 0U;
    g_alarm_state.cd_remaining_ms = remaining_ms;
    g_alarm_state.cd_seconds = (int32_t)((remaining_ms + 999U) / 1000U);

    if(remaining_ms > 0) {
        return false;
    }

    g_alarm_state.cd_deadline_tick = 0;
    g_alarm_state.cd_seconds = 0;
    g_alarm_state.cd_state = CD_DONE;
    return true;
}

static void alarm_apply_countdown_state(void)
{
    alarm_set_hidden(g_alarm_view.cd.idle_cont, g_alarm_state.cd_state != CD_IDLE);
    alarm_set_hidden(g_alarm_view.cd.running_cont, g_alarm_state.cd_state == CD_IDLE);

    if(g_alarm_state.cd_state == CD_DONE) {
        alarm_set_hidden(g_alarm_view.cd.play_btn, true);

        if(g_alarm_view.cd.reset_btn) {
            lv_obj_set_size(g_alarm_view.cd.reset_btn, 48, 48);
            lv_obj_set_style_bg_color(g_alarm_view.cd.reset_btn, ALARM_RED, 0);
            lv_obj_set_style_bg_opa(g_alarm_view.cd.reset_btn, alarm_pct_opa(20), 0);
            lv_obj_set_style_border_color(g_alarm_view.cd.reset_btn, ALARM_RED, 0);
            lv_obj_set_style_border_opa(g_alarm_view.cd.reset_btn, alarm_pct_opa(35), 0);
        }
        if(g_alarm_view.cd.arc) {
            lv_obj_set_style_arc_color(g_alarm_view.cd.arc, ALARM_RED, LV_PART_INDICATOR);
        }
        if(g_alarm_view.cd.status_label) {
            lv_label_set_text(g_alarm_view.cd.status_label, "");
            alarm_set_hidden(g_alarm_view.cd.status_label, true);
        }
    } else {
        alarm_set_hidden(g_alarm_view.cd.play_btn, false);

        if(g_alarm_view.cd.reset_btn) {
            lv_obj_set_size(g_alarm_view.cd.reset_btn, 36, 36);
            lv_obj_set_style_bg_color(g_alarm_view.cd.reset_btn, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(g_alarm_view.cd.reset_btn, alarm_pct_opa(7), 0);
            lv_obj_set_style_border_color(g_alarm_view.cd.reset_btn, lv_color_white(), 0);
            lv_obj_set_style_border_opa(g_alarm_view.cd.reset_btn, alarm_pct_opa(10), 0);
        }
        if(g_alarm_view.cd.arc) {
            lv_obj_set_style_arc_color(g_alarm_view.cd.arc, ALARM_TEAL, LV_PART_INDICATOR);
        }
        if(g_alarm_view.cd.status_label) {
            alarm_set_hidden(g_alarm_view.cd.status_label, false);
            if(g_alarm_state.cd_state == CD_RUNNING) {
                lv_label_set_text(g_alarm_view.cd.status_label, "倒计时中");
            } else if(g_alarm_state.cd_state == CD_PAUSED) {
                lv_label_set_text(g_alarm_view.cd.status_label, "已暂停");
            } else {
                lv_label_set_text(g_alarm_view.cd.status_label, "");
            }
        }
    }

    if(g_alarm_view.cd.play_icon) {
        lv_label_set_text(g_alarm_view.cd.play_icon,
                          g_alarm_state.cd_state == CD_RUNNING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if(g_alarm_view.cd.play_btn) {
        lv_obj_set_style_bg_opa(g_alarm_view.cd.play_btn,
                                g_alarm_state.cd_state == CD_PAUSED ? alarm_pct_opa(75) : LV_OPA_COVER,
                                0);
    }

    alarm_update_countdown_time_label();
    alarm_update_countdown_total_label();
    alarm_update_countdown_progress();
}

static void alarm_reset_countdown(bool clear_total)
{
    g_alarm_state.cd_state = CD_IDLE;
    g_alarm_state.cd_seconds = 0;
    g_alarm_state.cd_remaining_ms = 0;
    g_alarm_state.cd_deadline_tick = 0;
    if(clear_total) {
        g_alarm_state.cd_total = 0;
    }
    alarm_apply_countdown_state();
}

static void alarm_service_timer_cb(lv_timer_t * timer)
{
    (void)timer;

    int32_t previous_seconds = g_alarm_state.cd_seconds;
    if(alarm_update_countdown_from_clock()) {
        alarm_apply_countdown_state();
        alarm_play_ring("倒计时结束", ALARM_ALERT_COUNTDOWN);
    } else if(g_alarm_state.cd_state == CD_RUNNING &&
              g_alarm_state.cd_seconds != previous_seconds) {
        alarm_apply_countdown_state();
    }

    alarm_ring_service_step();
    alarm_sync_items_if_changed(false);

    time_t now_ts = 0;
    struct tm now_tm;
    if(!alarm_get_now(&now_ts, &now_tm)) {
        return;
    }

    int32_t minute_key = alarm_get_minute_key(&now_tm);
    if(minute_key != g_alarm_state.last_hint_minute_key) {
        g_alarm_state.last_hint_minute_key = minute_key;
        alarm_refresh_next_hint();
    }

    alarm_check_due_alarms(now_ts, &now_tm);
}

/* ----------------------------- 闹钟卡片 ----------------------------- */

static int32_t alarm_card_get_offset(const alarm_card_view_t * card)
{
    if(!card || !card->card) {
        return 0;
    }
    return lv_obj_get_style_translate_x(card->card, LV_PART_MAIN);
}

static void alarm_update_delete_btn_position(alarm_card_view_t * card)
{
    if(!card || !card->delete_btn) {
        return;
    }

    int32_t offset = alarm_card_get_offset(card);
    int32_t x = ALARM_CARD_WIDTH + (ALARM_CARD_DELETE_REVEAL - ALARM_CARD_DELETE_BTN_WIDTH) + offset;
    int32_t y = (ALARM_CARD_HEIGHT - ALARM_CARD_DELETE_BTN_HEIGHT) / 2;
    lv_obj_set_pos(card->delete_btn, x, y);
}

static void alarm_card_set_offset(alarm_card_view_t * card, int32_t offset)
{
    if(!card || !card->card) {
        return;
    }

    if(offset > 0) {
        offset = 0;
    }
    if(offset < -ALARM_CARD_DELETE_REVEAL) {
        offset = -ALARM_CARD_DELETE_REVEAL;
    }

    lv_obj_set_style_translate_x(card->card, offset, 0);
    alarm_update_delete_btn_position(card);
}

static void alarm_anim_set_card_offset(alarm_card_view_t * card, int32_t value)
{
    alarm_card_set_offset(card, value);
}

static void alarm_refresh_swipe_back_state(void)
{
    bool enable_swipe_back = true;

    if(g_alarm_state.expanded_card_idx >= 0) {
        enable_swipe_back = false;
    } else {
        for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
            if(g_alarm_view.cards[i].swiping) {
                enable_swipe_back = false;
                break;
            }
        }
    }

    if(ui_AlarmScreen) {
        app_screen_set_swipe_back_enabled(ui_AlarmScreen, enable_swipe_back);
    }
}

static void alarm_anim_set_height(lv_obj_t * obj, int32_t value)
{
    if(obj && lv_obj_is_valid(obj)) {
        lv_obj_set_height(obj, value);
    }
}

static void alarm_anim_set_opa(lv_obj_t * obj, int32_t value)
{
    if(obj && lv_obj_is_valid(obj)) {
        lv_obj_set_style_opa(obj, (lv_opa_t)value, 0);
    }
}

static void alarm_cancel_card_animations(void)
{
    for(uint8_t i = 0; i < ALARM_MAX_ITEMS; ++i) {
        alarm_card_view_t * card = &g_alarm_view.cards[i];
        if(!card->card && !card->row && !card->delete_btn) {
            continue;
        }

        lv_anim_del(card, (lv_anim_exec_xcb_t)alarm_anim_set_card_offset);
        if(card->row) {
            lv_anim_del(card->row, (lv_anim_exec_xcb_t)alarm_anim_set_height);
        }
        if(card->card) {
            lv_anim_del(card->card, (lv_anim_exec_xcb_t)alarm_anim_set_opa);
        }
        if(card->delete_btn) {
            lv_anim_del(card->delete_btn, (lv_anim_exec_xcb_t)alarm_anim_set_opa);
        }
    }
}

static void alarm_close_expanded_card(bool anim);

static void alarm_animate_card_offset(uint8_t idx, int32_t target, uint32_t time, bool overshoot)
{
    if(idx >= g_alarm_state.count) {
        return;
    }

    alarm_card_view_t * card = &g_alarm_view.cards[idx];
    if(!card->card) {
        return;
    }

    lv_anim_del(card, (lv_anim_exec_xcb_t)alarm_anim_set_card_offset);

    if(time == 0) {
        alarm_card_set_offset(card, target);
    } else {
        lv_anim_t anim_dsc;
        lv_anim_init(&anim_dsc);
        lv_anim_set_var(&anim_dsc, card);
        lv_anim_set_values(&anim_dsc, alarm_card_get_offset(card), target);
        lv_anim_set_time(&anim_dsc, time);
        lv_anim_set_exec_cb(&anim_dsc, (lv_anim_exec_xcb_t)alarm_anim_set_card_offset);
        lv_anim_set_path_cb(&anim_dsc, overshoot ? lv_anim_path_overshoot : lv_anim_path_ease_out);
        lv_anim_start(&anim_dsc);
    }

    if(target == 0) {
        if(g_alarm_state.expanded_card_idx == (int16_t)idx) {
            g_alarm_state.expanded_card_idx = -1;
        }
    } else {
        g_alarm_state.expanded_card_idx = (int16_t)idx;
    }

    alarm_refresh_swipe_back_state();
}

static void alarm_expand_card(uint8_t idx, bool anim)
{
    if(g_alarm_state.expanded_card_idx >= 0 && g_alarm_state.expanded_card_idx != (int16_t)idx) {
        alarm_close_expanded_card(anim);
    }
    alarm_animate_card_offset(idx, -ALARM_CARD_DELETE_REVEAL, anim ? 220 : 0, false);
}

static void alarm_collapse_card(uint8_t idx, bool anim)
{
    alarm_animate_card_offset(idx, 0, anim ? 240 : 0, true);
}

static void alarm_close_expanded_card(bool anim)
{
    if(g_alarm_state.expanded_card_idx < 0 || g_alarm_state.expanded_card_idx >= g_alarm_state.count) {
        g_alarm_state.expanded_card_idx = -1;
        return;
    }
    alarm_collapse_card((uint8_t)g_alarm_state.expanded_card_idx, anim);
}

static void alarm_delete_anim_completed_cb(lv_anim_t * anim)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_anim_get_user_data(anim);
    alarm_delete_item(idx);
}

static void alarm_start_delete_anim(uint8_t idx)
{
    if(idx >= g_alarm_state.count) {
        return;
    }

    alarm_card_view_t * card = &g_alarm_view.cards[idx];
    if(!card->row || !card->card) {
        return;
    }

    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
    alarm_refresh_swipe_back_state();

    lv_anim_del(card, (lv_anim_exec_xcb_t)alarm_anim_set_card_offset);
    lv_obj_add_flag(card->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card->delete_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card->card, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t height_anim;
    lv_anim_init(&height_anim);
    lv_anim_set_var(&height_anim, card->row);
    lv_anim_set_values(&height_anim, ALARM_CARD_HEIGHT, 0);
    lv_anim_set_time(&height_anim, 220);
    lv_anim_set_exec_cb(&height_anim, (lv_anim_exec_xcb_t)alarm_anim_set_height);
    lv_anim_set_completed_cb(&height_anim, alarm_delete_anim_completed_cb);
    lv_anim_set_user_data(&height_anim, (void *)(uintptr_t)idx);
    lv_anim_set_path_cb(&height_anim, lv_anim_path_ease_in);
    lv_anim_start(&height_anim);

    lv_anim_t card_opa_anim;
    lv_anim_init(&card_opa_anim);
    lv_anim_set_var(&card_opa_anim, card->card);
    lv_anim_set_values(&card_opa_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&card_opa_anim, 180);
    lv_anim_set_exec_cb(&card_opa_anim, (lv_anim_exec_xcb_t)alarm_anim_set_opa);
    lv_anim_set_path_cb(&card_opa_anim, lv_anim_path_ease_in);
    lv_anim_start(&card_opa_anim);

    lv_anim_t delete_opa_anim;
    lv_anim_init(&delete_opa_anim);
    lv_anim_set_var(&delete_opa_anim, card->delete_btn);
    lv_anim_set_values(&delete_opa_anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&delete_opa_anim, 180);
    lv_anim_set_exec_cb(&delete_opa_anim, (lv_anim_exec_xcb_t)alarm_anim_set_opa);
    lv_anim_set_path_cb(&delete_opa_anim, lv_anim_path_ease_in);
    lv_anim_start(&delete_opa_anim);
}

static void alarm_refresh_card(uint8_t idx)
{
    if(idx >= g_alarm_state.count) {
        return;
    }

    alarm_card_view_t * card = &g_alarm_view.cards[idx];
    const alarm_item_t * item = &g_alarm_state.items[idx];

    if(!card->card) {
        return;
    }

    char time_buf[8];
    alarm_format_hhmm(time_buf, sizeof(time_buf), item->hour, item->minute);
    if(card->time_label) {
        lv_label_set_text(card->time_label, time_buf);
        lv_obj_set_style_text_color(card->time_label, item->on ? ALARM_CARD_ON_TEXT : ALARM_CARD_OFF_TEXT, 0);
        lv_obj_set_style_text_opa(card->time_label, item->on ? LV_OPA_COVER : alarm_pct_opa(52), 0);
    }

    if(card->card) {
        lv_obj_set_style_bg_color(card->card, item->on ? ALARM_AMBER : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card->card, item->on ? alarm_pct_opa(14) : alarm_pct_opa(4), 0);
        lv_obj_set_style_border_color(card->card, item->on ? ALARM_AMBER : lv_color_white(), 0);
        lv_obj_set_style_border_opa(card->card, item->on ? alarm_pct_opa(28) : alarm_pct_opa(7), 0);
    }

    alarm_set_hidden(card->accent, !item->on);

    if(card->toggle) {
        if(item->on) {
            lv_obj_add_state(card->toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(card->toggle, LV_STATE_CHECKED);
        }

        lv_obj_set_style_bg_color(card->toggle, item->on ? ALARM_AMBER : lv_color_white(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(card->toggle,
                                item->on ? LV_OPA_COVER : alarm_pct_opa(10),
                                LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(card->toggle, item->on ? ALARM_AMBER : lv_color_white(), LV_PART_INDICATOR | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(card->toggle,
                                item->on ? LV_OPA_COVER : alarm_pct_opa(10),
                                LV_PART_INDICATOR | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(card->toggle, lv_color_white(), LV_PART_KNOB | LV_STATE_PRESSED);
    }
}

static void alarm_create_alarm_card(lv_obj_t * parent, uint8_t idx)
{
    alarm_card_view_t * card = &g_alarm_view.cards[idx];

    card->row = alarm_create_clean_container(parent);
    lv_obj_set_size(card->row, ALARM_CARD_WIDTH, ALARM_CARD_HEIGHT);
    lv_obj_set_style_pad_all(card->row, 0, 0);

    card->delete_slot = alarm_create_clean_container(card->row);
    lv_obj_set_size(card->delete_slot, ALARM_CARD_WIDTH + ALARM_CARD_DELETE_REVEAL, ALARM_CARD_HEIGHT);
    lv_obj_set_pos(card->delete_slot, 0, 0);
    lv_obj_set_style_pad_all(card->delete_slot, 0, 0);

    card->delete_btn = alarm_create_clean_button(card->delete_slot);
    lv_obj_set_size(card->delete_btn, ALARM_CARD_DELETE_BTN_WIDTH, ALARM_CARD_DELETE_BTN_HEIGHT);
    lv_obj_set_style_radius(card->delete_btn, 18, 0);
    lv_obj_set_style_bg_color(card->delete_btn, ALARM_RED, 0);
    lv_obj_set_style_bg_opa(card->delete_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card->delete_btn, 1, 0);
    lv_obj_set_style_border_color(card->delete_btn, ALARM_RED, 0);
    lv_obj_set_style_border_opa(card->delete_btn, alarm_pct_opa(72), 0);
    lv_obj_set_style_shadow_width(card->delete_btn, 0, 0);
    lv_obj_add_event_cb(card->delete_btn, alarm_card_delete_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);
    alarm_enable_press_glow(card->delete_btn, ALARM_RED);

    card->delete_icon = alarm_create_label(card->delete_btn, LV_SYMBOL_TRASH, &lv_font_montserrat_14, lv_color_white(), LV_OPA_COVER);
    lv_obj_center(card->delete_icon);
    alarm_make_passthrough(card->delete_icon);

    card->card = alarm_create_clean_container(card->row);
    lv_obj_set_size(card->card, ALARM_CARD_WIDTH, ALARM_CARD_HEIGHT);
    lv_obj_set_style_radius(card->card, 18, 0);
    lv_obj_set_style_border_width(card->card, 1, 0);
    lv_obj_set_style_pad_hor(card->card, 16, 0);
    lv_obj_set_style_pad_ver(card->card, 10, 0);
    lv_obj_set_style_pad_gap(card->card, 10, 0);
    lv_obj_set_flex_flow(card->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card->card, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(card->card, alarm_card_swipe_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(card->card, alarm_card_swipe_event_cb, LV_EVENT_PRESSING, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(card->card, alarm_card_swipe_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(card->card, alarm_card_swipe_event_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(card->card, alarm_card_swipe_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    card->accent = alarm_create_clean_container(card->card);
    lv_obj_set_size(card->accent, 3, LV_PCT(100));
    lv_obj_set_style_radius(card->accent, 2, 0);
    lv_obj_set_style_bg_color(card->accent, ALARM_AMBER, 0);
    lv_obj_set_style_bg_opa(card->accent, LV_OPA_COVER, 0);
    lv_obj_add_flag(card->accent, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * texts = alarm_create_clean_container(card->card);
    lv_obj_set_size(texts, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    /* 时间文本列保持左上对齐，和原始卡片视觉一致。 */
    lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_clear_flag(texts, LV_OBJ_FLAG_CLICKABLE);

    card->time_label = alarm_create_label(texts, "00:00", &lv_font_montserrat_28, lv_color_white(), LV_OPA_COVER);
    alarm_make_passthrough(card->time_label);

    card->toggle = lv_switch_create(card->card);
    lv_obj_set_size(card->toggle, 42, 24);
    lv_obj_set_ext_click_area(card->toggle, 12);
    lv_obj_set_style_radius(card->toggle, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->toggle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->toggle, alarm_pct_opa(6), LV_PART_MAIN);
    lv_obj_set_style_border_width(card->toggle, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(card->toggle, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card->toggle, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card->toggle, alarm_pct_opa(12), LV_PART_MAIN);
    lv_obj_set_style_pad_all(card->toggle, 2, 0);
    lv_obj_set_style_radius(card->toggle, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_add_event_cb(card->toggle, alarm_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)idx);
    alarm_enable_press_glow(card->toggle, ALARM_AMBER);

    alarm_update_delete_btn_position(card);
    alarm_refresh_card(idx);
}

static void alarm_rebuild_list(void)
{
    if(!g_alarm_view.list_cont) {
        return;
    }

    alarm_cancel_card_animations();
    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
    memset(g_alarm_view.cards, 0, sizeof(g_alarm_view.cards));

    int32_t child_count = lv_obj_get_child_count(g_alarm_view.list_cont);
    for(int32_t i = child_count - 1; i >= 0; --i) {
        lv_obj_delete(lv_obj_get_child(g_alarm_view.list_cont, i));
    }

    for(uint8_t i = 0; i < g_alarm_state.count; ++i) {
        alarm_create_alarm_card(g_alarm_view.list_cont, i);
    }

    if(g_alarm_state.count == 0) {
        lv_obj_t * empty = alarm_create_clean_container(g_alarm_view.list_cont);
        lv_obj_set_size(empty, LV_PCT(100), 96);
        lv_obj_set_style_pad_gap(empty, 7, 0);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t * icon = alarm_create_label(empty,
                                             LV_SYMBOL_BELL,
                                             &lv_font_montserrat_14,
                                             lv_color_white(),
                                             alarm_pct_opa(24));
        lv_obj_t * label = alarm_create_label(empty,
                                              "暂无闹钟",
                                              ui_builtin_text_font(),
                                              lv_color_white(),
                                              alarm_pct_opa(42));
        alarm_make_passthrough(icon);
        alarm_make_passthrough(label);
    }

    app_swipe_back_refresh_subtree(g_alarm_view.list_cont);
    alarm_refresh_swipe_back_state();
    alarm_refresh_next_hint();
}

/* ----------------------------- 编辑面板 ----------------------------- */

static void alarm_open_edit_panel(bool is_new, uint8_t idx)
{
    alarm_close_expanded_card(false);

    if(!g_alarm_view.edit.panel) {
        return;
    }
    if(is_new && g_alarm_state.count >= ALARM_MAX_ITEMS) {
        smartwatch_ui_runtime_show_notification("闹钟已达上限", 2500);
        return;
    }
    if(!is_new && idx >= g_alarm_state.count) {
        return;
    }

    g_alarm_state.editing_new = is_new;
    g_alarm_state.editing_idx = idx;

    if(is_new) {
        lv_label_set_text(g_alarm_view.edit.title, "新建闹钟");
        lv_roller_set_selected(g_alarm_view.edit.roller_hour, 7, LV_ANIM_OFF);
        lv_roller_set_selected(g_alarm_view.edit.roller_min, 0, LV_ANIM_OFF);
        alarm_set_hidden(g_alarm_view.edit.delete_btn, true);
    } else {
        const alarm_item_t * item = &g_alarm_state.items[idx];
        lv_label_set_text(g_alarm_view.edit.title, "编辑闹钟");
        lv_roller_set_selected(g_alarm_view.edit.roller_hour, item->hour, LV_ANIM_OFF);
        lv_roller_set_selected(g_alarm_view.edit.roller_min, item->minute, LV_ANIM_OFF);
        alarm_set_hidden(g_alarm_view.edit.delete_btn, false);
    }

    alarm_set_hidden(g_alarm_view.edit.panel, false);
    lv_obj_move_foreground(g_alarm_view.edit.panel);
}

static void alarm_close_edit_panel(void)
{
    alarm_set_hidden(g_alarm_view.edit.panel, true);
}

static void alarm_save_edit(void)
{
    uint16_t hour = lv_roller_get_selected(g_alarm_view.edit.roller_hour);
    uint16_t minute = lv_roller_get_selected(g_alarm_view.edit.roller_min);

    if(g_alarm_state.editing_new) {
        alarm_add_item((uint8_t)hour, (uint8_t)minute);
    } else if(g_alarm_state.editing_idx < g_alarm_state.count) {
        alarm_item_t * item = &g_alarm_state.items[g_alarm_state.editing_idx];
        if(item->hour != (uint8_t)hour || item->minute != (uint8_t)minute) {
            item->hour = (uint8_t)hour;
            item->minute = (uint8_t)minute;
            alarm_save_item(g_alarm_state.editing_idx);
        }
        alarm_refresh_card(g_alarm_state.editing_idx);
        alarm_refresh_next_hint();
    }

    alarm_close_edit_panel();
}

/* ----------------------------- Tab 切换 ----------------------------- */

static void alarm_apply_tab_style(void)
{
    bool show_alarm_tab = (g_alarm_state.tab == 0);

    alarm_close_expanded_card(false);

    if(g_alarm_view.tab_alarm.btn) {
        lv_obj_set_style_bg_color(g_alarm_view.tab_alarm.btn, ALARM_AMBER, 0);
        lv_obj_set_style_bg_opa(g_alarm_view.tab_alarm.btn,
                                show_alarm_tab ? LV_OPA_COVER : LV_OPA_TRANSP,
                                0);
        lv_obj_set_style_text_color(g_alarm_view.tab_alarm.icon,
                                    show_alarm_tab ? ALARM_TAB_DARK_TEXT : lv_color_white(),
                                    0);
        lv_obj_set_style_text_opa(g_alarm_view.tab_alarm.icon,
                                  show_alarm_tab ? LV_OPA_COVER : alarm_pct_opa(40),
                                  0);
        lv_obj_set_style_text_color(g_alarm_view.tab_alarm.label,
                                    show_alarm_tab ? ALARM_TAB_DARK_TEXT : lv_color_white(),
                                    0);
        lv_obj_set_style_text_opa(g_alarm_view.tab_alarm.label,
                                  show_alarm_tab ? LV_OPA_COVER : alarm_pct_opa(40),
                                  0);
    }

    if(g_alarm_view.tab_timer.btn) {
        lv_obj_set_style_bg_color(g_alarm_view.tab_timer.btn, ALARM_TEAL, 0);
        lv_obj_set_style_bg_opa(g_alarm_view.tab_timer.btn,
                                show_alarm_tab ? LV_OPA_TRANSP : LV_OPA_COVER,
                                0);
        lv_obj_set_style_text_color(g_alarm_view.tab_timer.icon,
                                    show_alarm_tab ? lv_color_white() : ALARM_TIMER_DARK_TEXT,
                                    0);
        lv_obj_set_style_text_opa(g_alarm_view.tab_timer.icon,
                                  show_alarm_tab ? alarm_pct_opa(40) : LV_OPA_COVER,
                                  0);
        lv_obj_set_style_text_color(g_alarm_view.tab_timer.label,
                                    show_alarm_tab ? lv_color_white() : ALARM_TIMER_DARK_TEXT,
                                    0);
        lv_obj_set_style_text_opa(g_alarm_view.tab_timer.label,
                                  show_alarm_tab ? alarm_pct_opa(40) : LV_OPA_COVER,
                                  0);
    }

    alarm_set_hidden(g_alarm_view.alarm_cont, !show_alarm_tab);
    alarm_set_hidden(g_alarm_view.timer_cont, show_alarm_tab);
    alarm_set_hidden(g_alarm_view.fab_btn, !show_alarm_tab);
    alarm_close_edit_panel();

    if(g_alarm_view.cd.arc && g_alarm_state.cd_state != CD_DONE) {
        lv_obj_set_style_arc_color(g_alarm_view.cd.arc, ALARM_TEAL, LV_PART_INDICATOR);
    }
}

/* ----------------------------- 事件回调 ----------------------------- */

static void alarm_tab_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    g_alarm_state.tab = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    alarm_apply_tab_style();
}

static void alarm_fab_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    alarm_open_edit_panel(true, 0);
}

static void alarm_card_click_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    alarm_open_edit_panel(false, (uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

static void alarm_card_swipe_event_cb(lv_event_t * e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(idx >= g_alarm_state.count) {
        return;
    }

    alarm_card_view_t * card = &g_alarm_view.cards[idx];
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_PRESSED) {
        card->press_started = true;
        card->swiping = false;
        card->moved = false;
        lv_anim_del(card, (lv_anim_exec_xcb_t)alarm_anim_set_card_offset);

        if(g_alarm_state.expanded_card_idx >= 0 && g_alarm_state.expanded_card_idx != (int16_t)idx) {
            alarm_close_expanded_card(true);
            g_alarm_state.suppress_click_idx = (int16_t)idx;
        }
        return;
    }

    if(code == LV_EVENT_PRESSING) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if(!indev) {
            return;
        }

        lv_point_t vect = { 0, 0 };
        lv_indev_get_vect(indev, &vect);

        if(!card->swiping) {
            if(LV_ABS(vect.x) < 2) {
                return;
            }
            if(LV_ABS(vect.x) <= LV_ABS(vect.y) && alarm_card_get_offset(card) == 0) {
                return;
            }
            card->swiping = true;
            alarm_refresh_swipe_back_state();
        }

        int32_t next_offset = alarm_card_get_offset(card) + vect.x;
        alarm_card_set_offset(card, next_offset);
        if(LV_ABS(alarm_card_get_offset(card)) > 4 || LV_ABS(vect.x) > 4) {
            card->moved = true;
        }
        return;
    }

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if(!card->press_started) {
            return;
        }

        card->press_started = false;
        if(!card->swiping) {
            card->moved = false;
            return;
        }

        card->swiping = false;
        alarm_refresh_swipe_back_state();
        if(card->moved) {
            g_alarm_state.suppress_click_idx = (int16_t)idx;
        }

        if(-alarm_card_get_offset(card) >= ALARM_CARD_SWIPE_THRESHOLD) {
            alarm_expand_card(idx, true);
        } else {
            alarm_collapse_card(idx, true);
        }

        card->moved = false;
        return;
    }

    if(code == LV_EVENT_CLICKED) {
        if(g_alarm_state.suppress_click_idx == (int16_t)idx) {
            g_alarm_state.suppress_click_idx = -1;
            return;
        }

        if(g_alarm_state.expanded_card_idx == (int16_t)idx) {
            alarm_collapse_card(idx, true);
            return;
        }

        alarm_card_click_cb(e);
    }
}

static void alarm_toggle_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(idx >= g_alarm_state.count) {
        return;
    }

    if(g_alarm_state.expanded_card_idx >= 0 && g_alarm_state.expanded_card_idx != (int16_t)idx) {
        alarm_close_expanded_card(true);
    }

    lv_obj_t * toggle = lv_event_get_target(e);
    g_alarm_state.items[idx].on = toggle && lv_obj_has_state(toggle, LV_STATE_CHECKED);
    alarm_refresh_card(idx);
    alarm_refresh_next_hint();
    alarm_save_item(idx);
}

static void alarm_card_delete_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    alarm_start_delete_anim(idx);
}

static void alarm_blank_area_click_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if(g_alarm_state.expanded_card_idx < 0 || g_alarm_state.expanded_card_idx >= g_alarm_state.count) {
        return;
    }

    lv_obj_t * target = lv_event_get_target(e);
    alarm_card_view_t * expanded = &g_alarm_view.cards[g_alarm_state.expanded_card_idx];
    if(!expanded->row) {
        g_alarm_state.expanded_card_idx = -1;
        return;
    }

    if(!alarm_obj_is_descendant(target, expanded->row)) {
        alarm_close_expanded_card(true);
    }
}

static void alarm_edit_close_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_close_edit_panel();
    }
}

static void alarm_edit_delete_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    alarm_delete_item(g_alarm_state.editing_idx);
    alarm_close_edit_panel();
}

static void alarm_edit_ok_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        alarm_save_edit();
    }
}

static void alarm_alert_left_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if(g_alarm_state.alert_kind == ALARM_ALERT_ALARM) {
        alarm_stop_ring();
        alarm_hide_alert_popup();
        return;
    }

    if(g_alarm_state.alert_kind == ALARM_ALERT_COUNTDOWN) {
        alarm_stop_ring();
        alarm_hide_alert_popup();

        if(g_alarm_state.cd_total > 0) {
            g_alarm_state.cd_state = CD_RUNNING;
            alarm_start_countdown_deadline((uint32_t)g_alarm_state.cd_total * 1000U);
        } else {
            g_alarm_state.cd_seconds = 0;
            g_alarm_state.cd_state = CD_IDLE;
            g_alarm_state.cd_remaining_ms = 0;
            g_alarm_state.cd_deadline_tick = 0;
        }

        alarm_apply_countdown_state();
    }
}

static void alarm_alert_right_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if(g_alarm_state.alert_kind == ALARM_ALERT_ALARM) {
        alarm_stop_ring();
        alarm_schedule_snooze_10min();
        alarm_hide_alert_popup();
        alarm_refresh_next_hint();
        smartwatch_ui_runtime_show_notification("10分钟后再次提醒", 2500);
        return;
    }

    if(g_alarm_state.alert_kind == ALARM_ALERT_COUNTDOWN) {
        alarm_stop_ring();
        alarm_hide_alert_popup();
        g_alarm_state.cd_total = 0;
        alarm_reset_countdown(true);
    }
}

static void alarm_cd_start_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    uint16_t minutes = lv_roller_get_selected(g_alarm_view.cd.roller_min);
    uint16_t seconds = lv_roller_get_selected(g_alarm_view.cd.roller_sec);
    int32_t total = (int32_t)minutes * 60 + (int32_t)seconds;
    if(total <= 0) {
        return;
    }

    g_alarm_state.cd_total = total;
    g_alarm_state.cd_state = CD_RUNNING;
    alarm_start_countdown_deadline((uint32_t)total * 1000U);
    alarm_apply_countdown_state();
}

static void alarm_cd_toggle_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if(g_alarm_state.cd_state == CD_RUNNING) {
        alarm_update_countdown_from_clock();
        if(g_alarm_state.cd_state != CD_RUNNING) {
            alarm_apply_countdown_state();
            return;
        }
        g_alarm_state.cd_state = CD_PAUSED;
        g_alarm_state.cd_deadline_tick = 0;
    } else if(g_alarm_state.cd_state == CD_PAUSED) {
        g_alarm_state.cd_state = CD_RUNNING;
        alarm_start_countdown_deadline(g_alarm_state.cd_remaining_ms);
    }

    alarm_apply_countdown_state();
}

static void alarm_cd_reset_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    alarm_stop_ring();
    alarm_hide_alert_popup();
    g_alarm_state.cd_total = 0;
    alarm_reset_countdown(true);
}

/* ----------------------------- UI 小部件 ----------------------------- */

static lv_obj_t * alarm_create_roller(lv_obj_t * parent,
                                      const char * options,
                                      uint16_t selected,
                                      lv_color_t active_color)
{
    lv_obj_t * roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(roller, 5);
    lv_obj_set_style_width(roller, 68, 0);
    /* 保留五行数字的原始显示密度，闹钟和倒计时选择器保持一致。 */
    lv_obj_set_style_height(roller, 200, 0);
    lv_obj_set_style_bg_color(roller, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(roller, alarm_pct_opa(6), 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_radius(roller, 8, 0);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(roller, lv_color_white(), 0);
    lv_obj_set_style_text_opa(roller, alarm_pct_opa(22), 0);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_24, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, active_color, LV_PART_SELECTED);
    lv_obj_set_style_text_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, active_color, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, alarm_pct_opa(18), LV_PART_SELECTED);
    lv_obj_set_style_border_side(roller, LV_BORDER_SIDE_NONE, LV_PART_SELECTED);
    lv_obj_set_style_border_width(roller, 0, LV_PART_SELECTED);
    return roller;
}

static lv_obj_t * alarm_create_dots(lv_obj_t * parent, lv_color_t color)
{
    lv_obj_t * dots = alarm_create_clean_container(parent);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(dots, 12, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(dots, 4, 0);

    for(uint8_t i = 0; i < 2; ++i) {
        lv_obj_t * dot = alarm_create_clean_container(dots);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, color, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }

    return dots;
}

static void alarm_enable_press_glow(lv_obj_t * obj, lv_color_t color)
{
    if(!obj) {
        return;
    }

    lv_obj_set_style_outline_width(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_outline_pad(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_outline_color(obj, color, LV_STATE_PRESSED);
    lv_obj_set_style_outline_opa(obj, alarm_pct_opa(72), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(obj, 10, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_spread(obj, 1, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(obj, color, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(obj, alarm_pct_opa(38), LV_STATE_PRESSED);

    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(parent) {
        lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }
}

static lv_obj_t * alarm_create_circle_button(lv_obj_t * parent,
                                             int32_t size,
                                             lv_color_t bg_color,
                                             lv_opa_t bg_opa,
                                             lv_color_t border_color,
                                             lv_opa_t border_opa)
{
    lv_obj_t * btn = alarm_create_clean_button(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, bg_opa, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, border_color, 0);
    lv_obj_set_style_border_opa(btn, border_opa, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    alarm_enable_press_glow(btn, border_color);
    return btn;
}

static void alarm_build_tab_button(lv_obj_t * parent,
                                   alarm_tab_btn_view_t * view,
                                   const char * icon_text,
                                   const char * text,
                                   uintptr_t user_data)
{
    view->btn = alarm_create_clean_button(parent);
    lv_obj_set_size(view->btn, 90, 32);
    lv_obj_set_ext_click_area(view->btn, 15);
    lv_obj_set_style_radius(view->btn, 999, 0);
    lv_obj_set_style_pad_hor(view->btn, 12, 0);
    lv_obj_set_style_pad_ver(view->btn, 8, 0);
    lv_obj_set_style_pad_gap(view->btn, 4, 0);
    lv_obj_set_flex_flow(view->btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(view->btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(view->btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(view->btn, alarm_tab_event_cb, LV_EVENT_CLICKED, (void *)user_data);
    alarm_enable_press_glow(view->btn, user_data == 0 ? ALARM_AMBER : ALARM_TEAL);

    view->icon = alarm_create_label(view->btn, icon_text, LV_FONT_DEFAULT, lv_color_white(), LV_OPA_COVER);
    view->label = alarm_create_label(view->btn, text, ui_builtin_text_font(), lv_color_white(), LV_OPA_COVER);
    alarm_make_passthrough(view->icon);
    alarm_make_passthrough(view->label);
}

/* ----------------------------- UI 组装 ----------------------------- */

static void alarm_build_header(void)
{
    g_alarm_view.header = alarm_create_clean_container(ui_AlarmScreen);
    lv_obj_set_size(g_alarm_view.header, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(g_alarm_view.header, LV_ALIGN_TOP_MID, 0, ALARM_PANEL_TOP_Y);
    lv_obj_set_style_pad_gap(g_alarm_view.header, 0, 0);
    lv_obj_set_flex_flow(g_alarm_view.header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_alarm_view.header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * tabs = alarm_create_clean_container(g_alarm_view.header);
    lv_obj_set_size(tabs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(tabs, 999, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tabs, alarm_pct_opa(6), 0);
    lv_obj_set_style_pad_all(tabs, 3, 0);
    lv_obj_set_style_pad_gap(tabs, 0, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    alarm_build_tab_button(tabs, &g_alarm_view.tab_alarm, LV_SYMBOL_BELL, "闹钟", 0);
    alarm_build_tab_button(tabs, &g_alarm_view.tab_timer, LV_SYMBOL_SHUFFLE, "倒计时", 1);
}

static void alarm_build_alarm_content(void)
{
    g_alarm_view.alarm_cont = alarm_create_clean_container(ui_AlarmScreen);
    lv_obj_set_size(g_alarm_view.alarm_cont, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_alarm_view.alarm_cont, 0, 0);
    lv_obj_set_style_pad_all(g_alarm_view.alarm_cont, 0, 0);
    lv_obj_add_flag(g_alarm_view.alarm_cont, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g_alarm_view.alarm_cont, alarm_blank_area_click_cb, LV_EVENT_CLICKED, NULL);

    g_alarm_view.next_pill = alarm_create_clean_container(g_alarm_view.alarm_cont);
    lv_obj_set_size(g_alarm_view.next_pill, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_radius(g_alarm_view.next_pill, 13, 0);
    lv_obj_set_style_bg_color(g_alarm_view.next_pill, ALARM_AMBER, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.next_pill, alarm_pct_opa(8), 0);
    lv_obj_set_style_border_width(g_alarm_view.next_pill, 1, 0);
    lv_obj_set_style_border_color(g_alarm_view.next_pill, ALARM_AMBER, 0);
    lv_obj_set_style_border_opa(g_alarm_view.next_pill, alarm_pct_opa(19), 0);
    lv_obj_set_style_pad_hor(g_alarm_view.next_pill, 10, 0);
    lv_obj_set_style_pad_gap(g_alarm_view.next_pill, 5, 0);
    lv_obj_set_flex_flow(g_alarm_view.next_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_alarm_view.next_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(g_alarm_view.next_pill, LV_ALIGN_TOP_MID, 0, 93);

    g_alarm_view.next_dot = alarm_create_clean_container(g_alarm_view.next_pill);
    lv_obj_set_size(g_alarm_view.next_dot, 5, 5);
    lv_obj_set_style_radius(g_alarm_view.next_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_alarm_view.next_dot, ALARM_AMBER, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.next_dot, LV_OPA_COVER, 0);

    g_alarm_view.next_label = alarm_create_label(g_alarm_view.next_pill, "", ui_builtin_text_font(), ALARM_CARD_ON_TEXT, alarm_pct_opa(75));

    g_alarm_view.list_cont = alarm_create_clean_container(g_alarm_view.alarm_cont);
    /* 54px 卡片 x 3 + 两个 8px 间距，完整显示三条且不覆盖新增按钮。 */
    lv_obj_set_size(g_alarm_view.list_cont, LV_PCT(100), 180);
    lv_obj_align(g_alarm_view.list_cont, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_pad_hor(g_alarm_view.list_cont, 28, 0);
    lv_obj_set_style_pad_gap(g_alarm_view.list_cont, 8, 0);
    lv_obj_set_flex_flow(g_alarm_view.list_cont, LV_FLEX_FLOW_COLUMN);
    /* 闹钟列表需要从顶部开始排布，只有 1 个卡片时也应贴近上方显示。 */
    lv_obj_set_flex_align(g_alarm_view.list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(g_alarm_view.list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_alarm_view.list_cont, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(g_alarm_view.list_cont, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(g_alarm_view.list_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(g_alarm_view.list_cont, alarm_blank_area_click_cb, LV_EVENT_CLICKED, NULL);

    g_alarm_view.fab_btn = alarm_create_circle_button(ui_AlarmScreen,
                                                      44,
                                                      ALARM_AMBER,
                                                      LV_OPA_COVER,
                                                      ALARM_AMBER,
                                                      alarm_pct_opa(60));
    lv_obj_align(g_alarm_view.fab_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(g_alarm_view.fab_btn, alarm_fab_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * fab_icon = alarm_create_label(g_alarm_view.fab_btn, LV_SYMBOL_PLUS, &lv_font_montserrat_24, lv_color_hex(0x1a0a00), LV_OPA_COVER);
    lv_obj_center(fab_icon);
    alarm_make_passthrough(fab_icon);
}

static void alarm_build_timer_content(void)
{
    g_alarm_view.timer_cont = alarm_create_clean_container(ui_AlarmScreen);
    lv_obj_set_size(g_alarm_view.timer_cont, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_alarm_view.timer_cont, 0, 0);
    lv_obj_set_style_pad_all(g_alarm_view.timer_cont, 0, 0);
    lv_obj_add_flag(g_alarm_view.timer_cont, LV_OBJ_FLAG_GESTURE_BUBBLE);

    g_alarm_view.cd.idle_cont = alarm_create_clean_container(g_alarm_view.timer_cont);
    lv_obj_set_size(g_alarm_view.cd.idle_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_alarm_view.cd.idle_cont, 0, 0);

    lv_obj_t * pickers_row = alarm_create_clean_container(g_alarm_view.cd.idle_cont);
    lv_obj_set_size(pickers_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(pickers_row, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_pad_gap(pickers_row, 8, 0);
    lv_obj_set_flex_flow(pickers_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pickers_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_alarm_view.cd.roller_min = alarm_create_roller(pickers_row, ALARM_MIN_SEC_OPTIONS_60, 5, ALARM_TEAL);
    alarm_create_dots(pickers_row, ALARM_TEAL);
    g_alarm_view.cd.roller_sec = alarm_create_roller(pickers_row, ALARM_MIN_SEC_OPTIONS_60, 0, ALARM_TEAL);

    g_alarm_view.cd.start_btn = alarm_create_clean_button(g_alarm_view.cd.idle_cont);
    lv_obj_set_size(g_alarm_view.cd.start_btn, 108, 38);
    lv_obj_align_to(g_alarm_view.cd.start_btn, pickers_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
    lv_obj_set_style_radius(g_alarm_view.cd.start_btn, 19, 0);
    lv_obj_set_style_bg_color(g_alarm_view.cd.start_btn, ALARM_TEAL, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.cd.start_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_alarm_view.cd.start_btn, 1, 0);
    lv_obj_set_style_border_color(g_alarm_view.cd.start_btn, ALARM_TEAL, 0);
    lv_obj_set_style_border_opa(g_alarm_view.cd.start_btn, alarm_pct_opa(53), 0);
    lv_obj_set_style_pad_gap(g_alarm_view.cd.start_btn, 6, 0);
    lv_obj_set_flex_flow(g_alarm_view.cd.start_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_alarm_view.cd.start_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(g_alarm_view.cd.start_btn, alarm_cd_start_cb, LV_EVENT_CLICKED, NULL);
    alarm_enable_press_glow(g_alarm_view.cd.start_btn, ALARM_TEAL);

    g_alarm_view.cd.start_icon = alarm_create_label(g_alarm_view.cd.start_btn, LV_SYMBOL_PLAY, &lv_font_montserrat_14, lv_color_white(), LV_OPA_COVER);
    g_alarm_view.cd.start_label = alarm_create_label(g_alarm_view.cd.start_btn, "开始", ui_builtin_text_font(), lv_color_white(), LV_OPA_COVER);
    alarm_make_passthrough(g_alarm_view.cd.start_icon);
    alarm_make_passthrough(g_alarm_view.cd.start_label);

    g_alarm_view.cd.running_cont = alarm_create_clean_container(g_alarm_view.timer_cont);
    lv_obj_set_size(g_alarm_view.cd.running_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(g_alarm_view.cd.running_cont, 0, 0);

    lv_obj_t * arc_cont = alarm_create_clean_container(g_alarm_view.cd.running_cont);
    lv_obj_set_size(arc_cont, ALARM_CD_ARC_SIZE, ALARM_CD_ARC_SIZE);
    lv_obj_align(arc_cont, LV_ALIGN_TOP_MID, 0, 104);

    g_alarm_view.cd.arc = lv_arc_create(arc_cont);
    lv_obj_remove_style_all(g_alarm_view.cd.arc);
    lv_obj_set_size(g_alarm_view.cd.arc, ALARM_CD_ARC_SIZE, ALARM_CD_ARC_SIZE);
    lv_arc_set_range(g_alarm_view.cd.arc, 0, 100);
    lv_arc_set_bg_angles(g_alarm_view.cd.arc, 0, 360);
    lv_arc_set_rotation(g_alarm_view.cd.arc, 270);
    lv_arc_set_value(g_alarm_view.cd.arc, 0);
    lv_obj_set_style_arc_width(g_alarm_view.cd.arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_alarm_view.cd.arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_alarm_view.cd.arc, alarm_pct_opa(22), LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_alarm_view.cd.arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_alarm_view.cd.arc, true, LV_PART_INDICATOR);
    lv_obj_center(g_alarm_view.cd.arc);
    lv_obj_clear_flag(g_alarm_view.cd.arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * text_cont = alarm_create_clean_container(arc_cont);
    lv_obj_set_size(text_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(text_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(text_cont, 3, 0);
    lv_obj_center(text_cont);

    g_alarm_view.cd.time_label = alarm_create_label(text_cont, "00:00", &lv_font_montserrat_44, lv_color_white(), LV_OPA_COVER);
    lv_obj_set_width(g_alarm_view.cd.time_label, LV_PCT(100));
    lv_obj_set_style_text_align(g_alarm_view.cd.time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(g_alarm_view.cd.time_label, 0, 0);

    g_alarm_view.cd.status_label = alarm_create_label(text_cont, "", ui_builtin_text_font(), lv_color_white(), alarm_pct_opa(28));
    lv_obj_set_width(g_alarm_view.cd.status_label, LV_PCT(100));
    lv_obj_set_style_text_align(g_alarm_view.cd.status_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t * controls = alarm_create_clean_container(g_alarm_view.cd.running_cont);
    lv_obj_set_size(controls, 112, 48);
    lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_style_pad_gap(controls, 14, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_alarm_view.cd.reset_btn = alarm_create_circle_button(controls,
                                                           36,
                                                           lv_color_white(),
                                                           alarm_pct_opa(7),
                                                           lv_color_white(),
                                                           alarm_pct_opa(10));
    lv_obj_add_event_cb(g_alarm_view.cd.reset_btn, alarm_cd_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * reset_icon = alarm_create_label(g_alarm_view.cd.reset_btn, LV_SYMBOL_REFRESH, &lv_font_montserrat_14, lv_color_white(), alarm_pct_opa(60));
    lv_obj_center(reset_icon);
    alarm_make_passthrough(reset_icon);

    g_alarm_view.cd.play_btn = alarm_create_circle_button(controls,
                                                          48,
                                                          ALARM_TEAL,
                                                          LV_OPA_COVER,
                                                          ALARM_TEAL,
                                                          alarm_pct_opa(47));
    lv_obj_add_event_cb(g_alarm_view.cd.play_btn, alarm_cd_toggle_cb, LV_EVENT_CLICKED, NULL);
    g_alarm_view.cd.play_icon = alarm_create_label(g_alarm_view.cd.play_btn, LV_SYMBOL_PLAY, &lv_font_montserrat_24, lv_color_white(), LV_OPA_COVER);
    lv_obj_center(g_alarm_view.cd.play_icon);
    alarm_make_passthrough(g_alarm_view.cd.play_icon);

    g_alarm_view.cd.total_label = alarm_create_label(g_alarm_view.cd.running_cont, "", ui_builtin_text_font(), lv_color_white(), alarm_pct_opa(20));
    lv_obj_align(g_alarm_view.cd.total_label, LV_ALIGN_TOP_MID, 0, 320);
}

static void alarm_build_edit_panel(void)
{
    g_alarm_view.edit.panel = alarm_create_clean_container(ui_AlarmScreen);
    lv_obj_set_size(g_alarm_view.edit.panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(g_alarm_view.edit.panel, ALARM_BG, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.edit.panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_alarm_view.edit.panel, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t * header = alarm_create_clean_container(g_alarm_view.edit.panel);
    lv_obj_set_size(header, LV_PCT(78), LV_SIZE_CONTENT);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, ALARM_PANEL_TOP_Y);
    lv_obj_set_style_pad_left(header, 30, 0);
    lv_obj_set_style_pad_right(header, 30, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_alarm_view.edit.close_btn = alarm_create_circle_button(header,
                                                             34,
                                                             lv_color_white(),
                                                             alarm_pct_opa(8),
                                                             lv_color_white(),
                                                             LV_OPA_TRANSP);
    lv_obj_add_event_cb(g_alarm_view.edit.close_btn, alarm_edit_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * close_icon = alarm_create_label(g_alarm_view.edit.close_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, lv_color_white(), alarm_pct_opa(70));
    lv_obj_center(close_icon);
    alarm_make_passthrough(close_icon);

    lv_obj_t * title_cont = alarm_create_clean_container(header);
    lv_obj_set_size(title_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(title_cont, 999, 0);
    lv_obj_set_style_bg_color(title_cont, ALARM_AMBER, 0);
    lv_obj_set_style_bg_opa(title_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(title_cont, 16, 0);
    lv_obj_set_style_pad_ver(title_cont, 8, 0);

    g_alarm_view.edit.title = alarm_create_label(title_cont, "新建闹钟", ui_builtin_text_font(), ALARM_TAB_DARK_TEXT, LV_OPA_COVER);
    lv_obj_center(g_alarm_view.edit.title);

    lv_obj_t * spacer = alarm_create_clean_container(header);
    lv_obj_set_size(spacer, 34, 1);

    lv_obj_t * time_cont = alarm_create_clean_container(g_alarm_view.edit.panel);
    lv_obj_set_size(time_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(time_cont, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_pad_gap(time_cont, 8, 0);
    lv_obj_set_flex_flow(time_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_alarm_view.edit.roller_hour = alarm_create_roller(time_cont, ALARM_HOUR_OPTIONS_24, 7, ALARM_AMBER);
    alarm_create_dots(time_cont, ALARM_AMBER);
    g_alarm_view.edit.roller_min = alarm_create_roller(time_cont, ALARM_MIN_SEC_OPTIONS_60, 0, ALARM_AMBER);

    lv_obj_t * buttons = alarm_create_clean_container(g_alarm_view.edit.panel);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_pad_gap(buttons, 48, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_alarm_view.edit.delete_btn = alarm_create_circle_button(buttons,
                                                              52,
                                                              ALARM_RED,
                                                              alarm_pct_opa(12),
                                                              ALARM_RED,
                                                              alarm_pct_opa(60));
    lv_obj_add_event_cb(g_alarm_view.edit.delete_btn, alarm_edit_delete_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * delete_icon = alarm_create_label(g_alarm_view.edit.delete_btn, LV_SYMBOL_CLOSE, &lv_font_montserrat_24, ALARM_RED, LV_OPA_COVER);
    lv_obj_center(delete_icon);
    alarm_make_passthrough(delete_icon);

    g_alarm_view.edit.ok_btn = alarm_create_circle_button(buttons,
                                                          52,
                                                          ALARM_AMBER,
                                                          LV_OPA_COVER,
                                                          ALARM_AMBER,
                                                          alarm_pct_opa(60));
    lv_obj_add_event_cb(g_alarm_view.edit.ok_btn, alarm_edit_ok_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * ok_icon = alarm_create_label(g_alarm_view.edit.ok_btn, LV_SYMBOL_OK, &lv_font_montserrat_24, lv_color_hex(0x1a0a00), LV_OPA_COVER);
    lv_obj_center(ok_icon);
    alarm_make_passthrough(ok_icon);
}

static void alarm_build_alert_overlay(void)
{
    g_alarm_view.alert.overlay = alarm_create_clean_container(ui_AlarmScreen);
    lv_obj_set_size(g_alarm_view.alert.overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(g_alarm_view.alert.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_alarm_view.alert.overlay, alarm_pct_opa(55), 0);
    lv_obj_add_flag(g_alarm_view.alert.overlay, LV_OBJ_FLAG_HIDDEN);

    g_alarm_view.alert.card = alarm_create_clean_container(g_alarm_view.alert.overlay);
    lv_obj_set_size(g_alarm_view.alert.card, 236, 144);
    lv_obj_center(g_alarm_view.alert.card);
    lv_obj_set_style_radius(g_alarm_view.alert.card, 18, 0);
    lv_obj_set_style_bg_color(g_alarm_view.alert.card, ALARM_ALERT_CARD_BG, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.alert.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_alarm_view.alert.card, 1, 0);
    lv_obj_set_style_border_color(g_alarm_view.alert.card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(g_alarm_view.alert.card, alarm_pct_opa(12), 0);
    lv_obj_set_style_shadow_width(g_alarm_view.alert.card, 18, 0);
    lv_obj_set_style_shadow_color(g_alarm_view.alert.card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(g_alarm_view.alert.card, alarm_pct_opa(35), 0);

    g_alarm_view.alert.title = alarm_create_label(g_alarm_view.alert.card, "闹钟响了", ui_builtin_text_font(), lv_color_white(), LV_OPA_COVER);
    lv_obj_align(g_alarm_view.alert.title, LV_ALIGN_TOP_MID, 0, 24);

    g_alarm_view.alert.message = alarm_create_label(g_alarm_view.alert.card, "请选择处理方式", ui_builtin_text_font(), lv_color_white(), alarm_pct_opa(60));
    lv_obj_align(g_alarm_view.alert.message, LV_ALIGN_TOP_MID, 0, 56);

    g_alarm_view.alert.left_btn = alarm_create_clean_button(g_alarm_view.alert.card);
    lv_obj_set_size(g_alarm_view.alert.left_btn, 92, 36);
    lv_obj_align(g_alarm_view.alert.left_btn, LV_ALIGN_BOTTOM_LEFT, 18, -16);
    lv_obj_set_style_radius(g_alarm_view.alert.left_btn, 12, 0);
    lv_obj_set_style_bg_color(g_alarm_view.alert.left_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g_alarm_view.alert.left_btn, alarm_pct_opa(8), 0);
    lv_obj_set_style_border_width(g_alarm_view.alert.left_btn, 1, 0);
    lv_obj_set_style_border_color(g_alarm_view.alert.left_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(g_alarm_view.alert.left_btn, alarm_pct_opa(12), 0);
    lv_obj_add_event_cb(g_alarm_view.alert.left_btn, alarm_alert_left_event_cb, LV_EVENT_CLICKED, NULL);
    alarm_enable_press_glow(g_alarm_view.alert.left_btn, lv_color_white());

    g_alarm_view.alert.left_label = alarm_create_label(g_alarm_view.alert.left_btn, "停止", ui_builtin_text_font(), lv_color_white(), alarm_pct_opa(70));
    lv_obj_center(g_alarm_view.alert.left_label);
    alarm_make_passthrough(g_alarm_view.alert.left_label);

    g_alarm_view.alert.right_btn = alarm_create_clean_button(g_alarm_view.alert.card);
    lv_obj_set_size(g_alarm_view.alert.right_btn, 92, 36);
    lv_obj_align(g_alarm_view.alert.right_btn, LV_ALIGN_BOTTOM_RIGHT, -18, -16);
    lv_obj_set_style_radius(g_alarm_view.alert.right_btn, 12, 0);
    lv_obj_set_style_bg_color(g_alarm_view.alert.right_btn, ALARM_AMBER, 0);
    lv_obj_set_style_bg_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(18), 0);
    lv_obj_set_style_border_width(g_alarm_view.alert.right_btn, 1, 0);
    lv_obj_set_style_border_color(g_alarm_view.alert.right_btn, ALARM_AMBER, 0);
    lv_obj_set_style_border_opa(g_alarm_view.alert.right_btn, alarm_pct_opa(30), 0);
    lv_obj_add_event_cb(g_alarm_view.alert.right_btn, alarm_alert_right_event_cb, LV_EVENT_CLICKED, NULL);
    alarm_enable_press_glow(g_alarm_view.alert.right_btn, ALARM_AMBER);

    g_alarm_view.alert.right_label = alarm_create_label(g_alarm_view.alert.right_btn, "稍后提醒", ui_builtin_text_font(), ALARM_CARD_ON_TEXT, LV_OPA_COVER);
    lv_obj_center(g_alarm_view.alert.right_label);
    alarm_make_passthrough(g_alarm_view.alert.right_label);
}

/* ----------------------------- 生命周期 ----------------------------- */

void ui_AlarmRuntime_init(void)
{
    if(g_alarm_state.runtime_initialized) {
        return;
    }

    memset(&g_alarm_state, 0, sizeof(g_alarm_state));
    g_alarm_state.tab = 0;
    g_alarm_state.cd_state = CD_IDLE;
    g_alarm_state.last_ring_minute_key = -1;
    g_alarm_state.last_hint_minute_key = -1;
    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
    g_alarm_state.runtime_initialized = true;

    alarm_load_items();
    g_alarm_state.alarm_generation = app_device_get_alarm_generation();
    g_alarm_state.service_timer = lv_timer_create(alarm_service_timer_cb, 1000, NULL);
}

void ui_AlarmRuntime_deinit(void)
{
    if(!g_alarm_state.runtime_initialized) {
        return;
    }

    alarm_stop_ring();
    if(g_alarm_state.service_timer) {
        lv_timer_delete(g_alarm_state.service_timer);
        g_alarm_state.service_timer = NULL;
    }

    memset(&g_alarm_state, 0, sizeof(g_alarm_state));
    g_alarm_state.last_ring_minute_key = -1;
    g_alarm_state.last_hint_minute_key = -1;
    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
}

void ui_AlarmScreen_init(void)
{
    if(ui_AlarmScreen) {
        return;
    }

    ui_AlarmRuntime_init();
    memset(&g_alarm_view, 0, sizeof(g_alarm_view));
    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
    alarm_sync_items_if_changed(false);

    ui_AlarmScreen = lv_obj_create(NULL);
    g_alarm_view.root = ui_AlarmScreen;

    lv_obj_remove_style_all(ui_AlarmScreen);
    lv_obj_set_size(ui_AlarmScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_AlarmScreen, ALARM_BG, 0);
    lv_obj_set_style_bg_opa(ui_AlarmScreen, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_AlarmScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_AlarmScreen, LV_OBJ_FLAG_SCROLLABLE);

    alarm_build_header();
    alarm_build_alarm_content();
    alarm_build_timer_content();
    lv_obj_move_foreground(g_alarm_view.header);
    alarm_build_edit_panel();
    alarm_build_alert_overlay();

    alarm_rebuild_list();
    alarm_apply_countdown_state();
    alarm_apply_tab_style();
    alarm_refresh_next_hint();

    if(g_alarm_state.ring_active && g_alarm_state.alert_kind != ALARM_ALERT_NONE) {
        alarm_show_alert_popup(g_alarm_state.alert_kind,
                               g_alarm_state.alert_kind == ALARM_ALERT_ALARM ?
                                   "闹钟响了" : "倒计时结束");
    }

    app_screen_enable_swipe_back(ui_AlarmScreen);
}

void ui_AlarmScreen_deinit(void)
{
    alarm_cancel_card_animations();
    if(ui_AlarmScreen) {
        lv_obj_delete(ui_AlarmScreen);
        ui_AlarmScreen = NULL;
    }

    memset(&g_alarm_view, 0, sizeof(g_alarm_view));
    g_alarm_state.expanded_card_idx = -1;
    g_alarm_state.suppress_click_idx = -1;
}
