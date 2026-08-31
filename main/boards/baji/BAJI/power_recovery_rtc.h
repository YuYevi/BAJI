#pragma once

#include <esp_system.h>

enum class BajiPowerRecoveryAction {
    NormalBoot,
    KeepPower,
    PowerOff,
};

BajiPowerRecoveryAction baji_power_recovery_action(esp_reset_reason_t reason);
bool baji_power_is_programmer_reset(esp_reset_reason_t reason);
bool baji_power_is_resume_reset(esp_reset_reason_t reason);
bool baji_power_recovery_is_power_off_pending();
void baji_power_recovery_request_power_off();
void baji_power_recovery_allow_boot();
void baji_power_recovery_mark_boot_validated();
bool baji_power_recovery_consume_boot_validated();
void baji_power_recovery_mark_stable();
