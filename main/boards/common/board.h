#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <cstdint>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"


enum class NetworkEvent {
    Scanning,              
    Connecting,            
    Connected,             
    Disconnected,          
    WifiConfigModeEnter,   
    WifiConfigModeExit,    
    
    ModemDetecting,        
    ModemErrorNoSim,       
    ModemErrorRegDenied,   
    ModemErrorInitFailed,  
    ModemErrorTimeout      
};


enum class PowerSaveLevel {
    LOW_POWER,    
    BALANCED,     
    PERFORMANCE,  
};

enum class BoardNetworkMode {
    WIFI,
    CELLULAR,
    UNSUPPORTED,
};

enum class BleSetupMode : uint8_t {
    BIND_ONLY = 0x00,
    WIFI_PROVISION_AND_BIND = 0x01,
};



using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; 
    Board& operator=(const Board&) = delete; 

protected:
    Board();
    std::string GenerateUuid();

    
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual std::string GetDeviceRole();
    virtual std::string GetDeviceNetworkVersion();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
    virtual BoardNetworkMode GetActiveNetworkMode() { return BoardNetworkMode::UNSUPPORTED; }
    virtual bool SwitchActiveNetworkMode(BoardNetworkMode mode) { (void)mode; return false; }
    virtual bool EnterBleBindMode(BleSetupMode setup_mode = BleSetupMode::BIND_ONLY) {
        (void)setup_mode;
        return false;
    }
    virtual void ExitBleBindMode() {}
    virtual bool IsBleBindModeActive() const { return false; }
    virtual bool IsWifiConfigModeActive() const { return false; }
    virtual bool GetAutoPowerSaveEnabled();
    virtual bool SetAutoPowerSaveEnabled(bool enabled);
    virtual void OnApplicationReady() {}
    virtual void OnApplicationClockTick() {}
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif 
