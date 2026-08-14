#include "blufi.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "board.h"
#include "esp_bt.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "console/console.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
extern void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
extern int esp_blufi_gatt_svr_init(void);
extern void esp_blufi_gatt_svr_deinit(void);
extern void esp_blufi_btc_init(void);
extern void esp_blufi_btc_deinit(void);
#endif

extern "C" {
void esp_blufi_adv_start(void);

void esp_blufi_adv_stop(void);

void esp_blufi_disconnect(void);

void btc_blufi_report_error(esp_blufi_error_state_t state);

#ifdef CONFIG_BT_BLUEDROID_ENABLED
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
int esp_blufi_gatt_svr_init(void);
void esp_blufi_gatt_svr_deinit(void);
void esp_blufi_btc_init(void);
void esp_blufi_btc_deinit(void);
#endif
}

#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/md5.h"
#include "ssid_manager.h"

static const char* BLUFI_TAG = "BLUFI_CLASS";

ESP_EVENT_DEFINE_BASE(BLUFI_INTERNAL_EVENT);

namespace {

constexpr int kConnectTimeoutMs = 45000;
constexpr int kConnectPollMs = 200;
constexpr int kFinalReportDelayMs = 300;
constexpr int kDisconnectDrainTimeoutMs = 1000;
constexpr int kDisconnectDrainPollMs = 20;
constexpr int kEventDrainTimeoutMs = 1500;
constexpr int kDeinitGraceDelayMs = 500;
constexpr int kMaxDhParamLen = 512;
constexpr int32_t kEventDrainId = 0;
constexpr uint8_t kServiceDataAdType = 0x16;
constexpr uint8_t kBlufiServiceUuidLow = 0xFF;
constexpr uint8_t kBlufiServiceUuidHigh = 0xFF;
constexpr uint8_t kDeviceIdentityType = 0x01;
constexpr uint8_t kDeviceIdentityVersion = 0x02;
constexpr uint8_t kDeviceIdentityFields = 0x07;

int HexValue(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool ParseUuid(const std::string& uuid, uint8_t* output) {
    size_t byte_index = 0;
    int high_nibble = -1;

    for (char value : uuid) {
        if (value == '-') {
            continue;
        }

        const int nibble = HexValue(value);
        if (nibble < 0 || byte_index >= 16) {
            return false;
        }
        if (high_nibble < 0) {
            high_nibble = nibble;
        } else {
            output[byte_index++] = static_cast<uint8_t>((high_nibble << 4) | nibble);
            high_nibble = -1;
        }
    }

    return byte_index == 16 && high_nibble < 0;
}

struct WifiConnectTaskContext {
    Blufi* blufi;
    uint32_t session_generation;
    uint32_t attempt_generation;
    uint32_t ble_generation;
    wifi_config_t wifi_config;
    std::string ssid;
    std::string password;
};

struct BlufiDeinitTaskContext {
    Blufi* blufi;
    uint32_t session_generation;
};

}  // namespace

Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

bool Blufi::IsActive() const {
    return inited_.load() && !m_deinited.load();
}

bool Blufi::IsProvisioning() const {
    std::lock_guard<std::recursive_mutex> lock(m_lifecycle_mutex);
    return IsActive() && m_wifi_started;
}

bool Blufi::IsBleConnected() const {
    return m_ble_is_connected.load();
}

void Blufi::SetProvisioningDoneCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_provisioning_done_callback = std::move(callback);
}

const char* Blufi::GetDeviceName() const {
    return m_device_name;
}

esp_err_t Blufi::_configure_identity_scan_response() {
    std::array<uint8_t, 6> sta_mac{};
    const esp_err_t mac_ret = esp_read_mac(sta_mac.data(), ESP_MAC_WIFI_STA);
    if (mac_ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to read STA MAC for BLUFI advertising: %s",
                 esp_err_to_name(mac_ret));
        return mac_ret;
    }

    std::array<uint8_t, 16> client_uuid{};
    if (!ParseUuid(Board::GetInstance().GetUuid(), client_uuid.data())) {
        ESP_LOGE(BLUFI_TAG, "Failed to parse client UUID for BLUFI advertising");
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    m_identity_scan_response[offset++] =
        static_cast<uint8_t>(m_identity_scan_response.size() - 1);
    m_identity_scan_response[offset++] = kServiceDataAdType;
    m_identity_scan_response[offset++] = kBlufiServiceUuidLow;
    m_identity_scan_response[offset++] = kBlufiServiceUuidHigh;
    m_identity_scan_response[offset++] = kDeviceIdentityType;
    m_identity_scan_response[offset++] = kDeviceIdentityVersion;
    m_identity_scan_response[offset++] = kDeviceIdentityFields;
    memcpy(m_identity_scan_response.data() + offset, sta_mac.data(), sta_mac.size());
    offset += sta_mac.size();
    memcpy(m_identity_scan_response.data() + offset, client_uuid.data(), client_uuid.size());
    offset += client_uuid.size();
    m_identity_scan_response[offset++] = static_cast<uint8_t>(m_setup_mode.load());
    assert(offset == m_identity_scan_response.size());

#ifdef CONFIG_BT_BLUEDROID_ENABLED
    return esp_ble_gap_config_scan_rsp_data_raw(
        m_identity_scan_response.data(),
        static_cast<uint32_t>(m_identity_scan_response.size()));
#else
    const int ret = ble_gap_adv_rsp_set_data(
        m_identity_scan_response.data(), static_cast<int>(m_identity_scan_response.size()));
    return ret == 0 ? ESP_OK : ESP_FAIL;
#endif
}

void Blufi::_start_advertising() {
    if (m_deinited.load()) {
        return;
    }
    const esp_err_t ret = _configure_identity_scan_response();
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to configure BLUFI identity scan response: %s",
                 esp_err_to_name(ret));
        esp_blufi_adv_start();
        return;
    }

#ifndef CONFIG_BT_BLUEDROID_ENABLED
    esp_blufi_adv_start();
#endif
}

Blufi::Blufi()
    : m_sec(nullptr),
      m_ble_is_connected(false),
      m_sta_connected(false),
      m_sta_got_ip(false),
      m_provisioned(false),
      m_deinited(false),
      m_sta_ssid_len(0),
      m_sta_is_connecting(false) {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    m_sta_bssid_valid = false;
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    memset(&m_sta_conn_info, 0, sizeof(m_sta_conn_info));

    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(m_device_name, sizeof(m_device_name), "%s-BLUFI-%02X%02X", BOARD_NAME, mac[4],
                 mac[5]);
    } else {
        snprintf(m_device_name, sizeof(m_device_name), "%s-BLUFI", BOARD_NAME);
    }
}

Blufi::~Blufi() {
    if (m_sec) {
        _security_deinit();
    }
}

