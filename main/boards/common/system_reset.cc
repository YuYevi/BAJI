#include "system_reset.h"
#include "abnormal_reporter.h"

#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>


#define TAG "SystemReset"


SystemReset::SystemReset(gpio_num_t reset_nvs_pin, gpio_num_t reset_factory_pin) : reset_nvs_pin_(reset_nvs_pin), reset_factory_pin_(reset_factory_pin) {
    
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << reset_nvs_pin_) | (1ULL << reset_factory_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}


void SystemReset::CheckButtons() {
    if (gpio_get_level(reset_factory_pin_) == 0) {
        
        ResetNvsFlash();
        ResetToFactory();
    }

    if (gpio_get_level(reset_nvs_pin_) == 0) {
        
        ResetNvsFlash();
    }
}

void SystemReset::ResetNvsFlash() {
    
    esp_err_t ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        
    }
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        
    }
}

void SystemReset::ResetToFactory() {
    
    
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (partition == NULL) {
        
        return;
    }
    esp_partition_erase_range(partition, 0, partition->size);
    

    
    RestartInSeconds(3);
}

void SystemReset::RestartInSeconds(int seconds) {
    for (int i = seconds; i > 0; i--) {
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    AbnormalReporter::MarkExpectedReset("system_reset");
    esp_restart();
}
