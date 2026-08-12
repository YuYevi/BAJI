#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "sdkconfig.h"
#include "button.h"
#include "board.h"
#include "config.h"
#include "settings.h"
#include "charging_boot_rtc.h"
#include "assets/lang_config.h"
#include "abnormal_reporter.h"
#include "mqtt_control.h"
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


enum class PowerUiHint {
    ShuttingDown,
};

class PowerManager {
private:
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_timer_handle_t power_timer_handle_ = nullptr;
    TaskHandle_t power_key_task_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> battery_mv_samples_;
    uint8_t battery_level_ = 30;
    uint8_t measured_battery_level_ = 30;
    uint8_t persisted_battery_level_ = 30;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool is_first_battery_read_ = true;
    bool battery_state_loaded_ = false;
    int ticks_ = 0;
    int last_average_battery_mv_ = 0;
    uint16_t charge_state_settle_seconds_ = 0;
    uint16_t battery_level_step_seconds_ = 0;
    uint16_t near_full_charge_seconds_ = 0;
    uint16_t charge_state_debounce_seconds_ = 0;
    uint16_t battery_persist_seconds_ = 0;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    enum class AdcCaliScheme {
        None,
        CurveFitting,
        LineFitting,
    };
    AdcCaliScheme adc_cali_scheme_ = AdcCaliScheme::None;
    bool adc_calibration_ready_ = false;

    struct BatteryPoint {
        uint16_t mv;
        uint8_t level;
    };

    // Measured BAJI pack range: full is about 4.10V, while hardware cut-off is about 3.00V. Keep reserve at the bottom so the UI reaches 0% before shutoff.
    inline static constexpr std::array<BatteryPoint, 21> kBatteryCurve = {{
        {4100, 100}, {4065, 95}, {4025, 90}, {3985, 85}, {3945, 80},
        {3905, 75},  {3865, 70}, {3825, 65}, {3790, 60}, {3755, 55},
        {3720, 50},  {3685, 45}, {3650, 40}, {3615, 35}, {3580, 30},
        {3540, 25},  {3500, 20}, {3420, 15}, {3340, 10}, {3240, 5},
        {3150, 0},
    }};

    static constexpr int kBatteryAdcInterval = 1;
    static constexpr int kBatteryAdcDataCount = 10;
    static constexpr int kBatteryAdcSampleCount = 7;
    static constexpr int kBatteryAdcTrimCount = 1;
    static constexpr int kBatteryFallbackFullScaleMv = 3900;
    static constexpr float kBatteryDividerRatio = 2.0f;  // R8=200k, R13=200k in the BAJI netlist.
    static constexpr int kBatteryPersistIntervalSeconds = 180;
    static constexpr int kLowBatteryLevel = 20;
    static constexpr int kLowBatteryRecoverLevel = 25;
    static constexpr int kChargeStateSettleTime = 20;
    static constexpr int kChargeStateDebounceTime = 2;
    static constexpr int kChargingLevelStepInterval = 12;
    static constexpr int kDischargingLevelStepInterval = 20;
    // Require a little headroom before declaring "full" on charge.
    static constexpr int kNearFullBatteryMv = 4050;
    static constexpr int kNearFullChargeTime = 180;
    static constexpr int kNearFullLevel = 95;

    bool new_charging_status = false;
    bool pending_charging_status_ = false;
    bool shutdown_requested_ = false;
    bool shutdown_first_ = true;
    bool power_key_raw_pressed_ = false;
    bool power_key_stable_pressed_ = false;
    bool power_key_long_press_handled_ = false;
    bool power_key_ignore_release_ = false;
    uint16_t power_key_debounce_ticks_ = 0;
    uint16_t power_key_press_ticks_ = 0;
    uint16_t power_key_force_press_ticks_ = 0;
    uint16_t power_key_click_window_ticks_ = 0;
    uint16_t power_key_click_guard_ticks_ = 0;
    uint8_t power_key_click_count_ = 0;
    bool power_key_force_reset_handled_ = false;

