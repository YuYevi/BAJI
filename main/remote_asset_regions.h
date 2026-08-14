#pragma once

#include <cstdint>

namespace RemoteAssetRegions {

constexpr uint32_t kMebibyte = 1024U * 1024U;
constexpr const char* kRemoteMediaPartitionLabel = "remote_media";

constexpr uint32_t kSystemSize = 8U * kMebibyte;  // Static assets partition.

constexpr uint32_t kWallpaperOffset = 0;
constexpr uint32_t kWallpaperSize = 8U * kMebibyte;  // Wallpaper batch bucket.

constexpr uint32_t kMjpegOffset = kWallpaperOffset + kWallpaperSize;
constexpr uint32_t kMjpegSize = 6U * kMebibyte;  // JPEG/MJPEG batch bucket.

constexpr uint32_t kRequiredRemoteMediaPartitionSize = kMjpegOffset + kMjpegSize;

static_assert(kSystemSize == 8U * kMebibyte, "Unexpected system assets partition size");
static_assert(kRequiredRemoteMediaPartitionSize == 14U * kMebibyte,
              "Unexpected remote media partition region layout");

}  // namespace RemoteAssetRegions
