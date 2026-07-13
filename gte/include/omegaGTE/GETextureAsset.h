#include "GTEBase.h"
#include "GETexture.h"
#include "GE.h"

#include <future>
#include <string>

#ifndef OMEGAGTE_GETEXTUREASSET_H
#define OMEGAGTE_GETEXTUREASSET_H

_NAMESPACE_BEGIN_

    /// @brief Loads textures from common image / container formats and
    /// uploads them to the GPU as a `GETexture`. Backend-agnostic public
    /// API; the concrete subclass is selected by `Create()` based on the
    /// linked-in graphics backend.
    ///
    /// Supported formats are backend-specific. The Metal backend
    /// (MetalKit / `MTKTextureLoader`) covers PNG, JPEG, TIFF, BMP, HDR,
    /// KTX, PVR, and ASTC out of the box. D3D12 (DirectXTex) and Vulkan
    /// (libktx + stb_image) implementations land in Phase 3.4.
    class OMEGAGTE_EXPORT GETextureAsset {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GETextureAsset")

        /// @brief Options used during load; backends honor what they can.
        struct OMEGAGTE_EXPORT LoadOptions {
            /// Generate mipmaps after upload.
            bool generateMipmaps = true;
            /// Treat the source as sRGB-encoded color data (typical for
            /// 8-bit color textures); affects the chosen pixel format.
            bool sRGB = true;
        };

        /// @brief Synchronously load a texture file from disk and upload
        /// it to the GPU. On success, `texture()` returns a non-null
        /// handle. Returns `false` and logs on failure.
        virtual bool load(const std::string & path,
                          const LoadOptions & options) = 0;

        /// @brief Convenience overload using default `LoadOptions`.
        bool load(const std::string & path);

        /// @brief Asynchronous variant. Default implementation runs
        /// `load` on `std::async(std::launch::async, ...)`.
        virtual std::future<bool> loadAsync(const std::string & path,
                                            const LoadOptions & options);

        /// @brief Convenience async overload using default `LoadOptions`.
        std::future<bool> loadAsync(const std::string & path);

        /// @brief Get the loaded texture, or null if load was not called
        /// or failed.
        OMEGA_NODISCARD virtual SharedHandle<GETexture> texture() const = 0;

        /// @brief Describe the loaded texture (dimensions, format, mips).
        /// Fields are zero / default when no texture is loaded.
        OMEGA_NODISCARD virtual TextureDescriptor descriptor() const = 0;

        /// @brief Free GPU and CPU resources held by this asset early.
        /// Optional — destruction handles release automatically.
        virtual void release() = 0;

        /// @brief Construct an instance bound to the given graphics
        /// engine. Backend (Metal / D3D12 / Vulkan) is chosen from the
        /// engine subtype at runtime.
        static SharedHandle<GETextureAsset> Create(SharedHandle<OmegaGraphicsEngine> & engine);

        virtual ~GETextureAsset() = default;
    };

_NAMESPACE_END_

#endif
