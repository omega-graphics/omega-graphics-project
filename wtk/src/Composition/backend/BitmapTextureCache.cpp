#include "BitmapTextureCache.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Composition {

    namespace {
        /// Below this dimension threshold mipmaps are not worth the +33%
        /// memory cost — minification at typical UI scales doesn't bite for
        /// small icons. Tunable; documented in
        /// `Direct-To-Drawable-And-SDF-Plan.md` §6.6.1 and the Risks list.
        constexpr unsigned kMipmapDimensionThreshold = 64;

        unsigned computeMipLevels(unsigned width, unsigned height){
            const unsigned maxDim = std::max(width, height);
            if(maxDim <= 1) return 1;
            unsigned levels = 1;
            unsigned d = maxDim;
            while(d > 1){
                d >>= 1u;
                ++levels;
            }
            return levels;
        }

        /// Runs `generateMipmaps` on a one-shot blit command buffer. Synchronous —
        /// blocks until the GPU is done so the caller can hand the texture to
        /// the renderer immediately. Acceptable here because uploads happen
        /// at `Canvas::drawImage` recording time, not in the per-frame draw
        /// loop. Returns false on submission failure.
        bool runGenerateMipmaps(SharedHandle<OmegaGTE::GETexture> & texture){
            auto queue = gte.graphicsEngine->makeCommandQueue(1);
            if(queue == nullptr) return false;
            auto cb = queue->getAvailableBuffer();
            if(cb == nullptr) return false;
            cb->startBlitPass();
            cb->generateMipmaps(texture);
            cb->finishBlitPass();
            queue->submitCommandBuffer(cb);
            queue->commitToGPUAndWait();
            return true;
        }
    }

    BitmapTextureCache & BitmapTextureCache::instance(){
        static BitmapTextureCache cache;
        return cache;
    }

    SharedHandle<OmegaGTE::GETexture>
    BitmapTextureCache::acquire(SharedHandle<OmegaCommon::Img::BitmapImage> image){
        if(image == nullptr || image->empty()){
            return nullptr;
        }
        OmegaCommon::Img::BitmapImage *key = image.get();

        std::lock_guard<std::mutex> guard(mutex_);

        auto it = entries_.find(key);
        if(it != entries_.end()){
            // The raw-pointer key is only stable while at least one
            // SharedPtr to the image survives; the weak-ptr sentinel
            // detects the rare collision where a freed BitmapImage's
            // address was reused for a fresh allocation.
            if(!it->second.sentinel.expired()){
                return it->second.texture;
            }
            entries_.erase(it);
        }

        const unsigned w = image->header.width;
        const unsigned h = image->header.height;
        if(w == 0 || h == 0){
            return nullptr;
        }

        const bool wantMips = (w >= kMipmapDimensionThreshold &&
                               h >= kMipmapDimensionThreshold);
        const unsigned mipLevels = wantMips ? computeMipLevels(w, h) : 1u;

        OmegaGTE::TextureDescriptor desc {};
        desc.usage     = OmegaGTE::GETexture::ToGPU;
        desc.width     = w;
        desc.height    = h;
        desc.mipLevels = mipLevels;
        // Pixel format / channel order: today we always allocate
        // RGBA8Unorm and pass the decoder's bytes through `copyBytes`
        // directly. Plan §6.6 calls out the swizzle question — when
        // platform decoders deliver BGRA the shader will need a
        // `.bgra` swizzle (or a CPU repack at upload time). Live
        // testing on each backend will surface the mismatch; until
        // then the existing convention is preserved.

        auto texture = gte.graphicsEngine->makeTexture(desc);
        if(texture == nullptr){
            return nullptr;
        }
        // WTK and GTE share a top-left / y-down convention, so the
        // decoded bitmap rows can be uploaded directly. No per-row Y flip.
        texture->copyBytes(image->data(), image->header.stride);

        if(wantMips){
            if(!runGenerateMipmaps(texture)){
                // Mip generation failed — keep the base level only. The
                // sampler still produces correct (linear) results without
                // the chain; callers just lose minification quality.
#ifdef OMEGAWTK_TRACE_RENDER
                std::cout << "BitmapTextureCache: generateMipmaps failed for "
                          << w << "x" << h << " texture; falling back to base level only."
                          << std::endl;
#endif
            }
        }

        Entry entry;
        entry.texture  = texture;
        entry.sentinel = image;
        entry.width    = w;
        entry.height   = h;
        entry.hasMips  = wantMips;
        entries_.emplace(key, std::move(entry));
        return texture;
    }

    void BitmapTextureCache::purgeDead(){
        std::lock_guard<std::mutex> guard(mutex_);
        for(auto it = entries_.begin(); it != entries_.end();){
            if(it->second.sentinel.expired()){
                it = entries_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void BitmapTextureCache::clear(){
        std::lock_guard<std::mutex> guard(mutex_);
        entries_.clear();
    }

}
