#include "mqtt_control.h"

#include "application.h"
#include "board.h"
#include "mqtt.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <ctime>
#include <string>
#include <utility>

#define TAG_CTRL "MqttControl"

namespace {

constexpr int kKeepAliveSeconds = 120; // MQTT 保活时间（秒）
constexpr int kReconnectTimeoutMs = 3000; // 重连超时（毫秒）
constexpr int kNetworkTimeoutMs = 10000; // 网络超时（毫秒）
constexpr int kOfflinePublishDrainDelayMs = 500; // 主动下线消息发送后等待底层刷出
constexpr int kTokenRefreshAheadSeconds = 3600; // 提前 1 小时刷新 Token
constexpr int kTokenRefreshCheckPeriodUs = 5 * 60 * 1000 * 1000; // Token 刷新检查周期（5分钟，微秒）
struct MqttEndpointConfig {
    std::string raw_endpoint;
    std::string uri;
    std::string host;
    int port = 1883;
    esp_mqtt_transport_t transport = MQTT_TRANSPORT_OVER_TCP;
    bool use_ssl = false;
    bool used_websocket = false;
};

// 获取存储的 JWT Token
std::string GetJwtToken() {
    Settings auth("auth", false);
    return auth.GetString("token");
}

// MQTT 对接统一使用 MAC 地址作为设备标识
std::string GetMqttDeviceId() {
    return SystemInfo::GetMacAddress();
}

// 检查 Token 是否即将过期
bool IsTokenExpiringSoon() {
    Settings auth("auth", false);
    const int expires = auth.GetInt("expires", 0);
    return expires <= 0 || time(nullptr) >= static_cast<time_t>(expires - kTokenRefreshAheadSeconds);
}

// 从 URL 中提取 Origin (scheme + host)
std::string GetUrlOrigin(const std::string& url) {
    const size_t scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        return {};
    }

    const size_t host_begin = scheme_pos + 3;
    const size_t host_end = url.find('/', host_begin);
    if (host_end == std::string::npos) {
        return url;
    }
    return url.substr(0, host_end);
}

// 获取认证服务器 URL
std::string GetAuthUrl() {
    Settings settings("wifi", false);
    std::string ota_url = settings.GetString("ota_url", CONFIG_OTA_URL);
    std::string origin = GetUrlOrigin(ota_url);
    if (origin.empty()) {
        origin = "https://www.xinyhx.com";
    }
    return origin + "/authcyhx/device";
}

// 刷新 JWT Token
bool RefreshToken() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG_CTRL, "Network is not ready for token refresh");
        return false;
    }

    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG_CTRL, "Failed to create auth HTTP client");
        return false;
    }

    const std::string device_id = GetMqttDeviceId();
    const std::string url = GetAuthUrl();

    // 构建认证请求
    cJSON* req = cJSON_CreateObject();
    if (req == nullptr) {
        ESP_LOGE(TAG_CTRL, "Failed to allocate auth request JSON");
        return false;
    }
    cJSON_AddStringToObject(req, "deviceId", device_id.c_str());
    char* req_str = cJSON_PrintUnformatted(req);
    if (req_str == nullptr) {
        ESP_LOGE(TAG_CTRL, "Failed to serialize auth request JSON");
        cJSON_Delete(req);
        return false;
    }
    std::string body = req_str;
    cJSON_free(req_str);
    cJSON_Delete(req);

    http->SetTimeout(30000);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::move(body));
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG_CTRL, "Failed to connect auth server: %s, err=%d", url.c_str(),
                 http->GetLastError());
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG_CTRL, "Auth failed: status=%d, err=%d", status_code, http->GetLastError());
        http->Close();
        return false;
    }

    std::string response = http->ReadAll();
    http->Close();

    // 解析认证响应
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG_CTRL, "Failed to parse auth response");
        return false;
    }

    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 100000) {
        ESP_LOGE(TAG_CTRL, "Auth response rejected: code=%d", code->valueint);
        cJSON_Delete(root);
        return false;
    }

    cJSON* payload = root;
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        payload = data;
    }

    cJSON* token = cJSON_GetObjectItem(payload, "token");
    cJSON* expires = cJSON_GetObjectItem(payload, "expires");
    if (!cJSON_IsString(token) || token->valuestring == nullptr || !cJSON_IsNumber(expires)) {
        ESP_LOGE(TAG_CTRL, "Auth response missing token/expires");
        cJSON_Delete(root);
        return false;
    }

    // 保存新 Token 和过期时间
    const time_t expires_at = time(nullptr) + static_cast<time_t>(expires->valueint);
    Settings auth("auth", true);
    auth.SetString("token", token->valuestring);
    auth.SetInt("expires", static_cast<int32_t>(expires_at));

    ESP_LOGW(TAG_CTRL, "Token refreshed, expires in %d seconds", expires->valueint);
    cJSON_Delete(root);
    return true;
}

