#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "NativePrivate/macos/CocoaUtils.h"
#include "../GlyphAtlas.h"

#include "omega-common/unicode.h"
#include "omega-common/assets.h"

#import <CoreText/CoreText.h>
#include <memory>
#include <cstring>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#ifdef OMEGAWTK_HAVE_MSDFGEN
#include <msdfgen.h>
#include <core/edge-coloring.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#endif

namespace OmegaWTK::Composition {

static CGFloat currentScreenScale(){
    CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
    if(scale <= 0.f){
        scale = 1.f;
    }
    return scale;
}

namespace {
    bool textTraceEnabled() {
        static const bool enabled = []() {
            auto e = OmegaCommon::getEnvVar("OMEGAWTK_TRACE_TEXT");
            return e.has_value() && !e->empty() && (*e)[0] != '0';
        }();
        return enabled;
    }

#ifdef OMEGAWTK_HAVE_MSDFGEN
    /// MSDF glyph tile size (Phase 6.7.1). Square 32×32 cells, 4 px
    /// distance range — identical to the Linux backend so the shared
    /// atlas / shader contract holds across platforms.
    constexpr int kMsdfTileSize = 32;
    constexpr double kMsdfRange = 4.0;

    /// `CGPathApply` callback context: builds a msdfgen `Shape` one
    /// contour at a time. Core Text glyph paths are always closed (each
    /// subpath ends with `CloseSubpath`), so the close handler stitches
    /// the contour shut explicitly — unlike FT outlines, which are
    /// implicitly closed by `FT_Outline_Decompose`.
    struct CGPathMsdfContext {
        msdfgen::Shape *shape = nullptr;
        msdfgen::Contour *contour = nullptr;
        msdfgen::Point2 lastPoint {0.0, 0.0};
        msdfgen::Point2 contourStart {0.0, 0.0};
    };

    msdfgen::Point2 toMsdfPoint(const CGPoint &p){
        return msdfgen::Point2(static_cast<double>(p.x), static_cast<double>(p.y));
    }

    void cgPathApplier(void *info, const CGPathElement *element){
        auto *ctx = static_cast<CGPathMsdfContext *>(info);
        switch(element->type){
            case kCGPathElementMoveToPoint: {
                ctx->contour = &ctx->shape->addContour();
                ctx->lastPoint = toMsdfPoint(element->points[0]);
                ctx->contourStart = ctx->lastPoint;
                break;
            }
            case kCGPathElementAddLineToPoint: {
                if(ctx->contour == nullptr) break;
                msdfgen::Point2 endpoint = toMsdfPoint(element->points[0]);
                if(endpoint != ctx->lastPoint){
                    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, endpoint));
                    ctx->lastPoint = endpoint;
                }
                break;
            }
            case kCGPathElementAddQuadCurveToPoint: {
                if(ctx->contour == nullptr) break;
                msdfgen::Point2 ctrl = toMsdfPoint(element->points[0]);
                msdfgen::Point2 endpoint = toMsdfPoint(element->points[1]);
                ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, ctrl, endpoint));
                ctx->lastPoint = endpoint;
                break;
            }
            case kCGPathElementAddCurveToPoint: {
                if(ctx->contour == nullptr) break;
                msdfgen::Point2 ctrl1 = toMsdfPoint(element->points[0]);
                msdfgen::Point2 ctrl2 = toMsdfPoint(element->points[1]);
                msdfgen::Point2 endpoint = toMsdfPoint(element->points[2]);
                ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, ctrl1, ctrl2, endpoint));
                ctx->lastPoint = endpoint;
                break;
            }
            case kCGPathElementCloseSubpath: {
                if(ctx->contour == nullptr) break;
                if(ctx->lastPoint != ctx->contourStart){
                    ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, ctx->contourStart));
                    ctx->lastPoint = ctx->contourStart;
                }
                break;
            }
        }
    }
#endif // OMEGAWTK_HAVE_MSDFGEN
}




 class CoreTextFont : public Font {
     CTFontRef native;
     /// Resolution-independent copy of the face at the *unscaled* design
     /// size (`desc.size`, no backing-scale factor baked in). The MSDF
     /// rasterizer extracts outlines from this so atlas tiles stay
     /// resolution-independent (mirrors the Linux backend sizing the
     /// FT face with `FT_Set_Pixel_Sizes(0, descSize)`); `shape()` also
     /// lays out against it. `native` keeps the scaled size for the
     /// legacy bitmap path.
     CTFontRef unscaled;
     friend class CTTextRect;
     friend class CTGlyphRun;
 public:
     CoreTextFont(FontDescriptor & desc,CTFontRef ref,CTFontRef unscaledRef):
        Font(desc),native(ref),unscaled(unscaledRef){};
     void * getNativeFont(){
         return (void *)native;
     };
     CTFontRef getUnscaledFont() const { return unscaled; }
     // Expose the protected mode setter to the engine factory so it can
     // promote the font to MSDF after the outline probe.
     using Font::setMode;

     // Text-Layout-Engine-Plan.md Phase 5: per-font metrics for the
     // WTK-owned layout engine. Sourced from the *unscaled* CTFontRef
     // so the values land in the same logical-pixel space the MSDF
     // atlas uses (the rasterize lambda walks outlines from the same
     // unscaled face). Core Text returns ascent/descent as *positive*
     // values in points, which equal pixels at 1× DPR — matching the
     // FontMetrics contract on the Linux side.
     FontMetrics getMetrics() const override {
         FontMetrics m;
         CTFontRef src = (unscaled != nullptr) ? unscaled : native;
         if(src == nullptr){
             return m;
         }
         m.ascent  = static_cast<float>(CTFontGetAscent(src));
         m.descent = static_cast<float>(CTFontGetDescent(src));
         m.lineGap = static_cast<float>(CTFontGetLeading(src));
         if(m.lineGap < 0.f) m.lineGap = 0.f;
         return m;
     }
     ~CoreTextFont() override {
         if(native != nullptr){
             CFRelease(native);
         }
         if(unscaled != nullptr){
             CFRelease(unscaled);
         }
     };
 };


  FontEngine * FontEngine::instance;

