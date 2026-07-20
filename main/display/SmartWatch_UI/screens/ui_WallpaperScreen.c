/**
 * @file ui_WallpaperScreen.c
 * @brief 壁纸库屏幕实现
 *
 * 布局（360x360圆形屏幕）：
 *   - 顶部：标题"壁纸"
 *   - 标签页：单图 | 轮播
 *   - 内容区：按实际数量显示壁纸卡片
 *   - 底部：应用壁纸按钮
 *
 * 当前实现：
 *   - 壁纸预览共用一套卡片样式，方便后续扩展网络壁纸
 *   - 三张图片壁纸在 SmartWatch_UI 启动时预加载，预览页直接复用共享缓存
 */

#include "ui_WallpaperScreen.h"
#include "ui_StandbyScreen.h"
#include "../ui_media_assets.h"
#include "../ui_runtime.h"
#include "../ui.h"
#include "../app_common.h"
#include <stdlib.h>

lv_obj_t * ui_WallpaperScreen;

wallpaper_config_t g_wallpaper_config = {
    .mode = WALLPAPER_MODE_TRIPLE,
    .selected_index = 0,
};

static lv_obj_t * tab_btns[2];
static uint8_t wallpaper_tab = 0;

static lv_obj_t * apply_btn;
static lv_obj_t * apply_label;
static bool applied;

static lv_grad_dsc_t tab_grad;

static lv_obj_t * content_area;
static lv_obj_t * cards_cont;
static bool cards_created;

static lv_obj_t ** cards;
static lv_obj_t ** card_imgs;
static lv_obj_t ** card_fallbacks;
static uint32_t wallpaper_card_count;
static uint32_t selected = 0;

static void ensure_cards(uint32_t count);
static void destroy_cards(void);

static const int32_t kWallpaperCardWidth = 100;
static const int32_t kWallpaperCardHeight = 136;
static const int32_t kWallpaperImageWidth = 96;
static const int32_t kWallpaperImageHeight = 132;
static const int32_t kWallpaperTabBarWidth = 186;
static const int32_t kWallpaperTabBarHeight = 38;
static const int32_t kWallpaperTabButtonWidth = 88;
static const int32_t kWallpaperTabButtonHeight = 32;

static uint8_t wallpaper_current_mode(void)
{
    return wallpaper_tab == WALLPAPER_MODE_TRIPLE ? WALLPAPER_MODE_TRIPLE : WALLPAPER_MODE_SINGLE;
}

static bool wallpaper_has_remote_previews(uint8_t mode)
{
    return smartwatch_ui_runtime_wallpaper_preview_is_remote(mode) &&
           smartwatch_ui_runtime_wallpaper_preview_count(mode) > 0;
}

static uint32_t wallpaper_visible_count(void)
{
    uint8_t mode = wallpaper_current_mode();
    if(!wallpaper_has_remote_previews(mode)) return 3;

    uint32_t count = smartwatch_ui_runtime_wallpaper_preview_count(mode);
    if(count == 0) return 1;
    return count;
}

static bool wallpaper_is_single_remote_preview(void)
{
    return wallpaper_current_mode() == WALLPAPER_MODE_SINGLE &&
           wallpaper_has_remote_previews(WALLPAPER_MODE_SINGLE);
}

static const lv_image_dsc_t * wallpaper_get_preview_image(uint32_t index)
{
    uint8_t mode = wallpaper_current_mode();
    if(wallpaper_has_remote_previews(mode)) {
        return smartwatch_ui_runtime_wallpaper_preview_get(mode, index);
    }
    return ui_media_assets_get_wallpaper_jpg(index);
}

static void wallpaper_clamp_selected(void)
{
    uint32_t count = wallpaper_visible_count();
    if(count == 0 || selected >= count) {
        selected = 0;
    }
}

static lv_obj_t * make_transp_cont(lv_obj_t * parent, int32_t w, int32_t h)
{
    lv_obj_t * c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    return c;
}

/* Shared card chrome for wallpaper previews. */
static void style_media_card(lv_obj_t * obj, int32_t w, int32_t h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x24243a), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
}