    static constexpr uint16_t kPowerKeyDebounceTicks =
        (POWER_KEY_DEBOUNCE_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyShutdownHoldTicks =
        (POWER_KEY_SHUTDOWN_HOLD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyForceResetHoldTicks =
        (POWER_KEY_FORCE_RESET_HOLD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyDoubleClickWindowTicks =
        (POWER_KEY_DOUBLE_CLICK_WINDOW_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyMultiClickGuardTicks =
        (POWER_KEY_MULTI_CLICK_GUARD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;

    std::function<void(PowerUiHint)> on_power_ui_;
    std::function<void()> on_power_single_click_;
    std::function<void()> on_power_double_click_;
    std::function<void()> on_power_triple_click_;

    void ResetPowerKeyClickState() {
        power_key_click_count_ = 0;
        power_key_click_window_ticks_ = 0;
    }

    void FinalizePowerKeyClicks() {
        const uint8_t click_count = power_key_click_count_;
        ResetPowerKeyClickState();

        if (click_count == 1) {
            if (on_power_single_click_) {
                on_power_single_click_();
            }
            return;
        }

        if (click_count == 2 && on_power_double_click_) {
            on_power_double_click_();
        }
    }

    void TriggerShutdownFromPowerKey() {
        shutdown_requested_ = true;
        shutdown();
    }

    void TriggerForceResetFromPowerKey() {
        if (power_key_force_press_ticks_ == 0) {
            return;
        }

        power_key_force_press_ticks_ = 0;
        power_key_force_reset_handled_ = true;
        shutdown_requested_ = true;

        AbnormalReporter::MarkExpectedReset("poweroff_force_reset");
        ESP_LOGW("PowerManager", "Power key hard power off");

        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
        }
        if (power_timer_handle_) {
            esp_timer_stop(power_timer_handle_);
        }

        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);

        gpio_config_t wake_in = {};
        wake_in.intr_type = GPIO_INTR_DISABLE;
        wake_in.mode = GPIO_MODE_INPUT;
        wake_in.pin_bit_mask = (1ULL << Power_Dec);
        wake_in.pull_down_en = GPIO_PULLDOWN_DISABLE;
        wake_in.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&wake_in);

        esp_err_t err = esp_sleep_enable_ext1_wakeup((1ULL << Power_Dec), ESP_EXT1_WAKEUP_ANY_LOW);
        if (err != ESP_OK) {
            esp_sleep_enable_ext0_wakeup(Power_Dec, 0);
        }

        gpio_set_level(Power_Control, 0);
        vTaskDelay(pdMS_TO_TICKS(200));

        while (POWER_KEY_PRESSED()) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_deep_sleep_start();
    }

    void OnPowerKeyStablePress() {
        if (power_key_click_guard_ticks_ == 0 &&
            power_key_click_count_ > 0 &&
            power_key_click_window_ticks_ >= kPowerKeyDoubleClickWindowTicks) {
            FinalizePowerKeyClicks();
        }
        power_key_press_ticks_ = 0;
        power_key_long_press_handled_ = false;
    }

    void OnPowerKeyStableRelease() {
        if (power_key_click_guard_ticks_ > 0) {
            ResetPowerKeyClickState();
            power_key_press_ticks_ = 0;
            power_key_long_press_handled_ = false;
            return;
        }

        if (power_key_ignore_release_) {
            power_key_ignore_release_ = false;
            power_key_press_ticks_ = 0;
            power_key_long_press_handled_ = false;
            return;
        }

        if (power_key_long_press_handled_) {
            power_key_press_ticks_ = 0;
            power_key_long_press_handled_ = false;
            return;
        }

        power_key_press_ticks_ = 0;
        power_key_click_window_ticks_ = 0;
        if (power_key_click_count_ < 3) {
            power_key_click_count_++;
        }

        if (power_key_click_count_ >= 3) {
            ResetPowerKeyClickState();
            power_key_click_guard_ticks_ = kPowerKeyMultiClickGuardTicks;
            if (on_power_triple_click_) {
                on_power_triple_click_();
            }
        }
    }

    void HandlePowerKey() {
        const bool raw_pressed = POWER_KEY_PRESSED();

        if (raw_pressed) {
            if (power_key_force_press_ticks_ < kPowerKeyForceResetHoldTicks) {
                power_key_force_press_ticks_++;
            }
            if (!power_key_force_reset_handled_ &&
                power_key_force_press_ticks_ >= kPowerKeyForceResetHoldTicks) {
                power_key_long_press_handled_ = true;
                ResetPowerKeyClickState();
                TriggerForceResetFromPowerKey();
                return;
            }
        } else {
            power_key_force_press_ticks_ = 0;
            power_key_force_reset_handled_ = false;
        }

        if (shutdown_requested_) {
            return;
        }

        if (raw_pressed != power_key_raw_pressed_) {
            power_key_raw_pressed_ = raw_pressed;
            power_key_debounce_ticks_ = 0;
        } else if (power_key_debounce_ticks_ < kPowerKeyDebounceTicks) {
            power_key_debounce_ticks_++;
        }

        if (power_key_debounce_ticks_ >= kPowerKeyDebounceTicks &&
            power_key_stable_pressed_ != power_key_raw_pressed_) {
            power_key_stable_pressed_ = power_key_raw_pressed_;
            if (power_key_stable_pressed_) {
                OnPowerKeyStablePress();
            } else {
                OnPowerKeyStableRelease();
            }
        }

        if (power_key_click_guard_ticks_ > 0) {
            if (power_key_stable_pressed_) {
                // Keep the guard active while the key is pressed, but still
                // allow a deliberate long press to reach the shutdown logic.
                power_key_click_guard_ticks_ = kPowerKeyMultiClickGuardTicks;
            } else {
                power_key_press_ticks_ = 0;
                power_key_long_press_handled_ = false;
                if (!power_key_raw_pressed_) {
                    power_key_click_guard_ticks_--;
                }
                return;
            }
        }

        if (power_key_stable_pressed_) {
            if (power_key_press_ticks_ < kPowerKeyShutdownHoldTicks) {
                power_key_press_ticks_++;
            }

            if (!power_key_long_press_handled_ &&
                power_key_press_ticks_ >= kPowerKeyShutdownHoldTicks) {
                power_key_long_press_handled_ = true;
                ResetPowerKeyClickState();
                TriggerShutdownFromPowerKey();
            }
            return;
        }

        if (!power_key_raw_pressed_ &&
            power_key_click_count_ > 0 && power_key_click_count_ < 3) {
            if (power_key_click_window_ticks_ < kPowerKeyDoubleClickWindowTicks) {
                power_key_click_window_ticks_++;
            }

            if (power_key_click_window_ticks_ >= kPowerKeyDoubleClickWindowTicks) {
                FinalizePowerKeyClicks();
            }
        }
    }

    static void PowerKeyTask(void* arg) {
        auto* self = static_cast<PowerManager*>(arg);

#if CONFIG_ESP_TASK_WDT_EN
        esp_err_t wdt_err = esp_task_wdt_add(nullptr);
        if (wdt_err != ESP_OK && wdt_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW("PowerManager", "Failed to add power key task to watchdog: %s",
                     esp_err_to_name(wdt_err));
        }
#endif

        while (true) {
            self->HandlePowerKey();
#if CONFIG_ESP_TASK_WDT_EN
            esp_task_wdt_reset();
#endif
            vTaskDelay(pdMS_TO_TICKS(POWER_KEY_SCAN_INTERVAL_MS));
        }
    }

    bool InitAdcCalibration(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten) {
        adc_cali_handle_ = nullptr;
        adc_cali_scheme_ = AdcCaliScheme::None;

        esp_err_t ret = ESP_FAIL;
        bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (!calibrated) {
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = unit,
                .chan = channel,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ret = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_);
            if (ret == ESP_OK) {
                adc_cali_scheme_ = AdcCaliScheme::CurveFitting;
                calibrated = true;
            }
        }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!calibrated) {
            adc_cali_line_fitting_config_t cali_config = {
                .unit_id = unit,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle_);
            if (ret == ESP_OK) {
                adc_cali_scheme_ = AdcCaliScheme::LineFitting;
                calibrated = true;
            }
        }
#endif

        adc_calibration_ready_ = calibrated;
        if (!calibrated) {
            adc_cali_handle_ = nullptr;
            adc_cali_scheme_ = AdcCaliScheme::None;
            ESP_LOGW("PowerManager", "ADC calibration unavailable, using fallback conversion");
        }
        return calibrated;
    }

