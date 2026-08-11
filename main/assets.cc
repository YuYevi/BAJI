/**
 * @file assets.cc
 * @brief 资源管理模块实现文件
 * 
 * 该文件实现了 Assets 类，负责管理嵌入式设备中的资源文件（如字体、表情、主题等）。
 * 支持两种策略：LvglStrategy（用于 LVGL 显示框架）和 EmoteStrategy（用于表情显示）。
 * 资源存储在 ESP32 的分区中，通过内存映射方式访问以提高性能。
 */

#include "assets.h"
#include "board.h"
#include "display.h"
#include "application.h"
#include "lvgl_theme.h"
#include "emote_display.h"
#include "expression_emote.h"
#include "remote_asset_regions.h"

#if HAVE_LVGL
#include "display/lcd_display.h"
#include <spi_flash_mmap.h>
#endif

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <cbin_font.h>

static constexpr int kAssetHttpTimeoutMs = 15000;

#define TAG                 "Assets"          // 日志标签
#define PARTITION_LABEL     "assets"          // 资源分区标签名

/**
 * @brief 资源表结构
 * 
 * 用于存储资源文件在分区中的元数据信息，包括文件名、大小、偏移量和尺寸（针对图片资源）。
 */
struct mmap_assets_table {
    char        asset_name[32];       ///< 资源名称（最大32字节）
    uint32_t    asset_size;           ///< 资源大小（字节）
    uint32_t    asset_offset;         ///< 资源在分区中的偏移量
    uint16_t    asset_width;          ///< 图片宽度（像素）
    uint16_t    asset_height;         ///< 图片高度（像素）
};

/**
 * @brief Assets 类构造函数
 * 
 * 根据编译配置选择资源加载策略：
 * - HAVE_LVGL 宏定义时使用 LvglStrategy
 * - 否则使用 EmoteStrategy
 * 
 * 构造完成后自动初始化分区。
 */
Assets::Assets() {
#if HAVE_LVGL
    strategy_ = std::make_unique<Assets::LvglStrategy>();
#else
    strategy_ = std::make_unique<Assets::EmoteStrategy>();
#endif
    
    InitializePartition();
}

/**
 * @brief Assets 类析构函数
 * 
 * 清理资源分区映射。
 */
Assets::~Assets() {
}

/**
 * @brief 查找资源分区
 * 
 * 在 ESP32 的分区表中查找指定标签的分区。
 * 
 * @param[in,out] assets  Assets 对象指针
 * @return true  分区查找成功
 * @return false 分区查找失败
 */
bool Assets::FindPartition(Assets* assets) {
    assets->partition_ = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, 
        ESP_PARTITION_SUBTYPE_ANY, 
        PARTITION_LABEL
    );
    if (assets->partition_ == nullptr) {
        ESP_LOGE(TAG, "Failed to find assets partition");
        return false;
    }
    return true;
}

/**
 * @brief 应用资源配置
 * 
 * 根据当前策略应用资源配置，包括加载字体、表情、主题等。
 * 
 * @return true  配置应用成功
 * @return false 配置应用失败
 */
bool Assets::Apply() {
    return strategy_ ? strategy_->Apply(this) : false;
}

/**
 * @brief 初始化资源分区
 * 
 * 调用当前策略的分区初始化方法。
 * 
 * @return true  初始化成功
 * @return false 初始化失败
 */
bool Assets::InitializePartition() {
    return strategy_ ? strategy_->InitializePartition(this) : false;
}

/**
 * @brief 反初始化资源分区
 * 
 * 调用当前策略的分区清理方法，释放内存映射。
 */
void Assets::UnApplyPartition() {
    if (strategy_) {
        strategy_->UnApplyPartition(this);
    }
}

/**
 * @brief 获取资源数据
 * 
 * 根据资源名称从分区中获取资源数据指针和大小。
 * 
 * @param[in]  name  资源名称
 * @param[out] ptr   资源数据指针（输出参数）
 * @param[out] size  资源大小（输出参数）
 * @return true      获取成功
 * @return false     获取失败
 */
bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    return strategy_ ? strategy_->GetAssetData(this, name, ptr, size) : false;
}

