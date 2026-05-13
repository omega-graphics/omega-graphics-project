#include "omegaGTE/GETextureAsset.h"
#include "GEVulkan.h"

#include "omega-common/img.h"
#include "omega-common/fs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

_NAMESPACE_BEGIN_

namespace {

// Phase 3.4 Vulkan implementation. Image decoding goes through
// OmegaCommon::Img (libpng / turbojpeg / libtiff already linked
// transitively via OmegaCommon), matching the user's "use our own image
// codec from OmegaCommon" directive for backends that can't lean on a
// platform-native loader. The decoded BitmapImage is normalized to
// 8-bit RGBA and uploaded via the engine's `ToGPU` path (LINEAR-tiled,
// HOST_VISIBLE) — `copyBytes` then does a direct memcpy into the image.
//
// Phase-3.4 v1 limitations (kept narrow on purpose; mirrored from the
// post-implementation notes in the plan doc):
// - Formats: PNG / JPEG / TIFF. KTX / KTX2 / HDR / BC need extra codec
//   support; stb_image can be folded into OmegaCommon::Img in a
//   follow-up per the user's note.
// - Mipmaps: `options.generateMipmaps` is honored as a best-effort hint
//   only. Runtime mip generation on Vulkan requires a graphics queue +
//   vkCmdBlitImage chain; that lives in the upload-queue refactor.
//   For now we always allocate mipLevels=1 and log a one-shot notice
//   when the caller asked for mips.
// - Texture kind: 2D only. 3D / cube / array decoders aren't on the
//   PNG/JPEG/TIFF code path anyway, so this is a documentation rather
//   than a functional limitation.

class BitmapImageOwner {
public:
    explicit BitmapImageOwner(OmegaCommon::Img::BitmapImage && img) : img_(img) {}
    ~BitmapImageOwner() {
        // OmegaCommon::Img::BitmapImage::data is owned by the codec
        // (PNGCodec/JpegCodec/TiffCodec) and was allocated with `new[]`
        // — the type has no destructor, so the consumer has to free it.
        delete[] img_.data;
        img_.data = nullptr;
    }
    OmegaCommon::Img::BitmapImage &get() { return img_; }
private:
    OmegaCommon::Img::BitmapImage img_;
};

/// Map the BitmapImage onto a 4-channel 8-bit-per-channel RGBA buffer.
/// Returns the new pixel buffer (always width*height*4 bytes) plus the
/// row stride. The OmegaCommon codecs already strip 16-bit PNGs to 8-bit
/// and expand grayscale/palette to RGB, so the only normalization step
/// here is RGB → RGBA padding.
struct NormalizedPixels {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rowBytes = 0;
    bool ok = false;
};

NormalizedPixels normalizeToRGBA8(const OmegaCommon::Img::BitmapImage &img) {
    using OmegaCommon::Img::ColorFormat;
    NormalizedPixels out;
    if (img.data == nullptr || img.header.width == 0 || img.header.height == 0) {
        return out;
    }
    if (img.header.bitDepth != 8) {
        std::cerr << "[GETextureAsset/Vulkan] unsupported bit depth "
                  << img.header.bitDepth << "; expected 8." << std::endl;
        return out;
    }

    out.width    = img.header.width;
    out.height   = img.header.height;
    out.rowBytes = out.width * 4u;
    out.rgba.resize(static_cast<size_t>(out.rowBytes) * out.height);

    const std::uint8_t *src = img.data;
    const size_t srcStride = img.header.stride > 0
        ? img.header.stride
        : static_cast<size_t>(out.width) * static_cast<size_t>(img.header.channels);

    if (img.header.color_format == ColorFormat::RGBA && img.header.channels == 4) {
        // Straight copy, honoring source stride.
        for (std::uint32_t y = 0; y < out.height; ++y) {
            std::memcpy(out.rgba.data() + y * out.rowBytes,
                        src + y * srcStride,
                        out.rowBytes);
        }
    } else if (img.header.color_format == ColorFormat::RGB && img.header.channels == 3) {
        // RGB → RGBA with alpha = 0xFF.
        for (std::uint32_t y = 0; y < out.height; ++y) {
            const std::uint8_t *srow = src + y * srcStride;
            std::uint8_t *drow = out.rgba.data() + y * out.rowBytes;
            for (std::uint32_t x = 0; x < out.width; ++x) {
                drow[x * 4 + 0] = srow[x * 3 + 0];
                drow[x * 4 + 1] = srow[x * 3 + 1];
                drow[x * 4 + 2] = srow[x * 3 + 2];
                drow[x * 4 + 3] = 0xFFu;
            }
        }
    } else {
        std::cerr << "[GETextureAsset/Vulkan] unsupported decoded color format ("
                  << static_cast<int>(img.header.color_format) << ", "
                  << img.header.channels << " channels); only RGB/RGBA are "
                     "handled in this pass." << std::endl;
        return out;
    }

    out.ok = true;
    return out;
}

class GEVulkanTextureAsset : public GETextureAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GETexture> loadedTexture;
    TextureDescriptor loadedDescriptor{};

public:
    explicit GEVulkanTextureAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            std::cerr << "[GETextureAsset/Vulkan] error: no engine bound." << std::endl;
            return false;
        }
        auto *vkEngine = dynamic_cast<GEVulkanEngine *>(engine.get());
        if (!vkEngine) {
            std::cerr << "[GETextureAsset/Vulkan] error: engine is not a Vulkan engine." << std::endl;
            return false;
        }

        OmegaCommon::FS::Path fsPath(path);
        auto decodeRes = OmegaCommon::Img::loadFromFile(fsPath);
        if (decodeRes.isErr()) {
            std::cerr << "[GETextureAsset/Vulkan] failed to decode '" << path
                      << "': " << decodeRes.error() << std::endl;
            return false;
        }
        BitmapImageOwner ownerGuard(std::move(decodeRes.value()));
        OmegaCommon::Img::BitmapImage &img = ownerGuard.get();

        NormalizedPixels px = normalizeToRGBA8(img);
        if (!px.ok) {
            return false;
        }

        // Best-effort mipmap honoring; see the v1-limitations note at the
        // top of the file. Logged once per process so the gap is visible.
        if (options.generateMipmaps) {
            static bool warned = false;
            if (!warned) {
                std::cerr << "[GETextureAsset/Vulkan] note: generateMipmaps=true "
                             "is ignored in this pass; runtime mip generation "
                             "lands with the shared upload-queue helper. "
                             "Mip 0 is uploaded; mipLevels=1." << std::endl;
                warned = true;
            }
        }

        TextureDescriptor texDesc{};
        texDesc.storage_opts = Shared;
        texDesc.usage       = GETexture::ToGPU;
        texDesc.pixelFormat = options.sRGB
            ? TexturePixelFormat::RGBA8Unorm_SRGB
            : TexturePixelFormat::RGBA8Unorm;
        texDesc.width       = px.width;
        texDesc.height      = px.height;
        texDesc.depth       = 1;
        texDesc.mipLevels   = 1;
        texDesc.sampleCount = 1;
        texDesc.kind        = TextureKind::Tex2D;
        texDesc.arrayLayers = 1;

        SharedHandle<GETexture> tex = engine->makeTexture(texDesc);
        if (!tex) {
            std::cerr << "[GETextureAsset/Vulkan] makeTexture failed for "
                      << px.width << "x" << px.height << " RGBA8." << std::endl;
            return false;
        }

        // `ToGPU` paths land on a LINEAR-tiled HOST_VISIBLE image, so
        // copyBytes does the upload directly without a staging buffer.
        tex->copyBytes(px.rgba.data(), px.rowBytes);

        loadedTexture = tex;

        loadedDescriptor = texDesc;
        return true;
    }

    SharedHandle<GETexture> texture() const override { return loadedTexture; }
    TextureDescriptor descriptor() const override { return loadedDescriptor; }

    void release() override {
        loadedTexture.reset();
        loadedDescriptor = TextureDescriptor{};
    }
};

}  // namespace

SharedHandle<GETextureAsset> GETextureAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GETextureAsset>(new GEVulkanTextureAsset(engine));
}

_NAMESPACE_END_
