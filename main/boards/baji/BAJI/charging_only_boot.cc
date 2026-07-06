
#include "config.h"
#include "charging_boot_rtc.h"
#include "abnormal_reporter.h"
#include "baji_185_bringup.h"

#include "display/lcd_display.h"
#include "backlight.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static void InitPowerKeyPin(void)
{
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << Power_Dec);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

static void InitV5mDetectPin(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << POWER_USB_IN);
#if POWER_CHARGE_DETECT_USE_GPIO
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#else
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#endif
    gpio_config(&io_conf);
}

static bool IsV5mPresent(void)
{
    return gpio_get_level(POWER_USB_IN) == POWER_USB_VBUS_ACTIVE_LEVEL;
}

static void LatchPowerControlOn(void)
{
    gpio_config_t po = {};
    po.intr_type = GPIO_INTR_DISABLE;
    po.mode = GPIO_MODE_OUTPUT;
    po.pin_bit_mask = (1ULL << Power_Control);
    po.pull_down_en = GPIO_PULLDOWN_ENABLE;
    po.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&po);
    gpio_set_level(Power_Control, 1);
}


static void ChargingOnlyPowerOffDeepSleep(void)
{
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);

    gpio_config_t wake_in = {};
    wake_in.intr_type = GPIO_INTR_DISABLE;
    wake_in.mode = GPIO_MODE_INPUT;
    wake_in.pin_bit_mask = (1ULL << Power_Dec);
    wake_in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    wake_in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&wake_in);

    gpio_config_t po = {};
    po.intr_type = GPIO_INTR_DISABLE;
    po.mode = GPIO_MODE_OUTPUT;
    po.pin_bit_mask = (1ULL << Power_Control);
    po.pull_down_en = GPIO_PULLDOWN_ENABLE;
    po.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&po);
    gpio_set_level(Power_Control, 0);

    vTaskDelay(pdMS_TO_TICKS(200));
    while (POWER_KEY_PRESSED()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = esp_sleep_enable_ext1_wakeup((1ULL << Power_Dec), ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        esp_sleep_enable_ext0_wakeup(Power_Dec, 0);
    }
    
    esp_deep_sleep_start();
}

extern "C" bool board_should_charging_only_boot(void)
{
    InitPowerKeyPin();
    InitV5mDetectPin();

    bool v5m = IsV5mPresent();
    if (charging_rtc_usb_shutdown_next_boot() && !v5m) {
        charging_rtc_clear_boot_flags();
        ChargingOnlyPowerOffDeepSleep();
    }

    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_SW) {
        charging_rtc_clear_boot_flags();
        return false;
    }

    if (!v5m) {
        charging_rtc_clear_boot_flags();
        return false;
    }

    if (charging_rtc_usb_shutdown_next_boot() || rr == ESP_RST_POWERON || rr == ESP_RST_DEEPSLEEP ||
        rr == ESP_RST_BROWNOUT) {
        return true;
    }

    charging_rtc_clear_boot_flags();
    return false;
}

extern "C" void board_charging_only_main(void)
{
    esp_log_level_set("*", ESP_LOG_ERROR);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    LatchPowerControlOn();

    SpiLcdDisplay* display = baji_185_create_lcd_display(true);
    if (display == nullptr) {
        
        ChargingOnlyPowerOffDeepSleep();
    }

    display->ShowChargingFullscreen(true);

    PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    backlight.SetBrightness(POWER_CHARGING_FULLSCREEN_BACKLIGHT);

    InitV5mDetectPin();

    const int poll_ms = 20;
    int hold_ms = 0;
    bool had_v5m = IsV5mPresent();

    for (;;) {
        bool v5m = IsV5mPresent();
        if (!v5m) {
            if (had_v5m) {
                charging_rtc_clear_boot_flags();
                display->ShowChargingFullscreen(false);
                ChargingOnlyPowerOffDeepSleep();
            }
        } else {
            had_v5m = true;
        }

        if (POWER_KEY_PRESSED()) {
            hold_ms += poll_ms;
            if (hold_ms >= POWER_KEY_HOLD_MS_TO_BOOT) {
                charging_rtc_clear_boot_flags();
                AbnormalReporter::MarkExpectedReset("charging_boot");
                esp_restart();
            }
        } else {
            hold_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}
