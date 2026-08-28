#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "sdkconfig.h"
#include "config.h"
#include "settings.h"
#include "charging_boot_rtc.h"
#include "power_latch.h"
#include "power_recovery_rtc.h"
#include "abnormal_reporter.h"
#include "mqtt_control.h"
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


class PowerManager {
private:
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_timer_handle_t power_timer_handle_ = nullptr;
    esp_timer_handle_t protection_timer_handle_ = nullptr;
    TaskHandle_t power_key_failsafe_task_handle_ = nullptr;
    inline static constexpr uint32_t kPowerKeyFailsafeStackSize = 3072;
    inline static StaticTask_t power_key_failsafe_task_buffer_{};
    inline static StackType_t power_key_failsafe_stack_[kPowerKeyFailsafeStackSize]{};
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    std::function<void()> on_shutdown_requested_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> battery_mv_samples_;
    std::vector<uint16_t> fast_battery_mv_samples_;
    std::atomic<uint8_t> battery_level_{30};
    uint8_t measured_battery_level_ = 30;
    uint8_t persisted_battery_level_ = 30;
    std::atomic<bool> is_charging_{false};
    std::atomic<bool> is_low_battery_{false};
    std::atomic<bool> is_battery_full_{false};
    bool is_first_battery_read_ = true;
    bool battery_state_loaded_ = false;
    int ticks_ = 0;
    int last_average_battery_mv_ = 0;
    uint16_t charge_state_settle_seconds_ = 0;
    uint16_t battery_level_step_seconds_ = 0;
    uint16_t near_full_charge_seconds_ = 0;
    uint16_t charge_state_debounce_seconds_ = 0;
    uint16_t battery_persist_seconds_ = 0;
    int latest_battery_mv_ = 0;
    int fast_average_battery_mv_ = 0;
    int64_t critical_battery_low_since_us_ = 0;
    std::atomic<bool> critical_shutdown_requested_{false};
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
    static constexpr int kLowBatteryLevel = POWER_BATTERY_LOW_LEVEL_PERCENT;
    static constexpr int kLowBatteryRecoverLevel = POWER_BATTERY_LOW_RECOVER_LEVEL_PERCENT;
    static constexpr int kChargeStateSettleTime = POWER_BATTERY_CHARGE_SETTLE_SECONDS;
    static constexpr int kChargeStateDebounceTime = POWER_BATTERY_CHARGE_DEBOUNCE_SECONDS;
    static constexpr int kChargingLevelStepInterval = 12;
    static constexpr int kDischargingLevelStepInterval = 20;
    // Require a little headroom before declaring "full" on charge.
    static constexpr int kNearFullBatteryMv = POWER_BATTERY_FULL_VOLTAGE_MV;
    static constexpr int kNearFullReleaseBatteryMv = POWER_BATTERY_FULL_RELEASE_VOLTAGE_MV;
    static constexpr int kNearFullChargeTime = POWER_BATTERY_FULL_CONFIRM_SECONDS;
    static constexpr int kNearFullLevel = POWER_BATTERY_FULL_LEVEL_PERCENT;
    static constexpr int kBatteryChargeReportBiasMv = POWER_BATTERY_CHARGE_REPORTING_BIAS_MV;
    static constexpr int kBatteryTransitionSeedSamples = POWER_BATTERY_TRANSITION_SEED_SAMPLES;
    static constexpr int kFastLowBatteryDataCount = 3;
    static constexpr int kFastBatteryDataCount = 4;

    std::atomic<bool> new_charging_status_{false};
    bool pending_charging_status_ = false;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> shutdown_task_started_{false};
    bool power_key_raw_pressed_ = false;
    bool power_key_stable_pressed_ = false;
    bool power_key_long_press_handled_ = false;
    bool power_key_ignore_release_ = false;
    uint16_t power_key_debounce_ticks_ = 0;
    uint16_t power_key_press_ticks_ = 0;
    uint16_t power_key_click_window_ticks_ = 0;
    uint16_t power_key_click_guard_ticks_ = 0;
    uint8_t power_key_click_count_ = 0;