/**
 * @brief 从索引文件加载语音识别模型
 * 
 * 解析 index.json 文件，获取 srmodels 字段指定的模型文件，并加载到音频服务中。
 * 
 * @param[in,out] assets   Assets 对象指针
 * @param[in]     root     cJSON 根节点（可为 nullptr，此时自动加载 index.json）
 * @return true            模型加载成功
 * @return false           模型加载失败
 */
bool Assets::LoadSrmodelsFromIndex(Assets* assets, cJSON* root) {
    void* ptr = nullptr;
    size_t size = 0;
    bool need_delete_root = false;

    // 如果未提供 cJSON 根节点，自动加载并解析 index.json
    if (root == nullptr) {
        if (!assets->GetAssetData("index.json", ptr, size)) {
            ESP_LOGE(TAG, "Failed to get index.json");
            return false;
        }

        root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse index.json");
            return false;
        }
        need_delete_root = true;
    }

    // 获取 srmodels 字段
    cJSON* srmodels = cJSON_GetObjectItem(root, "srmodels");
    if (cJSON_IsString(srmodels)) {
        std::string srmodels_file = srmodels->valuestring;
        if (assets->GetAssetData(srmodels_file, ptr, size)) {
            // 先释放旧模型
            if (assets->models_list_ != nullptr) {
                esp_srmodel_deinit(assets->models_list_);
                assets->models_list_ = nullptr;
            }
            // 加载新模型
            assets->models_list_ = srmodel_load(static_cast<uint8_t*>(ptr));
            if (assets->models_list_ != nullptr) {
                // 将模型设置到音频服务
                auto& app = Application::GetInstance();
                app.GetAudioService().SetModelsList(assets->models_list_);
                
                if (need_delete_root) {
                    cJSON_Delete(root);
                }
                return true;
            } else {
                ESP_LOGE(TAG, "Failed to load srmodels");
            }
        } else {
            ESP_LOGE(TAG, "Failed to get srmodels file: %s", srmodels_file.c_str());
        }
    }

    // 清理资源
    if (need_delete_root) {
        cJSON_Delete(root);
    }
    return false;
}

#if HAVE_LVGL

/**
 * @brief LvglStrategy 内部类 - 计算校验和
 * 
 * 简单的累加校验和算法，用于验证资源分区数据完整性。
 * 
 * @param[in] data    数据指针
 * @param[in] length  数据长度
 * @return 16位校验和值
 */
uint32_t Assets::LvglStrategy::CalculateChecksum(const char* data, uint32_t length) {
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum & 0xFFFF;
}

/**
 * @brief LvglStrategy - 初始化资源分区
 * 
 * 将资源分区映射到内存，验证数据完整性，并构建资源索引表。
 * 
 * @param[in,out] assets  Assets 对象指针
 * @return true           初始化成功
 * @return false          初始化失败
 */
bool Assets::LvglStrategy::InitializePartition(Assets* assets) {
    assets->partition_valid_ = false;
    assets_.clear();

    // 查找资源分区
    if (!Assets::FindPartition(assets)) {
        return false;
    }

    // 检查可用内存映射空间
    int free_pages = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
    uint32_t storage_size = free_pages * 64 * 1024;
    
    // 验证分区大小是否在可用空间范围内
    if (storage_size < assets->partition_->size) {
        ESP_LOGE(TAG, "Insufficient mmap storage: required %u, available %u", 
                 assets->partition_->size, storage_size);
        return false;
    }

    // 将分区映射到内存
    esp_err_t err = esp_partition_mmap(
        assets->partition_, 
        0, 
        assets->partition_->size, 
        ESP_PARTITION_MMAP_DATA, 
        (const void**)&mmap_root_, 
        &mmap_handle_
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap partition: %s", esp_err_to_name(err));
        return false;
    }

    assets->partition_valid_ = true;

    // 读取分区头部信息
    uint32_t stored_files = *(uint32_t*)(mmap_root_ + 0);   // 文件数量
    uint32_t stored_chksum = *(uint32_t*)(mmap_root_ + 4);  // 校验和
    uint32_t stored_len = *(uint32_t*)(mmap_root_ + 8);     // 数据长度

    // 验证数据长度有效性
    const uint32_t system_region_size =
        assets->partition_->size < RemoteAssetRegions::kSystemSize
            ? assets->partition_->size
            : RemoteAssetRegions::kSystemSize;
    if (system_region_size < 12 || stored_len > system_region_size - 12) {
        ESP_LOGD(TAG, "Invalid stored_len (0x%lx) exceeds system asset region (0x%lx)",
                 stored_len, system_region_size);
        return false;
    }

    // 计算并验证校验和
    auto start_time = esp_timer_get_time();
    uint32_t calculated_checksum = CalculateChecksum(mmap_root_ + 12, stored_len);
    auto end_time = esp_timer_get_time();
    
    ESP_LOGD(TAG, "Checksum calculation time: %lld us", end_time - start_time);

    if (calculated_checksum != stored_chksum) {
        ESP_LOGE(TAG, "Checksum mismatch: expected 0x%lx, got 0x%lx", 
                 stored_chksum, calculated_checksum);
        return false;
    }

    checksum_valid_ = true;

    // 解析资源表并构建索引
    for (uint32_t i = 0; i < stored_files; i++) {
        auto item = (const mmap_assets_table*)(mmap_root_ + 12 + i * sizeof(mmap_assets_table));
        auto asset = Asset{
            .size = static_cast<size_t>(item->asset_size),
            .offset = static_cast<size_t>(12 + sizeof(mmap_assets_table) * stored_files + item->asset_offset)
        };
        assets_[item->asset_name] = asset;
    }
    
    ESP_LOGD(TAG, "Loaded %u assets", stored_files);
    return checksum_valid_;
}