    void DeinitAdcCalibration() {
        if (adc_cali_handle_ == nullptr) {
            return;
        }

        if (adc_cali_scheme_ == AdcCaliScheme::CurveFitting) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(adc_cali_handle_));
#endif
        } else if (adc_cali_scheme_ == AdcCaliScheme::LineFitting) {
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(adc_cali_handle_));
#endif
        }
        adc_cali_handle_ = nullptr;
        adc_cali_scheme_ = AdcCaliScheme::None;
        adc_calibration_ready_ = false;
    }

    bool ConvertRawToMillivolts(int raw, int& voltage_mv) {
        if (adc_cali_handle_ != nullptr && adc_calibration_ready_) {
            esp_err_t ret = adc_cali_raw_to_voltage(adc_cali_handle_, raw, &voltage_mv);
            if (ret == ESP_OK) {
                return true;
            }
            ESP_LOGW("PowerManager", "ADC calibration conversion failed: %s", esp_err_to_name(ret));
        }

        voltage_mv = (raw * kBatteryFallbackFullScaleMv) / 4095;
        return true;
    }

    bool ReadBatteryVoltageMv(int& battery_mv) {
        std::array<int, kBatteryAdcSampleCount> samples {};
        int sample_count = 0;

        for (int i = 0; i < kBatteryAdcSampleCount; ++i) {
            int raw = 0;
            if (!ReadAdcChannel(POWER_BATTERY_ADC_CHANNEL, raw)) {
                return false;
            }

            int pin_mv = 0;
            if (!ConvertRawToMillivolts(raw, pin_mv)) {
                return false;
            }

            samples[sample_count++] = pin_mv;
        }

        std::sort(samples.begin(), samples.begin() + sample_count);

        int start = 0;
        int end = sample_count;
        if (sample_count > (kBatteryAdcTrimCount * 2)) {
            start = kBatteryAdcTrimCount;
            end = sample_count - kBatteryAdcTrimCount;
        }

        int pin_mv_sum = 0;
        int pin_mv_count = 0;
        for (int i = start; i < end; ++i) {
            pin_mv_sum += samples[i];
            pin_mv_count++;
        }

        if (pin_mv_count == 0) {
            return false;
        }

        const int pin_mv = (pin_mv_sum + (pin_mv_count / 2)) / pin_mv_count;
        battery_mv = static_cast<int>(std::lround(static_cast<double>(pin_mv) * kBatteryDividerRatio));
        return true;
    }

    uint8_t EstimateBatteryLevelFromVoltage(int battery_mv) const {
        if (battery_mv >= kBatteryCurve.front().mv) {
            return 100;
        }
        if (battery_mv <= kBatteryCurve.back().mv) {
            return 0;
        }

        for (size_t i = 0; i + 1 < kBatteryCurve.size(); ++i) {
            const BatteryPoint& upper = kBatteryCurve[i];
            const BatteryPoint& lower = kBatteryCurve[i + 1];
            if (battery_mv <= upper.mv && battery_mv >= lower.mv) {
                const float span = static_cast<float>(upper.mv - lower.mv);
                const float ratio = span > 0.0f
                    ? static_cast<float>(battery_mv - lower.mv) / span
                    : 0.0f;
                const float level = lower.level + ratio * (upper.level - lower.level);
                return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(level)), 0, 100));
            }
        }

        return 0;
    }

    void LoadBatteryState() {
        Settings settings("batt");
        if (!settings.GetBool("ok", false)) {
            battery_state_loaded_ = false;
            return;
        }

        int saved_level = settings.GetInt("lv", battery_level_);
        saved_level = std::clamp(saved_level, 0, 100);
        persisted_battery_level_ = static_cast<uint8_t>(saved_level);
        battery_level_ = persisted_battery_level_;
        measured_battery_level_ = persisted_battery_level_;
        battery_state_loaded_ = true;
        is_first_battery_read_ = false;
    }

    void PersistBatteryState(bool force = false) {
        if (is_first_battery_read_ && !battery_state_loaded_) {
            return;
        }

        if (!force && battery_state_loaded_ && battery_level_ == persisted_battery_level_) {
            return;
        }

        if (!force && battery_persist_seconds_ < kBatteryPersistIntervalSeconds) {
            return;
        }

        Settings settings("batt", true);
        settings.SetBool("ok", true);
        settings.SetInt("lv", battery_level_);
        settings.SetInt("mv", last_average_battery_mv_);

        persisted_battery_level_ = battery_level_;
        battery_state_loaded_ = true;
        battery_persist_seconds_ = 0;
    }

    void UpdateLowBatteryStatus() {
        if (is_charging_) {
            if (is_low_battery_) {
                is_low_battery_ = false;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(false);
                }
            }
            return;
        }

        if (battery_mv_samples_.size() < kBatteryAdcDataCount) {
            return;
        }

        bool new_low_battery_status = false;
        new_low_battery_status = is_low_battery_
            ? measured_battery_level_ < kLowBatteryRecoverLevel
            : measured_battery_level_ <= kLowBatteryLevel;

        if (new_low_battery_status != is_low_battery_) {
            is_low_battery_ = new_low_battery_status;
            if (on_low_battery_status_changed_) {
                on_low_battery_status_changed_(is_low_battery_);
            }
        }
    }

    bool ReadAdcChannel(adc_channel_t channel, int& adc_value) {
        if (adc_handle_ == nullptr) {
            ESP_LOGW("PowerManager", "ADC handle is not ready");
            return false;
        }

        esp_err_t err = adc_oneshot_read(adc_handle_, channel, &adc_value);
        if (err != ESP_OK) {
            ESP_LOGW("PowerManager", "ADC read channel %d failed: %s",
                     static_cast<int>(channel), esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool ReadChargingStatus(bool& charging) {
#if POWER_CHARGE_DETECT_USE_GPIO
        charging = (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL);
        return true;
#else
        int usb_adc_value = 0;
        if (!ReadAdcChannel(POWER_USBIN_ADC_CHANNEL, usb_adc_value)) {
            return false;
        }
        charging = (1500 < usb_adc_value && usb_adc_value < 4000);
        return true;
#endif
    }

    void ApplyChargingStatus(bool charging) {
        is_charging_ = charging;
        new_charging_status = charging;
        pending_charging_status_ = charging;
        charge_state_debounce_seconds_ = 0;
        battery_mv_samples_.clear();
        ticks_ = 0;
        charge_state_settle_seconds_ = kChargeStateSettleTime;
        battery_level_step_seconds_ = 0;
        near_full_charge_seconds_ = 0;
        battery_persist_seconds_ = 0;
        ReadBatteryAdcData();
        PersistBatteryState(true);
        if (on_charging_status_changed_) {
            on_charging_status_changed_(is_charging_);
        }
    }

    void CheckBatteryStatus() {
        bool raw_charging_status = false;
        if (!ReadChargingStatus(raw_charging_status)) {
            return;
        }

        if (raw_charging_status != is_charging_) {
            if (raw_charging_status != pending_charging_status_) {
                pending_charging_status_ = raw_charging_status;
                charge_state_debounce_seconds_ = 1;
            } else if (charge_state_debounce_seconds_ < 0xFFFF) {
                charge_state_debounce_seconds_++;
            }

            if (charge_state_debounce_seconds_ >= kChargeStateDebounceTime) {
                ApplyChargingStatus(raw_charging_status);
                return;
            }
        } else {
            new_charging_status = is_charging_;
            pending_charging_status_ = is_charging_;
            charge_state_debounce_seconds_ = 0;
        }

        if (raw_charging_status != is_charging_) {
            return;
        }

        if (charge_state_settle_seconds_ > 0) {
            charge_state_settle_seconds_--;
            ReadBatteryAdcData();
            return;
        }

        if (battery_level_step_seconds_ < 0xFFFF) {
            battery_level_step_seconds_++;
        }

        if (is_charging_ &&
            measured_battery_level_ >= kNearFullLevel &&
            last_average_battery_mv_ >= kNearFullBatteryMv) {
            if (near_full_charge_seconds_ < 0xFFFF) {
                near_full_charge_seconds_++;
            }
        } else {
            near_full_charge_seconds_ = 0;
        }

        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }

        if (battery_persist_seconds_ < 0xFFFF) {
            battery_persist_seconds_++;
        }
        PersistBatteryState();
    }

    void ReadBatteryAdcData() {
        int battery_mv = 0;
        if (!ReadBatteryVoltageMv(battery_mv)) {
            return;
        }

        battery_mv = std::clamp(battery_mv, 0, 0xFFFF);
        battery_mv_samples_.push_back(static_cast<uint16_t>(battery_mv));
        if (battery_mv_samples_.size() > kBatteryAdcDataCount) {
            battery_mv_samples_.erase(battery_mv_samples_.begin());
        }
        uint32_t average_battery_mv = 0;
        for (auto value : battery_mv_samples_) {
            average_battery_mv += value;
        }
        average_battery_mv /= battery_mv_samples_.size();
        last_average_battery_mv_ = static_cast<int>(average_battery_mv);

        measured_battery_level_ = EstimateBatteryLevelFromVoltage(last_average_battery_mv_);
        if (is_charging_ &&
            measured_battery_level_ >= kNearFullLevel &&
            last_average_battery_mv_ >= kNearFullBatteryMv &&
            near_full_charge_seconds_ >= kNearFullChargeTime) {
            measured_battery_level_ = 100;
        }

        if (is_first_battery_read_) {
            is_first_battery_read_ = false;
            battery_level_ = measured_battery_level_;
            PersistBatteryState(true);
        } else if (charge_state_settle_seconds_ == 0) {
            if (is_charging_) {
                if (measured_battery_level_ > battery_level_) {
                    if (battery_level_step_seconds_ >= kChargingLevelStepInterval) {
                        battery_level_ = static_cast<uint8_t>(std::min<int>(battery_level_ + 1, measured_battery_level_));
                        battery_level_step_seconds_ = 0;
                    }
                } else {
                    battery_level_step_seconds_ = 0;
                }
            } else {
                if (measured_battery_level_ < battery_level_) {
                    if (battery_level_step_seconds_ >= kDischargingLevelStepInterval) {
                        battery_level_ = static_cast<uint8_t>(std::max<int>(battery_level_ - 1, measured_battery_level_));
                        battery_level_step_seconds_ = 0;
                    }
                } else {
                    battery_level_step_seconds_ = 0;
                }
            }
        }

        UpdateLowBatteryStatus();
    }

    static void ShutdownTask(void* arg) {
        auto* self = static_cast<PowerManager*>(arg);
        self->RunShutdownSequence();
        vTaskDelete(nullptr);
    }

    void RunShutdownSequence() {
        if (on_power_ui_) {
            on_power_ui_(PowerUiHint::ShuttingDown);
        }
        vTaskDelay(pdMS_TO_TICKS(500));

        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
        }

        if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
            codec->SetOutputVolume(0, false);
            vTaskDelay(pdMS_TO_TICKS(20));
            codec->EnableOutput(false);
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        AbnormalReporter::MarkExpectedReset("poweroff");
        // Try to publish the MQTT offline status before board power is cut.
        MqttControl::GetInstance().Stop();
        PersistBatteryState(true);

#if POWER_CHARGE_DETECT_USE_GPIO
        if (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL) {
            charging_rtc_set_usb_shutdown_flag();
        }
#else
        if (new_charging_status) {
            charging_rtc_set_usb_shutdown_flag();
        }
#endif

        gpio_set_level(DISPLAY_BACKLIGHT_PIN, 0);

        
        gpio_config_t wake_in = {};
        wake_in.intr_type = GPIO_INTR_DISABLE;
        wake_in.mode = GPIO_MODE_INPUT;
        wake_in.pin_bit_mask = (1ULL << Power_Dec);
        wake_in.pull_down_en = GPIO_PULLDOWN_DISABLE;
        wake_in.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&wake_in);

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

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        
        gpio_config_t powerdecgpio_conf = {};
        powerdecgpio_conf.intr_type = GPIO_INTR_DISABLE;
        powerdecgpio_conf.mode = GPIO_MODE_INPUT;
        powerdecgpio_conf.pin_bit_mask = (1ULL << Power_Dec);
        powerdecgpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        powerdecgpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&powerdecgpio_conf);
        power_key_raw_pressed_ = POWER_KEY_PRESSED();
        power_key_stable_pressed_ = power_key_raw_pressed_;
        power_key_ignore_release_ = power_key_stable_pressed_;

        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
