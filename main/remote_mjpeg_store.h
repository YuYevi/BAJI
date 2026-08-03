#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include <esp_partition.h>

class RemoteMjpegStore {
public:
    static RemoteMjpegStore& GetInstance();

    bool IsSupported();
    bool Reload();
    bool Save(const std::string& role_id, const uint8_t* listen_data, size_t listen_size,
              const uint8_t* speak_data, size_t speak_size);
    bool Load(bool speaking, uint8_t** data, size_t* size);
    bool HasAssets();
    std::string GetRoleId();

private:
    RemoteMjpegStore() = default;

    bool EnsurePartition();
    void ResetMetadata();

    struct StoredEntry {
        uint32_t offset;
        uint32_t size;
        uint32_t checksum;
    };

    struct StoredHeader {
        uint32_t magic;
        uint16_t version;
        uint16_t count;
        uint32_t total_size;
        char role_id[64];
        StoredEntry entries[2];
        uint32_t header_checksum;
    };

    static_assert(sizeof(StoredEntry) == 12, "Unexpected remote MJPEG entry size");
    static_assert(sizeof(StoredHeader) == 104, "Unexpected remote MJPEG header size");

    const esp_partition_t* partition_ = nullptr;
    bool initialized_ = false;
    bool supported_ = false;
    bool metadata_loaded_ = false;
    bool has_assets_ = false;
    mutable std::recursive_mutex mutex_;
    StoredHeader header_ = {};
};