esp_err_t Blufi::StartBindMode(BleSetupMode setup_mode) {
    std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
    const BleSetupMode previous_mode = m_setup_mode.exchange(setup_mode);
    if (IsActive()) {
        if (previous_mode != setup_mode && !m_ble_is_connected.load()) {
            esp_blufi_adv_stop();
            _start_advertising();
        }
        return ESP_OK;
    }

    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        m_session_generation.fetch_add(1);
        m_connect_attempt_generation.store(0);
        m_ble_connection_generation.fetch_add(1);
        m_provisioned.store(false);
        m_finalizing.store(false);
        m_deinited.store(false);
        m_deinit_task_scheduled.store(false);
        m_ble_is_connected.store(false);
        m_sta_connected.store(false);
        m_sta_got_ip.store(false);
        m_sta_connect_failed.store(false);
        m_sta_is_connecting.store(false);
        m_scan_in_progress.store(false);
        m_scan_ble_generation.store(0);
        m_wifi_driver_state.store(WifiDriverState::kIdle);
        _invalidate_wifi_attempt();
        _clear_staged_credentials();
        memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
        m_sta_bssid_valid = false;
        memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
        m_sta_ssid_len = 0;
        m_sta_conn_info = {};
    }

    esp_err_t ret = ESP_OK;
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = _controller_init();
    if (ret != ESP_OK) {
        m_deinited.store(true);
        return ret;
    }
    m_controller_initialized = true;
#endif

    ret = _host_and_cb_init();
    if (ret != ESP_OK) {
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
        _controller_deinit();
        m_controller_initialized = false;
#endif
        m_deinited.store(true);
        return ret;
    }
    m_host_initialized = true;
    inited_.store(true);

    ESP_LOGI(BLUFI_TAG, "BLUFI bind/config broadcast started as %s", m_device_name);
    return ESP_OK;
}

esp_err_t Blufi::init() {
    std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
    const bool stack_active = IsActive();
    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        if (stack_active && m_wifi_started) {
            return ESP_OK;
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        m_session_generation.fetch_add(1);
        m_connect_attempt_generation.store(0);
        m_ble_connection_generation.fetch_add(1);
        m_provisioned.store(false);
        m_finalizing.store(false);
        m_deinited.store(false);
        m_deinit_task_scheduled.store(false);
        m_ble_is_connected.store(false);
        m_sta_connected.store(false);
        m_sta_got_ip.store(false);
        m_sta_connect_failed.store(false);
        m_sta_is_connecting.store(false);
        m_scan_in_progress.store(false);
        m_scan_ble_generation.store(0);
        m_wifi_driver_state.store(WifiDriverState::kIdle);
        _invalidate_wifi_attempt();
        _clear_staged_credentials();
        memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
        m_sta_bssid_valid = false;
        memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
        m_sta_ssid_len = 0;
        m_sta_conn_info = {};
    }

    esp_err_t ret = ESP_OK;
    if (!stack_active) {
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
        ret = _controller_init();
        if (ret != ESP_OK) {
            m_deinited.store(true);
            return ret;
        }
        m_controller_initialized = true;
#endif

        ret = _host_and_cb_init();
        if (ret != ESP_OK) {
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
            _controller_deinit();
            m_controller_initialized = false;
#endif
            m_deinited.store(true);
            return ret;
        }
        m_host_initialized = true;
        inited_.store(true);
    }

    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        ret = _ensure_provisioning_wifi_started();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "Failed to start provisioning Wi-Fi: %s", esp_err_to_name(ret));
        _deinit_locked();
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "BLUFI provisioning started as %s", m_device_name);
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
    return _deinit_locked();
}

esp_err_t Blufi::_deinit_locked() {
    if (!inited_.load()) {
        m_deinited.store(true);
        return ESP_OK;
    }

    bool expected = false;
    if (!m_deinited.compare_exchange_strong(expected, true)) {
        return ESP_OK;
    }

    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        m_session_generation.fetch_add(1);
        m_connect_attempt_generation.fetch_add(1);
        m_ble_connection_generation.fetch_add(1);
        _invalidate_wifi_attempt();
        m_sta_is_connecting.store(false);
        m_sta_connected.store(false);
        m_sta_got_ip.store(false);
        m_sta_connect_failed.store(false);
        m_ble_is_connected.store(false);
        m_finalizing.store(false);
        _clear_staged_credentials();
    }
    _stop_dedicated_wifi();

    esp_err_t result = ESP_OK;
    if (m_host_initialized) {
        const esp_err_t ret = _host_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to stop BLUFI host: %s", esp_err_to_name(ret));
            result = ret;
        }
        m_host_initialized = false;
    }
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    if (m_controller_initialized) {
        const esp_err_t ret = _controller_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to stop Bluetooth controller: %s",
                     esp_err_to_name(ret));
            if (result == ESP_OK) {
                result = ret;
            }
        }
        m_controller_initialized = false;
    }
#endif
    if (m_sec != nullptr) {
        _security_deinit();
    }
    std::function<void()> done_callback;
    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        inited_.store(false);
        m_deinit_task_scheduled.store(false);
        if (m_provisioned.load()) {
            std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
            done_callback = std::move(m_provisioning_done_callback);
        }
    }
    ESP_LOGI(BLUFI_TAG, "BLUFI provisioning stopped");
    if (done_callback) {
        done_callback();
    }
    return result;
}

void Blufi::_schedule_deinit(uint32_t session_generation) {
    if (!IsActive() || m_session_generation.load() != session_generation ||
        m_deinit_task_scheduled.exchange(true)) {
        return;
    }

    auto* context = new (std::nothrow) BlufiDeinitTaskContext{this, session_generation};
    if (context == nullptr ||
        xTaskCreate(&_deinit_task_trampoline, "blufi_deinit", 4096, context, 5, nullptr) !=
            pdPASS) {
        delete context;
        if (m_session_generation.load() == session_generation) {
            m_deinit_task_scheduled.store(false);
        }
        ESP_LOGE(BLUFI_TAG, "Failed to create BLUFI deinit task");
    }
}

