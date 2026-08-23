/**
 * @file ui_WallpaperScreen.c
 * @brief 壁纸库浏览与应用屏幕实现
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
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

lv_obj_t * ui_WallpaperScreen;

wallpaper_config_t g_wallpaper_config = {
    .mode = WALLPAPER_MODE_TRIPLE,
    .selected_index = 0,
};

typedef struct {
    lv_obj_t * root;
    lv_obj_t * image;
    lv_obj_t * fallback;
    lv_image_dsc_t preview_dsc;
    uint8_t * preview_data;
    bool load_finished;
    bool available;
} wallpaper_card_view_t;

static lv_obj_t * tab_btns[2];
static lv_obj_t * tab_indicator;
static uint8_t wallpaper_tab = WALLPAPER_MODE_SINGLE;
static uint32_t selected_by_mode[2];

static lv_obj_t * apply_btn;
static lv_obj_t * apply_label;
static bool apply_enabled;

static lv_obj_t * content_area;
static lv_obj_t * cards_cont;
static wallpaper_card_view_t * card_views;
static uint32_t wallpaper_card_count;
static lv_timer_t * preview_load_timer;
static uint32_t preview_load_index;
static uint8_t preview_load_mode;
static bool previews_dirty = true;
static bool wallpaper_screen_active;
static bool wallpaper_is_scrolling;

static void ensure_cards(uint32_t count);
static void destroy_cards(void);
static void refresh_cards(void);
static void refresh_apply_state(void);

static const int32_t kWallpaperCardWidth = 100;
static const int32_t kWallpaperCardHeight = 136;
static const int32_t kWallpaperImageWidth = 96;
static const int32_t kWallpaperImageHeight = 132;
static const int32_t kWallpaperCardsHeight = 148;
static const int32_t kWallpaperCardGap = 4;
static const int32_t kWallpaperTabBarWidth = 174;
static const int32_t kWallpaperTabBarHeight = 38;
static const int32_t kWallpaperTabButtonWidth = 81;
static const int32_t kWallpaperTabButtonHeight = 30;
static const int32_t kWallpaperTabPad = 3;
static const int32_t kWallpaperTabGap = 4;
static const uint32_t kWallpaperPreviewStepMs = 12;

/* The source wallpapers can be much larger than the 96x132 preview. Keeping
 * a thumbnail per card avoids repeating a costly cover transform on every
 * scroll frame. The original image is still used by standby mode. */
static void release_card_preview(wallpaper_card_view_t * card)
{
    if(!card) return;

    if(card->preview_data) {
        heap_caps_free(card->preview_data);
        card->preview_data = NULL;
    }
    memset(&card->preview_dsc, 0, sizeof(card->preview_dsc));
}

static uint16_t read_rgb565(const uint8_t * pixel)
{
    uint16_t value;
    memcpy(&value, pixel, sizeof(value));
    return value;
}

