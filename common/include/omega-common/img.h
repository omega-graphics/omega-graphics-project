#ifndef OMEGA_COMMON_IMG_H
#define OMEGA_COMMON_IMG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        int channels = 0;
        int bitDepth = 0;
        int compression_method = 0;
        int interlace_type = 0;
        ColorFormat color_format = ColorFormat::Unknown;
        AlphaFormat alpha_format = AlphaFormat::Unknown;
        std::size_t stride = 0;
    };

    /// Move-only owner of a decoded pixel buffer.
    ///
    /// Codecs allocate with whatever allocator the underlying library
    /// requires (libpng uses `new[]`, turbojpeg uses `new[]`, libtiff
    /// uses `_TIFFmalloc`) and hand the pointer to PixelStorage along
    /// with the matching deleter. PixelStorage runs that deleter on
    /// destruction, so callers never have to know which backend
    /// produced the bytes.
    ///
    /// Also supports a non-owning `view()` for cases where the bytes
    /// are borrowed from another owner (e.g. a Media Foundation sample
    /// buffer being adapted into a `BitmapImage` for the video sink).
    class OMEGACOMMON_EXPORT PixelStorage {
    public:
        using Deleter = void(*)(Byte *);

        PixelStorage() = default;
        ~PixelStorage() { reset(); }

        PixelStorage(const PixelStorage &) = delete;
        PixelStorage & operator=(const PixelStorage &) = delete;

        PixelStorage(PixelStorage && other) noexcept
            : data_(other.data_), size_(other.size_), deleter_(other.deleter_) {
            other.data_ = nullptr;
            other.size_ = 0;
            other.deleter_ = nullptr;
        }

        PixelStorage & operator=(PixelStorage && other) noexcept {
            if (this != &other) {
                reset();
                data_ = other.data_;
                size_ = other.size_;
                deleter_ = other.deleter_;
                other.data_ = nullptr;
                other.size_ = 0;
                other.deleter_ = nullptr;
            }
            return *this;
        }

        /// Take ownership of an existing buffer. `deleter` must match the
        /// allocator that produced `data` (e.g. `_TIFFfree` for libtiff
        /// buffers, a wrapper around `delete[]` for `new[]` buffers).
        /// Pass `nullptr` to create a non-owning storage — equivalent to
        /// calling `view()`.
        static PixelStorage adopt(Byte * data, std::size_t size, Deleter deleter) {
            PixelStorage s;
            s.data_ = data;
            s.size_ = size;
            s.deleter_ = deleter;
            return s;
        }

        /// Allocate `size` bytes with `new[]`. The matching `delete[]`
        /// deleter is installed automatically.
        static PixelStorage allocate(std::size_t size) {
            if (size == 0) return PixelStorage{};
            PixelStorage s;
            s.data_ = new Byte[size];
            s.size_ = size;
            s.deleter_ = &deleteArrayDeleter;
            return s;
        }

        /// Non-owning view over an externally-managed buffer. Useful for
        /// adapting video-frame sample buffers into BitmapImage without
        /// per-frame copies. The caller is responsible for keeping the
        /// underlying memory alive for as long as the PixelStorage is
        /// read.
        static PixelStorage view(Byte * data, std::size_t size) {
            return adopt(data, size, nullptr);
        }

        void reset() noexcept {
            if (deleter_ && data_) {
                deleter_(data_);
            }
            data_ = nullptr;
            size_ = 0;
            deleter_ = nullptr;
        }

        Byte * data() noexcept { return data_; }
        const Byte * data() const noexcept { return data_; }
        std::size_t size() const noexcept { return size_; }
        bool empty() const noexcept { return data_ == nullptr || size_ == 0; }
        bool owns() const noexcept { return deleter_ != nullptr; }

    private:
        static void deleteArrayDeleter(Byte * p) { delete[] p; }

        Byte * data_ = nullptr;
        std::size_t size_ = 0;
        Deleter deleter_ = nullptr;
    };

    /// Decoded image. Move-only; owns its pixel buffer through
    /// `pixels`. The legacy raw-pointer `data` field has been replaced
    /// by `data()`/`byteSize()` accessors that forward to `pixels`.
    struct OMEGACOMMON_EXPORT BitmapImage {
        Profile profile;
        PixelStorage pixels;
        Header header;
        bool sRGB = false;
        bool hasGamma = false;
        double gamma = 0.0;

        BitmapImage() = default;
        ~BitmapImage() = default;

        BitmapImage(const BitmapImage &) = delete;
        BitmapImage & operator=(const BitmapImage &) = delete;

        BitmapImage(BitmapImage &&) noexcept = default;
        BitmapImage & operator=(BitmapImage &&) noexcept = default;

        Byte * data() noexcept { return pixels.data(); }
        const Byte * data() const noexcept { return pixels.data(); }
        std::size_t byteSize() const noexcept { return pixels.size(); }
        bool empty() const noexcept { return pixels.empty(); }
    };

    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromFile(FS::Path path);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromAssets(AssetBundle & bundle, FS::Path path);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromBuffer(Byte * bufferData, std::size_t bufferSize, Format f);
    OMEGACOMMON_EXPORT Result<BitmapImage, std::string> loadFromURL(StrRef url, Format format);

}

}

#endif