void Blufi::_deinit_task_trampoline(void* context) {
    {
        std::unique_ptr<BlufiDeinitTaskContext> task_context(
            static_cast<BlufiDeinitTaskContext*>(context));
        vTaskDelay(pdMS_TO_TICKS(kDeinitGraceDelayMs));

        auto* self = task_context->blufi;
        std::lock_guard<std::mutex> operation_lock(self->m_operation_mutex);
        if (self->IsActive() && self->m_provisioned.load() &&
            self->m_session_generation.load() == task_context->session_generation) {
            self->_deinit_locked();
        } else if (self->m_session_generation.load() == task_context->session_generation) {
            self->m_deinit_task_scheduled.store(false);
        }
    }
    vTaskDelete(nullptr);
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
esp_err_t Blufi::_host_init() {
    esp_err_t ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        esp_bluedroid_deinit();
        return ret;
    }
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit() {
    esp_err_t result = esp_blufi_profile_deinit();
    esp_err_t ret = esp_bluedroid_disable();
    if (ret != ESP_OK && result == ESP_OK) {
        result = ret;
    }
    ret = esp_bluedroid_deinit();
    if (ret != ESP_OK && result == ESP_OK) {
        result = ret;
    }
    return result;
}

esp_err_t Blufi::_gap_register_callback() {
    esp_err_t rc = esp_ble_gap_register_callback(&_gap_event_callback_trampoline);
    if (rc) {
        return rc;
    }
    return esp_blufi_profile_init();
}

void Blufi::_gap_event_callback_trampoline(esp_gap_ble_cb_event_t event,
                                           esp_ble_gap_cb_param_t* param) {
    if (event == ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT) {
        if (param == nullptr ||
            param->scan_rsp_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(BLUFI_TAG, "Failed to apply BLUFI identity scan response");
        } else {
            ESP_LOGI(BLUFI_TAG, "BLUFI identity scan response configured");
        }
        if (!GetInstance().m_deinited.load()) {
            esp_blufi_adv_start();
        }
        return;
    }

    esp_blufi_gap_event_handler(event, param);
}

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = _host_init();
    if (ret) {
        
        return ret;
    }
    ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        _host_deinit();
        return ret;
    }
    ret = _gap_register_callback();
    if (ret) {
        _host_deinit();
        return ret;
    }
    return ESP_OK;
}
#endif 

#ifdef CONFIG_BT_NIMBLE_ENABLED

void ble_store_config_init();

void Blufi::_nimble_on_reset(int reason) {
    
}

void Blufi::_nimble_on_sync() { esp_blufi_profile_init(); }

void Blufi::_nimble_host_task(void* param) {
    
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t Blufi::_host_init() {
    ble_hs_cfg.reset_cb = _nimble_on_reset;
    ble_hs_cfg.sync_cb = _nimble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif

    int rc = esp_blufi_gatt_svr_init();
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_store_config_init();
    esp_blufi_btc_init();

    esp_err_t err = esp_nimble_enable(_nimble_host_task);
    if (err) {
        esp_blufi_btc_deinit();
        esp_blufi_gatt_svr_deinit();
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit(void) {
    esp_err_t result = nimble_port_stop();
    if (result == ESP_OK) {
        esp_nimble_deinit();
    }
    esp_blufi_gatt_svr_deinit();
    const esp_err_t ret = esp_blufi_profile_deinit();
    if (result == ESP_OK && ret != ESP_OK) {
        result = ret;
    }
    esp_blufi_btc_deinit();
    return result;
}

esp_err_t Blufi::_gap_register_callback(void) { return ESP_OK; }

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        
        return ret;
    }

    
    ret = _host_init();
    if (ret) {
        return ret;
    }
    return ESP_OK;
}
#endif 

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
esp_err_t Blufi::_controller_init() {
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    }
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        esp_bt_controller_deinit();
        return ret;
    }

#ifdef CONFIG_BT_NIMBLE_ENABLED
    ret = esp_nimble_init();
    if (ret) {
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }
#endif
    return ESP_OK;
}

esp_err_t Blufi::_controller_deinit() {
    esp_err_t result = esp_bt_controller_disable();
    const esp_err_t ret = esp_bt_controller_deinit();
    if (result == ESP_OK && ret != ESP_OK) {
        result = ret;
    }
    return result;
}
#endif

static int myrand(void* rng_state, unsigned char* output, size_t len) {
    esp_fill_random(output, len);
    return 0;
}

void Blufi::_security_init() {
    std::lock_guard<std::recursive_mutex> security_lock(m_security_mutex);
    if (m_sec != nullptr) {
        _security_deinit();
    }
    m_sec = new (std::nothrow) BlufiSecurity();
    if (m_sec == nullptr) {
        return;
    }
    memset(m_sec, 0, sizeof(BlufiSecurity));
    m_sec->dhm = new (std::nothrow) mbedtls_dhm_context();
    m_sec->aes = new (std::nothrow) mbedtls_aes_context();
    if (m_sec->dhm == nullptr || m_sec->aes == nullptr) {
        delete m_sec->dhm;
        delete m_sec->aes;
        delete m_sec;
        m_sec = nullptr;
        return;
    }

    mbedtls_dhm_init(m_sec->dhm);
    mbedtls_aes_init(m_sec->aes);

    memset(m_sec->iv, 0x0, sizeof(m_sec->iv));
}

void Blufi::_security_deinit() {
    std::lock_guard<std::recursive_mutex> security_lock(m_security_mutex);
    if (m_sec == nullptr)
        return;

    if (m_sec->dh_param) {
        free(m_sec->dh_param);
    }
    mbedtls_dhm_free(m_sec->dhm);
    mbedtls_aes_free(m_sec->aes);
    delete m_sec->dhm;
    delete m_sec->aes;
    delete m_sec;
    m_sec = nullptr;
}

void Blufi::_dh_negotiate_data_handler(uint8_t* data, int len, uint8_t** output_data,
                                       int* output_len, bool* need_free) {
    std::lock_guard<std::recursive_mutex> security_lock(m_security_mutex);
    if (m_sec == nullptr) {
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    if (data == nullptr || output_data == nullptr || output_len == nullptr || need_free == nullptr ||
        len < 1) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    switch (type) {
        case 0x00: {
            if (len < 3) {
                
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }

            const int dh_param_len = (data[1] << 8) | data[2];
            if (dh_param_len <= 0 || dh_param_len > kMaxDhParamLen) {
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }
            m_sec->dh_param_len = dh_param_len;
            if (m_sec->dh_param) {
                free(m_sec->dh_param);
                m_sec->dh_param = nullptr;
            }
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                m_sec->dh_param_len = 0;
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
                return;
            }
            break;
        }
        case 0x01: {
            if (m_sec->dh_param == nullptr || m_sec->dh_param_len <= 0 ||
                len - 1 != m_sec->dh_param_len) {
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            uint8_t* param = m_sec->dh_param;
            memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);
            int ret = mbedtls_dhm_read_params(m_sec->dhm, &param, &param[m_sec->dh_param_len]);
            if (ret) {
                
                btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
                return;
            }

            const int dhm_len = mbedtls_dhm_get_len(m_sec->dhm);
            if (dhm_len <= 0 || dhm_len > DH_SELF_PUB_KEY_LEN || dhm_len > SHARE_KEY_LEN) {
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }

            ret = mbedtls_dhm_make_public(m_sec->dhm, dhm_len, m_sec->self_public_key, dhm_len,
                                          myrand, NULL);
            if (ret != 0) {
                
                btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
                return;
            }
            ret = mbedtls_dhm_calc_secret(m_sec->dhm, m_sec->share_key, SHARE_KEY_LEN,
                                          &m_sec->share_len, myrand, NULL);
            if (ret != 0) {
                
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }

            ret = mbedtls_md5(m_sec->share_key, m_sec->share_len, m_sec->psk);
            if (ret != 0) {
                
                btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
                return;
            }
            ret = mbedtls_aes_setkey_enc(m_sec->aes, m_sec->psk, PSK_LEN * 8);
            if (ret != 0) {
                
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }
            *output_data = m_sec->self_public_key;
            *output_len = dhm_len;
            *need_free = false;
            

            free(m_sec->dh_param);
            m_sec->dh_param = nullptr;
            m_sec->dh_param_len = 0;
            break;
        }
        default:
            btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
    }
}

