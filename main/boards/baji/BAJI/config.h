#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

/*
 * 音频配置
 */
#define AUDIO_INPUT_SAMPLE_RATE     24000      // 音频输入采样率
#define AUDIO_OUTPUT_SAMPLE_RATE    24000      // 音频输出采样率
#define AUDIO_MIC_PCM_RIGHT_SHIFT   12         // 麦克风PCM右移位数

#define AUDIO_INPUT_REFERENCE       false      // 音频输入参考
#define AUDIO_INPUT_USE_SILICON_MIC 0          // 是否使用硅麦

/*
 * 音频I2S引脚配置
 */
#define AUDIO_I2S_GPIO_MCLK         GPIO_NUM_45    // I2S主时钟引脚
#define AUDIO_I2S_GPIO_WS           GPIO_NUM_46    // I2S字选引脚
#define AUDIO_I2S_GPIO_BCLK         GPIO_NUM_41    // I2S位时钟引脚
#define AUDIO_I2S_GPIO_DIN          GPIO_NUM_40    // I2S数据输入引脚
#define AUDIO_I2S_GPIO_DOUT         GPIO_NUM_42    // I2S数据输出引脚
#define AUDIO_I2S_GPIO_DIN_ANALOG   GPIO_NUM_39    // I2S模拟数据输入引脚

/*
 * 按键配置
 */
#define BOOT_BUTTON_GPIO            GPIO_NUM_0     // 启动按钮GPIO
#define VOLUME_UP_BUTTON_GPIO       GPIO_NUM_NC    // 音量加按钮GPIO
#define VOLUME_DOWN_BUTTON_GPIO     GPIO_NUM_NC    // 音量减按钮GPIO

/*
 * 电源配置
 */
#define POWER_USB_IN                GPIO_NUM_5     // USB输入检测引脚
#define Power_Control               GPIO_NUM_1     // 电源控制引脚
#define Power_Dec                   GPIO_NUM_2     // 电源检测引脚

#define POWER_KEY_SCAN_INTERVAL_MS              20   // 电源键扫描周期(ms)
#define POWER_KEY_DEBOUNCE_MS                   40   // 电源键按下/释放去抖(ms)
#define POWER_KEY_DOUBLE_CLICK_WINDOW_MS        250  // 电源键双击窗口(ms)
#define POWER_KEY_MULTI_CLICK_GUARD_MS           300  // 三击后需保持稳定释放的保护时间(ms)
#define POWER_KEY_SHUTDOWN_HOLD_MS              2600 // 电源键长按关机时间(ms)
#define POWER_KEY_FORCE_CUT_HOLD_MS             5000 // 独立任务强制释放电源锁存时间(ms)
#define POWER_CHARGING_FULLSCREEN_BACKLIGHT     5    // 充电全屏背光
#define POWER_KEY_HOLD_MS_TO_BOOT               3000 // 开机键按住时间(ms)
#define POWER_KEY_STABLE_RELEASE_MS             80   // 开机键稳定释放时间(ms)
#define POWER_KEY_REARM_WAIT_MS                 10000 // 深睡唤醒后等待新一轮长按的窗口(ms)
#define POWER_RECOVERY_MAX_CONSECUTIVE_RESETS      3 // 连续异常复位超过此次数后关机
#define POWER_RECOVERY_STABLE_UPTIME_MS         30000 // 运行稳定后清除连续复位计数(ms)

#define POWER_CBS_ADC_UNIT              ADC_UNIT_1     // 电源ADC单元
#define POWER_BATTERY_ADC_CHANNEL       ADC_CHANNEL_3  // 电池ADC通道
#define POWER_USBIN_ADC_CHANNEL         ADC_CHANNEL_1  // USB输入ADC通道

#define POWER_CHARGE_DETECT_USE_GPIO    1              // 是否使用GPIO检测充电
#define POWER_USB_VBUS_ACTIVE_LEVEL     1              // USB VBUS有效电平
#define POWER_KEY_LEVEL_WHEN_PRESSED    0              // 电源键按下时的电平

// 电源键状态检测宏
#define POWER_KEY_PRESSED()     (gpio_get_level(Power_Dec) == (POWER_KEY_LEVEL_WHEN_PRESSED))
#define POWER_KEY_RELEASED()    (gpio_get_level(Power_Dec) != (POWER_KEY_LEVEL_WHEN_PRESSED))

/*
 * TCA9554 I2C扩展器配置
 */
#define TCA9554_I2C_ADDR                0x20                            // TCA9554 I2C地址
#define TCA9554_GPIO_VIRTUAL_BASE       50                              // 虚拟GPIO基地址

/*
 * CST816S 触控配置
 */
#define CST816S_I2C_ADDR                0x15          // 触控芯片I2C地址
#define CST816S_I2C_SDA_PIN             GPIO_NUM_6     // 触控I2C SDA引脚
#define CST816S_I2C_SCL_PIN             GPIO_NUM_7     // 触控I2C SCL引脚
#define CST816S_TOUCH_INT_PIN           GPIO_NUM_17    // 触控中断引脚
#define CST816S_TOUCH_RST_PIN           QSPI_PIN_NUM_LCD_RST_VIRTUAL    // 触控复位引脚（与LCD共用）