/**
 * @brief LvglStrategy - 反初始化资源分区
 * 
 * 释放内存映射句柄，清空资源索引。
 * 
 * @param[in,out] assets  Assets 对象指针
 */
void Assets::LvglStrategy::UnApplyPartition(Assets* assets) {
    if (mmap_handle_ != 0) {
        esp_partition_munmap(mmap_handle_);
        mmap_handle_ = 0;
        mmap_root_ = nullptr;
    }
    checksum_valid_ = false;
    assets_.clear();
    (void)assets;  // 未使用的参数
}

/**
 * @brief LvglStrategy - 获取资源数据
 * 
 * 根据资源名称从内存映射中获取资源数据，验证资源头部标识。
 * 
 * @param[in]     assets  Assets 对象指针
 * @param[in]     name    资源名称
 * @param[out]    ptr     资源数据指针（输出参数）
 * @param[out]    size    资源大小（输出参数）
 * @return true           获取成功
 * @return false          获取失败
 */
bool Assets::LvglStrategy::GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) {
    auto asset = assets_.find(name);
    if (asset == assets_.end()) {
        ESP_LOGW(TAG, "Asset not found: %s", name.c_str());
        return false;
    }
    
    // 验证资源头部标识 "ZZ"
    auto data = (const char*)(mmap_root_ + asset->second.offset);
    if (data[0] != 'Z' || data[1] != 'Z') {
        ESP_LOGE(TAG, "Invalid asset header for: %s", name.c_str());
        return false;
    }

    ptr = static_cast<void*>(const_cast<char*>(data + 2));  // 跳过头部标识
    size = asset->second.size;
    return true;
}

/**
 * @brief LvglStrategy - 应用资源配置
 * 
 * 解析 index.json 配置文件，加载字体、表情、主题等资源，并应用到显示系统。
 * 
 * @param[in,out] assets  Assets 对象指针
 * @return true           应用成功
 * @return false          应用失败
 */
