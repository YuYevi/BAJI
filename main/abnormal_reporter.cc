#include "abnormal_reporter.h"

#include "mqtt_control.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>

namespace {

constexpr const char* kSettingsNamespace = "abnormal";
constexpr const char* kPendingEventsKey = "pending";
constexpr const char* kCleanShutdownKey = "clean";
constexpr const char* kExpectedResetKey = "expected";
constexpr int kMaxPendingEvents = 8;

const char* ResetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "unknown";
        case ESP_RST_POWERON:
            return "power_on";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_DEEPSLEEP:
            return "deep_sleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        default:
            return "other";
    }
}

bool ShouldReportReset(bool previous_shutdown_clean, esp_reset_reason_t reason) {
    if (!previous_shutdown_clean) {
        return true;
    }

    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

cJSON* LoadPendingEvents(Settings& settings) {
    const std::string pending_json = settings.GetString(kPendingEventsKey);
    if (pending_json.empty()) {
        return cJSON_CreateArray();
    }

    cJSON* root = cJSON_Parse(pending_json.c_str());
    if (!cJSON_IsArray(root)) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        return cJSON_CreateArray();
    }
    return root;
}

void SavePendingEvents(Settings& settings, cJSON* root) {
    if (root == nullptr || !cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
        settings.SetString(kPendingEventsKey, "[]");
        return;
    }

    char* json = cJSON_PrintUnformatted(root);
    if (json == nullptr) {
        return;
    }
    settings.SetString(kPendingEventsKey, json);
    cJSON_free(json);
}

void QueueEventInternal(Settings& settings, const char* type, const char* json) {
    if (type == nullptr || type[0] == '\0' || json == nullptr) {
        return;
    }

    cJSON* events = LoadPendingEvents(settings);
    if (events == nullptr) {
        return;
    }

    while (cJSON_GetArraySize(events) >= kMaxPendingEvents) {
        cJSON_DeleteItemFromArray(events, 0);
    }

    cJSON* event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", type);
    cJSON* parsed = cJSON_Parse(json);
    if (parsed != nullptr) {
        cJSON_AddItemToObject(event, "data", parsed);
    } else {
        cJSON_AddStringToObject(event, "message", json);
    }
    cJSON_AddItemToArray(events, event);

    SavePendingEvents(settings, events);
    cJSON_Delete(events);
}

}  // namespace

namespace AbnormalReporter {

void Initialize() {
    Settings settings(kSettingsNamespace, true);
    const bool previous_shutdown_clean = settings.GetBool(kCleanShutdownKey, true);
    const std::string expected_reset = settings.GetString(kExpectedResetKey);
    const esp_reset_reason_t reset_reason = esp_reset_reason();

    if (ShouldReportReset(previous_shutdown_clean, reset_reason)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "resetReason", ResetReasonToString(reset_reason));
        cJSON_AddBoolToObject(root, "previousShutdownClean", previous_shutdown_clean);
        if (!expected_reset.empty()) {
            cJSON_AddStringToObject(root, "expectedReset", expected_reset.c_str());
        }

        char* json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            QueueEventInternal(settings, "abnormal_reset", json);
            cJSON_free(json);
        }
        cJSON_Delete(root);
    }

    settings.SetBool(kCleanShutdownKey, false);
    settings.EraseKey(kExpectedResetKey);
}

void MarkExpectedReset(const char* reason) {
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(kCleanShutdownKey, true);
    if (reason != nullptr && reason[0] != '\0') {
        settings.SetString(kExpectedResetKey, reason);
    } else {
        settings.EraseKey(kExpectedResetKey);
    }
}

void QueueEvent(const char* type, const char* json) {
    Settings settings(kSettingsNamespace, true);
    QueueEventInternal(settings, type, json);
}

void PublishPendingEvents() {
    auto& mqtt_control = MqttControl::GetInstance();
    if (!mqtt_control.IsConnected()) {
        return;
    }

    Settings settings(kSettingsNamespace, true);
    cJSON* events = LoadPendingEvents(settings);
    if (events == nullptr) {
        return;
    }

    cJSON* remaining = cJSON_CreateArray();
    bool publish_blocked = false;
    const int event_count = cJSON_GetArraySize(events);
    for (int i = 0; i < event_count; ++i) {
        cJSON* item = cJSON_GetArrayItem(events, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        if (publish_blocked) {
            cJSON_AddItemToArray(remaining, cJSON_Duplicate(item, true));
            continue;
        }

        cJSON* type = cJSON_GetObjectItem(item, "type");
        cJSON* data = cJSON_GetObjectItem(item, "data");
        cJSON* message = cJSON_GetObjectItem(item, "message");
        if (!cJSON_IsString(type) || type->valuestring == nullptr) {
            continue;
        }

        char* payload = nullptr;
        if (data != nullptr) {
            payload = cJSON_PrintUnformatted(data);
        } else if (message != nullptr) {
            payload = cJSON_PrintUnformatted(message);
        }
        if (payload == nullptr) {
            continue;
        }

        if (!mqtt_control.ReportEvent(type->valuestring, payload)) {
            publish_blocked = true;
            cJSON_AddItemToArray(remaining, cJSON_Duplicate(item, true));
        }
        cJSON_free(payload);
    }

    SavePendingEvents(settings, remaining);
    cJSON_Delete(remaining);
    cJSON_Delete(events);
}

}  // namespace AbnormalReporter
