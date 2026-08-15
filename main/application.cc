/**
 * @file application.cc
 * @brief 应用程序主类实现
 * 
 * 该文件包含 Application 类的核心实现，负责管理设备的整体状态、
 * 网络连接、音频服务、协议通信等关键功能。
 */

#include "application.h"
#include "board.h"
#include "display.h"
#include "oled_display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "mqtt_control.h"
#include "abnormal_reporter.h"
#include "assets.h"
#include "settings.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

#if HAVE_LVGL
#include "lcd_display.h"
#include "lvgl_image.h"
#include "remote_asset_regions.h"
#include "remote_mjpeg_store.h"
#include "remote_wallpaper_store.h"
#include "display/SmartWatch_UI/ui_runtime.h"
#include <esp_heap_caps.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <font_awesome.h>
#include <vector>
#include <string>
#include <ctime>
#include <new>

#define TAG "Application"

static constexpr int kListeningSilenceTimeoutSeconds = 12;
static constexpr uint32_t kNetworkToastProgressMs = 1800;
static constexpr uint32_t kNetworkToastConnectedMs = 2200;
static constexpr uint32_t kSkinSyncToastCompletedMs = 2200;
static constexpr uint32_t kSkinSyncToastFailedMs = 2200;
static constexpr uint32_t kAiChatConnectionErrorNoticeMs = 2000;
static constexpr uint32_t kRebootUiReactionMs = 700;
static constexpr uint32_t kRebootFinalizeDelayMs = 550;
static constexpr uint32_t kRebootNotificationMs = 1600;
static constexpr uint32_t kRebootAudioShutdownDelayMs = 100;
static constexpr int kAssetDownloadHttpTimeoutMs = 15000;
static constexpr int kBleBindPollIntervalMs = 3000;
static constexpr int kBleBindCloudPollIntervalMs = 10000;
static constexpr int kBleBindReleaseBeforeCloudCheckMs = 500;
static constexpr int kBleBindTimeoutNotifyAttempts = 600;
static constexpr int kBleBindTimeoutNoticeMs = 5000;
static constexpr int kActivationRetryDelayMaxSec = 60;
static std::atomic<bool> g_skin_download_in_progress{false};
static std::atomic<bool> g_role_download_in_progress{false};
static std::atomic<uint32_t> g_skin_notification_generation{0};
static std::atomic<bool> g_ble_bind_wait_in_progress{false};

static const char* DeviceStateToMqttString(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting:
            return "starting";
        case kDeviceStateWifiConfiguring:
            return "wifi_configuring";
        case kDeviceStateIdle:
            return "idle";
        case kDeviceStateConnecting:
            return "connecting";
        case kDeviceStateListening:
            return "listening";
        case kDeviceStateSpeaking:
            return "speaking";
        case kDeviceStateUpgrading:
            return "upgrading";
        case kDeviceStateActivating:
            return "activating";
        case kDeviceStateAudioTesting:
            return "audio_testing";
        default:
            return "unknown";
    }
}

static const char* NetworkModeToString(BoardNetworkMode mode) {
    switch (mode) {
        case BoardNetworkMode::WIFI:
            return "wifi";
        case BoardNetworkMode::CELLULAR:
            return "cellular";
        default:
            return "unsupported";
    }
}

static const char* NetworkModeToMqttString(BoardNetworkMode mode) {
    switch (mode) {
        case BoardNetworkMode::WIFI:
            return "wifi";
        case BoardNetworkMode::CELLULAR:
            return "4g";
        default:
            return "unknown";
    }
}

static bool HasCachedProtocolConfig() {
    Settings websocket_settings("websocket", false);
    return !websocket_settings.GetString("url").empty();
}

static std::string FormatStringWithDeviceName(std::string text, const char* device_name) {
    const char* value = device_name != nullptr ? device_name : "";
    const auto placeholder = text.find("%s");
    if (placeholder != std::string::npos) {
        text.replace(placeholder, 2, value);
    } else if (value[0] != '\0') {
        text += value;
    }
    return text;
}

static std::string BuildBleBindTimeoutNotice() {
    std::string notice = Lang::Strings::SERVER_TIMEOUT;
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    std::string hint =
        FormatStringWithDeviceName(Lang::Strings::CONNECT_WITH_BLUFI,
                                   Blufi::GetInstance().GetDeviceName());
    if (!hint.empty()) {
        notice += "\n";
        notice += hint;
    }
#endif
    return notice;
}

static bool IsBleBindClientConnected() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    return Blufi::GetInstance().IsBleConnected();
#else
    return false;
#endif
}

static uint32_t GetBleBindClientDisconnectGeneration() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    return Blufi::GetInstance().GetBleClientDisconnectGeneration();
#else
    return 0;
#endif
}

static bool PauseBleBindModeBeforeCloudCheck(Board& board) {
    if (!board.IsBleBindModeActive()) {
        return false;
    }
    if (IsBleBindClientConnected()) {
        return false;
    }

    board.ExitBleBindMode();
    vTaskDelay(pdMS_TO_TICKS(kBleBindReleaseBeforeCloudCheckMs));
    return true;
}

static const char* BatteryStateToMqttString(bool charging, bool discharging) {
    if (charging) {
        return "charging";
    }
    if (discharging) {
        return "discharging";
    }
    return "idle";
}

static cJSON* BuildMqttDeviceInfoResult() {
    auto& board = Board::GetInstance();

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    auto cleanup_root = [&]() -> cJSON* {
        cJSON_Delete(root);
        return nullptr;
    };
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", Lang::CODE);
    cJSON_AddStringToObject(root, "role", board.GetDeviceRole().c_str());
    cJSON_AddStringToObject(root, "network_version", board.GetDeviceNetworkVersion().c_str());
    cJSON_AddNumberToObject(root, "flash_size", static_cast<double>(SystemInfo::GetFlashSize()));
    cJSON_AddNumberToObject(root, "flash_used_size",
                            static_cast<double>(SystemInfo::GetFlashUsedSize()));
    cJSON_AddNumberToObject(root, "psram_total_size",
                            static_cast<double>(SystemInfo::GetPsramTotalSize()));
    cJSON_AddNumberToObject(root, "psram_used_size",
                            static_cast<double>(SystemInfo::GetPsramUsedSize()));
    cJSON_AddStringToObject(root, "minimum_free_heap_size",
                            std::to_string(SystemInfo::GetMinimumFreeHeapSize()).c_str());
    cJSON_AddStringToObject(root, "mac_address", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "uuid", board.GetUuid().c_str());
    cJSON_AddStringToObject(root, "chip_model_name", SystemInfo::GetChipModelName().c_str());
    cJSON_AddStringToObject(root, "network_type",
                            NetworkModeToMqttString(board.GetActiveNetworkMode()));

    if (auto* backlight = board.GetBacklight(); backlight != nullptr) {
        cJSON_AddNumberToObject(root, "brightness", backlight->brightness());
    }

    if (auto* codec = board.GetAudioCodec(); codec != nullptr) {
        cJSON_AddNumberToObject(root, "volume", codec->output_volume());
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON_AddNumberToObject(root, "battery_level", battery_level);
        cJSON_AddStringToObject(root, "battery_status",
                                BatteryStateToMqttString(charging, discharging));
    }

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON* chip_info_json = cJSON_CreateObject();
    if (chip_info_json == nullptr) {
        return cleanup_root();
    }
    cJSON_AddNumberToObject(chip_info_json, "model", chip_info.model);
    cJSON_AddNumberToObject(chip_info_json, "cores", chip_info.cores);
    cJSON_AddNumberToObject(chip_info_json, "revision", chip_info.revision);
    cJSON_AddNumberToObject(chip_info_json, "features", chip_info.features);
    cJSON_AddItemToObject(root, "chip_info", chip_info_json);

    const esp_app_desc_t* app_desc = esp_app_get_description();
    cJSON* application = cJSON_CreateObject();
    if (application == nullptr) {
        return cleanup_root();
    }
    cJSON_AddStringToObject(application, "name", app_desc->project_name);
    cJSON_AddStringToObject(application, "version", app_desc->version);
    const std::string compile_time =
        std::string(app_desc->date) + "T" + std::string(app_desc->time) + "Z";
    cJSON_AddStringToObject(application, "compile_time", compile_time.c_str());
    cJSON_AddStringToObject(application, "idf_version", app_desc->idf_ver);
    char sha256_str[65] = {0};
    for (int i = 0; i < 32; ++i) {
        snprintf(sha256_str + i * 2, sizeof(sha256_str) - i * 2, "%02x",
                 app_desc->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(application, "elf_sha256", sha256_str);
    cJSON_AddItemToObject(root, "application", application);

    cJSON* partition_table = cJSON_CreateArray();
    if (partition_table == nullptr) {
        return cleanup_root();
    }
    cJSON_AddItemToObject(root, "partition_table", partition_table);

    esp_partition_iterator_t partition_it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (partition_it != nullptr) {
        const esp_partition_t* partition = esp_partition_get(partition_it);
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            esp_partition_iterator_release(partition_it);
            return cleanup_root();
        }
        cJSON_AddStringToObject(item, "label", partition->label);
        cJSON_AddNumberToObject(item, "type", partition->type);
        cJSON_AddNumberToObject(item, "subtype", partition->subtype);
        cJSON_AddNumberToObject(item, "address", partition->address);
        cJSON_AddNumberToObject(item, "size", partition->size);
        cJSON_AddItemToArray(partition_table, item);
        partition_it = esp_partition_next(partition_it);
    }

    cJSON* ota = cJSON_CreateObject();
    if (ota == nullptr) {
        return cleanup_root();
    }
    if (const esp_partition_t* running_partition = esp_ota_get_running_partition();
        running_partition != nullptr) {
        cJSON_AddStringToObject(ota, "label", running_partition->label);
    }
    cJSON_AddItemToObject(root, "ota", ota);

    cJSON* display = cJSON_CreateObject();
    if (display == nullptr) {
        return cleanup_root();
    }
    if (auto* board_display = board.GetDisplay(); board_display != nullptr) {
        cJSON_AddBoolToObject(display, "monochrome",
                              dynamic_cast<OledDisplay*>(board_display) != nullptr);
        cJSON_AddNumberToObject(display, "width", board_display->width());
        cJSON_AddNumberToObject(display, "height", board_display->height());
    }
    cJSON_AddItemToObject(root, "display", display);

    cJSON* board_json = cJSON_Parse(board.GetBoardJson().c_str());
    if (cJSON_IsObject(board_json)) {
        cJSON_AddItemToObject(root, "board", board_json);
    } else {
        if (board_json != nullptr) {
            cJSON_Delete(board_json);
        }
        board_json = cJSON_CreateObject();
        if (board_json == nullptr) {
            return cleanup_root();
        }
        cJSON_AddItemToObject(root, "board", board_json);
    }

    return root;
}

static cJSON* BuildMqttWakeWordConfigsResult(WakeWord* wake_word) {
    if (wake_word == nullptr) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON* items = cJSON_CreateArray();
    if (items == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }

    auto configs = wake_word->GetWakeWordConfigs();
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddNumberToObject(root, "count", static_cast<int>(configs.size()));
    cJSON_AddItemToObject(root, "items", items);
    for (const auto& config : configs) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            cJSON_Delete(root);
            return nullptr;
        }
        cJSON_AddStringToObject(item, "command", config.command.c_str());
        cJSON_AddStringToObject(item, "displayText", config.display_text.c_str());
        cJSON_AddStringToObject(item, "action", config.action.c_str());
        cJSON_AddItemToArray(items, item);
    }

    return root;
}

