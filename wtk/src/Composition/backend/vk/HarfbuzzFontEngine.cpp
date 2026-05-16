#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "../GlyphAtlas.h"

#include "omega-common/fs.h"

#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <fontconfig/fontconfig.h>
#include <cairo/cairo.h>
#include <gdk/gdk.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SIZES_H

// Text-Layout-Engine-Plan.md Phase 2: HarfBuzz directly (no Pango
// shaping). We use the hb-ft bridge so the same FT_Face drives both
// MSDF outline extraction and HB shaping at one fixed pixel size.
#include <hb.h>
#include <hb-ft.h>

// Phase 3: `ShaperInput::script` carries an ICU `UScriptCode` value.
// `uscript_getShortName` converts it to a 4-char ISO 15924 tag
// HarfBuzz can map via `hb_script_from_iso15924_tag`.
#include <unicode/uscript.h>

#ifdef OMEGAWTK_HAVE_MSDFGEN
#include <msdfgen.h>
#include <core/edge-coloring.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace OmegaWTK::Composition {

    // Interim scale source — matches the formula GTKAppWindow::scaleFactor()
    // returns: dpiScale (Xft.dpi / GNOME text-scaling-factor, derived via
    // gdk_screen_get_resolution) × integerScale (GDK device scale).
    //
    // TODO: Per DPI-Aware-Text-Plan.md, this should be sourced from the
    // Compositor (which reads it from the per-window NativeWindow::scaleFactor
    // via View::getRenderScale → ViewRenderTarget::renderScale_). Doing so
    // requires VKLayerTree to call view->setRenderScale(nativeWindow->
    // scaleFactor()) at construction (mirroring DCVisualTree on Win32) and
    // then HarfBuzzTextRect honoring the renderScale arg already passed to
    // TextRect::Create. Until that wiring lands, we read GDK directly here so
    // the value matches GTKAppWindow's window sizing.
    static double getScreenScaleFactor(){
        GdkDisplay *display = gdk_display_get_default();
        if(display == nullptr){
            return 1.0;
        }
        double dpiScale = 1.0;
        GdkScreen *screen = gdk_display_get_default_screen(display);
        if(screen != nullptr){
            gdouble dpi = gdk_screen_get_resolution(screen);
            if(dpi > 0.0 && std::isfinite(dpi)){
                dpiScale = dpi / 96.0;
                if(dpiScale < 0.5){
                    dpiScale = 0.5;
                }
            }
        }
        int integerScale = 1;
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if(monitor == nullptr){
            monitor = gdk_display_get_monitor(display,0);
        }
        if(monitor != nullptr){
            integerScale = gdk_monitor_get_scale_factor(monitor);
            if(integerScale < 1){
                integerScale = 1;
            }
        }
        return dpiScale * (double)integerScale;
    }

    static PangoAlignment toPangoAlignment(TextLayoutDescriptor::Alignment alignment){
        switch(alignment){
            case TextLayoutDescriptor::LeftUpper:
            case TextLayoutDescriptor::LeftCenter:
            case TextLayoutDescriptor::LeftLower:
                return PANGO_ALIGN_LEFT;
            case TextLayoutDescriptor::MiddleUpper:
            case TextLayoutDescriptor::MiddleCenter:
            case TextLayoutDescriptor::MiddleLower:
                return PANGO_ALIGN_CENTER;
            case TextLayoutDescriptor::RightUpper:
            case TextLayoutDescriptor::RightCenter:
            case TextLayoutDescriptor::RightLower:
                return PANGO_ALIGN_RIGHT;
            default:
                return PANGO_ALIGN_LEFT;
        }
    }

    static PangoWrapMode toPangoWrapMode(TextLayoutDescriptor::Wrapping wrapping){
        switch(wrapping){
            case TextLayoutDescriptor::WrapByWord:
                return PANGO_WRAP_WORD;
            case TextLayoutDescriptor::WrapByCharacter:
                return PANGO_WRAP_CHAR;
            case TextLayoutDescriptor::None:
            default:
                return PANGO_WRAP_WORD;
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

        /// MSDF glyph tile size (Phase 6.7.1). Square 32×32 cells leave
        /// a comfortable distance range (4 px) for Latin text at typical
        /// UI sizes. Tunable per-font in a follow-up; constant for now.
        constexpr int kMsdfTileSize  = 32;
        constexpr double kMsdfRange  = 4.0;

        /// Convert a 26.6 fixed-point FT coord to a font-unit double.
        /// FT outlines are reported in font units of the loaded glyph;
        /// `FT_Set_Pixel_Sizes(face, 0, 32)` scales the outline so that
        /// dividing by 64 gives pixel-space coordinates.
        inline double ftToDouble(FT_Pos v) {
            return static_cast<double>(v) / 64.0;
        }

#ifdef OMEGAWTK_HAVE_MSDFGEN
        /// `FT_Outline_Decompose` callback context: builds a msdfgen
        /// `Shape` one contour at a time.
        struct FtMsdfContext {
            msdfgen::Shape *shape    = nullptr;
            msdfgen::Contour *contour = nullptr;
            msdfgen::Point2 lastPoint {0.0, 0.0};
        };

        int ftMoveTo(const FT_Vector *to, void *user) {
            auto *ctx = static_cast<FtMsdfContext *>(user);
            ctx->contour = &ctx->shape->addContour();
            ctx->lastPoint = msdfgen::Point2(ftToDouble(to->x), ftToDouble(to->y));
            return 0;
        }

        int ftLineTo(const FT_Vector *to, void *user) {
            auto *ctx = static_cast<FtMsdfContext *>(user);
            if(ctx->contour == nullptr) return 0;
            msdfgen::Point2 endpoint(ftToDouble(to->x), ftToDouble(to->y));
            if(endpoint != ctx->lastPoint){
                ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, endpoint));
                ctx->lastPoint = endpoint;
            }
            return 0;
        }

        int ftConicTo(const FT_Vector *control, const FT_Vector *to, void *user) {
            auto *ctx = static_cast<FtMsdfContext *>(user);
            if(ctx->contour == nullptr) return 0;
            msdfgen::Point2 ctrl(ftToDouble(control->x), ftToDouble(control->y));
            msdfgen::Point2 endpoint(ftToDouble(to->x), ftToDouble(to->y));
            ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, ctrl, endpoint));
            ctx->lastPoint = endpoint;
            return 0;
        }

        int ftCubicTo(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user) {
            auto *ctx = static_cast<FtMsdfContext *>(user);
            if(ctx->contour == nullptr) return 0;
            msdfgen::Point2 ctrl1(ftToDouble(c1->x), ftToDouble(c1->y));
            msdfgen::Point2 ctrl2(ftToDouble(c2->x), ftToDouble(c2->y));
            msdfgen::Point2 endpoint(ftToDouble(to->x), ftToDouble(to->y));
            ctx->contour->addEdge(msdfgen::EdgeHolder(ctx->lastPoint, ctrl1, ctrl2, endpoint));
            ctx->lastPoint = endpoint;
            return 0;
        }
