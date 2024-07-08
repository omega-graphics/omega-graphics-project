#include "omegaGTE/GETexture.h"

_NAMESPACE_BEGIN_

GETexture::GETexture(const GETextureType & type,
                     const GETextureUsage & usage,
                     const TexturePixelFormat & pixelFormat):type(type),usage(usage),pixelFormat(pixelFormat){

}

bool GETexture::checkIfCanWrite() {
    assert(usage == ToGPU && "Cannot write to GETexture unless it has a `ToGPU` usage.");
    return true;
}

_NAMESPACE_END_