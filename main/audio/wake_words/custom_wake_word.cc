#include "custom_wake_word.h"
#include "audio_service.h"
#include "system_info.h"
#include "assets.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>
#include <algorithm>
#include <cmath>

#define TAG "CustomWakeWord"

CustomWakeWord::CustomWakeWord()
    : wake_word_pcm_(), wake_word_opus_() {
}

CustomWakeWord::~CustomWakeWord() {
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
}

void CustomWakeWord::ParseWakenetModelConfig() {
    
    auto& assets = Assets::GetInstance();
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                        commands_.push_back({command_name->valuestring, text->valuestring, action->valuestring});
                        
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

void CustomWakeWord::LoadPersistedConfig() {
    Settings settings("wake_word");

    int threshold_percent = settings.GetInt("threshold_percent", -1);
    if (threshold_percent >= 1 && threshold_percent <= 99) {
        threshold_ = static_cast<float>(threshold_percent) / 100.0f;
    }

    std::string commands_json = settings.GetString("commands_json");
    if (commands_json.empty()) {
        return;
    }

    std::deque<Command> persisted_commands;
    if (!DeserializeCommands(commands_json, persisted_commands)) {
        ESP_LOGW(TAG, "Failed to parse persisted wake word commands, using bundled defaults");
        return;
    }

    if (persisted_commands.empty()) {
        ESP_LOGW(TAG, "Persisted wake word commands are empty, using bundled defaults");
        return;
    }

    commands_ = std::move(persisted_commands);
}

void CustomWakeWord::RefreshActiveCommands() {
    if (multinet_model_data_ == nullptr || multinet_ == nullptr) {
        return;
    }

    esp_mn_commands_clear();
    for (int i = 0; i < commands_.size(); ++i) {
        esp_mn_commands_add(i + 1, commands_[i].command.c_str());
    }
    esp_mn_commands_update();
}

bool CustomWakeWord::DeserializeCommands(const std::string& json, std::deque<Command>& commands) const {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    std::deque<Command> parsed_commands;
    for (int i = 0; i < cJSON_GetArraySize(root); ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) {
            cJSON_Delete(root);
            return false;
        }

        cJSON* command = cJSON_GetObjectItem(item, "command");
        cJSON* text = cJSON_GetObjectItem(item, "text");
        cJSON* action = cJSON_GetObjectItem(item, "action");
        if (!cJSON_IsString(command) || !cJSON_IsString(text)) {
            cJSON_Delete(root);
            return false;
        }

        parsed_commands.push_back({
            command->valuestring,
            text->valuestring,
            cJSON_IsString(action) ? action->valuestring : "wake"
        });
    }

    cJSON_Delete(root);
    commands = std::move(parsed_commands);
    return true;
}

std::string CustomWakeWord::SerializeCommands(const std::deque<Command>& commands) const {
    cJSON* root = cJSON_CreateArray();
    if (root == nullptr) {
        return "";
    }

    for (const auto& command : commands) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) {
            cJSON_Delete(root);
            return "";
        }
        cJSON_AddStringToObject(item, "command", command.command.c_str());
        cJSON_AddStringToObject(item, "text", command.text.c_str());
        cJSON_AddStringToObject(item, "action", command.action.c_str());
        cJSON_AddItemToArray(root, item);
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) {
        return "";
    }

    std::string serialized(json);
    cJSON_free(json);
    return serialized;
}

bool CustomWakeWord::PersistCurrentConfig() const {
    if (commands_.empty()) {
        return false;
    }

    std::string commands_json = SerializeCommands(commands_);
    if (commands_json.empty()) {
        return false;
    }

    Settings settings("wake_word", true);
    int threshold_percent = static_cast<int>(std::lround(threshold_ * 100.0f));
    threshold_percent = std::max(1, std::min(99, threshold_percent));
    settings.SetInt("threshold_percent", threshold_percent);
    settings.SetString("commands_json", commands_json);
    return true;
}


bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    LoadPersistedConfig();

    if (models_ == nullptr || models_->num == -1) {
        
        return false;
    }

    
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        
        
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    RefreshActiveCommands();
    
    multinet_->print_active_speech_commands(multinet_model_data_);
    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void CustomWakeWord::Start() {
    running_ = true;
}

