// Process-wide cache mapping `Media::BitmapImage` instances to their uploaded
// `GETexture` (with mip chain when the source dimensions warrant it).
//
// Phase 6.6.1 lifted the `BitmapImage -> GETexture` upload out of
// `BackendRenderTargetContext::renderToTarget`. Two reasons:
//
//   1. Mipmap generation requires its own blit pass on a command buffer;
//      we cannot record a blit pass while a frame's render pass is open.
//   2. The pre-Phase-6.6 path re-uploaded the same image every frame it
//      was drawn — a measurable cost for any UI that decorates with
//      static icons / sprite atlases.
//
// The cache is keyed by raw `BitmapImage *` (the renderer side keeps the
// `SharedPtr<BitmapImage>` alive through the frame). Entries are dropped
// lazily when the underlying image is freed and the cache observes the
// dangling pointer at acquisition time (we keep `weak_ptr<BitmapImage>`
// alongside the texture so we can detect this).

#ifndef OMEGAWTK_COMPOSITION_BACKEND_BITMAPTEXTURECACHE_H
#define OMEGAWTK_COMPOSITION_BACKEND_BITMAPTEXTURECACHE_H

#include "omegaWTK/Core/GTEHandle.h"
#include "omegaWTK/Media/ImgCodec.h"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace OmegaWTK::Composition {

    /// Process-wide bitmap texture cache. Singleton accessed via `instance()`.
    class BitmapTextureCache {
    public:
        struct Entry {
            SharedHandle<OmegaGTE::GETexture> texture;
            std::weak_ptr<Media::BitmapImage> sentinel;
            unsigned width = 0;
            unsigned height = 0;
            bool hasMips = false;
        };

        /// Acquire the GPU texture for `image`, uploading + generating mipmaps
        /// on first use. Subsequent calls with the same `image` return the
        /// cached texture. Returns nullptr when the upload failed.
        SharedHandle<OmegaGTE::GETexture> acquire(Core::SharedPtr<Media::BitmapImage> image);

        /// Drop every cached entry whose `BitmapImage` has been destroyed.
        /// Safe to call from any thread.
        void purgeDead();

        /// Drop every cached entry. Used by `BackendResourceFactory::shutdownPools`.
        void clear();

        static BitmapTextureCache & instance();

    private:
        BitmapTextureCache() = default;

        std::mutex mutex_;
        std::unordered_map<Media::BitmapImage *, Entry> entries_;
    };

}

#endif
