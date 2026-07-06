/**
 * @file BAJI.cc
 * @brief BAJI-185 开发板硬件驱动实现
 * 
 * 该文件实现了BAJI-185开发板的硬件初始化和管理功能，包括：
 * - LCD显示驱动（ST77916）
 * - IO扩展器管理（TCA9554）
 * - 按键处理（电源键、音量键）
 * - 网络切换（WiFi/4G ML307模块）
 * - 电源管理（AXP2101）
 * - 低功耗模式管理
 */

#include <cstdlib>
#include <string>
#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_timer.h>
#include <iot_button.h>
#include <button_types.h>
#include <font_awesome.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <wifi_manager.h>

#include "config.h"
#include "settings.h"
#include "application.h"
#include "mqtt_control.h"
#include "system_reset.h"
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "boards/common/ml307_board.h"
#include "assets/lang_config.h"
#include "i2c_device.h"
#include "esp_io_expander_tca9554.h"
#include "esp_io_expander.h"
#include "esp_io_expander_gpio_wrapper.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "baji_185_bringup.h"
#include "axp2101.h"

#define TAG "baji_lcd_1_85"

#define LCD_OPCODE_WRITE_CMD    (0x02ULL)
#define LCD_OPCODE_READ_CMD     (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR  (0x32ULL)

/**
 * @brief ST77916 LCD厂商特定初始化命令序列
 * 
 * 包含完整的LCD初始化参数配置，用于驱动1.85英寸LCD显示屏
 */
static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
    {0xfe, (uint8_t []){0x00}, 0, 0},
    {0xef, (uint8_t []){0x00}, 0, 0},
    {0x80, (uint8_t []){0x19}, 1, 0},
    {0x82, (uint8_t []){0x09}, 1, 0},
    {0x83, (uint8_t []){0x03}, 1, 0},
    {0x88, (uint8_t []){0x00}, 1, 0},
    {0x89, (uint8_t []){0x38}, 1, 0},
    {0x8A, (uint8_t []){0x40}, 1, 0},
    {0x8B, (uint8_t []){0x0A}, 1, 0},
    {0x8C, (uint8_t []){0x00}, 1, 0},
    {0x81, (uint8_t []){0xFF}, 1, 0},
    {0x84, (uint8_t []){0xFF}, 1, 0},
    {0x85, (uint8_t []){0xFF}, 1, 0},
    {0x86, (uint8_t []){0xFF}, 1, 0},
    {0x87, (uint8_t []){0xFF}, 1, 0},
    {0x8E, (uint8_t []){0xFF}, 1, 0},
    {0x8F, (uint8_t []){0xFF}, 1, 0},
    {0x98, (uint8_t []){0x3E}, 1, 0},
    {0x99, (uint8_t []){0x3E}, 1, 0},
    {0x7D, (uint8_t []){0x72}, 1, 0},
    {0x70, (uint8_t []){0x02, 0x03, 0x03, 0x06, 0x03, 0x03, 0x09, 0x07, 0x09, 0x03}, 10, 0},
    {0x90, (uint8_t []){0x06, 0x06, 0x01, 0x01}, 4, 0},
    {0x93, (uint8_t []){0x02, 0xFF, 0x00}, 3, 0},
    {0xCB, (uint8_t []){0x02}, 1, 0},
    {0xFB, (uint8_t []){0x00, 0x00}, 2, 0},
    {0xF6, (uint8_t []){0xC0}, 1, 0},
    {0x6C, (uint8_t []){0x00, 0x00, 0x22, 0x00, 0xCC, 0x04, 0x58}, 7, 0},
    {0xAA, (uint8_t []){0x0B, 0x00}, 2, 0},
    {0xEC, (uint8_t []){0x07}, 1, 0},
    {0xF9, (uint8_t []){0x40}, 1, 0},
    {0xEB, (uint8_t []){0x01, 0x67}, 2, 0},
    {0x74, (uint8_t []){0x01, 0x60, 0x00, 0x00, 0x00, 0x00}, 6, 0},
    {0xB5, (uint8_t []){0x14, 0x14, 0x14}, 3, 0},
    {0x6E, (uint8_t []){
        0x0B, 0x0B, 0x09, 0x09, 0x13, 0x13, 0x11, 0x11,
        0x16, 0x15, 0x01, 0x04, 0x00, 0x0D, 0x1D, 0x00,
        0x00, 0x1D, 0x0D, 0x00, 0x04, 0x08, 0x15, 0x16,
        0x12, 0x12, 0x14, 0x14, 0x0A, 0x0A, 0x0C, 0x0C
    }, 32, 0},
    {0x60, (uint8_t []){0x38, 0x1C, 0x13, 0x56}, 4, 0},
    {0x61, (uint8_t []){0xF8, 0x0A, 0x13, 0x56}, 4, 0},
    {0x62, (uint8_t []){0xF8, 0x0B, 0x13, 0x56}, 4, 0},
    {0x63, (uint8_t []){0x38, 0x1C, 0x13, 0x56}, 4, 0},
    {0x64, (uint8_t []){0x38, 0x20, 0x72, 0xF8, 0x13, 0x56}, 6, 0},
    {0x65, (uint8_t []){0x78, 0x1A, 0x70, 0x0B, 0x56, 0x13}, 6, 0},
    {0x66, (uint8_t []){0x38, 0x24, 0x72, 0xFC, 0x13, 0x56}, 6, 0},
    {0x68, (uint8_t []){0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A}, 7, 0},
    {0x69, (uint8_t []){0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A}, 7, 0},
    {0x6A, (uint8_t []){0x00, 0x00}, 2, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x36, (uint8_t []){0x00}, 1, 0},
    {0x7C, (uint8_t []){0xB6, 0x29}, 2, 0},
    {0xAC, (uint8_t []){0x40}, 1, 0},
    {0xC3, (uint8_t []){0x1A}, 1, 0},
    {0xC4, (uint8_t []){0x24}, 1, 0},
    {0xC9, (uint8_t []){0x2F}, 1, 0},
    {0xF0, (uint8_t []){0x11, 0x17, 0x08, 0x06, 0x05, 0x38}, 6, 0},
    {0xF1, (uint8_t []){0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F}, 6, 0},
    {0xF2, (uint8_t []){0x11, 0x17, 0x08, 0x06, 0x05, 0x38}, 6, 0},
    {0xF3, (uint8_t []){0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F}, 6, 0},
    {0xB4, (uint8_t []){0x0A}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xEE, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 20},
    {0x29, (uint8_t []){0x00}, 0, 20},
};

