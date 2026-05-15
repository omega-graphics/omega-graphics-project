#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "NativePrivate/macos/CocoaUtils.h"
#include "../GlyphAtlas.h"

#include "omega-common/unicode.h"

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

static NSTextAlignment toNSTextAlignment(TextLayoutDescriptor::Alignment alignment){
    switch(alignment){
        case TextLayoutDescriptor::LeftUpper:
        case TextLayoutDescriptor::LeftCenter:
        case TextLayoutDescriptor::LeftLower:
            return NSTextAlignmentLeft;
        case TextLayoutDescriptor::MiddleUpper:
        case TextLayoutDescriptor::MiddleCenter:
        case TextLayoutDescriptor::MiddleLower:
            return NSTextAlignmentCenter;
        case TextLayoutDescriptor::RightUpper:
        case TextLayoutDescriptor::RightCenter:
        case TextLayoutDescriptor::RightLower:
            return NSTextAlignmentRight;
        default:
            return NSTextAlignmentLeft;
    }
}

static NSLineBreakMode toNSLineBreakMode(TextLayoutDescriptor::Wrapping wrapping){
    switch(wrapping){
        case TextLayoutDescriptor::WrapByWord:
            return NSLineBreakByWordWrapping;
        case TextLayoutDescriptor::WrapByCharacter:
            return NSLineBreakByCharWrapping;
        case TextLayoutDescriptor::None:
        default:
            return NSLineBreakByClipping;
    }
}

