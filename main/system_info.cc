#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_pm.h>
#include <esp_task_wdt.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

namespace {

constexpr size_t kPreferredScanChunkSize = 16 * 1024;
constexpr size_t kMinimumScanChunkSize = 1024;
constexpr TickType_t kSchedulerCooperateInterval = pdMS_TO_TICKS(50);

uint8_t* AllocateScanBuffer(size_t& buffer_size) {
    constexpr uint32_t kCapabilitySets[] = {
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT,
    };

    for (uint32_t capabilities : kCapabilitySets) {
        for (buffer_size = kPreferredScanChunkSize; buffer_size >= kMinimumScanChunkSize;
             buffer_size /= 2) {
            auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(buffer_size, capabilities));
            if (buffer != nullptr) {
                return buffer;
            }
        }
    }

    buffer_size = 0;
    return nullptr;
}

void CooperateWithScheduler(TickType_t& last_cooperate_tick) {
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (now - last_cooperate_tick < kSchedulerCooperateInterval) {
        return;
    }

#if CONFIG_ESP_TASK_WDT_EN
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
#endif
    vTaskDelay(1);
    last_cooperate_tick = xTaskGetTickCount();
}

size_t GetPartitionUsedSize(const esp_partition_t* partition, uint8_t* buffer,
                            size_t buffer_size, TickType_t& last_cooperate_tick) {
    if (partition == nullptr || partition->size == 0 || buffer == nullptr || buffer_size == 0) {
        return 0;
    }

    size_t remaining = partition->size;

    while (remaining > 0) {
        const size_t chunk_size = remaining >= buffer_size ? buffer_size : remaining;
        const size_t offset = remaining - chunk_size;
        if (esp_partition_read(partition, offset, buffer, chunk_size) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read partition %s at offset %u", partition->label,
                     static_cast<unsigned>(offset));
            return 0;
        }

        for (size_t i = chunk_size; i > 0; --i) {
            if (buffer[i - 1] != 0xFF) {
                return offset + i;
            }
        }

        remaining = offset;
        CooperateWithScheduler(last_cooperate_tick);
    }

    return 0;
}

}  // namespace

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetFlashUsedSize() {
    size_t buffer_size = 0;
    uint8_t* buffer = AllocateScanBuffer(buffer_size);
    if (buffer == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate flash scan buffer");
        return 0;
    }

    size_t used_size = 0;
    TickType_t last_cooperate_tick = xTaskGetTickCount();
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (it != nullptr) {
        const esp_partition_t* partition = esp_partition_get(it);
        if (partition != nullptr) {
            used_size += GetPartitionUsedSize(partition, buffer, buffer_size, last_cooperate_tick);
        }
        it = esp_partition_next(it);
    }

    heap_caps_free(buffer);
    return used_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

size_t SystemInfo::GetPsramTotalSize() {
#if CONFIG_SPIRAM
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

size_t SystemInfo::GetPsramUsedSize() {
#if CONFIG_SPIRAM
    size_t total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return total_size >= free_size ? (total_size - free_size) : 0;
#else
    return 0;
#endif
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    
}

void SystemInfo::PrintHeapStats() {
    (void)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    (void)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

void SystemInfo::PrintPmLocks() {
    esp_pm_dump_locks(stdout);
}
