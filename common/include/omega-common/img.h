#ifndef OMEGA_COMMON_IMG_H
#define OMEGA_COMMON_IMG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "omega-common/utils.h"
#include "omega-common/fs.h"
#include "omega-common/assets.h"

namespace OmegaCommon {

namespace Img {

    struct Profile {
        String name;
        int compression_type;
    };

    using Byte = unsigned char;

    enum class ColorFormat : std::uint8_t {
        RGB,
        RGBA,
        Pallete,
        Unknown
    };

    enum class AlphaFormat : std::uint8_t {
        Straight,
        Premultipled,
        Ignore,
        Unknown
    };

    enum class Format : std::uint8_t {
        PNG,
        TIFF,
        JPEG
    };

    struct Header {
        std::uint32_t width;
        std::uint32_t height;
        int channels;
        int bitDepth;
        int compression_method;
        int interlace_type;
        ColorFormat color_format;
        AlphaFormat alpha_format;
        std::size_t stride;
    };

    struct OMEGACOMMON_EXPORT BitmapImage {
        Profile profile;
        Byte * data;
        Header header;
        bool sRGB;
        bool hasGamma;
        double gamma;
    };

    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromFile(FS::Path path);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromAssets(AssetBundle & bundle, FS::Path path);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromBuffer(Byte * bufferData, std::size_t bufferSize, Format f);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromURL(StrRef url, Format format);

}

}

#endif
