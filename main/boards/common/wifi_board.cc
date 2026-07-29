#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_network.h>
#include <new>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static constexpr int CONNECT_TIMEOUT_SEC = 8;
static constexpr int BLUFI_AUDIO_STOP_TIMEOUT_MS = 5000;
static const char* TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    Blufi::GetInstance().SetProvisioningDoneCallback(nullptr);
    Blufi::GetInstance().deinit();
#endif
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

bool WifiBoard::EnsureWifiManagerInitialized() {
    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager_initialized_.load() && wifi_manager.IsInitialized()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(wifi_manager_init_mutex_);
    if (wifi_manager.IsInitialized()) {
        wifi_manager_initialized_.store(true);
        return true;
    }

    WifiManagerConfig config;
    config.ssid_prefix = "Baji";
    config.language = Lang::CODE;
    config.station_scan_min_interval_seconds = 3;
    config.station_scan_max_interval_seconds = 30;
    if (!wifi_manager.Initialize(config)) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi manager");
        return false;
    }

    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
            }
        });

    wifi_manager_initialized_.store(true);
    return true;
}

bool WifiBoard::DeinitializeWifiManager() {
    std::lock_guard<std::mutex> lock(wifi_manager_init_mutex_);
    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized()) {
        wifi_manager_initialized_.store(false);
        return true;
    }
    if (!wifi_manager.Deinitialize()) {
        return false;
    }
    wifi_manager_initialized_.store(false);
    return true;
}

void WifiBoard::StartNetwork() {
    if (!EnsureWifiManagerInitialized()) {
        return;
    }

    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        wifi_scan_notified_ = false;
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        suppress_config_exit_reconnect_.store(IsInWifiConfigMode());
        WifiManager::GetInstance().StartStation();
        suppress_config_exit_reconnect_.store(false);
    } else {
        wifi_scan_notified_ = false;
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            
            esp_timer_stop(connect_timer_);
            ClearManualWifiConfigMode();
            wifi_scan_notified_ = false;
            in_config_mode_.store(false);
            ClearWifiConfigNotifications();
            
            break;
        case NetworkEvent::Scanning:
            if (wifi_scan_notified_) {
                return;
            }
            wifi_scan_notified_ = true;
            break;
        case NetworkEvent::Connecting:
            
            break;
        case NetworkEvent::Disconnected:
            
            break;
        case NetworkEvent::WifiConfigModeEnter:
            wifi_scan_notified_ = false;
            in_config_mode_.store(true);
            break;
        case NetworkEvent::WifiConfigModeExit:
            
            ClearManualWifiConfigMode();
            wifi_scan_notified_ = false;
            in_config_mode_.store(false);
            ClearWifiConfigNotifications();
            
            if (suppress_config_exit_reconnect_.exchange(false)) {
                break;
            }
            if (wifi_auto_reconnect_enabled_.load()) {
                TryWifiConnect();
            }
            break;
        default:
            break;
    }

    
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::SetWifiAutoReconnectEnabled(bool enabled) {
    wifi_auto_reconnect_enabled_.store(enabled);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    Application::GetInstance().Schedule([board]() {
        auto& wifi_manager = WifiManager::GetInstance();

        if (wifi_manager.IsConnected()) {
            board->ClearManualWifiConfigMode();
            board->wifi_scan_notified_ = false;
            board->in_config_mode_.store(false);
            board->ClearWifiConfigNotifications();
            return;
        }

        wifi_manager.StopStation();
        board->StartWifiConfigMode();
    });
}

void WifiBoard::ClearManualWifiConfigMode() {
    std::lock_guard<std::mutex> lock(wifi_config_lifecycle_mutex_);
    manual_wifi_config_mode_.store(false);
    wifi_config_generation_.fetch_add(1);
}

uint32_t WifiBoard::BeginWifiConfigSession(bool manual) {
    std::lock_guard<std::mutex> lock(wifi_config_lifecycle_mutex_);
    if (manual) {
        manual_wifi_config_mode_.store(true);
    }
    return wifi_config_generation_.fetch_add(1) + 1;
}

