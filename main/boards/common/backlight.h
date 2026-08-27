#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include <driver/gpio.h>
#include <esp_timer.h>


class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    // Apply a brightness immediately and cancel any in-flight fade.
    void SetBrightnessImmediate(uint8_t brightness, bool permanent = false);
    // Limit the effective brightness without losing the caller's requested
    // value.  Clearing the limit restores the most recent requested value.
    void SetBrightnessLimit(uint8_t max_brightness, bool immediate = false);
    // Return the caller's requested level, excluding a temporary safety cap.
    uint8_t requested_brightness() const;
    uint8_t brightness() const;

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    mutable std::mutex mutex_;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t requested_brightness_ = 0;
    uint8_t brightness_limit_ = 100;
    int8_t step_ = 1;

    void ApplyTargetBrightnessLocked(uint8_t target_brightness, bool immediate);
};


class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
