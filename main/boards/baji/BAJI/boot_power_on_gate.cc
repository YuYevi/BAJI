
#include "config.h"
#include "power_latch.h"
#include "power_recovery_rtc.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

[[noreturn]] void PowerOffAfterRecoveryLoop()
{
    baji_power_recovery_request_power_off();
    (void)baji_power::EnablePowerKeyWakeup();
    baji_power::CutPowerAndHoldLow();

    while (POWER_KEY_PRESSED()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

}  // namespace

extern "C" void board_boot_power_on_gate(void)
{
    const BajiPowerRecoveryAction recovery_action =
        baji_power_recovery_action(esp_reset_reason());
    if (recovery_action == BajiPowerRecoveryAction::KeepPower) {
        baji_power::KeepPowerOnAcrossReset();
        return;
    }
    if (recovery_action == BajiPowerRecoveryAction::PowerOff) {
        PowerOffAfterRecoveryLoop();
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_EXT1) {
        if (baji_power_recovery_is_power_off_pending()) {
            PowerOffAfterRecoveryLoop();
        }
        return;
    }

    baji_power::ConfigurePowerKeyInput();

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
        

        int rearm_wait_ms = 0;
        while (POWER_KEY_RELEASED() && rearm_wait_ms < POWER_KEY_REARM_WAIT_MS) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
            rearm_wait_ms += poll_ms;
        }
        if (POWER_KEY_RELEASED()) {
            PowerOffAfterRecoveryLoop();
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
            baji_power::KeepPowerOnAcrossReset();
            vTaskDelay(pdMS_TO_TICKS(50));
            baji_power_recovery_allow_boot();
            return;
        }

        
        while (POWER_KEY_PRESSED()) {
            vTaskDelay(pdMS_TO_TICKS(poll_ms));
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        baji_power_recovery_request_power_off();
        (void)baji_power::EnablePowerKeyWakeup();
        baji_power::CutPowerAndHoldLow();
        esp_deep_sleep_start();
    }
}
