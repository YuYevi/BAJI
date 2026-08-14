#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void ShowPersistentNotification(const char* notification, bool top = false) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void ShowChargingFullscreen(bool show) override;
    virtual void ShowActivationQrCode(const char* code) override;
    virtual void ShowActivationPrompt(const char* message) override;
    virtual void HideActivationQrCode() override;
    virtual void SetPowerSaveMode(bool on);
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80);

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    
    lv_obj_t* battery_stack_ = nullptr;
    lv_obj_t* center_status_row_ = nullptr;
    lv_obj_t* network_img_ = nullptr;
    lv_obj_t* battery_img_ = nullptr;
    lv_obj_t* battery_percent_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    lv_obj_t* charging_fullscreen_ = nullptr;
    lv_obj_t* activation_qr_overlay_ = nullptr;
    lv_obj_t* activation_qr_title_ = nullptr;
    lv_obj_t* activation_qr_hint_ = nullptr;
    lv_obj_t* activation_qr_code_ = nullptr;
    lv_obj_t* activation_prompt_wait_label_ = nullptr;
    lv_timer_t* activation_prompt_wait_timer_ = nullptr;
    uint8_t activation_prompt_wait_dot_count_ = 0;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    
    lv_obj_t* top_bar_layer_ref_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


#endif