// Text-Layout-Engine-Plan.md Phase 5 — Core Text-backed `ITextShaper`.
//
// Shapes one logical run at a time using a one-line `CTLine` built from a
// per-run attributed string. Crucially we do *not* drive `CTFramesetter`
// (which would impose its own layout / wrap / vertical-alignment
// decisions, the very thing Phase 5 is moving out of platform hands).
// Core Text's shaper still produces kerning + ligatures + mark
// positioning; the layout engine owns line composition, baseline
// placement, and alignment.
//
// Output contract: one `ShaperGlyph` per output glyph (already
// post-kerning / -ligature), in *visual* order. `advance` is the per-
// glyph advance from `CTRunGetAdvances`; `xOffset / yOffset` are mark-
// positioning deltas computed from `CTRunGetPositions` — the difference
// between Core Text's chosen position and the position the layout
// engine would derive from cumulative advances alone. This matches
// HarfBuzz's `x_offset / y_offset` semantics so the WTK layout engine
// applies them the same way on both platforms.
class CoreTextShaper : public ITextShaper {
public:
    OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override {
        OmegaCommon::Vector<ShaperGlyph> out;
        if(input.font == nullptr || input.text.length() == 0){
            return out;
        }
        auto font = std::dynamic_pointer_cast<CoreTextFont>(input.font);
        if(font == nullptr){
            return out;
        }
        CTFontRef ctFont = font->getUnscaledFont();
        if(ctFont == nullptr){
            return out;
        }

        // Build a CFAttributedString carrying just the run text + font.
        // We pass UTF-16 directly via `CFStringCreateWithCharacters` —
        // UniString's internal buffer is already UTF-16, no conversion.
        CFStringRef text = CFStringCreateWithCharacters(
            kCFAllocatorDefault,
            reinterpret_cast<const UniChar *>(input.text.getBuffer()),
            static_cast<CFIndex>(input.text.length()));
        if(text == nullptr){
            return out;
        }
        CFMutableAttributedStringRef attr =
            CFAttributedStringCreateMutable(kCFAllocatorDefault, 0);
        if(attr == nullptr){
            CFRelease(text);
            return out;
        }
        CFAttributedStringReplaceString(attr, CFRangeMake(0, 0), text);
        const CFRange fullRange = CFRangeMake(0, CFAttributedStringGetLength(attr));
        CFAttributedStringSetAttribute(attr, fullRange, kCTFontAttributeName, ctFont);

        // Direction: build the typesetter with a *forced* embedding
        // level so Core Text doesn't second-guess the bidi pass the
        // layout engine already ran. Even-level = LTR (0), odd-level =
        // RTL (1). Without this option Core Text would re-derive the
        // direction from the run content, which mis-fires on an
        // RTL run that contains a single neutral character (e.g. a
        // trailing punctuation).
        CFDictionaryRef typesetterOpts = nullptr;
        SInt8 level = input.rightToLeft ? 1 : 0;
        CFNumberRef levelNum = CFNumberCreate(kCFAllocatorDefault,
                                              kCFNumberSInt8Type, &level);
        if(levelNum != nullptr){
            const void *keys[1]   = { kCTTypesetterOptionForcedEmbeddingLevel };
            const void *values[1] = { levelNum };
            typesetterOpts = CFDictionaryCreate(
                kCFAllocatorDefault, keys, values, 1,
                &kCFTypeDictionaryKeyCallBacks,
                &kCFTypeDictionaryValueCallBacks);
        }

        CTTypesetterRef ts = CTTypesetterCreateWithAttributedStringAndOptions(
            (CFAttributedStringRef)attr, typesetterOpts);
        if(typesetterOpts != nullptr) CFRelease(typesetterOpts);
        if(levelNum != nullptr) CFRelease(levelNum);
        if(ts == nullptr){
            CFRelease(attr);
            CFRelease(text);
            return out;
        }

        CTLineRef line = CTTypesetterCreateLine(
            ts, CFRangeMake(0, CFAttributedStringGetLength(attr)));
        CFRelease(ts);

        if(line != nullptr){
            CFArrayRef runs = CTLineGetGlyphRuns(line);
            const CFIndex runCount = (runs != nullptr) ? CFArrayGetCount(runs) : 0;
            for(CFIndex ri = 0; ri < runCount; ++ri){
                CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, ri);
                const CFIndex gc = CTRunGetGlyphCount(run);
                if(gc <= 0) continue;

                std::vector<CGGlyph>  glyphs((size_t)gc);
                std::vector<CGPoint>  positions((size_t)gc);
                std::vector<CGSize>   advances((size_t)gc);
                std::vector<CFIndex>  indices((size_t)gc);
                CTRunGetGlyphs(run, CFRangeMake(0, gc), glyphs.data());
                CTRunGetPositions(run, CFRangeMake(0, gc), positions.data());
                CTRunGetAdvances(run, CFRangeMake(0, gc), advances.data());
                // Phase 3.5: source string offsets for each glyph,
                // matching HarfBuzz's `cluster` field. Core Text reports
                // these as UTF-16 indices into the attributed string we
                // built from the run input — exactly what the wrap pass
                // expects (no offset adjustment needed).
                CTRunGetStringIndices(run, CFRangeMake(0, gc), indices.data());

                // Translate Core Text's absolute per-glyph positions
                // into HarfBuzz-style (advance, xOffset, yOffset)
                // tuples. Core Text gives us where it wants each glyph
                // drawn relative to the line origin; the layout engine
                // tracks its own pen and applies `xOffset/yOffset`
                // on top. So we derive the deltas:
                //   - cumulativeX tracks where the pen *would* be by
                //     advance accumulation alone.
                //   - xOffset = chosen - cumulativeX (zero for plain
                //     text; nonzero for mark positioning).
                //   - yOffset = -(chosen.y - baseline.y) because Core
                //     Text Y is Y-up from the baseline, and our
                //     convention is yOffset added to a Y-down canvasY.
                const double runOriginX = positions[0].x;
                const double baselineY  = positions[0].y;
                double cumulativeX = runOriginX;
                for(CFIndex gi = 0; gi < gc; ++gi){
                    ShaperGlyph g;
                    g.glyphId = (std::uint32_t)glyphs[(size_t)gi];
                    g.advance = (float)advances[(size_t)gi].width;
                    g.xOffset = (float)(positions[(size_t)gi].x - cumulativeX);
                    g.yOffset = -(float)(positions[(size_t)gi].y - baselineY);
                    g.cluster = (std::int32_t)indices[(size_t)gi];
                    out.push_back(g);
                    cumulativeX += advances[(size_t)gi].width;
                }
            }
            CFRelease(line);
        }