bool WifiBoard::IsWifiConfigSessionCurrent(uint32_t generation) const {
    return generation != 0 && wifi_config_generation_.load() == generation;
}

void WifiBoard::CancelWifiConfigSessionIfCurrent(uint32_t generation) {
    std::lock_guard<std::mutex> lock(wifi_config_lifecycle_mutex_);
    uint32_t expected = generation;
    if (generation != 0 &&
        wifi_config_generation_.compare_exchange_strong(expected, generation + 1)) {
        manual_wifi_config_mode_.store(false);
    }
}

void WifiBoard::ClearWifiConfigNotifications() {
    std::lock_guard<std::mutex> lock(wifi_config_lifecycle_mutex_);
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->ShowPersistentNotification("", true);
        display->ShowPersistentNotification("", false);
    }
}

void WifiBoard::ScheduleWifiConfigNotification(uint32_t generation) {
    Application::GetInstance().Schedule([this, generation]() {
        std::lock_guard<std::mutex> lock(wifi_config_lifecycle_mutex_);
        auto& wifi_manager = WifiManager::GetInstance();
        if (!IsWifiConfigSessionCurrent(generation) || !wifi_manager.IsConfigMode()) {
            return;
        }

        auto ap_ssid = wifi_manager.GetApSsid();
        auto ap_url = wifi_manager.GetApWebUrl();
        if (ap_ssid.empty() || ap_url.empty() ||
            !IsWifiConfigSessionCurrent(generation) || !wifi_manager.IsConfigMode()) {
            return;
        }

        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += ap_ssid;
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += ap_url;

        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowPersistentNotification(Lang::Strings::WIFI_CONFIG_MODE, true);
            // Keep the hotspot info visible on any smartwatch screen instead of
            // only inside the AI chat page's message area.
            display->ShowPersistentNotification(hint.c_str(), false);
        }
        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(),
                                         "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
}

void WifiBoard::StartWifiConfigMode(uint32_t expected_generation) {
    auto& wifi_manager = WifiManager::GetInstance();
    if (expected_generation != 0 && !IsWifiConfigSessionCurrent(expected_generation)) {
        return;
    }
    if (!EnsureWifiManagerInitialized()) {
        in_config_mode_.store(false);
        CancelWifiConfigSessionIfCurrent(expected_generation);
        suppress_config_exit_reconnect_.store(true);
        OnNetworkEvent(NetworkEvent::WifiConfigModeExit, "stopped");
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
        return;
    }
    if (IsInWifiConfigMode()) {
        return;
    }
    if (!manual_wifi_config_mode_.load() && wifi_manager.IsConnected()) {
        return;
    }

    const uint32_t generation = expected_generation != 0
        ? expected_generation
        : BeginWifiConfigSession();
    if (!IsWifiConfigSessionCurrent(generation)) {
        return;
    }

    bool expected_inactive = false;
    if (!in_config_mode_.compare_exchange_strong(expected_inactive, true)) {
        return;
    }
    wifi_scan_notified_ = false;

    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    wifi_manager.StartConfigAp();
    if (!wifi_manager.IsConfigMode()) {
        in_config_mode_.store(false);
        CancelWifiConfigSessionIfCurrent(generation);
        ClearWifiConfigNotifications();
        return;
    }
    ScheduleWifiConfigNotification(generation);
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    esp_err_t ret = SuspendAudioForBlufi() ? ESP_OK : ESP_ERR_TIMEOUT;
    bool canceled = false;
    if (ret == ESP_OK) {
        std::lock_guard<std::mutex> stack_lock(blufi_stack_lifecycle_mutex_);
        if (!IsWifiConfigSessionCurrent(generation) || !in_config_mode_.load()) {
            canceled = true;
        } else {
            blufi.SetProvisioningDoneCallback([this, generation]() {
                Application::GetInstance().Schedule([this, generation]() {
                    if (!IsWifiConfigSessionCurrent(generation)) {
                        ResumeAudioAfterBlufi();
                        return;
                    }

                    in_config_mode_.store(false);
                    ClearWifiConfigNotifications();
                    OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                    ResumeAudioAfterBlufi();
                });
            });

            ret = blufi.init();
            if (!IsWifiConfigSessionCurrent(generation) || !in_config_mode_.load()) {
                if (ret == ESP_OK) {
                    blufi.deinit();
                }
                canceled = true;
            } else if (ret == ESP_OK) {
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
            }
        }
    }

    if (canceled) {
        Application::GetInstance().Schedule([this]() {
            ResumeAudioAfterBlufi();
        });
        return;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLUFI: %s", esp_err_to_name(ret));
        const bool reconnect_saved_wifi =
            !SsidManager::GetInstance().GetSsidList().empty() &&
            wifi_auto_reconnect_enabled_.load();
        in_config_mode_.store(false);
        CancelWifiConfigSessionIfCurrent(generation);
        ClearWifiConfigNotifications();
        suppress_config_exit_reconnect_.store(!reconnect_saved_wifi);
        OnNetworkEvent(NetworkEvent::WifiConfigModeExit,
                       reconnect_saved_wifi ? "reconnect" : "stopped");
        Application::GetInstance().Schedule([this, ret, reconnect_saved_wifi]() {
            auto& app = Application::GetInstance();
            if (!reconnect_saved_wifi &&
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.SetDeviceState(kDeviceStateIdle);
            }
            ResumeAudioAfterBlufi();
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                std::string message = Lang::Strings::BLUFI_INIT_FAILED;
                message += ": ";
                message += esp_err_to_name(ret);
                display->ShowNotification(message.c_str(), 4000);
            }
        });
        return;
    }

    std::string device_name = blufi.GetDeviceName();
    Application::GetInstance().Schedule([this, generation, device_name = std::move(device_name)]() {
        if (!IsWifiConfigSessionCurrent(generation) || !Blufi::GetInstance().IsActive()) {
            return;
        }
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            std::string hint = Lang::Strings::CONNECT_WITH_BLUFI;
            const auto placeholder = hint.find("%s");
            if (placeholder != std::string::npos) {
                hint.replace(placeholder, 2, device_name);
            } else {
                // Keep the notification usable if a custom language omits the placeholder.
                hint += device_name;
            }
            display->ShowPersistentNotification(Lang::Strings::WIFI_CONFIG_MODE, true);
            display->ShowPersistentNotification(hint.c_str(), false);
            Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(),
                                             "bluetooth");
        }
    });
#else
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

bool WifiBoard::SuspendAudioForBlufi() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (!blufi_audio_suspended_.exchange(true)) {
        audio_service.Stop();
    }
    if (!audio_service.WaitForStopped(BLUFI_AUDIO_STOP_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Timed out waiting for audio tasks to stop before BLUFI");
        return false;
    }
#endif
    return true;
}

void WifiBoard::ResumeAudioAfterBlufi() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    if (in_config_mode_.load() || Blufi::GetInstance().IsActive()) {
        return;
    }
    if (!blufi_audio_suspended_.load()) {
        return;
    }
    auto& app = Application::GetInstance();
    auto& audio_service = app.GetAudioService();
    if (!audio_service.WaitForStopped(BLUFI_AUDIO_STOP_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "Timed out waiting for audio tasks before BLUFI recovery");
        return;
    }
    if (!blufi_audio_suspended_.exchange(false)) {
        return;
    }
    audio_service.Start();
    app.RefreshWakeWordDetection();
#endif
}

void WifiBoard::StopWifiConfigMode(bool reconnect) {
    bool was_active = manual_wifi_config_mode_.load() || in_config_mode_.exchange(false) ||
                      WifiManager::GetInstance().IsConfigMode();
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    was_active = was_active || Blufi::GetInstance().IsActive() ||
                 blufi_audio_suspended_.load();
#endif
    if (!was_active) {
        return;
    }

    ClearManualWifiConfigMode();
    ClearWifiConfigNotifications();
    suppress_config_exit_reconnect_.store(!reconnect);

#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager.IsConfigMode()) {
        wifi_manager.StopConfigAp();
        return;
    }
    OnNetworkEvent(NetworkEvent::WifiConfigModeExit, reconnect ? "reconnect" : "stopped");
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    esp_err_t ret = ESP_OK;
    {
        std::lock_guard<std::mutex> stack_lock(blufi_stack_lifecycle_mutex_);
        ret = Blufi::GetInstance().deinit();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop BLUFI: %s", esp_err_to_name(ret));
    }
    OnNetworkEvent(NetworkEvent::WifiConfigModeExit, reconnect ? "reconnect" : "stopped");
    Application::GetInstance().Schedule([this, reconnect]() {
        auto& app = Application::GetInstance();
        if (!reconnect && app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            app.SetDeviceState(kDeviceStateIdle);
        }
        ResumeAudioAfterBlufi();
    });
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    // If another path already opened config mode, take ownership so the next
    // triple-click can close it.
    if (IsInWifiConfigMode()) {
        manual_wifi_config_mode_.store(true);
        in_config_mode_.store(true);
        app.SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const uint32_t generation = BeginWifiConfigSession(true);

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        
        Application::GetInstance().ResetProtocol();

        struct WifiConfigDelayContext {
            WifiBoard* board;
            uint32_t generation;
        };
        auto* context = new (std::nothrow) WifiConfigDelayContext{this, generation};

        if (context == nullptr || xTaskCreate([](void* arg) {
            auto* context = static_cast<WifiConfigDelayContext*>(arg);
            auto* board = context->board;
            const uint32_t generation = context->generation;
            delete context;

            
            vTaskDelay(pdMS_TO_TICKS(1000));

            if (!board->IsWifiConfigSessionCurrent(generation)) {
                vTaskDelete(NULL);
                return;
            }

            
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            if (!board->IsWifiConfigSessionCurrent(generation)) {
                vTaskDelete(NULL);
                return;
            }

            
            board->StartWifiConfigMode(generation);

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, context, 2, NULL) != pdPASS) {
            delete context;
            CancelWifiConfigSessionIfCurrent(generation);
            const bool have_saved_wifi = !SsidManager::GetInstance().GetSsidList().empty();
            suppress_config_exit_reconnect_.store(!have_saved_wifi);
            OnNetworkEvent(NetworkEvent::WifiConfigModeExit,
                           have_saved_wifi ? "reconnect" : "stopped");
            if (!have_saved_wifi && app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.SetDeviceState(kDeviceStateIdle);
            }
        }
        return;
    }

    if (state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring) {
        ClearManualWifiConfigMode();
        return;
    }

    
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode(generation);
}