namespace {
    bool textTraceEnabled() {
        static const bool enabled = []() {
            const char *e = std::getenv("OMEGAWTK_TRACE_TEXT");
            return e != nullptr && e[0] != '\0' && e[0] != '0';
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
     ~CoreTextFont() override {
         if(native != nullptr){
             CFRelease(native);
         }
         if(unscaled != nullptr){
             CFRelease(unscaled);
         }
     };
 };

class CTGlyphRun : public GlyphRun {
public:
    NSAttributedString *str;
    Core::SharedPtr<CoreTextFont> font;

    Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
        return {};
    }

    // Phase 6.7-c3 (macOS): shape the run against its font at the
    // *unscaled* design size and produce positioned glyph IDs for the
    // MSDF render path. Core Text performs font fallback as part of
    // line construction; this chunk ships a single sub-run, so any run
    // whose resolved face does not match the requested face flips
    // `requiresFallback` and the caller routes the whole string to the
    // bitmap path.
    GlyphRun::ShapedTextRun shape(const Composition::Rect &rect,
                                  const TextLayoutDescriptor &layoutDesc) override {
        GlyphRun::ShapedTextRun result;
        if(font == nullptr || str == nil){
            result.requiresFallback = true;
            return result;
        }
        CTFontRef unscaledFont = font->getUnscaledFont();
        if(unscaledFont == nullptr){
            result.requiresFallback = true;
            return result;
        }

        // Rebuild the attributed string at the unscaled design size.
        // `str` was authored with the scaled `native` font for the
        // bitmap path; the MSDF atlas is resolution-independent, so we
        // lay out in logical pixels here and let the render viewport
        // apply DPR downstream.
        NSString *text = [str string];
        if(text == nil || text.length == 0){
            return result;
        }
        auto attributed = [[NSMutableAttributedString alloc] initWithString:text];
        auto range = NSMakeRange(0,attributed.length);
        auto paragraphStyle = [[NSMutableParagraphStyle alloc] init];
        [paragraphStyle setAlignment:toNSTextAlignment(layoutDesc.alignment)];
        [paragraphStyle setLineBreakMode:toNSLineBreakMode(layoutDesc.wrapping)];
        [attributed addAttribute:NSFontAttributeName value:(__bridge id)unscaledFont range:range];
        [attributed addAttribute:NSParagraphStyleAttributeName value:paragraphStyle range:range];

        CGPathRef textPath = CGPathCreateWithRect(CGRectMake(0.f,0.f,rect.w,rect.h),NULL);
        CTFramesetterRef framesetter =
            CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
        CFRange frameRange = CFRangeMake(0,CFIndex(attributed.length));
        if(layoutDesc.lineLimit > 0){
            CTFrameRef preliminaryFrame = CTFramesetterCreateFrame(framesetter,frameRange,textPath,NULL);
            if(preliminaryFrame != nullptr){
                CFArrayRef lines = CTFrameGetLines(preliminaryFrame);
                CFIndex lineCount = lines != nullptr ? CFArrayGetCount(lines) : 0;
                if(lineCount > static_cast<CFIndex>(layoutDesc.lineLimit)){
                    CTLineRef lastAllowedLine = (CTLineRef)CFArrayGetValueAtIndex(
                            lines,
                            static_cast<CFIndex>(layoutDesc.lineLimit) - 1);
                    if(lastAllowedLine != nullptr){
                        CFRange visibleRange = CTLineGetStringRange(lastAllowedLine);
                        CFIndex endIndex = visibleRange.location + visibleRange.length;
                        if(endIndex > 0 && endIndex < frameRange.length){
                            frameRange.length = endIndex;
                        }
                    }
                }
                CFRelease(preliminaryFrame);
            }
        }
        CTFrameRef frame = CTFramesetterCreateFrame(framesetter,frameRange,textPath,NULL);
        CGPathRelease(textPath);
        CFRelease(framesetter);
        if(frame == nullptr){
            result.requiresFallback = true;
            return result;
        }

        // Requested face's PostScript name. A CTRun whose resolved font
        // reports a different name was substituted by Core Text's
        // fallback — chunk 3 bails the whole string to the bitmap path
        // for those.
        CFStringRef ourName = CTFontCopyPostScriptName(unscaledFont);

        CFArrayRef lines = CTFrameGetLines(frame);
        CFIndex lineCount = lines != nullptr ? CFArrayGetCount(lines) : 0;
        std::vector<CGPoint> lineOrigins(lineCount > 0 ? (size_t)lineCount : 1);
        if(lineCount > 0){
            CTFrameGetLineOrigins(frame,CFRangeMake(0,lineCount),lineOrigins.data());
        }

        bool fallback = false;
        for(CFIndex li = 0; li < lineCount && !fallback; ++li){
            CTLineRef line = (CTLineRef)CFArrayGetValueAtIndex(lines,li);
            // Core Text frame coordinates are Y-up with origin at the
            // bottom-left of the path rect; the render path wants Y-down
            // baseline positions relative to the rect's top-left.
            CGPoint lineOrigin = lineOrigins[(size_t)li];
            CFArrayRef runs = CTLineGetGlyphRuns(line);
            CFIndex runCount = runs != nullptr ? CFArrayGetCount(runs) : 0;
            for(CFIndex ri = 0; ri < runCount; ++ri){
                CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs,ri);
                CFDictionaryRef attrs = CTRunGetAttributes(run);
                CTFontRef runFont = attrs != nullptr
                    ? (CTFontRef)CFDictionaryGetValue(attrs,kCTFontAttributeName)
                    : nullptr;
                bool matches = false;
                if(runFont != nullptr && ourName != nullptr){
                    CFStringRef runName = CTFontCopyPostScriptName(runFont);
                    if(runName != nullptr){
                        matches = (CFStringCompare(ourName,runName,0) == kCFCompareEqualTo);
                        CFRelease(runName);
                    }
                }
                if(!matches){
                    fallback = true;
                    break;
                }

                CFIndex glyphCount = CTRunGetGlyphCount(run);
                if(glyphCount <= 0){
                    continue;
                }
                std::vector<CGGlyph> glyphs((size_t)glyphCount);
                std::vector<CGPoint> positions((size_t)glyphCount);
                CTRunGetGlyphs(run,CFRangeMake(0,glyphCount),glyphs.data());
                CTRunGetPositions(run,CFRangeMake(0,glyphCount),positions.data());
                for(CFIndex gi = 0; gi < glyphCount; ++gi){
                    CGGlyph glyph = glyphs[(size_t)gi];
                    if(glyph == 0){
                        // .notdef — should have triggered fallback above,
                        // but skip defensively.
                        continue;
                    }
                    const double penX = (double)lineOrigin.x + (double)positions[(size_t)gi].x;
                    const double penYUp = (double)lineOrigin.y + (double)positions[(size_t)gi].y;
                    const double baselineFromTop = (double)rect.h - penYUp;
                    result.glyphIds.push_back((std::uint32_t)glyph);
                    result.positions.push_back(Composition::Point2D{
                        (float)penX,
                        (float)baselineFromTop});
                }
            }
        }

        if(ourName != nullptr){
            CFRelease(ourName);
        }
        CFRelease(frame);

        if(fallback){
            result.requiresFallback = true;
            result.glyphIds.clear();
            result.positions.clear();
        }

        if(textTraceEnabled()){
            std::cout << "[wtk-text] CTGlyphRun::shape -> "
                      << (result.requiresFallback ? "FALLBACK" : "MSDF")
                      << ", glyphs=" << result.glyphIds.size() << std::endl;
        }
        return result;
    }

};

