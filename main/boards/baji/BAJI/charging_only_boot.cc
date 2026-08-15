
#include "config.h"
#include "charging_boot_rtc.h"
#include "abnormal_reporter.h"
#include "baji_185_bringup.h"
#include "power_latch.h"
#include "power_recovery_rtc.h"

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
    baji_power::ConfigurePowerKeyInput();
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
    baji_power::KeepPowerOnAcrossReset();
}


static void ChargingOnlyPowerOffDeepSleep(void)
{
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);

    baji_power_recovery_request_power_off();
    (void)baji_power::EnablePowerKeyWakeup();
    baji_power::CutPowerAndHoldLow();

    vTaskDelay(pdMS_TO_TICKS(200));
    while (POWER_KEY_PRESSED()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

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

    PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    backlight.SetBrightness(0);

    SpiLcdDisplay* display = baji_185_create_lcd_display(true);
    if (display == nullptr) {
        
        ChargingOnlyPowerOffDeepSleep();
    }

    display->ShowChargingFullscreen(true);
    vTaskDelay(pdMS_TO_TICKS(100));
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