bool WifiBoard::ExitManualWifiConfigMode() {
    if (!manual_wifi_config_mode_.load()) {
        return false;
    }

    const bool have_saved_wifi = !SsidManager::GetInstance().GetSsidList().empty();
    if (IsInWifiConfigMode()) {
        StopWifiConfigMode(have_saved_wifi);
    } else {
        ClearManualWifiConfigMode();
        ClearWifiConfigNotifications();
        suppress_config_exit_reconnect_.store(!have_saved_wifi);
        OnNetworkEvent(NetworkEvent::WifiConfigModeExit,
                       have_saved_wifi ? "reconnect" : "stopped");
        Application::GetInstance().Schedule([this, have_saved_wifi]() {
            auto& app = Application::GetInstance();
            if (!have_saved_wifi && app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.SetDeviceState(kDeviceStateIdle);
            }
            ResumeAudioAfterBlufi();
        });
    }
    return true;
}

bool WifiBoard::IsInWifiConfigMode() const {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    return in_config_mode_.load() || Blufi::GetInstance().IsActive();
#else
    return in_config_mode_.load() || WifiManager::GetInstance().IsConfigMode();
#endif
}

bool WifiBoard::IsManualWifiConfigMode() const {
    return manual_wifi_config_mode_.load();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI_SLASH;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!IsInWifiConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