// IO扩展器引脚掩码
#define BAJI185_IOX_PIN_MASK_4G_PWRON   (1u << 0)   // 4G模块电源使能
#define BAJI185_IOX_PIN_MASK_4G_RST     (1u << 1)   // 4G模块复位
#define BAJI185_IOX_PIN_MASK_VOL_UP     (1u << 2)   // 音量加
#define BAJI185_IOX_PIN_MASK_VOL_DOWN   (1u << 3)   // 音量减
#define BAJI185_IOX_PIN_MASK_PA         (1u << 5)   // 音频功率放大器
#define BAJI185_IOX_PIN_MASK_RUN_LED    (1u << 6)   // 运行指示灯
#define BAJI185_IOX_PIN_MASK_LCD_RST    (1u << 7)   // LCD复位

#define BAJI185_RUN_LED_AUTO_BLINK      0           // 运行指示灯是否自动闪烁

// 虚拟GPIO定义
#define ML307_MOD_PWRON_GPIO_VIRTUAL    ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 0))
#define ML307_MOD_RST_GPIO_VIRTUAL      ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 1))
#define AUDIO_CODEC_PA_GPIO_VIRTUAL     ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 5))
#define BAJI185_RUN_LED_GPIO_VIRTUAL    ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 6))
#define QSPI_PIN_NUM_LCD_RST_VIRTUAL    ((gpio_num_t)(TCA9554_GPIO_VIRTUAL_BASE + 7))
#define AUDIO_CODEC_PA_PIN              AUDIO_CODEC_PA_GPIO_VIRTUAL

/*
 * 音频编解码器配置
 */
#define AUDIO_CODEC_PA_INVERTED         0                               // 音频功率放大器是否反相
#define AUDIO_CODEC_I2C_SDA_PIN         GPIO_NUM_6                      // 音频编解码器I2C SDA引脚
#define AUDIO_CODEC_I2C_SCL_PIN         GPIO_NUM_7                      // 音频编解码器I2C SCL引脚
#define AUDIO_CODEC_ES8311_ADDR         ES8311_CODEC_DEFAULT_ADDR       // ES8311编解码器I2C地址

/*
 * UART配置
 */
#define UART0_DTR                       GPIO_NUM_38     // UART0 DTR引脚
#define UART_4G_RXD                     GPIO_NUM_48     // 4G模块UART接收引脚
#define UART_4G_TXD                     GPIO_NUM_47     // 4G模块UART发送引脚

/*
 * ML307 4G模块配置
 */
#define ML307_ENABLE_EDRX               1               // 是否启用EDRX
#define ML307_EDRX_ACT                  7               // EDRX激活时间
#define ML307_EDRX_VALUE                "0011"          // EDRX值

/*
 * OTA identity fields
 * role: MJQ / DCX / SYX / LYW / ZZY / YHX / HJL
 * network_version: WIFI / 4G
 */
#define BAJI_OTA_ROLE                   ""
#define BAJI_OTA_NETWORK_VERSION        "4G"

/*
 * 显示配置
 */
#define DISPLAY_WIDTH                   360     // 显示宽度
#define DISPLAY_HEIGHT                  360     // 显示高度
#define DISPLAY_MIRROR_X                true    // X轴镜像
#define DISPLAY_MIRROR_Y                true    // Y轴镜像
#define DISPLAY_SWAP_XY                 false   // 交换XY轴

#define DISPLAY_OFFSET_X                0       // 显示X偏移
#define DISPLAY_OFFSET_Y                0       // 显示Y偏移

/*
 * QSPI LCD配置
 */
#define QSPI_LCD_H_RES                  (360)           // LCD水平分辨率
#define QSPI_LCD_V_RES                  (360)           // LCD垂直分辨率
#define QSPI_LCD_BIT_PER_PIXEL          (16)            // LCD每像素位数

#define QSPI_LCD_HOST                   SPI2_HOST       // LCD SPI主机
#define QSPI_PIN_NUM_LCD_PCLK           GPIO_NUM_12     // LCD时钟引脚
#define QSPI_PIN_NUM_LCD_CS             GPIO_NUM_10     // LCD片选引脚
#define QSPI_PIN_NUM_LCD_DATA0          GPIO_NUM_11     // LCD数据0引脚
#define QSPI_PIN_NUM_LCD_DATA1          GPIO_NUM_13     // LCD数据1引脚
#define QSPI_PIN_NUM_LCD_DATA2          GPIO_NUM_14     // LCD数据2引脚
#define QSPI_PIN_NUM_LCD_DATA3          GPIO_NUM_9      // LCD数据3引脚
#define QSPI_PIN_NUM_LCD_TE             GPIO_NUM_8      // LCD TE同步引脚
#define QSPI_PIN_NUM_LCD_RST            GPIO_NUM_NC     // LCD复位引脚
#define QSPI_PIN_NUM_LCD_BL             GPIO_NUM_15     // LCD背光引脚

#define DISPLAY_BACKLIGHT_PIN           QSPI_PIN_NUM_LCD_BL          // 显示背光引脚
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false                         // 背光输出是否反相

/*
 * ST77916面板QSPI总线配置
 */
#define TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

#endif 
