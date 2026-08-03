#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <limits>
#include <arpa/inet.h>
#include <utility>
#include "assets/lang_config.h"

#define TAG "MQTT"
#define VOICE_SESSION_TAG "VoiceSession"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                
                auto alive = protocol->alive_;  
                app.Schedule([protocol, alive]() {
                    if (*alive) {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    
    
    
    *alive_ = false;
    
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

namespace {

bool ParseEndpoint(const std::string& endpoint, std::string* address, int* port) {
    size_t pos = endpoint.find(':');
    if (pos == std::string::npos) {
        *address = endpoint;
        return !address->empty();
    }

    *address = endpoint.substr(0, pos);
    std::string port_text = endpoint.substr(pos + 1);
    if (address->empty() || port_text.empty()) {
        return false;
    }

    int parsed_port = 0;
    for (char ch : port_text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const int digit = ch - '0';
        if (parsed_port > (65535 - digit) / 10) {
            return false;
        }
        parsed_port = parsed_port * 10 + digit;
    }
    if (parsed_port <= 0) {
        return false;
    }

    *port = parsed_port;
    return true;
}

bool HexValue(char c, uint8_t* value) {
    if (c >= '0' && c <= '9') {
        *value = c - '0';
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *value = c - 'A' + 10;
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *value = c - 'a' + 10;
        return true;
    }
    return false;
}

}  // namespace

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }

    mqtt_ = network->CreateMqtt(0);
    if (mqtt_ == nullptr) {
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        }
        return false;
    }
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                auto alive = alive_;  
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    
    std::string broker_address;
    int broker_port = 8883;
    if (!ParseEndpoint(endpoint, &broker_address, &broker_port)) {
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    
    if (!mqtt_->Publish(publish_topic_, text)) {
        
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr || packet == nullptr || aes_nonce_.size() < 16 ||
        packet->payload.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    std::string nonce(aes_nonce_);
    uint16_t payload_size = htons(static_cast<uint16_t>(packet->payload.size()));
    uint32_t timestamp = htonl(packet->timestamp);
    uint32_t sequence = htonl(++local_sequence_);
    memcpy(&nonce[2], &payload_size, sizeof(payload_size));
    memcpy(&nonce[8], &timestamp, sizeof(timestamp));
    memcpy(&nonce[12], &sequence, sizeof(sequence));

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet->payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet->payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    

    
    
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (message.empty() || !SendText(message)) {
        return false;
    }

    
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    udp_ = network->CreateUdp(2);
    if (udp_ == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    udp_->OnMessage([this](const std::string& data) {
        
        if (aes_nonce_.size() < 16 || data.size() < aes_nonce_.size()) {
            
            return;
        }
        if (data[0] != 0x01) {
            
            return;
        }
        uint32_t timestamp_net = 0;
        uint32_t sequence_net = 0;
        memcpy(&timestamp_net, data.data() + 8, sizeof(timestamp_net));
        memcpy(&sequence_net, data.data() + 12, sizeof(sequence_net));
        uint32_t timestamp = ntohl(timestamp_net);
        uint32_t sequence = ntohl(sequence_net);
        if (sequence < remote_sequence_) {
            
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    auto cleanup_root = [&]() -> std::string {
        cJSON_Delete(root);
        return {};
    };
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
    if (features == nullptr) {
        return cleanup_root();
    }
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    if (audio_params == nullptr) {
        return cleanup_root();
    }
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    if (json_str == nullptr) {
        return cleanup_root();
    }
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || transport->valuestring == nullptr ||
        strcmp(transport->valuestring, "udp") != 0) {
        
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

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key = cJSON_GetObjectItem(udp, "key");
    auto nonce = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || server->valuestring == nullptr ||
        !cJSON_IsNumber(port) || port->valueint <= 0 ||
        !cJSON_IsString(key) || key->valuestring == nullptr ||
        !cJSON_IsString(nonce) || nonce->valuestring == nullptr) {
        return;
    }

    std::string decoded_key = DecodeHexString(key->valuestring);
    std::string decoded_nonce = DecodeHexString(nonce->valuestring);
    if (decoded_key.size() != 16 || decoded_nonce.size() < 16) {
        return;
    }

    udp_server_ = server->valuestring;
    udp_port_ = port->valueint;

    
    
    aes_nonce_ = std::move(decoded_nonce);
    mbedtls_aes_init(&aes_ctx_);
    if (mbedtls_aes_setkey_enc(&aes_ctx_, reinterpret_cast<const unsigned char*>(decoded_key.data()), 128) != 0) {
        return;
    }
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    if ((hex_string.size() % 2) != 0) {
        return {};
    }

    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        uint8_t high = 0;
        uint8_t low = 0;
        if (!HexValue(hex_string[i], &high) || !HexValue(hex_string[i + 1], &low)) {
            return {};
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
