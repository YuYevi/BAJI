#include "charging_boot_rtc.h"
#include <cstdint>

#include <esp_attr.h>

#define CHARGING_RTC_MAGIC 0x43485247u
#define FLAG_USB_SHUTDOWN_NEXT 0x01u

RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_flags;

extern "C" void charging_rtc_set_usb_shutdown_flag(void)
{
    s_rtc_magic = CHARGING_RTC_MAGIC;
    s_rtc_flags |= FLAG_USB_SHUTDOWN_NEXT;
}

extern "C" void charging_rtc_clear_boot_flags(void)
{
    s_rtc_magic = 0;
    s_rtc_flags = 0;
}

extern "C" bool charging_rtc_usb_shutdown_next_boot(void)
{
    return (s_rtc_magic == CHARGING_RTC_MAGIC) && (s_rtc_flags & FLAG_USB_SHUTDOWN_NEXT);
}