        CFRelease(attr);
        CFRelease(text);
        return out;
    }
};

class CTFontEngine;

// Text-Layout-Engine-Plan.md Phase 4 — Core Text-backed `IFontFallback`.
//
// Uses `CTFontCreateForString` to ask Core Text "what face will render
// this codepoint?" — Apple's documented public API for accessing the
// system fallback chain. The substitute is materialized through the
// engine's normal `CreateFont` path so it goes through the MSDF probe
// and gets a real `CoreTextFont` with both `native` and `unscaled`
// CTFontRefs; the layout engine then groups substituted glyphs into
// their own `TextSubRun` and renders them through the substitute
// font's `GlyphAtlas`.
//
// Cache key is the substitute face's family name — repeated fallback
// for codepoints serviced by the same face (e.g. every CJK ideograph
// in a string falls back to PingFang SC) returns one shared `Font`
// and one shared atlas.
class CoreTextFontFallback : public IFontFallback {
    CTFontEngine *engine_ = nullptr;
    std::unordered_map<std::string, Core::SharedPtr<Font>> byFamily_;
public:
    explicit CoreTextFontFallback(CTFontEngine *engine)
        : engine_(engine) {}

    Core::SharedPtr<Font> fallbackForCodepoint(
        Core::SharedPtr<Font> requested,
        std::uint32_t codepoint) override;
};

class CTFontEngine : public FontEngine {
    CoreTextShaper shaper_;
    CoreTextFontFallback fallback_{this};
public:
    ITextShaper *   shaper()   override { return &shaper_;   }
    IFontFallback * fallback() override { return &fallback_; }