static cJSON* BuildMqttWakeWordThresholdResult(WakeWord* wake_word) {
    if (wake_word == nullptr) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }

    const float threshold = wake_word->GetWakeWordThreshold();
    cJSON_AddNumberToObject(root, "threshold", threshold);
    cJSON_AddNumberToObject(root, "thresholdPercent",
                            static_cast<int>(threshold * 100.0f + 0.5f));
    return root;
}

static WakeWord* GetMqttCustomWakeWord(std::string* reason) {
    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.PrewarmWakeWordDetection();
    auto* wake_word = audio_service.GetWakeWord();
    if (wake_word == nullptr || !wake_word->IsCustomWakeWord()) {
        if (reason != nullptr) {
            *reason = "wake_word_not_supported";
        }
        return nullptr;
    }
    return wake_word;
}

static bool GetMqttStringParam(cJSON* params, const char* name, std::string* out) {
    if (!cJSON_IsObject(params) || name == nullptr || out == nullptr) {
        return false;
    }
    cJSON* value = cJSON_GetObjectItem(params, name);
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') {
        return false;
    }
    *out = value->valuestring;
    return true;
}

static bool GetMqttWakeWordDisplayText(cJSON* params, std::string* out) {
    return GetMqttStringParam(params, "displayText", out) ||
           GetMqttStringParam(params, "display_text", out);
}

static bool ParseMqttWakeWordConfigs(cJSON* params, std::vector<WakeWordConfig>* configs,
                                     std::string* reason) {
    if (!cJSON_IsObject(params) || configs == nullptr) {
        if (reason != nullptr) {
            *reason = "missing_items";
        }
        return false;
    }

    cJSON* items = cJSON_GetObjectItem(params, "items");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) <= 0) {
        if (reason != nullptr) {
            *reason = "missing_items";
        }
        return false;
    }

    std::vector<WakeWordConfig> parsed_configs;
    for (int i = 0; i < cJSON_GetArraySize(items); ++i) {
        cJSON* item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) {
            if (reason != nullptr) {
                *reason = "invalid_item";
            }
            return false;
        }

        std::string command;
        std::string display_text;
        std::string action = "wake";
        if (!GetMqttStringParam(item, "command", &command)) {
            if (reason != nullptr) {
                *reason = "missing_command";
            }
            return false;
        }
        if (!GetMqttWakeWordDisplayText(item, &display_text)) {
            if (reason != nullptr) {
                *reason = "missing_display_text";
            }
            return false;
        }
        GetMqttStringParam(item, "action", &action);

        for (const auto& existing : parsed_configs) {
            if (existing.command == command) {
                if (reason != nullptr) {
                    *reason = "duplicate_command";
                }
                return false;
            }
        }

        parsed_configs.push_back({command, display_text, action});
    }

    *configs = std::move(parsed_configs);
    return true;
}

static bool ParseMqttWakeWordThreshold(cJSON* params, float* threshold, std::string* reason) {
    if (!cJSON_IsObject(params) || threshold == nullptr) {
        if (reason != nullptr) {
            *reason = "missing_threshold";
        }
        return false;
    }

    cJSON* value = cJSON_GetObjectItem(params, "thresholdPercent");
    bool percent_value = cJSON_IsNumber(value);
    if (!percent_value) {
        value = cJSON_GetObjectItem(params, "threshold_percent");
        percent_value = cJSON_IsNumber(value);
    }
    if (!percent_value) {
        value = cJSON_GetObjectItem(params, "threshold");
    }

    if (!cJSON_IsNumber(value)) {
        if (reason != nullptr) {
            *reason = "missing_threshold";
        }
        return false;
    }

    const double raw_threshold = value->valuedouble;
    if (percent_value) {
        if (raw_threshold < 1.0 || raw_threshold > 99.0) {
            if (reason != nullptr) {
                *reason = "invalid_threshold";
            }
            return false;
        }
        *threshold = static_cast<float>(raw_threshold / 100.0);
        return true;
    }

    if (raw_threshold > 0.0 && raw_threshold < 1.0) {
        *threshold = static_cast<float>(raw_threshold);
        return true;
    }
    if (raw_threshold >= 1.0 && raw_threshold <= 99.0) {
        *threshold = static_cast<float>(raw_threshold / 100.0);
        return true;
    }

    if (reason != nullptr) {
        *reason = "invalid_threshold";
    }
    return false;
}

static void ClearCloudBindingSettings() {
    Settings auth_settings("auth", true);
    auth_settings.EraseAll();

    Settings mqtt_settings("mqtt", true);
    mqtt_settings.EraseAll();

    Settings websocket_settings("websocket", true);
    websocket_settings.EraseAll();

    Settings mqtt_ctrl_settings("mqtt_ctrl", true);
    mqtt_ctrl_settings.EraseAll();
}

#if HAVE_LVGL
struct SkinMaterialParseResult {
    bool ok = false;
    int material_type = 0;
    std::vector<std::string> urls;
    uint32_t interval_ms = 3000;
};

struct SkinUpdateTaskPayload {
    Application* app = nullptr;
    SkinMaterialParseResult parsed;
};

struct RoleSwitchParseResult {
    std::string role_id;
    std::string listen_mjpeg_url;
    std::string speak_mjpeg_url;
};

struct RoleSwitchTaskPayload {
    Application* app = nullptr;
    RoleSwitchParseResult parsed;
};

struct DownloadedWallpaperData {
    uint8_t* data = nullptr;
    size_t size = 0;
};

static void FreeDownloadedWallpapers(std::vector<DownloadedWallpaperData>* wallpapers) {
    if (wallpapers == nullptr) {
        return;
    }
    for (auto& wallpaper : *wallpapers) {
        if (wallpaper.data != nullptr) {
            heap_caps_free(wallpaper.data);
            wallpaper.data = nullptr;
            wallpaper.size = 0;
        }
    }
    wallpapers->clear();
}

