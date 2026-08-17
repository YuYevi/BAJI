#include "app_common.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "settings.h"

static uint8_t clamp_percent(int32_t value, uint8_t fallback)
{
    if (value < 0) {
        return fallback;
    }
    if (value > 100) {
        return 100;
    }
    return static_cast<uint8_t>(value);
}

static std::string alarm_key(const char * prefix, uint8_t idx)
{
    return std::string(prefix) + std::to_string(static_cast<int>(idx));
}

extern "C" uint8_t app_device_get_brightness(void)
{
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr && backlight->brightness() > 0) {
        return clamp_percent(backlight->brightness(), 100);
    }

    Settings settings("display");
    return clamp_percent(settings.GetInt("brightness", 100), 100);
}

extern "C" void app_device_set_brightness(uint8_t brightness, bool permanent)
{
    if (brightness == 0) {
        brightness = 1;
    } else if (brightness > 100) {
        brightness = 100;
    }

    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(brightness, permanent);
        return;
    }

    if (permanent) {
        Settings settings("display", true);
        settings.SetInt("brightness", brightness);
    }
}

extern "C" uint8_t app_device_get_volume(void)
{
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        return clamp_percent(codec->output_volume(), 100);
    }

    Settings settings("audio");
    return clamp_percent(settings.GetInt("output_volume", 100), 100);
}

extern "C" void app_device_set_volume(uint8_t volume, bool permanent)
{
    if (volume > 100) {
        volume = 100;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(volume, permanent);
        return;
    }

    if (permanent) {
        Settings settings("audio", true);
        settings.SetInt("output_volume", volume);
    }
}

extern "C" bool app_device_get_network_mode_is_4g(void)
{
    auto& board = Board::GetInstance();
    BoardNetworkMode mode = board.GetActiveNetworkMode();
    if (mode != BoardNetworkMode::UNSUPPORTED) {
        return mode == BoardNetworkMode::CELLULAR;
    }

    Settings settings("network");
    return settings.GetInt("type", 1) == 1;
}

extern "C" bool app_device_switch_network_mode(bool use_4g)
{
    auto& board = Board::GetInstance();
    BoardNetworkMode target = use_4g ? BoardNetworkMode::CELLULAR : BoardNetworkMode::WIFI;
    if (board.SwitchActiveNetworkMode(target)) {
        return true;
    }

    // A runtime-capable board may be offline while still owning a target
    // mode.  Do not overwrite that request with the boot preference.
    const BoardNetworkStatus status = board.GetNetworkStatus();
    const bool transition_in_flight =
        status.phase == BoardNetworkPhase::CONNECTING ||
        status.phase == BoardNetworkPhase::SWITCHING ||
        status.phase == BoardNetworkPhase::PROVISIONING;
    if ((status.phase == BoardNetworkPhase::ONLINE && status.link_up &&
         status.active_mode == target) ||
        (transition_in_flight && status.target_mode == target)) {
        return true;
    }
    if (status.target_mode != BoardNetworkMode::UNSUPPORTED ||
        status.phase != BoardNetworkPhase::OFFLINE) {
        return false;
    }

    if (board.GetActiveNetworkMode() != BoardNetworkMode::UNSUPPORTED) {
        return board.SwitchActiveNetworkMode(target);
    }

    Settings settings("network", true);
    settings.SetInt("type", use_4g ? 1 : 0);
    return false;
}

extern "C" bool app_device_get_network_status(app_device_network_status_t *status)
{
    if (status == nullptr) {
        return false;
    }

    const BoardNetworkStatus snapshot = Board::GetInstance().GetNetworkStatus();
    switch (snapshot.active_mode) {
        case BoardNetworkMode::WIFI:
            status->active_mode = APP_DEVICE_NETWORK_WIFI;
            break;
        case BoardNetworkMode::CELLULAR:
            status->active_mode = APP_DEVICE_NETWORK_4G;
            break;
        case BoardNetworkMode::UNSUPPORTED:
        default:
            status->active_mode = APP_DEVICE_NETWORK_UNSUPPORTED;
            break;
    }

    switch (snapshot.target_mode) {
        case BoardNetworkMode::WIFI:
            status->target_mode = APP_DEVICE_NETWORK_WIFI;
            break;
        case BoardNetworkMode::CELLULAR:
            status->target_mode = APP_DEVICE_NETWORK_4G;
            break;
        case BoardNetworkMode::UNSUPPORTED:
        default:
            status->target_mode = APP_DEVICE_NETWORK_UNSUPPORTED;
            break;
    }

    status->phase = static_cast<uint8_t>(snapshot.phase);
    status->link_up = snapshot.link_up;
    status->generation = snapshot.generation;
    return true;
}

extern "C" bool app_device_get_auto_power_save_enabled(void)
{
    return Board::GetInstance().GetAutoPowerSaveEnabled();
}

extern "C" bool app_device_set_auto_power_save_enabled(bool enabled)
{
    return Board::GetInstance().SetAutoPowerSaveEnabled(enabled);
}

extern "C" int32_t app_device_get_alarm_count(void)
{
    Settings settings("alarm");
    return settings.GetInt("count", -1);
}

extern "C" bool app_device_get_alarm_item(uint8_t idx, uint8_t * hour, uint8_t * minute, bool * enabled)
{
    if (hour == nullptr || minute == nullptr || enabled == nullptr) {
        return false;
    }

    Settings settings("alarm");
    int32_t count = settings.GetInt("count", -1);
    if (count < 0 || idx >= static_cast<uint8_t>(count)) {
        return false;
    }

    *hour = clamp_percent(settings.GetInt(alarm_key("h", idx), 0), 0);
    if (*hour > 23) {
        *hour = 0;
    }

    *minute = clamp_percent(settings.GetInt(alarm_key("m", idx), 0), 0);
    if (*minute > 59) {
        *minute = 0;
    }

    *enabled = settings.GetBool(alarm_key("on", idx), false);
    return true;
}

extern "C" void app_device_set_alarm_item(uint8_t idx, uint8_t hour, uint8_t minute, bool enabled)
{
    Settings settings("alarm", true);
    settings.SetInt(alarm_key("h", idx), hour > 23 ? 0 : hour);
    settings.SetInt(alarm_key("m", idx), minute > 59 ? 0 : minute);
    settings.SetBool(alarm_key("on", idx), enabled);
}

extern "C" void app_device_set_alarm_count(uint8_t count)
{
    Settings settings("alarm", true);
    settings.SetInt("count", count);
}

extern "C" void app_device_reboot(void)
{
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().Reboot();
    });
}