#endif // OMEGAWTK_HAVE_MSDFGEN
    }

    /// Returns the vertical alignment category from a TextLayoutDescriptor::Alignment.
    /// 0 = upper, 1 = center, 2 = lower
    static int verticalAlignmentCategory(TextLayoutDescriptor::Alignment alignment){
        switch(alignment){
            case TextLayoutDescriptor::LeftUpper:
            case TextLayoutDescriptor::MiddleUpper:
            case TextLayoutDescriptor::RightUpper:
                return 0;
            case TextLayoutDescriptor::LeftCenter:
            case TextLayoutDescriptor::MiddleCenter:
            case TextLayoutDescriptor::RightCenter:
                return 1;
            case TextLayoutDescriptor::LeftLower:
            case TextLayoutDescriptor::MiddleLower:
            case TextLayoutDescriptor::RightLower:
                return 2;
            default:
                return 0;
        }
    }

    class HarfBuzzFont : public Font {
        PangoFontDescription *fontDesc;
        /// Resolved PangoFont (may be null when font construction
        /// couldn't load a face, e.g. requested family unavailable).
        /// Owned via g_object_ref / g_object_unref. Held so the
        /// rasterize lambda can re-lock the FT_Face on demand without
        /// reloading the description through Pango each time.
        PangoFont *resolvedFont = nullptr;

        // Text-Layout-Engine-Plan.md Phase 2: FreeType face + HarfBuzz
        // font opened directly via FontConfig (no PangoFc descent).
        // Owned per-font; sized once to `desc.size` and never resized
        // afterward, so callers can share the FT_Face without worrying
        // about cross-talk between MSDF rasterization and HB shaping.
        // Null when the engine couldn't materialize the face — the
        // font then falls back to the legacy Pango/Cairo bitmap path
        // (which still works through `resolvedFont`).
        FT_Face ftFace_ = nullptr;
        hb_font_t *hbFont_ = nullptr;
    public:
        explicit HarfBuzzFont(FontDescriptor &desc, PangoFontDescription *fontDesc, PangoFont *resolved)
            :Font(desc), fontDesc(fontDesc), resolvedFont(resolved){
            if(resolvedFont != nullptr){
                g_object_ref(resolvedFont);
            }
        }

        void * getNativeFont() override {
            return fontDesc;
        }

        PangoFont * getResolvedPangoFont() const { return resolvedFont; }

        // Direct FT/HB accessors (Phase 2). MSDF rasterization uses
        // `ftFace`; HB shaping uses `hbFont`. May return null if
        // FontConfig / FreeType couldn't open the face; callers stay
        // on the bitmap path.
        FT_Face ftFace() const { return ftFace_; }
        hb_font_t * hbFont() const { return hbFont_; }
        void setFTHandles(FT_Face face, hb_font_t *hb){
            ftFace_ = face;
            hbFont_ = hb;
        }

        FontMetrics getMetrics() const override {
            FontMetrics m;
            if(ftFace_ != nullptr && ftFace_->size != nullptr){
                // FreeType returns metrics in 26.6 fixed-point pixels
                // after `FT_Set_Pixel_Sizes`. ascender is positive,
                // descender is *negative* in FT's convention; flip
                // to match WTK's positive-descent contract.
                const auto &m26 = ftFace_->size->metrics;
                m.ascent  = static_cast<float>(m26.ascender)  / 64.f;
                m.descent = static_cast<float>(-m26.descender) / 64.f;
                m.lineGap = static_cast<float>(m26.height - m26.ascender + m26.descender) / 64.f;
                if(m.lineGap < 0.f) m.lineGap = 0.f;
            }
            return m;
        }

        // Expose the protected mode setter to the engine factory so it
        // can promote a font to MSDF after running the outline probe.
        using Font::setMode;

        ~HarfBuzzFont() override {
            if(hbFont_ != nullptr){
                hb_font_destroy(hbFont_);
            }
            if(ftFace_ != nullptr){
                FT_Done_Face(ftFace_);
            }
            if(resolvedFont != nullptr){
                g_object_unref(resolvedFont);
            }
            if(fontDesc != nullptr){
                pango_font_description_free(fontDesc);
            }
        }
    };

    class HarfBuzzGlyphRun : public GlyphRun {
    public:
        OmegaCommon::UniString str;
        Core::SharedPtr<HarfBuzzFont> font;

        HarfBuzzGlyphRun(const OmegaCommon::UniString &str, Core::SharedPtr<Font> &font)
            :str(str), font(std::dynamic_pointer_cast<HarfBuzzFont>(font)){
        }

        Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
            (void)glyphIdx;
            return {};
        }

        // Phase 6.7-c4: lift shaping out of `drawRun`. Builds a
        // PangoLayout at the font's *unscaled* design size (DPR is
        // applied downstream by the render viewport, keeping the MSDF
        // atlas resolution-independent), walks the layout's runs, and
        // groups them by resolved `PangoFont *` — one `TextSubRun` per
        // resolved face. The requested face occupies sub-run 0 when it
        // appears; fallback faces are adopted via
        // `FontEngine::adoptResolvedFace` so they share one `Font` and
        // one `GlyphAtlas` per native handle across the process. If
        // any resolved face ends up in `BitmapFallback` mode (e.g.
        // a non-scalable color-emoji face), the whole string is
        // routed to the legacy bitmap path — per-glyph bitmap caching
        // for fallback sub-runs is a Phase-6.7 follow-up.
        GlyphRun::ShapedTextRun shape(const Composition::Rect &rect,
                                      const TextLayoutDescriptor &layoutDesc) override {
            GlyphRun::ShapedTextRun result;
            if(font == nullptr){
                result.requiresFallback = true;
                return result;
            }
            PangoFont *ourFont = font->getResolvedPangoFont();
            auto *srcDesc = (PangoFontDescription *)font->getNativeFont();
            if(ourFont == nullptr || srcDesc == nullptr){
                result.requiresFallback = true;
                return result;
            }

            PangoFontMap *fontMap = pango_cairo_font_map_get_default();
            if(fontMap == nullptr){
                result.requiresFallback = true;
                return result;
            }
            PangoContext *ctx = pango_font_map_create_context(fontMap);
            if(ctx == nullptr){
                result.requiresFallback = true;
                return result;
            }
            PangoLayout *layout = pango_layout_new(ctx);

            const OmegaCommon::UnicodeChar *u16Buf = str.getBuffer();
            int32_t u16Len = str.length();
            if(u16Buf != nullptr && u16Len > 0){
                GError *err = nullptr;
                gchar *utf8 = g_utf16_to_utf8(
                    (const gunichar2 *)u16Buf,
                    (glong)u16Len,
                    nullptr, nullptr, &err);
                if(utf8 != nullptr){
                    pango_layout_set_text(layout, utf8, -1);
                    g_free(utf8);
                }
                if(err != nullptr){
                    g_error_free(err);
                }
            }

            // Lay out at an *absolute* pixel size equal to the design
            // size. `srcDesc` carries a point size (`set_size`), which
            // Pango scales to pixels through the font map's DPI
            // resolution — but the MSDF raselizer sizes the FT face
            // with `FT_Set_Pixel_Sizes(face, 0, descSize)`, i.e. raw
            // pixels with no DPI conversion. Using the point size here
            // would space glyphs ~DPI/72 wider than their rasterized
            // tiles ("bouncing" text). `set_absolute_size` pins the
            // layout to the same pixel space as the atlas tiles; the
            // display DPR is applied downstream by the render viewport.
            PangoFontDescription *layoutFontDesc = pango_font_description_copy(srcDesc);
            pango_font_description_set_absolute_size(
                layoutFontDesc,
                (double)font->desc.size * (double)PANGO_SCALE);
            pango_layout_set_font_description(layout, layoutFontDesc);
            pango_font_description_free(layoutFontDesc);

            if(layoutDesc.wrapping != TextLayoutDescriptor::None){
                pango_layout_set_wrap(layout, toPangoWrapMode(layoutDesc.wrapping));
                pango_layout_set_width(layout, (int)(rect.w * PANGO_SCALE));
            } else {
                pango_layout_set_width(layout, -1);
            }
            pango_layout_set_alignment(layout, toPangoAlignment(layoutDesc.alignment));
            if(layoutDesc.lineLimit > 0){
                pango_layout_set_height(layout, -(int)layoutDesc.lineLimit);
            }

            // Vertical alignment offset, mirroring the bitmap path.
            double yOffset = 0.0;
            int vAlign = verticalAlignmentCategory(layoutDesc.alignment);
            if(vAlign != 0){
                int layoutW = 0, layoutH = 0;
                pango_layout_get_pixel_size(layout, &layoutW, &layoutH);
                if(vAlign == 1){
                    yOffset = ((double)rect.h - layoutH) / 2.0;
                } else {
                    yOffset = (double)rect.h - layoutH;
                }
                if(yOffset < 0.0) yOffset = 0.0;
            }

            // Resolved family of the requested face — used as the
            // identity fallback when `runFont` pointer-compares differ
            // (Pango may resolve the *same* logical face through a
            // different `PangoFont *` than the engine factory loaded
            // — e.g. when fontset iteration picks the first matching
            // entry rather than the cached singleton).
            PangoFontDescription *ourResolvedDesc = pango_font_describe(ourFont);
            const char *ourFamily = (ourResolvedDesc != nullptr)
                ? pango_font_description_get_family(ourResolvedDesc)
                : nullptr;

            // Group runs by resolved PangoFont*. The first entry is
            // always the requested face when it appears (we seed it
            // lazily on the first matching run). `subRunIndex` maps
            // resolved pointer → index in `result.subRuns`.
            std::unordered_map<PangoFont *, std::size_t> subRunIndex;
            bool fallback = false;

            auto getOrCreateSubRun =
                [&](PangoFont *runFont) -> std::size_t {
                    auto it = subRunIndex.find(runFont);
                    if(it != subRunIndex.end()) return it->second;

                    Core::SharedPtr<Font> resolvedFontPtr;
                    bool isPrimary = (runFont == ourFont);
                    if(!isPrimary && ourFamily != nullptr){
                        // Family-name fallback (same logical face,
                        // different Pango pointer). Treat as primary.
                        PangoFontDescription *runDesc = pango_font_describe(runFont);
                        if(runDesc != nullptr){
                            const char *runFamily = pango_font_description_get_family(runDesc);
                            if(runFamily != nullptr &&
                               g_ascii_strcasecmp(ourFamily, runFamily) == 0){
                                isPrimary = true;
                            }
                            pango_font_description_free(runDesc);
                        }
                    }

                    if(isPrimary){
                        resolvedFontPtr = std::static_pointer_cast<Font>(font);
                    } else if(FontEngine::inst() != nullptr){
                        resolvedFontPtr = FontEngine::inst()->adoptResolvedFace(runFont);
                    }
                    if(resolvedFontPtr == nullptr){
                        return SIZE_MAX;
                    }
                    TextSubRun sr;
                    sr.resolvedFont = resolvedFontPtr;
                    result.subRuns.push_back(std::move(sr));
                    std::size_t idx = result.subRuns.size() - 1;
                    subRunIndex[runFont] = idx;
                    return idx;
                };

            PangoLayoutIter *iter = pango_layout_get_iter(layout);
            if(iter != nullptr){
                do {
                    PangoLayoutRun *run = pango_layout_iter_get_run_readonly(iter);
                    if(run == nullptr){
                        // NULL run = end of a line; keep iterating.
                        continue;
                    }
                    PangoFont *runFont = run->item->analysis.font;
                    if(runFont == nullptr){
                        fallback = true;
                        break;
                    }

                    std::size_t srIdx = getOrCreateSubRun(runFont);
                    if(srIdx == SIZE_MAX){
                        fallback = true;
                        break;
                    }

                    PangoRectangle logical {};
                    pango_layout_iter_get_run_extents(iter, nullptr, &logical);
                    int baseline = pango_layout_iter_get_baseline(iter);
                    double penX = (double)logical.x;
                    const double baseY = (double)baseline;

                    TextSubRun &sr = result.subRuns[srIdx];
                    PangoGlyphString *glyphs = run->glyphs;
                    for(int i = 0; i < glyphs->num_glyphs; ++i){
                        const PangoGlyphInfo &gi = glyphs->glyphs[i];
                        PangoGlyph glyph = gi.glyph;
                        if(glyph != PANGO_GLYPH_EMPTY &&
                           !(glyph & PANGO_GLYPH_UNKNOWN_FLAG)){
                            const double gx = penX + (double)gi.geometry.x_offset;
                            const double gy = baseY + (double)gi.geometry.y_offset;
                            sr.glyphIds.push_back((std::uint32_t)glyph);
                            sr.positions.push_back(Composition::Point2D{
                                (float)(gx / (double)PANGO_SCALE),
                                (float)(gy / (double)PANGO_SCALE + yOffset)});
                        }
                        penX += (double)gi.geometry.width;
                    }
                } while(pango_layout_iter_next_run(iter));
                pango_layout_iter_free(iter);
            }

            if(ourResolvedDesc != nullptr){
                pango_font_description_free(ourResolvedDesc);
            }
            g_object_unref(layout);
            g_object_unref(ctx);

            // Whole-string bitmap-fallback gate: if any resolved face
            // ended up in `BitmapFallback` mode the MSDF render path
            // can't service the string end-to-end. Per the chunk-4
            // decision (per-glyph bitmap caching deferred), bail to
            // the legacy `TextRect` path for the whole string.
            if(!fallback){
                for(const auto &sr : result.subRuns){
                    if(sr.resolvedFont == nullptr ||
                       sr.resolvedFont->mode() != Font::Mode::MSDF){
                        fallback = true;
                        break;
                    }
                }
            }

            if(fallback){
                result.requiresFallback = true;
                result.subRuns.clear();
            }

            if(textTraceEnabled()){
                std::size_t totalGlyphs = 0;
                for(const auto &sr : result.subRuns){
                    totalGlyphs += sr.glyphIds.size();
                }
                std::cout << "[wtk-text] HarfBuzzGlyphRun::shape -> "
                          << (result.requiresFallback ? "FALLBACK" : "MSDF")
                          << ", subRuns=" << result.subRuns.size()
                          << ", glyphs=" << totalGlyphs << std::endl;
                for(std::size_t s = 0; s < result.subRuns.size(); ++s){
                    const auto &sr = result.subRuns[s];
                    std::cout << "[wtk-text]   subRun[" << s << "] family='"
                              << (sr.resolvedFont ? sr.resolvedFont->desc.family : std::string("<null>"))
                              << "' glyphs=" << sr.glyphIds.size() << std::endl;
                    for(std::size_t k = 0; k < sr.glyphIds.size() && k < 6; ++k){
                        std::cout << "[wtk-text]     gid=" << sr.glyphIds[k]
                                  << " pos=(" << sr.positions[k].x << ","
                                  << sr.positions[k].y << ")" << std::endl;
                    }
                }
            }
            return result;
        }
    };

    class HarfBuzzTextRect : public TextRect {
        TextLayoutDescriptor layoutDesc;
        unsigned char *pixelData = nullptr;
        size_t pixelWidth = 0;
        size_t pixelHeight = 0;
        size_t bytesPerRow = 0;
    public:
        explicit HarfBuzzTextRect(Composition::Rect rect, const TextLayoutDescriptor &layoutDesc)
            :TextRect(rect), layoutDesc(layoutDesc){
        }

        void drawRun(Core::SharedPtr<GlyphRun> &glyphRun, const Composition::Color &color) override {
            auto gr = std::dynamic_pointer_cast<HarfBuzzGlyphRun>(glyphRun);
            if(gr == nullptr){
                return;
            }

            double scaleFactor = getScreenScaleFactor();
            pixelWidth = (size_t)(rect.w * scaleFactor);
            pixelHeight = (size_t)(rect.h * scaleFactor);
            bytesPerRow = pixelWidth * 4;
            size_t byteCount = bytesPerRow * pixelHeight;

            delete[] pixelData;
            pixelData = new unsigned char[byteCount];
            std::memset(pixelData, 0, byteCount);

            cairo_surface_t *surface = cairo_image_surface_create_for_data(
                pixelData,
                CAIRO_FORMAT_ARGB32,
                (int)pixelWidth,
                (int)pixelHeight,
                (int)bytesPerRow);

            cairo_t *cr = cairo_create(surface);
            cairo_set_antialias(cr, CAIRO_ANTIALIAS_SUBPIXEL);

            PangoLayout *layout = pango_cairo_create_layout(cr);

            // Convert UTF-16 string to UTF-8 for Pango
            const OmegaCommon::UnicodeChar *u16Buf = gr->str.getBuffer();
            int32_t u16Len = gr->str.length();
            if(u16Buf != nullptr && u16Len > 0){
                GError *err = nullptr;
                gchar *utf8 = g_utf16_to_utf8(
                    (const gunichar2 *)u16Buf,
                    (glong)u16Len,
                    nullptr, nullptr, &err);
                if(utf8 != nullptr){
                    pango_layout_set_text(layout, utf8, -1);
                    g_free(utf8);
                }
                if(err != nullptr){
                    g_error_free(err);
                }
            }

            // Apply font
            if(gr->font != nullptr){
                auto *srcDesc = (PangoFontDescription *)gr->font->getNativeFont();
                if(srcDesc != nullptr){
                    PangoFontDescription *scaled = pango_font_description_copy(srcDesc);
                    int origSize = pango_font_description_get_size(scaled);
                    pango_font_description_set_size(scaled, (int)(origSize * scaleFactor));
                    pango_layout_set_font_description(layout, scaled);
                    pango_font_description_free(scaled);
                }
            }

            // Layout parameters
            if(layoutDesc.wrapping != TextLayoutDescriptor::None){
                pango_layout_set_wrap(layout, toPangoWrapMode(layoutDesc.wrapping));
                pango_layout_set_width(layout, (int)(pixelWidth * PANGO_SCALE));
            } else {
                pango_layout_set_width(layout, -1);
            }

            pango_layout_set_alignment(layout, toPangoAlignment(layoutDesc.alignment));

            if(layoutDesc.lineLimit > 0){
                pango_layout_set_height(layout, -(int)layoutDesc.lineLimit);
            }

            // Compute vertical offset for center/lower alignment
            double yOffset = 0.0;
            int vAlign = verticalAlignmentCategory(layoutDesc.alignment);
            if(vAlign != 0){
                int layoutW, layoutH;
                pango_layout_get_pixel_size(layout, &layoutW, &layoutH);
                if(vAlign == 1){
                    yOffset = ((double)pixelHeight - layoutH) / 2.0;
                } else {
                    yOffset = (double)pixelHeight - layoutH;
                }
                if(yOffset < 0.0) yOffset = 0.0;
            }

            // Draw
            cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
            cairo_move_to(cr, 0.0, yOffset);
            pango_cairo_show_layout(cr, layout);
            cairo_surface_flush(surface);

            g_object_unref(layout);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }

        BitmapRes toBitmap() override {
            BitmapRes res {nullptr, nullptr};
            if(pixelData == nullptr || pixelWidth == 0 || pixelHeight == 0){
                return res;
            }

            OmegaGTE::TextureDescriptor desc {};
            desc.usage = OmegaGTE::GETexture::ToGPU;
            desc.storage_opts = OmegaGTE::Shared;
            desc.pixelFormat = OmegaGTE::TexturePixelFormat::BGRA8Unorm;
            // desc.defaultSwizzle = OmegaGTE::TextureSwizzle::swapRB();
            desc.kind = OmegaGTE::TextureKind::Tex2D;
            desc.width = (unsigned)pixelWidth;
            desc.height = (unsigned)pixelHeight;

            auto texture = gte.graphicsEngine->makeTexture(desc);
            if(texture == nullptr){
                // Surface a recognizable failure to the caller rather than
                // segfaulting on the upcoming `copyBytes` virtual dispatch.
                // The underlying error has already been logged by the GTE
                // backend; we just propagate the empty BitmapRes.
                return res;
            }
            texture->copyBytes(pixelData, bytesPerRow);

            res.s = texture;
            return res;
        }

        void *getNative() override {
            return nullptr;
        }

        ~HarfBuzzTextRect() override {
            delete[] pixelData;
        }
    };

    FontEngine *FontEngine::instance = nullptr;

    // Phase-2 HarfBuzz-backed shaper. Stateless modulo the
    // hb_buffer_t it reuses across calls; the per-shape font handle
    // is pulled from the `ShaperInput`'s `Font` via its concrete
    // `HarfBuzzFont` subclass.
    class HarfBuzzShaper : public ITextShaper {
        hb_buffer_t *buffer_ = nullptr;
    public:
        HarfBuzzShaper(){
            buffer_ = hb_buffer_create();
        }
        ~HarfBuzzShaper() override {
            if(buffer_ != nullptr){
                hb_buffer_destroy(buffer_);
            }
        }
        HarfBuzzShaper(const HarfBuzzShaper &) = delete;
        HarfBuzzShaper & operator=(const HarfBuzzShaper &) = delete;

        OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override {
            OmegaCommon::Vector<ShaperGlyph> out;
            if(buffer_ == nullptr || input.font == nullptr || input.text.length() == 0){
                return out;
            }
            auto fontHb = std::dynamic_pointer_cast<HarfBuzzFont>(input.font);
            if(fontHb == nullptr || fontHb->hbFont() == nullptr){
                return out;
            }

            hb_buffer_reset(buffer_);
            hb_buffer_add_utf16(buffer_,
                reinterpret_cast<const uint16_t *>(input.text.getBuffer()),
                input.text.length(),
                0, input.text.length());
            hb_buffer_set_direction(buffer_,
                input.rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);

            // Phase 3: set the HB script explicitly when the layout
            // engine pre-segmented by script. Falls back to HB's
            // content-based guess when `script == USCRIPT_COMMON`
            // (Phase 1 / 2 callers that don't segment).
            if(input.script != USCRIPT_COMMON){
                const char *iso15924 = uscript_getShortName(
                    static_cast<UScriptCode>(input.script));
                if(iso15924 != nullptr){
                    hb_buffer_set_script(buffer_,
                        hb_script_from_iso15924_tag(
                            hb_tag_from_string(iso15924, -1)));
                }
            } else {
                hb_buffer_guess_segment_properties(buffer_);
            }

            hb_shape(fontHb->hbFont(), buffer_, nullptr, 0);

            unsigned int n = 0;
            const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer_, &n);
            const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer_, &n);
            if(infos == nullptr || positions == nullptr){
                return out;
            }

            out.reserve(static_cast<std::size_t>(n));
            // HarfBuzz returns advances/offsets in 26.6 fixed-point
            // units after `hb_ft_font_create` (matches FT's pixel
            // units, since the FT_Face was sized via
            // FT_Set_Pixel_Sizes). Convert with /64.f.
            for(unsigned int i = 0; i < n; ++i){
                ShaperGlyph g;
                g.glyphId  = static_cast<std::uint32_t>(infos[i].codepoint);
                g.advance  = static_cast<float>(positions[i].x_advance) / 64.f;
                g.xOffset  = static_cast<float>(positions[i].x_offset)  / 64.f;
                // HarfBuzz's y_offset is positive-up (toward the
                // ascender). The layout engine convention is Y-down
                // baseline distance; flip sign so a positive HB
                // offset moves the glyph upward on the canvas (i.e.
                // toward smaller Y).
                g.yOffset  = -static_cast<float>(positions[i].y_offset) / 64.f;
                // Phase 3.5: HarfBuzz's `cluster` is the source UTF-16
                // offset of the cluster start within the shape input
                // (we call `hb_buffer_add_utf16` with a single source
                // range covering the whole input). The wrap pass maps
                // this back to break-iterator boundaries.
                g.cluster  = static_cast<std::int32_t>(infos[i].cluster);
                out.push_back(g);
            }
            return out;
        }
    };

    class HarfBuzzFontEngine;

    // Text-Layout-Engine-Plan.md Phase 4 — FontConfig-backed
    // `IFontFallback`.
    //
    // Asks FontConfig "what installed font covers this codepoint?"
    // by building an `FcCharSet` with the codepoint and running the
    // standard substitute → match pipeline. The matched family name
    // is then handed to `HarfBuzzFontEngine::CreateFont`, which
    // already knows how to open an FT_Face / hb_font_t and run the
    // MSDF probe. The returned `Font` flows back through the layout
    // engine with its own `GlyphAtlas`, so e.g. every CJK ideograph
    // in a Latin-primary string shares one substitute face and one
    // shared atlas across the process.
    class FontConfigFontFallback : public IFontFallback {
        HarfBuzzFontEngine *engine_ = nullptr;
        // Cache keyed by (resolved family name + size + style). The
        // family-name string is the lookup-stable identifier for a
        // FontConfig match — repeated lookups for the same codepoint
        // returning the same face name hit this map and reuse one
        // `Font` instance.
        std::unordered_map<std::string, Core::SharedPtr<Font>> byKey_;
    public:
        explicit FontConfigFontFallback(HarfBuzzFontEngine *engine)
            : engine_(engine) {}

        Core::SharedPtr<Font> fallbackForCodepoint(
            Core::SharedPtr<Font> requested,
            std::uint32_t codepoint) override;
    };

    class HarfBuzzFontEngine : public FontEngine {
        /// Process-lifetime cache of fonts adopted from layout-engine
        /// resolved faces (Phase 6.7-c4). Keyed by raw `PangoFont *`
        /// — the cache holds a `g_object_ref` on the key via the
        /// adopted `HarfBuzzFont`'s ctor, so the pointer stays valid
        /// for the lifetime of the cache entry. Repeated fallback to
        /// the same substitute face returns the same `Font`, sharing
        /// its `GlyphAtlas` across the process.
        std::unordered_map<PangoFont *, Core::SharedPtr<Font>> adoptedCache_;

        // Phase-2 direct FreeType / HarfBuzz handles. One FT_Library
        // shared across all fonts; FT_Faces are owned per-Font and
        // closed by `~HarfBuzzFont` before this engine is torn down,
        // so we can safely destroy the library in the engine dtor.
        FT_Library ftLibrary_ = nullptr;
        // One reusable shaper instance — `HarfBuzzShaper` carries an
        // internal `hb_buffer_t` for shaping reuse.
        HarfBuzzShaper shaper_;
        // Phase 4: FontConfig-driven fallback driver.
        FontConfigFontFallback fallback_{this};
    public:
        HarfBuzzFontEngine() {
            if(FT_Init_FreeType(&ftLibrary_) != 0){
                ftLibrary_ = nullptr;
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] HarfBuzzFontEngine: FT_Init_FreeType failed; "
                              << "all fonts will use BitmapFallback." << std::endl;
                }
            }
            // Ensure FontConfig has been initialized before our first
            // pattern match. `FcInit()` is idempotent and returns
            // true if already initialized — cheap to call.
            FcInit();
        }
        ~HarfBuzzFontEngine() override {
            if(ftLibrary_ != nullptr){
                FT_Done_FreeType(ftLibrary_);
            }
        }

        ITextShaper *   shaper()   override { return &shaper_;   }
        IFontFallback * fallback() override { return &fallback_; }

        // Phase-2 helper. Open an FT_Face + hb_font_t for `desc` via
        // FontConfig + FreeType directly (no Pango lock dance). Sets
        // the face's pixel size to `desc.size` once; both MSDF
        // rasterization and HarfBuzz shaping share that state.
        // Returns true on success and writes the handles to `outFace`
        // / `outHB`; on failure leaves them null and the font stays
        // on the legacy Pango/Cairo bitmap path.
        bool openFTAndHB(const FontDescriptor &desc,
                         FT_Face &outFace,
                         hb_font_t *&outHB){
            outFace = nullptr;
            outHB = nullptr;
            if(ftLibrary_ == nullptr){
                return false;
            }
            FcPattern *pattern = FcPatternBuild(nullptr,
                FC_FAMILY, FcTypeString, desc.family.c_str(),
                FC_PIXEL_SIZE, FcTypeDouble, (double)desc.size,
                nullptr);
            if(pattern == nullptr){
                return false;
            }
            // Style: weight + slant.
            const int fcWeight = (desc.style == FontDescriptor::Bold ||
                                  desc.style == FontDescriptor::BoldAndItalic)
                                 ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR;
            const int fcSlant  = (desc.style == FontDescriptor::Italic ||
                                  desc.style == FontDescriptor::BoldAndItalic)
                                 ? FC_SLANT_ITALIC : FC_SLANT_ROMAN;
            FcPatternAddInteger(pattern, FC_WEIGHT, fcWeight);
            FcPatternAddInteger(pattern, FC_SLANT,  fcSlant);

            FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);

            FcResult fcResult;
            FcPattern *matched = FcFontMatch(nullptr, pattern, &fcResult);
            FcPatternDestroy(pattern);
            if(matched == nullptr){
                return false;
            }

            FcChar8 *filePath = nullptr;
            int faceIndex = 0;
            if(FcPatternGetString(matched, FC_FILE, 0, &filePath) != FcResultMatch ||
               filePath == nullptr){
                FcPatternDestroy(matched);
                return false;
            }
            FcPatternGetInteger(matched, FC_INDEX, 0, &faceIndex);

            FT_Face face = nullptr;
            const FT_Error err = FT_New_Face(ftLibrary_,
                reinterpret_cast<const char *>(filePath),
                faceIndex, &face);
            FcPatternDestroy(matched);
            if(err != 0 || face == nullptr){
                return false;
            }
            if(FT_Set_Pixel_Sizes(face, 0, desc.size) != 0){
                FT_Done_Face(face);
                return false;
            }

            hb_font_t *hb = hb_ft_font_create_referenced(face);
            if(hb == nullptr){
                FT_Done_Face(face);
                return false;
            }

            outFace = face;
            outHB = hb;
            return true;
        }

        Core::SharedPtr<Font> CreateFont(FontDescriptor &desc) override {
            PangoFontDescription *fontDesc = pango_font_description_new();
            pango_font_description_set_family(fontDesc, desc.family.c_str());
            pango_font_description_set_size(fontDesc, (gint)desc.size * PANGO_SCALE);

            switch(desc.style){
                case FontDescriptor::Bold:
                    pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
                    break;
                case FontDescriptor::Italic:
                    pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
                    break;
                case FontDescriptor::BoldAndItalic:
                    pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
                    pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
                    break;
                case FontDescriptor::Regular:
                default:
                    break;
            }

            // Resolve the description against the default Cairo font map
            // so we can probe the actual face's outline support and own a
            // PangoFont reference for the lifetime of the WTK Font.
            PangoFont *resolved = nullptr;
            PangoFontMap *fontMap = pango_cairo_font_map_get_default();
            if(fontMap != nullptr){
                PangoContext *ctx = pango_font_map_create_context(fontMap);
                if(ctx != nullptr){
                    resolved = pango_font_map_load_font(fontMap, ctx, fontDesc);
                    g_object_unref(ctx);
                }
            }

            auto font = Core::SharedPtr<HarfBuzzFont>(new HarfBuzzFont(desc, fontDesc, resolved));

            // Phase 2: open an FT_Face + hb_font_t directly via
            // FontConfig — independent of the Pango lock path that
            // the legacy MSDF probe still uses as a fallback. The
            // new layout engine drives shaping through `hb_font_t`,
            // and the rasterize lambda below prefers `Font::ftFace()`
            // when present.
            FT_Face ftFace = nullptr;
            hb_font_t *hbFont = nullptr;
            if(openFTAndHB(desc, ftFace, hbFont)){
                font->setFTHandles(ftFace, hbFont);
            }

            // Probe + install MSDF rasterize callback. Failure of any
            // step leaves the font on Mode::BitmapFallback (the default
            // installed by Font's base ctor).
            probeAndInstallMsdf(*font);

            if(resolved != nullptr){
                // pango_font_map_load_font returns a ref the caller owns;
                // HarfBuzzFont's ctor took its own ref. Release the
                // factory's ref now.
                g_object_unref(resolved);
            }

            return font;
        }

        /// Decide whether `font`'s underlying face supports MSDF outline
        /// extraction; if so, install a RasterizeFn on its atlas and
        /// flip its mode. Otherwise log once (when traced) and leave on
        /// BitmapFallback.
        ///
        /// Phase 2: prefers the directly-opened `Font::ftFace()` (via
        /// FontConfig + FreeType) and skips the Pango lock dance
        /// entirely when it's present. The Pango lock path stays as a
        /// fallback for `adoptResolvedFace` callers (which only have
        /// a `PangoFont *`).
        static void probeAndInstallMsdf(HarfBuzzFont &font) {
#ifdef OMEGAWTK_HAVE_MSDFGEN
            FT_Face directFace = font.ftFace();
            bool scalable = false;
            bool hasColor = false;

            if(directFace != nullptr){
                scalable = (directFace->face_flags & FT_FACE_FLAG_SCALABLE) != 0;
                hasColor = FT_HAS_COLOR(directFace);
            } else {
                // Adopted-face path (Phase 6.7-c4): no direct FT_Face,
                // descend through PangoFc.
                PangoFont *resolved = font.getResolvedPangoFont();
                if(resolved == nullptr || !PANGO_IS_FC_FONT(resolved)){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] HarfBuzzFont: '"
                                  << font.desc.family << "' size=" << font.desc.size
                                  << " no FT face (direct or Pango); using BitmapFallback"
                                  << std::endl;
                    }
                    return;
                }
                PangoFcFont *fcFont = PANGO_FC_FONT(resolved);
                FT_Face probeFace = pango_fc_font_lock_face(fcFont);
                if(probeFace == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] HarfBuzzFont: '"
                                  << font.desc.family << "' size=" << font.desc.size
                                  << " pango_fc_font_lock_face returned null; using BitmapFallback"
                                  << std::endl;
                    }
                    return;
                }
                scalable = (probeFace->face_flags & FT_FACE_FLAG_SCALABLE) != 0;
                hasColor = FT_HAS_COLOR(probeFace);
                pango_fc_font_unlock_face(fcFont);
            }

            // Bitmap-only faces flunk MSDF outright. Color faces with
            // scalable outlines (COLR/CPAL, COLRv1) keep the MSDF path
            // — the alpha mask is correct; the color tables are
            // ignored for now (out-of-scope per Phase 6.7 plan).
            if(!scalable){
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] HarfBuzzFont: '"
                              << font.desc.family << "' size=" << font.desc.size
                              << " (color=" << hasColor << ") not scalable; using BitmapFallback"
                              << std::endl;
                }
                return;
            }

            // The lambda captures both the directly-opened FT_Face
            // (Phase 2 path) and the resolved PangoFont (Phase 6.7-c4
            // adoption path). At call time it prefers the direct
            // face — sized once at Font construction, no lock needed
            // — and falls back to the Pango lock dance for adopted
            // faces that never opened a direct handle.
            FT_Face capturedDirect = directFace;
            PangoFont *capturedResolved = font.getResolvedPangoFont();
            const unsigned descSize = font.desc.size;
            font.atlas().setRasterizeFn(
                    [capturedDirect, capturedResolved, descSize](std::uint32_t glyphId,
                                                                 GlyphAtlas::RasterizedGlyph &out) -> bool {
                FT_Face face = nullptr;
                PangoFcFont *fc = nullptr;
                if(capturedDirect != nullptr){
                    face = capturedDirect;
                } else {
                    if(capturedResolved == nullptr || !PANGO_IS_FC_FONT(capturedResolved)){
                        return false;
                    }
                    fc = PANGO_FC_FONT(capturedResolved);
                    face = pango_fc_font_lock_face(fc);
                    if(face == nullptr){
                        return false;
                    }
                }
                auto unlock = [&](){ if(fc != nullptr) pango_fc_font_unlock_face(fc); };

                // Set the face to the requested point size so the
                // outline metrics map to pixel-space at the same DPI
                // chunk-3 will use to author quads. (Direct faces
                // are already sized at construction; this re-set is
                // idempotent and keeps the adopted-face path
                // correct.)
                if(FT_Set_Pixel_Sizes(face, 0, descSize) != 0){
                    unlock();
                    return false;
                }
                if(FT_Load_Glyph(face, glyphId,
                                 FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0){
                    unlock();
                    return false;
                }

                msdfgen::Shape shape;
                FtMsdfContext fc_ctx;
                fc_ctx.shape = &shape;

                FT_Outline_Funcs callbacks {};
                callbacks.move_to  = ftMoveTo;
                callbacks.line_to  = ftLineTo;
                callbacks.conic_to = ftConicTo;
                callbacks.cubic_to = ftCubicTo;
                callbacks.shift    = 0;
                callbacks.delta    = 0;
                if(FT_Outline_Decompose(&face->glyph->outline, &callbacks, &fc_ctx) != 0){
                    unlock();
                    return false;
                }

                // msdfgen pipeline: normalize → orient contours → edge
                // coloring → generate. `orientContours()` is essential
                // and easy to miss — it resolves the CW/CCW winding
                // ambiguity between font formats (TrueType outer
                // contours are clockwise, CFF/Type1 counter-clockwise).
                // Without it the signed-distance sign is inverted for
                // one whole family of fonts: the glyph silhouette reads
                // as "outside" and the surrounding tile as "inside",
                // which renders as a filled box with the glyph punched
                // out.
                shape.normalize();
                shape.orientContours();
                msdfgen::edgeColoringSimple(shape, 3.0);

                // Compute a tight bounding box of the shape and fit
                // the 32×32 tile around it with a small padding so the
                // distance band stays inside the tile. `Shape::bound`
                // *expands* the passed-in box — it never initializes it
                // — so seeding with zeros would force every glyph's
                // bbox to include the origin (0,0), inflating the box
                // for any glyph with a left bearing or whose ink does
                // not touch the baseline. `getBounds()` seeds correctly.
                const msdfgen::Shape::Bounds bounds = shape.getBounds();
                double l = bounds.l, b = bounds.b, r = bounds.r, t = bounds.t;
                if(r <= l || t <= b){
                    unlock();
                    return false;
                }
                // The tile is sized to the glyph's padded bbox, NOT a
                // fixed square. A fixed square tile + uniform-scale fit
                // leaves a per-glyph empty margin, and every scheme to
                // address that margin (sub-rect UV, etc.) just relocates
                // the seam. With a glyph-sized tile there is no margin:
                // the tile *is* the glyph, the render path's quad is the
                // tile, UV is the whole packed rect. `scale` keeps the
                // larger dimension at `kMsdfTileSize` so atlas density
                // is unchanged.
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
                // glyph; `BitmapSection::reorient(Y_DOWNWARD)` flips
                // the section view (pixel pointer to last row,
                // negated `rowStride`) so straight `section(x, y)`
                // reads now run top-to-bottom. Phase-2.5: collapse the
                // three-stage flip chain to a single canonical
                // orientation — `GlyphAtlas` then uploads straight,
                // and the canvas-top ↔ `v0` UV pairing in
                // `emitTextSubRun` carries the orientation through to
                // the fragment with zero implicit flips.
                msdfgen::BitmapSection<float, 3> section = msdf;
                section.reorient(msdfgen::Y_DOWNWARD);

                // Quantize float → uint8, straight through. No extra
                // +0.5 bias — `generateMSDF` already maps signed
                // distance to [0, 1] with the glyph edge at 0.5.
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

                // Phase-2.5 Skia-style top-anchored metrics. `advance.x`
                // is 26.6 fixed-point pixels after `FT_Set_Pixel_Sizes`.
                out.metrics.advance = static_cast<float>(face->glyph->advance.x) / 64.f;

                // Pen-relative quad placement. `l, b, r, t` are the
                // padded bbox extents in shape coords (Y-up, pen origin
                // at 0). Convert to top-anchored canvas-space metrics:
                //   fLeft   = bbox left = l (positive → right of pen).
                //   fTop    = distance from baseline up to bbox top = t.
                //   fWidth  = bbox width  in font-pixels = r - l.
                //   fHeight = bbox height in font-pixels = t - b.
                // No `scale` round-trip — `fWidth/fHeight` are the exact
                // canvas-pixel dimensions of the quad. The tile-vs-quad
                // ratio (`pxW / fWidth`) is the SDF base-scale factor;
                // it falls out of the UV mapping implicitly when the
                // fragment shader samples the (pxW × pxH) tile across
                // the (fWidth × fHeight) quad.
                out.metrics.fLeft   = static_cast<float>(l);
                out.metrics.fTop    = static_cast<float>(t);
                out.metrics.fWidth  = static_cast<float>(r - l);
                out.metrics.fHeight = static_cast<float>(t - b);

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

                unlock();
                return true;
            });
            font.setMode(Font::Mode::MSDF);

            if(textTraceEnabled()){
                std::cout << "[wtk-text] HarfBuzzFont: '"
                          << font.desc.family << "' size=" << font.desc.size
                          << " (scalable=" << scalable
                          << " color=" << hasColor
                          << ") -> MSDF mode" << std::endl;
            }

            // Smoke-probe: rasterize the glyph for 'A' so any breakage
            // in the FT/msdfgen pipeline surfaces at Font construction
            // time, not on the first drawText call. Phase 2: prefer
            // the direct face for the char-index lookup; only descend
            // through Pango if no direct face is open.
            FT_Face smokeFace = font.ftFace();
            PangoFcFont *smokeFc = nullptr;
            if(smokeFace == nullptr){
                PangoFont *resolved2 = font.getResolvedPangoFont();
                if(resolved2 != nullptr && PANGO_IS_FC_FONT(resolved2)){
                    smokeFc = PANGO_FC_FONT(resolved2);
                    smokeFace = pango_fc_font_lock_face(smokeFc);
                }
            }
            if(smokeFace != nullptr){
                FT_UInt gid = FT_Get_Char_Index(smokeFace, 'A');
                if(smokeFc != nullptr){
                    pango_fc_font_unlock_face(smokeFc);
                }
                if(gid != 0){
                    const bool ok = font.atlas().ensureGlyph(gid);
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] HarfBuzzFont: smoke ensureGlyph('A' = "
                                  << gid << ") -> " << (ok ? "ok" : "FAILED") << std::endl;
                    }
                }
            }
