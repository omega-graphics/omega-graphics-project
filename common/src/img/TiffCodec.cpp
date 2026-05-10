#include "ImgCodecPriv.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include <tiffio.h>

namespace OmegaCommon::Img {

    class TiffIStream {
        std::istream * in = nullptr;
        thandle_t self;

        std::size_t strSize;

        TIFF * tiff;

        static tmsize_t readProc(thandle_t s, void * data, tmsize_t size) {
            auto * str = (TiffIStream *)s;
            std::size_t dataLeft = str->strSize - str->in->tellg();
            auto amountToRead = dataLeft > size ? size : dataLeft;
            str->in->read((char *)data, amountToRead);
            return str->in->gcount();
        }

        static tmsize_t writeProc(thandle_t s, void * data, tmsize_t size) {
            auto * str = (TiffIStream *)s;
            return 0;
        }

        static toff_t seekProc(thandle_t s, toff_t off, int whence) {
            auto * str = (TiffIStream *)s;
            std::ios::seekdir dir;
            switch (whence) {
                case SEEK_SET: {
                    dir = std::ios::beg;
                    break;
                }
                case SEEK_CUR: {
                    dir = std::ios::cur;
                    break;
                }
                case SEEK_END: {
                    dir = std::ios::end;
                    break;
                }
                default: {
                    dir = std::ios::beg;
                    break;
                }
            }
            str->in->seekg(off, dir);
            return str->in->tellg();
        }

        static toff_t sizeProc(thandle_t s) {
            auto * str = (TiffIStream *)s;
            str->in->seekg(0, std::ios::end);
            auto size = str->in->tellg();
            str->in->seekg(0, std::ios::beg);
            return toff_t(size);
        }

        static int closeProc(thandle_t s) {
            auto * str = (TiffIStream *)s;
            str->in = nullptr;
            return -1;
        }

    public:
        TiffIStream() : self((thandle_t)this) {}
        TIFF * open(std::istream * in) {
            this->in = in;
            in->seekg(0, std::ios::end);
            strSize = in->tellg();
            in->seekg(0, std::ios::beg);

            return TIFFClientOpen("TiffIStream", "rb", self, readProc, writeProc, seekProc, closeProc, sizeProc, nullptr, nullptr);
        }


    };

    class TiffCodec : public ImgCodec {
        bool load_tiff_from_file() {
            TiffIStream tiffStream;
            TIFF * tiff = tiffStream.open(&in);
            if (tiff == nullptr) {
                return false;
            }

            std::uint32_t width = 0, height = 0;
            std::uint16_t srcBitDepth = 0, srcChannels = 0, compression = 0;

            TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
            TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
            TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &srcBitDepth);
            TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &srcChannels);
            TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression);

            Header header{};
            header.width = width;
            header.height = height;
            // TIFFReadRGBAImage normalizes to 8-bit RGBA regardless of source layout.
            header.bitDepth = 8;
            header.channels = 4;
            header.stride = static_cast<std::size_t>(width) * 4;
            header.color_format = ColorFormat::RGBA;
            header.alpha_format = AlphaFormat::Straight;
            header.compression_method = static_cast<int>(compression);
            header.interlace_type = 0;

            bool rc = false;

            std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            std::uint32_t * buffer = (std::uint32_t *)_TIFFmalloc(pixelCount * sizeof(std::uint32_t));
            if (buffer != nullptr) {
                if (TIFFReadRGBAImage(tiff, width, height, buffer, 0)) {
                    storage->data = (Byte *)buffer;
                    storage->header = header;
                    rc = true;
                } else {
                    _TIFFfree(buffer);
                }
            }
            TIFFClose(tiff);
            return rc;
        }
    public:
        void readToStorage() override {
            if (!load_tiff_from_file()) {
                storage->data = nullptr;
            }
        }
        TiffCodec(std::istream & stream, BitmapImage * res) : ImgCodec(stream, res) {}
    };

    std::unique_ptr<ImgCodec> getTiffCodec(std::istream & in, BitmapImage * img) {
        return std::make_unique<TiffCodec>(in, img);
    }

}
