// Phase G.1 — TessellationCache path-walk hash.
//
// `hashPath2D` lives out of line because the underlying
// `GVectorPath_Base<GPoint2D>::transformEachPoint` template is only
// defined in `omegaGTE/GTEBase.h`. Pulling that whole header into
// `TessellationCache.h` would force every consumer of the cache key
// (RenderTarget.h, FrameBuilder, …) to compile the full GTE math API.
// The signature stays in the header; the body lives here.

#include "TessellationCache.h"

#include <omegaGTE/GTEBase.h>

namespace OmegaWTK::Composition {

    std::pair<std::uint64_t, std::uint32_t>
    hashPath2D(OmegaGTE::GVectorPath2D & path){
        constexpr std::uint64_t FnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t FnvPrime  = 1099511628211ULL;
        std::uint64_t h = FnvOffset;
        std::uint32_t n = 0;
        path.transformEachPoint([&](OmegaGTE::GPoint2D & p){
            std::uint32_t xb = 0;
            std::uint32_t yb = 0;
            std::memcpy(&xb, &p.x, sizeof(xb));
            std::memcpy(&yb, &p.y, sizeof(yb));
            for(int i = 0; i < 4; ++i){
                h ^= static_cast<std::uint8_t>((xb >> (i * 8)) & 0xFFu);
                h *= FnvPrime;
            }
            for(int i = 0; i < 4; ++i){
                h ^= static_cast<std::uint8_t>((yb >> (i * 8)) & 0xFFu);
                h *= FnvPrime;
            }
            ++n;
        });
        return {h, n};
    }

}