int Blufi::_aes_encrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    std::lock_guard<std::recursive_mutex> security_lock(m_security_mutex);
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len <= 0) {
        
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);

    if (ret == 0) {
        return crypt_len;
    } else {
        
        return ret;
    }
}

int Blufi::_aes_decrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    std::lock_guard<std::recursive_mutex> security_lock(m_security_mutex);
    if (!m_sec || !m_sec->aes || !crypt_data || crypt_len < 0) {
        
        return -ESP_ERR_INVALID_ARG;
    }

    size_t iv_offset = 0;
    uint8_t iv0[16];
    memcpy(iv0, m_sec->iv, 16);
    iv0[0] = iv8;
    int ret = mbedtls_aes_crypt_cfb128(m_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0,
                                       crypt_data, crypt_data);
    if (ret != 0) {
        
        return ret;
    } else {
        return crypt_len;
    }
}

uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len) {
    return esp_crc16_be(0, data, len);
}

void Blufi::_clear_staged_credentials() {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    m_received_ssid_len = 0;
    m_received_password_len = 0;
}

void Blufi::_invalidate_wifi_attempt() {
    std::lock_guard<std::mutex> attempt_lock(m_attempt_mutex);
    m_attempt = {};
}

void Blufi::_cancel_wifi_attempt(bool disconnect_wifi) {
    m_connect_attempt_generation.fetch_add(1);
    _invalidate_wifi_attempt();
    m_sta_is_connecting.store(false);
    m_sta_connected.store(false);
    m_sta_got_ip.store(false);
    m_sta_connect_failed.store(false);
    if (!m_provisioned.load()) {
        m_finalizing.store(false);
    }
    _clear_staged_credentials();
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    m_sta_bssid_valid = false;
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    m_sta_ssid_len = 0;
    m_sta_conn_info = {};

    if (!disconnect_wifi) {
        return;
    }
    const esp_err_t ret = _request_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(BLUFI_TAG, "Failed to cancel provisioning connection: %s",
                 esp_err_to_name(ret));
    }
}

esp_err_t Blufi::_ensure_provisioning_wifi_started() {
    if (m_wifi_started) {
        return ESP_OK;
    }

    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
        return ESP_FAIL;
    }
    if (wifi_manager.IsConfigMode()) {
        wifi_manager.StopConfigAp();
    }
    wifi_manager.StopStation();

    return _start_dedicated_wifi();
}

bool Blufi::_is_attempt_current(uint32_t session_generation, uint32_t attempt_generation,
                                uint32_t ble_generation) const {
    return IsActive() && m_session_generation.load() == session_generation &&
           m_connect_attempt_generation.load() == attempt_generation &&
           m_ble_connection_generation.load() == ble_generation;
}

esp_err_t Blufi::_drain_default_event_loop(TickType_t timeout_ticks) {
    std::lock_guard<std::mutex> drain_lock(m_event_drain_mutex);
    if (m_event_drain_semaphore == nullptr || m_drain_event_instance == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(m_event_drain_semaphore, 0);
    esp_err_t ret = esp_event_post(BLUFI_INTERNAL_EVENT, kEventDrainId, nullptr, 0, timeout_ticks);
    if (ret != ESP_OK) {
        return ret;
    }
    return xSemaphoreTake(m_event_drain_semaphore, timeout_ticks) == pdTRUE ? ESP_OK
                                                                           : ESP_ERR_TIMEOUT;
}

esp_err_t Blufi::_send_wifi_conn_report(esp_blufi_sta_conn_state_t state,
                                        esp_blufi_extra_info_t* extra_info) {
    const esp_err_t ret =
        esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, state, 0, extra_info);
    if (ret != ESP_OK) {
        ESP_LOGW(BLUFI_TAG, "Failed to queue BLUFI Wi-Fi report (state %d): %s",
                 static_cast<int>(state), esp_err_to_name(ret));
    }
    return ret;
}

void Blufi::start_wifi_scan() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
    if (!IsActive() || !m_ble_is_connected.load() || m_finalizing.load() ||
        m_sta_is_connecting.load() || m_scan_in_progress.exchange(true)) {
        return;
    }
    esp_err_t ret = _ensure_provisioning_wifi_started();
    if (ret != ESP_OK) {
        m_scan_in_progress.store(false);
        ESP_LOGE(BLUFI_TAG, "Failed to prepare Wi-Fi scan: %s", esp_err_to_name(ret));
        if (m_ble_is_connected.load()) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        return;
    }
    m_scan_ble_generation.store(m_ble_connection_generation.load());

    wifi_scan_config_t scan_config = {
        .ssid = nullptr,
        .bssid = nullptr,
        .channel = 0,
        .show_hidden = false,
    };
    ret = esp_wifi_scan_start(&scan_config, false);
    if (ret != ESP_OK) {
        m_scan_in_progress.store(false);
        ESP_LOGE(BLUFI_TAG, "Failed to start Wi-Fi scan: %s", esp_err_to_name(ret));
        if (m_ble_is_connected.load()) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
    }
}

void Blufi::_send_wifi_list(uint32_t ble_generation) {
    if (!IsActive() || !m_ble_is_connected.load() ||
        m_ble_connection_generation.load() != ble_generation) {
        return;
    }

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    {
        std::lock_guard<std::mutex> scan_lock(m_scan_mutex);
        blufi_ap_list.reserve(m_ap_records.size());
        for (const auto& ap : m_ap_records) {
            esp_blufi_ap_record_t blufi_ap{};
            memcpy(blufi_ap.ssid, ap.ssid, sizeof(blufi_ap.ssid));
            blufi_ap.rssi = ap.rssi;
            blufi_ap_list.push_back(blufi_ap);
        }
        m_ap_records.clear();
    }

    if (IsActive() && m_ble_is_connected.load() &&
        m_ble_connection_generation.load() == ble_generation) {
        esp_blufi_send_wifi_list(static_cast<int>(blufi_ap_list.size()),
                                 blufi_ap_list.empty() ? nullptr : blufi_ap_list.data());
    }
}