    // Phase 7. Replaces the legacy `CTTextRect::drawRun` /
    // `CTFramesetter` path. The layout engine has already positioned
    // every glyph; this only needs to lower the per-glyph (gid,
    // canvasX, canvasY) tuples onto a CGBitmapContext via
    // `CTFontDrawGlyphs` and upload the result as a GETexture. Same
    // texture lifecycle as a `drawImage` call.
    BitmapTextResult rasterizeSubRunToTexture(
            const TextSubRun & subRun,
            const Composition::Rect & rect,
            const Composition::Color & color,
            float renderScale) override {
        BitmapTextResult res;
        if(subRun.resolvedFont == nullptr || subRun.glyphIds.empty()
                || subRun.glyphIds.size() != subRun.positions.size()){
            return res;
        }
        auto ctF = std::dynamic_pointer_cast<CoreTextFont>(subRun.resolvedFont);
        if(ctF == nullptr){
            return res;
        }
        // Rasterize against the *unscaled* CTFontRef — the layout
        // engine works in logical pixels and so does this face. The
        // `renderScale` factor lives only on the surface transform.
        CTFontRef ctFont = ctF->getUnscaledFont();
        if(ctFont == nullptr){
            return res;
        }
        const float scale = (renderScale > 0.f) ? renderScale : 1.f;
        const std::size_t pixW =
            std::max<std::size_t>(1, (std::size_t)std::ceil(rect.w * scale));
        const std::size_t pixH =
            std::max<std::size_t>(1, (std::size_t)std::ceil(rect.h * scale));
        const std::size_t bpr = pixW * 4;
        std::vector<std::uint8_t> data(bpr * pixH, 0);

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreateWithData(
            data.data(), pixW, pixH, 8, bpr, cs,
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
            NULL, NULL);
        CGColorSpaceRelease(cs);
        if(ctx == nullptr){
            return res;
        }

        // Flip the user space to Y-down so the layout engine's
        // canvas-space positions map straight in (origin = top-left,
        // y increases downward), and counter-flip the text matrix so
        // glyph ink renders right-side up in screen space.
        CGContextTranslateCTM(ctx, 0.f, (CGFloat)pixH);
        CGContextScaleCTM(ctx, (CGFloat)scale, -(CGFloat)scale);
        CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));

        CGContextSetShouldAntialias(ctx, true);
        CGContextSetAllowsAntialiasing(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, true);
        CGContextSetAllowsFontSmoothing(ctx, true);
        CGContextSetShouldSubpixelPositionFonts(ctx, true);
        CGContextSetAllowsFontSubpixelPositioning(ctx, true);
        CGContextSetRGBFillColor(ctx, color.r, color.g, color.b, color.a);

        std::vector<CGGlyph> glyphs;
        std::vector<CGPoint> positions;
        glyphs.reserve(subRun.glyphIds.size());
        positions.reserve(subRun.positions.size());
        for(std::size_t i = 0; i < subRun.glyphIds.size(); ++i){
            glyphs.push_back(static_cast<CGGlyph>(subRun.glyphIds[i]));
            positions.push_back(CGPointMake(
                (CGFloat)subRun.positions[i].x,
                (CGFloat)subRun.positions[i].y));
        }
        CTFontDrawGlyphs(ctFont, glyphs.data(), positions.data(),
                         glyphs.size(), ctx);
        CGContextFlush(ctx);
        CGContextRelease(ctx);

        OmegaGTE::TextureDescriptor desc {};
        desc.usage         = OmegaGTE::GETexture::ToGPU;
        desc.storage_opts  = OmegaGTE::Shared;
        desc.pixelFormat   = OmegaGTE::TexturePixelFormat::BGRA8Unorm;
        desc.kind          = OmegaGTE::TextureKind::Tex2D;
        desc.width         = (unsigned)pixW;
        desc.height        = (unsigned)pixH;
        auto texture = gte.graphicsEngine->makeTexture(desc);
        if(texture == nullptr){
            return res;
        }
        // The Y-flip transform above lays pixels out top-row-first
        // already, matching the GTE sampler convention — single
        // contiguous copyBytes, no per-row mirroring.
        texture->copyBytes(data.data(), bpr);
        res.texture = texture;
        return res;
    }

    Core::SharedPtr<Font> CreateFont(FontDescriptor & desc) override{
     CTFontRef ref = CTFontCreateWithNameAndOptions((__bridge CFStringRef)[NSString stringWithUTF8String:desc.family.c_str()],CGFloat(desc.size),NULL,kCTFontOptionsPreferSystemFont);
     CTFontSymbolicTraits fontTraits;

     switch (desc.style) {
         case FontDescriptor::Bold:
             fontTraits = kCTFontTraitBold;
             break;
         case FontDescriptor::Italic:
             fontTraits = kCTFontTraitItalic;
             break;
         case FontDescriptor::BoldAndItalic:
             fontTraits = kCTFontTraitBold | kCTFontTraitItalic;
             break;
         case FontDescriptor::Regular :
             fontTraits = 0;
             break;
         default:
             break;
     }

     auto scaleFactor = currentScreenScale();

     CTFontRef _font_final = CTFontCreateCopyWithSymbolicTraits(ref,CGFloat(desc.size) * scaleFactor,NULL,kCTFontTraitBold | kCTFontTraitItalic, fontTraits);
     // Unscaled copy for the resolution-independent MSDF path.
     CTFontRef _font_unscaled = CTFontCreateCopyWithSymbolicTraits(ref,CGFloat(desc.size),NULL,kCTFontTraitBold | kCTFontTraitItalic, fontTraits);
     CFRelease(ref);
     auto font = SharedHandle<CoreTextFont>(new CoreTextFont(desc,_font_final,_font_unscaled));
     // Probe + install the MSDF rasterize callback. Failure of any step
     // leaves the font on Mode::BitmapFallback (the default installed by
     // Font's base ctor).
     probeAndInstallMsdf(*font);
     return font;
 };

 CTFontEngine() = default;

