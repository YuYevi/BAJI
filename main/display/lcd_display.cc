/**
 * @file lcd_display.cc
 * @brief LCD显示模块实现文件
 * @details 包含LcdDisplay基类及其子类(SpiLcdDisplay/RgbLcdDisplay/MipiLcdDisplay)的实现
 */

#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_lcd_touch.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"
#include "application.h"
#include "config.h"
#include "ui_runtime.h"

#include <esp_lcd_touch_cst816s.h>

#if defined(BAJI_185_CENTER_STATUS_UI)
#include "boards/baji/BAJI/baji_185_status_icons.h"
#endif

#define TAG "LcdDisplay"

#ifndef LCD_TOUCH_DEBUG_POLL_ENABLED
#define LCD_TOUCH_DEBUG_POLL_ENABLED 0
#endif

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

extern "C" void baji_lcd_te_attach_display(lv_display_t* display) __attribute__((weak));

namespace {
bool ShouldEnterSmartWatchAiChat(DeviceState state) {
    return state == kDeviceStateConnecting ||
           state == kDeviceStateListening ||
           state == kDeviceStateSpeaking;
}

const char* GetInitialNetworkModeIcon(Board& board) {
    const char* icon = board.GetNetworkStateIcon();
    if (icon != nullptr && icon[0] != '\0') {
        return icon;
    }

    switch (board.GetActiveNetworkMode()) {
        case BoardNetworkMode::CELLULAR:
            return FONT_AWESOME_SIGNAL_OFF;
        case BoardNetworkMode::WIFI:
            return FONT_AWESOME_WIFI_SLASH;
        case BoardNetworkMode::UNSUPPORTED:
        default:
            return FONT_AWESOME_WIFI_SLASH;
    }
}

bool IsNetworkTransition(BoardNetworkPhase phase) {
    return phase == BoardNetworkPhase::CONNECTING ||
           phase == BoardNetworkPhase::SWITCHING ||
           phase == BoardNetworkPhase::PROVISIONING;
}
}  // namespace

/**
 * @brief 初始化LCD主题
 * @details 创建亮色和暗色两种主题，并注册到主题管理器
 */
void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // 创建亮色主题
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // 创建暗色主题
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    // 注册主题到管理器
    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

void LcdDisplay::InitializeTouch() {
#if defined(CST816S_I2C_ADDR) && defined(CST816S_TOUCH_INT_PIN) && defined(CST816S_TOUCH_RST_PIN)
    if (display_ == nullptr) {
        return;
    }
    if (touch_indev_ != nullptr || touch_handle_ != nullptr) {
        return;
    }

    extern i2c_master_bus_handle_t baji_185_get_i2c_bus(void);
    i2c_master_bus_handle_t i2c_bus = baji_185_get_i2c_bus();

    const esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = CST816S_I2C_ADDR,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &touch_io_));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = static_cast<uint16_t>(width_),
        .y_max = static_cast<uint16_t>(height_),
        .rst_gpio_num =
#if defined(QSPI_PIN_NUM_LCD_RST_VIRTUAL)
            (CST816S_TOUCH_RST_PIN == QSPI_PIN_NUM_LCD_RST_VIRTUAL ? GPIO_NUM_NC : CST816S_TOUCH_RST_PIN),
#else
            CST816S_TOUCH_RST_PIN,
#endif
        .int_gpio_num = CST816S_TOUCH_INT_PIN,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
        .process_coordinates = nullptr,
        .interrupt_callback = nullptr,
        .user_data = nullptr,
        .driver_data = nullptr,
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(touch_io_, &tp_cfg, &touch_handle_));

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display_,
        .handle = touch_handle_,
        .scale = {
            .x = 1.0f,
            .y = 1.0f,
        },
    };
    touch_indev_ = lvgl_port_add_touch(&touch_cfg);

    ESP_LOGW(TAG, "Touch enabled addr=0x%02X int=%d rst=%d", CST816S_I2C_ADDR, CST816S_TOUCH_INT_PIN, tp_cfg.rst_gpio_num);