#if POWER_CHARGE_DETECT_USE_GPIO
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#else
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
#endif
        gpio_config(&io_conf);

        const bool v5m_present = (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL);
        const esp_reset_reason_t reset_reason = esp_reset_reason();
        const esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();

        
        gpio_config_t powercontgpio_conf = {};
        powercontgpio_conf.intr_type = GPIO_INTR_DISABLE;
        powercontgpio_conf.mode = GPIO_MODE_OUTPUT;
        powercontgpio_conf.pin_bit_mask = (1ULL << Power_Control); 
        powercontgpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE; 
        powercontgpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;     
        gpio_config(&powercontgpio_conf);
        
        if ((v5m_present && POWER_KEY_RELEASED()) || reset_reason == ESP_RST_SW ||
            wc == ESP_SLEEP_WAKEUP_EXT0 || wc == ESP_SLEEP_WAKEUP_EXT1) {
            gpio_set_level(Power_Control, 1);
            
        } else {
            const int poll_ms = 20;
            int elapsed = 0;
            
            while (elapsed < POWER_KEY_HOLD_MS_TO_BOOT) {
                if (POWER_KEY_RELEASED()) {
                    
                    RunShutdownSequence();
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
                elapsed += poll_ms;
            }
            gpio_set_level(Power_Control, 1);
            
        }
        
        
        BaseType_t power_key_task_ok = xTaskCreate(PowerKeyTask, "power_key_scan", 3072, this,
                                                   tskIDLE_PRIORITY + 11, &power_key_task_handle_);
        if (power_key_task_ok != pdPASS) {
            ESP_LOGW("PowerManager", "Failed to create power key task, falling back to esp_timer");
            esp_timer_create_args_t power_timer_args = {
                .callback = [](void* arg) {
                    PowerManager* self = static_cast<PowerManager*>(arg);
                    self->HandlePowerKey();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "power_cotrol_timer",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&power_timer_args, &power_timer_handle_));
            ESP_ERROR_CHECK(esp_timer_start_periodic(power_timer_handle_, POWER_KEY_SCAN_INTERVAL_MS * 1000));
        }

        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = POWER_CBS_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_BATTERY_ADC_CHANNEL, &chan_config));
        InitAdcCalibration(POWER_CBS_ADC_UNIT, POWER_BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_12);
        battery_mv_samples_.reserve(kBatteryAdcDataCount);
