#include "GTEBase.h"
#include "GE.h"

#include <cstdint>

#ifndef OMEGAGTE_GETEXTURE_H
#define OMEGAGTE_GETEXTURE_H

_NAMESPACE_BEGIN_

    using TexturePixelFormat = PixelFormat;

    /// @brief Pipeline-Completion-Extension-Plan §6.1 — names the shape of a
    /// `GETexture` precisely enough to drive the right native view dimension
    /// and to validate against the OmegaSL layout-desc the shader was
    /// compiled with. `Auto` is a back-compat sentinel: when a caller leaves
    /// `kind` at its default the backend derives the kind from the legacy
    /// `type` + `sampleCount` fields (1D / 2D / 2DMS / 3D).
    enum class TextureKind : uint8_t {
        Auto = 0,
        Tex1D,
        Tex2D,
        Tex3D,
        Tex1DArray,
        Tex2DArray,
        TexCube,
        TexCubeArray,
        Tex2DMS,
        Tex2DMSArray,
    };

    /**
     * @brief A Buffer that contains texel data arranged in regular rows.*/
    class  OMEGAGTE_EXPORT GETexture : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GETexture")
        using GETextureUsage = enum : unsigned char {
            ToGPU = 0x00,
            FromGPU = 0x01,
            GPUAccessOnly = 0x02,
            RenderTarget = 0x03,
            MSResolveSrc = 0x04,
            RenderTargetAndDepthStencil = 0x05
        };
    protected:
        GETextureUsage usage;
        TexturePixelFormat pixelFormat;
        TextureKind kind = TextureKind::Tex2D;
        unsigned arrayLayers = 1;
        unsigned sampleCount = 1;
        bool checkIfCanWrite();
        explicit GETexture(
                  const TextureKind & kind,
                  const GETextureUsage & usage,
                  const TexturePixelFormat & pixelFormat);
    public:
        /// @brief Effective shape of this texture. Phase B view-dimension
        /// pickers and bind-time validators consult this so cube / array
        /// / MS textures bind with the right view dimension.
        TextureKind getKind() const { return kind; }
        unsigned getArrayLayers() const { return arrayLayers; }
        unsigned getSampleCount() const { return sampleCount; }
        /// @brief Backend-only: record the effective shape after the
        /// native resource has been built. Public so that backend
        /// `makeTexture` paths (which sit outside the friend boundary)
        /// can populate the fields without touching `protected:` state.
        void setShape(TextureKind k, unsigned layers, unsigned samples){
            kind = k;
            arrayLayers = layers;
            sampleCount = samples;
        }
        /** @brief Upload data to the texture stored on the device from the CPU.
         * @param[in] bytes A pointer to the buffer
         * @param[in] bytesPerRow The bytes per row in the data.
         * @paragraph
         * This function can only be and should only be invoked if the texture has a `ToGPU` usage,
         * and for initial upload of texture data.
        */
        virtual void copyBytes(void *bytes,size_t bytesPerRow) = 0;

        /** @brief Upload data to a sub-region of the texture from a CPU buffer.
         * @param[in] bytes Pointer to the source buffer. Must point to at least
         *        `bytesPerRow * destRegion.h * max(destRegion.d, 1)` bytes
         *        starting at the top-left of the region's source data. To read
         *        from a sub-area of a larger source bitmap, the caller advances
         *        the pointer to the desired row/column before calling.
         * @param[in] bytesPerRow Bytes per row in the source buffer.
         * @param[in] destRegion Sub-region of the texture (mip 0) to overwrite.
         *        `{x, y, z}` is the destination origin; `{w, h, d}` is the
         *        extent. For 2D textures `z = 0` and `d = 1`.
         * @paragraph
         * Pipeline-Completion-Extension-Plan §4.5. Only valid for textures
         * created with `ToGPU` usage. The single-arg `copyBytes` overload
         * remains as a convenience that targets the full mip 0 extent.
        */
        virtual void copyBytes(void *bytes,
                               size_t bytesPerRow,
                               const TextureRegion &destRegion) = 0;

        /** @brief Download data from the texture stored on the device to the CPU.
         * @param[in,out] bytes A pointer to the buffer to receive the data. (Can be nullptr when querying data size)
         * @param[out] bytesPerRow The bytes per row in the data.
         * @returns The size of the buffer
         * @paragraph
         * This function can only be called if the texture has a `FromGPU` usage.
        */
        virtual size_t getBytes(void *bytes,size_t bytesPerRow) = 0;
        virtual ~GETexture() = default;
    };


    struct  OMEGAGTE_EXPORT TextureDescriptor {
        StorageOpts storage_opts = Shared;
        GETexture::GETextureUsage usage = GETexture::ToGPU;
        TexturePixelFormat pixelFormat = TexturePixelFormat::RGBA8Unorm;
        unsigned width;
        unsigned height;
        unsigned depth = 1;
        unsigned mipLevels = 1;
        unsigned sampleCount = 1;
        /// @brief Texture shape. Drives view-dimension picks (1D/2D/3D,
        /// arrays, cubes, MS) in every backend's `makeTexture`. `Auto` is
        /// not a valid descriptor input — backends treat it as `Tex2D`
        /// for safety, but new code should always set this explicitly.
        TextureKind kind = TextureKind::Tex2D;
        /// @brief Layer count for array kinds. For `TexCube` the engine
        /// fixes this at 6; for `TexCubeArray` it must be a multiple of 6;
        /// for `Tex1DArray` / `Tex2DArray` / `Tex2DMSArray` it is the
        /// number of array layers. Ignored for non-array kinds.
        unsigned arrayLayers = 1;
        /// @brief Default channel swizzle baked into the texture's primary
        /// view at creation time. When non-identity, the backend allocates
        /// the primary `VkImageView` / `MTLTexture` view / SRV with this
        /// channel mapping, so every bind without a runtime override sees
        /// the swizzled channels for free. A runtime override passed to
        /// `bindResourceAt*` always takes precedence over this default.
        TextureSwizzle defaultSwizzle = TextureSwizzle::identity();
    };

    /// @brief Map an OmegaSL layout-desc texture type to the `TextureKind`
    /// the shader expects to be bound. Returns `TextureKind::Auto` for any
    /// non-texture layout-desc type (buffer, sampler, constant). Phase B
    /// bind paths use the result to validate `texture->getKind()` against
    /// the shader's compiled expectation. Intentionally lives in this
    /// header (not the backends) so D3D12 / Metal / Vulkan all share one
    /// canonical mapping.
    inline TextureKind textureKindForOmegaSLLayoutType(int layoutDescType){
        // Hard-coded constants mirror the values in `omegasl.h` so this
        // header avoids pulling in the OmegaSL dependency. The order
        // matches the enum declaration site; a static_assert in the
        // backend translation units pins the contract.
        switch(layoutDescType){
            case 2:  return TextureKind::Tex1D;          // OMEGASL_SHADER_TEXTURE1D_DESC
            case 3:  return TextureKind::Tex2D;          // OMEGASL_SHADER_TEXTURE2D_DESC
            case 4:  return TextureKind::Tex3D;          // OMEGASL_SHADER_TEXTURE3D_DESC
            case 11: return TextureKind::Tex1DArray;     // OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC
            case 12: return TextureKind::Tex2DArray;     // OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC
            case 13: return TextureKind::TexCube;        // OMEGASL_SHADER_TEXTURECUBE_DESC
            case 14: return TextureKind::TexCubeArray;   // OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC
            case 15: return TextureKind::Tex2DMS;        // OMEGASL_SHADER_TEXTURE2D_MS_DESC
            case 16: return TextureKind::Tex2DMSArray;   // OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC
            default: return TextureKind::Auto;
        }
    }

    inline const char * textureKindName(TextureKind k){
        switch(k){
            case TextureKind::Auto:         return "Auto";
            case TextureKind::Tex1D:        return "Tex1D";
            case TextureKind::Tex2D:        return "Tex2D";
            case TextureKind::Tex3D:        return "Tex3D";
            case TextureKind::Tex1DArray:   return "Tex1DArray";
            case TextureKind::Tex2DArray:   return "Tex2DArray";
            case TextureKind::TexCube:      return "TexCube";
            case TextureKind::TexCubeArray: return "TexCubeArray";
            case TextureKind::Tex2DMS:      return "Tex2DMS";
            case TextureKind::Tex2DMSArray: return "Tex2DMSArray";
        }
        return "<unknown>";
    }

    /// @brief §6.3 — at bind time, verify the bound texture's
    /// `TextureKind` matches the shader's expected layout-desc texture
    /// type. Writes a diagnostic to @c stderr and returns @c false on
    /// mismatch so the caller can skip the bind. The expected kind is
    /// `Auto` for non-texture layout-desc types; in that case we have
    /// no expectation and accept whatever the caller passes.
    inline bool validateTextureBindKind(int layoutDescType,
                                        TextureKind boundKind,
                                        unsigned boundSampleCount,
                                        const char *shaderName,
                                        unsigned bindingLocation){
        const TextureKind expected = textureKindForOmegaSLLayoutType(layoutDescType);
        if(expected == TextureKind::Auto){
            return true;
        }
        if(expected != boundKind){
            std::cerr << "[OmegaGTE] Texture bind kind mismatch: shader '"
                      << (shaderName ? shaderName : "<anon>")
                      << "' binding " << bindingLocation
                      << " expects " << textureKindName(expected)
                      << " but bound texture is " << textureKindName(boundKind)
                      << std::endl;
            return false;
        }
        // §6.4 — for MS variants, require sample_count > 1 on the bound
        // texture. The exact value cannot be cross-checked yet (the
        // OmegaSL layout-desc does not carry sample_count); that is the
        // remaining 6.4 task once OmegaSL grows the field.
        const bool isMS = (expected == TextureKind::Tex2DMS ||
                           expected == TextureKind::Tex2DMSArray);
        if(isMS && boundSampleCount <= 1){
            std::cerr << "[OmegaGTE] Texture bind sample-count mismatch: shader '"
                      << (shaderName ? shaderName : "<anon>")
                      << "' binding " << bindingLocation
                      << " expects multisampled texture but bound texture has sample_count="
                      << boundSampleCount << std::endl;
            return false;
        }
        return true;
    }

_NAMESPACE_END_

#endif