static std::string TrimAsciiWhitespace(const std::string& input) {
    size_t begin = 0;
    size_t end = input.size();
    while (begin < end) {
        char ch = input[begin];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++begin;
    }
    while (end > begin) {
        char ch = input[end - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        --end;
    }
    return input.substr(begin, end - begin);
}

static std::vector<std::string> SplitSkinUrls(const std::string& skin_url) {
    std::vector<std::string> urls;
    size_t start = 0;
    while (start <= skin_url.size()) {
        size_t end = skin_url.find(';', start);
        std::string item = end == std::string::npos
                               ? skin_url.substr(start)
                               : skin_url.substr(start, end - start);
        item = TrimAsciiWhitespace(item);
        if (!item.empty()) {
            urls.push_back(std::move(item));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return urls;
}

static bool ParseSkinUpdateParams(cJSON* params, SkinMaterialParseResult* out) {
    out->ok = false;
    out->urls.clear();
    out->interval_ms = 3000;
    out->material_type = 0;

    if (!cJSON_IsObject(params)) {
        return false;
    }

    cJSON* type = cJSON_GetObjectItem(params, "type");
    if (cJSON_IsNumber(type)) {
        out->material_type = type->valueint;
    }

    cJSON* mode = cJSON_GetObjectItem(params, "mode");
    if (cJSON_IsString(mode) && mode->valuestring != nullptr) {
        if (strcmp(mode->valuestring, "single") == 0) {
            out->material_type = 1;
        } else if (strcmp(mode->valuestring, "carousel") == 0) {
            out->material_type = 2;
        }
    }

    cJSON* skin_url = cJSON_GetObjectItem(params, "skinUrl");
    if (!cJSON_IsString(skin_url) || skin_url->valuestring == nullptr) {
        return false;
    }

    std::vector<std::string> urls = SplitSkinUrls(skin_url->valuestring);
    if (urls.empty()) {
        return false;
    }

    cJSON* config_str = cJSON_GetObjectItem(params, "config");
    int carousel_count = -1;
    if (cJSON_IsString(config_str) && config_str->valuestring != nullptr &&
        config_str->valuestring[0] != '\0') {
        cJSON* config = cJSON_Parse(config_str->valuestring);
        if (config != nullptr) {
            cJSON* cnt = cJSON_GetObjectItem(config, "count");
            if (cJSON_IsNumber(cnt) && cnt->valueint > 0) {
                carousel_count = cnt->valueint;
            }
            cJSON* inv = cJSON_GetObjectItem(config, "interval");
            if (cJSON_IsNumber(inv) && inv->valueint > 0) {
                out->interval_ms = static_cast<uint32_t>(inv->valueint);
            }
            cJSON_Delete(config);
        }
    }

    int limit = static_cast<int>(urls.size());
    if (out->material_type == 1) {
        limit = 1;
    } else if (out->material_type == 2 && carousel_count > 0) {
        limit = std::min(static_cast<int>(urls.size()), carousel_count);
    }

    for (int i = 0; i < limit && i < static_cast<int>(urls.size()); ++i) {
        out->urls.push_back(std::move(urls[i]));
    }

    if (out->interval_ms < 500) {
        out->interval_ms = 3000;
    }

    out->ok = !out->urls.empty();
    return out->ok;
}

static bool ParseRoleSwitchParams(cJSON* params, RoleSwitchParseResult* out) {
    if (!cJSON_IsObject(params) || out == nullptr) {
        return false;
    }

    cJSON* role_id = cJSON_GetObjectItem(params, "roleId");
    cJSON* listen_url = cJSON_GetObjectItem(params, "listenMjpegUrl");
    cJSON* speak_url = cJSON_GetObjectItem(params, "speakMjpegUrl");
    if (!cJSON_IsString(role_id) || role_id->valuestring == nullptr ||
        !cJSON_IsString(listen_url) || listen_url->valuestring == nullptr ||
        !cJSON_IsString(speak_url) || speak_url->valuestring == nullptr) {
        return false;
    }

    out->role_id = TrimAsciiWhitespace(role_id->valuestring);
    out->listen_mjpeg_url = TrimAsciiWhitespace(listen_url->valuestring);
    out->speak_mjpeg_url = TrimAsciiWhitespace(speak_url->valuestring);
    return out->role_id.size() < 64 && !out->listen_mjpeg_url.empty() &&
           !out->speak_mjpeg_url.empty();
}

static bool DownloadFileToPsram(const char* url, size_t max_size, uint8_t** out_data,
                                size_t* out_len) {
    constexpr int kMaxRetries = 3;
    constexpr size_t kChunkSize = 4096;
    *out_data = nullptr;
    *out_len = 0;

    if (url == nullptr || url[0] == '\0' || max_size == 0) {
        return false;
    }

    for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) {
            ESP_LOGW(TAG, "Network is not ready for download attempt %d", attempt);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        auto http = network->CreateHttp(3);
        if (http == nullptr) {
            ESP_LOGW(TAG, "Failed to create HTTP client for download attempt %d", attempt);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }
        http->SetTimeout(kAssetDownloadHttpTimeoutMs);
        if (!http->Open("GET", url)) {
            http->Close();
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (http->GetStatusCode() != 200) {
            http->Close();
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        size_t content_length = http->GetBodyLength();
        uint8_t* data = nullptr;
        size_t total_read = 0;
        bool ok = true;

        if (content_length > 0) {
            if (content_length > max_size) {
                ESP_LOGW(TAG, "Download size %u exceeds limit %u", static_cast<unsigned>(content_length),
                         static_cast<unsigned>(max_size));
                http->Close();
                return false;
            }
            data = reinterpret_cast<uint8_t*>(
                heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (data == nullptr) {
                http->Close();
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            while (total_read < content_length) {
                int ret = http->Read(reinterpret_cast<char*>(data + total_read),
                                    content_length - total_read);
                if (ret < 0) {
                    ok = false;
                    break;
                }
                if (ret == 0) {
                    ok = false;
                    break;
                }
                total_read += static_cast<size_t>(ret);
            }
            if (total_read != content_length) {
                ok = false;
            }
        } else {
            size_t capacity = std::min(kChunkSize * 2, max_size);
            data = reinterpret_cast<uint8_t*>(
                heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (data == nullptr) {
                http->Close();
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }

            while (true) {
                if (total_read == max_size) {
                    uint8_t overflow_byte = 0;
                    if (http->Read(reinterpret_cast<char*>(&overflow_byte), 1) != 0) {
                        ok = false;
                    }
                    break;
                }

                if (capacity - total_read < kChunkSize && capacity < max_size) {
                    size_t new_capacity = std::min(capacity * 2, max_size);
                    uint8_t* grown = reinterpret_cast<uint8_t*>(
                        heap_caps_realloc(data, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (grown == nullptr) {
                        ok = false;
                        break;
                    }
                    data = grown;
                    capacity = new_capacity;
                }

                const size_t read_size = std::min(kChunkSize, capacity - total_read);
                int ret = http->Read(reinterpret_cast<char*>(data + total_read), read_size);
                if (ret < 0) {
                    ok = false;
                    break;
                }
                if (ret == 0) {
                    break;
                }
                total_read += static_cast<size_t>(ret);
            }
            if (total_read == 0) {
                ok = false;
            }
        }

        http->Close();

        if (!ok) {
            heap_caps_free(data);
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        *out_data = data;
        *out_len = total_read;
        return true;
    }

    return false;
}

static void DownloadServerBackgroundTask(void* arg) {
    std::unique_ptr<SkinUpdateTaskPayload> payload(static_cast<SkinUpdateTaskPayload*>(arg));
    auto cleanup_and_exit = [&payload]() {
        g_skin_download_in_progress.store(false);
        payload.reset();
        vTaskDelete(nullptr);
    };

    const uint32_t progress_gen = g_skin_notification_generation.fetch_add(1) + 1;
    auto schedule_notification = [&](uint32_t gen, std::string text, uint32_t duration_ms) {
        if (payload == nullptr || payload->app == nullptr) {
            return;
        }
        payload->app->Schedule([gen, duration_ms, text = std::move(text)]() {
            if (gen != g_skin_notification_generation.load()) {
                return;
            }
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowNotification(text.c_str(), duration_ms);
            }
        });
    };
    auto schedule_persistent_top_notification = [&](uint32_t gen, std::string text) {
        if (payload == nullptr || payload->app == nullptr) {
            return;
        }
        payload->app->Schedule([gen, text = std::move(text)]() {
            if (gen != g_skin_notification_generation.load()) {
                return;
            }
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowPersistentNotification(text.c_str(), true);
            }
        });
    };
    auto notify_result = [&](const char* text, uint32_t duration_ms) {
        if (text == nullptr) {
            return;
        }
        const uint32_t final_gen = g_skin_notification_generation.fetch_add(1) + 1;
        schedule_notification(final_gen, std::string(text), duration_ms);
    };
    auto notify_progress = [&](size_t current, size_t total) {
        if (total == 0) {
            return;
        }
        std::string text = "正在下载图片 " + std::to_string(current) + "/" +
                           std::to_string(total) + "...";
        schedule_persistent_top_notification(progress_gen, std::move(text));
    };

    if (payload == nullptr || payload->app == nullptr || payload->parsed.urls.empty()) {
        notify_result("下载失败", kSkinSyncToastFailedMs);
        cleanup_and_exit();
        return;
    }

    // 逐个下载图片原始数据，优先写入 flash，显示时再按需解码。
    std::vector<DownloadedWallpaperData> downloaded_wallpapers;
    downloaded_wallpapers.reserve(payload->parsed.urls.size());
    const size_t total_urls = payload->parsed.urls.size();
    for (size_t i = 0; i < total_urls; ++i) {
        notify_progress(i + 1, total_urls);
        const auto& url = payload->parsed.urls[i];
        uint8_t* compressed_data = nullptr;
        size_t compressed_len = 0;
        if (DownloadFileToPsram(url.c_str(), RemoteAssetRegions::kWallpaperSize / 2,
                                 &compressed_data, &compressed_len)) {
            downloaded_wallpapers.push_back({compressed_data, compressed_len});
        }
    }
    if (downloaded_wallpapers.empty()) {
        notify_result("下载失败", kSkinSyncToastFailedMs);
        cleanup_and_exit();
        return;
    }

    const uint8_t wallpaper_mode = payload->parsed.material_type == 2 ? 1 : 0;
    const uint32_t wallpaper_interval_ms = payload->parsed.interval_ms;

    std::vector<std::pair<const uint8_t*, size_t>> wallpaper_refs;
    wallpaper_refs.reserve(downloaded_wallpapers.size());
    for (const auto& wallpaper : downloaded_wallpapers) {
        wallpaper_refs.emplace_back(wallpaper.data, wallpaper.size);
    }

    if (RemoteWallpaperStore::GetInstance().Save(wallpaper_refs, wallpaper_mode,
                                                 wallpaper_interval_ms)) {
        FreeDownloadedWallpapers(&downloaded_wallpapers);
        const uint32_t final_gen = g_skin_notification_generation.fetch_add(1) + 1;
        payload->app->Schedule([]() {
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                DisplayLockGuard lock(display);
                smartwatch_ui_runtime_wallpaper_preview_reload_remote();
            }
        });
        schedule_notification(final_gen, "下载完成", kSkinSyncToastCompletedMs);
        cleanup_and_exit();
        return;
    }

    // 不支持远程持久化时，回退到旧的内存预览逻辑。
    std::vector<std::unique_ptr<LvglImage>> images;
    images.reserve(downloaded_wallpapers.size());
    for (auto& wallpaper : downloaded_wallpapers) {
        auto image = DecodeCompressedImageToLvgl(wallpaper.data, wallpaper.size);
        wallpaper.data = nullptr;
        wallpaper.size = 0;
        if (image) {
            images.push_back(std::move(image));
        }
    }
    FreeDownloadedWallpapers(&downloaded_wallpapers);

    if (images.empty()) {
        notify_result("下载失败", kSkinSyncToastFailedMs);
        cleanup_and_exit();
        return;
    }

    auto shared_images =
        std::make_shared<std::vector<std::unique_ptr<LvglImage>>>(std::move(images));
    const uint32_t final_gen = g_skin_notification_generation.fetch_add(1) + 1;
    payload->app->Schedule([shared_images, wallpaper_mode, wallpaper_interval_ms]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            DisplayLockGuard lock(display);
            smartwatch_ui_runtime_wallpaper_preview_set_images(std::move(*shared_images), wallpaper_mode,
                                                               wallpaper_interval_ms);
        }
    });
    schedule_notification(final_gen, "下载完成", kSkinSyncToastCompletedMs);
    cleanup_and_exit();
    return;
}

static void DownloadRoleMjpegTask(void* arg) {
    std::unique_ptr<RoleSwitchTaskPayload> payload(static_cast<RoleSwitchTaskPayload*>(arg));
    DownloadedWallpaperData listen_mjpeg;
    DownloadedWallpaperData speak_mjpeg;
    const uint32_t notification_gen = g_skin_notification_generation.fetch_add(1) + 1;

    auto schedule_top_notification = [&](std::string text) {
        if (payload == nullptr || payload->app == nullptr) {
            return;
        }
        payload->app->Schedule([notification_gen, text = std::move(text)]() {
            if (notification_gen != g_skin_notification_generation.load()) {
                return;
            }
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowPersistentNotification(text.c_str(), true);
            }
        });
    };
    auto schedule_result_notification = [&](const char* text, uint32_t duration_ms) {
        if (payload == nullptr || payload->app == nullptr || text == nullptr) {
            return;
        }
        payload->app->Schedule([notification_gen, duration_ms, text = std::string(text)]() {
            uint32_t expected = notification_gen;
            if (!g_skin_notification_generation.compare_exchange_strong(
                    expected, notification_gen + 1)) {
                return;
            }
            auto* display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->ShowPersistentNotification("", true);
                display->ShowNotification(text.c_str(), duration_ms);
            }
        });
    };

    auto cleanup = [&]() {
        if (listen_mjpeg.data != nullptr) {
            heap_caps_free(listen_mjpeg.data);
        }
        if (speak_mjpeg.data != nullptr) {
            heap_caps_free(speak_mjpeg.data);
        }
        payload.reset();
        g_role_download_in_progress.store(false);
    };

    schedule_top_notification("正在更换背景图片...");

    bool downloaded = payload != nullptr && payload->app != nullptr &&
                      DownloadFileToPsram(payload->parsed.listen_mjpeg_url.c_str(),
                                          RemoteAssetRegions::kMjpegSize, &listen_mjpeg.data,
                                          &listen_mjpeg.size) &&
                      DownloadFileToPsram(payload->parsed.speak_mjpeg_url.c_str(),
                                          RemoteAssetRegions::kMjpegSize, &speak_mjpeg.data,
                                          &speak_mjpeg.size);
    if (!downloaded) {
        ESP_LOGW(TAG, "Failed to download role MJPEG files");
        schedule_result_notification("更换失败", kSkinSyncToastFailedMs);
        cleanup();
        vTaskDelete(nullptr);
        return;
    }

    if (!RemoteMjpegStore::GetInstance().Save(
            payload->parsed.role_id, listen_mjpeg.data, listen_mjpeg.size, speak_mjpeg.data,
            speak_mjpeg.size)) {
        ESP_LOGW(TAG, "Failed to persist role MJPEG files");
        schedule_result_notification("更换失败", kSkinSyncToastFailedMs);
        cleanup();
        vTaskDelete(nullptr);
        return;
    }

    payload->app->Schedule([]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            DisplayLockGuard lock(display);
            smartwatch_ui_runtime_reload_ai_chat_mjpeg();
        }
    });
    schedule_result_notification("更换完成", kSkinSyncToastCompletedMs);
    ESP_LOGI(TAG, "Role MJPEG files updated for role '%s'", payload->parsed.role_id.c_str());

    cleanup();
    vTaskDelete(nullptr);
}
#endif

/**
 * @brief Application 构造函数
 * 
 * 初始化事件组和时钟定时器，根据配置设置 AEC 模式。
 */
Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

/**
 * @brief Application 析构函数
 * 
 * 释放定时器和事件组资源。
 */
Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

/**
 * @brief 设置设备状态
 * @param state 目标设备状态
 * @return 是否成功转换状态
 */
bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

/**
 * @brief 初始化应用程序
 * 
 * 初始化显示、音频服务、MCP 服务器，设置网络事件回调，启动网络连接。
 */
void Application::Initialize() {
    AbnormalReporter::Initialize();

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    auto display = board.GetDisplay();
    display->SetupUI();
    board.OnApplicationReady();
    if (auto* backlight = board.GetBacklight(); backlight != nullptr) {
        backlight->RestoreBrightness();
    }
    display->SetChatMessage("system", "Baji 吧唧");

    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    audio_service_.SetCallbacks(callbacks);

    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    auto& mqtt_control = MqttControl::GetInstance();
    mqtt_control.OnCommand([this](const char* json, int len) {
        this->HandleMqttCommand(json, len);
    });

    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        Schedule([this, event, data]() {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowPersistentNotification(Lang::Strings::SCANNING_WIFI, true);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                display->ShowPersistentNotification("", true);
                if (data.empty()) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), kNetworkToastProgressMs);
                }
                break;
            }
            case NetworkEvent::Connected: {
                display->ShowPersistentNotification("", true);
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), kNetworkToastConnectedMs);
                last_connected_network_mode_.store(
                    static_cast<int>(Board::GetInstance().GetActiveNetworkMode()));
                network_connected_generation_.fetch_add(1);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                display->ShowPersistentNotification("", true);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                display->ShowPersistentNotification("", true);
                break;
            case NetworkEvent::WifiConfigModeExit:
                display->ShowPersistentNotification("", true);
                break;
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });
    });

    board.StartNetwork();
    display->UpdateStatusBar(true);
}

