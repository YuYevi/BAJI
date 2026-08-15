/**
 * @file ui_AIChatScreen.c
 * @brief AI男友对话屏幕实现
 *
 * 布局（360x360圆形屏幕）：
 *   - 全屏：MJPG动态背景
 *   - 顶部：时间、状态和情绪文案
 *   - 底部：AI回复胶囊
 * 交互：
 */
#include "ui_AIChatScreen.h"

#include "../ui.h"
#include "../app_common.h"
#include "../ui_helpers.h"
#include "../ui_media_assets.h"
#include "../ui_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

lv_obj_t * ui_AIChatScreen;

static lv_timer_t * chat_clock_timer;
static lv_obj_t * chat_mjpeg_bg;
static smartwatch_ui_runtime_mjpeg_player_t * chat_mjpeg_player;
static lv_obj_t * chat_time_label;
static lv_obj_t * chat_status_chip;
static lv_obj_t * chat_status_label;
static lv_obj_t * chat_emotion_label;
static lv_obj_t * chat_ai_capsule;
static lv_obj_t * chat_ai_label;

static char g_chat_status[96];
static char g_chat_emotion[64];
static char * g_chat_ai = NULL;
static bool g_chat_mjpeg_is_speaking;

static void chat_copy_text(char * dst, size_t size, const char * src)
{
    if(size == 0) return;
    if(!src) {
        dst[0] = '\0';
        return;
    }
    lv_snprintf(dst, size, "%s", src);
}

static void chat_replace_text(char ** dst, const char * src)
{
    if(*dst) {
        free(*dst);
        *dst = NULL;
    }

    if(!src || src[0] == '\0') {
        return;
    }

    size_t len = strlen(src);
    char * copy = (char *)malloc(len + 1);
    if(!copy) {
        return;
    }

    memcpy(copy, src, len + 1);
    *dst = copy;
}

static bool chat_is_meaningful_emotion(const char * emotion)
{
    if(!emotion || emotion[0] == '\0') return false;
    return strcmp(emotion, "neutral") != 0;
}

static const lv_font_t * chat_text_font(void)
{
    const lv_font_t * font = smartwatch_ui_runtime_get_text_font();
    return font ? font : &BUILTIN_TEXT_FONT;
}

static void chat_update_clock(void)
{
    if(!chat_time_label) return;

    time_t now = time(NULL);
    struct tm tmbuf;
    struct tm * tm_info = NULL;

#if defined(_WIN32)
    if(localtime_s(&tmbuf, &now) == 0) tm_info = &tmbuf;
#else
    tm_info = localtime(&now);
    if(tm_info) {
        tmbuf = *tm_info;
        tm_info = &tmbuf;
    }
#endif

    if(!tm_info) return;

    char time_buf[8];
    lv_snprintf(time_buf, sizeof(time_buf), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
    lv_label_set_text(chat_time_label, time_buf);
}

static void chat_clock_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    chat_update_clock();
}

static bool chat_get_status_mjpeg(bool speaking, const uint8_t ** data, size_t * size)
{
    return speaking ? ui_media_assets_get_ai_chat_speaking_mjpeg(data, size)
                    : ui_media_assets_get_ai_chat_idle_mjpeg(data, size);
}

static void chat_refresh_mjpeg_state(void)
{
    const uint8_t * mjpeg_data = NULL;
    size_t mjpeg_size = 0;

    if(!chat_mjpeg_bg || !chat_mjpeg_player) return;

    bool speaking = smartwatch_ui_runtime_is_ai_speaking();
    if(!chat_get_status_mjpeg(speaking, &mjpeg_data, &mjpeg_size) || !mjpeg_data || mjpeg_size == 0) return;

    if(g_chat_mjpeg_is_speaking == speaking) {
        return;
    }

    if(!smartwatch_ui_runtime_mjpeg_player_set_src(chat_mjpeg_player, mjpeg_data, mjpeg_size)) {
        return;
    }

    g_chat_mjpeg_is_speaking = speaking;
    smartwatch_ui_runtime_mjpeg_player_set_visible(chat_mjpeg_player,
                                                     lv_screen_active() == ui_AIChatScreen);
}