/** SPI总线初始化标志 */
static bool s_baji_spi_bus_inited = false;

/** I2C总线句柄 */
static i2c_master_bus_handle_t g_baji185_i2c_bus = nullptr;

/** IO扩展器句柄 */
static esp_io_expander_handle_t g_baji185_io_expander = nullptr;

/** IO扩展器GPIO包装器初始化标志 */
static bool g_baji185_io_wrapper_appended = false;

#if BAJI185_RUN_LED_AUTO_BLINK
/** 运行指示灯定时器句柄 */
static esp_timer_handle_t g_baji185_run_led_timer = nullptr;
#endif

/**
 * @brief 获取I2C总线句柄
 * 
 * @return i2c_master_bus_handle_t I2C总线句柄
 */
i2c_master_bus_handle_t baji_185_get_i2c_bus(void)
{
    if (g_baji185_i2c_bus == nullptr) {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &g_baji185_i2c_bus));
    }
    return g_baji185_i2c_bus;
}

/**
 * @brief 获取IO扩展器句柄
 * 
 * @return esp_io_expander_handle_t IO扩展器句柄
 */
esp_io_expander_handle_t baji_185_get_io_expander(void)
{
    return g_baji185_io_expander;
}

#if BAJI185_RUN_LED_AUTO_BLINK
/**
 * @brief 运行指示灯定时器回调函数
 * 
 * @param arg 回调参数（未使用）
 */
static void baji185_run_led_timer_cb(void* arg)
{
    (void)arg;
    static bool on;
    on = !on;
    gpio_set_level(BAJI185_RUN_LED_GPIO_VIRTUAL, on ? 1 : 0);
}
#endif

/**
 * @brief 确保IO扩展器已初始化
 * 
 * 初始化TCA9554 IO扩展器，配置输入输出引脚，设置初始电平
 */
static void Waveshare185EnsureIoExpander(void)
{
    if (g_baji185_io_expander != nullptr) {
        return;
    }

    baji_185_get_i2c_bus();
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(g_baji185_i2c_bus, TCA9554_I2C_ADDR, &g_baji185_io_expander));

    const uint32_t in_pins = BAJI185_IOX_PIN_MASK_VOL_DOWN | BAJI185_IOX_PIN_MASK_VOL_UP;
    const uint32_t out_pins = BAJI185_IOX_PIN_MASK_4G_PWRON | BAJI185_IOX_PIN_MASK_4G_RST |
                              BAJI185_IOX_PIN_MASK_PA | BAJI185_IOX_PIN_MASK_RUN_LED | BAJI185_IOX_PIN_MASK_LCD_RST;

    ESP_ERROR_CHECK(esp_io_expander_set_dir(g_baji185_io_expander, in_pins, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(g_baji185_io_expander, out_pins, IO_EXPANDER_OUTPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_4G_PWRON, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_4G_RST, 1));

#ifdef AUDIO_CODEC_PA_INVERTED
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_PA, AUDIO_CODEC_PA_INVERTED ? 1 : 0));
#else
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_PA, 0));
#endif

    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_RUN_LED, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_LCD_RST, 1));

    if (!g_baji185_io_wrapper_appended) {
        ESP_ERROR_CHECK(esp_io_expander_gpio_wrapper_append_handler(g_baji185_io_expander, TCA9554_GPIO_VIRTUAL_BASE));
        g_baji185_io_wrapper_appended = true;
    }

#if BAJI185_RUN_LED_AUTO_BLINK
    if (g_baji185_run_led_timer == nullptr) {
        const esp_timer_create_args_t targs = {
            .callback = baji185_run_led_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "baji185_run_led",
        };
        ESP_ERROR_CHECK(esp_timer_create(&targs, &g_baji185_run_led_timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(g_baji185_run_led_timer, 500000));
    }
#endif
}

/**
 * @brief 初始化SPI总线（仅执行一次）
 */
static void Waveshare185_InitSpiBusOnce(void)
{
    if (s_baji_spi_bus_inited) {
        return;
    }

    const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                            QSPI_PIN_NUM_LCD_DATA0,
                                                                            QSPI_PIN_NUM_LCD_DATA1,
                                                                            QSPI_PIN_NUM_LCD_DATA2,
                                                                            QSPI_PIN_NUM_LCD_DATA3,
                                                                            QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    s_baji_spi_bus_inited = true;
}

/**
 * @brief 重置LCD GPIO
 */
static void Waveshare185_ResetLcdGpio(void)
{
    Waveshare185EnsureIoExpander();
    gpio_set_direction(QSPI_PIN_NUM_LCD_RST_VIRTUAL, GPIO_MODE_OUTPUT);
    gpio_set_level(QSPI_PIN_NUM_LCD_RST_VIRTUAL, 1);
    gpio_set_level(QSPI_PIN_NUM_LCD_RST_VIRTUAL, 0);
    gpio_set_level(QSPI_PIN_NUM_LCD_RST_VIRTUAL, 1);
}

/**
 * @brief 创建LCD显示对象
 * 
 * @param quiet_boot 是否静默启动（不输出调试信息）
 * @return SpiLcdDisplay* LCD显示对象指针
 */