/**
 * @brief 应用程序主循环
 * 
 * 这是应用程序的核心事件循环，等待并处理所有事件，包括：
 * - 网络连接/断开
 * - 唤醒词检测
 * - 音频发送/接收
 * - 状态变化
 * - 定时时钟tick
 */
void Application::Run() {
    vTaskPrioritySet(nullptr, 10);
#if CONFIG_ESP_TASK_WDT_EN
    esp_err_t wdt_err = esp_task_wdt_add(nullptr);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add application task to watchdog: %s",
                 esp_err_to_name(wdt_err));
    }
#endif

    const EventBits_t ALL_EVENTS = MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO |
                                   MAIN_EVENT_WAKE_WORD_DETECTED | MAIN_EVENT_VAD_CHANGE |
                                   MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
                                   MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED |
                                   MAIN_EVENT_TOGGLE_CHAT | MAIN_EVENT_START_LISTENING |
                                   MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
                                   MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED;

    while (true) {
        // 等待事件组中的位，直到有事件发生
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif

        if (bits & MAIN_EVENT_ERROR) {
            // 处理错误事件
            const auto state = GetDeviceState();
            std::string error_message = last_error_message_.empty()
                                            ? Lang::Strings::SERVER_NOT_CONNECTED
                                            : last_error_message_;
            last_error_message_.clear();
            keep_ai_chat_visible_on_idle_ = false;
            idle_assistant_message_.clear();
            pending_idle_notification_.clear();
            pending_idle_notification_duration_ms_ = 0;

            const bool ai_runtime_state = state == kDeviceStateIdle ||
                                          state == kDeviceStateConnecting ||
                                          state == kDeviceStateListening ||
                                          state == kDeviceStateSpeaking;
            if (ai_runtime_state) {
                pending_idle_notification_ = std::move(error_message);
                pending_idle_notification_duration_ms_ = kAiChatConnectionErrorNoticeMs;
                if (state == kDeviceStateIdle || !SetDeviceState(kDeviceStateIdle)) {
                    auto display = Board::GetInstance().GetDisplay();
                    display->SetStatus(Lang::Strings::STANDBY);
                    display->ClearChatMessages();
                    display->SetEmotion("neutral");
                    ShowPendingIdleNotification(display);
                    RefreshWakeWordDetection();
                }
            } else {
                auto display = Board::GetInstance().GetDisplay();
                display->ShowNotification(error_message.c_str(), kAiChatConnectionErrorNoticeMs);
            }
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // 发送音频数据包
            constexpr size_t kMaxPacketsPerDispatch = 2;
            size_t packets_sent = 0;
            bool send_failed = false;
            while (packets_sent < kMaxPacketsPerDispatch) {
                auto packet = audio_service_.PopPacketFromSendQueue();
                if (!packet) {
                    break;
                }
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    while (audio_service_.PopPacketFromSendQueue()) {
                    }
                    send_failed = true;
                    break;
                }
                ++packets_sent;
            }
            if (!send_failed && audio_service_.HasPendingSendPackets()) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            // 处理 VAD（语音活动检测）变化
            if (GetDeviceState() == kDeviceStateListening) {
                listening_silence_ticks_ = 0;
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            // 执行调度任务
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            // 处理秒级时钟 tick
            clock_ticks_++;
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            display->UpdateStatusBar();

            // 监听模式下的静音检测
            if (GetDeviceState() == kDeviceStateListening &&
                listening_mode_ != kListeningModeManualStop) {
                if (audio_service_.IsVoiceDetected()) {
                    listening_silence_ticks_ = 0;
                } else if (++listening_silence_ticks_ >= kListeningSilenceTimeoutSeconds) {
                    if (protocol_) {
                        protocol_->SendStopListening();
                        protocol_->CloseAudioChannel();
                    }
                    SetDeviceState(kDeviceStateIdle);
                }
            }

            // 定期打印堆栈统计信息
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }

            board.OnApplicationClockTick();
        }
    }
}

/**
 * @brief 处理网络连接事件
 * 
 * 当网络连接成功时触发，根据当前设备状态执行不同的操作：
 * - 首次联网完成: 启动激活任务
 * - 运行时配网完成: 恢复 Idle 状态
 * - Idle 且无协议: 重启协议连接
 */
void Application::HandleNetworkConnectedEvent() {
    auto state = GetDeviceState();

    Settings mqtt_ctrl_settings("mqtt_ctrl");
    const bool has_mqtt_control_config =
        !mqtt_ctrl_settings.GetString("endpoint").empty() ||
        !mqtt_ctrl_settings.GetString("cellular_endpoint").empty();

    if (has_mqtt_control_config) {
        MqttControl::GetInstance().Start();
    }

    // A runtime network switch can move the application to Idle while the
    // boot-time provisioning screen is still active. Until the first
    // activation completes, the next successful connection must still finish
    // startup so the normal UI is initialized.
    const bool needs_startup_activation = !startup_activation_completed_ &&
        (state == kDeviceStateStarting ||
         state == kDeviceStateWifiConfiguring ||
         state == kDeviceStateIdle);
    if (needs_startup_activation) {
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            return;
        }

        BaseType_t ok = xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
        if (ok != pdPASS) {
            activation_task_handle_ = nullptr;
            ESP_LOGE(TAG, "Failed to create activation task");
            Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_ERROR, "triangle_exclamation",
                  Lang::Sounds::OGG_EXCLAMATION);
            SetDeviceState(kDeviceStateIdle);
        }
    } else {
        auto& board = Board::GetInstance();
        // Runtime reprovisioning clears the protocol before BLUFI starts. Wait
        // for the managed station to reconnect before leaving the config state.
        const bool runtime_wifi_provisioning_completed =
            startup_activation_completed_ &&
            state == kDeviceStateWifiConfiguring &&
            !board.IsWifiConfigModeActive();
        if (runtime_wifi_provisioning_completed) {
            SetDeviceState(kDeviceStateIdle);
            state = GetDeviceState();
        }

        if (state == kDeviceStateIdle && protocol_ == nullptr) {
            RestartProtocolFromSettings();
        }
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::RestartProtocolFromSettings() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    protocol_ = std::make_unique<WebsocketProtocol>();
    const uint32_t generation = protocol_generation_.fetch_add(1) + 1;
    SetupProtocolCallbacks(display, codec, board, generation);

    protocol_->Start();

    // WebSocket 传输在 Start() 过程中不会触发 OnConnected 事件，因此在此处刷新空闲界面，以避免显示过时的“登录服务器...”文本
    if (GetDeviceState() == kDeviceStateIdle) {
        DismissAlert();
    }
    
}