bool Assets::LvglStrategy::Apply(Assets* assets) {
    void* ptr = nullptr;
    size_t size = 0;

    // 加载 index.json
    if (!assets->GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "Failed to load index.json");
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return false;
    }

    // 验证版本号
    cJSON* version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsNumber(version)) {
        if (version->valuedouble > 1) {
            ESP_LOGE(TAG, "Unsupported version: %f", version->valuedouble);
            cJSON_Delete(root);
            return false;
        }
    }

    // 加载语音识别模型
    Assets::LoadSrmodelsFromIndex(assets, root);

    // 获取主题管理器
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto light_theme = theme_manager.GetTheme("light");
    auto dark_theme = theme_manager.GetTheme("dark");

    // 加载字体配置
    cJSON* font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font)) {
        std::string fonts_text_file = font->valuestring;
        if (assets->GetAssetData(fonts_text_file, ptr, size)) {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr) {
                ESP_LOGE(TAG, "Failed to load font: %s", fonts_text_file.c_str());
                cJSON_Delete(root);
                return false;
            }
            if (light_theme != nullptr) {
                light_theme->set_text_font(text_font);
            }
            if (dark_theme != nullptr) {
                dark_theme->set_text_font(text_font);
            }
        } else {
            ESP_LOGW(TAG, "Failed to get font file: %s", fonts_text_file.c_str());
        }
    }

    // 加载表情集合
    cJSON* emoji_collection = cJSON_GetObjectItem(root, "emoji_collection");
    if (cJSON_IsArray(emoji_collection)) {
        auto custom_emoji_collection = std::make_shared<EmojiCollection>();
        int emoji_count = cJSON_GetArraySize(emoji_collection);
        
        for (int i = 0; i < emoji_count; i++) {
            cJSON* emoji = cJSON_GetArrayItem(emoji_collection, i);
            if (cJSON_IsObject(emoji)) {
                cJSON* name = cJSON_GetObjectItem(emoji, "name");
                cJSON* file = cJSON_GetObjectItem(emoji, "file");
                cJSON* eaf = cJSON_GetObjectItem(emoji, "eaf");
                
                if (cJSON_IsString(name) && cJSON_IsString(file) && (NULL == eaf)) {
                    if (!assets->GetAssetData(file->valuestring, ptr, size)) {
                        ESP_LOGW(TAG, "Failed to get emoji file: %s", file->valuestring);
                        continue;
                    }
                    custom_emoji_collection->AddEmoji(name->valuestring, new LvglRawImage(ptr, size));
                }
            }
        }
        
        if (light_theme != nullptr) {
            light_theme->set_emoji_collection(custom_emoji_collection);
        }
        if (dark_theme != nullptr) {
            dark_theme->set_emoji_collection(custom_emoji_collection);
        }
    }

    // 加载主题皮肤配置
    cJSON* skin = cJSON_GetObjectItem(root, "skin");
    if (cJSON_IsObject(skin)) {
        // 加载浅色主题皮肤
        cJSON* light_skin = cJSON_GetObjectItem(skin, "light");
        if (cJSON_IsObject(light_skin) && light_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(light_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(light_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(light_skin, "background_image");
            
            if (cJSON_IsString(text_color)) {
                light_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                light_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                light_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!assets->GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "Failed to get light background image");
                    cJSON_Delete(root);
                    return false;
                }
                auto background_image_ptr = std::make_shared<LvglCBinImage>(ptr);
                light_theme->set_background_image(background_image_ptr);
            }
        }

        // 加载深色主题皮肤
        cJSON* dark_skin = cJSON_GetObjectItem(skin, "dark");
        if (cJSON_IsObject(dark_skin) && dark_theme != nullptr) {
            cJSON* text_color = cJSON_GetObjectItem(dark_skin, "text_color");
            cJSON* background_color = cJSON_GetObjectItem(dark_skin, "background_color");
            cJSON* background_image = cJSON_GetObjectItem(dark_skin, "background_image");
            
            if (cJSON_IsString(text_color)) {
                dark_theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
            }
            if (cJSON_IsString(background_color)) {
                dark_theme->set_background_color(LvglTheme::ParseColor(background_color->valuestring));
                dark_theme->set_chat_background_color(LvglTheme::ParseColor(background_color->valuestring));
            }
            if (cJSON_IsString(background_image)) {
                if (!assets->GetAssetData(background_image->valuestring, ptr, size)) {
                    ESP_LOGE(TAG, "Failed to get dark background image");
                    cJSON_Delete(root);
                    return false;
                }
                auto background_image_ptr = std::make_shared<LvglCBinImage>(ptr);
                dark_theme->set_background_image(background_image_ptr);
            }
        }
    }

    // 刷新显示主题
    auto display = Board::GetInstance().GetDisplay();
    auto current_theme = display->GetTheme();
    if (current_theme != nullptr) {
        display->SetTheme(current_theme);
    }

    // 设置字幕隐藏选项
    cJSON* hide_subtitle = cJSON_GetObjectItem(root, "hide_subtitle");
    if (cJSON_IsBool(hide_subtitle)) {
        bool hide = cJSON_IsTrue(hide_subtitle);
        auto lcd_display = dynamic_cast<LcdDisplay*>(display);
        if (lcd_display != nullptr) {
            lcd_display->SetHideSubtitle(hide);
        }
    }
    
    cJSON_Delete(root);
    return true;
}

