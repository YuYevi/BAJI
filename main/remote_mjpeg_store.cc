#include "remote_mjpeg_store.h"

#include "remote_asset_regions.h"

#include <cstring>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {

constexpr const char* kTag = "RemoteMjpeg";
constexpr uint32_t kMagic = 0x314A4D52;  // "RMJ1"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kAssetCount = 2;

uint32_t CalculateChecksum(const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    uint32_t checksum = 0;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        checksum = (checksum * 131U) + bytes[i];
    }
    return checksum;
}

template <typename T>
uint32_t CalculateHeaderChecksum(const T& header) {
    return CalculateChecksum(&header, sizeof(T) - sizeof(header.header_checksum));
}

bool ContainsJpegFrame(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4) {
        return false;
    }

    size_t frame_start = SIZE_MAX;
    for (size_t i = 0; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD8) {
            frame_start = i;
            break;
        }
    }
    if (frame_start == SIZE_MAX) {
        return false;
    }

    for (size_t i = frame_start + 2; i + 1 < size; ++i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            return true;
        }
    }
    return false;
}

}  // namespace

RemoteMjpegStore& RemoteMjpegStore::GetInstance() {
    static RemoteMjpegStore instance;
    return instance;
}

bool RemoteMjpegStore::EnsurePartition() {
    if (initialized_) {
        return supported_;
    }

    initialized_ = true;
    partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                                          RemoteAssetRegions::kRemoteMediaPartitionLabel);
    if (partition_ == nullptr) {
        ESP_LOGW(kTag, "Remote media partition not found");
        return false;
    }

    if (partition_->size < RemoteAssetRegions::kRequiredRemoteMediaPartitionSize) {
        ESP_LOGW(kTag, "Remote media partition size %u does not support the MJPEG region",
                 static_cast<unsigned>(partition_->size));
        return false;
    }

    supported_ = true;
    return true;
}

void RemoteMjpegStore::ResetMetadata() {
    header_ = {};
    has_assets_ = false;
    metadata_loaded_ = true;
}

bool RemoteMjpegStore::IsSupported() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return EnsurePartition();
}

bool RemoteMjpegStore::Reload() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!EnsurePartition()) {
        ResetMetadata();
        return false;
    }

    StoredHeader header = {};
    esp_err_t err = esp_partition_read(partition_, RemoteAssetRegions::kMjpegOffset, &header,
                                       sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to read MJPEG header: %s", esp_err_to_name(err));
        ResetMetadata();
        return false;
    }

    if (header.magic != kMagic || header.version != kVersion || header.count != kAssetCount ||
        header.header_checksum != CalculateHeaderChecksum(header) ||
        std::memchr(header.role_id, '\0', sizeof(header.role_id)) == nullptr ||
        header.total_size < sizeof(StoredHeader) ||
        header.total_size > RemoteAssetRegions::kMjpegSize) {
        ResetMetadata();
        return false;
    }

    uint32_t previous_end = sizeof(StoredHeader);
    for (uint16_t i = 0; i < kAssetCount; ++i) {
        const StoredEntry& entry = header.entries[i];
        const uint64_t entry_end = static_cast<uint64_t>(entry.offset) + entry.size;
        if (entry.size == 0 || entry.offset < previous_end || entry_end > header.total_size) {
            ESP_LOGW(kTag, "MJPEG entry %u is out of range", static_cast<unsigned>(i));
            ResetMetadata();
            return false;
        }
        previous_end = static_cast<uint32_t>(entry_end);
    }

    header_ = header;
    has_assets_ = true;
    metadata_loaded_ = true;
    return true;
}