static uint16_t blend_rgb565(uint16_t p00, uint16_t p10, uint16_t p01, uint16_t p11,
                             uint32_t fx, uint32_t fy)
{
    const uint64_t wx0 = 65536U - fx;
    const uint64_t wy0 = 65536U - fy;
    const uint64_t weight00 = wx0 * wy0;
    const uint64_t weight10 = (uint64_t)fx * wy0;
    const uint64_t weight01 = wx0 * fy;
    const uint64_t weight11 = (uint64_t)fx * fy;

    const uint32_t r = (uint32_t)((((p00 >> 11) & 0x1FU) * weight00 +
                        ((p10 >> 11) & 0x1FU) * weight10 +
                        ((p01 >> 11) & 0x1FU) * weight01 +
                        ((p11 >> 11) & 0x1FU) * weight11 + 0x80000000ULL) >> 32);
    const uint32_t g = (uint32_t)((((p00 >> 5) & 0x3FU) * weight00 +
                        ((p10 >> 5) & 0x3FU) * weight10 +
                        ((p01 >> 5) & 0x3FU) * weight01 +
                        ((p11 >> 5) & 0x3FU) * weight11 + 0x80000000ULL) >> 32);
    const uint32_t b = (uint32_t)(((p00 & 0x1FU) * weight00 +
                        (p10 & 0x1FU) * weight10 +
                        (p01 & 0x1FU) * weight01 +
                        (p11 & 0x1FU) * weight11 + 0x80000000ULL) >> 32);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool make_card_preview(const lv_image_dsc_t * source, wallpaper_card_view_t * card)
{
    if(!source || !card || source->header.cf != LV_COLOR_FORMAT_RGB565 ||
       !source->data || source->header.w == 0 || source->header.h == 0 ||
       source->header.stride < source->header.w * 2U) {
        return false;
    }

    const uint32_t source_w = source->header.w;
    const uint32_t source_h = source->header.h;
    const uint32_t target_w = (uint32_t)kWallpaperImageWidth;
    const uint32_t target_h = (uint32_t)kWallpaperImageHeight;
    const uint32_t stride = source->header.stride;
    const uint32_t scaled_w = (target_w * source_h >= target_h * source_w)
                                  ? target_w
                                  : (target_h * source_w + source_h - 1U) / source_h;
    const uint32_t scaled_h = (target_w * source_h >= target_h * source_w)
                                  ? (target_w * source_h + source_w - 1U) / source_w
                                  : target_h;
    const uint32_t crop_x = (scaled_w - target_w) / 2U;
    const uint32_t crop_y = (scaled_h - target_h) / 2U;
    const size_t data_size = (size_t)target_w * target_h * sizeof(uint16_t);
    uint8_t * data = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!data) return false;

    for(uint32_t y = 0; y < target_h; y++) {
        const uint32_t source_y_fp = (uint32_t)(((uint64_t)(y + crop_y) * source_h << 16) / scaled_h);
        const uint32_t source_y = source_y_fp >> 16;
        const uint32_t fy = source_y_fp & 0xFFFFU;
        const uint32_t y0 = source_y < source_h ? source_y : source_h - 1U;
        const uint32_t y1 = y0 + 1U < source_h ? y0 + 1U : y0;
        const uint8_t * row0 = source->data + (size_t)y0 * stride;
        const uint8_t * row1 = source->data + (size_t)y1 * stride;
        for(uint32_t x = 0; x < target_w; x++) {
            const uint32_t source_x_fp = (uint32_t)(((uint64_t)(x + crop_x) * source_w << 16) / scaled_w);
            const uint32_t source_x = source_x_fp >> 16;
            const uint32_t fx = source_x_fp & 0xFFFFU;
            const uint32_t x0 = source_x < source_w ? source_x : source_w - 1U;
            const uint32_t x1 = x0 + 1U < source_w ? x0 + 1U : x0;
            const uint16_t pixel = blend_rgb565(
                read_rgb565(row0 + x0 * 2U), read_rgb565(row0 + x1 * 2U),
                read_rgb565(row1 + x0 * 2U), read_rgb565(row1 + x1 * 2U), fx, fy);
            memcpy(data + ((size_t)y * target_w + x) * sizeof(pixel), &pixel, sizeof(pixel));
        }
    }

    card->preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    card->preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    card->preview_dsc.header.w = target_w;
    card->preview_dsc.header.h = target_h;
    card->preview_dsc.header.stride = target_w * sizeof(uint16_t);
    card->preview_dsc.data = data;
    card->preview_dsc.data_size = (uint32_t)data_size;
    card->preview_data = data;
    return true;
}

static uint8_t wallpaper_current_mode(void)
{
    return wallpaper_tab == WALLPAPER_MODE_TRIPLE ? WALLPAPER_MODE_TRIPLE : WALLPAPER_MODE_SINGLE;
}

static uint32_t wallpaper_selected(void)
{
    return selected_by_mode[wallpaper_current_mode()];
}

static void wallpaper_set_selected(uint32_t index)
{
    selected_by_mode[wallpaper_current_mode()] = index;
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
    if(count == 0 || wallpaper_selected() >= count) {
        wallpaper_set_selected(0);
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
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x202128), 0);
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

    lv_obj_set_style_pad_gap(cards_cont, kWallpaperCardGap, 0);
    lv_obj_set_flex_align(cards_cont,
                          wallpaper_card_count > 3 ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(cards_cont, wallpaper_card_count > 3 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID, 0, 0);

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        wallpaper_card_view_t * card = &card_views[i];
        if(!card->root || !card->image) continue;

        style_media_card(card->root, kWallpaperCardWidth, kWallpaperCardHeight);
        style_media_image(card->image, kWallpaperImageWidth, kWallpaperImageHeight);
        lv_obj_center(card->image);
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

static void free_card_arrays(void)
{
    if(card_views) {
        for(uint32_t i = 0; i < wallpaper_card_count; i++) {
            release_card_preview(&card_views[i]);
        }
    }
    free(card_views);
    card_views = NULL;
    wallpaper_card_count = 0;
}

static bool alloc_card_arrays(uint32_t count)
{
    card_views = (wallpaper_card_view_t *)calloc(count, sizeof(*card_views));
    if(!card_views) return false;
    wallpaper_card_count = count;
    return true;
}

static lv_obj_t * make_tab_btn(lv_obj_t * parent, const char * text, int32_t w)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, kWallpaperTabButtonHeight);
    lv_obj_set_style_radius(btn, kWallpaperTabButtonHeight / 2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, ui_builtin_text_font(), 0);
    ui_make_decor_hit_passthrough(lbl);
    lv_obj_center(lbl);
    return btn;
}

