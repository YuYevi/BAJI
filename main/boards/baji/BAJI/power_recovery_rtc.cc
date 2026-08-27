#include "power_recovery_rtc.h"

#include "config.h"

#include <cstdint>
#include <esp_attr.h>

namespace {

constexpr uint32_t kRecoveryMagic = 0x42505243u;

RTC_NOINIT_ATTR volatile uint32_t s_recovery_magic;
RTC_NOINIT_ATTR volatile uint32_t s_consecutive_fault_resets;
RTC_NOINIT_ATTR volatile uint32_t s_power_off_pending;

bool IsFaultReset(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
        case ESP_RST_PWR_GLITCH:
        case ESP_RST_CPU_LOCKUP:
            return true;
        default:
            return false;
    }
}

bool IsProgrammerReset(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_USB:
        case ESP_RST_JTAG:
        case ESP_RST_EXT:
            return true;
        default:
            return false;
    }
}

void ClearRecoveryState() {
    // Treat magic as the commit marker so an interrupted clear is retried.
    s_recovery_magic = 0;
    s_consecutive_fault_resets = 0;
    s_power_off_pending = 0;
    s_recovery_magic = kRecoveryMagic;
}

void EnsureRecoveryState() {
    if (s_recovery_magic != kRecoveryMagic) {
        ClearRecoveryState();
    }
}

}  // namespace

BajiPowerRecoveryAction baji_power_recovery_action(esp_reset_reason_t reason) {
    EnsureRecoveryState();

    // USB/JTAG flashing and external reset-control tools must be allowed to
    // recover a board that was previously put into the power-off state.
    if (IsProgrammerReset(reason)) {
        ClearRecoveryState();
        return BajiPowerRecoveryAction::KeepPower;
    }

    if (s_power_off_pending != 0) {
        if (reason == ESP_RST_POWERON) {
            ClearRecoveryState();
            return BajiPowerRecoveryAction::NormalBoot;
        }
        if (reason == ESP_RST_DEEPSLEEP) {
            // The boot gate must validate the wake source and full key hold
            // before it clears the pending power-off state.
            return BajiPowerRecoveryAction::NormalBoot;
        }
        return BajiPowerRecoveryAction::PowerOff;
    }

    if (reason == ESP_RST_SW) {
        ClearRecoveryState();
        return BajiPowerRecoveryAction::KeepPower;
    }

    if (!IsFaultReset(reason)) {
        ClearRecoveryState();
        return BajiPowerRecoveryAction::NormalBoot;
    }

    if (s_consecutive_fault_resets < UINT32_MAX) {
        s_consecutive_fault_resets = s_consecutive_fault_resets + 1;
    }

    return s_consecutive_fault_resets <= POWER_RECOVERY_MAX_CONSECUTIVE_RESETS
               ? BajiPowerRecoveryAction::KeepPower
               : BajiPowerRecoveryAction::PowerOff;
}

bool baji_power_is_programmer_reset(esp_reset_reason_t reason) {
    return IsProgrammerReset(reason);
}

bool baji_power_is_resume_reset(esp_reset_reason_t reason) {
    return reason == ESP_RST_SW || IsFaultReset(reason) || IsProgrammerReset(reason);
}

bool baji_power_recovery_is_power_off_pending() {
    EnsureRecoveryState();
    return s_power_off_pending != 0;
}

void baji_power_recovery_request_power_off() {
    EnsureRecoveryState();
    s_power_off_pending = 1;
}

void baji_power_recovery_allow_boot() {
    ClearRecoveryState();
}

void baji_power_recovery_mark_stable() {
    EnsureRecoveryState();
    // Never clear a concurrent/earlier shutdown request when the stable-uptime
    // clock tick arrives.
    s_consecutive_fault_resets = 0;
}