void Application::SetupProtocolCallbacks(Display* display, AudioCodec* codec, Board& board,
                                        uint32_t generation) {
    protocol_->OnConnected([this, generation]() {
        if (generation != protocol_generation_.load()) {
            return;
        }
        DismissAlert();
    });

    protocol_->OnNetworkError([this, generation](const std::string& message) {
        if (generation != protocol_generation_.load()) {
            return;
        }
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this, generation](std::unique_ptr<AudioStreamPacket> packet) {
        if (generation != protocol_generation_.load()) {
            return;
        }
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board, generation]() {
        if (generation != protocol_generation_.load()) {
            return;
        }
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        (void)codec;
    });

    protocol_->OnAudioChannelClosed([this, &board, generation]() {
        if (generation != protocol_generation_.load()) {
            return;
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this, generation]() {
            if (generation != protocol_generation_.load() || reboot_in_progress_.load()) {
                return;
            }
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display, generation](const cJSON* root) {
        if (generation != protocol_generation_.load() || root == nullptr) {
            return;
        }
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type) || type->valuestring == nullptr) {
            return;
        }

        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state) || state->valuestring == nullptr) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text) && text->valuestring != nullptr) {
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && text->valuestring != nullptr) {
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion) && emotion->valuestring != nullptr) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command) && command->valuestring != nullptr) {
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() { Reboot(); });
                }
            }
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            std::string closing_session;
            if (cJSON_IsString(session_id) && session_id->valuestring != nullptr) {
                closing_session = session_id->valuestring;
            }
            Schedule([this, closing_session]() {
                if (protocol_ == nullptr) {
                    return;
                }
                if (!closing_session.empty() && protocol_->session_id() != closing_session) {
                    return;
                }
                protocol_->CloseAudioChannel(false);
            });
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion) &&
                status->valuestring != nullptr && message->valuestring != nullptr &&
                emotion->valuestring != nullptr) {
                Schedule([this,
                          status_str = std::string(status->valuestring),
                          message_str = std::string(message->valuestring),
                          emotion_str = std::string(emotion->valuestring)]() {
                    Alert(status_str.c_str(), message_str.c_str(), emotion_str.c_str(),
                          Lang::Sounds::OGG_VIBRATION);
                });
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                char* printed = cJSON_PrintUnformatted(payload);
                if (printed != nullptr) {
                    std::string payload_str(printed);
                    cJSON_free(printed);
                    Schedule([display, payload_str = std::move(payload_str)]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
                }
            }
#endif
        }
    });
}

void Application::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();
    auto& board = Board::GetInstance();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return;
    }
    cJSON_AddStringToObject(root, "state", DeviceStateToMqttString(state));
    cJSON_AddStringToObject(root, "network", NetworkModeToString(board.GetActiveNetworkMode()));
    cJSON_AddStringToObject(root, "reason", "network_disconnected");
    char* event_json = cJSON_PrintUnformatted(root);
    if (event_json != nullptr) {
        auto& mqtt_control = MqttControl::GetInstance();
        if (!mqtt_control.ReportEvent("network_disconnect", event_json)) {
            AbnormalReporter::QueueEvent("network_disconnect", event_json);
        }
        cJSON_free(event_json);
    }
    cJSON_Delete(root);

    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        if (board.GetActiveNetworkMode() != BoardNetworkMode::CELLULAR) {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            } else {
                SetDeviceState(kDeviceStateIdle);
            }
        }
    }

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    SystemInfo::PrintHeapStats();
    startup_activation_completed_ = true;
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    display->HideActivationQrCode();
    audio_service_.PrewarmSpeechPipeline();
    audio_service_.Start();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() { audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS); });
}

/**
 * @brief 激活任务
 * 
 * 执行设备激活流程：
 * 1. 检查资源版本并更新
 * 2. 检查固件版本并升级
 * 3. 初始化协议连接
 */
