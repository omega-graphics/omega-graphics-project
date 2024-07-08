#include "GTEBase.h"
#include "GE.h"

#ifndef OMEGAGTE_GETEXTURE_H
#define OMEGAGTE_GETEXTURE_H

_NAMESPACE_BEGIN_

    enum class TexturePixelFormat : int {
        RGBA8Unorm,
        RGBA16Unorm,
        RGBA8Unorm_SRGB
    };

    /**
     * @brief A Buffer that contains texel data arranged in regular rows.*/
    class  OMEGAGTE_EXPORT GETexture : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GETexture")
        typedef enum : unsigned char {
            Texture1D,
            Texture2D,
            Texture3D,
        } GETextureType;
        typedef enum : unsigned char {
            ToGPU = 0x00,
            FromGPU = 0x01,
            GPUAccessOnly = 0x02,
            RenderTarget = 0x03,
            MSResolveSrc = 0x04,
            RenderTargetAndDepthStencil = 0x05
        } GETextureUsage;
    protected:
        GETextureType type;
        GETextureUsage usage;
        TexturePixelFormat pixelFormat;
        bool checkIfCanWrite();
        explicit GETexture(
                const GETextureType & type,
                  const GETextureUsage & usage,
                  const TexturePixelFormat & pixelFormat);
    public:
        /** @brief Upload data to the texture stored on the device from the CPU.
         * @param[in] bytes A pointer to the buffer
         * @param[in] bytesPerRow The bytes per row in the data.
         * @paragraph
         * This function can only be and should only be invoked if the texture has a `ToGPU` usage,
         * and for initial upload of texture data.
        */
        virtual void copyBytes(void *bytes,size_t bytesPerRow) = 0;

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
        GETexture::GETextureType type;
        StorageOpts storage_opts = Shared;
        GETexture::GETextureUsage usage = GETexture::ToGPU;
        TexturePixelFormat pixelFormat = TexturePixelFormat::RGBA8Unorm;
        unsigned width;
        unsigned height;
        unsigned depth = 1;
        unsigned mipLevels = 1;
        unsigned sampleCount = 1;
    };

_NAMESPACE_END_

#endif
