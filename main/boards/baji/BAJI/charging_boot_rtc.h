#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


void charging_rtc_set_usb_shutdown_flag(void);
void charging_rtc_clear_boot_flags(void);
bool charging_rtc_usb_shutdown_next_boot(void);

#ifdef __cplusplus
}
#endif
