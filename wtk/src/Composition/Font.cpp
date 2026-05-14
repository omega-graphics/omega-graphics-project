// Out-of-line definitions for `Font` (Phase 6.7.1).
//
// `Font` holds a `unique_ptr<GlyphAtlas>`, but `GlyphAtlas` is forward-
// declared in the public header to keep backend internals out of public
// WTK includes. Defining the ctor / dtor / accessors here gives the
// compiler a complete view of `GlyphAtlas` at the point where the
// unique_ptr is constructed and destroyed.

#include "omegaWTK/Composition/FontEngine.h"
#include "backend/GlyphAtlas.h"

namespace OmegaWTK::Composition {

    Font::Font(FontDescriptor & desc)
        : desc(desc),
          mode_(Mode::BitmapFallback),
          atlas_(std::make_unique<GlyphAtlas>(nullptr)) {
        // Default construction installs a no-rasterize atlas so
        // `ensureGlyph` always returns false. Backends that successfully
        // probe their face for vector outlines (Phase 6.7.4) can swap
        // in a real RasterizeFn-bearing atlas and call setMode(MSDF)
        // before the Font is handed back to the caller.
    }

    Font::~Font() = default;

    GlyphAtlas & Font::atlas() {
        return *atlas_;
    }

    Font::Mode Font::mode() const {
        return mode_;
    }

    void Font::setMode(Mode m) {
        mode_ = m;
    }

    void Font::ensureGlyphsResident(const OmegaCommon::Vector<std::uint32_t> & glyphIds) {
        for(std::uint32_t gid : glyphIds) {
            atlas_->ensureGlyph(gid);
        }
    }

}