static void tab_indicator_set_x(void * obj, int32_t value)
{
    if(obj) lv_obj_set_x((lv_obj_t *)obj, value);
}

static void move_tab_indicator(bool animated)
{
    if(!tab_indicator) return;

    const int32_t x = kWallpaperTabPad +
                      (int32_t)wallpaper_tab * (kWallpaperTabButtonWidth + kWallpaperTabGap);
    lv_anim_del(tab_indicator, (lv_anim_exec_xcb_t)tab_indicator_set_x);
    if(!animated) {
        lv_obj_set_x(tab_indicator, x);
        return;
    }

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tab_indicator);
    lv_anim_set_values(&anim, lv_obj_get_x(tab_indicator), x);
    lv_anim_set_time(&anim, 150);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)tab_indicator_set_x);
    lv_anim_start(&anim);
}

static void wallpaper_set_border_opa(void * obj, int32_t value)
{
    if(obj) lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void wallpaper_set_shadow_opa(void * obj, int32_t value)
{
    if(obj) lv_obj_set_style_shadow_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void wallpaper_start_anim(lv_obj_t * obj, lv_anim_exec_xcb_t exec_cb,
                                 int32_t from, int32_t to, uint32_t duration)
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

static bool wallpaper_selection_is_applied(void)
{
    return g_wallpaper_config.mode == (wallpaper_mode_t)wallpaper_current_mode() &&
           g_wallpaper_config.selected_index == wallpaper_selected();
}

static wallpaper_card_view_t * wallpaper_selected_card(void)
{
    uint32_t index = wallpaper_selected();
    if(!card_views || index >= wallpaper_card_count) return NULL;
    return &card_views[index];
}

static void refresh_apply_state(void)
{
    if(!apply_btn || !apply_label) return;

    wallpaper_card_view_t * card = wallpaper_selected_card();
    bool available = card && card->load_finished && card->available;
    bool is_applied = available && wallpaper_selection_is_applied();
    uint32_t color_hex;

    apply_enabled = available && !is_applied;
    if(!card || !card->load_finished) {
        lv_label_set_text(apply_label, "加载中");
        color_hex = 0x5c5c66;
    }
    else if(!card->available) {
        lv_label_set_text(apply_label, "加载失败");
        color_hex = 0x5c5c66;
    }
    else if(is_applied) {
        lv_label_set_text(apply_label, "已应用");
        color_hex = 0x10b981;
    }
    else {
        lv_label_set_text(apply_label, "应用壁纸");
        color_hex = 0xf472b6;
    }

    lv_anim_del(apply_btn, (lv_anim_exec_xcb_t)wallpaper_set_border_opa);
    lv_anim_del(apply_btn, (lv_anim_exec_xcb_t)wallpaper_set_shadow_opa);
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(color_hex), 0);
    lv_obj_set_style_border_color(apply_btn, lv_color_hex(color_hex), 0);
    lv_obj_set_style_shadow_color(apply_btn, lv_color_hex(color_hex), 0);
    lv_obj_set_style_bg_opa(apply_btn, available ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 55 / 100), 0);
    lv_obj_set_style_border_opa(apply_btn,
                                available ? (lv_opa_t)(LV_OPA_COVER * 35 / 100) : LV_OPA_0, 0);
    lv_obj_set_style_shadow_opa(apply_btn,
                                available ? (lv_opa_t)(LV_OPA_COVER * 22 / 100) : LV_OPA_0, 0);
}