// 字符串转小写
std::string ToLower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool ParseEndpointPort(const std::string& endpoint, std::string* host, int* port) {
    *host = endpoint;

    const size_t colon_pos = endpoint.rfind(':');
    if (colon_pos == std::string::npos || endpoint.find(':') != colon_pos) {
        return true;
    }

    const std::string parsed_host = endpoint.substr(0, colon_pos);
    const std::string port_str = endpoint.substr(colon_pos + 1);
    if (parsed_host.empty() || port_str.empty()) {
        return false;
    }

    try {
        *port = std::stoi(port_str);
    } catch (...) {
        return false;
    }

    *host = parsed_host;
    return true;
}

bool ResolveEndpointConfig(Settings& settings, bool native_transport, MqttEndpointConfig* config) {
    const bool use_websocket = settings.GetBool("use_websocket", false);
    const std::string cellular_endpoint = settings.GetString("cellular_endpoint");
    std::string endpoint = native_transport ? cellular_endpoint : settings.GetString("endpoint");
    if (endpoint.empty()) {
        endpoint = settings.GetString("endpoint");
    }
    if (endpoint.empty()) {
        return false;
    }

    config->raw_endpoint = endpoint;
    if (endpoint.find("://") != std::string::npos) {
        config->uri = endpoint;
        const size_t scheme_end = endpoint.find("://");
        const std::string scheme = ToLower(endpoint.substr(0, scheme_end));
        const size_t host_begin = scheme_end + 3;
        const size_t path_begin = endpoint.find('/', host_begin);
        std::string host_port = path_begin == std::string::npos
                                    ? endpoint.substr(host_begin)
                                    : endpoint.substr(host_begin, path_begin - host_begin);

        if (scheme == "ws") {
            config->transport = MQTT_TRANSPORT_OVER_WS;
            config->port = 80;
            config->used_websocket = true;
        } else if (scheme == "wss") {
            config->transport = MQTT_TRANSPORT_OVER_WSS;
            config->port = 443;
            config->use_ssl = true;
            config->used_websocket = true;
        } else if (scheme == "mqtts" || scheme == "ssl") {
            config->transport = MQTT_TRANSPORT_OVER_SSL;
            config->port = 8883;
            config->use_ssl = true;
        } else {
            config->transport = MQTT_TRANSPORT_OVER_TCP;
            config->port = 1883;
        }

        if (!ParseEndpointPort(host_port, &config->host, &config->port)) {
            return false;
        }

        if (config->used_websocket && path_begin == std::string::npos) {
            config->uri += "/mqtt";
        }
    } else {
        config->port = (native_transport && !cellular_endpoint.empty())
                           ? (settings.GetBool("cellular_ssl", false) ? 8883 : 1883)
                           : (use_websocket ? 8083 : 1883);
        if (!ParseEndpointPort(endpoint, &config->host, &config->port)) {
            return false;
        }

        if (use_websocket) {
            config->transport = MQTT_TRANSPORT_OVER_WS;
            config->used_websocket = true;
            config->uri = "ws://" + config->host + ":" + std::to_string(config->port) + "/mqtt";
        } else {
            config->transport = MQTT_TRANSPORT_OVER_TCP;
            config->uri = "mqtt://" + config->host + ":" + std::to_string(config->port);
        }
    }

    if (native_transport) {
        const int cellular_port = settings.GetInt("cellular_port", 0);
        if (cellular_port > 0) {
            config->port = cellular_port;
        } else if (config->used_websocket) {
            config->port = config->use_ssl ? 8883 : 1883;
        }
        config->use_ssl = settings.GetBool("cellular_ssl", config->use_ssl);
    }

    return !config->host.empty();
}

}  // namespace