Core::SharedPtr<GlyphRun>
GlyphRun::fromUStringAndFont(const OmegaCommon::UniString &str, Core::SharedPtr<Font> &font) {
    auto run = new CTGlyphRun();
    run->font = std::dynamic_pointer_cast<CoreTextFont>(font);
    auto text = [NSString stringWithCharacters:(const unichar *)str.getBuffer() length:str.length()];
    auto nativeFont = (run->font != nullptr) ? (CTFontRef)run->font->getNativeFont() : nullptr;
    if(nativeFont != nullptr){
        run->str = [[NSAttributedString alloc] initWithString:text
                                                    attributes:@{NSFontAttributeName:(__bridge id)nativeFont}];
    }
    else {
        run->str = [[NSAttributedString alloc] initWithString:text];
    }
    return (Core::SharedPtr<GlyphRun>)run;
}

 class CTTextRect : public TextRect {
     CTFramesetterRef framesetterRef;
     CTFrameRef frame;
     NSAttributedString *strData;
     TextLayoutDescriptor layoutDesc;
     void _updateStrInternal(){

     };
 public:
     CTTextRect(Composition::Rect & rect,const TextLayoutDescriptor &layoutDesc):
     TextRect(rect),
     framesetterRef(nullptr),
     frame(nullptr),
     strData(nil),
     layoutDesc(layoutDesc){
         NSLog(@"CTTextRect Create With W: %f H: %f",rect.w,rect.h);
     };
     void drawRun(Core::SharedPtr<GlyphRun> &glyphRun, const Composition::Color &color) override {
          auto gr = std::dynamic_pointer_cast<CTGlyphRun>(glyphRun);
          if(gr == nullptr || gr->str == nil){
              return;
          }
          if(frame != nullptr){
              CFRelease(frame);
              frame = nullptr;
          }
          if(framesetterRef != nullptr){
              CFRelease(framesetterRef);
              framesetterRef = nullptr;
          }

          auto attributed = [[NSMutableAttributedString alloc] initWithAttributedString:gr->str];
          auto range = NSMakeRange(0,attributed.length);
          auto textColor = [NSColor colorWithSRGBRed:color.r green:color.g blue:color.b alpha:color.a];
          auto paragraphStyle = [[NSMutableParagraphStyle alloc] init];
          [paragraphStyle setAlignment:toNSTextAlignment(layoutDesc.alignment)];
          [paragraphStyle setLineBreakMode:toNSLineBreakMode(layoutDesc.wrapping)];

          if(range.length > 0){
              [attributed addAttribute:NSForegroundColorAttributeName value:textColor range:range];
              [attributed addAttribute:NSParagraphStyleAttributeName value:paragraphStyle range:range];
          }

          auto nativeFont = (gr->font != nullptr) ? (CTFontRef)gr->font->getNativeFont() : nullptr;
          if(range.length > 0 && nativeFont != nullptr){
              [attributed addAttribute:NSFontAttributeName value:(__bridge id)nativeFont range:range];
          }

          strData = attributed;

          CGFloat scaleFactor = currentScreenScale();
          CGPathRef textPath = CGPathCreateWithRect(CGRectMake(0.f,0.f,rect.w * scaleFactor,rect.h * scaleFactor),NULL);
          framesetterRef = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)strData);
          CFRange frameRange = CFRangeMake(0,CFIndex(strData.length));
          if(layoutDesc.lineLimit > 0){
              CTFrameRef preliminaryFrame = CTFramesetterCreateFrame(framesetterRef,frameRange,textPath,NULL);
              if(preliminaryFrame != nullptr){
                  CFArrayRef lines = CTFrameGetLines(preliminaryFrame);
                  CFIndex lineCount = lines != nullptr ? CFArrayGetCount(lines) : 0;
                  if(lineCount > static_cast<CFIndex>(layoutDesc.lineLimit)){
                      CTLineRef lastAllowedLine = (CTLineRef)CFArrayGetValueAtIndex(
                              lines,
                              static_cast<CFIndex>(layoutDesc.lineLimit) - 1);
                      if(lastAllowedLine != nullptr){
                          CFRange visibleRange = CTLineGetStringRange(lastAllowedLine);
                          CFIndex endIndex = visibleRange.location + visibleRange.length;
                          if(endIndex > 0 && endIndex < frameRange.length){
                              frameRange.length = endIndex;
                          }
                      }
                  }
                  CFRelease(preliminaryFrame);
              }
          }
          frame = CTFramesetterCreateFrame(framesetterRef,frameRange,textPath,NULL);
          CGPathRelease(textPath);
     }
     void * getNative() override{
         return (void *)frame;
     };
     void getGlyphBoundingBoxes(Composition::Rect **rects, unsigned * count){
         *count = 0;
         CoreTextFont *fontRef = nullptr;
         CFArrayRef lines = CTFrameGetLines(frame);
         for(unsigned idx = 0;idx < CFArrayGetCount(lines);idx++){
             CTLineRef line = (CTLineRef)CFArrayGetValueAtIndex(lines,idx);
             CFArrayRef runs =  CTLineGetGlyphRuns(line);
             for(unsigned j = 0;j < CFArrayGetCount(runs);j++){
                 CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs,j);
                 const CGGlyph *ptr = CTRunGetGlyphsPtr(run);
                 CFIndex count = CTRunGetGlyphCount(run);
                 CGRect *rect = new CGRect[count];
                 CTFontGetBoundingRectsForGlyphs(fontRef->native,kCTFontOrientationDefault,ptr,rect,count);
             };
         }
     };
     // void reload() {

     // };
     BitmapRes toBitmap() override{
        BitmapRes res;
         CGFloat scaleFactor = currentScreenScale();
         const size_t pixelWidth = size_t(rect.w * scaleFactor);
         const size_t pixelHeight = size_t(rect.h * scaleFactor);
         const size_t bytesPerRow = pixelWidth * 4;
         const size_t byteCount = bytesPerRow * pixelHeight;
         auto *data = new unsigned char[byteCount];
         std::memset(data,0,byteCount);

         CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
         CGContextRef context = CGBitmapContextCreateWithData(data,pixelWidth,pixelHeight,8,bytesPerRow,colorSpace,kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,NULL,NULL);
         CGColorSpaceRelease(colorSpace);
         if(context == nullptr){
             delete [](unsigned char *) data;
             return res;
         }
         CGContextSetAllowsAntialiasing(context,true);
         CGContextSetShouldAntialias(context,true);
         CGContextSetInterpolationQuality(context,kCGInterpolationHigh);
         CGContextSetShouldSmoothFonts(context,true);
         CGContextSetAllowsFontSmoothing(context,true);
         CGContextSetShouldSubpixelPositionFonts(context,true);
         CGContextSetAllowsFontSubpixelPositioning(context,true);
         CGContextSetShouldSubpixelQuantizeFonts(context,true);
         CGContextSetAllowsFontSubpixelQuantization(context,true);
         CGContextSetTextMatrix(context,CGAffineTransformIdentity);
         if(frame != nullptr){
             CTFrameDraw(frame,context);
         }
         CGContextFlush(context);


         OmegaGTE::TextureDescriptor desc {};
         desc.usage = OmegaGTE::GETexture::ToGPU;
         desc.storage_opts = OmegaGTE::Shared;
         desc.pixelFormat = OmegaGTE::TexturePixelFormat::BGRA8Unorm;
         desc.kind = OmegaGTE::TextureKind::Tex2D;
         desc.width = (unsigned)pixelWidth;
         desc.height = (unsigned)pixelHeight;

         auto texture = gte.graphicsEngine->makeTexture(desc);
         NSLog(@"CGBitmapContextData: %p",data);
         // CGBitmapContext writes rows bottom-up (CG default coord system);
         // GTE samplers treat row 0 as the top, so an unflipped upload
         // shows text upside down. Mirror BitmapTextureCache's §4.5
         // region-aware row-flip on upload — dest row `r` consumes source
         // row `pixelHeight - 1 - r`.
         {
             auto *base = static_cast<unsigned char *>(data);
             for(unsigned r = 0; r < (unsigned)pixelHeight; ++r){
                 OmegaGTE::TextureRegion region {0, r, 0, (unsigned)pixelWidth, 1, 1};
                 texture->copyBytes(base + (pixelHeight - 1 - r) * bytesPerRow,
                                    bytesPerRow, region);
             }
         }

        CGContextRelease(context);


         delete [](unsigned char *) data;
        res.s = texture;
         return res;
     };
     ~CTTextRect(){
         if(frame != nullptr){
             CFRelease(frame);
             frame = nullptr;
         }
         if(framesetterRef != nullptr){
             CFRelease(framesetterRef);
             framesetterRef = nullptr;
         }
     };
 };

 Core::SharedPtr<TextRect> TextRect::Create(Composition::Rect rect,const TextLayoutDescriptor & layoutDesc, float renderScale){
     // TODO: DPI plumbing — see wtk/docs/DPI-Aware-Text-Plan.md. Currently
     // ignores renderScale; CoreText path needs an offscreen bitmap sized to
     // physical pixels plus a backingScaleFactor on the CGContext.
     (void)renderScale;
     return Core::SharedPtr<TextRect>(new CTTextRect(rect,layoutDesc));
 };

  FontEngine * FontEngine::instance;
