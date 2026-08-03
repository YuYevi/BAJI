#include "remote_wallpaper_store.h"

#include "remote_asset_regions.h"

#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "display/lvgl_display/lvgl_image.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_log.h>

namespace {

constexpr const char* kTag = "RemoteWallpaper";
constexpr uint32_t kMagic = 0x31505752;  // "RWP1"
constexpr uint16_t kLegacyVersion = 1;
constexpr uint16_t kVersion = 2;
constexpr uint32_t kDefaultIntervalMs = 2000;

struct StoredHeaderPrefix {
    uint32_t magic;
    uint16_t version;
    uint8_t mode;
    uint8_t reserved;
};

struct LegacyStoredEntry {
    uint32_t offset;
    uint32_t size;
    uint32_t checksum;
    uint32_t reserved;
};

struct StoredHeaderV1 {
    uint32_t magic;
    uint16_t version;
    uint8_t mode;
    uint8_t count;
    uint32_t interval_ms;
    uint32_t total_size;
    LegacyStoredEntry entries[3];
    uint32_t seq;
    uint32_t header_checksum;
};

static_assert(sizeof(StoredHeaderV1) == 72, "Unexpected legacy remote wallpaper header size");

uint32_t CalculateChecksum(const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    uint32_t checksum = 0;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        checksum = (checksum * 131U) + bytes[i];
    }
    return checksum;
}

template <typename T>
uint32_t CalculateHeaderChecksum(const T& header) {
    return CalculateChecksum(&header, sizeof(T) - sizeof(header.header_checksum));
}

bool IsJpegImage(const uint8_t* data, size_t len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

bool IsPngImage(const uint8_t* data, size_t len) {
    return len >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E &&
           data[3] == 0x47 && data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A &&
           data[7] == 0x0A;
}

}  // namespace

std::unique_ptr<LvglImage> DecodeCompressedImageToLvgl(uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return nullptr;
    }

    if (IsJpegImage(data, size)) {
        uint8_t* decoded = nullptr;
        size_t decoded_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        esp_err_t err = jpeg_to_image(data, size, &decoded, &decoded_len, &width, &height, &stride);
        heap_caps_free(data);
        data = nullptr;
        if (err != ESP_OK || decoded == nullptr || decoded_len == 0 || width == 0 || height == 0) {
            if (decoded != nullptr) {
                heap_caps_free(decoded);
            }
            return nullptr;
        }

        uint8_t* psram_data = reinterpret_cast<uint8_t*>(
            heap_caps_malloc(decoded_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (psram_data == nullptr) {
            heap_caps_free(decoded);
            return nullptr;
        }
        std::memcpy(psram_data, decoded, decoded_len);
        heap_caps_free(decoded);

        try {
            return std::make_unique<LvglAllocatedImage>(
                psram_data, decoded_len, static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(stride), LV_COLOR_FORMAT_RGB565);
        } catch (const std::exception&) {
            heap_caps_free(psram_data);
            return nullptr;
        }
    }

    if (!IsPngImage(data, size)) {
        ESP_LOGW(kTag, "Unsupported remote wallpaper format, trying LVGL raw image");
    }

    try {
        return std::make_unique<LvglAllocatedImage>(data, size);
    } catch (const std::exception&) {
        heap_caps_free(data);
        return nullptr;
    }
}

RemoteWallpaperStore& RemoteWallpaperStore::GetInstance() {
    static RemoteWallpaperStore instance;
    return instance;
}

bool RemoteWallpaperStore::EnsurePartition() {
    if (initialized_) {
        return supported_;
    }

    initialized_ = true;
    partition_ =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (partition_ == nullptr) {
        ESP_LOGW(kTag, "Assets partition not found");
        return false;
    }

    if (partition_->size < RemoteAssetRegions::kRequiredAssetsPartitionSize) {
        ESP_LOGW(kTag, "Assets partition size %u does not support the wallpaper region",
                 static_cast<unsigned>(partition_->size));
        return false;
    }

    region_offset_ = RemoteAssetRegions::kWallpaperOffset;
    region_size_ = RemoteAssetRegions::kWallpaperSize;
    if (SlotSize() < (sizeof(StoredHeader) + 1024)) {
        ESP_LOGW(kTag, "Reserved flash region is too small for dual slot wallpapers");
        return false;
    }
    supported_ = true;
    return true;
}

void RemoteWallpaperStore::ResetMetadata() {
    metadata_loaded_ = true;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        slots_[slot].header = {};
        slots_[slot].entries.clear();
        has_images_[slot] = false;
    }
}