#endif  // HAVE_LVGL

/**
 * @brief EmoteStrategy - 初始化资源分区
 * 
 * 使用表情显示框架挂载资源分区。
 * 
 * @param[in,out] assets  Assets 对象指针
 * @return true           初始化成功
 * @return false          初始化失败
 */
bool Assets::EmoteStrategy::InitializePartition(Assets* assets) {
    assets->partition_valid_ = false;

    // 查找资源分区
    if (!Assets::FindPartition(assets)) {
        return false;
    }

    esp_err_t ret = ESP_ERR_INVALID_STATE;
    auto display = Board::GetInstance().GetDisplay();
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);
    
    // 如果是表情显示设备，挂载资源
    if (emote_display && emote_display->GetEmoteHandle() != nullptr) {
        const emote_data_t data = {
            .type = EMOTE_SOURCE_PARTITION,
            .source = {
                .partition_label = PARTITION_LABEL,
            },
            .flags = {
                .mmap_enable = true,  // 启用内存映射
            },
        };
        ret = emote_mount_assets(emote_display->GetEmoteHandle(), &data);
    } else {
        ESP_LOGW(TAG, "Emote display not available");
    }
    
    assets->partition_valid_ = (ret == ESP_OK);
    return assets->partition_valid_;
}

/**
 * @brief EmoteStrategy - 反初始化资源分区
 * 
 * 卸载表情资源。
 * 
 * @param[in,out] assets  Assets 对象指针
 */
void Assets::EmoteStrategy::UnApplyPartition(Assets* assets) {
    auto display = Board::GetInstance().GetDisplay();
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);
    if (emote_display && emote_display->GetEmoteHandle() != nullptr) {
        emote_unmount_assets(emote_display->GetEmoteHandle());
    }
    (void)assets;  // 未使用的参数
}

/**
 * @brief EmoteStrategy - 获取资源数据
 * 
 * 通过表情框架接口获取资源数据。
 * 
 * @param[in]     assets  Assets 对象指针
 * @param[in]     name    资源名称
 * @param[out]    ptr     资源数据指针（输出参数）
 * @param[out]    size    资源大小（输出参数）
 * @return true           获取成功
 * @return false          获取失败
 */
bool Assets::EmoteStrategy::GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) {
    auto display = Board::GetInstance().GetDisplay();
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);
    
    if (emote_display && emote_display->GetEmoteHandle() != nullptr) {
        const uint8_t* data = nullptr;
        size_t data_size = 0;
        
        if (ESP_OK == emote_get_asset_data_by_name(
            emote_display->GetEmoteHandle(), 
            name.c_str(), 
            &data, 
            &data_size
        )) {
            ptr = const_cast<void*>(static_cast<const void*>(data));
            size = data_size;
            return true;
        }
        
        ESP_LOGW(TAG, "Failed to get asset: %s", name.c_str());
        return false;
    }
    
    (void)assets;  // 未使用的参数
    return false;
}

/**
 * @brief EmoteStrategy - 应用资源配置
 * 
 * 加载语音识别模型并加载表情资源。
 * 
 * @param[in,out] assets  Assets 对象指针
 * @return true           应用成功
 * @return false          应用失败
 */
bool Assets::EmoteStrategy::Apply(Assets* assets) {
    // 加载语音识别模型
    Assets::LoadSrmodelsFromIndex(assets);

    // 加载表情资源
    auto display = Board::GetInstance().GetDisplay();
    auto* emote_display = dynamic_cast<emote::EmoteDisplay*>(display);

    if (emote_display && emote_display->GetEmoteHandle() != nullptr) {
        emote_load_assets(emote_display->GetEmoteHandle());
    }
    
    return true;
}

/**
 * @brief 下载资源并写入分区
 * 
 * 从指定 URL 下载资源包，擦除分区并写入新数据，最后重新初始化分区。
 * 
 * @param[in] url               资源包下载地址
 * @param[in] progress_callback 下载进度回调函数（进度百分比，下载速度字节/秒）
 * @return true                 下载并写入成功
 * @return false                下载或写入失败
 */
