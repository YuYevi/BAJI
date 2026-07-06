#include "backlight.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/ledc.h>

#define TAG "Backlight"

namespace {
void StopTransitionTimer(esp_timer_handle_t timer) {
    if (timer == nullptr) {
        return;
    }

    esp_err_t ret = esp_timer_stop(timer);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
}
}


Backlight::Backlight() {
    
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnTransitionTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));
}

Backlight::~Backlight() {
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
}

void Backlight::RestoreBrightness() {
    
    Settings settings("display");  
    int saved_brightness = settings.GetInt("brightness", 100);
    
    
    if (saved_brightness <= 0) {
        saved_brightness = 100;  
    }
    
    SetBrightness(saved_brightness);
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    if (permanent) {
        Settings settings("display", true);
        settings.SetInt("brightness", brightness);
    }

    if (brightness_ == brightness && target_brightness_ == brightness) {
        return;
    }

    target_brightness_ = brightness;
    if (brightness_ == target_brightness_) {
        StopTransitionTimer(transition_timer_);
        return;
    }

    step_ = (target_brightness_ > brightness_) ? 1 : -1;

    if (transition_timer_ != nullptr) {
        StopTransitionTimer(transition_timer_);
        ESP_ERROR_CHECK(esp_timer_start_periodic(transition_timer_, 5 * 1000));
    }
}

void Backlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        StopTransitionTimer(transition_timer_);
        return;
    }

    brightness_ += step_;
    SetBrightnessImpl(brightness_);

    if (brightness_ == target_brightness_) {
        StopTransitionTimer(transition_timer_);
    }
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz) : Backlight() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz, 
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));

    
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert,
        }
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::~PwmBacklight() {
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    
    uint32_t duty_cycle = (1023 * brightness) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