bool RemoteWallpaperStore::IsSupported() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return EnsurePartition();
}

uint32_t RemoteWallpaperStore::SlotOffset(uint8_t mode) const {
    uint8_t slot = mode == 1 ? 1 : 0;
    return region_offset_ + slot * SlotSize();
}

uint32_t RemoteWallpaperStore::SlotSize() const {
    return region_size_ / 2;
}

bool RemoteWallpaperStore::ReloadSlot(uint8_t mode) {
    uint8_t slot = mode == 1 ? 1 : 0;
    has_images_[slot] = false;
    slots_[slot].header = {};
    slots_[slot].entries.clear();

    StoredHeaderPrefix prefix = {};
    const uint32_t base = SlotOffset(slot);
    esp_err_t err = esp_partition_read(partition_, base, &prefix, sizeof(prefix));
    if (err != ESP_OK) {
        return false;
    }

    if (prefix.magic != kMagic) {
        return false;
    }

    if (prefix.version == kLegacyVersion) {
        StoredHeaderV1 legacy = {};
        err = esp_partition_read(partition_, base, &legacy, sizeof(legacy));
        if (err != ESP_OK) {
            return false;
        }

        if (legacy.header_checksum != CalculateHeaderChecksum(legacy)) {
            ESP_LOGW(kTag, "Slot %u legacy header checksum mismatch", static_cast<unsigned>(slot));
            return false;
        }

        if (legacy.mode != slot || legacy.count == 0 || legacy.count > 3 ||
            legacy.total_size < sizeof(StoredHeaderV1) || legacy.total_size > SlotSize()) {
            ESP_LOGW(kTag, "Slot %u legacy header is out of range", static_cast<unsigned>(slot));
            return false;
        }

        auto& slot_data = slots_[slot];
        slot_data.header.magic = legacy.magic;
        slot_data.header.version = legacy.version;
        slot_data.header.mode = legacy.mode;
        slot_data.header.reserved = 0;
        slot_data.header.count = legacy.count;
        slot_data.header.interval_ms = legacy.interval_ms;
        slot_data.header.total_size = legacy.total_size;
        slot_data.header.seq = legacy.seq;
        slot_data.header.header_checksum = legacy.header_checksum;
        slot_data.entries.resize(legacy.count);

        for (uint32_t i = 0; i < legacy.count; ++i) {
            const LegacyStoredEntry& entry = legacy.entries[i];
            const uint64_t entry_end = static_cast<uint64_t>(entry.offset) + entry.size;
            if (entry.size == 0 || entry.offset < sizeof(StoredHeaderV1) ||
                entry_end > legacy.total_size) {
                ESP_LOGW(kTag, "Slot %u legacy entry %u is invalid", static_cast<unsigned>(slot),
                         static_cast<unsigned>(i));
                slot_data.entries.clear();
                return false;
            }
            slot_data.entries[i].offset = entry.offset;
            slot_data.entries[i].size = entry.size;
            slot_data.entries[i].checksum = entry.checksum;
            slot_data.entries[i].reserved = entry.reserved;
        }

        has_images_[slot] = true;
        return true;
    }

    if (prefix.version != kVersion) {
        return false;
    }

    StoredHeader header = {};
    err = esp_partition_read(partition_, base, &header, sizeof(header));
    if (err != ESP_OK) {
        return false;
    }

    if (header.header_checksum != CalculateHeaderChecksum(header)) {
        ESP_LOGW(kTag, "Slot %u header checksum mismatch", static_cast<unsigned>(slot));
        return false;
    }

    const uint64_t entries_size_u64 =
        static_cast<uint64_t>(header.count) * static_cast<uint64_t>(sizeof(StoredEntry));
    if (header.mode != slot || header.count == 0 || entries_size_u64 > UINT32_MAX) {
        ESP_LOGW(kTag, "Slot %u header count is invalid", static_cast<unsigned>(slot));
        return false;
    }

    const uint32_t entries_size = static_cast<uint32_t>(entries_size_u64);
    const uint32_t min_total_size = sizeof(StoredHeader) + entries_size;
    if (header.total_size < min_total_size || header.total_size > SlotSize()) {
        ESP_LOGW(kTag, "Slot %u header is out of range", static_cast<unsigned>(slot));
        return false;
    }

    std::vector<StoredEntry> entries(header.count);
    if (!entries.empty()) {
        err = esp_partition_read(partition_, base + sizeof(header), entries.data(), entries_size);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Failed to read slot %u entries: %s", static_cast<unsigned>(slot),
                     esp_err_to_name(err));
            return false;
        }
    }

    for (uint32_t i = 0; i < header.count; ++i) {
        const StoredEntry& entry = entries[i];
        const uint64_t entry_end = static_cast<uint64_t>(entry.offset) + entry.size;
        if (entry.size == 0 || entry.offset < sizeof(StoredHeader) || entry_end > header.total_size) {
            ESP_LOGW(kTag, "Slot %u entry %u is invalid", static_cast<unsigned>(slot),
                     static_cast<unsigned>(i));
            return false;
        }
    }

    slots_[slot].header = header;
    slots_[slot].entries = std::move(entries);
    has_images_[slot] = true;
    return true;
}

