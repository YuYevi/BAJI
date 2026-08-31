#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <font_awesome.h>

#include "lvgl_display.h"
#if defined(BAJI_185_CENTER_STATUS_UI)
#include "boards/baji/BAJI/baji_185_status_icons.h"
#endif
#include "lvgl_theme.h"
#include "board.h"
#include "application.h"
#include "device_state.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Display"

namespace {
constexpr int kPersistentLowBatteryLevelPercent = 10;
constexpr uint32_t kActivationPromptWaitIntervalMs = 550;
constexpr const char* kActivationWaitingPrefix =
    "\xE7\xAD\x89\xE5\xBE\x85\xE6\xBF\x80\xE6\xB4\xBB\xE4\xB8\xAD";

void SetActivationWaitingText(lv_obj_t* label, uint8_t dot_count) {
    if (label == nullptr) {
        return;
    }

    const char* dots = dot_count == 1 ? "." : dot_count == 2 ? ".." : "...";
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s%s", kActivationWaitingPrefix, dots);
    lv_label_set_text(label, buffer);
}

const char* GetStartupNetworkModeIcon(Board& board, Application& app, const char* fallback_icon) {
    // GetNetworkStateIcon() is the authoritative view of the backend.  Do
    // not replace a valid signal icon merely because activation is still in
    // progress; doing so left the status bar stuck on the "offline" icon
    // while the device already had a usable link.
    (void)app;
    const BoardNetworkStatus status = board.GetNetworkStatus();
    const bool online = status.phase == BoardNetworkPhase::ONLINE && status.link_up;
    if (online && fallback_icon != nullptr && fallback_icon[0] != '\0') {
        return fallback_icon;
    }

    const BoardNetworkMode mode = status.target_mode != BoardNetworkMode::UNSUPPORTED
        ? status.target_mode
        : status.active_mode;
    if (mode == BoardNetworkMode::UNSUPPORTED) {
        return fallback_icon;
    }
    return mode == BoardNetworkMode::CELLULAR
        ? FONT_AWESOME_SIGNAL_OFF
        : FONT_AWESOME_WIFI_SLASH;
}

bool IsCellularSignalIcon(const char* icon) {
    if (icon == nullptr) {
        return false;
    }
    return strcmp(icon, FONT_AWESOME_SIGNAL_STRONG) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_GOOD) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_FAIR) == 0 ||
           strcmp(icon, FONT_AWESOME_SIGNAL_WEAK) == 0;
}

void SetNetworkLabelIcon(lv_obj_t* label, const char* icon) {
    if (label == nullptr) {
        return;
    }
    lv_label_set_text(label, icon);
    // Font Awesome's cellular glyphs have a lower visual baseline than the
    // Wi-Fi glyphs in both status-bar fonts.
    lv_obj_set_style_translate_y(label, IsCellularSignalIcon(icon) ? -1 : 0, 0);
}
}  // namespace