/* Shared image settings so previews scale consistently. */
static void style_media_image(lv_obj_t * obj, int32_t w, int32_t h)
{
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_COVER);
    lv_image_set_antialias(obj, true);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void layout_cards_for_current_mode(void)
{
    if(!cards_cont) return;

    const bool single_remote = wallpaper_is_single_remote_preview();
    lv_obj_set_style_pad_gap(cards_cont, single_remote ? 0 : 4, 0);
    lv_obj_set_flex_align(cards_cont,
                          wallpaper_card_count > 3 ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(cards_cont, wallpaper_card_count > 3 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID, 0, 0);

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        if(!cards[i] || !card_imgs[i]) continue;

        (void)single_remote;
        style_media_card(cards[i], kWallpaperCardWidth, kWallpaperCardHeight);
        style_media_image(card_imgs[i], kWallpaperImageWidth, kWallpaperImageHeight);
        lv_obj_center(card_imgs[i]);
    }
}

/* Fallback copy is kept visible until the real media loads successfully. */
static lv_obj_t * create_card_fallback_label(lv_obj_t * parent, const char * text, const lv_font_t * font)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, (lv_opa_t)(LV_OPA_COVER * 3 / 5), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/* Reset cached object pointers after the owning container is destroyed. */
static void clear_card_refs(void)
{
    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        cards[i] = NULL;
        card_imgs[i] = NULL;
        card_fallbacks[i] = NULL;
    }
}

static void free_card_arrays(void)
{
    free(cards);
    free(card_imgs);
    free(card_fallbacks);
    cards = NULL;
    card_imgs = NULL;
    card_fallbacks = NULL;
    wallpaper_card_count = 0;
}

static bool alloc_card_arrays(uint32_t count)
{
    cards = (lv_obj_t **)calloc(count, sizeof(*cards));
    card_imgs = (lv_obj_t **)calloc(count, sizeof(*card_imgs));
    card_fallbacks = (lv_obj_t **)calloc(count, sizeof(*card_fallbacks));
    if(!cards || !card_imgs || !card_fallbacks) {
        free_card_arrays();
        return false;
    }
    wallpaper_card_count = count;
    return true;
}

static lv_obj_t * make_tab_btn(lv_obj_t * parent, const char * text, int32_t w)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, kWallpaperTabButtonHeight);
    lv_obj_set_style_radius(btn, kWallpaperTabButtonHeight / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, ui_builtin_text_font(), 0);
    ui_make_decor_hit_passthrough(lbl);
    lv_obj_center(lbl);
    return btn;
}

static void set_applied(bool v)
{
    applied = v;
    if(!apply_btn) return;
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(v ? 0x10b981 : 0xf472b6), 0);
    lv_label_set_text(apply_label, v ? "已应用" : "应用壁纸");
}

static void apply_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED || applied) return;
    g_wallpaper_config.mode = (wallpaper_mode_t)wallpaper_tab;
    g_wallpaper_config.selected_index = selected;
    ui_StandbyScreen_apply_wallpaper();
    set_applied(true);
}

static void init_grad(void)
{
    const lv_color_t cols[] = { lv_color_hex(0xf472b6), lv_color_hex(0xa855f7) };
    const lv_opa_t opas[] = { LV_OPA_COVER, LV_OPA_COVER };
    const uint8_t fracs[] = { 0, 255 };
    lv_grad_init_stops(&tab_grad, cols, opas, fracs, 2);
    lv_grad_linear_init(&tab_grad, lv_pct(0), lv_pct(0), lv_pct(100), lv_pct(100), LV_GRAD_EXTEND_PAD);
}

static void apply_tab_style(void)
{
    for(uint32_t i = 0; i < 2; i++) {
        if(!tab_btns[i]) continue;
        bool a = (wallpaper_tab == i);
        lv_obj_set_style_bg_opa(tab_btns[i], a ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 8 / 100), 0);
        lv_obj_set_style_bg_grad(tab_btns[i], a ? &tab_grad : NULL, 0);
        lv_obj_set_style_text_color(tab_btns[i], a ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_opa(tab_btns[i], a ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 80 / 100), 0);
        lv_obj_set_style_shadow_width(tab_btns[i], a ? 8 : 0, 0);
        lv_obj_set_style_shadow_color(tab_btns[i], lv_color_hex(0xf472b6), 0);
        lv_obj_set_style_shadow_opa(tab_btns[i],
                                    a ? (lv_opa_t)(LV_OPA_COVER * 18 / 100) : LV_OPA_0, 0);
    }
}