bool Assets::Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback) {
    // 先卸载当前分区

    // 创建 HTTP 客户端
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "Network is not ready for asset download");
        return false;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        ESP_LOGE(TAG, "Failed to create asset download HTTP client");
        return false;
    }
    http->SetTimeout(kAssetHttpTimeoutMs);
    
    // 打开 HTTP 连接
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", url.c_str());
        http->Close();
        return false;
    }

    // 检查 HTTP 状态码
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status: %d", http->GetStatusCode());
        http->Close();
        return false;
    }

    // 获取内容长度
    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Content length is zero");
        http->Close();
        return false;
    }

    // 验证内容大小是否在分区范围内
    const size_t system_region_size =
        partition_->size < RemoteAssetRegions::kSystemSize
            ? partition_->size
            : RemoteAssetRegions::kSystemSize;
    if (content_length > system_region_size) {
        ESP_LOGE(TAG, "Content size %u exceeds system asset region %u", content_length,
                 system_region_size);
        http->Close();
        return false;
    }

    // 获取扇区大小并计算需要擦除的扇区数
    UnApplyPartition();

    const size_t SECTOR_SIZE = esp_partition_get_main_flash_sector_size();
    size_t sectors_to_erase = (content_length + SECTOR_SIZE - 1) / SECTOR_SIZE;
    (void)sectors_to_erase;  // 实际擦除在循环中按需进行

    // 分配扇区大小的缓冲区
    char* buffer = (char*)heap_caps_malloc(SECTOR_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        http->Close();
        if (!InitializePartition()) {
            ESP_LOGE(TAG, "Failed to reinitialize partition after download");
        }
        return false;
    }

    auto cleanup_after_write = [&]() -> bool {
        http->Close();
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
        if (!InitializePartition()) {
            ESP_LOGE(TAG, "Failed to reinitialize partition after download");
        }
        return false;
    };

    size_t total_written = 0;
    size_t recent_written = 0;
    size_t current_sector = 0;
    auto last_calc_time = esp_timer_get_time();

    // 循环读取并写入数据
    while (true) {
        int ret = http->Read(buffer, SECTOR_SIZE);
        if (ret < 0) {
            ESP_LOGE(TAG, "HTTP read failed");
            return cleanup_after_write();
        }

        if (ret == 0) {
            break;  // 读取完成
        }

        // 计算需要的扇区数
        size_t write_end_offset = total_written + ret;
        size_t needed_sectors = (write_end_offset + SECTOR_SIZE - 1) / SECTOR_SIZE;
        
        // 按需擦除扇区
        while (current_sector < needed_sectors) {
            size_t sector_start = current_sector * SECTOR_SIZE;
            size_t sector_end = (current_sector + 1) * SECTOR_SIZE;
            
            // 检查扇区是否超出分区范围
            if (sector_end > system_region_size) {
                ESP_LOGE(TAG, "Sector %u exceeds system asset region", current_sector);
                return cleanup_after_write();
            }
            
            ESP_LOGD(TAG, "Erasing sector %u (offset: %u, size: %u)", 
                     current_sector, sector_start, SECTOR_SIZE);
            
            esp_err_t err = esp_partition_erase_range(partition_, sector_start, SECTOR_SIZE);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase sector %u: %s", current_sector, esp_err_to_name(err));
                return cleanup_after_write();
            }
            
            current_sector++;
        }

        // 写入数据到分区
        esp_err_t err = esp_partition_write(partition_, total_written, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write to partition: %s", esp_err_to_name(err));
            return cleanup_after_write();
        }

        total_written += ret;
        recent_written += ret;

        // 计算并回调下载进度（每秒或完成时）
        if (esp_timer_get_time() - last_calc_time >= 1000000 || 
            total_written == content_length || 
            ret == 0) {
            size_t progress = total_written * 100 / content_length;
            size_t speed = recent_written;  // 字节/秒
            
            if (progress_callback) {
                progress_callback(progress, speed);
            }
            
            last_calc_time = esp_timer_get_time();
            recent_written = 0;
        }
    }
    
    // 清理 HTTP 连接和缓冲区
    // 验证写入数据完整性
    if (total_written != content_length) {
        ESP_LOGE(TAG, "Written size %u mismatch with content length %u", total_written, content_length);
        return cleanup_after_write();
    }

    http->Close();
    heap_caps_free(buffer);
    buffer = nullptr;

    // 重新初始化分区
    if (!InitializePartition()) {
        ESP_LOGE(TAG, "Failed to reinitialize partition after download");
        return false;
    }

    return true;
}
