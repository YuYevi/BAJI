#include "ui_runtime.h"

#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "remote_mjpeg_store.h"
#include "remote_wallpaper_store.h"
#include "screens/ui_StandbyScreen.h"
#include "screens/ui_WallpaperScreen.h"
#include "../lvgl_display/lvgl_image.h"
#include "../lvgl_display/jpg/jpeg_to_image.h"
#include "../lvgl_display/lvgl_theme.h"

#include <esp_heap_caps.h>
#include <cstring>
#include <memory>
#include <vector>

static constexpr uint32_t kMjpegFrameIntervalMs = 100;

struct smartwatch_ui_runtime_mjpeg_player {
    lv_obj_t * target = nullptr;
    const uint8_t * data = nullptr;
    size_t size = 0;
    size_t next_offset = 0;
    lv_timer_t * timer = nullptr;
    lv_image_dsc_t frame_dsc = {};
    uint8_t * frame_data = nullptr;
    bool loaded = false;
};

struct smartwatch_ui_runtime_wallpaper_layer {
    lv_obj_t * root = nullptr;
};

static smartwatch_ui_runtime_wallpaper_layer g_wallpaper_layer;
static std::vector<std::unique_ptr<LvglImage>> g_wallpaper_preview_images[2];
static uint32_t g_wallpaper_preview_interval_ms[2] = {2000, 2000};
static uint8_t g_wallpaper_preview_last_mode = 0;
static bool g_wallpaper_remote_preview_loaded = false;
static lv_timer_t * g_wallpaper_standby_refresh_timer = NULL;
static bool g_wallpaper_standby_refresh_pending = false;
static uint8_t * g_remote_ai_chat_mjpeg[2] = {nullptr, nullptr};
static size_t g_remote_ai_chat_mjpeg_size[2] = {0, 0};
static bool g_remote_ai_chat_mjpeg_checked = false;

static void smartwatch_ui_runtime_release_remote_ai_chat_mjpeg_cache(void)
{
    for (uint8_t i = 0; i < 2; ++i) {
        if (g_remote_ai_chat_mjpeg[i] != nullptr) {
            heap_caps_free(g_remote_ai_chat_mjpeg[i]);
            g_remote_ai_chat_mjpeg[i] = nullptr;
        }
        g_remote_ai_chat_mjpeg_size[i] = 0;
    }
}

static void smartwatch_ui_runtime_ensure_remote_ai_chat_mjpeg_cache(void)
{
    if (g_remote_ai_chat_mjpeg_checked) {
        return;
    }
    g_remote_ai_chat_mjpeg_checked = true;

    auto& store = RemoteMjpegStore::GetInstance();
    if (!store.Reload()) {
        return;
    }

    uint8_t * listening = nullptr;
    uint8_t * speaking = nullptr;
    size_t listening_size = 0;
    size_t speaking_size = 0;
    if (!store.Load(false, &listening, &listening_size) ||
        !store.Load(true, &speaking, &speaking_size)) {
        if (listening != nullptr) {
            heap_caps_free(listening);
        }
        if (speaking != nullptr) {
            heap_caps_free(speaking);
        }
        return;
    }

    g_remote_ai_chat_mjpeg[0] = listening;
    g_remote_ai_chat_mjpeg_size[0] = listening_size;
    g_remote_ai_chat_mjpeg[1] = speaking;
    g_remote_ai_chat_mjpeg_size[1] = speaking_size;
}

static void smartwatch_ui_runtime_wallpaper_standby_refresh_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    if(!g_wallpaper_standby_refresh_pending) return;
    g_wallpaper_standby_refresh_pending = false;
    ui_StandbyScreen_apply_wallpaper();
}

