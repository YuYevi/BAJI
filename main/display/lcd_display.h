#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>
#include <esp_lcd_touch.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglTheme;
#ifndef CONFIG_BAJI_WIFI_ONLY
enum class BoardNetworkMode;
#endif

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_handle_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;
    esp_timer_handle_t touch_debug_timer_ = nullptr;
    bool touch_last_pressed_ = false;
    uint16_t touch_last_x_ = 0;
    uint16_t touch_last_y_ = 0;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
#ifndef CONFIG_BAJI_WIFI_ONLY
    lv_obj_t* ota_network_switch_container_ = nullptr;
    lv_obj_t* ota_wifi_switch_btn_ = nullptr;
    lv_obj_t* ota_wifi_switch_label_ = nullptr;
    lv_obj_t* ota_4g_switch_btn_ = nullptr;
    lv_obj_t* ota_4g_switch_label_ = nullptr;
#endif
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;
    bool smart_watch_ui_active_ = false;
#ifndef CONFIG_BAJI_WIFI_ONLY
    bool wifi_mode_switch_pending_ = false;
    bool ota_network_switch_pending_ = false;
#endif

    void InitializeLcdThemes();
    void ApplyContainerBackground(LvglTheme* theme);
    void InitializeTouch();  // 触摸初始化函数
#ifndef CONFIG_BAJI_WIFI_ONLY
    void CreateNetworkSwitchButtons(lv_obj_t* screen, const lv_font_t* text_font,
                                    LvglTheme* lvgl_theme);
    void UpdateWifiModeSwitchButton();
    void OnOtaNetworkSwitchButtonClicked(BoardNetworkMode target);
#else
    void UpdateWifiModeSwitchButton() {}
#endif
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetStatus(const char* status) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000) override;
    virtual void ShowPersistentNotification(const char* notification, bool top = false) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void ShowActivationQrCode(const char* code) override;
    virtual void ShowActivationPrompt(const char* message) override;
    virtual void HideActivationQrCode() override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;

    virtual void SetTheme(Theme* theme) override;

    void SetHideSubtitle(bool hide);

    void SetEmojiVisible(bool visible);
    void SetTouchEnabled(bool enabled);
    bool IsSmartWatchUiActive() const { return smart_watch_ui_active_; }
    bool SmartWatchUiBack();
    void SmartWatchUiShowStandby();
    void SmartWatchUiShowHome();
    bool IsSmartWatchAiChatActive();
};


class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, bool fill_splash_white = true);
};


class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};


class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif 