void Application::ActivationTask() {
    ota_ = std::make_unique<Ota>();

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    while (GetDeviceState() == kDeviceStateActivating && board.IsWifiConfigModeActive()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (GetDeviceState() != kDeviceStateActivating) {
        return;
    }

    CheckNewVersion();
    CheckAssetsVersion();
    if (ota_->HasMqttControlConfig()) {
        MqttControl::GetInstance().Start();
    }
    InitializeProtocol();

    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        return;
    }

    Settings settings("assets", true);
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        // 如果有待下载的资源 URL
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down",
              Lang::Sounds::OGG_UPGRADE);

        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        // 下载并解压资源文件
        bool success = assets.Download(
            download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // 加载资源
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    const int INITIAL_SILENT_RETRY_DELAY_SEC = 2;
    const TickType_t activation_sound_interval = pdMS_TO_TICKS(10000);
    int retry_count = 0;
    int retry_delay = 10;
    int activation_sound_play_count = 0;
    TickType_t last_activation_sound_finished_tick = 0;
    TickType_t last_activation_cloud_check_tick = 0;
    uint32_t handled_ble_disconnect_generation =
        GetBleBindClientDisconnectGeneration();
    bool waiting_for_activation = false;
    bool activation_bind_ui_shown = false;
    bool activation_status_ui_shown = false;
    std::string last_activation_status_message;

    auto& board = Board::GetInstance();
    int silent_retry_count =
        board.GetActiveNetworkMode() == BoardNetworkMode::CELLULAR ? 2 : 0;
    while (true) {
        auto display = board.GetDisplay();
        if (!waiting_for_activation) {
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
        } else {
            const TickType_t now = xTaskGetTickCount();
            const bool fast_activation_check_pending =
                GetBleBindClientDisconnectGeneration() != handled_ble_disconnect_generation;
            if (board.IsBleBindModeActive() && IsBleBindClientConnected()) {
                vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
                continue;
            }
            if (!fast_activation_check_pending && last_activation_cloud_check_tick != 0 &&
                now - last_activation_cloud_check_tick <
                    pdMS_TO_TICKS(kBleBindCloudPollIntervalMs)) {
                vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
                continue;
            }
        }

        // 检查服务器是否有新版本
        const bool paused_ble_for_check =
            waiting_for_activation && PauseBleBindModeBeforeCloudCheck(board);
        if (waiting_for_activation && !paused_ble_for_check &&
            board.IsBleBindModeActive()) {
            vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
            continue;
        }
        if (waiting_for_activation) {
            handled_ble_disconnect_generation = GetBleBindClientDisconnectGeneration();
        }
        esp_err_t err = ota_->CheckVersion();
        if (waiting_for_activation) {
            last_activation_cloud_check_tick = xTaskGetTickCount();
        }
        if (err != ESP_OK) {
            if (paused_ble_for_check && GetDeviceState() == kDeviceStateActivating &&
                !board.EnterBleBindMode()) {
                Alert(Lang::Strings::ERROR, Lang::Strings::BLUFI_INIT_FAILED,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
            }
            retry_count++;
            const bool use_cached_config_on_failure =
                !waiting_for_activation &&
                board.GetActiveNetworkMode() == BoardNetworkMode::WIFI &&
                HasCachedProtocolConfig();
            const int max_retry = use_cached_config_on_failure ? 1 : MAX_RETRY;
            if (retry_count >= max_retry) {
                if (use_cached_config_on_failure) {
                    ESP_LOGW(TAG,
                             "Version check failed, continue with cached protocol config: "
                             "code=%d, url=%s",
                             err, ota_->GetCheckVersionUrl().c_str());
                    return;
                }
                if (!waiting_for_activation) {
                    return;
                }
                ESP_LOGW(TAG,
                         "Version check failed while activation is pending, keep waiting: "
                         "code=%d, url=%s",
                         err, ota_->GetCheckVersionUrl().c_str());
                retry_count = 0;
            }

            // 失败重试逻辑
            int wait_seconds = retry_delay;
            bool used_silent_retry = false;
            if (silent_retry_count > 0) {
                silent_retry_count--;
                wait_seconds = INITIAL_SILENT_RETRY_DELAY_SEC;
                used_silent_retry = true;
            } else {
                char error_message[128];
                snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                         ota_->GetCheckVersionUrl().c_str());
                char buffer[256];
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         wait_seconds, error_message);
                Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);
            }

            for (int i = 0; i < wait_seconds; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            if (!used_silent_retry) {
                retry_delay = std::min(retry_delay * 2, kActivationRetryDelayMaxSec);
            }
            continue;
        }
        retry_count = 0;
        retry_delay = 10;

        // 如果有新版本，启动固件升级
        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;
            }
        }

        ota_->MarkCurrentVersionValid();
        // 检查激活状态
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            if (board.IsBleBindModeActive()) {
                board.ExitBleBindMode();
            }
            break;
        }

        if (!waiting_for_activation && ota_->HasActivationCode()) {
            last_activation_cloud_check_tick = xTaskGetTickCount();
        }
        waiting_for_activation = true;
        display->SetStatus(Lang::Strings::ACTIVATION);

        if (ota_->HasActivationCode()) {
            if ((!activation_bind_ui_shown || !board.IsBleBindModeActive()) &&
                !board.EnterBleBindMode()) {
                Alert(Lang::Strings::ERROR, Lang::Strings::BLUFI_INIT_FAILED,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            activation_bind_ui_shown = true;
            activation_status_ui_shown = true;

            const TickType_t now = xTaskGetTickCount();
            const bool should_play_sound =
                activation_sound_play_count == 0 ||
                (activation_sound_play_count < 3 &&
                 (now - last_activation_sound_finished_tick) >= activation_sound_interval);
            if (should_play_sound) {
                audio_service_.PlaySound(Lang::Sounds::OGG_ACTIVATION);
                ++activation_sound_play_count;
                if (!audio_service_.WaitForPlaybackQueueEmpty(5000)) {
                    ESP_LOGW(TAG, "Timed out waiting for activation sound playback");
                }
                last_activation_sound_finished_tick = xTaskGetTickCount();
            }

            vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
            continue;
        }

        // 尝试激活设备
        std::string activation_message = ota_->GetActivationMessage();
        if (activation_message.empty()) {
            activation_message = Lang::Strings::PLEASE_WAIT;
        }
        if (!activation_status_ui_shown || activation_message != last_activation_status_message) {
            ShowActivationStatus(activation_message, "microchip_ai");
            activation_status_ui_shown = true;
            last_activation_status_message = activation_message;
        }

        for (int i = 0; i < 10; ++i) {
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    protocol_ = std::make_unique<WebsocketProtocol>();
    const uint32_t generation = protocol_generation_.fetch_add(1) + 1;
    SetupProtocolCallbacks(display, codec, board, generation);

    protocol_->Start();

}

void Application::ShowActivationCode(const std::string& code, const std::string& message,
                                     bool play_sound) {
    auto display = Board::GetInstance().GetDisplay();
    display->ShowActivationQrCode(code.c_str());

    Alert(Lang::Strings::ACTIVATION, "", "",
          play_sound ? Lang::Sounds::OGG_ACTIVATION : std::string_view{});
}

void Application::ShowActivationStatus(const std::string& message, const char* emotion) {
    auto display = Board::GetInstance().GetDisplay();
    const char* detail = message.empty() ? Lang::Strings::PLEASE_WAIT : message.c_str();
    display->ShowActivationPrompt(detail);
    display->SetStatus(Lang::Strings::ACTIVATION);
    display->SetEmotion(emotion);
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ShowPendingIdleNotification(Display* display) {
    if (display == nullptr || pending_idle_notification_.empty()) {
        return;
    }

    const int duration_ms = pending_idle_notification_duration_ms_ > 0
                                ? pending_idle_notification_duration_ms_
                                : static_cast<int>(kAiChatConnectionErrorNoticeMs);
    std::string notification = std::move(pending_idle_notification_);
    pending_idle_notification_.clear();
    pending_idle_notification_duration_ms_ = 0;
    display->ShowNotification(notification.c_str(), duration_ms);
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();

    if ((state == kDeviceStateIdle || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking) &&
        !IsWakeWordAllowedOnCurrentScreen()) {
        RefreshWakeWordDetection();
        return;
    }

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }

        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);

        while (audio_service_.PopPacketFromSendQueue()) {
        }

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            RefreshWakeWordDetection();
        } else {
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    if (state != kDeviceStateConnecting &&
        !(state == kDeviceStateIdle && protocol_->IsAudioChannelOpened())) {
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }

    protocol_->SendWakeWordDetected(wake_word);
#endif

    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

bool Application::IsWakeWordAllowedOnCurrentScreen() {
#if HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    auto lcd_display = dynamic_cast<LcdDisplay*>(display);
    if (lcd_display != nullptr) {
        return lcd_display->IsSmartWatchAiChatActive();
    }
#endif
    return true;
}

void Application::RefreshWakeWordDetection() {
    bool enable = false;
    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        enable = IsWakeWordAllowedOnCurrentScreen();
    } else if (state == kDeviceStateListening) {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
        enable = IsWakeWordAllowedOnCurrentScreen() && audio_service_.IsAfeWakeWord();
#else
        enable = false;
#endif
    } else if (state == kDeviceStateSpeaking) {
        enable = IsWakeWordAllowedOnCurrentScreen() && audio_service_.IsAfeWakeWord();
    }

    audio_service_.EnableWakeWordDetection(enable);
}

void Application::ExitAiChatToStandby() {
    const auto state = GetDeviceState();
    const bool is_ai_runtime_state =
        state == kDeviceStateIdle || state == kDeviceStateConnecting ||
        state == kDeviceStateListening || state == kDeviceStateSpeaking;
    // UI navigation posts this call asynchronously. Ignore a stale AI-chat exit
    // after another flow has already entered activation, provisioning, or upgrade.
    if (!is_ai_runtime_state) {
        return;
    }

    play_popup_on_listening_ = false;
    keep_ai_chat_visible_on_idle_ = false;
    idle_assistant_message_.clear();
    listening_silence_ticks_ = 0;

    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel(false);
    }

    if (state != kDeviceStateIdle) {
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * @brief 处理设备状态变化事件
 * 
 * 根据新状态更新显示、LED、音频服务等组件的行为。
 */
void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
#if HAVE_LVGL
    auto lcd_display = dynamic_cast<LcdDisplay*>(display);
#endif
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            listening_silence_ticks_ = 0;
            display->SetStatus(Lang::Strings::STANDBY);
            if (keep_ai_chat_visible_on_idle_) {
                if (!idle_assistant_message_.empty()) {
                    display->SetChatMessage("assistant", idle_assistant_message_.c_str());
                }
            } else {
                display->ClearChatMessages();
            }
            display->SetEmotion("neutral");
            ShowPendingIdleNotification(display);
#if HAVE_LVGL
            if (lcd_display != nullptr) {
                lcd_display->SetEmojiVisible(false);
            }
#endif
            audio_service_.EnableVoiceProcessing(false);
            RefreshWakeWordDetection();
            break;
        case kDeviceStateConnecting:
            keep_ai_chat_visible_on_idle_ = false;
            idle_assistant_message_.clear();
            listening_silence_ticks_ = 0;
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
#if HAVE_LVGL
            if (lcd_display != nullptr) {
                lcd_display->SetEmojiVisible(false);
            }
#endif
            display->ClearChatMessages();
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            keep_ai_chat_visible_on_idle_ = false;
            idle_assistant_message_.clear();
            listening_silence_ticks_ = 0;
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
#if HAVE_LVGL
            if (lcd_display != nullptr) {
                lcd_display->SetEmojiVisible(false);
            }
#endif

            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                if (listening_mode_ == kListeningModeAutoStop &&
                    !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            keep_ai_chat_visible_on_idle_ = false;
            idle_assistant_message_.clear();
            listening_silence_ticks_ = 0;
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetEmotion("neutral");
#if HAVE_LVGL
            if (lcd_display != nullptr) {
                lcd_display->SetEmojiVisible(false);
            }
#endif

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                RefreshWakeWordDetection();
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
        case kDeviceStateActivating:
#if HAVE_LVGL
            if (lcd_display != nullptr) {
                lcd_display->SetEmojiVisible(false);
            }
#endif
            listening_silence_ticks_ = 0;
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            listening_silence_ticks_ = 0;
            break;
    }
}

void Application::StartListeningAudio() {
    if (GetDeviceState() != kDeviceStateListening || protocol_ == nullptr) {
        return;
    }

    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);
    ConfigureWakeWordForListening();

    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    RefreshWakeWordDetection();
#else
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::PublishMqttTelemetry() {
    auto& mqtt_control = MqttControl::GetInstance();
    if (!mqtt_control.IsConnected()) {
        return;
    }

    AbnormalReporter::PublishPendingEvents();

    auto& board = Board::GetInstance();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate telemetry JSON");
        return;
    }
    cJSON_AddStringToObject(root, "deviceId", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "state", DeviceStateToMqttString(GetDeviceState()));
    cJSON_AddStringToObject(root, "role", board.GetDeviceRole().c_str());
    cJSON_AddStringToObject(root, "network_version", board.GetDeviceNetworkVersion().c_str());
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(time(nullptr)));

    // 添加背光亮度信息
    if (auto* backlight = board.GetBacklight(); backlight != nullptr) {
        cJSON_AddNumberToObject(root, "brightness", backlight->brightness());
    }

    // 添加音频信息（音量、是否静音）
    if (auto* codec = board.GetAudioCodec(); codec != nullptr) {
        cJSON_AddNumberToObject(root, "volume", codec->output_volume());
        cJSON_AddBoolToObject(root, "muted", codec->output_volume() == 0);
    }

    // 添加电池信息
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        if (battery != nullptr) {
            cJSON_AddNumberToObject(battery, "level", battery_level);
            cJSON_AddBoolToObject(battery, "charging", charging);
            cJSON_AddBoolToObject(battery, "discharging", discharging);
            cJSON_AddItemToObject(root, "battery", battery);
        }
    }

    char* json = cJSON_PrintUnformatted(root);
    mqtt_control.ReportTelemetry(json);
    cJSON_free(json);
    cJSON_Delete(root);
}