static void smartwatch_ui_runtime_wallpaper_request_standby_refresh(void)
{
    g_wallpaper_standby_refresh_pending = true;
    if(g_wallpaper_standby_refresh_timer == NULL) {
        g_wallpaper_standby_refresh_timer = lv_timer_create(
            smartwatch_ui_runtime_wallpaper_standby_refresh_timer_cb, 50, NULL);
    }
    if(g_wallpaper_standby_refresh_timer) {
        lv_timer_resume(g_wallpaper_standby_refresh_timer);
        lv_timer_reset(g_wallpaper_standby_refresh_timer);
    }
}

static void smartwatch_ui_runtime_wallpaper_detach_old_resources(void)
{
    smartwatch_ui_runtime_wallpaper_clear();
    ui_WallpaperScreen_release_preview_images();
}

/* The server's newest wallpaper set is the active draft until the user
 * chooses another set in the wallpaper page. */
static void smartwatch_ui_runtime_wallpaper_select_latest_preview(void)
{
    const uint8_t mode = g_wallpaper_preview_last_mode == 1 ? 1 : 0;
    if (g_wallpaper_preview_images[mode].empty()) {
        return;
    }

    g_wallpaper_config.mode = mode == 1 ? WALLPAPER_MODE_TRIPLE : WALLPAPER_MODE_SINGLE;
    g_wallpaper_config.selected_index = 0;
}

static void smartwatch_ui_runtime_wallpaper_apply_remote_state(void)
{
    smartwatch_ui_runtime_wallpaper_detach_old_resources();
    for (uint8_t mode = 0; mode < 2; ++mode) {
        g_wallpaper_preview_images[mode].clear();
        g_wallpaper_preview_interval_ms[mode] = 2000;
    }

    auto& store = RemoteWallpaperStore::GetInstance();
    if (!store.Reload()) {
        g_wallpaper_remote_preview_loaded = true;
        return;
    }

    for (uint8_t mode = 0; mode < 2; ++mode) {
        if (!store.HasImages(mode)) {
            continue;
        }
        g_wallpaper_preview_images[mode].resize(store.GetCount(mode));
        g_wallpaper_preview_interval_ms[mode] = store.GetIntervalMs(mode);
    }
    g_wallpaper_preview_last_mode = store.GetLastMode();
    g_wallpaper_remote_preview_loaded = true;
    smartwatch_ui_runtime_wallpaper_select_latest_preview();
}

static void smartwatch_ui_runtime_wallpaper_ensure_remote_state(void)
{
    if (!g_wallpaper_remote_preview_loaded) {
        smartwatch_ui_runtime_wallpaper_apply_remote_state();
    }
}

static void smartwatch_ui_runtime_mjpeg_player_release_frame(smartwatch_ui_runtime_mjpeg_player_t * player)
{
    if (player == nullptr) {
        return;
    }

    if (player->frame_data != nullptr) {
        heap_caps_free(player->frame_data);
        player->frame_data = nullptr;
    }
    std::memset(&player->frame_dsc, 0, sizeof(player->frame_dsc));
    player->loaded = false;
}

static bool smartwatch_ui_runtime_mjpeg_find_frame(const uint8_t * data, size_t size, size_t search_offset,
                                                   size_t * frame_offset, size_t * frame_size, size_t * next_offset)
{
    if (data == nullptr || size < 4 || search_offset >= size || frame_offset == nullptr || frame_size == nullptr ||
        next_offset == nullptr) {
        return false;
    }

    size_t start = SIZE_MAX;
    for (size_t i = search_offset; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            start = i;
            break;
        }
    }
    if (start == SIZE_MAX) {
        return false;
    }

    for (size_t i = start + 2; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            *frame_offset = start;
            *frame_size = (i + 2) - start;
            *next_offset = i + 2;
            return true;
        }
    }

    return false;
}

