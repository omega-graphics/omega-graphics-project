#include "omegaGTE/GETexture.h"

_NAMESPACE_BEGIN_

GETexture::GETexture(const TextureKind & kind,
                     const GETextureUsage & usage,
                     const TexturePixelFormat & pixelFormat):usage(usage),pixelFormat(pixelFormat),kind(kind){

}

bool GETexture::checkIfCanWrite() {
    assert(usage == ToGPU && "Cannot write to GETexture unless it has a `ToGPU` usage.");
    return true;
}

_NAMESPACE_END_