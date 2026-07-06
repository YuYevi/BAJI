
#include "config.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void board_boot_power_on_gate(void)
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_EXT1) {
        return;
    }

    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_INPUT;
    io.pin_bit_mask = (1ULL << Power_Dec);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    const int poll_ms = 20;
    const int stable_need = (POWER_KEY_STABLE_RELEASE_MS + poll_ms - 1) / poll_ms;

    for (;;) {
        int stable = 0;
        while (stable < stable_need) {
            if (POWER_KEY_RELEASED()) {
                stable++;
            } else {
                stable = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
        }
        

        while (POWER_KEY_RELEASED()) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
        }
        

        int elapsed = 0;
        bool aborted = false;
        while (elapsed < POWER_KEY_HOLD_MS_TO_BOOT) {
            if (POWER_KEY_RELEASED()) {
                aborted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            elapsed += poll_ms;
        }

        if (!aborted) {
            gpio_config_t po = {};
            po.intr_type = GPIO_INTR_DISABLE;
            po.mode = GPIO_MODE_OUTPUT;
            po.pin_bit_mask = (1ULL << Power_Control);
            po.pull_down_en = GPIO_PULLDOWN_ENABLE;
            po.pull_up_en = GPIO_PULLUP_DISABLE;
            gpio_config(&po);
            gpio_set_level(Power_Control, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            
            return;
        }

        
        while (POWER_KEY_PRESSED()) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_sleep_enable_ext1_wakeup((1ULL << Power_Dec), ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    }
}