void CustomWakeWord::Stop() {
    running_ = false;

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    if (multinet_model_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    
    if (!running_) {
        return;
    }

    
    if (codec_->input_channels() == 2) {
        for (size_t i = 0; i < data.size(); i += 2) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    }
    
    int chunksize = multinet_->get_samp_chunksize(multinet_model_data_);
    while (input_buffer_.size() >= chunksize) {
        std::vector<int16_t> chunk(input_buffer_.begin(), input_buffer_.begin() + chunksize);
        StoreWakeWordData(chunk);
        
        esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, chunk.data());
        
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet_->get_results(multinet_model_data_);
            for (int i = 0; i < mn_result->num && running_; i++) {
                
                auto& command = commands_[mn_result->command_id[i] - 1];
                if (command.action == "wake") {
                    last_detected_wake_word_ = command.text;
                    running_ = false;
                    input_buffer_.clear();
                    
                    if (wake_word_detected_callback_) {
                        wake_word_detected_callback_(last_detected_wake_word_);
                    }
                }
            }
            multinet_->clean(multinet_model_data_);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
            multinet_->clean(multinet_model_data_);
        }
        
        if (!running_) {
            break;
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}

void CustomWakeWord::StoreWakeWordData(const std::vector<int16_t>& data) {
    
    wake_word_pcm_.push_back(data);
    
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void CustomWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (CustomWakeWord*)arg;
        {
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                return;
            }
            
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            
            int packets = 0;
            std::vector<int16_t> in_buffer;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            for (auto& pcm: this_->wake_word_pcm_) {
                if (in_buffer.empty()) {
                    in_buffer = std::move(pcm);
                } else {
                    in_buffer.reserve(in_buffer.size() + pcm.size());
                    in_buffer.insert(in_buffer.end(), pcm.begin(), pcm.end());
                }
                while (in_buffer.size() >= frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t *)(in_buffer.data());
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;
                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        
                    }
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + frame_size);
                }
            }
            this_->wake_word_pcm_.clear();
            
            esp_opus_enc_close(encoder_handle);

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool CustomWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}

std::vector<WakeWordConfig> CustomWakeWord::GetWakeWordConfigs() const {
    std::vector<WakeWordConfig> configs;
    for (const auto& cmd : commands_) {
        configs.push_back({cmd.command, cmd.text, cmd.action});
    }
    return configs;
}

bool CustomWakeWord::AddWakeWord(const WakeWordConfig& config) {
    if (config.command.empty() || config.display_text.empty()) {
        return false;
    }
    
    for (const auto& cmd : commands_) {
        if (cmd.command == config.command) {
            return false;
        }
    }
    
    bool was_running = running_;
    if (was_running) {
        Stop();
    }

    commands_.push_back({config.command, config.display_text, config.action.empty() ? "wake" : config.action});
    if (!PersistCurrentConfig()) {
        commands_.pop_back();
        if (was_running) {
            Start();
        }
        return false;
    }

    RefreshActiveCommands();

    if (was_running) {
        Start();
    }
    
    return true;
}

bool CustomWakeWord::RemoveWakeWord(const std::string& command) {
    if (commands_.size() <= 1) {
        return false;
    }
    
    auto it = std::find_if(commands_.begin(), commands_.end(),
                           [&command](const Command& cmd) { return cmd.command == command; });
    
    if (it == commands_.end()) {
        return false;
    }
    
    bool was_running = running_;
    if (was_running) {
        Stop();
    }

    size_t removed_index = std::distance(commands_.begin(), it);
    Command removed = *it;
    commands_.erase(it);
    if (!PersistCurrentConfig()) {
        commands_.insert(commands_.begin() + removed_index, removed);
        if (was_running) {
            Start();
        }
        return false;
    }

    RefreshActiveCommands();

    if (was_running) {
        Start();
    }
    
    return true;
}

bool CustomWakeWord::SetWakeWordThreshold(float threshold) {
    if (threshold < 0.0f || threshold > 1.0f) {
        return false;
    }

    float previous_threshold = threshold_;
    threshold_ = threshold;
    if (!PersistCurrentConfig()) {
        threshold_ = previous_threshold;
        return false;
    }

    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->set_det_threshold(multinet_model_data_, threshold_);
    }
    
    return true;
}
