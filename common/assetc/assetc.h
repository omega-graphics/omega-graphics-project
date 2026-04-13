#include <cstdint>
#include <type_traits>

#include "omega-common/assets.h"

#ifndef OMEGAWTK_ASSETC_ASSETC_H
#define OMEGAWTK_ASSETC_ASSETC_H

namespace OmegaCommon::assetc {

inline constexpr std::uint8_t BundleMagic[4] = {'P', 'A', 'K', '\0'};
inline constexpr std::uint16_t BundleVersion = 2U;

enum class BundleFlags : std::uint16_t {
  None = 0,
  Compressed = 1U << 0,
  Encrypted = 1U << 1,
  Signed = 1U << 2,
};

enum class AssetEntryFlags : std::uint32_t {
  None = 0,
  Compressed = 1U << 0,
  Encrypted = 1U << 1,
};

using AssetType = OmegaCommon::AssetType;

#pragma pack(push, 1)

struct LegacyAssetsFileHeader {
  std::uint32_t asset_count;
};

struct LegacyAssetsFileEntry {
  std::uint64_t string_size;
  std::uint64_t file_size;
};

struct BundleHeader {
  std::uint8_t magic[4] = {'P', 'A', 'K', '\0'};
  std::uint16_t version = BundleVersion;
  std::uint16_t flags = static_cast<std::uint16_t>(BundleFlags::None);
  std::uint32_t entryCount = 0;
  std::uint32_t stringTableSize = 0;
  std::uint64_t dataRegionOffset = 0;
  std::uint64_t dataRegionSize = 0;
  std::uint8_t bundleHash[32] = {};
};

struct AssetEntry {
  std::uint32_t nameOffset = 0;
  std::uint16_t nameLength = 0;
  std::uint16_t assetType = static_cast<std::uint16_t>(AssetType::Raw);
  std::uint64_t dataOffset = 0;
  std::uint64_t rawSize = 0;
  std::uint64_t storedSize = 0;
  std::uint32_t flags = static_cast<std::uint32_t>(AssetEntryFlags::None);
  std::uint8_t entryHash[32] = {};
};

#pragma pack(pop)

using AssetsFileHeader = LegacyAssetsFileHeader;
using AssetsFileEntry = LegacyAssetsFileEntry;

constexpr bool hasBundleMagic(const std::uint8_t magic[4]) {
  return magic[0] == BundleMagic[0] && magic[1] == BundleMagic[1] &&
         magic[2] == BundleMagic[2] && magic[3] == BundleMagic[3];
}

static_assert(sizeof(LegacyAssetsFileHeader) == 4,
              "Legacy asset header layout changed.");
static_assert(sizeof(LegacyAssetsFileEntry) == 16,
              "Legacy asset entry layout changed.");
static_assert(sizeof(BundleHeader) == 64, "Bundle header layout changed.");
static_assert(sizeof(AssetEntry) == 68, "Bundle asset entry layout changed.");
static_assert(std::is_standard_layout_v<LegacyAssetsFileHeader>);
static_assert(std::is_standard_layout_v<LegacyAssetsFileEntry>);
static_assert(std::is_standard_layout_v<BundleHeader>);
static_assert(std::is_standard_layout_v<AssetEntry>);

}

#endif
