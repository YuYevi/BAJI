#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"
#define VOICE_SESSION_TAG "VoiceSession"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    
    if (!websocket_->Send(text)) {
        
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    auto websocket = std::move(websocket_);
    if (send_goodbye && websocket != nullptr && websocket->IsConnected() && !session_id_.empty()) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        websocket->Send(message);
    }
    if (websocket != nullptr) {
        websocket->Close();
    }
    NotifyAudioChannelClosed();
}

bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    audio_channel_open_ = false;
    session_id_.clear();
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    if (!token.empty()) {
        
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (data == nullptr || len == 0) {
            return;
        }

        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Dropping short binary v2 frame: %u", static_cast<unsigned>(len));
                        return;
                    }
                    const auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(data);
                    const uint32_t timestamp = ntohl(bp2->timestamp);
                    const uint32_t payload_size = ntohl(bp2->payload_size);
                    if (payload_size > len - sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Dropping invalid binary v2 frame: payload=%u, len=%u",
                                 static_cast<unsigned>(payload_size), static_cast<unsigned>(len));
                        return;
                    }
                    auto payload = reinterpret_cast<const uint8_t*>(data + sizeof(BinaryProtocol2));
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)
                    }));
                } else if (version_ == 3) {
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Dropping short binary v3 frame: %u", static_cast<unsigned>(len));
                        return;
                    }
                    const auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(data);
                    const uint16_t payload_size = ntohs(bp3->payload_size);
                    if (payload_size > len - sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Dropping invalid binary v3 frame: payload=%u, len=%u",
                                 payload_size, static_cast<unsigned>(len));
                        return;
                    }
                    auto payload = reinterpret_cast<const uint8_t*>(data + sizeof(BinaryProtocol3));
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGW(TAG, "Dropping invalid websocket JSON frame");
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        NotifyAudioChannelClosed();
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
    });

    
    if (!websocket_->Connect(url.c_str())) {
        
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    

    audio_channel_open_ = true;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || transport->valuestring == nullptr ||
        strcmp(transport->valuestring, "websocket") != 0) {
        
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

void WebsocketProtocol::NotifyAudioChannelClosed() {
    if (!audio_channel_open_) {
        return;
    }
    audio_channel_open_ = false;
    session_id_.clear();
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}
