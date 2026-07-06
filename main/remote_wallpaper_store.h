#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <esp_partition.h>

class LvglImage;

std::unique_ptr<LvglImage> DecodeCompressedImageToLvgl(uint8_t* data, size_t size);

class RemoteWallpaperStore {
public:
    static RemoteWallpaperStore& GetInstance();

    bool IsSupported();
    bool Reload();
    bool Save(const std::vector<std::pair<const uint8_t*, size_t>>& images, uint8_t mode,
              uint32_t interval_ms);

    bool HasImages(uint8_t mode);
    uint32_t GetCount(uint8_t mode);
    uint32_t GetIntervalMs(uint8_t mode);
    uint8_t GetLastMode();
    std::unique_ptr<LvglImage> LoadImage(uint8_t mode, uint32_t index);

private:
    RemoteWallpaperStore() = default;

    bool EnsurePartition();
    void ResetMetadata();
    bool ReloadSlot(uint8_t mode);
    bool SaveSlot(const std::vector<std::pair<const uint8_t*, size_t>>& images, uint8_t mode,
                  uint32_t interval_ms);
    uint32_t SlotOffset(uint8_t mode) const;
    uint32_t SlotSize() const;

    struct StoredEntry {
        uint32_t offset;
        uint32_t size;
        uint32_t checksum;
        uint32_t reserved;
    };

    struct StoredHeader {
        uint32_t magic;
        uint16_t version;
        uint8_t mode;
        uint8_t reserved;
        uint32_t count;
        uint32_t interval_ms;
        uint32_t total_size;
        uint32_t seq;
        uint32_t header_checksum;
    };

    static_assert(sizeof(StoredEntry) == 16, "Unexpected remote wallpaper entry size");

    struct SlotMetadata {
        StoredHeader header = {};
        std::vector<StoredEntry> entries;
    };

    const esp_partition_t* partition_ = nullptr;
    bool initialized_ = false;
    bool supported_ = false;
    bool metadata_loaded_ = false;
    uint32_t region_offset_ = 0;
    uint32_t region_size_ = 0;
    SlotMetadata slots_[2];
    bool has_images_[2] = {false, false};
};