esp_err_t Blufi::_start_dedicated_wifi() {
    if (m_wifi_started) {
        return ESP_OK;
    }

    esp_netif_t* station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == nullptr) {
        return ESP_FAIL;
    }
    m_station_netif.store(station_netif);

    if (m_event_drain_semaphore == nullptr) {
        m_event_drain_semaphore = xSemaphoreCreateBinary();
        if (m_event_drain_semaphore == nullptr) {
            _stop_dedicated_wifi();
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = ESP_OK;
    if (m_drain_event_instance == nullptr) {
        ret = esp_event_handler_instance_register(BLUFI_INTERNAL_EVENT, kEventDrainId,
                                                  &Blufi::_event_drain_handler, this,
                                                  &m_drain_event_instance);
    }
    if (ret == ESP_OK) {
        ret = _drain_default_event_loop(pdMS_TO_TICKS(kEventDrainTimeoutMs));
    }
    if (ret == ESP_OK && m_scan_event_instance == nullptr) {
        ret = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &Blufi::_wifi_scan_event_handler, this,
            &m_scan_event_instance);
    }
    if (ret == ESP_OK && m_connected_event_instance == nullptr) {
        ret = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &Blufi::_wifi_connected_event_handler, this,
            &m_connected_event_instance);
    }
    if (ret == ESP_OK && m_disconnect_event_instance == nullptr) {
        ret = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &Blufi::_wifi_disconnect_event_handler, this,
            &m_disconnect_event_instance);
    }
    if (ret == ESP_OK && m_ip_event_instance == nullptr) {
        ret = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &Blufi::_ip_event_handler, this, &m_ip_event_instance);
    }
    if (ret == ESP_OK) {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (ret == ESP_OK) {
        ret = esp_wifi_start();
    }
    if (ret != ESP_OK) {
        _stop_dedicated_wifi();
        return ret;
    }
    m_wifi_started = true;
    return ESP_OK;
}

void Blufi::_stop_dedicated_wifi() {
    _invalidate_wifi_attempt();

    if (m_scan_in_progress.exchange(false)) {
        const uint32_t scan_generation = m_wifi_scan_done_generation.load();
        const esp_err_t ret = esp_wifi_scan_stop();
        if (ret == ESP_OK) {
            for (int waited_ms = 0;
                 waited_ms < kDisconnectDrainTimeoutMs &&
                 m_wifi_scan_done_generation.load() == scan_generation;
                 waited_ms += kDisconnectDrainPollMs) {
                vTaskDelay(pdMS_TO_TICKS(kDisconnectDrainPollMs));
            }
        } else if (ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(BLUFI_TAG, "Failed to stop provisioning scan: %s", esp_err_to_name(ret));
        }
    }
    if (m_wifi_started) {
        const uint32_t disconnect_generation = m_wifi_disconnect_generation.load();
        const esp_err_t disconnect_ret = _wait_for_disconnect(disconnect_generation);
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(BLUFI_TAG, "Failed to drain provisioning disconnect: %s",
                     esp_err_to_name(disconnect_ret));
        }
        const esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK) {
            ESP_LOGW(BLUFI_TAG, "Failed to stop provisioning Wi-Fi: %s", esp_err_to_name(ret));
        }
        m_wifi_started = false;
    }
    m_wifi_driver_state.store(WifiDriverState::kIdle);

    const esp_err_t drain_ret = _drain_default_event_loop(pdMS_TO_TICKS(kEventDrainTimeoutMs));
    if (drain_ret != ESP_OK && drain_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(BLUFI_TAG, "Failed to drain provisioning events: %s",
                 esp_err_to_name(drain_ret));
    }

    auto unregister_handler = [](esp_event_base_t event_base, int32_t event_id,
                                 esp_event_handler_instance_t& instance, const char* name) {
        if (instance == nullptr) {
            return;
        }
        const esp_err_t ret =
            esp_event_handler_instance_unregister(event_base, event_id, instance);
        if (ret == ESP_OK) {
            instance = nullptr;
        } else {
            ESP_LOGW(BLUFI_TAG, "Failed to unregister %s handler: %s", name,
                     esp_err_to_name(ret));
        }
    };

    unregister_handler(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, m_scan_event_instance, "scan");
    unregister_handler(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, m_connected_event_instance,
                       "connected");
    unregister_handler(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, m_disconnect_event_instance,
                       "disconnect");
    unregister_handler(IP_EVENT, IP_EVENT_STA_GOT_IP, m_ip_event_instance, "got-ip");

    {
        std::lock_guard<std::mutex> drain_lock(m_event_drain_mutex);
        unregister_handler(BLUFI_INTERNAL_EVENT, kEventDrainId, m_drain_event_instance, "drain");
        if (m_drain_event_instance == nullptr && m_event_drain_semaphore != nullptr) {
            vSemaphoreDelete(m_event_drain_semaphore);
            m_event_drain_semaphore = nullptr;
        }
    }

    esp_netif_t* station_netif = m_station_netif.exchange(nullptr);
    if (station_netif != nullptr) {
        esp_netif_destroy_default_wifi(station_netif);
    }
    std::lock_guard<std::mutex> scan_lock(m_scan_mutex);
    m_ap_records.clear();
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE) {
        return;
    }
    self->m_wifi_scan_done_generation.fetch_add(1);

    {
        std::lock_guard<std::mutex> scan_lock(self->m_scan_mutex);
        uint16_t ap_num = 0;
        self->m_ap_records.clear();
        if (esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK && ap_num > 0) {
            self->m_ap_records.resize(ap_num);
            if (esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data()) == ESP_OK) {
                self->m_ap_records.resize(ap_num);
            } else {
                self->m_ap_records.clear();
            }
        }
    }
    const uint32_t ble_generation = self->m_scan_ble_generation.load();
    self->m_scan_in_progress.store(false);
    self->_send_wifi_list(ble_generation);
}

void Blufi::_wifi_connected_event_handler(void* arg, esp_event_base_t event_base,
                                          int32_t event_id, void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_STA_CONNECTED ||
        event_data == nullptr) {
        return;
    }
    const auto* event = static_cast<const wifi_event_sta_connected_t*>(event_data);

    std::lock_guard<std::mutex> attempt_lock(self->m_attempt_mutex);
    auto& attempt = self->m_attempt;
    if (!attempt.accepting_events || attempt.failed ||
        !self->_is_attempt_current(attempt.session_generation, attempt.attempt_generation,
                                   attempt.ble_generation) ||
        event->ssid_len != attempt.target_ssid_len ||
        memcmp(event->ssid, attempt.target_ssid, attempt.target_ssid_len) != 0 ||
        (attempt.desired_bssid_set &&
         memcmp(event->bssid, attempt.desired_bssid, sizeof(attempt.desired_bssid)) != 0)) {
        return;
    }

    attempt.associated = true;
    attempt.actual_bssid_set = true;
    memcpy(attempt.actual_bssid, event->bssid, sizeof(attempt.actual_bssid));
}

void Blufi::_wifi_disconnect_event_handler(void* arg, esp_event_base_t event_base,
                                           int32_t event_id, void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }
    self->m_wifi_driver_state.store(WifiDriverState::kIdle);
    self->m_wifi_disconnect_generation.fetch_add(1);
    if (event_data == nullptr) {
        return;
    }
    const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(event_data);

    std::lock_guard<std::mutex> attempt_lock(self->m_attempt_mutex);
    auto& attempt = self->m_attempt;
    if (!attempt.accepting_events ||
        !self->_is_attempt_current(attempt.session_generation, attempt.attempt_generation,
                                   attempt.ble_generation) ||
        event->ssid_len != attempt.target_ssid_len ||
        memcmp(event->ssid, attempt.target_ssid, attempt.target_ssid_len) != 0) {
        return;
    }
    if ((attempt.associated && attempt.actual_bssid_set &&
         memcmp(event->bssid, attempt.actual_bssid, sizeof(attempt.actual_bssid)) != 0) ||
        (!attempt.associated && attempt.desired_bssid_set &&
         memcmp(event->bssid, attempt.desired_bssid, sizeof(attempt.desired_bssid)) != 0)) {
        return;
    }

    attempt.failed = true;
    attempt.got_ip = false;
    attempt.accepting_events = false;
    self->m_sta_connected.store(false);
    self->m_sta_got_ip.store(false);
    self->m_sta_connect_failed.store(true);
}

void Blufi::_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                              void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP || event_data == nullptr) {
        return;
    }
    const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
    if (event->esp_netif != self->m_station_netif.load()) {
        return;
    }

    std::lock_guard<std::mutex> attempt_lock(self->m_attempt_mutex);
    auto& attempt = self->m_attempt;
    if (!attempt.accepting_events || !attempt.associated || attempt.failed ||
        !self->_is_attempt_current(attempt.session_generation, attempt.attempt_generation,
                                   attempt.ble_generation)) {
        return;
    }
    attempt.got_ip = true;
    self->m_sta_connected.store(true);
    self->m_sta_got_ip.store(true);
}

void Blufi::_event_drain_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                 void* event_data) {
    auto* self = static_cast<Blufi*>(arg);
    if (event_base == BLUFI_INTERNAL_EVENT && event_id == kEventDrainId &&
        self->m_event_drain_semaphore != nullptr) {
        xSemaphoreGive(self->m_event_drain_semaphore);
    }
}

esp_err_t Blufi::_request_wifi_disconnect() {
    for (;;) {
        auto expected = WifiDriverState::kConnectActive;
        if (m_wifi_driver_state.compare_exchange_weak(expected,
                                                      WifiDriverState::kDisconnectPending)) {
            break;
        }
        if (expected == WifiDriverState::kIdle ||
            expected == WifiDriverState::kDisconnectPending) {
            return ESP_OK;
        }
    }

    const esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK) {
        const WifiDriverState fallback =
            (ret == ESP_ERR_WIFI_NOT_CONNECT || ret == ESP_ERR_WIFI_NOT_STARTED)
                ? WifiDriverState::kIdle
                : WifiDriverState::kConnectActive;
        auto expected = WifiDriverState::kDisconnectPending;
        m_wifi_driver_state.compare_exchange_strong(expected, fallback);
    }
    return ret;
}

esp_err_t Blufi::_wait_for_disconnect(uint32_t previous_disconnect_generation) {
    const esp_err_t ret = _request_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(BLUFI_TAG, "Failed to cancel provisioning connection: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    if (m_wifi_driver_state.load() != WifiDriverState::kDisconnectPending) {
        return ESP_OK;
    }

    for (int waited_ms = 0; waited_ms < kDisconnectDrainTimeoutMs;
         waited_ms += kDisconnectDrainPollMs) {
        if (m_wifi_driver_state.load() != WifiDriverState::kDisconnectPending ||
            m_wifi_disconnect_generation.load() != previous_disconnect_generation) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(kDisconnectDrainPollMs));
    }
    ESP_LOGW(BLUFI_TAG, "Timed out draining provisioning disconnect event");
    return ESP_ERR_TIMEOUT;
}

void Blufi::_run_wifi_connect_task(uint32_t session_generation, uint32_t attempt_generation,
                                   uint32_t ble_generation, wifi_config_t wifi_config,
                                   std::string ssid, std::string password) {
    esp_err_t ret = ESP_OK;
    {
        std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
        {
            std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
            if (!_is_attempt_current(session_generation, attempt_generation, ble_generation) ||
                !m_ble_is_connected.load() || m_finalizing.load()) {
                return;
            }
            _invalidate_wifi_attempt();
        }

        ret = _wait_for_disconnect(m_wifi_disconnect_generation.load());
        if (ret == ESP_OK) {
            ret = _drain_default_event_loop(pdMS_TO_TICKS(kEventDrainTimeoutMs));
        }
        if (ret != ESP_OK) {
            ESP_LOGW(BLUFI_TAG, "Failed to drain old Wi-Fi events before connect: %s",
                     esp_err_to_name(ret));
        }

        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        if (ret == ESP_OK &&
            _is_attempt_current(session_generation, attempt_generation, ble_generation) &&
            m_ble_is_connected.load() && !m_finalizing.load()) {
            ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if (ret == ESP_OK) {
                {
                    std::lock_guard<std::mutex> attempt_lock(m_attempt_mutex);
                    m_attempt = {};
                    m_attempt.session_generation = session_generation;
                    m_attempt.attempt_generation = attempt_generation;
                    m_attempt.ble_generation = ble_generation;
                    m_attempt.target_ssid_len = ssid.size();
                    memcpy(m_attempt.target_ssid, ssid.data(), ssid.size());
                    m_attempt.desired_bssid_set = wifi_config.sta.bssid_set;
                    if (wifi_config.sta.bssid_set) {
                        memcpy(m_attempt.desired_bssid, wifi_config.sta.bssid,
                               sizeof(m_attempt.desired_bssid));
                    }
                    m_attempt.accepting_events = true;
                }
                m_wifi_driver_state.store(WifiDriverState::kConnectActive);
                ret = esp_wifi_connect();
                if (ret != ESP_OK) {
                    auto expected = WifiDriverState::kConnectActive;
                    m_wifi_driver_state.compare_exchange_strong(expected, WifiDriverState::kIdle);
                    std::lock_guard<std::mutex> attempt_lock(m_attempt_mutex);
                    if (m_attempt.session_generation == session_generation &&
                        m_attempt.attempt_generation == attempt_generation &&
                        m_attempt.ble_generation == ble_generation) {
                        m_attempt.accepting_events = false;
                        m_attempt.failed = true;
                    }
                }
            }
        } else if (ret == ESP_OK) {
            return;
        }
    }

    if (ret != ESP_OK) {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        if (_is_attempt_current(session_generation, attempt_generation, ble_generation)) {
            ESP_LOGE(BLUFI_TAG, "Failed to start credential validation: %s",
                     esp_err_to_name(ret));
            m_sta_connect_failed.store(true);
        }
    }

    int waited_ms = 0;
    while (ret == ESP_OK && waited_ms < kConnectTimeoutMs && !m_sta_got_ip.load() &&
           !m_sta_connect_failed.load() &&
           _is_attempt_current(session_generation, attempt_generation, ble_generation)) {
        vTaskDelay(pdMS_TO_TICKS(kConnectPollMs));
        waited_ms += kConnectPollMs;
    }

    if (_is_attempt_current(session_generation, attempt_generation, ble_generation) &&
        m_sta_got_ip.load()) {
        const esp_err_t drain_ret =
            _drain_default_event_loop(pdMS_TO_TICKS(kEventDrainTimeoutMs));
        if (drain_ret != ESP_OK) {
            std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
            if (_is_attempt_current(session_generation, attempt_generation, ble_generation)) {
                ESP_LOGW(BLUFI_TAG, "Failed to drain final Wi-Fi events: %s",
                         esp_err_to_name(drain_ret));
                m_sta_connect_failed.store(true);
            }
        }
    }

    bool success = false;
    bool disconnect_after_failure = false;
    {
        std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
        std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
        const bool attempt_is_current =
            _is_attempt_current(session_generation, attempt_generation, ble_generation) &&
            m_ble_is_connected.load() && !m_provisioned.load();

        if (attempt_is_current) {
            wifi_ap_record_t ap_info{};
            const bool ap_info_valid = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
            const size_t ap_ssid_len =
                ap_info_valid
                    ? strnlen(reinterpret_cast<const char*>(ap_info.ssid), sizeof(ap_info.ssid))
                    : 0;

            std::lock_guard<std::mutex> attempt_lock(m_attempt_mutex);
            const bool connection_is_stable =
                m_attempt.accepting_events && m_attempt.associated && m_attempt.got_ip &&
                !m_attempt.failed && !m_sta_connect_failed.load() && ap_info_valid &&
                ap_ssid_len == ssid.size() &&
                memcmp(ap_info.ssid, ssid.data(), ssid.size()) == 0 &&
                m_attempt.actual_bssid_set &&
                memcmp(ap_info.bssid, m_attempt.actual_bssid,
                       sizeof(m_attempt.actual_bssid)) == 0;

            if (connection_is_stable && !m_finalizing.exchange(true)) {
                esp_blufi_extra_info_t info{};
                memcpy(info.sta_bssid, ap_info.bssid, sizeof(info.sta_bssid));
                info.sta_bssid_set = true;
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;

                if (_send_wifi_conn_report(ESP_BLUFI_STA_CONN_SUCCESS, &info) == ESP_OK) {
                    memcpy(m_sta_bssid, ap_info.bssid, sizeof(m_sta_bssid));
                    m_sta_bssid_valid = true;
                    m_sta_connected.store(true);
                    m_sta_is_connecting.store(false);
                    SsidManager::GetInstance().AddSsid(ssid, password);
                    m_provisioned.store(true);
                    success = true;
                } else {
                    m_finalizing.store(false);
                    m_attempt.accepting_events = false;
                    m_attempt.failed = true;
                    m_sta_is_connecting.store(false);
                    m_sta_connected.store(false);
                    m_sta_got_ip.store(false);
                    m_sta_connect_failed.store(true);
                    disconnect_after_failure = true;
                }
            } else {
                m_attempt.accepting_events = false;
                m_sta_is_connecting.store(false);
                m_sta_connected.store(false);
                m_sta_got_ip.store(false);
                m_sta_connect_failed.store(false);
                esp_blufi_extra_info_t info{};
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                _send_wifi_conn_report(ESP_BLUFI_STA_CONN_FAIL, &info);
                disconnect_after_failure = true;
            }
        }
    }

    if (!success) {
        if (disconnect_after_failure) {
            std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
            if (_is_attempt_current(session_generation, attempt_generation, ble_generation)) {
                _wait_for_disconnect(m_wifi_disconnect_generation.load());
            }
        }
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(kFinalReportDelayMs));
    bool disconnect_ble = false;
    {
        std::lock_guard<std::mutex> operation_lock(m_operation_mutex);
        {
            std::lock_guard<std::recursive_mutex> lifecycle_lock(m_lifecycle_mutex);
            if (_is_attempt_current(session_generation, attempt_generation, ble_generation) &&
                m_ble_is_connected.load() && m_provisioned.load() && m_finalizing.load()) {
                disconnect_ble = true;
                _schedule_deinit(session_generation);
            }
        }
        if (disconnect_ble) {
            esp_blufi_disconnect();
        }
    }
}

