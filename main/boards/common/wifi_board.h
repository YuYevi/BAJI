#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    esp_timer_handle_t wifi_config_countdown_timer_ = nullptr;
    bool in_config_mode_ = false;
    bool manual_wifi_config_mode_ = false;
    bool suppress_config_exit_reconnect_ = false;
    bool wifi_scan_notified_ = false;
    int wifi_config_countdown_seconds_ = 0;
    NetworkEventCallback network_event_callback_ = nullptr;
    bool wifi_auto_reconnect_enabled_ = true;

    virtual std::string GetBoardJson() override;

    
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    
    void TryWifiConnect();

    
    void StartWifiConfigMode();

    void StartWifiConfigCountdown();
    void StopWifiConfigCountdown();
    void UpdateWifiConfigCountdownNotification() const;
    void OnWifiConfigCountdownTick();

    void SetWifiAutoReconnectEnabled(bool enabled);

    
    static void OnWifiConnectTimeout(void* arg);
    static void OnWifiConfigCountdown(void* arg);

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
