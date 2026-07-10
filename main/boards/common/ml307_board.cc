#include "ml307_board.h"

#include "audio_codec.h"
#include "display.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <font_awesome.h>
#include <utility>

static constexpr int MODEM_DETECT_MAX_RETRIES = 30;

static constexpr int NETWORK_REG_MAX_RETRIES = 6;
static constexpr int NETWORK_REG_WAIT_TIMEOUT_MS = 3000;
static constexpr int MODEM_DETECT_BAUD_SWITCH_DELAY_MS = 250;
static constexpr int MODEM_DETECT_RETRY_DELAY_MS = 1200;
static constexpr int CSQ_CACHE_REFRESH_MS = 5000;



#ifndef ML307_ENABLE_EDRX
// Keep eDRX disabled by default. Interactive websocket audio sessions over
// cellular are sensitive to modem receive sleep windows and may be dropped.
#define ML307_ENABLE_EDRX 0
#endif
#ifndef ML307_EDRX_ACT



#define ML307_EDRX_ACT 7
#endif
#ifndef ML307_EDRX_VALUE

#define ML307_EDRX_VALUE "0011"
#endif
#ifndef ML307_EDRX_PTW

#define ML307_EDRX_PTW ""
#endif

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin) : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin) {
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void Ml307Board::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::ModemDetecting:
            
            break;
        case NetworkEvent::Connecting:
            
            break;
        case NetworkEvent::Connected:
            
            break;
        case NetworkEvent::Disconnected:
            
            break;
        case NetworkEvent::ModemErrorNoSim:
            
            break;
        case NetworkEvent::ModemErrorRegDenied:
            
            break;
        case NetworkEvent::ModemErrorInitFailed:
            
            break;
        case NetworkEvent::ModemErrorTimeout:
            
            break;
        default:
            break;
    }

    
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void Ml307Board::NetworkTask() {
    OnNetworkEvent(NetworkEvent::ModemDetecting);

    static const int detect_baud_rates[] = {921600, 115200};
    int detect_retries = 0;
    while (detect_retries < MODEM_DETECT_MAX_RETRIES) {
        if (stop_requested_) {
            return;
        }

        for (size_t i = 0; i < sizeof(detect_baud_rates) / sizeof(detect_baud_rates[0]); ++i) {
            if (stop_requested_) {
                return;
            }

            modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, detect_baud_rates[i], 8000);
            if (modem_ != nullptr) {
                break;
            }

            if (i + 1 < sizeof(detect_baud_rates) / sizeof(detect_baud_rates[0])) {
                vTaskDelay(pdMS_TO_TICKS(MODEM_DETECT_BAUD_SWITCH_DELAY_MS));
            }
        }

        if (modem_ != nullptr) {
            modem_->GetAtUart()->SetDebug(true);  
            break;
        }
        detect_retries++;
        vTaskDelay(pdMS_TO_TICKS(MODEM_DETECT_RETRY_DELAY_MS));
    }

    if (modem_ == nullptr) {
        if (stop_requested_) {
            return;
        }
        
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return;
    }

    

#if ML307_ENABLE_EDRX
    {
        auto uart = modem_->GetAtUart();
        if (uart) {
            
            
            const int act_try_list[] = {ML307_EDRX_ACT, 7, 9, 4, 5};
            bool ok = false;
            for (int act : act_try_list) {
                if (act <= 0) continue;
                std::string cmd3 = std::string("AT+CEDRXS=2,") + std::to_string(act) + ",\"" + ML307_EDRX_VALUE + "\"";
                std::string cmd4 = std::string("AT+CEDRXS=2,") + std::to_string(act) + ",\"" + ML307_EDRX_VALUE + "\",\"" + ML307_EDRX_PTW + "\"";

                
                if (uart->SendCommand(cmd3, 2500)) {
                    ok = true;
                    break;
                }
                if (strlen(ML307_EDRX_PTW) > 0) {
                    
                    if (uart->SendCommand(cmd4, 2500)) {
                        ok = true;
                        break;
                    }
                }
            }
            if (!ok) {
                
            }
            
            (void)uart->SendCommand("AT+CEDRXS?", 2500);
        }
    }
#endif

    OnNetworkEvent(NetworkEvent::Connecting);

    
    int reg_retries = 0;
    while (reg_retries < NETWORK_REG_MAX_RETRIES) {
        if (stop_requested_) {
            return;
        }
        auto result = modem_->WaitForNetworkReady(NETWORK_REG_WAIT_TIMEOUT_MS);
        if (result == NetworkStatus::Ready) {
            break;
        } else if (result == NetworkStatus::ErrorInsertPin) {
            OnNetworkEvent(NetworkEvent::ModemErrorNoSim);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            OnNetworkEvent(NetworkEvent::ModemErrorRegDenied);
        } else if (result == NetworkStatus::ErrorTimeout) {
            OnNetworkEvent(NetworkEvent::ModemErrorTimeout);
        }
        reg_retries++;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    if (!modem_->network_ready()) {
        if (stop_requested_) {
            return;
        }
        
        return;
    }

    // Delay the first Connected event until WaitForNetworkReady() confirms the
    // modem has both registered and obtained an IP address.
    modem_->OnNetworkStateChanged([this](bool network_ready) {
        if (network_ready) {
            OnNetworkEvent(NetworkEvent::Connected);
        } else {
            OnNetworkEvent(NetworkEvent::Disconnected);
        }
    });
    OnNetworkEvent(NetworkEvent::Connected);

    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    
    
    
}

void Ml307Board::StartNetwork() {
    if (running_) {
        return;
    }
    stop_requested_ = false;
    running_ = true;
    
    BaseType_t ok = xTaskCreate([](void* arg) {
        Ml307Board* board = static_cast<Ml307Board*>(arg);
        board->NetworkTask();
        board->running_ = false;
        vTaskDelete(NULL);
    }, "ml307_net", 8192, this, 5, NULL);
    if (ok != pdPASS) {
        running_ = false;
        stop_requested_ = true;
        ESP_LOGE("Ml307Board", "Failed to create ml307_net task");
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
    }
}

void Ml307Board::StopNetwork() {
    stop_requested_ = true;
    if (modem_) {
        modem_->SetFlightMode(true);
    }
    OnNetworkEvent(NetworkEvent::Disconnected);
}

bool Ml307Board::WaitUntilStopped(int timeout_ms) const {
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();

    while (running_.load()) {
        if (timeout_ms >= 0 && (xTaskGetTickCount() - start) >= timeout_ticks) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return true;
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return FONT_AWESOME_SIGNAL_OFF;
    }

    auto now = std::chrono::steady_clock::now();
    if (cached_csq_ < 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_csq_update_).count() >= CSQ_CACHE_REFRESH_MS) {
        cached_csq_ = modem_->GetCsq();
        last_csq_update_ = now;
    }
    int csq = cached_csq_;
    if (csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 9) {
        return FONT_AWESOME_SIGNAL_WEAK;
    } else if (csq >= 10 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_FAIR;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_GOOD;
    } else if (csq >= 20 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }

    
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveLevel(PowerSaveLevel level) {
    
    (void)level;
}

std::string Ml307Board::GetDeviceStatusJson() {
    
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { 
        auto theme = display->GetTheme();
        if (theme != nullptr) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
