#include "omega-common/fs.h"

#include <cstdint>

#ifndef OMEGAWTK_ASSETS_H
#define OMEGAWTK_ASSETS_H

namespace OmegaCommon {

  enum class AssetType : std::uint16_t {
    Raw = 0,
    Image = 1,
    Font = 2,
    Shader = 3,
    Text = 4,
    Audio = 5,
    Binary = 6,
    Model = 7,
    Material = 8,
    Scene = 9,
    Animation = 10,
    Skeleton = 11,
  };

  OMEGACOMMON_EXPORT const char *assetTypeName(AssetType type);

  struct OMEGACOMMON_EXPORT AssetInfo {
    String name;
    AssetType type = AssetType::Raw;
    size_t rawSize = 0;
  };

  class OMEGACOMMON_EXPORT AssetBundle {
    public:
      struct Impl;
    private:
      Impl *impl = nullptr;

      explicit AssetBundle(Impl *p);
    public:
      AssetBundle() = default;
      AssetBundle(const AssetBundle &) = delete;
      AssetBundle & operator=(const AssetBundle &) = delete;
      AssetBundle(AssetBundle && other) noexcept;
      AssetBundle & operator=(AssetBundle && other) noexcept;
      ~AssetBundle();

      static Result<AssetBundle, String> open(FS::Path path);
      static Result<AssetBundle, String> open(FS::Path path, ArrayRef<std::uint8_t> key);

      OMEGACOMMON_NODISCARD size_t entryCount() const;
      OMEGACOMMON_NODISCARD Optional<AssetInfo> info(StrRef name) const;
      OMEGACOMMON_NODISCARD bool contains(StrRef name) const;
      OMEGACOMMON_NODISCARD Vector<AssetInfo> entries() const;

      OMEGACOMMON_NODISCARD Result<Vector<std::uint8_t>, String> load(StrRef name) const;
      OMEGACOMMON_NODISCARD Result<String, String> loadText(StrRef name) const;
  };
  
  class [[deprecated("Use AssetBundle instead")]] OMEGACOMMON_EXPORT AssetLibrary {
    public:
        
        struct AssetBuffer {
            size_t filesize;
            void *data;
            AssetType type = AssetType::Raw;
        };
        static OmegaCommon::Map<OmegaCommon::String,AssetBuffer> assets_res;
        static void loadAssetFile(OmegaCommon::FS::Path & path);
    };

};

#endif // OMEGAWTK_ASSETS_H
