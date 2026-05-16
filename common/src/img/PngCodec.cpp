#include "ImgCodecPriv.h"

#include <memory>

#include <png.h>

namespace OmegaCommon::Img {

    class PNGCodec : public ImgCodec {
        int srgb_intent;
        int num_palette;
        png_colorp palette;
        static void userReadData(png_structp png_ptr, png_bytep data, png_size_t length) {
            png_voidp stream = png_get_io_ptr(png_ptr);
            ((std::istream *)stream)->read((char *)data, std::streamsize(length));
        }
        #define SIG_SIZE 8
        bool validate_signature() {

            png_byte sig[SIG_SIZE];

            in.read((char *)sig, SIG_SIZE);
            if (!in.good()) {
                return false;
            }

            return png_sig_cmp(sig, 0, SIG_SIZE) == 0;
        }

        Header read_header(png_structp png_ptr, png_infop info_ptr) {
            ColorFormat colorFormat;
            AlphaFormat alphaFormat;

            std::uint32_t width;
            std::uint32_t height;
            int bitDepth;
            int channels;
            int color_type;
            int filter_method;
            int compression_method;
            int interlace_type;
            png_get_IHDR(png_ptr, info_ptr, &width, &height, &bitDepth, &color_type, &interlace_type, &compression_method, &filter_method);
            channels = png_get_channels(png_ptr, info_ptr);

            if (bitDepth == 16) {
                png_set_strip_16(png_ptr);
                bitDepth = 8;
            }

            std::size_t rowBytes = (width * channels * bitDepth) / 8;

            switch (color_type) {
                case PNG_COLOR_TYPE_RGBA: {
                    colorFormat = ColorFormat::RGBA;
                    alphaFormat = AlphaFormat::Straight;
                    break;
                }
                case PNG_COLOR_TYPE_RGB: {
                    png_bytep trans_alpha;
                    int num_trans;
                    png_color_16p trans_color;
                    if (png_get_tRNS(png_ptr, info_ptr, &trans_alpha, &num_trans, &trans_color) == PNG_INFO_tRNS) {
                        colorFormat = ColorFormat::RGBA;
                        alphaFormat = AlphaFormat::Straight;
                        png_set_tRNS_to_alpha(png_ptr);
                    } else {
                        colorFormat = ColorFormat::RGB;
                        alphaFormat = AlphaFormat::Ignore;
                    }

                    break;
                }
                case PNG_COLOR_TYPE_GRAY: {
                    png_set_gray_to_rgb(png_ptr);
                    colorFormat = ColorFormat::RGB;
                    alphaFormat = AlphaFormat::Ignore;
                    break;
                }
                case PNG_COLOR_TYPE_PALETTE: {
                    png_get_PLTE(png_ptr, info_ptr, &palette, &num_palette);
                    png_set_palette_to_rgb(png_ptr);
                    colorFormat = ColorFormat::RGB;
                    alphaFormat = AlphaFormat::Ignore;
                    break;
                }
                default: {
                    colorFormat = ColorFormat::Unknown;
                    alphaFormat = AlphaFormat::Unknown;
                    break;
                }
            }

            return Header{width, height, channels, bitDepth, compression_method, interlace_type, colorFormat, alphaFormat, rowBytes};
        }

        Profile read_profile(png_structp png_ptr, png_infop info_ptr) {
            png_charp name;
            int compression_ty;
            png_bytep profile;
            png_uint_32 length;

            if (png_get_iCCP(png_ptr, info_ptr, &name, &compression_ty, &profile, &length) == PNG_INFO_iCCP) {
                storage->sRGB = false;
                return Profile({name, compression_ty});
            }
            /// Else use SRGB!
            int srgb_intent;
            if (png_get_sRGB(png_ptr, info_ptr, &srgb_intent) == PNG_INFO_sRGB) {
                storage->sRGB = true;
                this->srgb_intent = srgb_intent;
            }
            return {};
        }


        bool load_png_from_file() {
            if (validate_signature()) {
                png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
                if (!png_ptr) {
                    return false;
                }

                png_infop info_ptr = png_create_info_struct(png_ptr);
                if (!info_ptr) {
                    png_destroy_read_struct(&png_ptr, 0, 0);
                    return false;
                }

                png_bytep * rowPtrs = NULL;
                PixelStorage pixels;

                if (setjmp(png_jmpbuf(png_ptr))) {

                    png_destroy_read_struct(&png_ptr, &info_ptr, 0);
                    if (rowPtrs != NULL) delete[] rowPtrs;
                    // `pixels` frees its buffer (if any) on scope exit.

                    return false;
                }

                png_set_read_fn(png_ptr, (png_voidp)&in, userReadData);

                png_set_sig_bytes(png_ptr, SIG_SIZE);

                png_read_info(png_ptr, info_ptr);
                /// Set Info!
                auto header = read_header(png_ptr, info_ptr);
                auto profile = read_profile(png_ptr, info_ptr);
                /// Gamma Chunk!
                double file_gamma;
                if (png_get_gAMA(png_ptr, info_ptr, &file_gamma) == PNG_INFO_gAMA) {
                    storage->hasGamma = true;
                    storage->gamma = file_gamma;
                    if (!storage->sRGB) {
                        png_set_gamma(png_ptr, DEFAULT_SCREEN_GAMMA, file_gamma);
                    }
                } else {
                    storage->hasGamma = false;
                }
                png_bytep trans_alpha;
                int num_trans;
                png_color_16p trans_color;
                png_get_tRNS(png_ptr, info_ptr, &trans_alpha, &num_trans, &trans_color);

                /// If SRGB colorspace is used!
                if (storage->sRGB) {
                    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_cHRM) && png_get_valid(png_ptr, info_ptr, PNG_INFO_gAMA)) {
                        png_set_sRGB_gAMA_and_cHRM(png_ptr, info_ptr, srgb_intent);
                        storage->hasGamma = false;
                    } else {
                        png_set_sRGB(png_ptr, info_ptr, srgb_intent);
                    }
                    storage->sRGB = false;
                }

                const std::size_t totalBytes =
                    (static_cast<std::size_t>(header.width) * header.height *
                     header.bitDepth * header.channels) / 8;
                rowPtrs = new png_bytep[header.height];
                pixels = PixelStorage::allocate(totalBytes);
                Byte * data = pixels.data();
                unsigned int stride = (header.width * header.bitDepth * header.channels) / 8;

                for (std::size_t i = 0; i < header.height; i++) {
                    png_uint_32 ptr = (header.height - i - 1) * stride;
                    /// Read from top to bottom!
                    rowPtrs[header.height - 1 - i] = (png_bytep)data + ptr;
                }

                png_read_image(png_ptr, rowPtrs);

                png_read_end(png_ptr, info_ptr);

                delete[] rowPtrs;
                png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)0);

                storage->pixels = std::move(pixels);
                storage->header = header;
                storage->profile = profile;

                return true;

            } else {
                return false;
            }
        }
    public:
        void readToStorage() override {
            if (!load_png_from_file()) {
                storage->pixels.reset();
            }
        }
        PNGCodec(std::istream & stream, BitmapImage * res) : ImgCodec(stream, res) {}
        ~PNGCodec() {}
    };

    std::unique_ptr<ImgCodec> getPngCodec(std::istream & in, BitmapImage * img) {
        return std::make_unique<PNGCodec>(in, img);
    }

}