SpiLcdDisplay* baji_185_create_lcd_display(bool quiet_boot)
{
    Waveshare185EnsureIoExpander();
    Waveshare185_InitSpiBusOnce();
    Waveshare185_ResetLcdGpio();

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 60 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    if (!quiet_boot) {
        printf("-------------------------------------- Version selection -------------------------------------- \r\n");
    }

    esp_err_t ret;
    int lcd_cmd = 0x04;
    uint8_t register_data[3];
    size_t param_size = sizeof(register_data);
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= LCD_OPCODE_READ_CMD << 24;
    ret = esp_lcd_panel_io_rx_param(panel_io, lcd_cmd, register_data, param_size);

    if (!quiet_boot) {
        if (ret == ESP_OK) {
            printf("Register 0x04 data: %02x %02x %02x\n", register_data[0], register_data[1], register_data[2]);
        } else {
            printf("Failed to read register 0x04, error code: %d\n", ret);
        }
    }

    io_config.pclk_hz = 60 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io) != ESP_OK) {
        if (!quiet_boot) {
            printf("Failed to set LCD communication parameters -- SPI\r\n");
        }
        return nullptr;
    }

    if (!quiet_boot) {
        printf("LCD communication parameters are set successfully -- SPI\r\n");
    }

    if (register_data[0] == 0x00 && register_data[1] == 0x7F && register_data[2] == 0x7F) {
        if (!quiet_boot) {
            printf("Vendor-specific initialization for case 1.\n");
        }
    } else if (register_data[0] == 0xFF && register_data[1] == 0xFF && register_data[2] == 0xFF) {
        vendor_config.init_cmds = vendor_specific_init_new;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
        if (!quiet_boot) {
            printf("Vendor-specific initialization for case 2.\n");
        }
    }

    if (!quiet_boot) {
        printf("------------------------------------- End of version selection------------------------------------- \r\n");
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    return new SpiLcdDisplay(panel_io, panel,
                             DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                             DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, !quiet_boot);
}

/**
 * @brief 获取音量增加按键电平
 * 
 * @param drv 按键驱动（未使用）
 * @return uint8_t 按键电平（1=按下，0=释放）
 */
static uint8_t Ws185VolUpKeyLevel(button_driver_t* drv)
{
    (void)drv;
    static uint8_t last = 0;
    uint32_t lm = 0;

    if (g_baji185_io_expander == nullptr) {
        return last;
    }

    if (esp_io_expander_get_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_VOL_UP, &lm) != ESP_OK) {
        return last;
    }

    last = ((lm & BAJI185_IOX_PIN_MASK_VOL_UP) == 0) ? 1 : 0;
    return last;
}

/**
 * @brief 获取音量减小按键电平
 * 
 * @param drv 按键驱动（未使用）
 * @return uint8_t 按键电平（1=按下，0=释放）
 */
static uint8_t Ws185VolDownKeyLevel(button_driver_t* drv)
{
    (void)drv;
    static uint8_t last = 0;
    uint32_t lm = 0;

    if (g_baji185_io_expander == nullptr) {
        return last;
    }

    if (esp_io_expander_get_level(g_baji185_io_expander, BAJI185_IOX_PIN_MASK_VOL_DOWN, &lm) != ESP_OK) {
        return last;
    }

    last = ((lm & BAJI185_IOX_PIN_MASK_VOL_DOWN) == 0) ? 1 : 0;
    return last;
}

/**
 * @brief CustomBoard类实现
 * 
 * 继承自WifiBoard，实现BAJI-185开发板的完整硬件管理功能
 */
class CustomBoard : public WifiBoard {
private:
    static constexpr uint8_t kAutoPowerTargetBrightness = 30;
    static constexpr uint8_t kAutoPowerTargetVolume = 30;
    i2c_master_bus_handle_t i2c_bus_;           ///< I2C总线句柄
    LcdDisplay* display_;                        ///< 显示设备指针
    button_handle_t vol_up_handle_ = nullptr;    ///< 音量+按键句柄
    button_handle_t vol_down_handle_ = nullptr;  ///< 音量-按键句柄
    PowerSaveTimer* power_save_timer_;           ///< 省电定时器
    PowerManager* power_manager_;                ///< 电源管理器
    static CustomBoard* instance_;               ///< 单例实例
    bool pending_4g_switch_ = false;             ///< 待处理的4G切换请求
    bool fourg_power_on_ = false;                ///< 4G模块电源状态
    esp_timer_handle_t fourg_confirm_timer_ = nullptr; ///< 4G切换确认定时器
    bool auto_power_restore_pending_ = false;    ///< 是否记录了开启前的亮度/音量
    uint8_t auto_power_saved_brightness_ = 100;  ///< 开启前亮度
    uint8_t auto_power_saved_volume_ = 100;      ///< 开启前音量

    /** 网络模式枚举 */
    enum class NetMode {
        WIFI,   ///< WiFi模式
        ML307   ///< 4G ML307模块模式
    };

    enum class NetFlowState {
        Idle,
        SwitchingToWifi,
        SwitchingTo4G,
        ConnectingWifi,
        Connecting4G,
        WifiProvisioning,
    };

    NetMode net_mode_ = NetMode::WIFI;           ///< 当前网络模式
    std::unique_ptr<Ml307Board> ml307_board_;    ///< ML307模块管理对象
    NetworkEventCallback net_cb_;                ///< 网络事件回调
    bool prefer_4g_on_boot_ = true;             ///< 开机偏好4G网络
    TaskHandle_t fourg_boot_task_ = nullptr;     ///< 4G启动任务句柄
    TaskHandle_t net_switch_task_ = nullptr;     ///< 网络切换任务句柄
    TaskHandle_t wifi_reprovision_task_ = nullptr; ///< 重新配网任务句柄
    std::atomic<NetFlowState> net_flow_state_{NetFlowState::Idle}; ///< 网络流程保护状态