~CTFontEngine() = default;

    /// Decide whether `font`'s underlying face exposes vector outlines
    /// msdfgen can walk (Phase 6.7.4); if so, install a RasterizeFn on
    /// its atlas and flip its mode. Apple Color Emoji and other
    /// outline-less faces fail the probe and stay on BitmapFallback.
    static void probeAndInstallMsdf(CoreTextFont &font) {
#ifdef OMEGAWTK_HAVE_MSDFGEN
        CTFontRef unscaledFont = font.getUnscaledFont();
        if(unscaledFont == nullptr){
            return;
        }

        // Outline probe: 'A' should map to a real glyph with an
        // extractable path. A NULL path (Apple Color Emoji, bitmap-only
        // faces) routes the font to the bitmap fallback path.
        UniChar probeChar = 'A';
        CGGlyph probeGlyph = 0;
        if(!CTFontGetGlyphsForCharacters(unscaledFont,&probeChar,&probeGlyph,1) || probeGlyph == 0){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] CoreTextFont: '" << font.desc.family
                          << "' size=" << font.desc.size
                          << " no glyph for probe char; using BitmapFallback" << std::endl;
            }
            return;
        }
        CGPathRef probePath = CTFontCreatePathForGlyph(unscaledFont,probeGlyph,NULL);
        if(probePath == nullptr){
            if(textTraceEnabled()){
                std::cout << "[wtk-text] CoreTextFont: '" << font.desc.family
                          << "' size=" << font.desc.size
                          << " no extractable outline; using BitmapFallback" << std::endl;
            }
            return;
        }
        CGPathRelease(probePath);

        // The lambda captures the WTK Font's unscaled CTFontRef
        // (lifetime tied to the Font, which outlives its atlas). Each
        // call walks the glyph outline via CGPathApply and runs msdfgen.
        CTFontRef capturedFont = unscaledFont;
        font.atlas().setRasterizeFn(
                [capturedFont](std::uint32_t glyphId,
                               GlyphAtlas::RasterizedGlyph &out) -> bool {
            if(capturedFont == nullptr){
                return false;
            }
            CGGlyph glyph = static_cast<CGGlyph>(glyphId);
            CGPathRef path = CTFontCreatePathForGlyph(capturedFont,glyph,NULL);
            if(path == nullptr){
                return false;
            }

            msdfgen::Shape shape;
            CGPathMsdfContext ctx;
            ctx.shape = &shape;
            CGPathApply(path,&ctx,cgPathApplier);
            CGPathRelease(path);

            // msdfgen pipeline: normalize → orient contours → edge
            // coloring → generate. `orientContours()` resolves the
            // CW/CCW winding ambiguity between font formats; without it
            // the signed-distance sign inverts for one whole family of
            // fonts.
            shape.normalize();
            shape.orientContours();
            msdfgen::edgeColoringSimple(shape, 3.0);

            // Tight bound + fit the 32×32 tile around the shape with a
            // small padding so the distance band stays inside the tile.
            // `Shape::bound` *expands* the passed-in box — it never
            // initializes it — so seeding with zeros would force every
            // glyph's bbox to include the origin (0,0), inflating the
            // box for any glyph with a left bearing or whose ink does
            // not touch the baseline. `getBounds()` seeds correctly.
            const msdfgen::Shape::Bounds bounds = shape.getBounds();
            double l = bounds.l, b = bounds.b, r = bounds.r, t = bounds.t;
            if(r <= l || t <= b){
                return false;
            }
            // The tile is sized to the glyph's padded bbox, NOT a fixed
            // square. A fixed square tile + uniform-scale fit leaves a
            // per-glyph empty margin, and every scheme to address that
            // margin (sub-rect UV, etc.) just relocates the seam. With a
            // glyph-sized tile there is no margin: the tile *is* the
            // glyph, the render path's quad is the tile, UV is the whole
            // packed rect. `scale` keeps the larger dimension at
            // `kMsdfTileSize` so atlas density is unchanged.
            const double padding = 2.0;
            l -= padding; b -= padding; r += padding; t += padding;
            const double scale = static_cast<double>(kMsdfTileSize) /
                                 std::max(r - l, t - b);
            const unsigned tileW = std::max(1u,
                static_cast<unsigned>(std::ceil((r - l) * scale)));
            const unsigned tileH = std::max(1u,
                static_cast<unsigned>(std::ceil((t - b) * scale)));
            const msdfgen::Vector2 scaleV(scale, scale);
            const msdfgen::Vector2 translate(-l, -b);

            msdfgen::Bitmap<float, 3> msdf((int)tileW, (int)tileH);
            msdfgen::generateMSDF(msdf, shape,
                                  msdfgen::Range(kMsdfRange / scale),
                                  scaleV, translate);

            // Reorient the msdfgen tile to Y-downward. msdfgen's
            // default `Y_UPWARD` stores row 0 at the bottom of the
            // glyph; `BitmapSection::reorient(Y_DOWNWARD)` flips the
            // section view (pixel pointer to last row, negated
            // `rowStride`) so straight `section(x, y)` reads now run
            // top-to-bottom. Phase-2.5: collapse the three-stage flip
            // chain to a single canonical orientation — `GlyphAtlas`
            // then uploads straight, and the canvas-top ↔ `v0` UV
            // pairing in `emitTextSubRun` carries the orientation
            // through to the fragment with zero implicit flips.
            msdfgen::BitmapSection<float, 3> section = msdf;
            section.reorient(msdfgen::Y_DOWNWARD);

            // Quantize float → uint8, straight through. No extra +0.5
            // bias — `generateMSDF` already maps signed distance to
            // [0, 1] with the glyph edge at 0.5.
            out.pxW = tileW;
            out.pxH = tileH;
            out.rgb.resize(static_cast<std::size_t>(tileW) * tileH * 3);
            for(unsigned y = 0; y < tileH; ++y){
                for(unsigned x = 0; x < tileW; ++x){
                    const float *px = section((int)x, (int)y);
                    const auto quant = [](float v) {
                        const float scaled = std::clamp(v * 255.f + 0.5f, 0.f, 255.f);
                        return static_cast<std::uint8_t>(scaled);
                    };
                    const std::size_t i = (static_cast<std::size_t>(y) * tileW + x) * 3;
                    out.rgb[i + 0] = quant(px[0]);
                    out.rgb[i + 1] = quant(px[1]);
                    out.rgb[i + 2] = quant(px[2]);
                }
            }

            // Phase-2.5 Skia-style top-anchored metrics. `advance.width`
            // is in font-pixel space (the unscaled CTFontRef is sized in
            // points == logical pixels).
            CGSize advance {};
            CTFontGetAdvancesForGlyphs(capturedFont,kCTFontOrientationHorizontal,&glyph,&advance,1);
            out.metrics.advance = static_cast<float>(advance.width);

            // Pen-relative quad placement. `l, b, r, t` are the padded
            // bbox extents in shape coords (Y-up, pen origin at 0).
            // Convert to top-anchored canvas-space metrics:
            //   fLeft   = bbox left = l (positive → right of pen).
            //   fTop    = distance from baseline up to bbox top = t.
            //   fWidth  = bbox width  in font-pixels = r - l.
            //   fHeight = bbox height in font-pixels = t - b.
            // No `scale` round-trip — `fWidth/fHeight` are the exact
            // canvas-pixel dimensions of the quad.
            out.metrics.fLeft   = static_cast<float>(l);
            out.metrics.fTop    = static_cast<float>(t);
            out.metrics.fWidth  = static_cast<float>(r - l);
            out.metrics.fHeight = static_cast<float>(t - b);

            // ASCII dump of the rasterized tile (median of the 3 MSDF
            // channels) — mirrors the Linux backend. Row 0 first, so
            // the dump shows the tile exactly as `out.rgb` is laid out
            // (before the upload flip in GlyphAtlas).
            if(textTraceEnabled()){
                std::cout << "[wtk-text] DUMP gid=" << glyphId
                          << " l=" << l << " b=" << b << " r=" << r << " t=" << t
                          << " scale=" << scale
                          << " tile=" << tileW << "x" << tileH << std::endl;
                for(unsigned yy = 0; yy < tileH; ++yy){
                    std::string row;
                    for(unsigned xx = 0; xx < tileW; ++xx){
                        const std::size_t idx = (static_cast<std::size_t>(yy) * tileW + xx) * 3;
                        const int med = std::max(std::min((int)out.rgb[idx+0], (int)out.rgb[idx+1]),
                                                 std::min(std::max((int)out.rgb[idx+0], (int)out.rgb[idx+1]), (int)out.rgb[idx+2]));
                        row += (med > 153 ? '#' : (med > 102 ? '.' : ' '));
                    }
                    std::cout << "[wtk-text]   |" << row << "|" << std::endl;
                }
            }

            return true;
        });
        font.setMode(Font::Mode::MSDF);

        if(textTraceEnabled()){
            std::cout << "[wtk-text] CoreTextFont: '" << font.desc.family
                      << "' size=" << font.desc.size << " -> MSDF mode" << std::endl;
        }

        // Smoke-probe: rasterize the 'A' glyph so any breakage in the
        // CGPath/msdfgen pipeline surfaces at Font construction time.
        const bool ok = font.atlas().ensureGlyph(probeGlyph);
        if(textTraceEnabled()){
            std::cout << "[wtk-text] CoreTextFont: smoke ensureGlyph('A' = "
                      << probeGlyph << ") -> " << (ok ? "ok" : "FAILED") << std::endl;
        }