#if LCD_TOUCH_DEBUG_POLL_ENABLED
    const esp_timer_create_args_t touch_debug_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            if (display->touch_handle_ == nullptr) {
                return;
            }

            if (esp_lcd_touch_read_data(display->touch_handle_) != ESP_OK) {
                return;
            }

            esp_lcd_touch_point_data_t point = {};
            uint8_t point_cnt = 0;
            if (esp_lcd_touch_get_data(display->touch_handle_, &point, &point_cnt, 1) != ESP_OK) {
                return;
            }

            const bool pressed = (point_cnt > 0);
            if (pressed != display->touch_last_pressed_ ||
                (pressed && (point.x != display->touch_last_x_ || point.y != display->touch_last_y_))) {
                if (pressed) {
                    ESP_LOGW(TAG, "Touch down x=%" PRIu16 " y=%" PRIu16, point.x, point.y);
                    display->touch_last_x_ = point.x;
                    display->touch_last_y_ = point.y;
                } else {
                    ESP_LOGW(TAG, "Touch up");
                }
                display->touch_last_pressed_ = pressed;
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "touch_dbg",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&touch_debug_timer_args, &touch_debug_timer_) == ESP_OK) {
        esp_timer_start_periodic(touch_debug_timer_, 200 * 1000);
    }
#endif
#endif
}

/**
 * @brief LcdDisplay构造函数
 * @param panel_io LCD面板IO句柄
 * @param panel LCD面板句柄
 * @param width 屏幕宽度
 * @param height 屏幕高度
 */
LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // 初始化LCD主题
    InitializeLcdThemes();

    // 从设置中加载主题配置
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // 创建预览图片定时器
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

}

/**
 * @brief SPI LCD显示器构造函数
 * @param panel_io LCD面板IO句柄
 * @param panel LCD面板句柄
 * @param width 屏幕宽度
 * @param height 屏幕高度
 * @param offset_x X轴偏移量
 * @param offset_y Y轴偏移量
 * @param mirror_x X轴镜像
 * @param mirror_y Y轴镜像
 * @param swap_xy 交换XY轴
 * @param fill_splash_white 启动画面是否填充白色
 */
SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy, bool fill_splash_white)
    : LcdDisplay(panel_io, panel, width, height) {

    // 绘制启动画面填充色
    const uint16_t fill = fill_splash_white ? 0xFFFF : 0x0000;
    std::vector<uint16_t> buffer(width_, fill);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // 开启显示面板
    esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
    if (__err != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(__err);
    }

    // 初始化LVGL库
    lv_init();

#if CONFIG_SPIRAM
    // 根据PSRAM大小配置LVGL图像缓存
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
    }
#endif

    // 配置LVGL端口
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 5;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    // 配置显示参数
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    // 添加显示设备
    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        return;
    }

    // 设置显示偏移
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
    
    InitializeTouch();
    if (baji_lcd_te_attach_display) {
        baji_lcd_te_attach_display(display_);
    }
}



/**
 * @brief RGB LCD显示器构造函数
 * @param panel_io LCD面板IO句柄
 * @param panel LCD面板句柄
 * @param width 屏幕宽度
 * @param height 屏幕高度
 * @param offset_x X轴偏移量
 * @param offset_y Y轴偏移量
 * @param mirror_x X轴镜像
 * @param mirror_y Y轴镜像
 * @param swap_xy 交换XY轴
 */
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y,
                             bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // 绘制白色启动画面
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // 初始化LVGL库
    lv_init();

    // 配置LVGL端口
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 5;
    lvgl_port_init(&port_cfg);

    // 配置显示参数
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    // 配置RGB显示参数
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    // 添加RGB显示设备
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        return;
    }
    
    // 设置显示偏移
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    InitializeTouch();
}

/**
 * @brief MIPI LCD显示器构造函数
 * @param panel_io LCD面板IO句柄
 * @param panel LCD面板句柄
 * @param width 屏幕宽度
 * @param height 屏幕高度
 * @param offset_x X轴偏移量
 * @param offset_y Y轴偏移量
 * @param mirror_x X轴镜像
 * @param mirror_y Y轴镜像
 * @param swap_xy 交换XY轴
 */
MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y,
                               bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // 初始化LVGL库
    lv_init();

    // 配置LVGL端口
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.timer_period_ms = 5;
    lvgl_port_init(&port_cfg);

    // 配置显示参数
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
        },
    };

    // 配置DSI显示参数
    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };

    // 添加DSI显示设备
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        return;
    }

    // 设置显示偏移
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    InitializeTouch();
}

/**
 * @brief LcdDisplay析构函数
 * @details 释放所有资源，包括定时器、LVGL对象和LCD面板
 */
LcdDisplay::~LcdDisplay() {
    // 清除预览图片
    SetPreviewImage(nullptr);

    // 停止并释放GIF控制器
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    // 停止并删除定时器
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }
    if (touch_debug_timer_ != nullptr) {
        esp_timer_stop(touch_debug_timer_);
        esp_timer_delete(touch_debug_timer_);
        touch_debug_timer_ = nullptr;
    }

    // 删除LVGL对象
    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (ota_network_switch_container_ != nullptr) {
        ota_wifi_switch_btn_ = nullptr;
        ota_wifi_switch_label_ = nullptr;
        ota_4g_switch_btn_ = nullptr;
        ota_4g_switch_label_ = nullptr;
        lv_obj_del(ota_network_switch_container_);
    }
    if (status_bar_ != nullptr) {
        notification_label_ = nullptr;
        status_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        battery_label_ = nullptr;
        mute_label_ = nullptr;
        battery_stack_ = nullptr;
        center_status_row_ = nullptr;
        network_img_ = nullptr;
        battery_img_ = nullptr;
        battery_percent_label_ = nullptr;
        top_bar_layer_ref_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }

    // 清理背景图片并删除容器
    if (container_ != nullptr) {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (touch_indev_ != nullptr) {
        lvgl_port_remove_touch(touch_indev_);
        touch_indev_ = nullptr;
    }
    if (touch_handle_ != nullptr) {
        esp_lcd_touch_del(touch_handle_);
        touch_handle_ = nullptr;
    }
    if (touch_io_ != nullptr) {
        esp_lcd_panel_io_del(touch_io_);
        touch_io_ = nullptr;
    }

    // 删除显示设备和面板
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

/**
 * @brief 获取LVGL显示锁
 * @param timeout_ms 超时时间(毫秒)
 * @return true表示获取锁成功，false表示超时
 */
bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

/**
 * @brief 释放LVGL显示锁
 */
void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::CreateNetworkSwitchButtons(lv_obj_t* screen, const lv_font_t* text_font,
                                            LvglTheme* lvgl_theme) {
    if (screen == nullptr || text_font == nullptr || lvgl_theme == nullptr ||
        ota_network_switch_container_ != nullptr) {
        return;
    }

    ota_network_switch_container_ = lv_obj_create(screen);
    lv_obj_set_size(ota_network_switch_container_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ota_network_switch_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ota_network_switch_container_, 0, 0);
    lv_obj_set_style_shadow_width(ota_network_switch_container_, 0, 0);
    lv_obj_set_style_pad_all(ota_network_switch_container_, 0, 0);
    lv_obj_set_style_pad_column(ota_network_switch_container_, lvgl_theme->spacing(2), 0);
    lv_obj_set_flex_flow(ota_network_switch_container_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ota_network_switch_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ota_network_switch_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ota_network_switch_container_, LV_OBJ_FLAG_HIDDEN);

    auto style_ota_switch_btn = [lvgl_theme, text_font](lv_obj_t* btn, uint32_t color) {
        lv_obj_set_size(btn, 76, text_font->line_height + lvgl_theme->spacing(7));
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x6B7280), LV_STATE_DISABLED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_DISABLED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, lvgl_theme->spacing(3), 0);
        lv_obj_set_style_pad_ver(btn, lvgl_theme->spacing(2), 0);
    };

    ota_wifi_switch_btn_ = lv_btn_create(ota_network_switch_container_);
    style_ota_switch_btn(ota_wifi_switch_btn_, 0x0F766E);
    lv_obj_add_event_cb(
        ota_wifi_switch_btn_,
        [](lv_event_t* e) {
            auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (display == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            display->OnOtaNetworkSwitchButtonClicked(BoardNetworkMode::WIFI);
        },
        LV_EVENT_CLICKED, this);
    ota_wifi_switch_label_ = lv_label_create(ota_wifi_switch_btn_);
    lv_label_set_text(ota_wifi_switch_label_, "WiFi");
    lv_obj_set_style_text_color(ota_wifi_switch_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(ota_wifi_switch_label_, text_font, 0);
    lv_obj_center(ota_wifi_switch_label_);

    ota_4g_switch_btn_ = lv_btn_create(ota_network_switch_container_);
    style_ota_switch_btn(ota_4g_switch_btn_, 0x2563EB);
    lv_obj_add_event_cb(
        ota_4g_switch_btn_,
        [](lv_event_t* e) {
            auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(e));
            if (display == nullptr || lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            display->OnOtaNetworkSwitchButtonClicked(BoardNetworkMode::CELLULAR);
        },
        LV_EVENT_CLICKED, this);
    ota_4g_switch_label_ = lv_label_create(ota_4g_switch_btn_);
    lv_label_set_text(ota_4g_switch_label_, "4G");
    lv_obj_set_style_text_color(ota_4g_switch_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(ota_4g_switch_label_, text_font, 0);
    lv_obj_center(ota_4g_switch_label_);
}

void LcdDisplay::UpdateWifiModeSwitchButton() {
    if (ota_network_switch_container_ == nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    const BoardNetworkStatus network_status = board.GetNetworkStatus();
    BoardNetworkMode mode = network_status.active_mode;
    DeviceState state = app.GetDeviceState();
    const bool wifi_provisioning = state == kDeviceStateWifiConfiguring &&
                                   network_status.phase == BoardNetworkPhase::PROVISIONING;
    const bool network_switching = IsNetworkTransition(network_status.phase) &&
                                   !wifi_provisioning;
    const BoardNetworkMode selected_mode = mode != BoardNetworkMode::UNSUPPORTED
        ? mode
        : network_status.target_mode;
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    lv_coord_t bottom_offset = lvgl_theme != nullptr ? static_cast<lv_coord_t>(lvgl_theme->spacing(6)) : 12;
    lv_coord_t gap = lvgl_theme != nullptr ? static_cast<lv_coord_t>(lvgl_theme->spacing(3)) : 6;

    if (state != kDeviceStateWifiConfiguring || mode == BoardNetworkMode::CELLULAR) {
        wifi_mode_switch_pending_ = false;
    }

    const bool ota_upgrade_in_progress = app.IsOtaUpgradeInProgress();
    const bool app_ota_network_switch_pending = app.IsOtaNetworkSwitchPending();
    const bool network_selectable = mode != BoardNetworkMode::UNSUPPORTED ||
                                    network_status.phase == BoardNetworkPhase::OFFLINE ||
                                    network_status.phase == BoardNetworkPhase::FAILED ||
                                    network_switching ||
                                    wifi_provisioning;
    if (!ota_upgrade_in_progress || !app_ota_network_switch_pending) {
        ota_network_switch_pending_ = false;
    }

    bool show_wifi_config_switch = state == kDeviceStateWifiConfiguring &&
                                   (!wifi_mode_switch_pending_ || network_switching) &&
                                   network_selectable &&
                                   mode != BoardNetworkMode::CELLULAR;
    bool show_ota_switch = ota_upgrade_in_progress &&
                           state != kDeviceStateWifiConfiguring &&
                           (!app_ota_network_switch_pending || network_switching) &&
                           (!ota_network_switch_pending_ || network_switching) &&
                           network_selectable;
    if (!show_wifi_config_switch && !show_ota_switch) {
        if (!ota_upgrade_in_progress || !app_ota_network_switch_pending) {
            ota_network_switch_pending_ = false;
        }
        lv_obj_add_flag(ota_network_switch_container_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (network_switching) {
        lv_obj_add_state(ota_wifi_switch_btn_, LV_STATE_DISABLED);
        lv_obj_add_state(ota_4g_switch_btn_, LV_STATE_DISABLED);
    } else if (selected_mode == BoardNetworkMode::WIFI) {
        lv_obj_add_state(ota_wifi_switch_btn_, LV_STATE_DISABLED);
        lv_obj_clear_state(ota_4g_switch_btn_, LV_STATE_DISABLED);
    } else if (selected_mode == BoardNetworkMode::CELLULAR) {
        lv_obj_clear_state(ota_wifi_switch_btn_, LV_STATE_DISABLED);
        lv_obj_add_state(ota_4g_switch_btn_, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ota_wifi_switch_btn_, LV_STATE_DISABLED);
        lv_obj_clear_state(ota_4g_switch_btn_, LV_STATE_DISABLED);
    }

    if (bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align_to(ota_network_switch_container_, bottom_bar_, LV_ALIGN_OUT_TOP_MID, 0, -gap);
    } else {
        lv_obj_align(ota_network_switch_container_, LV_ALIGN_BOTTOM_MID, 0, -bottom_offset);
    }

    lv_obj_move_foreground(ota_network_switch_container_);
    if (top_bar_layer_ref_ != nullptr) {
        lv_obj_move_foreground(top_bar_layer_ref_);
    }
    lv_obj_clear_flag(ota_network_switch_container_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::OnOtaNetworkSwitchButtonClicked(BoardNetworkMode target) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    const BoardNetworkStatus network_status = board.GetNetworkStatus();
    BoardNetworkMode mode = network_status.active_mode;
    const bool leaving_wifi_provisioning =
        app.GetDeviceState() == kDeviceStateWifiConfiguring &&
        network_status.phase == BoardNetworkPhase::PROVISIONING &&
        target == BoardNetworkMode::CELLULAR;
    if (IsNetworkTransition(network_status.phase) && !leaving_wifi_provisioning) {
        UpdateWifiModeSwitchButton();
        return;
    }
    if (network_status.link_up && mode == target) {
        ota_network_switch_pending_ = false;
        UpdateWifiModeSwitchButton();
        return;
    }

    bool switch_started = false;
    if (app.IsOtaUpgradeInProgress()) {
        ota_network_switch_pending_ = true;
        switch_started = app.RequestOtaNetworkSwitch(target);
    } else {
        wifi_mode_switch_pending_ = true;
        switch_started = board.SwitchActiveNetworkMode(target);
    }

    if (!switch_started) {
        ota_network_switch_pending_ = false;
        wifi_mode_switch_pending_ = false;
    }
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::SetStatus(const char* status) {
    if (!smart_watch_ui_active_) {
        LvglDisplay::SetStatus(status);
        DisplayLockGuard lock(this);
        UpdateWifiModeSwitchButton();
        return;
    }

    DisplayLockGuard lock(this);
    if (ShouldEnterSmartWatchAiChat(Application::GetInstance().GetDeviceState())) {
        smartwatch_ui_runtime_show_ai_chat();
    }
    smartwatch_ui_runtime_set_status(status);
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LcdDisplay::ShowPersistentNotification(const char* notification, bool top) {
    if (!smart_watch_ui_active_) {
        LvglDisplay::ShowPersistentNotification(notification, top);
        return;
    }

    DisplayLockGuard lock(this);
    if (top) {
        smartwatch_ui_runtime_show_top_notification(notification);
        return;
    }
    smartwatch_ui_runtime_show_notification(notification, 0);
}

void LcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!smart_watch_ui_active_) {
        LvglDisplay::ShowNotification(notification, duration_ms);
        return;
    }

    DisplayLockGuard lock(this);
    smartwatch_ui_runtime_show_notification(notification, static_cast<uint32_t>(duration_ms));
}

void LcdDisplay::UpdateStatusBar(bool update_all) {
    if (!smart_watch_ui_active_) {
        LvglDisplay::UpdateStatusBar(update_all);
        return;
    }

    Application::GetInstance().RefreshWakeWordDetection();

    auto& board = Board::GetInstance();
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    bool have_battery = board.GetBatteryLevel(battery_level, charging, discharging);
    const char* icon = board.GetNetworkStateIcon();
    DisplayLockGuard lock(this);
    smartwatch_ui_runtime_set_network_icon(icon);
    if (have_battery) {
        if (battery_level < 0) {
            battery_level = 0;
        } else if (battery_level > 100) {
            battery_level = 100;
        }
        smartwatch_ui_runtime_set_battery(static_cast<uint8_t>(battery_level), charging);
    }
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::ShowActivationQrCode(const char* code) {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        smartwatch_ui_runtime_show_top_notification("");
        smartwatch_ui_runtime_show_notification("", 0);
        smartwatch_ui_runtime_show_standby();
    }
    LvglDisplay::ShowActivationQrCode(code);
}

void LcdDisplay::ShowActivationPrompt(const char* message) {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        smartwatch_ui_runtime_show_top_notification("");
        smartwatch_ui_runtime_show_notification("", 0);
        smartwatch_ui_runtime_show_standby();
    }
    LvglDisplay::ShowActivationPrompt(message);
}

void LcdDisplay::HideActivationQrCode() {
    LvglDisplay::HideActivationQrCode();

    if (smart_watch_ui_active_) {
        return;
    }

    DisplayLockGuard lock(this);
    smartwatch_ui_runtime_init();
    auto& board = Board::GetInstance();
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    smartwatch_ui_runtime_set_network_icon(GetInitialNetworkModeIcon(board));
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (battery_level < 0) {
            battery_level = 0;
        } else if (battery_level > 100) {
            battery_level = 100;
        }
        smartwatch_ui_runtime_set_battery(static_cast<uint8_t>(battery_level), charging);
    }
    smart_watch_ui_active_ = true;
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::SetPowerSaveMode(bool on) {
    if (!smart_watch_ui_active_) {
        LvglDisplay::SetPowerSaveMode(on);
        return;
    }

    DisplayLockGuard lock(this);
    if (on) {
        smartwatch_ui_runtime_set_emotion("sleepy");
        smartwatch_ui_runtime_clear_chat_messages();
        smartwatch_ui_runtime_show_standby();
        return;
    }

    smartwatch_ui_runtime_set_emotion("neutral");
}

bool LcdDisplay::SmartWatchUiBack() {
    if (!smart_watch_ui_active_) {
        return false;
    }

    DisplayLockGuard lock(this);
    return smartwatch_ui_runtime_back();
}

void LcdDisplay::SmartWatchUiShowStandby() {
    if (!smart_watch_ui_active_) {
        return;
    }

    DisplayLockGuard lock(this);
    smartwatch_ui_runtime_show_standby();
}

void LcdDisplay::SmartWatchUiShowHome() {
    if (!smart_watch_ui_active_) {
        return;
    }

    DisplayLockGuard lock(this);
    smartwatch_ui_runtime_show_home();
}

bool LcdDisplay::IsSmartWatchAiChatActive() {
    if (!smart_watch_ui_active_) {
        return false;
    }

    DisplayLockGuard lock(this);
    return smartwatch_ui_runtime_is_ai_chat_active();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
/**
 * @brief 初始化微信消息风格UI
 * @details 创建容器、顶部栏、状态栏、内容区域和低电量弹窗等UI组件
 */
void LcdDisplay::SetupUI() {
    // 防止重复调用
    if (setup_ui_called_) {
        return;
    }

    // 调用父类初始化
    Display::SetupUI();
    DisplayLockGuard lock(this);

    // 获取主题和字体
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    // 设置屏幕样式
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    // 创建主容器
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    // 创建顶部栏
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // 创建网络状态标签
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // 创建右侧图标容器
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 创建静音标签
    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    // 创建电池标签
    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    // 创建状态栏
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // 创建通知标签
    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    // 创建状态标签
    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    // 创建内容区域
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0);

    // 设置内容区域滚动属性
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);

    // 初始化聊天消息标签
    chat_message_label_ = nullptr;

    CreateNetworkSwitchButtons(screen, text_font, lvgl_theme);

    // 创建低电量弹窗
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // 创建表情图像
    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // 创建表情标签
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, "");

    // 应用容器背景
    ApplyContainerBackground(lvgl_theme);
    UpdateWifiModeSwitchButton();
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
/**
 * @brief 设置聊天消息（微信消息风格）
 * @param role 消息角色：user/assistant/system
 * @param content 消息内容
 */
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        if (role != nullptr && content != nullptr && content[0] != '\0' &&
            (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0)) {
            smartwatch_ui_runtime_show_ai_chat();
        }
        smartwatch_ui_runtime_set_chat_message(role, content);
        UpdateWifiModeSwitchButton();
        return;
    }

    if (!setup_ui_called_) {
        return;
    }

    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // 消息数量限制处理
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // 删除最早的消息
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            child_count = lv_obj_get_child_cnt(content_);
        }

        // 滚动到最新消息
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // 系统消息特殊处理：合并连续的系统消息
    if (strcmp(role, "system") == 0) {
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && 
                lv_obj_get_child_cnt(last_container) > 0) {
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && 
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // 删除上一条系统消息，合并显示
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // 非系统消息时隐藏表情
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    
    // 空内容不处理
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // 创建消息气泡
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // 创建消息文本
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // 计算消息宽度
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;
    lv_coord_t min_width = 20;

    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    if (text_width < min_width) {
        text_width = min_width;
    }

    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    
    // 根据角色设置样式
    if (strcmp(role, "user") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    
    // 根据角色设置对齐方式
    if (strcmp(role, "user") == 0) {
        // 用户消息右对齐
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // 系统消息居中
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // 助手消息左对齐
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    chat_message_label_ = msg_text;
}

/**
 * @brief 设置预览图片（微信消息风格）
 * @param image 预览图片对象
 */
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // 创建图片气泡容器
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // 创建图片对象
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // 计算图片缩放比例
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;
    lv_coord_t max_height = LV_VER_RES * 50 / 100;

    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    if (zoom > 256) {
        zoom = 256;
    }

    // 设置图片源和缩放
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // 设置图片资源自动释放回调
    LvglImage* raw_image = image.release();
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img;
        }
    }, LV_EVENT_DELETE, (void*)raw_image);

    // 设置气泡大小
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    lv_obj_center(preview_image);

    // 对齐并滚动到视图
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

/**
 * @brief 清除聊天消息（微信消息风格）
 */
void LcdDisplay::ClearChatMessages() {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        smartwatch_ui_runtime_clear_chat_messages();
        UpdateWifiModeSwitchButton();
        return;
    }

    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // 清空内容容器
    lv_obj_clean(content_);
    chat_message_label_ = nullptr;

    // 重置表情标签
    if (emoji_label_ != nullptr) {
        lv_label_set_text(emoji_label_, "");
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
}
#else
/**
 * @brief 初始化标准UI（非微信消息风格）
 * @details 创建容器、表情框、预览图片、顶部栏和状态栏等UI组件
 */
void LcdDisplay::SetupUI() {
    // 防止重复调用
    if (setup_ui_called_) {
        return;
    }

    // 调用父类初始化
    Display::SetupUI();
    DisplayLockGuard lock(this);

    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    // 设置屏幕样式
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    // 创建主容器
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    // 创建表情框
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    // 创建表情标签
    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, "");

    // 创建表情图像
    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // 创建预览图片
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

#if defined(BAJI_185_CENTER_STATUS_UI)
    lv_obj_set_style_pad_column(top_bar_, lvgl_theme->spacing(3), 0);

    
    lv_obj_t* left_spacer = lv_obj_create(top_bar_);
    lv_obj_set_size(left_spacer, lvgl_theme->spacing(2), 1);
    lv_obj_set_flex_grow(left_spacer, 1);
    lv_obj_set_style_bg_opa(left_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_spacer, 0, 0);

    center_status_row_ = lv_obj_create(top_bar_);
    lv_obj_set_height(center_status_row_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center_status_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center_status_row_, 0, 0);
    lv_obj_set_style_pad_all(center_status_row_, 0, 0);
    lv_obj_set_flex_flow(center_status_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(center_status_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(center_status_row_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_flex_grow(center_status_row_, 0, 0);
    lv_obj_set_style_min_width(center_status_row_, 88, 0);

    network_img_ = lv_image_create(center_status_row_);
    lv_image_set_src(network_img_, &baji185_wifi_slash);
    lv_obj_set_size(network_img_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_flag(network_img_, LV_OBJ_FLAG_HIDDEN);

    
    network_label_ = lv_label_create(center_status_row_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    battery_stack_ = lv_obj_create(center_status_row_);
    lv_obj_set_size(battery_stack_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(battery_stack_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_stack_, 0, 0);
    lv_obj_set_style_pad_all(battery_stack_, 0, 0);
    lv_obj_set_flex_flow(battery_stack_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_stack_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_stack_, lvgl_theme->spacing(2), 0);

    battery_img_ = lv_image_create(battery_stack_);
    lv_image_set_src(battery_img_, &baji185_bat_2);
    lv_obj_set_size(battery_img_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    battery_percent_label_ = lv_label_create(battery_stack_);
    lv_obj_set_style_text_font(battery_percent_label_, text_font, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(battery_percent_label_, "--%");
    lv_obj_set_style_text_align(battery_percent_label_, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t* right_spacer = lv_obj_create(top_bar_);
    lv_obj_set_size(right_spacer, lvgl_theme->spacing(2), 1);
    lv_obj_set_flex_grow(right_spacer, 1);
    lv_obj_set_style_bg_opa(right_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_spacer, 0, 0);

    battery_label_ = nullptr;

    mute_label_ = lv_label_create(top_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    top_bar_layer_ref_ = top_bar_;
    lv_obj_move_foreground(top_bar_);
#else
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);
#endif

    
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  
#if defined(BAJI_185_CENTER_STATUS_UI)
    lv_obj_align_to(status_bar_, top_bar_, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
#else
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  
#endif

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  

    
    {
        const int32_t bottom_text_shift_y = -2 * static_cast<int32_t>(text_font->line_height);
        lv_obj_set_style_translate_y(bottom_bar_, bottom_text_shift_y, 0);
    }
#else
    
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_pad_bottom(chat_message_label_, lvgl_theme->spacing(5), 0);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    
    {
        const int32_t bottom_text_shift_y = -2 * static_cast<int32_t>(text_font->line_height);
        lv_obj_set_style_translate_y(bottom_bar_, bottom_text_shift_y, 0);
    }

    
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  
#endif

    CreateNetworkSwitchButtons(screen, text_font, lvgl_theme);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    ApplyContainerBackground(lvgl_theme);
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        if (role != nullptr && content != nullptr && content[0] != '\0' &&
            (strcmp(role, "assistant") == 0 || strcmp(role, "user") == 0)) {
            smartwatch_ui_runtime_show_ai_chat();
        }
        smartwatch_ui_runtime_set_chat_message(role, content);
        return;
    }

    if (!setup_ui_called_) {
        
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    
    
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
    UpdateWifiModeSwitchButton();
}

void LcdDisplay::ClearChatMessages() {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        smartwatch_ui_runtime_clear_chat_messages();
        return;
    }

    DisplayLockGuard lock(this);
    
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateWifiModeSwitchButton();
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (smart_watch_ui_active_) {
        DisplayLockGuard lock(this);
        smartwatch_ui_runtime_set_emotion(emotion);
        return;
    }

    if (!setup_ui_called_) {
        
    }
    
    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (image->IsGif()) {
        
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::ApplyContainerBackground(LvglTheme* theme) {
    if (container_ == nullptr) {
        return;
    }
    LvglTheme* t = theme ? theme : static_cast<LvglTheme*>(current_theme_);
    lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    if (t->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, t->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_color(container_, t->background_color(), 0);
    }
}

void LcdDisplay::SetEmojiVisible(bool visible) {
    DisplayLockGuard lock(this);
    if (emoji_image_ != nullptr) {
        if (visible) {
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (emoji_label_ != nullptr) {
        if (visible) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    
    lv_obj_t* screen = lv_screen_active();

    
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
        }
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, icon_font, 0);
        }
    }

    
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    ApplyContainerBackground(lvgl_theme);

    
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    }
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    }
    if (battery_percent_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_percent_label_, lvgl_theme->text_color(), 0);
    }
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        
        
        
        if (lv_obj_get_child_cnt(obj) > 0) {
            
            
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                
                bubble = lv_obj_get_child(obj, 0);
            } else {
                
                bubble = obj;
            }
        } else {
            
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            
        }
    }
#else
    
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    
    
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
