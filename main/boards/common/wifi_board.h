#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    std::atomic_bool in_config_mode_{false};
    std::atomic_bool manual_wifi_config_mode_{false};
    // Invalidates delayed start and notification work from older config sessions.
    std::atomic<uint32_t> wifi_config_generation_{0};
    // Orders session changes with persistent-notification show/clear operations.
    std::mutex wifi_config_lifecycle_mutex_;
    std::atomic_bool suppress_config_exit_reconnect_{false};
    bool wifi_scan_notified_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;
    std::atomic_bool wifi_auto_reconnect_enabled_{true};
    std::atomic_bool blufi_audio_suspended_{false};
    std::atomic_bool wifi_manager_initialized_{false};
    std::mutex wifi_manager_init_mutex_;
    std::mutex blufi_stack_lifecycle_mutex_;

    virtual std::string GetBoardJson() override;

    
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    bool EnsureWifiManagerInitialized();

    bool DeinitializeWifiManager();

    
    void TryWifiConnect();

    
    void StartWifiConfigMode(uint32_t expected_generation = 0);

    void StopWifiConfigMode(bool reconnect);

    bool SuspendAudioForBlufi();

    void ResumeAudioAfterBlufi();

    void ClearManualWifiConfigMode();

    uint32_t BeginWifiConfigSession(bool manual = false);

    bool IsWifiConfigSessionCurrent(uint32_t generation) const;

    void CancelWifiConfigSessionIfCurrent(uint32_t generation);

    void ScheduleWifiConfigNotification(uint32_t generation);

    void ClearWifiConfigNotifications();

    void SetWifiAutoReconnectEnabled(bool enabled);

    
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    
    void EnterWifiConfigMode();
    bool ExitManualWifiConfigMode();
    
    
    bool IsInWifiConfigMode() const;
    bool IsManualWifiConfigMode() const;
};

#endif 
