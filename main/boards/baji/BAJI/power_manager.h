#pragma once
#include <vector>
#include <functional>

#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include "sdkconfig.h"
#include "button.h"
#include "board.h"
#include "config.h"
#include "charging_boot_rtc.h"
#include "assets/lang_config.h"
#include "abnormal_reporter.h"
#include "mqtt_control.h"
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


enum class PowerUiHint {
    ShuttingDown,
};

class PowerManager {
private:
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_timer_handle_t power_timer_handle_ = nullptr;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    uint32_t battery_level_ = 30;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    bool is_first_battery_read_ = true;
    int ticks_ = 0;
    uint16_t last_average_adc_ = 0;
    uint16_t charge_state_settle_seconds_ = 0;
    uint16_t battery_level_step_seconds_ = 0;
    uint16_t near_full_charge_seconds_ = 0;
    uint16_t charge_state_debounce_seconds_ = 0;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    const int kBatteryAdcInterval = 30; 
    const int kBatteryAdcDataCount = 10;
    const int kLowBatteryLevel = 20;
    const int kLowBatteryRecoverLevel = 25;
    const int kChargeStateSettleTime = 12;
    const int kChargeStateDebounceTime = 2;
    const int kChargingLevelStepInterval = 12;
    const int kDischargingLevelStepInterval = 20;
    const int kNearFullAdcThreshold = 2380;
    const int kNearFullChargeTime = 180;
    const int kNearFullLevel = 95;

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
    uint16_t power_key_click_window_ticks_ = 0;
    uint8_t power_key_click_count_ = 0;

    static constexpr uint16_t kPowerKeyDebounceTicks =
        (POWER_KEY_DEBOUNCE_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyShutdownHoldTicks =
        (POWER_KEY_SHUTDOWN_HOLD_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;
    static constexpr uint16_t kPowerKeyDoubleClickWindowTicks =
        (POWER_KEY_DOUBLE_CLICK_WINDOW_MS + POWER_KEY_SCAN_INTERVAL_MS - 1) / POWER_KEY_SCAN_INTERVAL_MS;

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
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
            timer_handle_ = nullptr;
        }
        shutdown_requested_ = true;
        shutdown();
    }

    void OnPowerKeyStablePress() {
        if (power_key_click_count_ > 0 &&
            power_key_click_window_ticks_ >= kPowerKeyDoubleClickWindowTicks) {
            FinalizePowerKeyClicks();
        }
        power_key_press_ticks_ = 0;
        power_key_long_press_handled_ = false;
    }

    void OnPowerKeyStableRelease() {
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
            if (on_power_triple_click_) {
                on_power_triple_click_();
            }
        }
    }

    void HandlePowerKey() {
        if (shutdown_requested_) {
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

        if (power_key_click_count_ > 0 && power_key_click_count_ < 3) {
            if (power_key_click_window_ticks_ < kPowerKeyDoubleClickWindowTicks) {
                power_key_click_window_ticks_++;
            }

            if (power_key_click_window_ticks_ >= kPowerKeyDoubleClickWindowTicks) {
                FinalizePowerKeyClicks();
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
        adc_values_.clear();
        ticks_ = 0;
        charge_state_settle_seconds_ = kChargeStateSettleTime;
        battery_level_step_seconds_ = 0;
        near_full_charge_seconds_ = 0;
        ReadBatteryAdcData();
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

        if (battery_level_step_seconds_ < 0xFFFF) {
            battery_level_step_seconds_++;
        }

        if (is_charging_ &&
            battery_level_ >= kNearFullLevel &&
            last_average_adc_ >= kNearFullAdcThreshold) {
            if (near_full_charge_seconds_ < 0xFFFF) {
                near_full_charge_seconds_++;
            }
        } else {
            near_full_charge_seconds_ = 0;
        }

        if (charge_state_settle_seconds_ > 0) {
            charge_state_settle_seconds_--;
            ReadBatteryAdcData();
            return;
        }

        
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();
        }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        if (!ReadAdcChannel(POWER_BATTERY_ADC_CHANNEL, adc_value)) {
            return;
        }

        
        adc_values_.push_back(adc_value);
        if (adc_values_.size() > kBatteryAdcDataCount) {
            adc_values_.erase(adc_values_.begin());
        }
        uint32_t average_adc = 0;
        for (auto value : adc_values_) {
            average_adc += value;
        }
        average_adc /= adc_values_.size();
        last_average_adc_ = average_adc;

        
        const struct {
            uint16_t adc;
            uint8_t level;
        } levels[] = {
            {1970, 0},   
            {2000, 5},   
            {2050, 15},  
            {2100, 30},  
            {2150, 45},  
            {2200, 60},  
            {2250, 72},  
            {2300, 82},  
            {2350, 90},  
            {2380, 96},  
            {2400, 100}  
        };

        uint8_t new_battery_level = battery_level_;
        
        if (average_adc < levels[0].adc) {
            new_battery_level = 0;
        } else if (average_adc >= levels[10].adc) {
            new_battery_level = 100;
        } else {
            for (int i = 0; i < 10; i++) {
                if (average_adc >= levels[i].adc && average_adc < levels[i+1].adc) {
                    float ratio = static_cast<float>(average_adc - levels[i].adc) / (levels[i+1].adc - levels[i].adc);
                    new_battery_level = levels[i].level + ratio * (levels[i+1].level - levels[i].level);
                    break;
                }
            }
        }

        if (is_charging_ &&
            new_battery_level >= kNearFullLevel &&
            average_adc >= kNearFullAdcThreshold &&
            near_full_charge_seconds_ >= kNearFullChargeTime) {
            new_battery_level = 100;
        }

        if (is_first_battery_read_) {
            is_first_battery_read_ = false;
            battery_level_ = new_battery_level;
        } else if (charge_state_settle_seconds_ == 0) {
            if (is_charging_) {
                if (new_battery_level > battery_level_) {
                    if (new_battery_level == 100 && near_full_charge_seconds_ >= kNearFullChargeTime) {
                        battery_level_ = 100;
                    } else if (battery_level_step_seconds_ >= kChargingLevelStepInterval) {
                        battery_level_ += 1;
                        battery_level_step_seconds_ = 0;
                    }
                } else {
                    battery_level_step_seconds_ = 0;
                }
            } else {
                if (new_battery_level < battery_level_) {
                    if (battery_level_step_seconds_ >= kDischargingLevelStepInterval) {
                        battery_level_ -= 1;
                        battery_level_step_seconds_ = 0;
                    }
                } else {
                    battery_level_step_seconds_ = 0;
                }
            }
        }

        
        if (adc_values_.size() >= kBatteryAdcDataCount) {
            bool new_low_battery_status = is_low_battery_
                ? battery_level_ < kLowBatteryRecoverLevel
                : battery_level_ <= kLowBatteryLevel;
            if (new_low_battery_status != is_low_battery_) {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(is_low_battery_);
                }
            }
        }

        
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

        if (power_timer_handle_) {
            esp_timer_stop(power_timer_handle_);
        }
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
#if !POWER_CHARGE_DETECT_USE_GPIO
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_USBIN_ADC_CHANNEL, &chan_config));
#endif

        bool initial_charging_status = false;
        if (ReadChargingStatus(initial_charging_status)) {
            is_charging_ = initial_charging_status;
            new_charging_status = initial_charging_status;
            pending_charging_status_ = initial_charging_status;
        }
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
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (power_timer_handle_) {
            esp_timer_stop(power_timer_handle_);
            esp_timer_delete(power_timer_handle_);
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
