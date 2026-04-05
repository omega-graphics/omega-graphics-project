#include "omega-common/common.h"
#include "omega-common/assets.h"

#include "../assetc/assetc.h"

typedef unsigned char Byte;

namespace OmegaCommon {

    OmegaCommon::Map<OmegaCommon::String,AssetLibrary::AssetBuffer>  AssetLibrary::assets_res;

    void AssetLibrary::loadAssetFile(OmegaCommon::FS::Path & path){
            auto str = path.absPath();
            std::ifstream in(str,std::ios::binary | std::ios::in);
            assetc::AssetsFileHeader header;
            in.read((char *)&header,sizeof(assetc::AssetsFileHeader));
            // MessageBoxA(GetDesktopWindow(),(std::string("Asset Count:") + std::to_string(header.asset_count)).c_str(),"NOTE", MB_OK);
            unsigned i = 0;
            while(i < header.asset_count){
                assetc::AssetsFileEntry fentry;
                in.read((char *)&fentry,sizeof(assetc::AssetsFileEntry));
                /// Read/Buffer the Asset Name
                Byte * name = new Byte[fentry.string_size];
                in.read((char *)name,fentry.string_size);

                /// Read/Buffer the Asset Data
                Byte * data = new Byte[fentry.file_size];
                in.read((char *)data,fentry.file_size);

                std::string filename ((char *)name,fentry.string_size);
                delete[] name;
                AssetBuffer buffer {};
                buffer.filesize = fentry.file_size;
                buffer.data = data;
                assets_res.insert(std::make_pair(std::move(filename),std::move(buffer)));
                i += 1;
            };
            // MessageBoxA(GetDesktopWindow(),"Read Assets File","NOTE", MB_OK);

            in.close();
        };

    void loadAssetFile(OmegaCommon::FS::Path path){
        AssetLibrary::loadAssetFile(path);
    };

};