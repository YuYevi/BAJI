#ifndef MQTT_CONTROL_H
#define MQTT_CONTROL_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <esp_timer.h>
#include <mqtt_client.h>

class Mqtt;

class MqttControl {
public:
    static MqttControl& GetInstance();

    void Start();           // OTA 完成后调用，启动常驻连接
    void Stop();
    void StopForNetworkSwitch();
    bool IsConnected() const;

    // 设置指令回调
    void OnCommand(std::function<void(const char* json, int len)> callback);

    // 上报 MQTT 消息
    bool ReportAck(const char* json);
    bool ReportStatus(const char* json);
    bool ReportTelemetry(const char* json);
    bool ReportEvent(const char* type, const char* json);

private:
    MqttControl();
    ~MqttControl();

    static void EventHandler(void* handler_args, esp_event_base_t base,
                             int32_t event_id, void* event_data);
    static void TokenRefreshTimerCallback(void* arg);
    static void TokenRefreshTask(void* arg);
    void StartLocked();
    void StopLocked(bool publish_offline_status);
    bool PublishOfflineStatusLocked();
    void HandleEvent(esp_mqtt_event_handle_t event);
    void ScheduleTokenRefreshCheck();
    void RefreshTokenIfNeeded();
    bool PublishLocked(const std::string& topic, const char* json, int qos, bool retain);
    bool Publish(const std::string& topic, const char* json, int qos, bool retain);
    bool PublishStatusPayload(bool online);
    std::string BuildTopic(const char* suffix) const;
    void PostConnectSetup();

    enum class Backend {
        NONE,
        ESP_MQTT,
        NATIVE_MQTT,
    };

    std::mutex mutex_;
    Backend backend_ = Backend::NONE;
    esp_mqtt_client_handle_t client_ = nullptr;
    std::unique_ptr<Mqtt> native_client_;
    esp_timer_handle_t token_refresh_timer_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> refresh_task_running_{false};
    std::function<void(const char*, int)> on_command_;
    std::string command_buffer_;
};

#endif