static void update_selection(void)
{
    wallpaper_clamp_selected();
    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        if(!cards[i]) continue;
        bool s = (i == selected);
        lv_obj_set_style_border_width(cards[i], s ? 2 : 0, 0);
        lv_obj_set_style_border_color(cards[i], lv_color_hex(0xf472b6), 0);
        lv_obj_set_style_border_opa(cards[i], s ? LV_OPA_COVER : LV_OPA_0, 0);
        lv_obj_set_style_shadow_width(cards[i], s ? 10 : 0, 0);
        lv_obj_set_style_shadow_color(cards[i], lv_color_hex(0xf472b6), 0);
        lv_obj_set_style_shadow_opa(cards[i], s ? (lv_opa_t)(LV_OPA_COVER * 30 / 100) : LV_OPA_0, 0);
    }
}

static void refresh_cards(void)
{
    uint32_t visible_count = wallpaper_visible_count();
    if(visible_count == 0) visible_count = 1;
    ensure_cards(visible_count);
    if(!cards_created) return;

    layout_cards_for_current_mode();
    wallpaper_clamp_selected();
    if(content_area) {
        lv_obj_scroll_to_x(content_area, 0, LV_ANIM_OFF);
    }
    uint8_t mode = wallpaper_current_mode();
    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        if(!cards[i] || !card_imgs[i] || !card_fallbacks[i]) continue;

        const lv_image_dsc_t * img = wallpaper_get_preview_image(i);
        if(img) {
            lv_image_set_src(card_imgs[i], img);
            lv_obj_add_flag(card_fallbacks[i], LV_OBJ_FLAG_HIDDEN);
        }
        else {
            char fallback_text[16];
            if(wallpaper_has_remote_previews(mode)) {
                lv_snprintf(fallback_text, sizeof(fallback_text), "加载失败");
            }
            else {
                lv_snprintf(fallback_text, sizeof(fallback_text), "壁纸 %d", (int)(i + 1));
            }
            lv_image_set_src(card_imgs[i], NULL);
            lv_label_set_text(card_fallbacks[i], fallback_text);
            lv_obj_clear_flag(card_fallbacks[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    update_selection();
}

static void card_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if(idx >= wallpaper_card_count) return;
    selected = idx;
    update_selection();
    set_applied(false);
}

static void ensure_cards(uint32_t count)
{
    if(count == 0) count = 1;
    if(cards_created && wallpaper_card_count == count) return;

    destroy_cards();
    if(!alloc_card_arrays(count)) return;

    cards_cont = make_transp_cont(content_area, LV_SIZE_CONTENT, 140);
    lv_obj_align(cards_cont, count > 3 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(cards_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cards_cont, 4, 0);
    lv_obj_add_flag(cards_cont, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        cards[i] = lv_btn_create(cards_cont);
        lv_obj_remove_style_all(cards[i]);
        style_media_card(cards[i], kWallpaperCardWidth, kWallpaperCardHeight);
        lv_obj_add_event_cb(cards[i], card_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        card_imgs[i] = lv_image_create(cards[i]);
        style_media_image(card_imgs[i], kWallpaperImageWidth, kWallpaperImageHeight);
        lv_obj_center(card_imgs[i]);

        char fallback_text[16];
        lv_snprintf(fallback_text, sizeof(fallback_text), "壁纸 %d", (int)(i + 1));
        card_fallbacks[i] = create_card_fallback_label(cards[i], fallback_text, ui_builtin_text_font());

    }

    cards_created = true;
}

static void destroy_cards(void)
{
    clear_card_refs();
    if(cards_cont) {
        lv_obj_delete(cards_cont);
        cards_cont = NULL;
    }
    free_card_arrays();
    cards_created = false;
}

static void tab_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t new_tab = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(new_tab == wallpaper_tab) return;
    wallpaper_tab = new_tab;
    apply_tab_style();
    refresh_cards();
    set_applied(false);
}

static void wallpaper_content_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(!ui_WallpaperScreen) return;

    switch(code) {
        case LV_EVENT_PRESSED:
        case LV_EVENT_PRESSING:
        case LV_EVENT_SCROLL_BEGIN:
        case LV_EVENT_GESTURE:
            app_screen_set_swipe_back_enabled(ui_WallpaperScreen, false);
            return;

        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
        case LV_EVENT_SCROLL_END:
            app_screen_set_swipe_back_enabled(ui_WallpaperScreen, true);
            return;

        default:
            return;
    }
}

void ui_WallpaperScreen_init(void) {
    if(ui_WallpaperScreen) return;
    init_grad();

    ui_WallpaperScreen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui_WallpaperScreen);
    lv_obj_set_size(ui_WallpaperScreen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui_WallpaperScreen, lv_color_hex(0x0e0e14), 0);
    lv_obj_set_style_bg_opa(ui_WallpaperScreen, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_WallpaperScreen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ui_WallpaperScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * title = lv_label_create(ui_WallpaperScreen);
    lv_obj_remove_style_all(title);
    lv_label_set_text(title, "壁纸");
    lv_obj_set_style_text_font(title, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_opa(title, (lv_opa_t)(LV_OPA_COVER * 85 / 100), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t * tabs = make_transp_cont(ui_WallpaperScreen,
                                       kWallpaperTabBarWidth, kWallpaperTabBarHeight);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_pad_all(tabs, 3, 0);
    lv_obj_set_style_pad_gap(tabs, 4, 0);
    lv_obj_set_style_radius(tabs, kWallpaperTabBarHeight / 2, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tabs, (lv_opa_t)(LV_OPA_COVER * 9 / 100), 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_white(), 0);
    lv_obj_set_style_border_opa(tabs, (lv_opa_t)(LV_OPA_COVER * 4 / 100), 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    tab_btns[0] = make_tab_btn(tabs, "单图", kWallpaperTabButtonWidth);
    tab_btns[1] = make_tab_btn(tabs, "多图", kWallpaperTabButtonWidth);

    for(uint32_t i = 0; i < 2; i++)
        lv_obj_add_event_cb(tab_btns[i], tab_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    content_area = make_transp_cont(ui_WallpaperScreen, 316, 140);
    lv_obj_align(content_area, LV_ALIGN_TOP_MID, 0, 141);
    lv_obj_add_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content_area, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(content_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(content_area, wallpaper_content_event_cb, LV_EVENT_ALL, NULL);

    apply_btn = lv_btn_create(ui_WallpaperScreen);
    lv_obj_remove_style_all(apply_btn);
    lv_obj_set_size(apply_btn, 180, 40);
    lv_obj_set_style_radius(apply_btn, 999, 0);
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_bg_opa(apply_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(apply_btn, 16, 0);
    lv_obj_set_style_shadow_color(apply_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_shadow_opa(apply_btn, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_align(apply_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_CLICKED, NULL);

    apply_label = lv_label_create(apply_btn);
    lv_obj_remove_style_all(apply_label);
    lv_label_set_text(apply_label, "应用壁纸");
    lv_obj_set_style_text_font(apply_label, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(apply_label, lv_color_white(), 0);
    ui_make_decor_hit_passthrough(apply_label);
    lv_obj_center(apply_label);

    applied = false;
    wallpaper_tab = 0;
    selected = 0;
    apply_tab_style();
    ensure_cards(wallpaper_visible_count());
    refresh_cards();
    app_screen_enable_swipe_back(ui_WallpaperScreen);
}

void ui_WallpaperScreen_deinit(void) {
    destroy_cards();
    if(ui_WallpaperScreen) { lv_obj_delete(ui_WallpaperScreen); ui_WallpaperScreen = NULL; }
    for(uint32_t i = 0; i < 2; i++) tab_btns[i] = NULL;
    content_area = NULL;
    cards_created = false;
    apply_btn = NULL;
    apply_label = NULL;
}

void ui_WallpaperScreen_reload_previews(void) {
    wallpaper_tab = smartwatch_ui_runtime_wallpaper_preview_last_mode();
    selected = 0;
    if(cards_created) {
        refresh_cards();
    }
    apply_tab_style();
    set_applied(false);
}

void ui_WallpaperScreen_release_preview_images(void) {
    if(!cards_created) return;

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        if(card_imgs[i]) {
            lv_image_set_src(card_imgs[i], NULL);
        }
    }
}