MqttControl& MqttControl::GetInstance() {
    static MqttControl instance;
    return instance;
}

MqttControl::MqttControl() {
    // 创建 Token 刷新定时器
    esp_timer_create_args_t timer_args = {
        .callback = &MqttControl::TokenRefreshTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mqtt_token_refresh",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &token_refresh_timer_));

}

MqttControl::~MqttControl() {
    Stop();
    if (token_refresh_timer_ != nullptr) {
        esp_timer_delete(token_refresh_timer_);
        token_refresh_timer_ = nullptr;
    }
}

void MqttControl::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    StartLocked();
}

void MqttControl::StartLocked() {
    if (client_ != nullptr || native_client_ != nullptr) {
        return;
    }

    // 检查 Token 是否需要刷新
    if (IsTokenExpiringSoon() || GetJwtToken().empty()) {
        if (!RefreshToken()) {
            ESP_LOGE(TAG_CTRL, "Failed to obtain MQTT token");
            return;
        }
    }

    auto& board = Board::GetInstance();
    const bool use_native_mqtt = board.GetActiveNetworkMode() == BoardNetworkMode::CELLULAR;
    Settings settings("mqtt_ctrl", false);
    MqttEndpointConfig endpoint_config;
    if (!ResolveEndpointConfig(settings, use_native_mqtt, &endpoint_config)) {
        ESP_LOGE(TAG_CTRL, "Missing mqtt_control endpoint");
        return;
    }

    const std::string device_id = GetMqttDeviceId();
    const std::string token = GetJwtToken();
    if (device_id.empty() || token.empty()) {
        ESP_LOGE(TAG_CTRL, "MQTT credentials are incomplete");
        return;
    }

    if (use_native_mqtt) {
        auto network = board.GetNetwork();
        if (network == nullptr) {
            ESP_LOGE(TAG_CTRL, "Network is not ready for native MQTT");
            return;
        }

        native_client_ = network->CreateMqtt(1);
        if (native_client_ == nullptr) {
            ESP_LOGE(TAG_CTRL, "Failed to create native MQTT client");
            return;
        }

        backend_ = Backend::NATIVE_MQTT;
        native_client_->SetKeepAlive(kKeepAliveSeconds);
        native_client_->OnConnected([this]() {
            connected_ = true;
            Application::GetInstance().Schedule([this]() {
                PostConnectSetup();
            });
        });
        native_client_->OnDisconnected([this]() {
            connected_ = false;
            command_buffer_.clear();
            ESP_LOGW(TAG_CTRL, "Disconnected(native)");
        });
        native_client_->OnMessage([this](const std::string& topic, const std::string& payload) {
            ESP_LOGW(TAG_CTRL, "CMD(native) topic=%s payload=%s", topic.c_str(), payload.c_str());
            auto callback = on_command_;
            if (callback) {
                // Avoid re-entering the modem AT parser task. Command handling may publish
                // MQTT replies synchronously, which must run outside the native MQTT URC path.
                Application::GetInstance().Schedule([callback = std::move(callback), payload]() {
                    callback(payload.c_str(), static_cast<int>(payload.size()));
                });
            }
        });
        native_client_->OnError([this](const std::string& error) {
            connected_ = false;
            ESP_LOGE(TAG_CTRL, "MQTT native error: %s", error.c_str());
        });

        if (!native_client_->Connect(endpoint_config.host, endpoint_config.port, device_id, device_id,
                                     token)) {
            ESP_LOGE(TAG_CTRL, "Failed to start native MQTT client: endpoint=%s, err=%d",
                     endpoint_config.host.c_str(),
                     native_client_ != nullptr ? native_client_->GetLastError() : -1);
            native_client_.reset();
            backend_ = Backend::NONE;
            return;
        }

        if (token_refresh_timer_ != nullptr) {
            esp_timer_stop(token_refresh_timer_);
            esp_timer_start_periodic(token_refresh_timer_, kTokenRefreshCheckPeriodUs);
        }

        command_buffer_.clear();
        if (endpoint_config.used_websocket) {
            ESP_LOGW(TAG_CTRL,
                     "Cellular MQTT is using native transport fallback, broker=%s:%d (from %s)",
                     endpoint_config.host.c_str(), endpoint_config.port,
                     endpoint_config.raw_endpoint.c_str());
        }
        ESP_LOGW(TAG_CTRL, "Started(native), broker=%s:%d, device=%s",
                 endpoint_config.host.c_str(), endpoint_config.port, device_id.c_str());
        return;
    }

    // 构建遗嘱消息 (Last Will and Testament)
    const std::string status_topic = BuildTopic("status");
    const std::string will_message =
        "{\"online\":false,\"deviceId\":\"" + device_id +
        "\",\"timestamp\":" + std::to_string(static_cast<long long>(time(nullptr))) + "}";

    // 配置 MQTT 客户端
    esp_mqtt_client_config_t config = {};
    config.broker.address.uri = endpoint_config.uri.c_str();
    if (endpoint_config.transport == MQTT_TRANSPORT_OVER_WSS ||
        endpoint_config.transport == MQTT_TRANSPORT_OVER_SSL) {
        config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.credentials.client_id = device_id.c_str();
    config.credentials.username = device_id.c_str();
    config.credentials.authentication.password = token.c_str();
    config.session.keepalive = kKeepAliveSeconds;
    config.session.last_will.topic = status_topic.c_str();
    config.session.last_will.msg = will_message.c_str();
    config.session.last_will.msg_len = static_cast<int>(will_message.size());
    config.session.last_will.qos = 1;
    config.session.last_will.retain = true;
    config.network.reconnect_timeout_ms = kReconnectTimeoutMs;
    config.network.timeout_ms = kNetworkTimeoutMs;

    client_ = esp_mqtt_client_init(&config);
    if (client_ == nullptr) {
        ESP_LOGE(TAG_CTRL, "Failed to initialize MQTT client");
        return;
    }

    backend_ = Backend::ESP_MQTT;
    esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, EventHandler, this);
    const esp_err_t err = esp_mqtt_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_CTRL, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        backend_ = Backend::NONE;
        return;
    }

    // 启动周期性 Token 刷新检查
    if (token_refresh_timer_ != nullptr) {
        esp_timer_stop(token_refresh_timer_);
        esp_timer_start_periodic(token_refresh_timer_, kTokenRefreshCheckPeriodUs);
    }

    command_buffer_.clear();
    ESP_LOGW(TAG_CTRL, "Started, broker=%s, device=%s", endpoint_config.uri.c_str(),
             device_id.c_str());
}

