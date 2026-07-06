#include "es8311_audio_codec.h"

#include <esp_log.h>

#if __has_include("config.h")
#include "config.h"
#endif
#ifndef AUDIO_INPUT_USE_SILICON_MIC
#define AUDIO_INPUT_USE_SILICON_MIC 1
#endif
#ifndef AUDIO_MIC_PCM_RIGHT_SHIFT
#define AUDIO_MIC_PCM_RIGHT_SHIFT 12
#endif

#if AUDIO_INPUT_USE_SILICON_MIC
#include <climits>
#include <vector>
#endif

#define TAG "Es8311AudioCodec"

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted) {
    duplex_ = true; 
    input_reference_ = false; 
    input_channels_ = 1; 
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    input_gain_ = 30;

    assert(input_sample_rate_ == output_sample_rate_);

    
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_direction(pa_pin_, GPIO_MODE_OUTPUT);
        const int off_level = pa_inverted_ ? 1 : 0;
        gpio_set_level(pa_pin_, off_level);
    }
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    
    
    es8311_cfg.pa_pin = (pa_pin >= GPIO_NUM_MAX) ? GPIO_NUM_NC : pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted_;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        
    } else {
#if AUDIO_INPUT_USE_SILICON_MIC
        
#else
        
#endif
    }
}

Es8311AudioCodec::~Es8311AudioCodec() {
    esp_codec_dev_delete(dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::UpdateDeviceState() {
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        esp_codec_dev_close(dev_);
        dev_ = nullptr;
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

#if AUDIO_INPUT_USE_SILICON_MIC
    
    i2s_std_config_t rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    i2s_std_config_t tx_cfg = rx_cfg;
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH; 
#else
    
    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    i2s_std_config_t rx_cfg = tx_cfg;
#endif

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    
}

void Es8311AudioCodec::SetOutputVolume(int volume, bool permanent) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    if (dev_ == nullptr) {
        AudioCodec::SetOutputVolume(volume, permanent);
        return;
    }
    esp_err_t err = esp_codec_dev_set_out_vol(dev_, volume);
    if (err != ESP_OK) {
        AudioCodec::SetOutputVolume(volume, permanent);
        return;
    }
    AudioCodec::SetOutputVolume(volume, permanent);
}

void Es8311AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
#if AUDIO_INPUT_USE_SILICON_MIC
    if (!input_enabled_) {
        return samples;
    }
    size_t bytes_read = 0;
    std::vector<int32_t> bit32_buffer(samples);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        
        return 0;
    }
    int n = bytes_read / sizeof(int32_t);
    for (int i = 0; i < n; i++) {
        int32_t value = bit32_buffer[i] >> AUDIO_MIC_PCM_RIGHT_SHIFT;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    if (input_gain_ > 0 && n > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < n; i++) {
            int32_t amplified = (int32_t)dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return n;
#else
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
#endif
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_direction(pa_pin_, GPIO_MODE_OUTPUT);
            const int on_level = pa_inverted_ ? 0 : 1;
            gpio_set_level(pa_pin_, on_level);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