    /**
     * @brief 4G切换确认定时器回调
     * 
     * @param arg 回调参数（CustomBoard实例指针）
     */
    static void FourgConfirmTimerCb(void* arg) {
        auto self = static_cast<CustomBoard*>(arg);
        self->pending_4g_switch_ = false;
    }

    NetFlowState GetNetFlowState() const {
        return net_flow_state_.load();
    }

    void SetNetFlowState(NetFlowState state) {
        net_flow_state_.store(state);
    }

    bool IsNetFlowProtected() const {
        return GetNetFlowState() != NetFlowState::Idle;
    }

    static uint8_t ClampBrightnessPercent(int value) {
        if (value <= 0) {
            return 1;
        }
        if (value > 100) {
            return 100;
        }
        return static_cast<uint8_t>(value);
    }

    static uint8_t ClampVolumePercent(int value) {
        if (value < 0) {
            return 0;
        }
        if (value > 100) {
            return 100;
        }
        return static_cast<uint8_t>(value);
    }

    uint8_t GetCurrentBrightnessLevel() {
        auto* backlight = GetBacklight();
        if (backlight != nullptr && backlight->brightness() > 0) {
            return ClampBrightnessPercent(backlight->brightness());
        }

        Settings settings("display");
        return ClampBrightnessPercent(settings.GetInt("brightness", 100));
    }

    uint8_t GetCurrentVolumeLevel() {
        auto* codec = GetAudioCodec();
        if (codec != nullptr) {
            return ClampVolumePercent(codec->output_volume());
        }

        Settings settings("audio");
        return ClampVolumePercent(settings.GetInt("output_volume", 100));
    }

    uint8_t GetStoredBrightnessLevel() {
        Settings settings("display");
        return ClampBrightnessPercent(settings.GetInt("brightness", 100));
    }

    uint8_t GetStoredVolumeLevel() {
        Settings settings("audio");
        return ClampVolumePercent(settings.GetInt("output_volume", 100));
    }

    void ApplyAutoPowerMediaLevels() {
        auto* backlight = GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(kAutoPowerTargetBrightness, false);
        }

