#include "omega-common/img.h"
#include "omega-common/net.h"
#include "omega-common/assets.h"

#include <fstream>
#include <memory>
#include <sstream>

#include "ImgCodecPriv.h"

namespace OmegaCommon::Img {

namespace {

    Optional<Format> imageFormatForExtension(StrRef ext) {
        if (ext == "png") {
            return Format::PNG;
        }
        if (ext == "tif" || ext == "tiff") {
            return Format::TIFF;
        }
        if (ext == "jpg" || ext == "jpeg") {
            return Format::JPEG;
        }
        return std::nullopt;
    }

    struct ImgBuffer : public std::streambuf {
        ImgBuffer(void * begin, void * end) {
            this->setg((char *)begin, (char *)begin, (char *)end);
        }
    protected:
        pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                         std::ios_base::openmode which = std::ios_base::in) override {
            if (!(which & std::ios_base::in)) {
                return pos_type(off_type(-1));
            }
            char * base = eback();
            char * end = egptr();
            char * target = nullptr;
            if (dir == std::ios_base::beg) {
                target = base + off;
            } else if (dir == std::ios_base::cur) {
                target = gptr() + off;
            } else if (dir == std::ios_base::end) {
                target = end + off;
            } else {
                return pos_type(off_type(-1));
            }
            if (target < base || target > end) {
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

    std::shared_ptr<HttpClientContext> http_client;
}

// Defined in PngCodec.cpp / JpegCodec.cpp / TiffCodec.cpp:
std::unique_ptr<ImgCodec> getPngCodec(std::istream & in, BitmapImage * img);
std::unique_ptr<ImgCodec> getJpegCodec(std::istream & in, BitmapImage * img);
std::unique_ptr<ImgCodec> getTiffCodec(std::istream & in, BitmapImage * img);

static std::unique_ptr<ImgCodec> obtainCodecForImageFormat(Format & format, std::istream & in, BitmapImage * img) {
    switch (format) {
        case Format::PNG:
            return getPngCodec(in, img);
        case Format::TIFF:
            return getTiffCodec(in, img);
        case Format::JPEG:
            return getJpegCodec(in, img);
        default:
            return nullptr;
    }
}

Result<BitmapImage, std::string> loadFromAssets(AssetBundle & bundle, FS::Path path) {
    auto format = imageFormatForExtension(path.ext());
    if (!format.has_value()) {
        return Result<BitmapImage, std::string>::err(std::string("Unsupported image asset format"));
    }

    auto assetInfo = bundle.info(path.str());
    if (!assetInfo.has_value()) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to Load Image from Assets"));
    }

    if (assetInfo->type != AssetType::Image &&
        assetInfo->type != AssetType::Raw) {
        return Result<BitmapImage, std::string>::err(std::string("Asset is not tagged as an image"));
    }

    auto bytesResult = bundle.load(path.str());
    if (bytesResult.isErr()) {
        return Result<BitmapImage, std::string>::err(bytesResult.error());
    }

    auto & bytes = bytesResult.value();
    if (bytes.empty()) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to Load Image from Assets"));
    }

    return loadFromBuffer(bytes.data(), bytes.size(), *format);
}

Result<BitmapImage, std::string> loadFromFile(FS::Path path) {
    auto format = imageFormatForExtension(path.ext());
    if (!format.has_value()) {
        return Result<BitmapImage, std::string>::err(std::string("Unsupported image file format"));
    }

    BitmapImage img{};
    auto os_corrected_path = path.absPath();
    std::ifstream in(os_corrected_path, std::ios::binary);
    if (!in.is_open()) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to Load Image from File"));
    }

    std::unique_ptr<ImgCodec> codec = obtainCodecForImageFormat(*format, in, &img);
    if (!codec) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to Load Image from File"));
    }
    codec->readToStorage();
    if (img.empty()) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to Load Image from File"));
    }
    return Result<BitmapImage, std::string>::ok(std::move(img));
}

Result<BitmapImage, std::string> loadFromURL(StrRef url, Format format) {
    if (!http_client) {
        http_client = HttpClientContext::Create();
    }
    auto resp = http_client->makeRequest({url}).get();
    return loadFromBuffer((Byte *)resp.body.data(), resp.body.size(), format);
}

Result<BitmapImage, std::string> loadFromBuffer(Byte * bufferData, std::size_t bufferSize, Format f) {
    if (bufferData == nullptr || bufferSize == 0) {
        return Result<BitmapImage, std::string>::err(std::string("Empty image buffer"));
    }
    BitmapImage img{};
    ImgBuffer imgBuffer{bufferData, bufferData + bufferSize};
    std::istream in(&imgBuffer);
    std::unique_ptr<ImgCodec> codec = obtainCodecForImageFormat(f, in, &img);
    if (!codec) {
        return Result<BitmapImage, std::string>::err(std::string("Unsupported image format"));
    }
    codec->readToStorage();
    if (img.empty()) {
        return Result<BitmapImage, std::string>::err(std::string("Failed to decode image buffer"));
    }
    return Result<BitmapImage, std::string>::ok(std::move(img));
}

}
