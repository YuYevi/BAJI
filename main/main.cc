#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"


extern "C" __attribute__((weak)) void board_boot_power_on_gate(void) {}

extern "C" __attribute__((weak)) bool board_should_charging_only_boot(void)
{
    return false;
}
extern "C" __attribute__((weak)) void board_charging_only_main(void) {}

extern "C" void app_main(void)
{
    board_boot_power_on_gate();

    if (board_should_charging_only_boot()) {
        board_charging_only_main();
    }

    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  
}
