#include "dual_network_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
DualNetworkBoard::DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin, int32_t default_net_type) 
    : Board(), 
      ml307_tx_pin_(ml307_tx_pin), 
      ml307_rx_pin_(ml307_rx_pin), 
      ml307_dtr_pin_(ml307_dtr_pin) {
    
    
    network_type_ = LoadNetworkTypeFromSettings(default_net_type);
    
    
    InitializeCurrentBoard();
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    Settings settings("network", true);
    int network_type = settings.GetInt("type", default_net_type); 
    return network_type == 1 ? NetworkType::ML307 : NetworkType::WIFI;
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::ML307) ? 1 : 0;
    settings.SetInt("type", network_type);
}

void DualNetworkBoard::InitializeCurrentBoard() {
    if (network_type_ == NetworkType::ML307) {
        
        current_board_ = std::make_unique<Ml307Board>(ml307_tx_pin_, ml307_rx_pin_, ml307_dtr_pin_);
    } else {
        
        current_board_ = std::make_unique<WifiBoard>();
    }
}

void DualNetworkBoard::SwitchNetworkType() {
    SetNetworkType(network_type_ == NetworkType::WIFI ? NetworkType::ML307 : NetworkType::WIFI);
}

void DualNetworkBoard::SetNetworkType(NetworkType type) {
    auto display = GetDisplay();
    SaveNetworkTypeToSettings(type);
    network_type_ = type;
    if (display != nullptr) {
        display->ShowNotification(type == NetworkType::ML307
            ? Lang::Strings::SWITCH_TO_4G_NETWORK
            : Lang::Strings::SWITCH_TO_WIFI_NETWORK);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    auto& app = Application::GetInstance();
    app.Reboot();
}

 
std::string DualNetworkBoard::GetBoardType() {
    return current_board_->GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    auto display = Board::GetInstance().GetDisplay();
    
    if (network_type_ == NetworkType::WIFI) {
        display->SetStatus(Lang::Strings::CONNECTING);
    } else {
        display->SetStatus(Lang::Strings::DETECTING_MODULE);
    }
    current_board_->StartNetwork();
}

void DualNetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    
    current_board_->SetNetworkEventCallback(std::move(callback));
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return current_board_->GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return current_board_->GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    current_board_->SetPowerSaveLevel(level);
}

std::string DualNetworkBoard::GetBoardJson() {   
    return current_board_->GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return current_board_->GetDeviceStatusJson();
}

BoardNetworkMode DualNetworkBoard::GetActiveNetworkMode() {
    return network_type_ == NetworkType::ML307 ? BoardNetworkMode::CELLULAR : BoardNetworkMode::WIFI;
}

bool DualNetworkBoard::SwitchActiveNetworkMode(BoardNetworkMode mode) {
    if (mode == BoardNetworkMode::UNSUPPORTED) {
        return false;
    }

    NetworkType target = (mode == BoardNetworkMode::CELLULAR) ? NetworkType::ML307 : NetworkType::WIFI;
    if (target == network_type_) {
        return true;
    }

    SetNetworkType(target);
    return true;
}

bool DualNetworkBoard::EnterBleBindMode() {
    if (network_type_ == NetworkType::WIFI && current_board_ != nullptr) {
        return current_board_->EnterBleBindMode();
    }
    return Board::EnterBleBindMode();
}

void DualNetworkBoard::ExitBleBindMode() {
    if (network_type_ == NetworkType::WIFI && current_board_ != nullptr) {
        current_board_->ExitBleBindMode();
        return;
    }
    Board::ExitBleBindMode();
}

bool DualNetworkBoard::IsBleBindModeActive() const {
    if (network_type_ == NetworkType::WIFI && current_board_ != nullptr) {
        return current_board_->IsBleBindModeActive();
    }
    return Board::IsBleBindModeActive();
}

uint32_t DualNetworkBoard::GetBleBindNonce() const {
    if (network_type_ == NetworkType::WIFI && current_board_ != nullptr) {
        return current_board_->GetBleBindNonce();
    }
    return Board::GetBleBindNonce();
}