#else
        (void)font;
        if(textTraceEnabled()){
            std::cout << "[wtk-text] CoreTextFont: built without OMEGAWTK_HAVE_MSDFGEN; using BitmapFallback"
                      << std::endl;
        }
#endif // OMEGAWTK_HAVE_MSDFGEN
    }

 Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor &desc) override{
     CTFontSymbolicTraits fontTraits;

     switch (desc.style) {
         case FontDescriptor::Bold:
             fontTraits = kCTFontTraitBold;
             break;
         case FontDescriptor::Italic:
             fontTraits = kCTFontTraitItalic;
             break;
         case FontDescriptor::BoldAndItalic:
             fontTraits = kCTFontTraitBold | kCTFontTraitItalic;
             break;
         case FontDescriptor::Regular :
             fontTraits = 0;
             break;
         default:
             break;
     }

     NSURL *url = [NSURL fileURLWithPath:Native::Cocoa::common_string_to_ns_string(path.absPath()) isDirectory:NO];
     CFArrayRef fontDescriptors = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
     CTFontDescriptorRef idealFont;
     for(unsigned i = 0;i < CFArrayGetCount(fontDescriptors);i++){
         CTFontDescriptorRef fd = (CTFontDescriptorRef)CFArrayGetValueAtIndex(fontDescriptors,i);
         CFStringRef name = (CFStringRef)CTFontDescriptorCopyAttribute(fd,kCTFontNameAttribute);
         CFNumberRef symbolicTraits = (CFNumberRef)CTFontDescriptorCopyAttribute(fd,kCTFontSymbolicTrait);
         NSString *str = (__bridge id)name;
         int val;
         CFNumberGetValue(symbolicTraits,kCFNumberIntType,&val);
         if(desc.family == str.UTF8String && fontTraits == val){
             idealFont = fd;
         };
     };

     auto scaleFactor = currentScreenScale();

     CTFontRef f = CTFontCreateWithFontDescriptor(idealFont,CGFloat(desc.size) * scaleFactor,NULL);
     CTFontRef fUnscaled = CTFontCreateWithFontDescriptor(idealFont,CGFloat(desc.size),NULL);
     // MSDF probe for file-loaded fonts is a Phase-6.7 follow-up; this
     // font stays on BitmapFallback for now.
     return std::make_shared<CoreTextFont>(desc,f,fUnscaled);
 };

 // Asset-bundle font loading. Builds CTFontDescriptors directly off the
 // bundle's in-memory bytes via CTFontManagerCreateFontDescriptorsFromData,
 // then materializes a scaled + unscaled CTFontRef for the descriptor
 // matching the requested family + style. No temp file, no font-manager
 // registration. The MSDF probe runs against the unscaled face so an
 // asset-loaded font lights up the same MSDF path as system-loaded ones.
 Core::SharedPtr<Font> CreateFontFromAsset(
         OmegaCommon::AssetBundle *bundle,
         const OmegaCommon::String &assetName,
         FontDescriptor &desc) override {
     if(bundle == nullptr){
         return nullptr;
     }
     auto loadResult = bundle->load(assetName);
     if(!loadResult.isOk()){
         if(textTraceEnabled()){
             std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                       << "' bundle->load failed: " << loadResult.error()
                       << std::endl;
         }
         return nullptr;
     }
     auto blob = std::move(loadResult.value());
     if(blob.empty()){
         if(textTraceEnabled()){
             std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                       << "' bundle entry is empty" << std::endl;
         }
         return nullptr;
     }

     // CTFontManagerCreateFontDescriptorsFromData copies the bytes into
     // its own backing store, so the blob can be a local that goes out
     // of scope after this call. No lifetime anchoring needed on the
     // CoreTextFont (contrast with FreeType's FT_New_Memory_Face).
     CFDataRef cfData = CFDataCreate(kCFAllocatorDefault,
                                     blob.data(),
                                     (CFIndex)blob.size());
     if(cfData == nullptr){
         return nullptr;
     }
     CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromData(cfData);
     CFRelease(cfData);
     if(descriptors == nullptr || CFArrayGetCount(descriptors) == 0){
         if(textTraceEnabled()){
             std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                       << "' did not parse as a font" << std::endl;
         }
         if(descriptors != nullptr) CFRelease(descriptors);
         return nullptr;
     }

     // Required symbolic traits derived from desc.style.
     CTFontSymbolicTraits requiredTraits = 0;
     switch(desc.style){
         case FontDescriptor::Bold:
             requiredTraits = kCTFontTraitBold; break;
         case FontDescriptor::Italic:
             requiredTraits = kCTFontTraitItalic; break;
         case FontDescriptor::BoldAndItalic:
             requiredTraits = kCTFontTraitBold | kCTFontTraitItalic; break;
         case FontDescriptor::Regular:
         default:
             requiredTraits = 0; break;
     }

     // Walk the descriptors and pick the one matching desc.family +
     // requiredTraits. Fall back to descriptor[0] if no exact match —
     // single-face TTFs typically carry one descriptor with the
     // canonical family name, which matches the request only when the
     // caller already knows what's in the file.
     CTFontDescriptorRef chosen = nullptr;
     const CFIndex n = CFArrayGetCount(descriptors);
     for(CFIndex i = 0; i < n; ++i){
         CTFontDescriptorRef fd = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descriptors, i);
         CFStringRef family = (CFStringRef)CTFontDescriptorCopyAttribute(fd, kCTFontFamilyNameAttribute);
         CFNumberRef traitsNum = (CFNumberRef)CTFontDescriptorCopyAttribute(fd, kCTFontSymbolicTrait);
         int traitsVal = 0;
         if(traitsNum != nullptr){
             CFNumberGetValue(traitsNum, kCFNumberIntType, &traitsVal);
             CFRelease(traitsNum);
         }
         bool familyMatches = false;
         if(family != nullptr){
             NSString *familyStr = (__bridge id)family;
             familyMatches = (desc.family == familyStr.UTF8String);
             CFRelease(family);
         }
         if(familyMatches &&
            (static_cast<CTFontSymbolicTraits>(traitsVal) & requiredTraits) == requiredTraits){
             chosen = fd;
             break;
         }
     }
     if(chosen == nullptr){
         chosen = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descriptors, 0);
     }

     const auto scaleFactor = currentScreenScale();
     CTFontRef scaledFont   = CTFontCreateWithFontDescriptor(chosen,
         CGFloat(desc.size) * scaleFactor, NULL);
     CTFontRef unscaledFont = CTFontCreateWithFontDescriptor(chosen,
         CGFloat(desc.size), NULL);
     CFRelease(descriptors);
     if(scaledFont == nullptr || unscaledFont == nullptr){
         if(scaledFont != nullptr) CFRelease(scaledFont);
         if(unscaledFont != nullptr) CFRelease(unscaledFont);
         return nullptr;
     }

     auto font = SharedHandle<CoreTextFont>(
         new CoreTextFont(desc, scaledFont, unscaledFont));
     probeAndInstallMsdf(*font);
     if(textTraceEnabled()){
         std::cout << "[wtk-text] CreateFontFromAsset: '" << desc.family
                   << "' size=" << desc.size
                   << " loaded from bundle asset '" << assetName
                   << "' (" << blob.size() << " bytes) -> "
                   << (font->mode() == Font::Mode::MSDF ? "MSDF" : "BitmapFallback")
                   << std::endl;
     }
     return font;
 }
};