bool RemoteWallpaperStore::Reload() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!EnsurePartition()) {
        ResetMetadata();
        return false;
    }
    bool ok0 = ReloadSlot(0);
    bool ok1 = ReloadSlot(1);
    metadata_loaded_ = true;
    return ok0 || ok1;
}

bool RemoteWallpaperStore::SaveSlot(const std::vector<std::pair<const uint8_t*, size_t>>& images,
                                    uint8_t mode, uint32_t interval_ms) {
    if (!EnsurePartition()) {
        return false;
    }
    if (images.empty()) {
        return false;
    }
    if (images.size() > std::numeric_limits<uint32_t>::max()) {
        ESP_LOGW(kTag, "Remote wallpaper image count exceeds supported range");
        return false;
    }

    StoredHeader header = {};
    header.magic = kMagic;
    header.version = kVersion;
    header.mode = mode == 1 ? 1 : 0;
    header.reserved = 0;
    header.count = static_cast<uint32_t>(images.size());
    header.interval_ms = interval_ms >= 500 ? interval_ms : kDefaultIntervalMs;
    const uint64_t entries_size_u64 =
        static_cast<uint64_t>(header.count) * static_cast<uint64_t>(sizeof(StoredEntry));
    if (entries_size_u64 > UINT32_MAX) {
        ESP_LOGW(kTag, "Remote wallpaper entry table exceeds supported range");
        return false;
    }

    header.total_size = sizeof(StoredHeader) + static_cast<uint32_t>(entries_size_u64);
    if (header.total_size > SlotSize()) {
        ESP_LOGW(kTag, "Remote wallpaper metadata exceeds reserved flash region");
        return false;
    }

    uint32_t max_seq = std::max(slots_[0].header.seq, slots_[1].header.seq);
    header.seq = max_seq + 1;
    std::vector<StoredEntry> entries(header.count);

    uint32_t write_offset = header.total_size;
    for (uint32_t i = 0; i < header.count; ++i) {
        const auto& image = images[i];
        if (image.first == nullptr || image.second == 0) {
            ESP_LOGW(kTag, "Remote wallpaper image %u is empty", static_cast<unsigned>(i));
            return false;
        }
        if (image.second > std::numeric_limits<uint32_t>::max()) {
            ESP_LOGW(kTag, "Remote wallpaper image %u exceeds supported size",
                     static_cast<unsigned>(i));
            return false;
        }

        uint32_t image_size = static_cast<uint32_t>(image.second);
        if (image_size > SlotSize() - header.total_size) {
            ESP_LOGW(kTag, "Remote wallpaper image %u exceeds region size", static_cast<unsigned>(i));
            return false;
        }

        entries[i].offset = write_offset;
        entries[i].size = image_size;
        entries[i].checksum = CalculateChecksum(image.first, image.second);
        entries[i].reserved = 0;
        write_offset += image_size;
        header.total_size += image_size;
        if (header.total_size > SlotSize()) {
            ESP_LOGW(kTag, "Remote wallpaper payload exceeds reserved flash region");
            return false;
        }
    }

    header.header_checksum = CalculateHeaderChecksum(header);

    const uint32_t sector_size = esp_partition_get_main_flash_sector_size();
    const uint32_t erase_size =
        ((header.total_size + sector_size - 1) / sector_size) * sector_size;
    const uint32_t base = SlotOffset(header.mode);
    esp_err_t err = esp_partition_erase_range(partition_, base, erase_size);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to erase remote wallpaper region: %s", esp_err_to_name(err));
        return false;
    }

    if (!entries.empty()) {
        err = esp_partition_write(partition_, base + sizeof(header), entries.data(),
                                  static_cast<size_t>(entries.size()) * sizeof(StoredEntry));
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to write remote wallpaper entries: %s", esp_err_to_name(err));
            return false;
        }
    }

    for (uint32_t i = 0; i < header.count; ++i) {
        const auto& image = images[i];
        err = esp_partition_write(partition_, base + entries[i].offset, image.first, image.second);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to write remote wallpaper %u: %s", static_cast<unsigned>(i),
                     esp_err_to_name(err));
            return false;
        }
    }

    err = esp_partition_write(partition_, base, &header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to write remote wallpaper header: %s", esp_err_to_name(err));
        return false;
    }

    return Reload();
}

