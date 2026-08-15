#pragma once

#include "config.h"

#include <driver/gpio.h>
#include <esp_sleep.h>

namespace baji_power {

inline void ConfigurePowerKeyInput() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_INPUT;
    config.pin_bit_mask = (1ULL << Power_Dec);
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    (void)gpio_config(&config);
}

inline void ConfigurePowerControlOutput() {
    gpio_config_t config = {};
    config.intr_type = GPIO_INTR_DISABLE;
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = (1ULL << Power_Control);
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    (void)gpio_config(&config);
}

// Preload the new output level while the RTC pad is still held, then release
// and re-arm the hold. This avoids a low-going glitch during reset recovery.
inline void SetPowerControlHeld(int level) {
    // Preload the GPIO output register before enabling the output on a cold
    // boot, then preload it again while an existing RTC pad hold is active.
    (void)gpio_set_level(Power_Control, level);
    ConfigurePowerControlOutput();
    (void)gpio_set_level(Power_Control, level);
    (void)gpio_hold_dis(Power_Control);
    (void)gpio_set_level(Power_Control, level);
    (void)gpio_hold_en(Power_Control);
}

inline void KeepPowerOnAcrossReset() {
    SetPowerControlHeld(1);
}

inline void CutPowerAndHoldLow() {
    SetPowerControlHeld(0);
}

inline esp_err_t EnablePowerKeyWakeup() {
    ConfigurePowerKeyInput();
    // ext1 wakeup IOs are additive in IDF 5.x. Remove any timer/touch/ULP or
    // stale GPIO source so an intentional power-off can only wake by key.
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(
        (1ULL << Power_Dec), ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        // A partially configured EXT1 source can conflict with EXT0.
        (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        err = esp_sleep_enable_ext0_wakeup(Power_Dec, 0);
    }
    return err;
}

}  // namespace baji_power