// Phase 4. Out-of-line because the cache miss path delegates to
// `CTFontEngine::CreateFont` — defining it inside the class would
// force the lookup helper to be visible before `CTFontEngine` is
// complete, which is awkward with the existing single-pass file
// layout.
Core::SharedPtr<Font> CoreTextFontFallback::fallbackForCodepoint(
        Core::SharedPtr<Font> requested,
        std::uint32_t codepoint){
    if(engine_ == nullptr || requested == nullptr){
        return nullptr;
    }
    auto reqCT = std::dynamic_pointer_cast<CoreTextFont>(requested);
    if(reqCT == nullptr){
        return nullptr;
    }
    // Drive the lookup from the *unscaled* face: the layout engine
    // and the MSDF probe both work in logical-pixel space, so we ask
    // Core Text for a substitute at the same size class.
    CTFontRef baseFont = reqCT->getUnscaledFont();
    if(baseFont == nullptr){
        return nullptr;
    }

    // Build a CFString carrying just this codepoint (surrogate pair
    // for supplementary plane).
    UniChar buf[2];
    CFIndex bufLen = 0;
    if(codepoint < 0x10000u){
        buf[0] = (UniChar)codepoint;
        bufLen = 1;
    } else if(codepoint <= 0x10FFFFu){
        const std::uint32_t v = codepoint - 0x10000u;
        buf[0] = (UniChar)(0xD800u + (v >> 10));
        buf[1] = (UniChar)(0xDC00u + (v & 0x3FFu));
        bufLen = 2;
    } else {
        return nullptr;
    }
    CFStringRef probe = CFStringCreateWithCharacters(
        kCFAllocatorDefault, buf, bufLen);
    if(probe == nullptr){
        return nullptr;
    }
    CTFontRef sub = CTFontCreateForString(baseFont, probe,
                                          CFRangeMake(0, bufLen));
    CFRelease(probe);
    if(sub == nullptr){
        return nullptr;
    }

    // Equal-to-requested → Core Text declined to substitute, which
    // means the requested face actually does cover the codepoint
    // (the shaper still produced .notdef for some other reason —
    // a malformed glyph table, say). Don't loop.
    CFStringRef subName = CTFontCopyPostScriptName(sub);
    CFStringRef reqName = CTFontCopyPostScriptName(baseFont);
    const bool sameAsRequested = (subName != nullptr && reqName != nullptr
        && CFStringCompare(subName, reqName, 0) == kCFCompareEqualTo);
    if(reqName != nullptr) CFRelease(reqName);
    if(sameAsRequested){
        if(subName != nullptr) CFRelease(subName);
        CFRelease(sub);
        return nullptr;
    }

    // Substitute's family name keys the cache and feeds the
    // FontDescriptor we hand to CreateFont. PostScript name (used
    // for the equality check above) sometimes diverges from family
    // name for subfamilies — for our cache it's the family name we
    // actually want, since CTFontCreateWithNameAndOptions matches
    // on family.
    CFStringRef familyCF = CTFontCopyFamilyName(sub);
    if(subName != nullptr) CFRelease(subName);
    CFRelease(sub);
    if(familyCF == nullptr){
        return nullptr;
    }
    char familyBuf[256] = {};
    const bool gotFamily = CFStringGetCString(
        familyCF, familyBuf, sizeof(familyBuf), kCFStringEncodingUTF8);
    CFRelease(familyCF);
    if(!gotFamily || familyBuf[0] == '\0'){
        return nullptr;
    }

    auto it = byFamily_.find(familyBuf);
    if(it != byFamily_.end()){
        return it->second;
    }

    // Materialize a real `CoreTextFont` for the substitute family at
    // the requested font's size + style. CreateFont handles the MSDF
    // probe, so the returned Font is ready to feed the layout engine
    // / atlas path with no further setup.
    FontDescriptor fbDesc(familyBuf, requested->desc.size,
                          requested->desc.style);
    Core::SharedPtr<Font> resolved = engine_->CreateFont(fbDesc);
    byFamily_[familyBuf] = resolved;

    if(textTraceEnabled()){
        std::cout << "[wtk-text] CoreTextFontFallback: cp=U+"
                  << std::hex << codepoint << std::dec
                  << " -> '" << familyBuf << "' mode="
                  << (resolved != nullptr
                      && resolved->mode() == Font::Mode::MSDF
                      ? "MSDF" : "BitmapFallback") << std::endl;
    }
    return resolved;
}

FontEngine * FontEngine::inst(){
    return instance;
};

 void FontEngine::Create(){
        instance = new CTFontEngine();
     };
      void FontEngine::Destroy(){
         // Free glyph-atlas GPU textures before engine teardown / OmegaGTE::Close
         // — owned through Font, which app widgets keep alive past shutdown.
         // See GlyphAtlas::releaseAllTextures. (Metal's allocator is forgiving,
         // but the contract is the same across backends.)
         GlyphAtlas::releaseAllTextures();
         delete instance;
         instance = nullptr;
     };

};
