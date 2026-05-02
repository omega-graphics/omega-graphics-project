#include "omegaWTK/Media/ImgCodec.h"
#include "omegaWTK/Core/Core.h"

#include "omega-common/net.h"

#include "omega-common/assets.h"

#include <sstream>



#include <iostream>
#include <fstream>
#include <memory>

#include "ImgCodecPriv.h"



namespace OmegaWTK::Media {

namespace {

    OmegaCommon::Optional<BitmapImage::Format> imageFormatForExtension(OmegaCommon::StrRef ext){
        if(ext == "png"){
            return BitmapImage::PNG;
        }
        if(ext == "tif" || ext == "tiff"){
            return BitmapImage::TIFF;
        }
        if(ext == "jpg" || ext == "jpeg"){
            return BitmapImage::JPEG;
        }
        return std::nullopt;
    }

}




Core::UniquePtr<ImgCodec> getPngCodec(Core::IStream &in,BitmapImage *img);
Core::UniquePtr<ImgCodec> getJpegCodec(Core::IStream &in,BitmapImage *img);
Core::UniquePtr<ImgCodec> getTiffCodec(Core::IStream &in,BitmapImage *img);

Core::UniquePtr<ImgCodec> obtainCodecForImageFormat(BitmapImage::Format &format,Core::IStream &in,BitmapImage *img){
    switch (format) {
        case BitmapImage::PNG:
        {
            return getPngCodec(in,img);
            break;
        }
        case BitmapImage::TIFF:
        {
            
            return getTiffCodec(in,img);
            break;
        }
        case BitmapImage::JPEG:
        {
            return getJpegCodec(in,img);
            break;
        }
        default:
            return nullptr;
            break;
    }
    
   
};

    struct ImgBuffer : public std::streambuf {
        ImgBuffer(void *begin,void *end){
            this->setg((char *)begin,(char *)begin,(char *)end);
        };
    protected:
        pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                         std::ios_base::openmode which = std::ios_base::in) override {
            if(!(which & std::ios_base::in)){
                return pos_type(off_type(-1));
            }
            char *base = eback();
            char *end = egptr();
            char *target = nullptr;
            if(dir == std::ios_base::beg){
                target = base + off;
            } else if(dir == std::ios_base::cur){
                target = gptr() + off;
            } else if(dir == std::ios_base::end){
                target = end + off;
            } else {
                return pos_type(off_type(-1));
            }
            if(target < base || target > end){
                return pos_type(off_type(-1));
            }
            setg(base, target, end);
            return pos_type(target - base);
        }
        pos_type seekpos(pos_type pos,
                         std::ios_base::openmode which = std::ios_base::in) override {
            return seekoff(off_type(pos), std::ios_base::beg, which);
        }
    };

    StatusWithObj<BitmapImage> loadImageFromAssets(OmegaCommon::AssetBundle &bundle,OmegaCommon::FS::Path path){
        auto format = imageFormatForExtension(path.ext());
        if(!format.has_value()){
            return {"Unsupported image asset format"};
        }

        auto assetInfo = bundle.info(path.str());
        if(!assetInfo.has_value()){
            return {"Failed to Load Image from Assets"};
        }

        if(assetInfo->type != OmegaCommon::AssetType::Image &&
           assetInfo->type != OmegaCommon::AssetType::Raw){
            return {"Asset is not tagged as an image"};
        }

        auto bytesResult = bundle.load(path.str());
        if(bytesResult.isErr()){
            return {bytesResult.error().c_str()};
        }

        auto &bytes = bytesResult.value();
        if(bytes.empty()){
            return {"Failed to Load Image from Assets"};
        }

        return loadImageFromBuffer(bytes.data(),bytes.size(),*format);
    };

    StatusWithObj<BitmapImage> loadImageFromFile(OmegaCommon::FS::Path path) {
        BitmapImage img;
        auto format = imageFormatForExtension(path.ext());
        if(!format.has_value()){
            return {"Unsupported image file format"};
        }
        auto os_corrected_path = path.absPath();
        std::ifstream in(os_corrected_path,std::ios::binary);
        if(in.is_open()){
            Core::UniquePtr<ImgCodec> codec = obtainCodecForImageFormat(*format,in,&img);
            if(!codec)
                return {"Failed to Load Image from File"};
            codec->readToStorage();
            if(img.data == nullptr)
                return {"Failed to Load Image from File"};
            return std::move(img);
        }
        else
            return {"Failed to Load Image from File"};
    };

    std::shared_ptr<OmegaCommon::HttpClientContext> http_client;

    StatusWithObj<BitmapImage> loadImageFromURL(OmegaCommon::StrRef url,BitmapImage::Format format){
        if(!http_client){
            http_client = OmegaCommon::HttpClientContext::Create();
        }
        auto resp = http_client->makeRequest({url}).get();
        return loadImageFromBuffer((ImgByte *)resp.body.data(),resp.body.size(),format);
    }

    StatusWithObj<BitmapImage> loadImageFromBuffer(ImgByte *bufferData,size_t bufferSize,BitmapImage::Format f){
        if(bufferData == nullptr || bufferSize == 0){
            return {"Empty image buffer"};
        }
        BitmapImage img;
        ImgBuffer imgBuffer {bufferData,bufferData + bufferSize};
        std::istream in(&imgBuffer);
        Core::UniquePtr<ImgCodec> codec = obtainCodecForImageFormat(f,in,&img);
        if(!codec){
            return {"Unsupported image format"};
        }
        codec->readToStorage();
        if(!img.data)
            return {"Failed to decode image buffer"};
        return std::move(img);
    };
};
