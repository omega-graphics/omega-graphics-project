#include "omega-common/fs.h"

#ifndef OMEGAWTK_ASSETS_H
#define OMEGAWTK_ASSETS_H

namespace OmegaCommon {
  
  class AssetLibrary {
    public:
        
        struct AssetBuffer {
            size_t filesize;
            void *data;
        };
        static OmegaCommon::Map<OmegaCommon::String,AssetBuffer> assets_res;
        static void loadAssetFile(OmegaCommon::FS::Path & path);
    };

};

#endif // OMEGAWTK_ASSETS_H