#else
            (void)font;
            if(textTraceEnabled()){
                std::cout << "[wtk-text] HarfBuzzFont: built without OMEGAWTK_HAVE_MSDFGEN; using BitmapFallback"
                          << std::endl;
            }
#endif // OMEGAWTK_HAVE_MSDFGEN
        }

        Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor &desc) override {
            FcConfigAppFontAddFile(FcConfigGetCurrent(),
                (const FcChar8 *)path.str().c_str());

            return CreateFont(desc);
        }

        // Phase 6.7-c4. The shaping pass calls this when the layout
        // engine substituted a fallback face for a run whose resolved
        // `PangoFont *` differs from the requested face. The cache is
        // process-lifetime keyed by raw pointer: two calls with the
        // same resolved face return the same `Font` (and therefore
        // the same `GlyphAtlas`).
        Core::SharedPtr<Font> adoptResolvedFace(void *nativeHandle) override {
            if(nativeHandle == nullptr){
                return nullptr;
            }
            auto *resolved = static_cast<PangoFont *>(nativeHandle);
            auto it = adoptedCache_.find(resolved);
            if(it != adoptedCache_.end()){
                return it->second;
            }

            // Derive a FontDescriptor from the resolved face's own
            // description. Sizes returned by `pango_font_describe` are
            // in PANGO_SCALE units; for faces resolved at absolute
            // pixel size this is pixels*PANGO_SCALE, which is exactly
            // what the rasterize lambda's `descSize` arg
            // (FT_Set_Pixel_Sizes) expects after dividing out
            // PANGO_SCALE.
            PangoFontDescription *resolvedDesc = pango_font_describe(resolved);
            if(resolvedDesc == nullptr){
                return nullptr;
            }
            const char *family = pango_font_description_get_family(resolvedDesc);
            OmegaCommon::String familyStr = (family != nullptr) ? family : "";
            int pgSize = pango_font_description_get_size(resolvedDesc);
            unsigned size = (pgSize > 0) ? (unsigned)(pgSize / PANGO_SCALE) : 0;
            if(size == 0){
                // No usable size: bail. The shape pass will flip
                // `requiresFallback` because `adoptResolvedFace`
                // returned null and route the string to the bitmap
                // path.
                pango_font_description_free(resolvedDesc);
                return nullptr;
            }
            PangoStyle pgStyle = pango_font_description_get_style(resolvedDesc);
            PangoWeight pgWeight = pango_font_description_get_weight(resolvedDesc);
            const bool italic = (pgStyle == PANGO_STYLE_ITALIC ||
                                 pgStyle == PANGO_STYLE_OBLIQUE);
            const bool bold = (pgWeight >= PANGO_WEIGHT_BOLD);
            FontDescriptor::FontStyle style = FontDescriptor::Regular;
            if(bold && italic)      style = FontDescriptor::BoldAndItalic;
            else if(bold)           style = FontDescriptor::Bold;
            else if(italic)         style = FontDescriptor::Italic;

            FontDescriptor desc(familyStr, size, style);

            // `HarfBuzzFont` takes ownership of the description it's
            // handed; give it its own copy so destruction lines up.
            PangoFontDescription *ownedDesc = pango_font_description_copy(resolvedDesc);
            pango_font_description_free(resolvedDesc);

            auto adopted = Core::SharedPtr<HarfBuzzFont>(
                new HarfBuzzFont(desc, ownedDesc, resolved));

            // Same MSDF probe as `CreateFont`. May leave the adopted
            // font in `Mode::BitmapFallback` (e.g. non-scalable color
            // emoji fonts). Callers check `Font::mode()` and dispatch
            // per sub-run.
            probeAndInstallMsdf(*adopted);

            Core::SharedPtr<Font> base = adopted;
            adoptedCache_[resolved] = base;

            if(textTraceEnabled()){
                std::cout << "[wtk-text] adoptResolvedFace: '"
                          << familyStr << "' size=" << size
                          << " -> "
                          << (base->mode() == Font::Mode::MSDF ? "MSDF" : "BitmapFallback")
                          << std::endl;
            }
            return base;
        }
    };

    // Phase 4. Out-of-line because the cache-miss path delegates back
    // to `HarfBuzzFontEngine::CreateFont`, which has to be complete
    // before we can call it — defining this inside the class would
    // require a forward-resolution dance.
    Core::SharedPtr<Font> FontConfigFontFallback::fallbackForCodepoint(
            Core::SharedPtr<Font> requested,
            std::uint32_t codepoint){
        if(engine_ == nullptr || requested == nullptr){
            return nullptr;
        }

        // Build a pattern carrying just the codepoint (in a charset)
        // and the requested size/style. FontConfig's standard
        // substitute pipeline will rank installed faces by how well
        // they cover the charset; FcFontMatch returns the best match.
        FcCharSet *charset = FcCharSetCreate();
        if(charset == nullptr) return nullptr;
        FcCharSetAddChar(charset, (FcChar32)codepoint);

        FcPattern *pattern = FcPatternBuild(nullptr,
            FC_CHARSET, FcTypeCharSet, charset,
            FC_PIXEL_SIZE, FcTypeDouble, (double)requested->desc.size,
            FC_SCALABLE, FcTypeBool, FcTrue,
            nullptr);
        FcCharSetDestroy(charset);
        if(pattern == nullptr) return nullptr;

        const int fcWeight =
            (requested->desc.style == FontDescriptor::Bold ||
             requested->desc.style == FontDescriptor::BoldAndItalic)
            ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR;
        const int fcSlant  =
            (requested->desc.style == FontDescriptor::Italic ||
             requested->desc.style == FontDescriptor::BoldAndItalic)
            ? FC_SLANT_ITALIC : FC_SLANT_ROMAN;
        FcPatternAddInteger(pattern, FC_WEIGHT, fcWeight);
        FcPatternAddInteger(pattern, FC_SLANT,  fcSlant);

        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);

        FcResult fcResult;
        FcPattern *matched = FcFontMatch(nullptr, pattern, &fcResult);
        FcPatternDestroy(pattern);
        if(matched == nullptr) return nullptr;

        FcChar8 *familyStr = nullptr;
        if(FcPatternGetString(matched, FC_FAMILY, 0, &familyStr)
                != FcResultMatch || familyStr == nullptr){
            FcPatternDestroy(matched);
            return nullptr;
        }
        std::string family(reinterpret_cast<const char *>(familyStr));
        FcPatternDestroy(matched);
        if(family.empty()){
            return nullptr;
        }

        // Same-as-requested → FontConfig declined to substitute; the
        // requested face already covers the codepoint. Don't loop.
        if(family == requested->desc.family){
            return nullptr;
        }

        auto it = byKey_.find(family);
        if(it != byKey_.end()){
            return it->second;
        }

        // Build the substitute face through the engine's normal
        // CreateFont path so it goes through FT/HB opening + the
        // MSDF probe + atlas hookup.
        FontDescriptor fbDesc(family, requested->desc.size,
                              requested->desc.style);
        Core::SharedPtr<Font> resolved = engine_->CreateFont(fbDesc);
        byKey_[family] = resolved;

        if(textTraceEnabled()){
            std::cout << "[wtk-text] FontConfigFontFallback: cp=U+"
                      << std::hex << codepoint << std::dec
                      << " -> '" << family << "' mode="
                      << (resolved != nullptr
                          && resolved->mode() == Font::Mode::MSDF
                          ? "MSDF" : "BitmapFallback") << std::endl;
        }
        return resolved;
    }

    FontEngine *FontEngine::inst(){
        return instance;
    }

    void FontEngine::Create(){
        if(instance == nullptr){
            instance = new HarfBuzzFontEngine();
        }
    }

    void FontEngine::Destroy(){
        delete instance;
        instance = nullptr;
    }

    Core::SharedPtr<GlyphRun>
    GlyphRun::fromUStringAndFont(const OmegaCommon::UniString &str, Core::SharedPtr<Font> &font) {
        return Core::SharedPtr<GlyphRun>(new HarfBuzzGlyphRun(str, font));
    }

    Core::SharedPtr<TextRect> TextRect::Create(Composition::Rect rect, const TextLayoutDescriptor &layoutDesc, float renderScale){
        // TODO: DPI plumbing — see wtk/docs/DPI-Aware-Text-Plan.md. Currently
        // ignores renderScale; HarfBuzz path needs to scale the rasterization
        // target and pass an FT_Set_Char_Size resolution driven by the scale.
        (void)renderScale;
        return Core::SharedPtr<TextRect>(new HarfBuzzTextRect(rect, layoutDesc));
    }

}