        auto* codec = GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(kAutoPowerTargetVolume, false);
        }
    }

    void RestoreAutoPowerMediaLevels() {
        uint8_t brightness = auto_power_restore_pending_
            ? auto_power_saved_brightness_
            : GetStoredBrightnessLevel();
        uint8_t volume = auto_power_restore_pending_
            ? auto_power_saved_volume_
            : GetStoredVolumeLevel();

        auto* backlight = GetBacklight();
        if (backlight != nullptr) {
            backlight->SetBrightness(brightness, false);
        }

        auto* codec = GetAudioCodec();
        if (codec != nullptr) {
            codec->SetOutputVolume(volume, false);
        }

        auto_power_restore_pending_ = false;
    }

    bool IsApplicationBusyForNetOp() const {
        auto state = Application::GetInstance().GetDeviceState();
        return state == kDeviceStateConnecting ||
               state == kDeviceStateWifiConfiguring ||
               state == kDeviceStateStarting ||
               state == kDeviceStateActivating ||
               state == kDeviceStateUpgrading;
    }

    void ShowApplicationBusyNotification() {
        const char* message = "设备连接过程中，请稍后再试";
        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateWifiConfiguring) {
            message = "配网过程中暂不允许其他网络操作";
        } else if (state == kDeviceStateUpgrading) {
            message = "升级过程中暂不允许切换网络";
        }

        if (display_ != nullptr) {
            display_->ShowNotification(message, 2000);
        }
    }

    BoardNetworkMode GetDisplayedNetworkMode() const {
        switch (GetNetFlowState()) {
            case NetFlowState::SwitchingToWifi:
            case NetFlowState::ConnectingWifi:
            case NetFlowState::WifiProvisioning:
                return BoardNetworkMode::WIFI;
            case NetFlowState::SwitchingTo4G:
            case NetFlowState::Connecting4G:
                return BoardNetworkMode::CELLULAR;
            case NetFlowState::Idle:
            default:
                return net_mode_ == NetMode::ML307 ? BoardNetworkMode::CELLULAR : BoardNetworkMode::WIFI;
        }
    }

    void ShowNetFlowBusyNotification(const char* action) {
        const char* message = nullptr;
        switch (GetNetFlowState()) {
            case NetFlowState::WifiProvisioning:
                message = "配网过程中暂不允许其他网络操作";
                break;
            case NetFlowState::SwitchingToWifi:
            case NetFlowState::SwitchingTo4G:
                message = "网络切换过程中，请稍后再试";
                break;
            case NetFlowState::ConnectingWifi:
                message = "WiFi连接过程中，请稍后再试";
                break;
            case NetFlowState::Connecting4G:
                message = "4G连接过程中，请稍后再试";
                break;
            case NetFlowState::Idle:
            default:
                message = action;
                break;
        }

        if (display_ != nullptr) {
            display_->ShowNotification(message, 2000);
        }
    }

    void HandleManagedNetworkEvent(NetworkEvent event, const std::string& data) {
        NetFlowState state = GetNetFlowState();
        switch (event) {
            case NetworkEvent::Connecting:
                if (net_mode_ == NetMode::ML307 ||
                    state == NetFlowState::SwitchingTo4G ||
                    state == NetFlowState::Connecting4G) {
                    SetNetFlowState(NetFlowState::Connecting4G);
                } else if (net_mode_ == NetMode::WIFI ||
                           state == NetFlowState::SwitchingToWifi ||
                           state == NetFlowState::ConnectingWifi) {
                    SetNetFlowState(NetFlowState::ConnectingWifi);
                }
                break;
            case NetworkEvent::Connected:
                SetNetFlowState(NetFlowState::Idle);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                SetNetFlowState(NetFlowState::WifiProvisioning);
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (state == NetFlowState::WifiProvisioning) {
                    SetNetFlowState(NetFlowState::ConnectingWifi);
                }
                break;
            case NetworkEvent::ModemDetecting:
                SetNetFlowState(NetFlowState::Connecting4G);
                break;
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                SetNetFlowState(NetFlowState::Idle);
                break;
            case NetworkEvent::Scanning:
            case NetworkEvent::Disconnected:
            default:
                break;
        }

        if (net_cb_) {
            net_cb_(event, data);
        }
    }

    /**
     * @brief 设置4G模块电源键电平
     * 
     * @param level 电平值（1=高，0=低）
     */
    void Set4GPwrKeyLevel(int level) {
        Waveshare185EnsureIoExpander();
        gpio_set_direction(ML307_MOD_PWRON_GPIO_VIRTUAL, GPIO_MODE_OUTPUT);
        gpio_set_level(ML307_MOD_PWRON_GPIO_VIRTUAL, level ? 1 : 0);
    }

    /**
     * @brief 脉冲复位4G模块
     */
    void Pulse4GRst() {
        Waveshare185EnsureIoExpander();
        gpio_set_direction(ML307_MOD_RST_GPIO_VIRTUAL, GPIO_MODE_OUTPUT);
        
        gpio_set_level(ML307_MOD_RST_GPIO_VIRTUAL, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(ML307_MOD_RST_GPIO_VIRTUAL, 1);
    }

    /**
     * @brief 脉冲控制调制解调器电源键
     */
    void ModemPowerKeyPulse() {
        Set4GPwrKeyLevel(1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        Set4GPwrKeyLevel(0);
    }

    /**
     * @brief 快速检查4G调制解调器是否存活
     * 
     * @return bool true=调制解调器存活，false=未检测到
     */
    bool Is4GModemAliveQuickCheck() {
        auto probe = AtModem::Detect(UART_4G_TXD, UART_4G_RXD, UART0_DTR, 115200, 1200);
        return probe != nullptr;
    }

    /**
     * @brief 打开4G模块
     */
    void TurnOn4GModule() {
        if (fourg_power_on_) {
            return;
        }

        // On MCU reboot the modem may still be powered. Avoid sending another
        // power-key pulse here, otherwise we can accidentally turn it off.
        if (Is4GModemAliveQuickCheck()) {
            fourg_power_on_ = true;
            return;
        }

        gpio_set_direction(ML307_MOD_RST_GPIO_VIRTUAL, GPIO_MODE_OUTPUT);
        gpio_set_level(ML307_MOD_RST_GPIO_VIRTUAL, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        ModemPowerKeyPulse();

        vTaskDelay(pdMS_TO_TICKS(1500));
        fourg_power_on_ = true;
    }

    /**
     * @brief 关闭4G模块
     * 
     * @param force 是否强制关闭（忽略当前电源状态）
     */
    void TurnOff4GModule(bool force = false) {
        if (!fourg_power_on_ && !force) {
            Set4GPwrKeyLevel(0);
            return;
        }

        ModemPowerKeyPulse();
        vTaskDelay(pdMS_TO_TICKS(500));
        fourg_power_on_ = false;
    }

    /**
     * @brief 保存首选网络设置并重启
     * 
     * @param target 目标网络模式
     */
    void SavePreferredNetworkAndReboot(NetMode target) {
        if (target == NetMode::WIFI) {
            TurnOff4GModule();
        }

        Settings settings("network", true);
        settings.SetInt("type", target == NetMode::ML307 ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        Application::GetInstance().Reboot();
    }

    /**
     * @brief 保存首选网络设置
     * 
     * @param target 目标网络模式
     */
    void SavePreferredNetwork(NetMode target) {
        Settings settings("network", true);
        settings.SetInt("type", target == NetMode::ML307 ? 1 : 0);
        prefer_4g_on_boot_ = (target == NetMode::ML307);
    }

    /**
     * @brief 立即停止WiFi
     */
    void StopWifiNow() {
        MqttControl::GetInstance().StopForNetworkSwitch();
        esp_timer_stop(connect_timer_);
        StopWifiConfigCountdown();
        in_config_mode_ = false;
        auto& wifi_manager = WifiManager::GetInstance();
        if (wifi_manager.IsConfigMode()) {
            wifi_manager.StopConfigAp();
        }
        wifi_manager.StopStation();
    }

    /**
     * @brief 立即停止4G网络
     */
    void Stop4gNow() {
        MqttControl::GetInstance().StopForNetworkSwitch();
        if (ml307_board_) {
            ml307_board_->StopNetwork();
            (void)ml307_board_->WaitUntilStopped(4000);
            ml307_board_.reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        TurnOff4GModule();
    }

    /**
     * @brief 在切换网络前确保BAJI已停止
     */
    void EnsureBAJIStoppedBeforeSwitch() {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();

        if (state == kDeviceStateUpgrading) {
            return;
        }

        if (state == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }

        app.ResetProtocolSync(1500);
        app.SetDeviceState(kDeviceStateIdle);
    }

    /**
     * @brief 等待设备空闲后再切换网络
     * 
     * @param timeout_ms 超时时间（毫秒）
     * @return bool true=成功等待到空闲，false=超时
     */
    bool WaitForIdleBeforeSwitch(int timeout_ms) {
        auto& app = Application::GetInstance();
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

        while (xTaskGetTickCount() < deadline) {
            auto st = app.GetDeviceState();
            if (st != kDeviceStateSpeaking && st != kDeviceStateListening && st != kDeviceStateConnecting) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        return false;
    }

    /**
     * @brief 异步启动4G网络
     * 
     * @param for_dialog_wake 是否为对话唤醒启动
     */
    void Start4gAsync(bool for_dialog_wake) {
        if (fourg_boot_task_ != nullptr) {
            return;
        }

        struct Start4gCtx {
            CustomBoard* self;
            bool for_dialog_wake;
        };

        auto* ctx = new Start4gCtx{this, for_dialog_wake};

        if (xTaskCreate([](void* arg) {
            auto* ctx = static_cast<Start4gCtx*>(arg);
            auto self = ctx->self;
            const bool for_dialog_wake = ctx->for_dialog_wake;
            delete ctx;

            self->TurnOn4GModule();

            if (self->fourg_power_on_) {
                // Wait for the modem UART to become stable before the first AT probe.
                vTaskDelay(pdMS_TO_TICKS(for_dialog_wake ? 2200 : 2600));
            }

            if (!self->ml307_board_) {
                self->ml307_board_ = std::make_unique<Ml307Board>(UART_4G_TXD, UART_4G_RXD, UART0_DTR);
                self->ml307_board_->SetNetworkEventCallback([self](NetworkEvent event, const std::string& data) {
                    self->HandleManagedNetworkEvent(event, data);
                });
            }

            self->SetNetFlowState(NetFlowState::Connecting4G);
            self->ml307_board_->StartNetwork();
            self->fourg_boot_task_ = nullptr;

            vTaskDelete(nullptr);
        }, "baji185_4g_start", 8192, ctx, 5, &fourg_boot_task_) != pdPASS) {
            delete ctx;
            SetNetFlowState(NetFlowState::Idle);
        }
    }

    /**
     * @brief 运行时切换网络模式
     * 
     * @param target 目标网络模式
     * @param for_dialog_wake 是否为对话唤醒切换
     */
    void SwitchNetworkRuntime(NetMode target, bool for_dialog_wake = false) {
        if (net_switch_task_ != nullptr) {
            return;
        }

        struct SwitchCtx {
            CustomBoard* self;
            NetMode target;
            bool for_dialog_wake;
        };

        auto* ctx = new SwitchCtx{this, target, for_dialog_wake};

        if (xTaskCreate([](void* arg) {
            auto* ctx = static_cast<SwitchCtx*>(arg);
            auto self = ctx->self;
            NetMode target = ctx->target;
            const bool for_dialog_wake = ctx->for_dialog_wake;
            delete ctx;

            self->EnsureBAJIStoppedBeforeSwitch();
            (void)self->WaitForIdleBeforeSwitch(1500);

            if (target == NetMode::ML307) {
                self->SetWifiAutoReconnectEnabled(false);
                self->StopWifiNow();
                self->net_mode_ = NetMode::ML307;
                self->SavePreferredNetwork(NetMode::ML307);
                self->Start4gAsync(for_dialog_wake);
            } else {
                self->Stop4gNow();
                self->net_mode_ = NetMode::WIFI;
                self->SavePreferredNetwork(NetMode::WIFI);
                self->SetWifiAutoReconnectEnabled(true);
                self->WifiBoard::StartNetwork();
            }

            self->net_switch_task_ = nullptr;
            vTaskDelete(nullptr);
        }, "baji185_net_switch", 6144, ctx, 5, &net_switch_task_) != pdPASS) {
            delete ctx;
            SetNetFlowState(NetFlowState::Idle);
        }
    }

    /**
     * @brief 请求切换网络模式
     *
     * @param target 目标网络模式
     * @param for_dialog_wake 是否为对话唤醒切换
     * @param show_notification 是否显示切换提示
     * @return bool true=已受理切换请求，false=当前不可切换
     */
    bool RequestNetworkSwitch(NetMode target, bool for_dialog_wake = false, bool show_notification = true) {
        if (target == net_mode_) {
            return false;
        }

        if (net_switch_task_ != nullptr || wifi_reprovision_task_ != nullptr) {
            if (show_notification) {
                ShowNetFlowBusyNotification("网络忙，请稍后再试");
            }
            return false;
        }

        auto& app = Application::GetInstance();
        if (app.IsOtaUpgradeInProgress()) {
            app.NotifyOtaNetworkSwitchRequested(target == NetMode::ML307
                ? BoardNetworkMode::CELLULAR
                : BoardNetworkMode::WIFI);
        }

        if (show_notification) {
            GetDisplay()->ShowNotification(target == NetMode::ML307
                ? Lang::Strings::SWITCH_TO_4G_NETWORK
                : Lang::Strings::SWITCH_TO_WIFI_NETWORK, 2000);
        }

        SetNetFlowState(target == NetMode::ML307
            ? NetFlowState::SwitchingTo4G
            : NetFlowState::SwitchingToWifi);
        SwitchNetworkRuntime(target, for_dialog_wake);
        return true;
    }

    /**
     * @brief 根据音量等级获取音量图标
     * 
     * @param vol 音量值（0-100）
     * @return const char* 图标字符串
     */
    static const char* VolumeIconForLevel(int vol) {
        if (vol <= 0) {
            return FONT_AWESOME_VOLUME_XMARK;
        }
        if (vol < 35) {
            return FONT_AWESOME_VOLUME_LOW;
        }
        if (vol < 70) {
            return FONT_AWESOME_VOLUME;
        }
        return FONT_AWESOME_VOLUME_HIGH;
    }

    /**
     * @brief 在显示屏上显示音量信息
     * 
     * @param volume 当前音量值
     */
    void ShowVolumeOnDisplay(int volume) {
        std::string msg = std::string(VolumeIconForLevel(volume)) + "  " + Lang::Strings::VOLUME + std::to_string(volume);
        GetDisplay()->ShowNotification(msg);
    }

    void TriggerWifiReprovision() {
        if (IsManualWifiConfigMode() && ExitManualWifiConfigMode()) {
            GetDisplay()->ShowNotification("退出配网模式", 2000);
            return;
        }

        if (IsApplicationBusyForNetOp()) {
            ShowApplicationBusyNotification();
            return;
        }

        if (net_switch_task_ != nullptr || wifi_reprovision_task_ != nullptr || IsNetFlowProtected()) {
            ShowNetFlowBusyNotification("当前暂不允许进入配网");
            return;
        }

        SetNetFlowState(NetFlowState::WifiProvisioning);

        struct ReprovisionCtx {
            CustomBoard* self;
        };

        auto* ctx = new ReprovisionCtx{this};
        if (xTaskCreate([](void* arg) {
            auto* ctx = static_cast<ReprovisionCtx*>(arg);
            auto self = ctx->self;
            delete ctx;

            self->EnsureBAJIStoppedBeforeSwitch();
            (void)self->WaitForIdleBeforeSwitch(1500);

            if (self->net_mode_ == NetMode::ML307) {
                self->Stop4gNow();
            } else {
                self->StopWifiNow();
            }

            self->net_mode_ = NetMode::WIFI;
            self->SavePreferredNetwork(NetMode::WIFI);
            self->SetWifiAutoReconnectEnabled(true);
            self->EnterWifiConfigMode();
            self->wifi_reprovision_task_ = nullptr;

            vTaskDelete(nullptr);
        }, "baji185_wifi_reprov", 6144, ctx, 5, &wifi_reprovision_task_) != pdPASS) {
            delete ctx;
            SetNetFlowState(NetFlowState::Idle);
        }
    }

    /**
     * @brief 初始化电源管理器
     */
    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_USB_IN);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
        power_manager_->OnPowerUi([this](PowerUiHint hint) {
            if (hint == PowerUiHint::ShuttingDown) {
                GetDisplay()->ShowNotification("正在关机...", 2000);
                if (auto* backlight = GetBacklight(); backlight != nullptr) {
                    backlight->SetBrightness(0);
                }
            }
        });
        power_manager_->OnPowerSingleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (power_save_timer_) {
                    power_save_timer_->WakeUp();
                }
                if (display_ != nullptr && display_->IsSmartWatchUiActive()) {
                    display_->SmartWatchUiBack();
                }
            });
        });
        power_manager_->OnPowerDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (power_save_timer_) {
                    power_save_timer_->WakeUp();
                }
                if (display_ != nullptr && display_->IsSmartWatchUiActive()) {
                    display_->SmartWatchUiShowHome();
                }
            });
        });
        power_manager_->OnPowerTripleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (power_save_timer_) {
                    power_save_timer_->WakeUp();
                }
                TriggerWifiReprovision();
            });
        });
    }

    /**
     * @brief 初始化省电定时器
     */
    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            display_->SetChatMessage("system", "");
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            display_->SetChatMessage("system", "");
            if (GetAutoPowerSaveEnabled()) {
                GetBacklight()->SetBrightness(kAutoPowerTargetBrightness, false);
            } else {
                GetBacklight()->RestoreBrightness();
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    /**
     * @brief 初始化I2C总线
     */
    void InitializeI2c() {
        i2c_bus_ = baji_185_get_i2c_bus();
    }

    /**
     * @brief 初始化所有按键
     */
    void InitializeButtons() {
        instance_ = this;
        InitializeVolumeButtons();
    }

    /**
     * @brief 初始化音量按键
     */
    void InitializeVolumeButtons() {
        Waveshare185EnsureIoExpander();

        button_config_t vol_up_cfg = {
            .long_press_time = 3000,
            .short_press_time = 0,
        };

        button_config_t vol_down_cfg = {
            .long_press_time = 2000,
            .short_press_time = 0,
        };

        if (vol_up_handle_ == nullptr) {
            button_driver_t* du = (button_driver_t*)calloc(1, sizeof(button_driver_t));
            du->enable_power_save = false;
            du->get_key_level = Ws185VolUpKeyLevel;
            ESP_ERROR_CHECK(iot_button_create(&vol_up_cfg, du, &vol_up_handle_));
        }

        if (vol_down_handle_ == nullptr) {
            button_driver_t* dd = (button_driver_t*)calloc(1, sizeof(button_driver_t));
            dd->enable_power_save = false;
            dd->get_key_level = Ws185VolDownKeyLevel;
            ESP_ERROR_CHECK(iot_button_create(&vol_down_cfg, dd, &vol_down_handle_));
        }

        iot_button_register_cb(vol_up_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);

            if (self->pending_4g_switch_) {
                self->pending_4g_switch_ = false;
                if (self->fourg_confirm_timer_) {
                    esp_timer_stop(self->fourg_confirm_timer_);
                }

                bool to_4g = (self->net_mode_ == NetMode::WIFI);
                self->RequestNetworkSwitch(to_4g ? NetMode::ML307 : NetMode::WIFI, false, true);
                return;
            }

            auto* codec = self->GetAudioCodec();
            int volume = codec->output_volume() + 10;

            if (volume > 100) {
                volume = 100;
            }

            codec->SetOutputVolume(volume);
            self->ShowVolumeOnDisplay(volume);
        }, this);

        iot_button_register_cb(vol_up_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            self->pending_4g_switch_ = true;

            if (self->fourg_confirm_timer_ == nullptr) {
                const esp_timer_create_args_t targs = {
                    .callback = FourgConfirmTimerCb,
                    .arg = self,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "baji185_4g_confirm",
                    .skip_unhandled_events = true,
                };
                ESP_ERROR_CHECK(esp_timer_create(&targs, &self->fourg_confirm_timer_));
            }

            esp_timer_stop(self->fourg_confirm_timer_);
            ESP_ERROR_CHECK(esp_timer_start_once(self->fourg_confirm_timer_, 5 * 1000000ULL));

            self->GetDisplay()->ShowNotification(
                self->net_mode_ == NetMode::WIFI ? "切换4G？短按音量+确认" : "切换WiFi？短按音量+确认", 2000);
        }, this);

        iot_button_register_cb(vol_down_handle_, BUTTON_SINGLE_CLICK, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            auto* codec = self->GetAudioCodec();
            int volume = codec->output_volume() - 10;

            if (volume < 0) {
                volume = 0;
            }

            codec->SetOutputVolume(volume);
            self->ShowVolumeOnDisplay(volume);
        }, this);

        iot_button_register_cb(vol_down_handle_, BUTTON_LONG_PRESS_START, nullptr, [](void* button_handle, void* usr_data) {
            auto self = static_cast<CustomBoard*>(usr_data);
            self->GetAudioCodec()->SetOutputVolume(0);
            std::string msg = std::string(FONT_AWESOME_VOLUME_XMARK) + "  " + Lang::Strings::MUTED;
            self->GetDisplay()->ShowNotification(msg);
        }, this);
    }