void Application::HandleMqttCommand(const char* json, int len) {
    auto& mqtt_control = MqttControl::GetInstance();

    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid MQTT command payload");
        mqtt_control.ReportEvent("cmd_invalid", "{\"reason\":\"invalid_json\"}");
        return;
    }

    cJSON* msg_id = cJSON_GetObjectItem(root, "msgId");
    cJSON* method = cJSON_GetObjectItem(root, "method");
    cJSON* params = cJSON_GetObjectItem(root, "params");

    bool success = false;
    std::string reason = "unsupported_command";
    cJSON* command_result = nullptr;
    bool should_publish_telemetry = false;

    // 解析并执行 MQTT 指令
    if (!cJSON_IsString(msg_id) || msg_id->valuestring == nullptr ||
        !cJSON_IsString(method) || method->valuestring == nullptr) {
        reason = "missing_msgId_or_method";
    } else if (strcmp(method->valuestring, "device_info") == 0) {
        command_result = BuildMqttDeviceInfoResult();
        if (command_result == nullptr) {
            reason = "device_info_build_failed";
        } else {
            success = true;
        }
    } else if (strcmp(method->valuestring, "set_brightness") == 0) {
        // 设置亮度
        cJSON* value = cJSON_GetObjectItem(params, "value");
        auto* backlight = Board::GetInstance().GetBacklight();
        if (!cJSON_IsNumber(value) || backlight == nullptr) {
            reason = "invalid_brightness";
        } else {
            int brightness = value->valueint;
            if (brightness < 0) {
                brightness = 0;
            } else if (brightness > 100) {
                brightness = 100;
            }
            backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
            success = true;
            should_publish_telemetry = true;
        }
    } else if (strcmp(method->valuestring, "set_volume") == 0) {
        // 设置音量
        cJSON* value = cJSON_GetObjectItem(params, "value");
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (!cJSON_IsNumber(value) || codec == nullptr) {
            reason = "invalid_volume";
        } else {
            int volume = value->valueint;
            if (volume < 0) {
                volume = 0;
            } else if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume, true);
            success = true;
            should_publish_telemetry = true;
        }
    } else if (strcmp(method->valuestring, "mute") == 0) {
        // 静音控制
        cJSON* enable = cJSON_GetObjectItem(params, "enable");
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (!cJSON_IsBool(enable) || codec == nullptr) {
            reason = "invalid_mute";
        } else {
            Settings audio_settings("audio", true);
            if (cJSON_IsTrue(enable)) {
                const int current_volume = codec->output_volume();
                if (current_volume > 0) {
                    audio_settings.SetInt("pre_mute_volume", current_volume);
                }
                codec->SetOutputVolume(0, true);
            } else {
                int restore_volume = audio_settings.GetInt("pre_mute_volume", 100);
                if (restore_volume <= 0 || restore_volume > 100) {
                    restore_volume = 100;
                }
                codec->SetOutputVolume(restore_volume, true);
            }
            success = true;
            should_publish_telemetry = true;
        }
    } else if (strcmp(method->valuestring, "reboot") == 0) {
        // 重启设备
        success = true;
        Schedule([this]() { Reboot(); });
    } else if (strcmp(method->valuestring, "unbind") == 0) {
        ClearCloudBindingSettings();
        success = true;
        Schedule([this]() {
            ExitAiChatToStandby();
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel(false);
            }
            protocol_generation_.fetch_add(1);
            protocol_.reset();
            MqttControl::GetInstance().StopForNetworkSwitch();

            auto& board = Board::GetInstance();
            bool expected = false;
            if (!g_ble_bind_wait_in_progress.compare_exchange_strong(expected, true)) {
                return;
            }

            SetDeviceState(kDeviceStateActivating);
            ShowActivationStatus(Lang::Strings::PLEASE_WAIT, "bluetooth");
            PlaySound(Lang::Sounds::OGG_ACTIVATION);
            if (!audio_service_.WaitForPlaybackQueueEmpty(5000)) {
                ESP_LOGW(TAG, "Timed out waiting for activation sound playback");
            }
            audio_service_.Stop();
            if (!audio_service_.WaitForStopped(5000)) {
                g_ble_bind_wait_in_progress = false;
                board.GetDisplay()->HideActivationQrCode();
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, Lang::Strings::BLUFI_INIT_FAILED,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                return;
            }

            const uint32_t initial_ble_disconnect_generation =
                GetBleBindClientDisconnectGeneration();
            if (!board.EnterBleBindMode()) {
                g_ble_bind_wait_in_progress = false;
                board.ExitBleBindMode();
                board.GetDisplay()->HideActivationQrCode();
                audio_service_.Start();
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, Lang::Strings::BLUFI_INIT_FAILED,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                return;
            }

            struct BleBindWaitTaskCtx {
                Application* app;
                uint32_t initial_ble_disconnect_generation;
            };
            auto* ctx = new (std::nothrow)
                BleBindWaitTaskCtx{this, initial_ble_disconnect_generation};
            if (ctx == nullptr ||
                xTaskCreate(
                    [](void* arg) {
                        auto* ctx = static_cast<BleBindWaitTaskCtx*>(arg);
                        auto* app = ctx->app;
                        uint32_t handled_ble_disconnect_generation =
                            ctx->initial_ble_disconnect_generation;
                        delete ctx;

                        int attempt = 0;
                        TickType_t last_cloud_check_tick = xTaskGetTickCount();
                        while (app->GetDeviceState() == kDeviceStateActivating) {
                            auto& board = Board::GetInstance();
                            if (!board.IsBleBindModeActive()) {
                                app->Schedule([app]() {
                                    Board::GetInstance().GetDisplay()->HideActivationQrCode();
                                    app->GetAudioService().Start();
                                    app->SetDeviceState(kDeviceStateIdle);
                                    app->Alert(Lang::Strings::ERROR,
                                               Lang::Strings::BLUFI_INIT_FAILED,
                                               "triangle_exclamation",
                                               Lang::Sounds::OGG_EXCLAMATION);
                                });
                                break;
                            }

                            const TickType_t now = xTaskGetTickCount();
                            const bool fast_activation_check_pending =
                                GetBleBindClientDisconnectGeneration() !=
                                handled_ble_disconnect_generation;
                            const bool ble_client_connected = IsBleBindClientConnected();
                            const bool should_check_cloud =
                                !ble_client_connected &&
                                (fast_activation_check_pending ||
                                 now - last_cloud_check_tick >=
                                     pdMS_TO_TICKS(kBleBindCloudPollIntervalMs));
                            if (should_check_cloud) {
                                const bool paused_ble_for_check =
                                    PauseBleBindModeBeforeCloudCheck(board);
                                if (!paused_ble_for_check && board.IsBleBindModeActive()) {
                                    vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
                                    continue;
                                }
                                handled_ble_disconnect_generation =
                                    GetBleBindClientDisconnectGeneration();
                                Ota ota;
                                const bool rebind_done =
                                    ota.CheckVersion() == ESP_OK && !ota.HasActivationCode() &&
                                    !ota.HasActivationChallenge();
                                last_cloud_check_tick = xTaskGetTickCount();
                                if (rebind_done) {
                                    app->HandleRebindSuccess();
                                    break;
                                }
                                if (paused_ble_for_check &&
                                    app->GetDeviceState() == kDeviceStateActivating &&
                                    !board.EnterBleBindMode()) {
                                    app->Schedule([app]() {
                                        auto display = Board::GetInstance().GetDisplay();
                                        display->HideActivationQrCode();
                                        app->GetAudioService().Start();
                                        app->SetDeviceState(kDeviceStateIdle);
                                        app->Alert(Lang::Strings::ERROR,
                                                   Lang::Strings::BLUFI_INIT_FAILED,
                                                   "triangle_exclamation",
                                                   Lang::Sounds::OGG_EXCLAMATION);
                                    });
                                    break;
                                }
                            }
                            ++attempt;
                            if (attempt % kBleBindTimeoutNotifyAttempts == 0) {
                                std::string notice = BuildBleBindTimeoutNotice();
                                app->Schedule([app, notice = std::move(notice)]() {
                                    if (app->GetDeviceState() != kDeviceStateActivating ||
                                        !g_ble_bind_wait_in_progress.load()) {
                                        return;
                                    }
                                    auto display = Board::GetInstance().GetDisplay();
                                    display->ShowActivationPrompt(notice.c_str());
                                });
                                vTaskDelay(pdMS_TO_TICKS(kBleBindTimeoutNoticeMs));
                                app->Schedule([app]() {
                                    if (app->GetDeviceState() == kDeviceStateActivating) {
                                        Board::GetInstance().EnterBleBindMode();
                                    }
                                });
                            }
                            vTaskDelay(pdMS_TO_TICKS(kBleBindPollIntervalMs));
                        }

                        g_ble_bind_wait_in_progress = false;
                        vTaskDelete(nullptr);
                    },
                    "ble_bind_wait", 6144, ctx, 4, nullptr) != pdPASS) {
                delete ctx;
                g_ble_bind_wait_in_progress = false;
                board.ExitBleBindMode();
                board.GetDisplay()->HideActivationQrCode();
                audio_service_.Start();
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, Lang::Strings::BLUFI_INIT_FAILED,
                      "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
            }
        });
    } else if (strcmp(method->valuestring, "bind_success") == 0) {
        success = true;
        HandleRebindSuccess();
    } else if (strcmp(method->valuestring, "wake_word_set_all") == 0 ||
               strcmp(method->valuestring, "wake_word_sync") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            std::vector<WakeWordConfig> configs;
            if (ParseMqttWakeWordConfigs(params, &configs, &reason)) {
                if (!wake_word->SetWakeWordConfigs(configs)) {
                    reason = "wake_word_set_all_failed";
                } else {
                    success = true;
                    command_result = BuildMqttWakeWordConfigsResult(wake_word);
                }
            }
        }
    } else if (strcmp(method->valuestring, "wake_word_get_configs") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            command_result = BuildMqttWakeWordConfigsResult(wake_word);
            if (command_result == nullptr) {
                reason = "wake_word_result_build_failed";
            } else {
                success = true;
            }
        }
    } else if (strcmp(method->valuestring, "wake_word_add") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            std::string command;
            std::string display_text;
            std::string action = "wake";
            if (!GetMqttStringParam(params, "command", &command)) {
                reason = "missing_command";
            } else if (!GetMqttWakeWordDisplayText(params, &display_text)) {
                reason = "missing_display_text";
            } else {
                GetMqttStringParam(params, "action", &action);
                WakeWordConfig config{command, display_text, action};
                if (!wake_word->AddWakeWord(config)) {
                    reason = "wake_word_add_failed";
                } else {
                    success = true;
                    command_result = BuildMqttWakeWordConfigsResult(wake_word);
                }
            }
        }
    } else if (strcmp(method->valuestring, "wake_word_remove") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            std::string command;
            if (!GetMqttStringParam(params, "command", &command)) {
                reason = "missing_command";
            } else if (!wake_word->RemoveWakeWord(command)) {
                reason = "wake_word_remove_failed";
            } else {
                success = true;
                command_result = BuildMqttWakeWordConfigsResult(wake_word);
            }
        }
    } else if (strcmp(method->valuestring, "wake_word_set_threshold") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            float threshold = 0.0f;
            if (ParseMqttWakeWordThreshold(params, &threshold, &reason)) {
                if (!wake_word->SetWakeWordThreshold(threshold)) {
                    reason = "invalid_threshold";
                } else {
                    success = true;
                    command_result = BuildMqttWakeWordThresholdResult(wake_word);
                }
            }
        }
    } else if (strcmp(method->valuestring, "wake_word_get_threshold") == 0) {
        auto* wake_word = GetMqttCustomWakeWord(&reason);
        if (wake_word != nullptr) {
            command_result = BuildMqttWakeWordThresholdResult(wake_word);
            if (command_result == nullptr) {
                reason = "wake_word_result_build_failed";
            } else {
                success = true;
            }
        }
    } else if (strcmp(method->valuestring, "skin_update") == 0) {
#if HAVE_LVGL
        SkinMaterialParseResult parsed;
        if (!ParseSkinUpdateParams(params, &parsed)) {
            reason = "invalid_skin_update";
        } else {
            bool expected = false;
            if (!g_skin_download_in_progress.compare_exchange_strong(expected, true)) {
                reason = "skin_update_in_progress";
            } else {
                auto* payload = new (std::nothrow) SkinUpdateTaskPayload();
                if (payload == nullptr) {
                    g_skin_download_in_progress.store(false);
                    reason = "skin_update_task_alloc_failed";
                } else {
                    payload->app = this;
                    payload->parsed = std::move(parsed);
                    if (xTaskCreate(
                            [](void* arg) {
                                DownloadServerBackgroundTask(arg);
                            },
                            "mqtt_skin", 8192, payload, 5, nullptr) != pdPASS) {
                        delete payload;
                        g_skin_download_in_progress.store(false);
                        reason = "skin_update_task_create_failed";
                    } else {
                        success = true;
                    }
                }
            }
        }
#else
        reason = "skin_update_not_supported";
#endif
    } else if (strcmp(method->valuestring, "switch_role") == 0) {
#if HAVE_LVGL
        RoleSwitchParseResult parsed;
        if (!ParseRoleSwitchParams(params, &parsed)) {
            reason = "invalid_switch_role";
        } else {
            bool expected = false;
            if (!g_role_download_in_progress.compare_exchange_strong(expected, true)) {
                reason = "switch_role_in_progress";
            } else {
                auto* payload = new (std::nothrow) RoleSwitchTaskPayload();
                if (payload == nullptr) {
                    g_role_download_in_progress.store(false);
                    reason = "switch_role_task_alloc_failed";
                } else {
                    payload->app = this;
                    payload->parsed = std::move(parsed);
                    if (xTaskCreate(
                            [](void* arg) {
                                DownloadRoleMjpegTask(arg);
                            },
                            "mqtt_role", 8192, payload, 5, nullptr) != pdPASS) {
                        delete payload;
                        g_role_download_in_progress.store(false);
                        reason = "switch_role_task_create_failed";
                    } else {
                        success = true;
                    }
                }
            }
        }
#else
        reason = "switch_role_not_supported";
#endif
    }

    // 发送指令应答 (Ack)
    if (cJSON_IsString(msg_id) && msg_id->valuestring != nullptr) {
        cJSON* ack = cJSON_CreateObject();
        if (ack != nullptr) {
            cJSON_AddStringToObject(ack, "msgId", msg_id->valuestring);
            cJSON_AddStringToObject(ack, "status", success ? "success" : "failed");
            if (success && command_result != nullptr &&
                cJSON_IsString(method) && method->valuestring != nullptr) {
                cJSON_AddStringToObject(ack, "method", method->valuestring);
                cJSON_AddItemToObject(ack, "result", command_result);
                command_result = nullptr;
            }
            char* ack_json = cJSON_PrintUnformatted(ack);
            mqtt_control.ReportAck(ack_json);
            cJSON_free(ack_json);
            cJSON_Delete(ack);
        }
    }

    // 上报指令执行事件
    cJSON* event = cJSON_CreateObject();
    if (event != nullptr) {
        if (cJSON_IsString(msg_id) && msg_id->valuestring != nullptr) {
            cJSON_AddStringToObject(event, "msgId", msg_id->valuestring);
        }
        if (cJSON_IsString(method) && method->valuestring != nullptr) {
            cJSON_AddStringToObject(event, "method", method->valuestring);
        }
        cJSON_AddStringToObject(event, "status", success ? "success" : "failed");
        if (!success) {
            cJSON_AddStringToObject(event, "reason", reason.c_str());
        }
        char* event_json = cJSON_PrintUnformatted(event);
        mqtt_control.ReportEvent(success ? "cmd_executed" : "cmd_failed", event_json);
        cJSON_free(event_json);
        cJSON_Delete(event);
    }

    if (success && should_publish_telemetry) {
        PublishMqttTelemetry();
    }

    if (command_result != nullptr) {
        cJSON_Delete(command_result);
    }
    cJSON_Delete(root);
}