static void apply_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(!apply_btn) return;

    if(code == LV_EVENT_PRESSED && apply_enabled) {
        wallpaper_start_anim(apply_btn, (lv_anim_exec_xcb_t)wallpaper_set_border_opa,
                             lv_obj_get_style_border_opa(apply_btn, LV_PART_MAIN),
                             (int32_t)(LV_OPA_COVER * 82 / 100), 90);
        wallpaper_start_anim(apply_btn, (lv_anim_exec_xcb_t)wallpaper_set_shadow_opa,
                             lv_obj_get_style_shadow_opa(apply_btn, LV_PART_MAIN),
                             (int32_t)(LV_OPA_COVER * 48 / 100), 90);
        return;
    }

    if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL) {
        refresh_apply_state();
        return;
    }

    if(code != LV_EVENT_CLICKED || !apply_enabled) return;

    wallpaper_card_view_t * card = wallpaper_selected_card();
    if(!card || !card->available) {
        refresh_apply_state();
        return;
    }

    g_wallpaper_config.mode = (wallpaper_mode_t)wallpaper_current_mode();
    g_wallpaper_config.selected_index = wallpaper_selected();
    ui_StandbyScreen_apply_wallpaper();
    refresh_apply_state();
}

static void apply_tab_style(void)
{
    for(uint32_t i = 0; i < 2; i++) {
        if(!tab_btns[i]) continue;
        bool a = (wallpaper_tab == i);
        lv_obj_set_style_bg_opa(tab_btns[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tab_btns[i], 0, 0);
        lv_obj_set_style_border_color(tab_btns[i], lv_color_hex(0xf9a8d4), 0);
        lv_obj_set_style_text_color(tab_btns[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(tab_btns[i],
                                  a ? LV_OPA_COVER : (lv_opa_t)(LV_OPA_COVER * 62 / 100), 0);
    }
}

static void update_selection(void)
{
    wallpaper_clamp_selected();
    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        wallpaper_card_view_t * card = &card_views[i];
        if(!card->root) continue;
        bool is_selected = (i == wallpaper_selected());
        lv_obj_set_style_border_width(card->root, is_selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(card->root, lv_color_hex(0xf472b6), 0);
        lv_obj_set_style_border_opa(card->root, is_selected ? LV_OPA_COVER : LV_OPA_0, 0);
        lv_obj_set_style_shadow_width(card->root, is_selected && !wallpaper_is_scrolling ? 8 : 0, 0);
        lv_obj_set_style_shadow_color(card->root, lv_color_hex(0xf472b6), 0);
        lv_obj_set_style_shadow_opa(card->root,
                                    is_selected && !wallpaper_is_scrolling
                                        ? (lv_opa_t)(LV_OPA_COVER * 24 / 100)
                                        : LV_OPA_0, 0);
    }
    refresh_apply_state();
}

static void set_scroll_rendering(bool scrolling)
{
    if(wallpaper_is_scrolling == scrolling) return;
    wallpaper_is_scrolling = scrolling;

    wallpaper_card_view_t * card = wallpaper_selected_card();
    if(!card || !card->root) return;
    lv_obj_set_style_shadow_width(card->root, scrolling ? 0 : 8, 0);
    lv_obj_set_style_shadow_opa(card->root,
                                scrolling ? LV_OPA_0 : (lv_opa_t)(LV_OPA_COVER * 24 / 100), 0);
}

static void preview_load_timer_cb(lv_timer_t * timer)
{
    if(!wallpaper_screen_active || preview_load_mode != wallpaper_current_mode() ||
       !card_views || preview_load_index >= wallpaper_card_count) {
        lv_timer_pause(timer);
        if(preview_load_index >= wallpaper_card_count) previews_dirty = false;
        return;
    }

    uint32_t index = preview_load_index++;
    wallpaper_card_view_t * card = &card_views[index];
    const lv_image_dsc_t * image = wallpaper_get_preview_image(index);
    card->load_finished = true;
    card->available = image != NULL;

    if(image) {
        lv_image_set_src(card->image, NULL);
        release_card_preview(card);
        const lv_image_dsc_t * display_image = make_card_preview(image, card) ? &card->preview_dsc : image;
        lv_image_set_src(card->image, display_image);
        lv_obj_add_flag(card->fallback, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_image_set_src(card->image, NULL);
        lv_label_set_text(card->fallback, "加载失败");
        lv_obj_clear_flag(card->fallback, LV_OBJ_FLAG_HIDDEN);
    }

    if(index == wallpaper_selected()) refresh_apply_state();
    if(preview_load_index >= wallpaper_card_count) {
        previews_dirty = false;
        lv_timer_pause(timer);
    }
}

static void refresh_cards(void)
{
    uint32_t visible_count = wallpaper_visible_count();
    if(visible_count == 0) visible_count = 1;
    ensure_cards(visible_count);
    if(!card_views || !cards_cont) return;

    layout_cards_for_current_mode();
    wallpaper_clamp_selected();
    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        wallpaper_card_view_t * card = &card_views[i];
        card->load_finished = false;
        card->available = false;
        lv_image_set_src(card->image, NULL);
        release_card_preview(card);
        lv_label_set_text(card->fallback, "加载中");
        lv_obj_clear_flag(card->fallback, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_update_layout(content_area);
    if(wallpaper_selected() < wallpaper_card_count) {
        lv_obj_scroll_to_view(card_views[wallpaper_selected()].root, LV_ANIM_OFF);
    }

    preview_load_mode = wallpaper_current_mode();
    preview_load_index = 0;
    previews_dirty = true;
    update_selection();
    if(preview_load_timer && wallpaper_screen_active) {
        lv_timer_set_period(preview_load_timer, kWallpaperPreviewStepMs);
        lv_timer_reset(preview_load_timer);
        lv_timer_resume(preview_load_timer);
    }
}

static void card_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if(idx >= wallpaper_card_count) return;
    wallpaper_set_selected(idx);
    update_selection();
}

static void ensure_cards(uint32_t count)
{
    if(count == 0) count = 1;
    if(cards_cont && card_views && wallpaper_card_count == count) return;

    destroy_cards();
    if(!alloc_card_arrays(count)) return;

    cards_cont = make_transp_cont(content_area, LV_SIZE_CONTENT, kWallpaperCardsHeight);
    lv_obj_align(cards_cont, count > 3 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(cards_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(cards_cont, 4, 0);
    lv_obj_set_style_pad_right(cards_cont, 4, 0);
    lv_obj_set_style_pad_top(cards_cont, 6, 0);
    lv_obj_set_style_pad_bottom(cards_cont, 6, 0);
    lv_obj_set_style_pad_gap(cards_cont, kWallpaperCardGap, 0);
    lv_obj_add_flag(cards_cont, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        wallpaper_card_view_t * card = &card_views[i];
        card->root = lv_btn_create(cards_cont);
        lv_obj_remove_style_all(card->root);
        style_media_card(card->root, kWallpaperCardWidth, kWallpaperCardHeight);
        lv_obj_set_style_outline_width(card->root, 1, LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(card->root, lv_color_hex(0xf9a8d4), LV_STATE_PRESSED);
        lv_obj_set_style_outline_opa(card->root, (lv_opa_t)(LV_OPA_COVER * 70 / 100), LV_STATE_PRESSED);
        lv_obj_add_event_cb(card->root, card_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        card->image = lv_image_create(card->root);
        style_media_image(card->image, kWallpaperImageWidth, kWallpaperImageHeight);
        lv_obj_center(card->image);

        card->fallback = create_card_fallback_label(card->root, "加载中", ui_builtin_text_font());
    }
}

static void destroy_cards(void)
{
    if(preview_load_timer) lv_timer_pause(preview_load_timer);
    if(cards_cont) {
        lv_obj_delete(cards_cont);
        cards_cont = NULL;
    }
    free_card_arrays();
}

static void tab_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t new_tab = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if(new_tab == wallpaper_tab) return;
    wallpaper_tab = new_tab;
    apply_tab_style();
    move_tab_indicator(true);
    refresh_cards();
}

static void wallpaper_content_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(!ui_WallpaperScreen) return;

    switch(code) {
        case LV_EVENT_SCROLL_BEGIN:
            set_scroll_rendering(true);
            app_screen_set_swipe_back_enabled(ui_WallpaperScreen, false);
            return;

        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
        case LV_EVENT_CANCEL:
            if(lv_obj_is_scrolling(content_area)) return;
            set_scroll_rendering(false);
            app_screen_set_swipe_back_enabled(ui_WallpaperScreen, true);
            return;

        case LV_EVENT_SCROLL_END:
            set_scroll_rendering(false);
            app_screen_set_swipe_back_enabled(ui_WallpaperScreen, true);
            return;

        default:
            return;
    }
}

static void wallpaper_sync_draft_to_applied(void)
{
    wallpaper_tab = g_wallpaper_config.mode == WALLPAPER_MODE_TRIPLE
                        ? WALLPAPER_MODE_TRIPLE
                        : WALLPAPER_MODE_SINGLE;
    selected_by_mode[wallpaper_tab] = g_wallpaper_config.selected_index;
}

static void wallpaper_screen_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_SCREEN_LOADED) {
        wallpaper_screen_active = true;
        app_screen_set_swipe_back_enabled(ui_WallpaperScreen, true);
        (void)smartwatch_ui_runtime_wallpaper_preview_last_mode();
        wallpaper_sync_draft_to_applied();
        apply_tab_style();
        move_tab_indicator(false);
        if(previews_dirty || !card_views || preview_load_mode != wallpaper_current_mode()) {
            refresh_cards();
        }
        else {
            wallpaper_clamp_selected();
            update_selection();
            if(wallpaper_selected() < wallpaper_card_count) {
                lv_obj_scroll_to_view(card_views[wallpaper_selected()].root, LV_ANIM_OFF);
            }
        }
    }
    else if(code == LV_EVENT_SCREEN_UNLOADED) {
        wallpaper_screen_active = false;
        wallpaper_is_scrolling = false;
        if(preview_load_timer) lv_timer_pause(preview_load_timer);
        app_screen_set_swipe_back_enabled(ui_WallpaperScreen, true);
        refresh_apply_state();
    }
}

void ui_WallpaperScreen_init(void) {
    if(ui_WallpaperScreen) return;

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
    lv_obj_set_style_text_letter_space(title, 0, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t * tabs = make_transp_cont(ui_WallpaperScreen,
                                       kWallpaperTabBarWidth, kWallpaperTabBarHeight);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, 92);
    /* The children are positioned against the border-only content box. */
    lv_obj_set_style_pad_all(tabs, 0, 0);
    lv_obj_set_style_pad_gap(tabs, 0, 0);
    lv_obj_set_style_radius(tabs, kWallpaperTabBarHeight / 2, 0);
    lv_obj_set_style_clip_corner(tabs, true, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_hex(0x181820), 0);
    lv_obj_set_style_bg_opa(tabs, (lv_opa_t)(LV_OPA_COVER * 92 / 100), 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(0x41414d), 0);
    lv_obj_set_style_border_opa(tabs, (lv_opa_t)(LV_OPA_COVER * 75 / 100), 0);
    lv_obj_set_style_shadow_width(tabs, 7, 0);
    lv_obj_set_style_shadow_color(tabs, lv_color_hex(0x050509), 0);
    lv_obj_set_style_shadow_opa(tabs, (lv_opa_t)(LV_OPA_COVER * 45 / 100), 0);
    tab_indicator = lv_obj_create(tabs);
    lv_obj_remove_style_all(tab_indicator);
    lv_obj_set_size(tab_indicator, kWallpaperTabButtonWidth, kWallpaperTabButtonHeight);
    lv_obj_set_style_radius(tab_indicator, kWallpaperTabButtonHeight / 2, 0);
    lv_obj_set_style_clip_corner(tab_indicator, true, 0);
    lv_obj_set_style_bg_color(tab_indicator, lv_color_hex(0xe85d9f), 0);
    lv_obj_set_style_bg_opa(tab_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab_indicator, 1, 0);
    lv_obj_set_style_border_color(tab_indicator, lv_color_hex(0xffb5d8), 0);
    lv_obj_set_style_border_opa(tab_indicator, (lv_opa_t)(LV_OPA_COVER * 55 / 100), 0);
    lv_obj_set_style_shadow_width(tab_indicator, 9, 0);
    lv_obj_set_style_shadow_color(tab_indicator, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_shadow_opa(tab_indicator, (lv_opa_t)(LV_OPA_COVER * 30 / 100), 0);
    lv_obj_clear_flag(tab_indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    tab_btns[0] = make_tab_btn(tabs, "单图", kWallpaperTabButtonWidth);
    tab_btns[1] = make_tab_btn(tabs, "多图", kWallpaperTabButtonWidth);

    lv_obj_set_pos(tab_indicator, kWallpaperTabPad, kWallpaperTabPad);
    lv_obj_set_pos(tab_btns[0], kWallpaperTabPad, kWallpaperTabPad);
    lv_obj_set_pos(tab_btns[1], kWallpaperTabPad + kWallpaperTabButtonWidth + kWallpaperTabGap,
                   kWallpaperTabPad);

    for(uint32_t i = 0; i < 2; i++)
        lv_obj_add_event_cb(tab_btns[i], tab_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    content_area = make_transp_cont(ui_WallpaperScreen, 316, kWallpaperCardsHeight);
    lv_obj_align(content_area, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_add_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content_area, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(content_area, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(content_area, wallpaper_content_event_cb, LV_EVENT_ALL, NULL);

    apply_btn = lv_btn_create(ui_WallpaperScreen);
    lv_obj_remove_style_all(apply_btn);
    lv_obj_set_size(apply_btn, 180, 40);
    lv_obj_set_style_radius(apply_btn, 20, 0);
    lv_obj_set_style_bg_color(apply_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_bg_opa(apply_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(apply_btn, 2, 0);
    lv_obj_set_style_border_color(apply_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_border_opa(apply_btn, (lv_opa_t)(LV_OPA_COVER * 35 / 100), 0);
    lv_obj_set_style_shadow_width(apply_btn, 16, 0);
    lv_obj_set_style_shadow_color(apply_btn, lv_color_hex(0xf472b6), 0);
    lv_obj_set_style_shadow_opa(apply_btn, (lv_opa_t)(LV_OPA_COVER * 22 / 100), 0);
    lv_obj_set_style_shadow_spread(apply_btn, 0, 0);
    lv_obj_align(apply_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(apply_btn, apply_event_cb, LV_EVENT_CLICKED, NULL);

    apply_label = lv_label_create(apply_btn);
    lv_obj_remove_style_all(apply_label);
    lv_label_set_text(apply_label, "应用壁纸");
    lv_obj_set_style_text_font(apply_label, ui_builtin_text_font(), 0);
    lv_obj_set_style_text_color(apply_label, lv_color_white(), 0);
    ui_make_decor_hit_passthrough(apply_label);
    lv_obj_center(apply_label);

    wallpaper_sync_draft_to_applied();
    previews_dirty = true;
    wallpaper_screen_active = false;
    apply_tab_style();
    move_tab_indicator(false);
    refresh_apply_state();
    preview_load_timer = lv_timer_create(preview_load_timer_cb, kWallpaperPreviewStepMs, NULL);
    if(preview_load_timer) lv_timer_pause(preview_load_timer);
    lv_obj_add_event_cb(ui_WallpaperScreen, wallpaper_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_WallpaperScreen, wallpaper_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    app_screen_enable_swipe_back(ui_WallpaperScreen);
}

void ui_WallpaperScreen_deinit(void) {
    wallpaper_screen_active = false;
    wallpaper_is_scrolling = false;
    if(tab_indicator) {
        lv_anim_del(tab_indicator, (lv_anim_exec_xcb_t)tab_indicator_set_x);
    }
    if(preview_load_timer) {
        lv_timer_delete(preview_load_timer);
        preview_load_timer = NULL;
    }
    destroy_cards();
    if(ui_WallpaperScreen) { lv_obj_delete(ui_WallpaperScreen); ui_WallpaperScreen = NULL; }
    for(uint32_t i = 0; i < 2; i++) tab_btns[i] = NULL;
    tab_indicator = NULL;
    content_area = NULL;
    apply_btn = NULL;
    apply_label = NULL;
    apply_enabled = false;
    previews_dirty = true;
}

void ui_WallpaperScreen_reload_previews(void) {
    wallpaper_sync_draft_to_applied();
    previews_dirty = true;
    if(wallpaper_screen_active) {
        refresh_cards();
    }
    apply_tab_style();
    refresh_apply_state();
}

void ui_WallpaperScreen_release_preview_images(void) {
    previews_dirty = true;
    if(preview_load_timer) lv_timer_pause(preview_load_timer);
    if(!card_views) return;

    for(uint32_t i = 0; i < wallpaper_card_count; i++) {
        wallpaper_card_view_t * card = &card_views[i];
        card->load_finished = false;
        card->available = false;
        if(card->image) {
            lv_image_set_src(card->image, NULL);
        }
        release_card_preview(card);
        if(card->fallback) {
            lv_label_set_text(card->fallback, "加载中");
            lv_obj_clear_flag(card->fallback, LV_OBJ_FLAG_HIDDEN);
        }
    }
    refresh_apply_state();
}