LvglDisplay::LvglDisplay() {
    
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (charging_fullscreen_ != nullptr) {
        lv_obj_del(charging_fullscreen_);
        charging_fullscreen_ = nullptr;
    }
    if (activation_prompt_wait_timer_ != nullptr) {
        lv_timer_delete(activation_prompt_wait_timer_);
        activation_prompt_wait_timer_ = nullptr;
    }
    if (activation_qr_overlay_ != nullptr) {
        lv_obj_del(activation_qr_overlay_);
        activation_qr_overlay_ = nullptr;
        activation_qr_title_ = nullptr;
        activation_qr_hint_ = nullptr;
        activation_qr_code_ = nullptr;
        activation_prompt_wait_label_ = nullptr;
        activation_prompt_wait_dot_count_ = 0;
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowPersistentNotification(const char* notification, bool top) {
    (void)top;
    ShowNotification(notification, 0);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    if (duration_ms <= 0) {
        return;
    }
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    
    {
        DisplayLockGuard lock(this);
        if (mute_label_ != nullptr) {
            if (codec->output_volume() == 0 && !muted_) {
                muted_ = true;
                lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
            } else if (codec->output_volume() > 0 && muted_) {
                muted_ = false;
                lv_label_set_text(mute_label_, "");
            }
        }
    }

    
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm);
                SetStatus(time_str);
            } else {
                
            }
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* icon = nullptr;
#if defined(BAJI_185_CENTER_STATUS_UI)
    if (battery_img_ != nullptr && battery_percent_label_ != nullptr && network_img_ != nullptr) {
        if (board.GetBatteryLevel(battery_level, charging, discharging)) {
            if (battery_level < 0) {
                battery_level = 0;
            } else if (battery_level > 100) {
                battery_level = 100;
            }
            const lv_image_dsc_t* bat_dsc = &baji185_bat_2;
            if (charging && board.IsBatteryFull()) {
                bat_dsc = &baji185_bat_4;
            } else if (charging) {
                bat_dsc = &baji185_bat_charge;
            } else {
                static const lv_image_dsc_t* kBat[] = {
                    &baji185_bat_0,
                    &baji185_bat_1,
                    &baji185_bat_2,
                    &baji185_bat_3,
                    &baji185_bat_4,
                };
                int idx = battery_level / 20;
                if (idx > 4) {
                    idx = 4;
                }
                bat_dsc = kBat[idx];
            }
            DisplayLockGuard lock(this);
            lv_image_set_src(battery_img_, bat_dsc);
            char pct[12];
            snprintf(pct, sizeof(pct), "%d%%", battery_level);
            lv_label_set_text(battery_percent_label_, pct);

            if (low_battery_popup_ != nullptr && !update_all) {
                const bool low_battery = battery_level <= kPersistentLowBatteryLevelPercent && discharging;
                if (low_battery) {
                    if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                        app.Schedule([&app]() {
                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                        });
                    }
                } else {
                    if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
                        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        }
    } else
#endif
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (battery_level < 0) {
            battery_level = 0;
        } else if (battery_level > 100) {
            battery_level = 100;
        }
        if (charging) {
            icon = board.IsBatteryFull() ? FONT_AWESOME_BATTERY_FULL : FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, 
                FONT_AWESOME_BATTERY_QUARTER,    
                FONT_AWESOME_BATTERY_HALF,    
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    
                FONT_AWESOME_BATTERY_FULL, 
                FONT_AWESOME_BATTERY_FULL, 
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        
        
        if (low_battery_popup_ != nullptr && !update_all) {
            const bool low_battery = battery_level <= kPersistentLowBatteryLevelPercent && discharging;
            if (low_battery) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { 
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.Schedule([&app]() {
                        app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                    });
                }
            } else {
                
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { 
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    
    
    {
        auto device_state = Application::GetInstance().GetDeviceState();
        if (device_state != kDeviceStateUpgrading) {
            icon = board.GetNetworkStateIcon();
            icon = GetStartupNetworkModeIcon(board, app, icon);
#if defined(BAJI_185_CENTER_STATUS_UI)
            if (network_img_ != nullptr) {
                const char* normalized_icon =
                    (icon != nullptr && icon[0] != '\0') ? icon : FONT_AWESOME_WIFI_SLASH;
                DisplayLockGuard lock(this);
                lv_obj_add_flag(network_img_, LV_OBJ_FLAG_HIDDEN);
                if (network_label_ != nullptr) {
                    if (network_icon_ != normalized_icon) {
                        network_icon_ = normalized_icon;
                    }
                    SetNetworkLabelIcon(network_label_, network_icon_);
                    lv_obj_remove_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
                }
            } else
#endif
            {
                static int seconds_counter = 0;
                if (update_all || seconds_counter++ % 10 == 0) {
                    const char* normalized_icon =
                        (icon != nullptr && icon[0] != '\0') ? icon : FONT_AWESOME_WIFI_SLASH;
                    if (network_label_ != nullptr && network_icon_ != normalized_icon) {
                        DisplayLockGuard lock(this);
                        network_icon_ = normalized_icon;
                        SetNetworkLabelIcon(network_label_, network_icon_);
                    }
                }
            }
        }
    }

#if defined(BAJI_185_CENTER_STATUS_UI)
    if (top_bar_layer_ref_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_obj_move_foreground(top_bar_layer_ref_);
    }
#endif

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::ShowChargingFullscreen(bool show) {
    DisplayLockGuard lock(this);
    if (!show) {
        if (charging_fullscreen_ != nullptr) {
            lv_obj_add_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (charging_fullscreen_ == nullptr) {
        lv_obj_t* scr = lv_screen_active();
        if (!setup_ui_called_) {
            lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        }
        charging_fullscreen_ = lv_obj_create(scr);
        lv_obj_set_size(charging_fullscreen_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(charging_fullscreen_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(charging_fullscreen_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(charging_fullscreen_, 0, 0);
        lv_obj_set_style_pad_all(charging_fullscreen_, 0, 0);
        lv_obj_align(charging_fullscreen_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(charging_fullscreen_, LV_OBJ_FLAG_SCROLLABLE);

        const lv_font_t* icon_font = LV_FONT_DEFAULT;
        const lv_font_t* text_font = LV_FONT_DEFAULT;
        if (current_theme_ != nullptr) {
            auto* th = static_cast<LvglTheme*>(current_theme_);
            icon_font = th->large_icon_font()->font();
            text_font = th->text_font()->font();
        }

        lv_obj_t* icon_lbl = lv_label_create(charging_fullscreen_);
        lv_label_set_text(icon_lbl, FONT_AWESOME_BATTERY_BOLT);
        lv_obj_set_style_text_font(icon_lbl, icon_font, 0);
        lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
        lv_obj_align(icon_lbl, LV_ALIGN_CENTER, 0, -lv_font_get_line_height(text_font));

        lv_obj_t* cap = lv_label_create(charging_fullscreen_);
        lv_label_set_text(cap, Lang::Strings::BATTERY_CHARGING);
        lv_obj_set_style_text_font(cap, text_font, 0);
        lv_obj_set_style_text_color(cap, lv_color_white(), 0);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(cap, LV_HOR_RES * 0.85);
        lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
        lv_obj_align_to(cap, icon_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    }
    lv_obj_remove_flag(charging_fullscreen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(charging_fullscreen_);
    lv_obj_invalidate(charging_fullscreen_);
    lv_refr_now(nullptr);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

void LvglDisplay::ShowActivationPrompt(const char* message) {
    LvglDisplay::HideActivationQrCode();

    DisplayLockGuard lock(this);

    lv_obj_t* layer = lv_layer_top();
    activation_qr_overlay_ = lv_obj_create(layer);
    lv_obj_remove_style_all(activation_qr_overlay_);
    lv_obj_set_size(activation_qr_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(activation_qr_overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(activation_qr_overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(activation_qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(activation_qr_overlay_);
    lv_obj_move_foreground(activation_qr_overlay_);

    const lv_font_t* text_font = LV_FONT_DEFAULT;
    const lv_font_t* icon_font = LV_FONT_DEFAULT;
    if (current_theme_ != nullptr) {
        auto* th = static_cast<LvglTheme*>(current_theme_);
        if (th->text_font() != nullptr) {
            text_font = th->text_font()->font();
        }
        if (th->large_icon_font() != nullptr) {
            icon_font = th->large_icon_font()->font();
        }
    }

    activation_qr_title_ = lv_label_create(activation_qr_overlay_);
    lv_label_set_text(activation_qr_title_, Lang::Strings::ACTIVATION);
    lv_obj_set_style_text_color(activation_qr_title_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_qr_title_, text_font, 0);
    lv_obj_align(activation_qr_title_, LV_ALIGN_TOP_MID, 0, 36);

    activation_qr_code_ = lv_label_create(activation_qr_overlay_);
    lv_label_set_text(activation_qr_code_, FONT_AWESOME_BLUETOOTH);
    lv_obj_set_style_text_color(activation_qr_code_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_qr_code_, icon_font, 0);
    lv_obj_align(activation_qr_code_, LV_ALIGN_CENTER, 0, -lv_font_get_line_height(text_font));

    activation_qr_hint_ = lv_label_create(activation_qr_overlay_);
    lv_label_set_text(activation_qr_hint_,
                      (message != nullptr && message[0] != '\0') ? message : Lang::Strings::PLEASE_WAIT);
    lv_obj_set_style_text_color(activation_qr_hint_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_qr_hint_, text_font, 0);
    lv_obj_set_style_text_align(activation_qr_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(activation_qr_hint_, LV_HOR_RES * 0.85);
    lv_label_set_long_mode(activation_qr_hint_, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(activation_qr_hint_, activation_qr_code_, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);

    activation_prompt_wait_dot_count_ = 1;
    activation_prompt_wait_label_ = lv_label_create(activation_qr_overlay_);
    SetActivationWaitingText(activation_prompt_wait_label_, activation_prompt_wait_dot_count_);
    lv_obj_set_style_text_color(activation_prompt_wait_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_prompt_wait_label_, text_font, 0);
    lv_obj_set_style_text_align(activation_prompt_wait_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(activation_prompt_wait_label_, LV_HOR_RES * 0.85);
    lv_label_set_long_mode(activation_prompt_wait_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(activation_prompt_wait_label_, LV_ALIGN_BOTTOM_MID, 0, -36);

    activation_prompt_wait_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* display = static_cast<LvglDisplay*>(lv_timer_get_user_data(timer));
            if (display == nullptr || display->activation_prompt_wait_label_ == nullptr) {
                return;
            }

            display->activation_prompt_wait_dot_count_ =
                display->activation_prompt_wait_dot_count_ % 3 + 1;
            SetActivationWaitingText(display->activation_prompt_wait_label_,
                                     display->activation_prompt_wait_dot_count_);
        },
        kActivationPromptWaitIntervalMs, this);

    lv_obj_invalidate(activation_qr_overlay_);
    lv_refr_now(nullptr);
}

void LvglDisplay::ShowActivationQrCode(const char* code) {
    DisplayLockGuard lock(this);
    
    LvglDisplay::HideActivationQrCode();
    
    lv_obj_t* layer = lv_layer_top();
    activation_qr_overlay_ = lv_obj_create(layer);
    lv_obj_remove_style_all(activation_qr_overlay_);
    lv_obj_set_size(activation_qr_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(activation_qr_overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(activation_qr_overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(activation_qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(activation_qr_overlay_);
    lv_obj_move_foreground(activation_qr_overlay_);

    const lv_font_t* text_font = LV_FONT_DEFAULT;
    if (current_theme_ != nullptr) {
        auto* th = static_cast<LvglTheme*>(current_theme_);
        text_font = th->text_font()->font();
    }

    activation_qr_title_ = lv_label_create(activation_qr_overlay_);
    lv_label_set_text(activation_qr_title_, Lang::Strings::ACTIVATION);
    lv_obj_set_style_text_color(activation_qr_title_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_qr_title_, text_font, 0);
    lv_obj_align(activation_qr_title_, LV_ALIGN_TOP_MID, 0, 36);

    activation_qr_code_ = lv_qrcode_create(activation_qr_overlay_);
    lv_qrcode_set_size(activation_qr_code_, 150);
    lv_qrcode_set_dark_color(activation_qr_code_, lv_color_black());
    lv_qrcode_set_light_color(activation_qr_code_, lv_color_white());
    lv_qrcode_update(activation_qr_code_, code, strlen(code));
    lv_obj_align_to(activation_qr_code_, activation_qr_title_, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);

    activation_qr_hint_ = lv_label_create(activation_qr_overlay_);
    lv_label_set_text(activation_qr_hint_, "请前往小程序扫码绑定设备");
    lv_obj_set_style_text_color(activation_qr_hint_, lv_color_black(), 0);
    lv_obj_set_style_text_font(activation_qr_hint_, text_font, 0);
    lv_obj_set_style_text_align(activation_qr_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(activation_qr_hint_, LV_HOR_RES * 0.85);
    lv_label_set_long_mode(activation_qr_hint_, LV_LABEL_LONG_WRAP);
    lv_obj_align(activation_qr_hint_, LV_ALIGN_BOTTOM_MID, 0, -64);
}

void LvglDisplay::HideActivationQrCode() {
    DisplayLockGuard lock(this);
    if (activation_prompt_wait_timer_ != nullptr) {
        lv_timer_delete(activation_prompt_wait_timer_);
        activation_prompt_wait_timer_ = nullptr;
    }
    if (activation_qr_overlay_ != nullptr) {
        lv_obj_del(activation_qr_overlay_);
        activation_qr_overlay_ = nullptr;
        activation_qr_title_ = nullptr;
        activation_qr_hint_ = nullptr;
        activation_qr_code_ = nullptr;
        activation_prompt_wait_label_ = nullptr;
        activation_prompt_wait_dot_count_ = 0;
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        
        return false;
    }

    
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    
    jpeg_data.clear();

    
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    
    return false;
#endif
}