void MqttControl::PostConnectSetup() {
    // 模组在刚连上 MQTT 后立即订阅/发布，容易让 URC 线程过载。
    vTaskDelay(pdMS_TO_TICKS(500));

    if (native_client_ == nullptr || !native_client_->IsConnected()) {
        return;
    }

    const std::string cmd_topic = BuildTopic("cmd");
    if (native_client_->Subscribe(cmd_topic, 1)) {
        ESP_LOGW(TAG_CTRL, "Connected(native), subscribed %s", cmd_topic.c_str());
    } else {
        ESP_LOGW(TAG_CTRL, "Subscribe(native) failed: %s, err=%d",
                 cmd_topic.c_str(), native_client_->GetLastError());
    }

    if (native_client_ != nullptr && native_client_->IsConnected()) {
        PublishStatusPayload(true);
    }

    Application::GetInstance().Schedule([]() {
        Application::GetInstance().PublishMqttTelemetry();
    });

}

void MqttControl::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked(true);
}

void MqttControl::StopForNetworkSwitch() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (token_refresh_timer_ != nullptr) {
        esp_timer_stop(token_refresh_timer_);
    }

    if (PublishOfflineStatusLocked()) {
        vTaskDelay(pdMS_TO_TICKS(kOfflinePublishDrainDelayMs));
    }

    if (backend_ == Backend::ESP_MQTT && client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    } else if (backend_ == Backend::NATIVE_MQTT && native_client_ != nullptr) {
        // Fast teardown avoids blocking on AT-level disconnect during network switching.
        native_client_.reset();
    }

    backend_ = Backend::NONE;
    connected_ = false;
    command_buffer_.clear();
}

