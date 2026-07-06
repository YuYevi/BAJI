#pragma once

#include <driver/i2c_master.h>
#include <esp_io_expander.h>

class SpiLcdDisplay;


i2c_master_bus_handle_t baji_185_get_i2c_bus(void);
esp_io_expander_handle_t baji_185_get_io_expander(void);


SpiLcdDisplay* baji_185_create_lcd_display(bool quiet_boot = false);