class CTFontEngine : public FontEngine {
public:
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

            // Quantize float → uint8, straight through. msdfgen emits a
            // Y-up tile; the upload flip in `GlyphAtlas` reconciles that
            // with the GTE sampler's row-0-is-top convention.
            out.pxW = tileW;
            out.pxH = tileH;
            out.rgb.resize(static_cast<std::size_t>(tileW) * tileH * 3);
            for(unsigned y = 0; y < tileH; ++y){
                for(unsigned x = 0; x < tileW; ++x){
                    const float *px = msdf((int)x, (int)y);
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

            // Metrics in pixel space (the unscaled CTFontRef is sized in
            // points == logical pixels). `advance.x` from Core Text.
            CGSize advance {};
            CTFontGetAdvancesForGlyphs(capturedFont,kCTFontOrientationHorizontal,&glyph,&advance,1);
            out.metrics.advance = static_cast<float>(advance.width);

            // MSDF tile placement (Phase 6.7-c3). `l`/`b` are the padded
            // bbox origin in font-pixel space; `scale` is tile-pixels
            // per font-pixel. `inkPx*` is the *exact* (un-ceiled)
            // content size in tile pixels — the render path and atlas
            // address this rather than the ceil'd `tileW/tileH` so the
            // ceil sliver can't displace the glyph.
            out.metrics.tileOriginX = static_cast<float>(l);
            out.metrics.tileOriginY = static_cast<float>(b);
            out.metrics.tileScale   = static_cast<float>(scale);
            out.metrics.inkPxW      = static_cast<float>((r - l) * scale);
            out.metrics.inkPxH      = static_cast<float>((t - b) * scale);

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
};

FontEngine * FontEngine::inst(){
    return instance;
};

 void FontEngine::Create(){
        instance = new CTFontEngine();
     };
      void FontEngine::Destroy(){
         delete instance;
     };

};