public:
    /**
     * @brief CustomBoard构造函数
     */
    CustomBoard() {
        Settings settings("network", true);
        prefer_4g_on_boot_ = (settings.GetInt("type", 1) == 1);

        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeI2c();

        Set4GPwrKeyLevel(0);
        gpio_set_direction(ML307_MOD_RST_GPIO_VIRTUAL, GPIO_MODE_OUTPUT);
        gpio_set_level(ML307_MOD_RST_GPIO_VIRTUAL, 1);
        GetBacklight()->SetBrightness(0);

        display_ = baji_185_create_lcd_display();

        if (display_ == nullptr) {
            abort();
        }

        InitializeButtons();
    }

    /**
     * @brief 获取音频编解码器
     * 
     * @return AudioCodec* 音频编解码器指针
     */
    virtual AudioCodec* GetAudioCodec() override {
#if AUDIO_INPUT_USE_SILICON_MIC
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN_ANALOG, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, false);
#else
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, false);
#endif
        return &audio_codec;
    }

    /**
     * @brief 获取显示设备
     * 
     * @return Display* 显示设备指针
     */
    virtual Display* GetDisplay() override {
        return display_;
    }

    /**
     * @brief 获取背光控制
     * 
     * @return Backlight* 背光控制指针
     */
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    /**
     * @brief 获取电池状态
     * 
     * @param level 电池电量（输出）
     * @param charging 是否充电中（输出）
     * @param discharging 是否放电中（输出）
     * @return bool true=获取成功
     */
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();

        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = power_manager_->GetBatteryLevel();
        return true;
    }

    bool GetAutoPowerSaveEnabled() override {
        Settings settings("wifi");
        return settings.GetBool("sleep_mode", false);
    }

    bool SetAutoPowerSaveEnabled(bool enabled) override {
        bool was_enabled = GetAutoPowerSaveEnabled();
        if (enabled && !was_enabled) {
            auto_power_saved_brightness_ = GetCurrentBrightnessLevel();
            auto_power_saved_volume_ = GetCurrentVolumeLevel();
            auto_power_restore_pending_ = true;
        }

        Settings settings("wifi", true);
        settings.SetBool("sleep_mode", enabled);

        if (power_save_timer_ != nullptr && power_manager_ != nullptr) {
            if (!enabled) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(power_manager_->IsDischarging());
            }
        }

        if (enabled) {
            ApplyAutoPowerMediaLevels();
        } else {
            RestoreAutoPowerMediaLevels();
        }

        return true;
    }

    /**
     * @brief 设置省电级别
     * 
     * @param level 省电级别
     */
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    /**
     * @brief 启动网络
     */
    void StartNetwork() override {
        if (!prefer_4g_on_boot_) {
            if (Is4GModemAliveQuickCheck()) {
                TurnOff4GModule(true);
            }

            net_mode_ = NetMode::WIFI;
            SetNetFlowState(NetFlowState::ConnectingWifi);
            SetWifiAutoReconnectEnabled(true);
            WifiBoard::StartNetwork();
            return;
        }

        net_mode_ = NetMode::ML307;
        SetNetFlowState(NetFlowState::Connecting4G);
        GetDisplay()->SetStatus("连接4G");
        Start4gAsync(false);
    }

    /**
     * @brief 设置网络事件回调
     * 
     * @param callback 回调函数
     */
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        net_cb_ = std::move(callback);
        WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            HandleManagedNetworkEvent(event, data);
        });

        if (ml307_board_) {
            ml307_board_->SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
                HandleManagedNetworkEvent(event, data);
            });
        }
    }

    /**
     * @brief 获取网络接口
     * 
     * @return NetworkInterface* 网络接口指针
     */
    NetworkInterface* GetNetwork() override {
        return net_mode_ == NetMode::ML307 && ml307_board_ ? ml307_board_->GetNetwork() : WifiBoard::GetNetwork();
    }

    /**
     * @brief 获取网络状态图标
     * 
     * @return const char* 图标字符串
     */
    const char* GetNetworkStateIcon() override {
        if (net_mode_ == NetMode::ML307) {
            return ml307_board_ ? ml307_board_->GetNetworkStateIcon() : FONT_AWESOME_SIGNAL_OFF;
        }
        return WifiBoard::GetNetworkStateIcon();
    }

    BoardNetworkMode GetActiveNetworkMode() override {
        return GetDisplayedNetworkMode();
    }

    bool SwitchActiveNetworkMode(BoardNetworkMode mode) override {
        if (mode == BoardNetworkMode::UNSUPPORTED) {
            return false;
        }

        NetMode target = (mode == BoardNetworkMode::CELLULAR) ? NetMode::ML307 : NetMode::WIFI;
        if (target == net_mode_) {
            return true;
        }

        return RequestNetworkSwitch(target, false, true);
    }
};

DECLARE_BOARD(CustomBoard);
CustomBoard* CustomBoard::instance_ = nullptr;
