#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

class Board;
class Display;
class AudioCodec;

#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();

    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);

    void Alert(const char* status, const char* message, const char* emotion = "",
               const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    void ToggleChatState();

    void StartListening();

    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    void RefreshWakeWordDetection();
    void ExitAiChatToStandby();
    void PublishMqttTelemetry();
    void HandleRebindSuccess();

    void ResetProtocol();
    void ResetProtocolSync(int timeout_ms = 1500);
    bool RequestOtaNetworkSwitch(BoardNetworkMode target);
    void NotifyOtaNetworkSwitchRequested(BoardNetworkMode target);
    bool IsOtaUpgradeInProgress() const { return ota_upgrade_in_progress_.load(); }
    bool IsOtaNetworkSwitchPending() const { return ota_network_switch_pending_.load(); }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool startup_activation_completed_ = false;
    bool play_popup_on_listening_ = false;
    bool keep_ai_chat_visible_on_idle_ = false;
    std::atomic<bool> reboot_in_progress_{false};
    std::atomic<uint32_t> protocol_generation_{0};
    std::atomic<bool> ota_upgrade_in_progress_{false};
    std::atomic<bool> ota_upgrade_cancel_requested_{false};
    std::atomic<bool> ota_network_switch_pending_{false};
    std::atomic<int> ota_network_switch_target_{static_cast<int>(BoardNetworkMode::UNSUPPORTED)};
    std::atomic<uint32_t> ota_network_switch_start_generation_{0};
    std::atomic<uint32_t> network_connected_generation_{0};
    std::atomic<int> last_connected_network_mode_{static_cast<int>(BoardNetworkMode::UNSUPPORTED)};
    int clock_ticks_ = 0;
    int listening_silence_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    TaskHandle_t reboot_task_handle_ = nullptr;
    std::string idle_assistant_message_;
    std::string pending_idle_notification_;
    int pending_idle_notification_duration_ms_ = 0;

    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void ShowPendingIdleNotification(Display* display);

    void ActivationTask();

    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationStatus(const std::string& message, const char* emotion);
    void RestartProtocolFromSettings();
    void SetupProtocolCallbacks(Display* display, AudioCodec* codec, Board& board,
                                uint32_t generation);
    void HandleMqttCommand(const char* json, int len);
    void ShowActivationCode(const std::string& code, const std::string& message, bool play_sound);
    bool WaitForOtaNetworkConnected(BoardNetworkMode target, uint32_t min_generation,
                                    int timeout_ms);
    void ClearOtaNetworkSwitchRequest();
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    bool IsWakeWordAllowedOnCurrentScreen();
    void FinishReboot();
    static void RebootTask(void* arg);

    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};

class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif
