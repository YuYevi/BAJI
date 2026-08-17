#ifndef APP_COMMON_H
#define APP_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    lv_obj_t * cont;
    lv_obj_t * wifi;
    lv_obj_t * charging;
    lv_obj_t * battery;
    lv_obj_t * battery_pct;
} app_status_bar_t;

void app_status_bar_init(app_status_bar_t * bar, lv_obj_t * parent);
void app_status_bar_deinit(app_status_bar_t * bar);

void app_status_set_network_icon(const char * icon);
void app_status_set_battery(uint8_t percent, bool charging);

void app_status_overlay_init(void);
void app_status_overlay_deinit(void);

bool app_touch_event_is_drag_release(lv_event_t * e);

void app_screen_enable_swipe_back(lv_obj_t * screen);
void app_screen_set_swipe_back_enabled(lv_obj_t * screen, bool enabled);
void app_swipe_back_refresh_subtree(lv_obj_t * root);

uint8_t app_device_get_brightness(void);
void app_device_set_brightness(uint8_t brightness, bool permanent);
uint8_t app_device_get_volume(void);
void app_device_set_volume(uint8_t volume, bool permanent);
bool app_device_get_network_mode_is_4g(void);
bool app_device_switch_network_mode(bool use_4g);

enum {
    APP_DEVICE_NETWORK_WIFI = 0,
    APP_DEVICE_NETWORK_4G = 1,
    APP_DEVICE_NETWORK_UNSUPPORTED = 255,
};

enum {
    APP_DEVICE_NETWORK_PHASE_OFFLINE = 0,
    APP_DEVICE_NETWORK_PHASE_CONNECTING = 1,
    APP_DEVICE_NETWORK_PHASE_ONLINE = 2,
    APP_DEVICE_NETWORK_PHASE_SWITCHING = 3,
    APP_DEVICE_NETWORK_PHASE_PROVISIONING = 4,
    APP_DEVICE_NETWORK_PHASE_FAILED = 5,
};

typedef struct {
    uint8_t active_mode;
    uint8_t target_mode;
    uint8_t phase;
    bool link_up;
    uint32_t generation;
} app_device_network_status_t;

bool app_device_get_network_status(app_device_network_status_t *status);
bool app_device_get_auto_power_save_enabled(void);
bool app_device_set_auto_power_save_enabled(bool enabled);
int32_t app_device_get_alarm_count(void);
bool app_device_get_alarm_item(uint8_t idx, uint8_t * hour, uint8_t * minute, bool * enabled);
void app_device_set_alarm_item(uint8_t idx, uint8_t hour, uint8_t minute, bool enabled);
void app_device_set_alarm_count(uint8_t count);
void app_device_reboot(void);


#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