void MqttControl::StopLocked(bool publish_offline_status) {
    if (token_refresh_timer_ != nullptr) {
        esp_timer_stop(token_refresh_timer_);
    }

    if (client_ == nullptr && native_client_ == nullptr) {
        backend_ = Backend::NONE;
        connected_ = false;
        command_buffer_.clear();
        return;
    }

    // 停止前发布离线状态
    if (publish_offline_status && PublishOfflineStatusLocked()) {
        vTaskDelay(pdMS_TO_TICKS(kOfflinePublishDrainDelayMs));
    }

    if (backend_ == Backend::ESP_MQTT && client_ != nullptr) {
        esp_mqtt_client_stop(client_);
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
    } else if (backend_ == Backend::NATIVE_MQTT && native_client_ != nullptr) {
        native_client_->Disconnect();
        native_client_.reset();
    }

    backend_ = Backend::NONE;
    connected_ = false;
    command_buffer_.clear();
}

bool MqttControl::IsConnected() const {
    return connected_.load();
}

void MqttControl::OnCommand(std::function<void(const char* json, int len)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_command_ = std::move(callback);
}

// 上报指令应答
bool MqttControl::ReportAck(const char* json) {
    return Publish(BuildTopic("ack"), json, 1, false);
}

// 上报设备状态
bool MqttControl::ReportStatus(const char* json) {
    return Publish(BuildTopic("status"), json, 1, true);
}

// 上报遥测数据
bool MqttControl::ReportTelemetry(const char* json) {
    return Publish(BuildTopic("telemetry"), json, 1, false);
}

// 上报事件
bool MqttControl::ReportEvent(const char* type, const char* json) {
    if (json == nullptr) {
        return false;
    }

    if (type == nullptr || type[0] == '\0') {
        return Publish(BuildTopic("events"), json, 1, false);
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(time(nullptr)));

    cJSON* parsed = cJSON_Parse(json);
    if (parsed != nullptr) {
        cJSON_AddItemToObject(root, "data", parsed);
    } else {
        cJSON_AddStringToObject(root, "message", json);
    }

    char* event_json = cJSON_PrintUnformatted(root);
    if (event_json == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    bool ok = Publish(BuildTopic("events"), event_json, 1, false);
    cJSON_free(event_json);
    cJSON_Delete(root);
    return ok;
}

// 构建 MQTT Topic
std::string MqttControl::BuildTopic(const char* suffix) const {
    return "device/" + GetMqttDeviceId() + "/" + suffix;
}

bool MqttControl::PublishOfflineStatusLocked() {
    if (!connected_) {
        return false;
    }

    const std::string offline_payload =
        "{\"online\":false,\"deviceId\":\"" + GetMqttDeviceId() +
        "\",\"timestamp\":" + std::to_string(static_cast<long long>(time(nullptr))) + "}";
    if (!PublishLocked(BuildTopic("status"), offline_payload.c_str(), 1, true)) {
        ESP_LOGW(TAG_CTRL, "Failed to publish offline status before disconnect");
        return false;
    }

    return true;
}

bool MqttControl::PublishLocked(const std::string& topic, const char* json, int qos, bool retain) {
    if (json == nullptr || !connected_) {
        return false;
    }

    if (backend_ == Backend::ESP_MQTT) {
        if (client_ == nullptr) {
            return false;
        }
        const int msg_id =
            esp_mqtt_client_publish(client_, topic.c_str(), json, 0, qos, retain ? 1 : 0);
        if (msg_id < 0) {
            ESP_LOGW(TAG_CTRL, "Publish failed, topic=%s", topic.c_str());
            return false;
        }
        return true;
    }

    if (backend_ == Backend::NATIVE_MQTT) {
        if (native_client_ == nullptr) {
            return false;
        }
        if (retain) {
            ESP_LOGW(TAG_CTRL, "Native MQTT backend does not support retain flag, topic=%s",
                     topic.c_str());
        }
        for (int retry = 0; retry < 3; retry++) {
            if (native_client_->Publish(topic, json, qos)) {
                return true;
            }
            ESP_LOGW(TAG_CTRL, "Native publish retry %d/3, topic=%s, err=%d",
                     retry + 1, topic.c_str(), native_client_->GetLastError());
            vTaskDelay(pdMS_TO_TICKS(100 * (retry + 1)));
        }
        return false;
    }

    return false;
}

// 发布 MQTT 消息
bool MqttControl::Publish(const std::string& topic, const char* json, int qos, bool retain) {
    std::lock_guard<std::mutex> lock(mutex_);
    return PublishLocked(topic, json, qos, retain);
}

// 发布设备状态 Payload
bool MqttControl::PublishStatusPayload(bool online) {
    const std::string payload =
        "{\"online\":" + std::string(online ? "true" : "false") +
        ",\"deviceId\":\"" + GetMqttDeviceId() +
        "\",\"timestamp\":" + std::to_string(static_cast<long long>(time(nullptr))) + "}";
    return ReportStatus(payload.c_str());
}

// MQTT 事件处理入口
void MqttControl::EventHandler(void* handler_args, esp_event_base_t base,
                               int32_t event_id, void* event_data) {
    if (event_data == nullptr) {
        return;
    }

    auto* self = static_cast<MqttControl*>(handler_args);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    event->event_id = static_cast<esp_mqtt_event_id_t>(event_id);
    self->HandleEvent(event);
}

// 处理具体的 MQTT 事件
void MqttControl::HandleEvent(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            connected_ = true;
            const std::string cmd_topic = BuildTopic("cmd");
            esp_mqtt_client_subscribe(client_, cmd_topic.c_str(), 1);
            ESP_LOGW(TAG_CTRL, "Connected, subscribed %s", cmd_topic.c_str());
            PublishStatusPayload(true);
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().PublishMqttTelemetry();
            });
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            connected_ = false;
            command_buffer_.clear();
            ESP_LOGW(TAG_CTRL, "Disconnected");
            break;

        case MQTT_EVENT_DATA: {
            // 处理接收到的指令数据（支持分包）
            if (event->current_data_offset == 0) {
                command_buffer_.clear();
            }
            command_buffer_.append(event->data, event->data_len);
            if (command_buffer_.size() < static_cast<size_t>(event->total_data_len)) {
                break;
            }

            ESP_LOGW(TAG_CTRL, "CMD topic=%.*s payload=%s", event->topic_len, event->topic,
                     command_buffer_.c_str());
            auto callback = on_command_;
            if (callback) {
                std::string payload = command_buffer_;
                Application::GetInstance().Schedule([callback = std::move(callback),
                                                     payload = std::move(payload)]() {
                    callback(payload.c_str(), static_cast<int>(payload.size()));
                });
            }
            command_buffer_.clear();
            break;
        }

        case MQTT_EVENT_ERROR:
            connected_ = false;
            if (event->error_handle != nullptr) {
                ESP_LOGE(TAG_CTRL, "MQTT error: type=%d, tls=%d, sock_errno=%d",
                         event->error_handle->error_type,
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_transport_sock_errno);
            } else {
                ESP_LOGE(TAG_CTRL, "MQTT error");
            }
            break;

        default:
            break;
    }
}

