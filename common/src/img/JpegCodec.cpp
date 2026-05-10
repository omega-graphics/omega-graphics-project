#include "ImgCodecPriv.h"

#include <memory>

#include <turbojpeg.h>

namespace OmegaCommon::Img {

    class JPEGCodec : public ImgCodec {
        typedef unsigned char tjByte;
        bool load_jpeg_from_file() {
            in.seekg(0, in.end);
            std::streampos endPos = in.tellg();
            in.seekg(0, in.beg);
            if (endPos <= 0) {
                return false;
            }
            std::size_t len = static_cast<std::size_t>(endPos);

            auto dataBuf = new tjByte[len];
            in.read(reinterpret_cast<char *>(dataBuf), static_cast<std::streamsize>(len));
            if (static_cast<std::size_t>(in.gcount()) != len) {
                delete[] dataBuf;
                return false;
            }

            auto decomp = tjInitDecompress();
            if (decomp == nullptr) {
                delete[] dataBuf;
                return false;
            }

            int w = 0, h = 0;
            int samp = 0;
            int colorspace = 0;
            if (tjDecompressHeader3(decomp, dataBuf, static_cast<unsigned long>(len), &w, &h, &samp, &colorspace) != 0) {
                tjDestroy(decomp);
                delete[] dataBuf;
                return false;
            }

            const std::size_t stride = static_cast<std::size_t>(w) * 4;
            auto * pixels = new tjByte[stride * static_cast<std::size_t>(h)];
            if (tjDecompress2(decomp, dataBuf, static_cast<unsigned long>(len), pixels, w, static_cast<int>(stride), h, TJPF_RGBA, TJFLAG_BOTTOMUP | TJFLAG_ACCURATEDCT) != 0) {
                delete[] pixels;
                tjDestroy(decomp);
                delete[] dataBuf;
                return false;
            }

            tjDestroy(decomp);
            delete[] dataBuf;

            Header header{};
            header.width = static_cast<std::uint32_t>(w);
            header.height = static_cast<std::uint32_t>(h);
            header.channels = 4;
            header.bitDepth = 8;
            header.compression_method = 0;
            header.interlace_type = 0;
            header.color_format = ColorFormat::RGBA;
            header.alpha_format = AlphaFormat::Ignore;
            header.stride = stride;

            storage->data = pixels;
            storage->header = header;
            storage->sRGB = false;
            storage->hasGamma = false;
            storage->gamma = 0.0;
            return true;
        }
    public:
        void readToStorage() override {
            if (!load_jpeg_from_file()) {
                storage->data = nullptr;
            }
        }
        JPEGCodec(std::istream & stream, BitmapImage * res) : ImgCodec(stream, res) {}
    };

    std::unique_ptr<ImgCodec> getJpegCodec(std::istream & in, BitmapImage * img) {
        return std::make_unique<JPEGCodec>(in, img);
    }

}