    static constexpr uint16_t kPowerKeyDebounceTicks =
        (POWER_KEY_DEBOUNCE_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyShutdownHoldTicks =
        (POWER_KEY_SHUTDOWN_HOLD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyDoubleClickWindowTicks =
        (POWER_KEY_DOUBLE_CLICK_WINDOW_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyMultiClickGuardTicks =
        (POWER_KEY_MULTI_CLICK_GUARD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;

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

    void CutPowerImmediately() {
        shutdown_requested_.store(true);
        baji_power_recovery_request_power_off();
        (void)baji_power::EnablePowerKeyWakeup();
        baji_power::CutPowerAndHoldLow();
    }

    void ResetFastBatteryProtection() {
        fast_battery_mv_samples_.clear();
        fast_average_battery_mv_ = 0;
        critical_battery_low_since_us_ = 0;
    }

    void PrimeBatteryHistory(int battery_mv) {
        if (battery_mv <= 0) {
            return;
        }

        const uint16_t seed_mv = static_cast<uint16_t>(std::clamp(battery_mv, 0, 0xFFFF));
        battery_mv_samples_.clear();
        battery_mv_samples_.reserve(kBatteryAdcDataCount);

        const size_t seed_count = std::min<size_t>(
            static_cast<size_t>(kBatteryTransitionSeedSamples),
            static_cast<size_t>(kBatteryAdcDataCount));
        for (size_t i = 0; i < seed_count; ++i) {
            battery_mv_samples_.push_back(seed_mv);
        }

        last_average_battery_mv_ = static_cast<int>(seed_mv);
    }

    void RecordFastBatterySample(int battery_mv) {
        fast_battery_mv_samples_.push_back(static_cast<uint16_t>(battery_mv));
        if (fast_battery_mv_samples_.size() > kFastBatteryDataCount) {
            fast_battery_mv_samples_.erase(fast_battery_mv_samples_.begin());
        }

        uint32_t total_mv = 0;
        for (const auto value : fast_battery_mv_samples_) {
            total_mv += value;
        }
        if (!fast_battery_mv_samples_.empty()) {
            fast_average_battery_mv_ = static_cast<int>(
                total_mv / fast_battery_mv_samples_.size());
        }
    }

    void TriggerCriticalBatteryShutdown() {
        if (critical_shutdown_requested_.exchange(true)) {
            return;
        }

        // A critical voltage is a power-integrity condition, not a UI event.
        // Cut the latch through the independent high-priority failsafe before
        // a brownout can reset the MCU and restart the LCD indefinitely.
        ESP_LOGE("PowerManager",
                 "Critical battery voltage (%d mV, fast avg %d mV); cutting power",
                 latest_battery_mv_, fast_average_battery_mv_);
        shutdown_requested_.store(true);
        if (timer_handle_ != nullptr) {
            (void)esp_timer_stop(timer_handle_);
        }
        if (protection_timer_handle_ != nullptr) {
            (void)esp_timer_stop(protection_timer_handle_);
        }
        baji_power_recovery_request_power_off();
        if (power_key_failsafe_task_handle_ != nullptr) {
            xTaskNotifyGive(power_key_failsafe_task_handle_);
        } else {
            CutPowerImmediately();
        }
    }

    void CheckFastBatteryProtection() {
        if (shutdown_requested_.load()) {
            return;
        }

        bool charging = false;
        if (!ReadChargingStatus(charging)) {
            return;
        }

        // Use the raw charger pin here instead of waiting for the one-second
        // status debounce.  A newly attached USB cable must cancel an
        // in-flight critical shutdown immediately.
        if (charging || is_charging_.load()) {
            ResetFastBatteryProtection();
            return;
        }

        int battery_mv = 0;
        if (!ReadBatteryVoltageMv(battery_mv)) {
            return;
        }

        latest_battery_mv_ = std::clamp(battery_mv, 0, 0xFFFF);
        RecordFastBatterySample(latest_battery_mv_);
        // Do not wait for the one-second UI sampler to apply the load-shedding
        // policy.  The short window is already debounced enough for the low
        // battery hysteresis and lets the application cap the backlight within
        // roughly one sampling interval.
        if (fast_battery_mv_samples_.size() >= kFastLowBatteryDataCount) {
            UpdateLowBatteryStatus();
        }

        if (fast_battery_mv_samples_.size() < kFastBatteryDataCount) {
            return;
        }

        if (fast_average_battery_mv_ <= POWER_BATTERY_CRITICAL_VOLTAGE_MV) {
            const int64_t now_us = esp_timer_get_time();
            if (critical_battery_low_since_us_ == 0) {
                critical_battery_low_since_us_ = now_us;
            } else if (now_us - critical_battery_low_since_us_ >=
                       static_cast<int64_t>(POWER_BATTERY_CRITICAL_CONFIRM_MS) * 1000LL) {
                TriggerCriticalBatteryShutdown();
            }
        } else {
            // Any sample above the critical threshold breaks the continuous
            // low-voltage interval. Do not carry elapsed time across a sag
            // followed by a partial recovery.
            critical_battery_low_since_us_ = 0;
        }
    }

    void EnterPowerOff() {
        // This GPIO action is intentionally repeatable. The running-system
        // failsafe task is the sole deep-sleep finalizer; pre-task boot paths
        // finish synchronously here.
        CutPowerImmediately();

        if (power_key_failsafe_task_handle_ != nullptr &&
            xTaskGetCurrentTaskHandle() != power_key_failsafe_task_handle_) {
            xTaskNotifyGive(power_key_failsafe_task_handle_);
            return;
        }

        while (POWER_KEY_PRESSED()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_deep_sleep_start();
    }

    static void PowerKeyFailsafeTask(void* arg) {
        auto* self = static_cast<PowerManager*>(arg);
        int64_t press_started_us = 0;
        bool force_cut_triggered = false;

        for (;;) {
            if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
                self->EnterPowerOff();
            }

            if (POWER_KEY_PRESSED()) {
                const int64_t now_us = esp_timer_get_time();
                if (press_started_us == 0) {
                    press_started_us = now_us;
                } else if (!force_cut_triggered &&
                           now_us - press_started_us >=
                               static_cast<int64_t>(POWER_KEY_FORCE_CUT_HOLD_MS) * 1000) {
                    force_cut_triggered = true;
                    self->EnterPowerOff();
                }
            } else {
                press_started_us = 0;
                force_cut_triggered = false;
            }

            vTaskDelay(pdMS_TO_TICKS(POWER_KEY_SCAN_INTERVAL_MS));
        }
    }

    void TriggerShutdownFromPowerKey() {
        shutdown_requested_.store(true);
        baji_power_recovery_request_power_off();
        shutdown();
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
        if (shutdown_requested_.load()) {
            return;
        }

        const bool raw_pressed = POWER_KEY_PRESSED();

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

        int saved_level = settings.GetInt("lv", battery_level_.load());
        saved_level = std::clamp(saved_level, 0, 100);
        persisted_battery_level_ = static_cast<uint8_t>(saved_level);
        battery_level_.store(persisted_battery_level_);
        measured_battery_level_ = persisted_battery_level_;
        battery_state_loaded_ = true;
    }

    void PersistBatteryState(bool force = false) {
        if (is_first_battery_read_ && !battery_state_loaded_) {
            return;
        }

        if (!force && battery_state_loaded_ && battery_level_.load() == persisted_battery_level_) {
            return;
        }

        if (!force && battery_persist_seconds_ < kBatteryPersistIntervalSeconds) {
            return;
        }

        Settings settings("batt", true);
        settings.SetBool("ok", true);
        settings.SetInt("lv", battery_level_.load());
        settings.SetInt("mv", last_average_battery_mv_);

        persisted_battery_level_ = battery_level_.load();
        battery_state_loaded_ = true;
        battery_persist_seconds_ = 0;
    }

    void UpdateLowBatteryStatus() {
        const bool is_charging = is_charging_.load();
        if (is_charging) {
            if (is_low_battery_.load()) {
                is_low_battery_.store(false);
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(false);
                }
            }
            return;
        }

        // The long window remains the source for the displayed percentage, but
        // protection must react before ten seconds of samples have accumulated.
        // Require a short confirmation window so one bad ADC conversion cannot
        // put a healthy unit into low-battery mode.
        const bool have_fast_window =
            fast_battery_mv_samples_.size() >= kFastLowBatteryDataCount;
        const bool have_smooth_window = battery_mv_samples_.size() >= kBatteryAdcDataCount;
        if (!have_fast_window && !have_smooth_window) {
            return;
        }

        const bool voltage_low = have_fast_window &&
            fast_average_battery_mv_ <= POWER_BATTERY_LOW_VOLTAGE_MV;
        const bool voltage_recovered = !have_fast_window ||
            fast_average_battery_mv_ >= POWER_BATTERY_LOW_RECOVER_VOLTAGE_MV;
        const bool level_low = have_smooth_window && measured_battery_level_ <= kLowBatteryLevel;
        const bool level_recovered = !have_smooth_window ||
            measured_battery_level_ >= kLowBatteryRecoverLevel;
        const bool old_low_battery_status = is_low_battery_.load();
        const bool new_low_battery_status = old_low_battery_status
            ? !(level_recovered && voltage_recovered)
            : level_low || voltage_low;

        if (new_low_battery_status != old_low_battery_status) {
            is_low_battery_.store(new_low_battery_status);
            ESP_LOGW("PowerManager", "Low battery protection %s (level=%u, avg=%d mV, fast=%d mV)",
                     new_low_battery_status ? "enabled" : "disabled",
                     static_cast<unsigned>(measured_battery_level_),
                     last_average_battery_mv_, fast_average_battery_mv_);
            if (on_low_battery_status_changed_) {
                on_low_battery_status_changed_(new_low_battery_status);
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
        is_charging_.store(charging);
        new_charging_status_.store(charging);
        pending_charging_status_ = charging;
        charge_state_debounce_seconds_ = 0;
        is_battery_full_.store(false);
        const int transition_seed_mv =
            last_average_battery_mv_ > 0 ? last_average_battery_mv_ : latest_battery_mv_;
        PrimeBatteryHistory(transition_seed_mv);
        ResetFastBatteryProtection();
        ticks_ = 0;
        charge_state_settle_seconds_ = kChargeStateSettleTime;
        battery_level_step_seconds_ = 0;
        near_full_charge_seconds_ = 0;
        battery_persist_seconds_ = 0;
        UpdateLowBatteryStatus();
        if (on_charging_status_changed_) {
            on_charging_status_changed_(charging);
        }
    }

    void CheckBatteryStatus() {
        if (shutdown_requested_.load()) {
            return;
        }

        bool raw_charging_status = false;
        if (!ReadChargingStatus(raw_charging_status)) {
            return;
        }

        const bool is_charging = is_charging_.load();
        if (raw_charging_status != is_charging) {
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
            new_charging_status_.store(is_charging);
            pending_charging_status_ = is_charging;
            charge_state_debounce_seconds_ = 0;
        }

        if (raw_charging_status != is_charging) {
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

        const uint8_t raw_estimated_level = EstimateBatteryLevelFromVoltage(last_average_battery_mv_);
        if (is_charging &&
            raw_estimated_level >= kNearFullLevel &&
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
        latest_battery_mv_ = battery_mv;
        RecordFastBatterySample(battery_mv);
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

        const bool charging = is_charging_.load();
        const uint8_t raw_estimated_level = EstimateBatteryLevelFromVoltage(last_average_battery_mv_);
        const int reported_battery_mv = charging
            ? std::max(0, last_average_battery_mv_ - kBatteryChargeReportBiasMv)
            : last_average_battery_mv_;
        measured_battery_level_ = EstimateBatteryLevelFromVoltage(reported_battery_mv);

        const bool full_charge_entered = charging &&
            raw_estimated_level >= kNearFullLevel &&
            last_average_battery_mv_ >= kNearFullBatteryMv &&
            near_full_charge_seconds_ >= kNearFullChargeTime;
        const bool full_charge_released = !charging ||
            last_average_battery_mv_ <= kNearFullReleaseBatteryMv;
        const bool was_battery_full = is_battery_full_.load();
        const bool battery_full = was_battery_full ? !full_charge_released : full_charge_entered;
        is_battery_full_.store(battery_full);
        if (battery_full) {
            measured_battery_level_ = 100;
            battery_level_.store(100);
            battery_level_step_seconds_ = 0;
            if (!was_battery_full) {
                PersistBatteryState(true);
            }
        }

        if (is_first_battery_read_) {
            is_first_battery_read_ = false;
            battery_level_.store(measured_battery_level_);
            PersistBatteryState(true);
        } else if (charge_state_settle_seconds_ == 0) {
            if (charging) {
                const uint8_t battery_level = battery_level_.load();
                if (measured_battery_level_ > battery_level) {
                    if (battery_level_step_seconds_ >= kChargingLevelStepInterval) {
                        battery_level_.store(static_cast<uint8_t>(
                            std::min<int>(battery_level + 1, measured_battery_level_)));
                        battery_level_step_seconds_ = 0;
                    }
                } else {
                    battery_level_step_seconds_ = 0;
                }
            } else {
                const uint8_t battery_level = battery_level_.load();
                if (measured_battery_level_ < battery_level) {
                    if (battery_level_step_seconds_ >= kDischargingLevelStepInterval) {
                        battery_level_.store(static_cast<uint8_t>(
                            std::max<int>(battery_level - 1, measured_battery_level_)));
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
        // Record the user's intent before any operation that can block. A WDT
        // reset during persistence must finish powering off, not boot again.
        baji_power_recovery_request_power_off();

        if (power_timer_handle_) {
            (void)esp_timer_stop(power_timer_handle_);
        }
        if (timer_handle_) {
            (void)esp_timer_stop(timer_handle_);
        }
        if (protection_timer_handle_) {
            (void)esp_timer_stop(protection_timer_handle_);
        }

        // Best-effort user-facing/audio handoff. The independent 5-second
        // failsafe never depends on this callback, LVGL, I2C, or audio.
        if (on_shutdown_requested_) {
            on_shutdown_requested_();
            // Give the UI/audio handoff enough time to be observable while
            // remaining far below the independent 5-second force-cut path.
            vTaskDelay(pdMS_TO_TICKS(700));
        }

#if POWER_CHARGE_DETECT_USE_GPIO
        if (gpio_get_level(charging_pin_) == POWER_USB_VBUS_ACTIVE_LEVEL) {
            charging_rtc_set_usb_shutdown_flag();
        }
#else
        if (new_charging_status_.load()) {
            charging_rtc_set_usb_shutdown_flag();
        }
#endif

        AbnormalReporter::MarkExpectedReset("poweroff");

        // Publish the retained offline status before cutting board power. The
        // independent force-cut path intentionally does not enter this
        // blocking sequence and relies on MQTT Last Will/timeout instead.
        MqttControl::GetInstance().Stop();

        PersistBatteryState(true);
        EnterPowerOff();
    }

public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        baji_power::ConfigurePowerKeyInput();
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

        if ((v5m_present && POWER_KEY_RELEASED()) ||
            baji_power_is_resume_reset(reset_reason) ||
            wc == ESP_SLEEP_WAKEUP_EXT0 || wc == ESP_SLEEP_WAKEUP_EXT1) {
            baji_power::KeepPowerOnAcrossReset();
        } else {
            const int poll_ms = 20;
            int elapsed = 0;
            
            while (elapsed < POWER_KEY_HOLD_MS_TO_BOOT) {
                if (POWER_KEY_RELEASED()) {
                    EnterPowerOff();
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(poll_ms));
                elapsed += poll_ms;
            }
            baji_power::KeepPowerOnAcrossReset();
        }

        power_key_failsafe_task_handle_ = xTaskCreateStatic(
            PowerKeyFailsafeTask,
            "power_key_failsafe",
            kPowerKeyFailsafeStackSize,
            this,
            configMAX_PRIORITIES - 1,
            power_key_failsafe_stack_,
            &power_key_failsafe_task_buffer_);
        if (power_key_failsafe_task_handle_ == nullptr) {
            ESP_LOGE("PowerManager", "Failed to create power-key failsafe task");
        }

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
        fast_battery_mv_samples_.reserve(kFastBatteryDataCount);
#if !POWER_CHARGE_DETECT_USE_GPIO
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_USBIN_ADC_CHANNEL, &chan_config));
#endif

        bool initial_charging_status = false;
        if (ReadChargingStatus(initial_charging_status)) {
            is_charging_.store(initial_charging_status);
            new_charging_status_.store(initial_charging_status);
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

        esp_timer_create_args_t protection_timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<PowerManager*>(arg);
                self->CheckFastBatteryProtection();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_protection_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&protection_timer_args, &protection_timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(
            protection_timer_handle_, POWER_BATTERY_PROTECTION_SAMPLE_MS * 1000));
    }

    ~PowerManager() {
        if (power_key_failsafe_task_handle_ != nullptr) {
            vTaskDelete(power_key_failsafe_task_handle_);
            power_key_failsafe_task_handle_ = nullptr;
        }
        PersistBatteryState(true);
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
            timer_handle_ = nullptr;
        }
        if (protection_timer_handle_) {
            esp_timer_stop(protection_timer_handle_);
            esp_timer_delete(protection_timer_handle_);
            protection_timer_handle_ = nullptr;
        }
        if (power_timer_handle_) {
            esp_timer_stop(power_timer_handle_);
            esp_timer_delete(power_timer_handle_);
            power_timer_handle_ = nullptr;
        }
        DeinitAdcCalibration();
        if (adc_handle_ != nullptr) {
            adc_oneshot_del_unit(adc_handle_);
            adc_handle_ = nullptr;
        }
    }

    bool IsCharging() {
        return is_charging_.load();
    }

    bool IsLowBattery() const {
        return is_low_battery_.load();
    }

    bool IsBatteryFull() const {
        return is_battery_full_.load();
    }

    bool IsDischarging() {
        
        return !is_charging_.load();
    }

    uint8_t GetBatteryLevel() {
        return battery_level_.load();
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = std::move(callback);
        if (on_low_battery_status_changed_ && is_low_battery_.load()) {
            on_low_battery_status_changed_(true);
        }
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }

    void OnShutdownRequested(std::function<void()> callback) {
        on_shutdown_requested_ = std::move(callback);
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
        shutdown_requested_.store(true);
        baji_power_recovery_request_power_off();

        bool expected = false;
        if (!shutdown_task_started_.compare_exchange_strong(expected, true)) {
            return;
        }

        BaseType_t ok = xTaskCreate(ShutdownTask, "pm_shutdown", 4096, this, tskIDLE_PRIORITY + 5, nullptr);
        if (ok != pdPASS) {
            ESP_LOGE("PowerManager", "Failed to create shutdown task; using power-key failsafe");
            if (power_key_failsafe_task_handle_ != nullptr) {
                EnterPowerOff();
            } else {
                // This fallback can run in ESP_TIMER_TASK, so it must not wait
                // or access NVS/I2C/LVGL.
                CutPowerImmediately();
            }
        }
    }
};
