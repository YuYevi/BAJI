/**
 * @file ota.cc
 * @brief OTA（空中升级）模块实现文件
 * 
 * 该文件实现了 Ota 类，负责设备的远程固件升级功能，包括：
 * - 版本检查
 * - 固件下载与安装
 * - 设备激活验证
 * - MQTT/WebSocket 配置更新
 * - 服务器时间同步
 */

#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>

#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <utility>

#define TAG "Ota"

/**
 * @brief Ota 类构造函数
 * 
 * 从 ESP32 eFuse 用户数据区域读取设备序列号（如果存在）。
 * 序列号用于设备激活验证。
 */
Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

/**
 * @brief Ota 类析构函数
 */
Ota::~Ota() {
}

/**
 * @brief 获取版本检查 URL
 * 
 * 从设置中读取 OTA URL，如果未配置则使用默认配置值。
 * 
 * @return OTA 版本检查服务器地址
 */
std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

/**
 * @brief 配置 HTTP 客户端
 * 
 * 设置 HTTP 请求头，包括设备标识、激活版本、用户代理等信息。
 * 
 * @return 配置好的 HTTP 客户端实例
 */
std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();

    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
    }
    
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/**
 * @brief 检查服务器版本更新
 * 
 * 向服务器发送版本检查请求，解析响应获取：
 * - 激活信息（消息、激活码、挑战码）
 * - MQTT 配置
 * - WebSocket 配置
 * - 服务器时间
 * - 固件更新信息（版本号、下载地址、是否强制更新）
 * 
 * @return ESP_OK 成功，其他错误码表示失败
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    current_version_ = app_desc->version;

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "OTA URL is too short: %s", url.c_str());
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Failed to open HTTP connection: %d", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 100000) {
        ESP_LOGE(TAG, "OTA server returned code=%d", code->valueint);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* config_root = root;
    cJSON* data_node = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data_node)) {
        config_root = data_node;
    }

    // 解析激活信息
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON* activation = cJSON_GetObjectItem(config_root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    // 解析 MQTT 配置
    has_mqtt_config_ = false;
    cJSON* mqtt = cJSON_GetObjectItem(config_root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            } else if (cJSON_IsBool(item)) {
                if (settings.GetBool(item->string) != cJSON_IsTrue(item)) {
                    settings.SetBool(item->string, cJSON_IsTrue(item));
                }
            }
        }
        has_mqtt_config_ = true;
    }

    // 解析 WebSocket 配置
    has_websocket_config_ = false;
    cJSON* websocket = cJSON_GetObjectItem(config_root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            } else if (cJSON_IsBool(item)) {
                if (settings.GetBool(item->string) != cJSON_IsTrue(item)) {
                    settings.SetBool(item->string, cJSON_IsTrue(item));
                }
            }
        }
        has_websocket_config_ = true;
    }

    // 解析 MQTT 控制通道配置
    has_mqtt_control_config_ = false;
    cJSON* mqtt_control = cJSON_GetObjectItem(config_root, "mqtt_control");
    if (cJSON_IsObject(mqtt_control)) {
        Settings settings("mqtt_ctrl", true);
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, mqtt_control) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            } else if (cJSON_IsBool(item)) {
                if (settings.GetBool(item->string) != cJSON_IsTrue(item)) {
                    settings.SetBool(item->string, cJSON_IsTrue(item));
                }
            }
        }
        has_mqtt_control_config_ = true;
    }

    // 解析服务器时间
    has_server_time_ = false;
    cJSON* server_time = cJSON_GetObjectItem(config_root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON* timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");

        if (cJSON_IsNumber(timestamp)) {
            struct timeval tv;
            double ts = timestamp->valuedouble;

            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000);
            }

            tv.tv_sec = (time_t)(ts / 1000);
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    }

    // 解析固件更新信息
    has_new_version_ = false;
    cJSON* firmware = cJSON_GetObjectItem(config_root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON* version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        
        cJSON* firmware_url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(firmware_url)) {
            firmware_url_ = firmware_url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(firmware_url)) {
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            
            cJSON* force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 标记当前版本为有效
 * 
 * 在成功升级并验证后调用，取消回滚标记。
 * 如果当前运行的是 factory 分区，则不需要标记。
 */
void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get partition state");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGD(TAG, "Marking current app as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

/**
 * @brief 执行固件升级
 * 
 * 从指定 URL 下载固件，写入 OTA 分区，并设置为启动分区。
 * 
 * @param[in] firmware_url 固件下载地址
 * @param[in] callback     进度回调函数（进度百分比，下载速度字节/秒）
 * @return true            升级成功
 * @return false           升级失败
 */
