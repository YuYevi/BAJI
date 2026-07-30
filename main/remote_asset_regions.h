#pragma once

#include <cstdint>

namespace RemoteAssetRegions {

constexpr uint32_t kMebibyte = 1024U * 1024U;

constexpr uint32_t kSystemOffset = 0;
constexpr uint32_t kSystemSize = 6U * kMebibyte;

constexpr uint32_t kWallpaperOffset = kSystemOffset + kSystemSize;
constexpr uint32_t kWallpaperSize = 5U * kMebibyte;

constexpr uint32_t kMjpegOffset = kWallpaperOffset + kWallpaperSize;
constexpr uint32_t kMjpegSize = 5U * kMebibyte;

constexpr uint32_t kRequiredAssetsPartitionSize = kMjpegOffset + kMjpegSize;

static_assert(kRequiredAssetsPartitionSize == 16U * kMebibyte,
              "Unexpected assets partition region layout");

}  // namespace RemoteAssetRegions
