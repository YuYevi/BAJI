#pragma once

#include <array>
#include <atomic>
#include <aes/esp_aes.h>
#include <cassert>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "esp_blufi_api.h"
#include "esp_err.h"
#include "esp_event.h"
#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_gap_ble_api.h"
#endif
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "wifi_manager.h"

class Blufi {
public:
    
    static Blufi &GetInstance();

    
    void start_wifi_scan();

    
    esp_err_t init();

    
    esp_err_t deinit();

    bool IsActive() const;

    bool IsProvisioning() const;

    void SetProvisioningDoneCallback(std::function<void()> callback);

    const char *GetDeviceName() const;

    
    Blufi(const Blufi &) = delete;

    Blufi &operator=(const Blufi &) = delete;

private:
    std::atomic_bool inited_{false};

    Blufi();

    ~Blufi();

    
    static esp_err_t _controller_init();

    static esp_err_t _controller_deinit();

    static esp_err_t _host_init();

    static esp_err_t _host_deinit();

    static esp_err_t _gap_register_callback();

    static esp_err_t _host_and_cb_init();

    void _security_init();

    void _security_deinit();

    void _dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len,
                                    bool *need_free);

    int _aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    int _aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _crc_checksum(uint8_t iv8, uint8_t *data, int len);

    void _handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

    void _start_advertising();
    esp_err_t _configure_identity_scan_response();
    void _schedule_deinit(uint32_t session_generation);
    esp_err_t _deinit_locked();
    void _clear_staged_credentials();
    void _invalidate_wifi_attempt();
    void _cancel_wifi_attempt(bool disconnect_wifi);
    bool _is_attempt_current(uint32_t session_generation, uint32_t attempt_generation,
                             uint32_t ble_generation) const;
    esp_err_t _drain_default_event_loop(TickType_t timeout_ticks);
    esp_err_t _send_wifi_conn_report(esp_blufi_sta_conn_state_t state,
                                     esp_blufi_extra_info_t *extra_info);
    void _run_wifi_connect_task(uint32_t session_generation, uint32_t attempt_generation,
                                uint32_t ble_generation, wifi_config_t wifi_config,
                                std::string ssid, std::string password);
    esp_err_t _request_wifi_disconnect();
    esp_err_t _wait_for_disconnect(uint32_t previous_disconnect_generation);
    static void _wifi_connect_task_trampoline(void *context);
    static void _deinit_task_trampoline(void *context);

    
    void _send_wifi_list(uint32_t ble_generation);
    esp_err_t _start_dedicated_wifi();
    void _stop_dedicated_wifi();
    static void _wifi_scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                         void *event_data);
    static void _wifi_connected_event_handler(void *arg, esp_event_base_t event_base,
                                              int32_t event_id, void *event_data);
    static void _wifi_disconnect_event_handler(void *arg, esp_event_base_t event_base,
                                               int32_t event_id, void *event_data);
    static void _ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                  void *event_data);
    static void _event_drain_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                     void *event_data);

    
    

    static void _event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

#ifdef CONFIG_BT_BLUEDROID_ENABLED
    static void _gap_event_callback_trampoline(esp_gap_ble_cb_event_t event,
                                               esp_ble_gap_cb_param_t *param);
#endif

    static void _negotiate_data_handler_trampoline(uint8_t *data, int len, uint8_t **output_data,
                                                   int *output_len, bool *need_free);

    static int _encrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static int _decrypt_func_trampoline(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

    static uint16_t _checksum_func_trampoline(uint8_t iv8, uint8_t *data, int len);

#ifdef CONFIG_BT_NIMBLE_ENABLED
    static void _nimble_on_reset(int reason);
    static void _nimble_on_sync();
    static void _nimble_host_task(void *param);
#endif

    
    struct BlufiSecurity {
#define DH_SELF_PUB_KEY_LEN 128
        uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN 128
        uint8_t share_key[SHARE_KEY_LEN];
        size_t share_len;
#define PSK_LEN 16
        uint8_t psk[PSK_LEN];
        uint8_t *dh_param;
        int dh_param_len;
        uint8_t iv[16];
        mbedtls_dhm_context *dhm;
        esp_aes_context *aes;
    };

    BlufiSecurity *m_sec;

    struct WifiAttemptState {
        uint32_t session_generation = 0;
        uint32_t attempt_generation = 0;
        uint32_t ble_generation = 0;
        bool accepting_events = false;
        bool associated = false;
        bool got_ip = false;
        bool failed = false;
        uint8_t target_ssid[32]{};
        size_t target_ssid_len = 0;
        bool desired_bssid_set = false;
        uint8_t desired_bssid[6]{};
        bool actual_bssid_set = false;
        uint8_t actual_bssid[6]{};
    };

    enum class WifiDriverState : uint8_t {
        kIdle,
        kConnectActive,
        kDisconnectPending,
    };

    
    wifi_config_t m_sta_config{};
    std::atomic_bool m_ble_is_connected;
    std::atomic_bool m_sta_connected;
    std::atomic_bool m_sta_got_ip;
    std::atomic_bool m_sta_connect_failed{false};
    std::atomic_bool m_provisioned;
    std::atomic_bool m_finalizing{false};
    std::atomic_bool m_deinited;
    uint8_t m_sta_bssid[6]{};
    bool m_sta_bssid_valid = false;
    uint8_t m_sta_ssid[32]{};
    int m_sta_ssid_len;
    size_t m_received_ssid_len = 0;
    size_t m_received_password_len = 0;
    std::atomic_bool m_sta_is_connecting;
    esp_blufi_extra_info_t m_sta_conn_info{};

    
    std::vector<wifi_ap_record_t> m_ap_records;
    std::atomic_bool m_scan_in_progress{false};
    esp_event_handler_instance_t m_scan_event_instance = nullptr;
    esp_event_handler_instance_t m_connected_event_instance = nullptr;
    esp_event_handler_instance_t m_disconnect_event_instance = nullptr;
    esp_event_handler_instance_t m_ip_event_instance = nullptr;
    esp_event_handler_instance_t m_drain_event_instance = nullptr;
    std::atomic<esp_netif_t *> m_station_netif{nullptr};
    SemaphoreHandle_t m_event_drain_semaphore = nullptr;
    bool m_wifi_started = false;

    std::array<uint8_t, 29> m_identity_scan_response{};
    char m_device_name[32]{};

    std::atomic<uint32_t> m_session_generation{0};
    std::atomic<uint32_t> m_connect_attempt_generation{0};
    std::atomic<uint32_t> m_ble_connection_generation{0};
    std::atomic<uint32_t> m_scan_ble_generation{0};
    std::atomic<uint32_t> m_wifi_scan_done_generation{0};
    std::atomic<uint32_t> m_wifi_disconnect_generation{0};
    std::atomic<WifiDriverState> m_wifi_driver_state{WifiDriverState::kIdle};
    std::atomic_bool m_deinit_task_scheduled{false};
    bool m_controller_initialized = false;
    bool m_host_initialized = false;
    WifiAttemptState m_attempt;

    mutable std::recursive_mutex m_lifecycle_mutex;
    std::mutex m_operation_mutex;
    std::recursive_mutex m_security_mutex;
    mutable std::mutex m_attempt_mutex;
    std::mutex m_scan_mutex;
    std::mutex m_event_drain_mutex;
    std::mutex m_callback_mutex;
    std::function<void()> m_provisioning_done_callback;
};