Ota::UpgradeResult Ota::UpgradeWithResult(const std::string& firmware_url,
                                          std::function<void(int progress, size_t speed)> callback,
                                          std::function<bool()> cancel_requested) {
    esp_ota_handle_t update_handle = 0;
    bool ota_started = false;
    bool http_opened = false;
    char* buffer = nullptr;

    auto is_cancelled = [&cancel_requested]() {
        return cancel_requested != nullptr && cancel_requested();
    };

    auto finish_before_ota_end = [&](UpgradeResult result) {
        if (http_opened) {
            http_opened = false;
        }
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
        if (result != UpgradeResult::Success && ota_started) {
            esp_ota_abort(update_handle);
            ota_started = false;
        }
        return result;
    };

    if (is_cancelled()) {
        ESP_LOGW(TAG, "OTA upgrade cancelled before start");
        return UpgradeResult::Cancelled;
    }

    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return UpgradeResult::Failed;
    }

    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    
    if (!http->Open("GET", firmware_url)) {
        if (is_cancelled()) {
            ESP_LOGW(TAG, "OTA upgrade cancelled while opening firmware URL");
            return UpgradeResult::Cancelled;
        }
        ESP_LOGE(TAG, "Failed to open firmware URL: %s", firmware_url.c_str());
        return UpgradeResult::Failed;
    }
    http_opened = true;

    if (http->GetStatusCode() != 200) {
        if (is_cancelled()) {
            http->Close();
            http_opened = false;
            ESP_LOGW(TAG, "OTA upgrade cancelled after HTTP open");
            return UpgradeResult::Cancelled;
        }
        ESP_LOGE(TAG, "Firmware download failed with status: %d", http->GetStatusCode());
        http->Close();
        http_opened = false;
        return UpgradeResult::Failed;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Firmware content length is zero");
        http->Close();
        http_opened = false;
        return UpgradeResult::Failed;
    }

    constexpr size_t PAGE_SIZE = 4096;
    buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        http->Close();
        http_opened = false;
        return UpgradeResult::Failed;
    }

    size_t buffer_offset = 0;
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();

    while (true) {
        if (is_cancelled()) {
            ESP_LOGW(TAG, "OTA upgrade cancelled before read");
            http->Close();
            http_opened = false;
            return finish_before_ota_end(UpgradeResult::Cancelled);
        }

        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            if (is_cancelled()) {
                ESP_LOGW(TAG, "OTA upgrade cancelled while reading");
                http->Close();
                http_opened = false;
                return finish_before_ota_end(UpgradeResult::Cancelled);
            }
            ESP_LOGE(TAG, "HTTP read failed: %d", ret);
            http->Close();
            http_opened = false;
            return finish_before_ota_end(UpgradeResult::Failed);
        }

        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;

        // 计算下载进度
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            
            if (callback) {
                callback(progress, recent_read);
            }
            
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        // 验证固件头部（仅在首次读取时）
        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + 
                sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, 
                       image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), 
                       sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to begin OTA update");
                    http->Close();
                    http_opened = false;
                    return finish_before_ota_end(UpgradeResult::Failed);
                }

                ota_started = true;
                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        if (is_cancelled()) {
            ESP_LOGW(TAG, "OTA upgrade cancelled before write");
            http->Close();
            http_opened = false;
            return finish_before_ota_end(UpgradeResult::Cancelled);
        }

        // 写入固件数据
        bool is_last_chunk = (ret == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
                http->Close();
                http_opened = false;
                return finish_before_ota_end(UpgradeResult::Failed);
            }
            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }

    http->Close();
    http_opened = false;
    heap_caps_free(buffer);
    buffer = nullptr;

    if (!ota_started) {
        ESP_LOGE(TAG, "OTA image header was not received");
        return UpgradeResult::Failed;
    }

    // 完成 OTA 写入
    esp_err_t err = esp_ota_end(update_handle);
    ota_started = false;
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "OTA validate failed");
        } else {
            ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        }
        return UpgradeResult::Failed;
    }

    // 设置启动分区
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return UpgradeResult::Failed;
    }

    return UpgradeResult::Success;
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    return UpgradeWithResult(firmware_url, std::move(callback)) == UpgradeResult::Success;
}

/**
 * @brief 启动升级流程
 * 
 * 使用检查版本时获取的固件 URL 启动升级。
 * 
 * @param[in] callback 进度回调函数
 * @return true        升级成功
 * @return false       升级失败
 */
bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}

/**
 * @brief 解析版本字符串
 * 
 * 将版本字符串（如 "1.2.3"）解析为整数向量。
 * 
 * @param[in] version 版本字符串
 * @return 版本号整数向量
 */
std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;

    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }

    return versionNumbers;
}

/**
 * @brief 比较版本号判断是否有新版本
 * 
 * 逐位比较版本号，判断新版本是否高于当前版本。
 * 
 * @param[in] currentVersion 当前版本
 * @param[in] newVersion     新版本
 * @return true              新版本可用
 * @return false             当前已是最新版本
 */
bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);

    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }

    return newer.size() > current.size();
}

/**
 * @brief 生成激活负载数据
 * 
 * 使用 HMAC-SHA256 算法对挑战码进行签名，生成设备激活请求数据。
 * 
 * @return JSON 格式的激活负载字符串
 */
std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32];

    esp_err_t ret = esp_hmac_calculate(
        HMAC_KEY0, 
        (uint8_t*)activation_challenge_.data(), 
        activation_challenge_.size(), 
        hmac_result
    );
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    return json;
}

/**
 * @brief 执行设备激活
 * 
 * 向服务器发送激活请求，完成设备身份验证。
 * 
 * @return ESP_OK      激活成功
 * @return ESP_ERR_TIMEOUT 激活等待中（HTTP 202）
 * @return ESP_FAIL    激活失败
 */
esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGE(TAG, "No activation challenge available");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open activation URL: %s", url.c_str());
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Activation failed with status: %d", status_code);
        return ESP_FAIL;
    }

    return ESP_OK;
}