static bool smartwatch_ui_runtime_mjpeg_player_decode_next_frame(smartwatch_ui_runtime_mjpeg_player_t * player)
{
    if (player == nullptr || player->target == nullptr || !lv_obj_is_valid(player->target) || player->data == nullptr ||
        player->size < 4) {
        return false;
    }

    size_t frame_offset = 0;
    size_t frame_size = 0;
    size_t next_offset = 0;
    size_t start_offset = player->next_offset < player->size ? player->next_offset : 0;

    bool found = smartwatch_ui_runtime_mjpeg_find_frame(player->data, player->size, start_offset, &frame_offset,
                                                        &frame_size, &next_offset);
    if (!found && start_offset != 0) {
        found = smartwatch_ui_runtime_mjpeg_find_frame(player->data, player->size, 0, &frame_offset, &frame_size,
                                                       &next_offset);
    }
    if (!found || frame_size == 0) {
        return false;
    }

    uint8_t * decoded = nullptr;
    size_t decoded_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    esp_err_t err = jpeg_to_image(player->data + frame_offset, frame_size, &decoded, &decoded_len, &width, &height,
                                  &stride);
    if (err != ESP_OK || decoded == nullptr || decoded_len == 0 || width == 0 || height == 0) {
        if (decoded != nullptr) {
            heap_caps_free(decoded);
        }
        return false;
    }

    uint8_t * previous_frame = player->frame_data;
    std::memset(&player->frame_dsc, 0, sizeof(player->frame_dsc));
    player->frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    player->frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    player->frame_dsc.header.w = (uint32_t)width;
    player->frame_dsc.header.h = (uint32_t)height;
    player->frame_dsc.header.stride = (uint32_t)stride;
    player->frame_dsc.data = decoded;
    player->frame_dsc.data_size = (uint32_t)decoded_len;
    player->frame_data = decoded;
    player->loaded = true;
    player->next_offset = next_offset < player->size ? next_offset : 0;

    lv_image_set_src(player->target, &player->frame_dsc);

    if (previous_frame != nullptr && previous_frame != decoded) {
        heap_caps_free(previous_frame);
    }

    return true;
}

static void smartwatch_ui_runtime_mjpeg_player_timer_cb(lv_timer_t * timer)
{
    auto * player = static_cast<smartwatch_ui_runtime_mjpeg_player_t *>(lv_timer_get_user_data(timer));
    if (player == nullptr) {
        return;
    }

    if (!smartwatch_ui_runtime_mjpeg_player_decode_next_frame(player) && player->timer != nullptr) {
        lv_timer_pause(player->timer);
    }
}

static void smartwatch_ui_runtime_wallpaper_ensure_layer()
{
    if (g_wallpaper_layer.root != nullptr && lv_obj_is_valid(g_wallpaper_layer.root)) {
        return;
    }

    lv_obj_t * layer = lv_layer_bottom();
    if (layer == nullptr) {
        return;
    }

    g_wallpaper_layer.root = lv_obj_create(layer);
    lv_obj_remove_style_all(g_wallpaper_layer.root);
    lv_obj_set_size(g_wallpaper_layer.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_wallpaper_layer.root, 0, 0);
    lv_obj_set_style_radius(g_wallpaper_layer.root, 0, 0);
    lv_obj_set_style_bg_color(g_wallpaper_layer.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_wallpaper_layer.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_opa(g_wallpaper_layer.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(g_wallpaper_layer.root, false, 0);
    lv_obj_clear_flag(g_wallpaper_layer.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_wallpaper_layer.root, LV_OBJ_FLAG_SCROLLABLE);
}

extern "C" const lv_font_t * smartwatch_ui_runtime_get_text_font(void)
{
    auto & theme_manager = LvglThemeManager::GetInstance();
    auto * theme = theme_manager.GetTheme("dark");
    if (theme == nullptr || theme->text_font() == nullptr) {
        return nullptr;
    }
    return theme->text_font()->font();
}

extern "C" bool smartwatch_ui_runtime_is_ai_speaking(void)
{
    return Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
}

extern "C" void smartwatch_ui_runtime_refresh_wake_word_detection(void)
{
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().RefreshWakeWordDetection();
    });
}

extern "C" void smartwatch_ui_runtime_exit_ai_chat_to_standby(void)
{
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().ExitAiChatToStandby();
    });
}

extern "C" bool smartwatch_ui_runtime_is_sound_idle(void)
{
    return Application::GetInstance().GetAudioService().IsIdle();
}

extern "C" void smartwatch_ui_runtime_play_alarm_sound(void)
{
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_INFORM);
}

extern "C" void smartwatch_ui_runtime_stop_sound(void)
{
    Application::GetInstance().GetAudioService().ResetDecoder();
}

extern "C" bool smartwatch_ui_runtime_get_asset(const char * name, const uint8_t ** data, size_t * size)
{
    if (name == nullptr || data == nullptr || size == nullptr) {
        return false;
    }

    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        return false;
    }

    void* ptr = nullptr;
    size_t asset_size = 0;
    if (!assets.GetAssetData(name, ptr, asset_size) || ptr == nullptr || asset_size == 0) {
        return false;
    }

    *data = static_cast<const uint8_t*>(ptr);
    *size = asset_size;
    return true;
}

extern "C" bool smartwatch_ui_runtime_get_remote_ai_chat_mjpeg(bool speaking,
                                                                  const uint8_t ** data,
                                                                  size_t * size)
{
    if (data == nullptr || size == nullptr) {
        return false;
    }

    smartwatch_ui_runtime_ensure_remote_ai_chat_mjpeg_cache();
    const uint8_t index = speaking ? 1 : 0;
    if (g_remote_ai_chat_mjpeg[index] == nullptr || g_remote_ai_chat_mjpeg_size[index] == 0) {
        return false;
    }

    *data = g_remote_ai_chat_mjpeg[index];
    *size = g_remote_ai_chat_mjpeg_size[index];
    return true;
}

extern "C" void smartwatch_ui_runtime_reset_remote_ai_chat_mjpeg_cache(void)
{
    smartwatch_ui_runtime_release_remote_ai_chat_mjpeg_cache();
    g_remote_ai_chat_mjpeg_checked = false;
}

extern "C" void smartwatch_ui_runtime_wallpaper_set_visible(bool visible)
{
    smartwatch_ui_runtime_wallpaper_ensure_layer();
    if (g_wallpaper_layer.root == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(g_wallpaper_layer.root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_wallpaper_layer.root, LV_OBJ_FLAG_HIDDEN);
    }
}

extern "C" void smartwatch_ui_runtime_wallpaper_set_static(const lv_image_dsc_t * image)
{
    smartwatch_ui_runtime_wallpaper_ensure_layer();
    if (g_wallpaper_layer.root == nullptr) {
        return;
    }

    lv_obj_set_style_bg_image_src(g_wallpaper_layer.root, image, 0);
    lv_obj_clear_flag(g_wallpaper_layer.root, LV_OBJ_FLAG_HIDDEN);
}

extern "C" void smartwatch_ui_runtime_wallpaper_clear(void)
{
    smartwatch_ui_runtime_wallpaper_ensure_layer();
    if (g_wallpaper_layer.root == nullptr) {
        return;
    }
    lv_obj_set_style_bg_image_src(g_wallpaper_layer.root, nullptr, 0);
}

extern "C" void smartwatch_ui_runtime_wallpaper_reset(void)
{
    g_wallpaper_standby_refresh_pending = false;
    if (g_wallpaper_standby_refresh_timer != nullptr) {
        lv_timer_delete(g_wallpaper_standby_refresh_timer);
        g_wallpaper_standby_refresh_timer = nullptr;
    }
    if (g_wallpaper_layer.root != nullptr && lv_obj_is_valid(g_wallpaper_layer.root)) {
        /* Drop the image pointer before releasing the owning LvglImage data. */
        lv_obj_set_style_bg_image_src(g_wallpaper_layer.root, nullptr, 0);
    }
    ui_WallpaperScreen_release_preview_images();
    for (uint8_t mode = 0; mode < 2; ++mode) {
        g_wallpaper_preview_images[mode].clear();
        g_wallpaper_preview_interval_ms[mode] = 2000;
    }
    g_wallpaper_preview_last_mode = 0;
    g_wallpaper_remote_preview_loaded = false;
    if (g_wallpaper_layer.root != nullptr && lv_obj_is_valid(g_wallpaper_layer.root)) {
        lv_obj_delete(g_wallpaper_layer.root);
    }
    g_wallpaper_layer.root = nullptr;
}