bool RemoteMjpegStore::Save(const std::string& role_id, const uint8_t* listen_data,
                            size_t listen_size, const uint8_t* speak_data, size_t speak_size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!EnsurePartition() || role_id.size() >= sizeof(StoredHeader::role_id) ||
        listen_data == nullptr || speak_data == nullptr || listen_size == 0 || speak_size == 0 ||
        listen_size > std::numeric_limits<uint32_t>::max() ||
        speak_size > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    if (!ContainsJpegFrame(listen_data, listen_size) || !ContainsJpegFrame(speak_data, speak_size)) {
        ESP_LOGW(kTag, "Refusing to store invalid MJPEG data");
        return false;
    }

    const uint64_t total_size =
        static_cast<uint64_t>(sizeof(StoredHeader)) + listen_size + speak_size;
    if (total_size > RemoteAssetRegions::kMjpegSize) {
        ESP_LOGW(kTag, "MJPEG payload size %llu exceeds the reserved region",
                 static_cast<unsigned long long>(total_size));
        return false;
    }

    StoredHeader header = {};
    header.magic = kMagic;
    header.version = kVersion;
    header.count = kAssetCount;
    header.total_size = static_cast<uint32_t>(total_size);
    std::memcpy(header.role_id, role_id.data(), role_id.size());
    header.entries[0].offset = sizeof(StoredHeader);
    header.entries[0].size = static_cast<uint32_t>(listen_size);
    header.entries[0].checksum = CalculateChecksum(listen_data, listen_size);
    header.entries[1].offset = header.entries[0].offset + header.entries[0].size;
    header.entries[1].size = static_cast<uint32_t>(speak_size);
    header.entries[1].checksum = CalculateChecksum(speak_data, speak_size);
    header.header_checksum = CalculateHeaderChecksum(header);

    const uint32_t sector_size = esp_partition_get_main_flash_sector_size();
    const uint32_t erase_size =
        ((header.total_size + sector_size - 1U) / sector_size) * sector_size;
    esp_err_t err = esp_partition_erase_range(partition_, RemoteAssetRegions::kMjpegOffset,
                                              erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to erase MJPEG region: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_partition_write(partition_, RemoteAssetRegions::kMjpegOffset + header.entries[0].offset,
                              listen_data, listen_size);
    if (err == ESP_OK) {
        err = esp_partition_write(partition_,
                                  RemoteAssetRegions::kMjpegOffset + header.entries[1].offset,
                                  speak_data, speak_size);
    }
    if (err == ESP_OK) {
        // The header is written last so incomplete payload writes are never considered valid.
        err = esp_partition_write(partition_, RemoteAssetRegions::kMjpegOffset, &header,
                                  sizeof(header));
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to write MJPEG region: %s", esp_err_to_name(err));
        ResetMetadata();
        return false;
    }

    return Reload();
}

bool RemoteMjpegStore::Load(bool speaking, uint8_t** data, size_t* size) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (data == nullptr || size == nullptr) {
        return false;
    }
    *data = nullptr;
    *size = 0;

    if (!HasAssets()) {
        return false;
    }

    const StoredEntry& entry = header_.entries[speaking ? 1 : 0];
    auto* loaded = reinterpret_cast<uint8_t*>(
        heap_caps_malloc(entry.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (loaded == nullptr) {
        ESP_LOGW(kTag, "Failed to allocate %u bytes for MJPEG", static_cast<unsigned>(entry.size));
        return false;
    }

    esp_err_t err = esp_partition_read(partition_, RemoteAssetRegions::kMjpegOffset + entry.offset,
                                       loaded, entry.size);
    if (err != ESP_OK || CalculateChecksum(loaded, entry.size) != entry.checksum ||
        !ContainsJpegFrame(loaded, entry.size)) {
        ESP_LOGW(kTag, "Failed to validate stored %s MJPEG", speaking ? "speaking" : "listening");
        heap_caps_free(loaded);
        return false;
    }

    *data = loaded;
    *size = entry.size;
    return true;
}

bool RemoteMjpegStore::HasAssets() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!metadata_loaded_) {
        Reload();
    }
    return has_assets_;
}

std::string RemoteMjpegStore::GetRoleId() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return HasAssets() ? std::string(header_.role_id) : std::string();
}
