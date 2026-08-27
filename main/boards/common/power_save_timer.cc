#include "power_save_timer.h"
#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PowerSaveTimer"


PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown)
    : cpu_max_freq_(cpu_max_freq), seconds_to_sleep_(seconds_to_sleep), seconds_to_shutdown_(seconds_to_shutdown) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

PowerSaveTimer::~PowerSaveTimer() {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (power_save_timer_ != nullptr) {
        (void)esp_timer_stop(power_save_timer_);
        (void)esp_timer_delete(power_save_timer_);
        power_save_timer_ = nullptr;
    }
    enabled_.store(false);
}

void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", false)) {
            return;
        }

        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (enabled_.load() || power_save_timer_ == nullptr) {
            return;
        }

        ticks_.store(0);
        const esp_err_t ret = esp_timer_start_periodic(power_save_timer_, 1000000);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to start power-save timer: %s", esp_err_to_name(ret));
            return;
        }
        enabled_.store(true);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (power_save_timer_ == nullptr) {
            return;
        }

        if (enabled_.load()) {
            const esp_err_t ret = esp_timer_stop(power_save_timer_);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "Failed to stop power-save timer: %s", esp_err_to_name(ret));
            }
            enabled_.store(false);
        }
    }
    // WakeUp must run even when the timer was already disabled: a callback may
    // have entered sleep just before SetEnabled(false) acquired the mutex.
    WakeUp();
}

void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

void PowerSaveTimer::PowerSaveCheck() {
    if (!enabled_.load()) {
        return;
    }

    auto& app = Application::GetInstance();
    if (!in_sleep_mode_ && !app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    ticks_++;
    if (seconds_to_sleep_ != -1 && ticks_ >= seconds_to_sleep_) {
        bool entered_sleep = false;
        {
            // SetEnabled(false) can stop the timer while this callback is
            // already out of the timer queue. Re-check under the same lock
            // used by WakeUp so a stale callback cannot re-enter sleep.
            std::lock_guard<std::mutex> lock(timer_mutex_);
            if (enabled_.load() && !in_sleep_mode_.load()) {
                in_sleep_mode_.store(true);
                entered_sleep = true;
            }
        }
        if (entered_sleep) {
            if (on_enter_sleep_mode_) {
                app.Schedule([this]() {
                    if (in_sleep_mode_ && on_enter_sleep_mode_) {
                        on_enter_sleep_mode_();
                    }
                });
            }

            if (cpu_max_freq_ != -1) {
                
                auto& audio_service = app.GetAudioService();
                is_wake_word_running_ = audio_service.IsWakeWordRunning();
                if (is_wake_word_running_) {
                    audio_service.EnableWakeWordDetection(false);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec) {
                    codec->EnableInput(false);
                }

                esp_pm_config_t pm_config = {
                    .max_freq_mhz = cpu_max_freq_,
                    .min_freq_mhz = 40,
                    .light_sleep_enable = true,
                };
                esp_pm_configure(&pm_config);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (!enabled_.load()) {
            return;
        }
    }
    if (seconds_to_shutdown_ != -1 && ticks_ >= seconds_to_shutdown_ && on_shutdown_request_) {
        on_shutdown_request_();
    }
}

void PowerSaveTimer::WakeUp() {
    ticks_ = 0;
    bool was_sleeping = false;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (in_sleep_mode_.load()) {
            in_sleep_mode_.store(false);
            was_sleeping = true;
        }
    }
    if (was_sleeping) {
        
        if (cpu_max_freq_ != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = cpu_max_freq_,
                .min_freq_mhz = cpu_max_freq_,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            
            auto& app = Application::GetInstance();
            auto& audio_service = app.GetAudioService();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
        }

        if (on_exit_sleep_mode_) {
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                if (!in_sleep_mode_ && on_exit_sleep_mode_) {
                    on_exit_sleep_mode_();
                }
            });
        }
    }
}