void smartwatch_ui_runtime_wallpaper_preview_set_images(std::vector<std::unique_ptr<LvglImage>> images,
                                                        uint8_t mode, uint32_t interval_ms)
{
    g_wallpaper_remote_preview_loaded = true;
    smartwatch_ui_runtime_wallpaper_detach_old_resources();
    uint8_t preview_mode = mode == 1 ? 1 : 0;
    g_wallpaper_preview_images[preview_mode] = std::move(images);
    g_wallpaper_preview_interval_ms[preview_mode] = interval_ms >= 500 ? interval_ms : 2000;
    g_wallpaper_preview_last_mode = preview_mode;
    smartwatch_ui_runtime_wallpaper_select_latest_preview();
    ui_WallpaperScreen_reload_previews();
    smartwatch_ui_runtime_wallpaper_request_standby_refresh();
}

extern "C" void smartwatch_ui_runtime_wallpaper_preview_reload_remote(void)
{
    smartwatch_ui_runtime_wallpaper_apply_remote_state();
    ui_WallpaperScreen_reload_previews();
    smartwatch_ui_runtime_wallpaper_request_standby_refresh();
}

extern "C" uint32_t smartwatch_ui_runtime_wallpaper_preview_count(uint8_t mode)
{
    smartwatch_ui_runtime_wallpaper_ensure_remote_state();
    uint8_t preview_mode = mode == 1 ? 1 : 0;
    return static_cast<uint32_t>(g_wallpaper_preview_images[preview_mode].size());
}

extern "C" const lv_image_dsc_t * smartwatch_ui_runtime_wallpaper_preview_get(uint8_t mode, uint32_t index)
{
    smartwatch_ui_runtime_wallpaper_ensure_remote_state();
    uint8_t preview_mode = mode == 1 ? 1 : 0;
    if (index >= g_wallpaper_preview_images[preview_mode].size()) {
        return nullptr;
    }

    if (g_wallpaper_preview_images[preview_mode][index] == nullptr &&
        RemoteWallpaperStore::GetInstance().HasImages(preview_mode)) {
        g_wallpaper_preview_images[preview_mode][index] =
            RemoteWallpaperStore::GetInstance().LoadImage(preview_mode, index);
    }

    if (g_wallpaper_preview_images[preview_mode][index] == nullptr) {
        return nullptr;
    }
    return g_wallpaper_preview_images[preview_mode][index]->image_dsc();
}

extern "C" bool smartwatch_ui_runtime_wallpaper_preview_is_remote(uint8_t mode)
{
    smartwatch_ui_runtime_wallpaper_ensure_remote_state();
    uint8_t preview_mode = mode == 1 ? 1 : 0;
    return !g_wallpaper_preview_images[preview_mode].empty();
}

extern "C" uint32_t smartwatch_ui_runtime_wallpaper_preview_interval_ms(uint8_t mode)
{
    smartwatch_ui_runtime_wallpaper_ensure_remote_state();
    uint8_t preview_mode = mode == 1 ? 1 : 0;
    return g_wallpaper_preview_interval_ms[preview_mode];
}

extern "C" uint8_t smartwatch_ui_runtime_wallpaper_preview_last_mode(void)
{
    smartwatch_ui_runtime_wallpaper_ensure_remote_state();
    return g_wallpaper_preview_last_mode;
}

extern "C" smartwatch_ui_runtime_mjpeg_player_t * smartwatch_ui_runtime_mjpeg_player_create(lv_obj_t * target)
{
    if (target == nullptr) {
        return nullptr;
    }

    auto * player = new smartwatch_ui_runtime_mjpeg_player_t();
    player->target = target;
    return player;
}