void MqttControl::TokenRefreshTimerCallback(void* arg) {
    auto* self = static_cast<MqttControl*>(arg);
    self->ScheduleTokenRefreshCheck();
}

// 调度 Token 刷新检查任务
void MqttControl::ScheduleTokenRefreshCheck() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backend_ == Backend::NONE || refresh_task_running_.exchange(true)) {
        return;
    }

    if (xTaskCreate(&MqttControl::TokenRefreshTask, "mqtt_token", 4096, this, 4, nullptr) != pdPASS) {
        refresh_task_running_ = false;
        ESP_LOGW(TAG_CTRL, "Failed to create token refresh task");
    }
}

void MqttControl::TokenRefreshTask(void* arg) {
    auto* self = static_cast<MqttControl*>(arg);
    self->RefreshTokenIfNeeded();
    self->refresh_task_running_ = false;
    vTaskDelete(nullptr);
}

// 如果 Token 即将过期，则刷新并重启 MQTT 客户端
void MqttControl::RefreshTokenIfNeeded() {
    if (!IsTokenExpiringSoon()) {
        return;
    }

    Backend backend_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        backend_snapshot = backend_;
        if (backend_snapshot == Backend::NONE) {
            return;
        }
    }

    ESP_LOGW(TAG_CTRL, "Token is expiring, refreshing...");
    if (RefreshToken()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (backend_ != backend_snapshot || backend_ == Backend::NONE) {
            return;
        }
        StopLocked(false);
        StartLocked();
    }
}