#if !POWER_CHARGE_DETECT_USE_GPIO
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_USBIN_ADC_CHANNEL, &chan_config));
#endif

        bool initial_charging_status = false;
        if (ReadChargingStatus(initial_charging_status)) {
            is_charging_ = initial_charging_status;
            new_charging_status = initial_charging_status;
            pending_charging_status_ = initial_charging_status;
        }
        LoadBatteryState();
        ReadBatteryAdcData();

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                PowerManager* self = static_cast<PowerManager*>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));
    }

    ~PowerManager() {
        PersistBatteryState(true);
        DeinitAdcCalibration();
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (power_timer_handle_) {
            esp_timer_stop(power_timer_handle_);
            esp_timer_delete(power_timer_handle_);
        }
        if (power_key_task_handle_ != nullptr) {
            vTaskDelete(power_key_task_handle_);
            power_key_task_handle_ = nullptr;
        }
        if (adc_handle_ != nullptr) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() {
        return is_charging_;
    }

    bool IsDischarging() {
        
        return !is_charging_;
    }

    uint8_t GetBatteryLevel() {
        return battery_level_;
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }

    void OnPowerUi(std::function<void(PowerUiHint)> callback) {
        on_power_ui_ = std::move(callback);
    }

    void OnPowerSingleClick(std::function<void()> callback) {
        on_power_single_click_ = std::move(callback);
    }

    void OnPowerDoubleClick(std::function<void()> callback) {
        on_power_double_click_ = std::move(callback);
    }

    void OnPowerTripleClick(std::function<void()> callback) {
        on_power_triple_click_ = std::move(callback);
    }

    void shutdown() {
        if (!shutdown_first_) {
            return;
        }
        shutdown_first_ = false;
        BaseType_t ok = xTaskCreate(ShutdownTask, "pm_shutdown", 4096, this, tskIDLE_PRIORITY + 5, nullptr);
        if (ok != pdPASS) {
            
            RunShutdownSequence();
        }
    }
};
