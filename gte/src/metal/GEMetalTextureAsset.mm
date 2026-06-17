#include "omegaGTE/GETextureAsset.h"
#include "GEMetal.h"
#include "GEMetalTexture.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <Foundation/Foundation.h>

#include <iostream>

_NAMESPACE_BEGIN_

namespace {

/// Map an `MTLPixelFormat` produced by `MTKTextureLoader` back to the
/// engine-side `TexturePixelFormat`. We only enumerate the formats the
/// engine knows about; anything else is reported as `RGBA8Unorm` and a
/// warning is logged so the caller can see the format was lossy.
TexturePixelFormat mapMetalPixelFormat(MTLPixelFormat fmt) {
    switch (fmt) {
        case MTLPixelFormatRGBA8Unorm:      return TexturePixelFormat::RGBA8Unorm;
        case MTLPixelFormatRGBA8Unorm_sRGB: return TexturePixelFormat::RGBA8Unorm_SRGB;
        case MTLPixelFormatBGRA8Unorm:      return TexturePixelFormat::BGRA8Unorm;
        case MTLPixelFormatBGRA8Unorm_sRGB: return TexturePixelFormat::BGRA8Unorm_SRGB;
        case MTLPixelFormatRGBA16Unorm:     return TexturePixelFormat::RGBA16Unorm;
        default:
            DEBUG_INFO(DEBUG_DOMAIN_ASSET, "MTLPixelFormat " << (int)fmt << " not modeled in TexturePixelFormat; reporting RGBA8Unorm.");
            return TexturePixelFormat::RGBA8Unorm;
    }
}

class GEMetalTextureAsset : public GETextureAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GETexture> loadedTexture;
    TextureDescriptor loadedDescriptor{};

public:
    explicit GEMetalTextureAsset(SharedHandle<OmegaGraphicsEngine> & e)
        : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "Texture asset load: no engine bound");
            return false;
        }
        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)engine->underlyingNativeDevice();
            if (device == nil) {
                DEBUG_ERROR(DEBUG_DOMAIN_ASSET, "Texture asset load: native device is nil");
                return false;
            }

            MTKTextureLoader *loader = [[MTKTextureLoader alloc] initWithDevice:device];

            NSString *nsPath = [[NSString alloc] initWithUTF8String:path.c_str()];
            NSURL *url = [NSURL fileURLWithPath:nsPath];

            NSMutableDictionary<MTKTextureLoaderOption, id> *opts = [NSMutableDictionary dictionary];
            opts[MTKTextureLoaderOptionGenerateMipmaps] = @(options.generateMipmaps);
            opts[MTKTextureLoaderOptionSRGB] = @(options.sRGB);
            opts[MTKTextureLoaderOptionTextureUsage] = @(MTLTextureUsageShaderRead);
            opts[MTKTextureLoaderOptionTextureStorageMode] = @(MTLStorageModePrivate);

            NSError *error = nil;
            id<MTLTexture> mtlTex = [loader newTextureWithContentsOfURL:url
                                                                options:opts
                                                                  error:&error];
            if (mtlTex == nil || error != nil) {
                DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "Texture asset load failed: path=" << path << " error=" << (error != nil ? error.localizedDescription.UTF8String : "unknown"));
                return false;
            }

            // Determine TextureKind from the MTLTextureType.
            TextureKind kind = TextureKind::Tex2D;
            switch (mtlTex.textureType) {
                case MTLTextureType1D:                kind = TextureKind::Tex1D; break;
                case MTLTextureType1DArray:           kind = TextureKind::Tex1DArray; break;
                case MTLTextureType2D:                kind = TextureKind::Tex2D; break;
                case MTLTextureType2DArray:           kind = TextureKind::Tex2DArray; break;
                case MTLTextureType2DMultisample:     kind = TextureKind::Tex2DMS; break;
                case MTLTextureTypeCube:              kind = TextureKind::TexCube; break;
                case MTLTextureTypeCubeArray:         kind = TextureKind::TexCubeArray; break;
                case MTLTextureType3D:                kind = TextureKind::Tex3D; break;
                default:                              kind = TextureKind::Tex2D; break;
            }

            TexturePixelFormat fmt = mapMetalPixelFormat(mtlTex.pixelFormat);

            NSSmartPtr texPtr = NSObjectHandle{NSOBJECT_CPP_BRIDGE mtlTex};
            loadedTexture = SharedHandle<GETexture>(
                new GEMetalTexture(kind, GETexture::ToGPU, fmt, texPtr));
            loadedTexture->setShape(kind,
                                    static_cast<unsigned>(mtlTex.arrayLength),
                                    static_cast<unsigned>(mtlTex.sampleCount));

            loadedDescriptor = TextureDescriptor{};
            loadedDescriptor.usage = GETexture::ToGPU;
            loadedDescriptor.pixelFormat = fmt;
            loadedDescriptor.width  = static_cast<unsigned>(mtlTex.width);
            loadedDescriptor.height = static_cast<unsigned>(mtlTex.height);
            loadedDescriptor.depth  = static_cast<unsigned>(mtlTex.depth);
            loadedDescriptor.mipLevels = static_cast<unsigned>(mtlTex.mipmapLevelCount);
            loadedDescriptor.sampleCount = static_cast<unsigned>(mtlTex.sampleCount);
            loadedDescriptor.kind = kind;
            loadedDescriptor.arrayLayers = static_cast<unsigned>(mtlTex.arrayLength);

            DEBUG_INFO(DEBUG_DOMAIN_ASSET, "Texture asset loaded: path=" << path << " " << mtlTex.width << "x" << mtlTex.height);
        }
        return true;
    }

    SharedHandle<GETexture> texture() const override {
        return loadedTexture;
    }

    TextureDescriptor descriptor() const override {
        return loadedDescriptor;
    }

    void release() override {
        loadedTexture.reset();
        loadedDescriptor = TextureDescriptor{};
    }
};

}  // namespace

SharedHandle<GETextureAsset> GETextureAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GETextureAsset>(new GEMetalTextureAsset(engine));
}

_NAMESPACE_END_