static void chat_create_mjpeg_bg(void)
{
    const uint8_t * mjpeg_data = NULL;
    size_t mjpeg_size = 0;
    bool speaking = smartwatch_ui_runtime_is_ai_speaking();
    if(!chat_get_status_mjpeg(speaking, &mjpeg_data, &mjpeg_size) || !mjpeg_data || mjpeg_size == 0) return;

    chat_mjpeg_bg = lv_image_create(ui_AIChatScreen);
    lv_obj_set_size(chat_mjpeg_bg, LV_HOR_RES, LV_VER_RES);
    lv_image_set_inner_align(chat_mjpeg_bg, LV_IMAGE_ALIGN_COVER);
    lv_image_set_antialias(chat_mjpeg_bg, true);
    lv_obj_set_pos(chat_mjpeg_bg, 0, 0);
    lv_obj_set_style_bg_opa(chat_mjpeg_bg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_mjpeg_bg, 0, 0);
    lv_obj_clear_flag(chat_mjpeg_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(chat_mjpeg_bg);
    chat_mjpeg_player = smartwatch_ui_runtime_mjpeg_player_create(chat_mjpeg_bg);
    g_chat_mjpeg_is_speaking = speaking;

    if(chat_mjpeg_player) {
        smartwatch_ui_runtime_mjpeg_player_set_src(chat_mjpeg_player, mjpeg_data, mjpeg_size);
        smartwatch_ui_runtime_mjpeg_player_set_visible(chat_mjpeg_player,
                                                         lv_screen_active() == ui_AIChatScreen);
    }
}

static void chat_refresh_status(void)
{
    if(!chat_status_chip || !chat_status_label) return;

    if(g_chat_status[0] == '\0') {
        lv_obj_add_flag(chat_status_chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_status_label, g_chat_status);
    lv_obj_clear_flag(chat_status_chip, LV_OBJ_FLAG_HIDDEN);
    chat_refresh_mjpeg_state();
}

static void chat_refresh_emotion(void)
{
    if(!chat_emotion_label) return;

    if(!chat_is_meaningful_emotion(g_chat_emotion)) {
        lv_obj_add_flag(chat_emotion_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_emotion_label, g_chat_emotion);
    lv_obj_clear_flag(chat_emotion_label, LV_OBJ_FLAG_HIDDEN);
}

static void chat_refresh_messages(void)
{
    if(g_chat_ai && g_chat_ai[0] != '\0') {
        lv_label_set_text(chat_ai_label, g_chat_ai);
        lv_obj_clear_flag(chat_ai_capsule, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(chat_ai_label, "");
        lv_obj_add_flag(chat_ai_capsule, LV_OBJ_FLAG_HIDDEN);
    }
}

static void chat_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_SCREEN_LOADED) {
        smartwatch_ui_runtime_mjpeg_player_set_visible(chat_mjpeg_player, true);
        smartwatch_ui_runtime_refresh_wake_word_detection();
    } else if(code == LV_EVENT_SCREEN_UNLOADED) {
        smartwatch_ui_runtime_mjpeg_player_set_visible(chat_mjpeg_player, false);
    }
}

/**
 * @brief 初始化AI聊天屏幕
 */
void ui_AIChatScreen_init(void)
{
    if(ui_AIChatScreen) return;

    ui_AIChatScreen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui_AIChatScreen);
    lv_obj_set_size(ui_AIChatScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_AIChatScreen, lv_color_hex(0x09090f), 0);
    lv_obj_set_style_bg_opa(ui_AIChatScreen, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_AIChatScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_AIChatScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_AIChatScreen, chat_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_AIChatScreen, chat_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    chat_mjpeg_bg = NULL;
    chat_mjpeg_player = NULL;
    chat_create_mjpeg_bg();

    chat_time_label = lv_label_create(ui_AIChatScreen);
    lv_obj_remove_style_all(chat_time_label);
    lv_label_set_text(chat_time_label, "00:00");
    lv_obj_set_style_text_font(chat_time_label, chat_text_font(), 0);
    lv_obj_set_style_text_color(chat_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(chat_time_label, (lv_opa_t)(LV_OPA_COVER * 3 / 4), 0);
    lv_obj_align(chat_time_label, LV_ALIGN_TOP_MID, 0, 56);

    chat_status_chip = lv_obj_create(ui_AIChatScreen);
    lv_obj_remove_style_all(chat_status_chip);
    lv_obj_set_size(chat_status_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(chat_status_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chat_status_chip, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(chat_status_chip, (lv_opa_t)(LV_OPA_COVER * 14 / 100), 0);
    lv_obj_set_style_border_width(chat_status_chip, 1, 0);
    lv_obj_set_style_border_color(chat_status_chip, lv_color_white(), 0);
    lv_obj_set_style_border_opa(chat_status_chip, (lv_opa_t)(LV_OPA_COVER / 12), 0);
    lv_obj_set_style_pad_hor(chat_status_chip, 10, 0);
    lv_obj_set_style_pad_ver(chat_status_chip, 4, 0);
    lv_obj_align(chat_status_chip, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_clear_flag(chat_status_chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    chat_status_label = lv_label_create(chat_status_chip);
    lv_obj_remove_style_all(chat_status_label);
    lv_label_set_text(chat_status_label, "");
    lv_obj_set_style_text_font(chat_status_label, chat_text_font(), 0);
    lv_obj_set_style_text_color(chat_status_label, lv_color_white(), 0);
    lv_obj_center(chat_status_label);

    chat_emotion_label = lv_label_create(ui_AIChatScreen);
    lv_obj_remove_style_all(chat_emotion_label);
    lv_label_set_text(chat_emotion_label, "");
    lv_obj_set_style_text_font(chat_emotion_label, chat_text_font(), 0);
    lv_obj_set_style_text_color(chat_emotion_label, lv_color_hex(0xf9a8d4), 0);
    lv_obj_set_style_text_opa(chat_emotion_label, (lv_opa_t)(LV_OPA_COVER * 4 / 5), 0);
    lv_obj_align(chat_emotion_label, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_add_flag(chat_emotion_label, LV_OBJ_FLAG_HIDDEN);

    chat_ai_capsule = lv_obj_create(ui_AIChatScreen);
    lv_obj_remove_style_all(chat_ai_capsule);
    lv_obj_set_size(chat_ai_capsule, 282, 44);
    lv_obj_set_style_radius(chat_ai_capsule, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chat_ai_capsule, lv_color_hex(0x6b7280), 0);
    lv_obj_set_style_bg_opa(chat_ai_capsule, (lv_opa_t)(LV_OPA_COVER * 2 / 5), 0);
    lv_obj_set_style_border_width(chat_ai_capsule, 1, 0);
    lv_obj_set_style_border_color(chat_ai_capsule, lv_color_white(), 0);
    lv_obj_set_style_border_opa(chat_ai_capsule, (lv_opa_t)(LV_OPA_COVER / 16), 0);
    lv_obj_set_style_pad_hor(chat_ai_capsule, 16, 0);
    lv_obj_set_style_pad_ver(chat_ai_capsule, 10, 0);
    lv_obj_align(chat_ai_capsule, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_clear_flag(chat_ai_capsule, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chat_ai_capsule, LV_OBJ_FLAG_HIDDEN);

    chat_ai_label = lv_label_create(chat_ai_capsule);
    lv_obj_remove_style_all(chat_ai_label);
    lv_obj_set_width(chat_ai_label, LV_PCT(100));
    lv_label_set_long_mode(chat_ai_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(chat_ai_label, chat_text_font(), 0);
    lv_obj_set_style_text_color(chat_ai_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(chat_ai_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(chat_ai_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(chat_ai_label, "");
    lv_obj_center(chat_ai_label);

    chat_update_clock();
    chat_refresh_status();
    chat_refresh_emotion();
    chat_refresh_messages();
    chat_refresh_mjpeg_state();

    chat_clock_timer = lv_timer_create(chat_clock_timer_cb, 1000, NULL);
    app_screen_enable_swipe_back(ui_AIChatScreen);
}

void ui_AIChatScreen_set_status(const char * status)
{
    chat_copy_text(g_chat_status, sizeof(g_chat_status), status);
    chat_refresh_status();
}

void ui_AIChatScreen_set_chat_message(const char * role, const char * content)
{
    if(!role) return;

    if(strcmp(role, "assistant") == 0 || strcmp(role, "system") == 0) {
        chat_replace_text(&g_chat_ai, content);
    }

    chat_refresh_messages();
}

void ui_AIChatScreen_clear_messages(void)
{
    chat_replace_text(&g_chat_ai, NULL);
    chat_refresh_messages();
}

void ui_AIChatScreen_set_emotion(const char * emotion)
{
    chat_copy_text(g_chat_emotion, sizeof(g_chat_emotion), emotion);
    chat_refresh_emotion();
}

void ui_AIChatScreen_reload_mjpeg(void)
{
    if(chat_mjpeg_player) {
        smartwatch_ui_runtime_mjpeg_player_destroy(chat_mjpeg_player);
        chat_mjpeg_player = NULL;
    }
    if(chat_mjpeg_bg) {
        lv_obj_delete(chat_mjpeg_bg);
        chat_mjpeg_bg = NULL;
    }

    smartwatch_ui_runtime_reset_remote_ai_chat_mjpeg_cache();
    g_chat_mjpeg_is_speaking = false;
    if(ui_AIChatScreen) {
        chat_create_mjpeg_bg();
    }
}

/**
 * @brief 反初始化AI聊天屏幕
 */
void ui_AIChatScreen_deinit(void)
{
    if(chat_clock_timer) {
        lv_timer_delete(chat_clock_timer);
        chat_clock_timer = NULL;
    }

    if(chat_mjpeg_player) {
        smartwatch_ui_runtime_mjpeg_player_destroy(chat_mjpeg_player);
        chat_mjpeg_player = NULL;
    }

    if(ui_AIChatScreen) {
        lv_obj_delete(ui_AIChatScreen);
        ui_AIChatScreen = NULL;
    }

    chat_time_label = NULL;
    chat_mjpeg_bg = NULL;
    chat_status_chip = NULL;
    chat_status_label = NULL;
    chat_emotion_label = NULL;
    chat_ai_capsule = NULL;
    chat_ai_label = NULL;
    g_chat_mjpeg_is_speaking = false;
    chat_replace_text(&g_chat_ai, NULL);
}