void Blufi::_wifi_connect_task_trampoline(void* context) {
    {
        std::unique_ptr<WifiConnectTaskContext> task_context(
            static_cast<WifiConnectTaskContext*>(context));
        task_context->blufi->_run_wifi_connect_task(
            task_context->session_generation, task_context->attempt_generation,
            task_context->ble_generation, task_context->wifi_config, std::move(task_context->ssid),
            std::move(task_context->password));
    }
    vTaskDelete(nullptr);
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    std::lock_guard<std::recursive_mutex> lock(m_lifecycle_mutex);
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            if (m_deinited.load()) {
                break;
            }
#ifdef CONFIG_BT_BLUEDROID_ENABLED
            esp_ble_gap_set_device_name(m_device_name);
#else
            ble_svc_gap_device_name_set(m_device_name);
#endif
            _start_advertising();
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            if (!IsActive()) {
                break;
            }
            m_ble_connection_generation.fetch_add(1);
            _cancel_wifi_attempt(m_wifi_started);
            m_ble_is_connected.store(true);
            esp_blufi_adv_stop();
            if (m_provisioned.load()) {
                esp_blufi_disconnect();
                _schedule_deinit(m_session_generation.load());
                break;
            }
            _security_init();
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            m_ble_is_connected.store(false);
            m_ble_connection_generation.fetch_add(1);
            if (IsActive()) {
                _cancel_wifi_attempt(m_wifi_started);
            }
            if (m_scan_in_progress.exchange(false)) {
                esp_wifi_scan_stop();
            }
            {
                std::lock_guard<std::mutex> scan_lock(m_scan_mutex);
                m_ap_records.clear();
            }
            _security_deinit();
            if (!IsActive()) {
                break;
            }
            if (!m_provisioned.load()) {
                _start_advertising();
            } else {
                esp_blufi_adv_stop();
                _schedule_deinit(m_session_generation.load());
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE: {
            if (param == nullptr) {
                break;
            }
            if (param->wifi_mode.op_mode != WIFI_MODE_STA) {
                ESP_LOGW(BLUFI_TAG, "Ignoring unsupported provisioning mode %d",
                         param->wifi_mode.op_mode);
                esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
                break;
            }
            const esp_err_t ret = _ensure_provisioning_wifi_started();
            if (ret != ESP_OK) {
                ESP_LOGE(BLUFI_TAG, "Failed to enter BLUFI provisioning mode: %s",
                         esp_err_to_name(ret));
                esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            if (!IsActive() || !m_ble_is_connected.load() || m_received_ssid_len == 0 ||
                m_provisioned.load()) {
                esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
                break;
            }
            const esp_err_t prepare_ret = _ensure_provisioning_wifi_started();
            if (prepare_ret != ESP_OK) {
                ESP_LOGE(BLUFI_TAG, "Failed to prepare BLUFI provisioning connect: %s",
                         esp_err_to_name(prepare_ret));
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0,
                                                &m_sta_conn_info);
                break;
            }
            if (m_sta_is_connecting.exchange(true)) {
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONNECTING, 0,
                                                &m_sta_conn_info);
                break;
            }

            const std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid),
                                   m_received_ssid_len);
            const std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password),
                                       m_received_password_len);
            const uint32_t session_generation = m_session_generation.load();
            const uint32_t attempt_generation = m_connect_attempt_generation.fetch_add(1) + 1;
            const uint32_t ble_generation = m_ble_connection_generation.load();

            if (m_scan_in_progress.exchange(false)) {
                esp_wifi_scan_stop();
            }
            {
                std::lock_guard<std::mutex> scan_lock(m_scan_mutex);
                m_ap_records.clear();
            }

            m_sta_ssid_len = static_cast<int>(ssid.size());
            memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
            memcpy(m_sta_ssid, ssid.data(), ssid.size());
            memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
            m_sta_bssid_valid = false;
            m_sta_connected.store(false);
            m_sta_got_ip.store(false);
            m_sta_connect_failed.store(false);
            m_sta_conn_info = {};
            m_sta_conn_info.sta_ssid = m_sta_ssid;
            m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;

            auto* context = new (std::nothrow) WifiConnectTaskContext{
                this, session_generation, attempt_generation, ble_generation, m_sta_config, ssid,
                password};
            if (context == nullptr ||
                xTaskCreate(&_wifi_connect_task_trampoline, "blufi_wifi_conn", 4096, context, 5,
                            nullptr) != pdPASS) {
                delete context;
                m_sta_is_connecting.store(false);
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0,
                                                &m_sta_conn_info);
                break;
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            if (m_provisioned.load()) {
                _schedule_deinit(m_session_generation.load());
                break;
            }
            _cancel_wifi_attempt(m_wifi_started);
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            if (m_provisioned.load() && m_sta_got_ip.load()) {
                esp_blufi_extra_info_t info{};
                if (m_sta_bssid_valid) {
                    memcpy(info.sta_bssid, m_sta_bssid, sizeof(m_sta_bssid));
                    info.sta_bssid_set = true;
                }
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_SUCCESS, 0,
                                                &info);
            } else if (m_sta_is_connecting.load()) {
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONNECTING, 0,
                                                &m_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(WIFI_MODE_STA, ESP_BLUFI_STA_CONN_FAIL, 0,
                                                &m_sta_conn_info);
            }
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            if (!m_ble_is_connected.load() || m_sta_is_connecting.load() ||
                m_provisioned.load() || param == nullptr) {
                esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
                break;
            }
            memcpy(m_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            m_sta_config.sta.bssid_set = true;
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID: {
            if (!m_ble_is_connected.load() || m_sta_is_connecting.load() ||
                m_provisioned.load()) {
                esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
                break;
            }
            if (param == nullptr || param->sta_ssid.ssid == nullptr ||
                param->sta_ssid.ssid_len <= 0 ||
                param->sta_ssid.ssid_len >
                    static_cast<int>(sizeof(m_sta_config.sta.ssid))) {
                esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
                break;
            }
            memset(m_sta_config.sta.ssid, 0, sizeof(m_sta_config.sta.ssid));
            memcpy(m_sta_config.sta.ssid, param->sta_ssid.ssid, param->sta_ssid.ssid_len);
            m_received_ssid_len = param->sta_ssid.ssid_len;
            memset(m_sta_config.sta.password, 0, sizeof(m_sta_config.sta.password));
            m_received_password_len = 0;
            m_sta_config.sta.bssid_set = false;
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
            if (!m_ble_is_connected.load() || m_sta_is_connecting.load() ||
                m_provisioned.load()) {
                esp_blufi_send_error_info(ESP_BLUFI_MSG_STATE_ERROR);
                break;
            }
            if (param == nullptr || param->sta_passwd.passwd_len < 0 ||
                (param->sta_passwd.passwd_len > 0 && param->sta_passwd.passwd == nullptr) ||
                param->sta_passwd.passwd_len >
                    static_cast<int>(sizeof(m_sta_config.sta.password))) {
                esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
                break;
            }
            memset(m_sta_config.sta.password, 0, sizeof(m_sta_config.sta.password));
            if (param->sta_passwd.passwd_len > 0) {
                memcpy(m_sta_config.sta.password, param->sta_passwd.passwd,
                       param->sta_passwd.passwd_len);
            }
            m_received_password_len = param->sta_passwd.passwd_len;
            break;
        }
        case ESP_BLUFI_EVENT_GET_WIFI_LIST:
            start_wifi_scan();
            break;
        case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
            if (m_ble_is_connected.load()) {
                esp_blufi_disconnect();
            }
            break;
        case ESP_BLUFI_EVENT_REPORT_ERROR:
            if (param != nullptr) {
                esp_blufi_send_error_info(param->report_error.state);
            }
            break;
        default:
            break;
    }
}

void Blufi::_event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance()._handle_event(event, param);
}

void Blufi::_negotiate_data_handler_trampoline(uint8_t* data, int len, uint8_t** output_data,
                                               int* output_len, bool* need_free) {
    GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);
}

int Blufi::_encrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);
}

int Blufi::_decrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);
}

uint16_t Blufi::_checksum_func_trampoline(uint8_t iv8, uint8_t* data, int len) {
    return _crc_checksum(iv8, data, len);
}