bool RemoteWallpaperStore::Save(const std::vector<std::pair<const uint8_t*, size_t>>& images, uint8_t mode,
                                uint32_t interval_ms) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!metadata_loaded_) {
        Reload();
    }
    return SaveSlot(images, mode, interval_ms);
}

bool RemoteWallpaperStore::HasImages(uint8_t mode) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!metadata_loaded_) {
        Reload();
    }
    uint8_t slot = mode == 1 ? 1 : 0;
    return has_images_[slot];
}

uint32_t RemoteWallpaperStore::GetCount(uint8_t mode) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint8_t slot = mode == 1 ? 1 : 0;
    return HasImages(mode) ? slots_[slot].header.count : 0;
}

uint32_t RemoteWallpaperStore::GetIntervalMs(uint8_t mode) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint8_t slot = mode == 1 ? 1 : 0;
    return HasImages(mode) ? slots_[slot].header.interval_ms : kDefaultIntervalMs;
}

uint8_t RemoteWallpaperStore::GetLastMode() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!metadata_loaded_) {
        Reload();
    }
    if (has_images_[0] && has_images_[1]) {
        return slots_[1].header.seq >= slots_[0].header.seq ? 1 : 0;
    }
    if (has_images_[1]) {
        return 1;
    }
    if (has_images_[0]) {
        return 0;
    }
    return 0;
}

std::unique_ptr<LvglImage> RemoteWallpaperStore::LoadImage(uint8_t mode, uint32_t index) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    uint8_t slot = mode == 1 ? 1 : 0;
    if (!HasImages(slot) || index >= slots_[slot].entries.size()) {
        return nullptr;
    }

    const StoredEntry& entry = slots_[slot].entries[index];
    uint8_t* compressed = reinterpret_cast<uint8_t*>(
        heap_caps_malloc(entry.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compressed == nullptr) {
        ESP_LOGW(kTag, "Failed to allocate %u bytes for remote wallpaper",
                 static_cast<unsigned>(entry.size));
        return nullptr;
    }

    esp_err_t err =
        esp_partition_read(partition_, SlotOffset(slot) + entry.offset, compressed, entry.size);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Failed to read remote wallpaper %u: %s", static_cast<unsigned>(index),
                 esp_err_to_name(err));
        heap_caps_free(compressed);
        return nullptr;
    }

    if (CalculateChecksum(compressed, entry.size) != entry.checksum) {
        ESP_LOGW(kTag, "Remote wallpaper %u checksum mismatch", static_cast<unsigned>(index));
        heap_caps_free(compressed);
        return nullptr;
    }

    return DecodeCompressedImageToLvgl(compressed, entry.size);
}