extern "C" void smartwatch_ui_runtime_mjpeg_player_destroy(smartwatch_ui_runtime_mjpeg_player_t * player)
{
    if (player == nullptr) {
        return;
    }

    if (player->timer != nullptr) {
        lv_timer_delete(player->timer);
        player->timer = nullptr;
    }
    if (player->target != nullptr && lv_obj_is_valid(player->target)) {
        lv_image_set_src(player->target, nullptr);
    }
    smartwatch_ui_runtime_mjpeg_player_release_frame(player);
    delete player;
}

extern "C" bool smartwatch_ui_runtime_mjpeg_player_set_src(smartwatch_ui_runtime_mjpeg_player_t * player,
                                                            const uint8_t * data, size_t size)
{
    if (player == nullptr || player->target == nullptr || !lv_obj_is_valid(player->target) || data == nullptr ||
        size < 4) {
        return false;
    }

    if (player->timer != nullptr) {
        lv_timer_pause(player->timer);
    }

    lv_image_set_src(player->target, nullptr);
    smartwatch_ui_runtime_mjpeg_player_release_frame(player);
    player->data = data;
    player->size = size;
    player->next_offset = 0;

    if (!smartwatch_ui_runtime_mjpeg_player_decode_next_frame(player)) {
        player->data = nullptr;
        player->size = 0;
        player->next_offset = 0;
        return false;
    }

    if (player->timer == nullptr) {
        player->timer = lv_timer_create(smartwatch_ui_runtime_mjpeg_player_timer_cb, kMjpegFrameIntervalMs, player);
        if (player->timer == nullptr) {
            lv_image_set_src(player->target, nullptr);
            smartwatch_ui_runtime_mjpeg_player_release_frame(player);
            player->data = nullptr;
            player->size = 0;
            player->next_offset = 0;
            return false;
        }
    }

    lv_timer_set_period(player->timer, kMjpegFrameIntervalMs);
    lv_timer_resume(player->timer);
    lv_timer_reset(player->timer);
    return true;
}

extern "C" void smartwatch_ui_runtime_mjpeg_player_restart(smartwatch_ui_runtime_mjpeg_player_t * player)
{
    if (player == nullptr || player->data == nullptr || player->size < 4) {
        return;
    }

    if (player->timer != nullptr) {
        lv_timer_pause(player->timer);
    }

    player->next_offset = 0;
    lv_image_set_src(player->target, nullptr);
    smartwatch_ui_runtime_mjpeg_player_release_frame(player);
    if (!smartwatch_ui_runtime_mjpeg_player_decode_next_frame(player)) {
        return;
    }

    if (player->timer == nullptr) {
        player->timer = lv_timer_create(smartwatch_ui_runtime_mjpeg_player_timer_cb, kMjpegFrameIntervalMs, player);
        if (player->timer == nullptr) {
            lv_image_set_src(player->target, nullptr);
            smartwatch_ui_runtime_mjpeg_player_release_frame(player);
            return;
        }
    }

    lv_timer_set_period(player->timer, kMjpegFrameIntervalMs);
    lv_timer_resume(player->timer);
    lv_timer_reset(player->timer);
}

extern "C" void smartwatch_ui_runtime_mjpeg_player_set_visible(smartwatch_ui_runtime_mjpeg_player_t * player,
                                                                  bool visible)
{
    if (player == nullptr || player->timer == nullptr) {
        return;
    }

    if (visible) {
        lv_timer_resume(player->timer);
        lv_timer_reset(player->timer);
    } else {
        lv_timer_pause(player->timer);
    }
}

extern "C" bool smartwatch_ui_runtime_mjpeg_player_is_loaded(const smartwatch_ui_runtime_mjpeg_player_t * player)
{
    return player != nullptr && player->loaded && player->frame_data != nullptr;
}