void Application::Reboot() {
    bool expected = false;
    if (!reboot_in_progress_.compare_exchange_strong(expected, true)) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->ShowNotification("正在重启...", kRebootNotificationMs);

    // Freeze voice interactions immediately so the UI can settle before reboot.
    play_popup_on_listening_ = false;
    keep_ai_chat_visible_on_idle_ = false;
    idle_assistant_message_.clear();
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);

    BaseType_t ok = xTaskCreate(RebootTask, "app_reboot", 4096, this, tskIDLE_PRIORITY + 5,
                                &reboot_task_handle_);
    if (ok != pdPASS) {
        reboot_task_handle_ = nullptr;
        vTaskDelay(pdMS_TO_TICKS(kRebootUiReactionMs));
        FinishReboot();
    }
}

void Application::RebootTask(void* arg) {
    auto* self = static_cast<Application*>(arg);

    vTaskDelay(pdMS_TO_TICKS(kRebootUiReactionMs));
    self->reboot_task_handle_ = nullptr;
    self->Schedule([self]() {
        self->FinishReboot();
    });
    vTaskDelete(nullptr);
}

void Application::FinishReboot() {
    if (!reboot_in_progress_.load()) {
        return;
    }

    AbnormalReporter::MarkExpectedReset("reboot");
    // Reboot should prefer fast teardown without blocking the application event loop.
    MqttControl::GetInstance().StopForNetworkSwitch();
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel(false);
    }
    protocol_.reset();
    audio_service_.Stop();
    if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
        codec->SetOutputVolume(0, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        codec->EnableOutput(false);
        vTaskDelay(pdMS_TO_TICKS(kRebootAudioShutdownDelayMs));
    }

    if (auto* backlight = Board::GetInstance().GetBacklight(); backlight != nullptr) {
        // Fade the screen out before the hardware reset to reduce perceived flash.
        backlight->SetBrightness(0);
    }

    vTaskDelay(pdMS_TO_TICKS(kRebootFinalizeDelayMs));
    esp_restart();
}

void Application::ClearOtaNetworkSwitchRequest() {
    ota_network_switch_pending_.store(false);
    ota_upgrade_cancel_requested_.store(false);
    ota_network_switch_target_.store(static_cast<int>(BoardNetworkMode::UNSUPPORTED));
    ota_network_switch_start_generation_.store(0);
}

void Application::NotifyOtaNetworkSwitchRequested(BoardNetworkMode target) {
    if (!ota_upgrade_in_progress_.load() || target == BoardNetworkMode::UNSUPPORTED) {
        return;
    }

    ota_network_switch_target_.store(static_cast<int>(target));
    ota_network_switch_start_generation_.store(network_connected_generation_.load());
    ota_network_switch_pending_.store(true);
    ota_upgrade_cancel_requested_.store(true);
}

bool Application::RequestOtaNetworkSwitch(BoardNetworkMode target) {
    if (target == BoardNetworkMode::UNSUPPORTED) {
        return false;
    }

    auto& board = Board::GetInstance();
    if (board.GetActiveNetworkMode() == target) {
        return true;
    }

    NotifyOtaNetworkSwitchRequested(target);
    if (!board.SwitchActiveNetworkMode(target)) {
        ClearOtaNetworkSwitchRequest();
        return false;
    }

    return true;
}

bool Application::WaitForOtaNetworkConnected(BoardNetworkMode target, uint32_t min_generation,
                                             int timeout_ms) {
    if (target == BoardNetworkMode::UNSUPPORTED) {
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    BoardNetworkMode waiting_target = target;
    uint32_t waiting_generation = min_generation;

    while (true) {
        BoardNetworkMode requested_target = static_cast<BoardNetworkMode>(
            ota_network_switch_target_.load());
        uint32_t requested_generation = ota_network_switch_start_generation_.load();
        if (requested_target != BoardNetworkMode::UNSUPPORTED &&
            (requested_target != waiting_target || requested_generation != waiting_generation)) {
            ESP_LOGI(TAG, "OTA network switch target updated while waiting: %s -> %s",
                     NetworkModeToString(waiting_target), NetworkModeToString(requested_target));
            waiting_target = requested_target;
            waiting_generation = requested_generation;
            start = xTaskGetTickCount();
        }

        if (network_connected_generation_.load() != waiting_generation &&
            last_connected_network_mode_.load() == static_cast<int>(waiting_target)) {
            return true;
        }

        if (timeout_ms >= 0 && (xTaskGetTickCount() - start) >= timeout_ticks) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;
    DeviceState state_after_failure =
        GetDeviceState() == kDeviceStateActivating ? kDeviceStateActivating : kDeviceStateIdle;

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    ota_upgrade_in_progress_.store(true);
    ClearOtaNetworkSwitchRequest();

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    auto wait_for_requested_network = [&]() {
        BoardNetworkMode target = static_cast<BoardNetworkMode>(
            ota_network_switch_target_.load());
        uint32_t switch_generation = ota_network_switch_start_generation_.load();
        const char* switch_status = target == BoardNetworkMode::CELLULAR
            ? Lang::Strings::SWITCH_TO_4G_NETWORK
            : Lang::Strings::SWITCH_TO_WIFI_NETWORK;

        SetDeviceState(kDeviceStateActivating);
        display->SetStatus(switch_status);
        display->SetChatMessage("system", "等待网络连接...");

        if (!WaitForOtaNetworkConnected(target, switch_generation, 120000)) {
            ESP_LOGE(TAG, "Timed out waiting for OTA network switch target");
            return false;
        }

        ClearOtaNetworkSwitchRequest();
        SetDeviceState(kDeviceStateUpgrading);
        display->SetStatus(Lang::Strings::OTA_UPGRADE);
        display->SetChatMessage("system", message.c_str());
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        return true;
    };

    while (true) {
        if (ota_network_switch_pending_.load()) {
            if (!wait_for_requested_network()) {
                break;
            }
            continue;
        }

        ota_upgrade_cancel_requested_.store(false);

        auto upgrade_result = Ota::UpgradeWithResult(
            upgrade_url,
            [this, display](int progress, size_t speed) {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            },
            [this]() { return ota_upgrade_cancel_requested_.load(); });

        if (upgrade_result == Ota::UpgradeResult::Success) {
            ota_upgrade_in_progress_.store(false);
            ClearOtaNetworkSwitchRequest();
            display->SetChatMessage("system", "Upgrade successful, rebooting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            Reboot();
            return true;
        }

        if (upgrade_result == Ota::UpgradeResult::Cancelled &&
            ota_network_switch_pending_.load()) {
            if (!wait_for_requested_network()) {
                break;
            }
            continue;
        }

        break;
    }

    ota_upgrade_in_progress_.store(false);
    ClearOtaNetworkSwitchRequest();
    audio_service_.Start();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark",
          Lang::Sounds::OGG_EXCLAMATION);
    vTaskDelay(pdMS_TO_TICKS(3000));
    SetDeviceState(state_after_failure);
    return false;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }

        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::HandleRebindSuccess() {
    Schedule([this]() {
        auto& board = Board::GetInstance();
        board.ExitBleBindMode();
        startup_activation_completed_ = true;
        SetDeviceState(kDeviceStateIdle);
        auto display = board.GetDisplay();
        display->HideActivationQrCode();
        audio_service_.Start();
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

        Settings mqtt_ctrl_settings("mqtt_ctrl", false);
        const bool has_mqtt_control_config =
            !mqtt_ctrl_settings.GetString("endpoint").empty() ||
            !mqtt_ctrl_settings.GetString("cellular_endpoint").empty();
        if (has_mqtt_control_config) {
            MqttControl::GetInstance().Start();
        }

        if (protocol_ == nullptr) {
            RestartProtocolFromSettings();
        }
    });
}

void Application::ResetProtocol() {
    Schedule([this]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        protocol_generation_.fetch_add(1);
        protocol_.reset();
    });
}

void Application::ResetProtocolSync(int timeout_ms) {
    TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    Schedule([this, waiter]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel(false);
        }
        protocol_generation_.fetch_add(1);
        protocol_.reset();
        if (!reboot_in_progress_.load()) {
            SetDeviceState(kDeviceStateIdle);
        }
        if (waiter != nullptr) {
            xTaskNotifyGive(waiter);
        }
    });

    if (waiter == nullptr) {
        return;
    }
    TickType_t wait_ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
}
