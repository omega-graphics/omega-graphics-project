#include "omegaWTK/Composition/FontEngine.h"
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

        // Expose the protected mode setter to the engine factory so it
        // can promote a font to MSDF after running the outline probe.
        using Font::setMode;

        ~HarfBuzzFont() override {
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

        // Phase 6.7-c3: lift shaping out of `drawRun`. Builds a
        // PangoLayout at the font's *unscaled* design size (DPR is
        // applied downstream by the render viewport, keeping the MSDF
        // atlas resolution-independent), walks the layout's runs, and
        // emits one positioned glyph per visible glyph. Pango/PangoFc
        // resolves font fallback at run granularity; this chunk ships a
        // single sub-run, so any run whose resolved face does not match
        // the requested face flips `requiresFallback` and the caller
        // routes the whole string to the bitmap path.
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

            // Resolved family of the requested face. Runs whose resolved
            // face reports a different family were substituted by Pango
            // fallback — chunk 3 bails to the bitmap path for those.
            PangoFontDescription *ourResolvedDesc = pango_font_describe(ourFont);
            const char *ourFamily = (ourResolvedDesc != nullptr)
                ? pango_font_description_get_family(ourResolvedDesc)
                : nullptr;

            bool fallback = false;
            PangoLayoutIter *iter = pango_layout_get_iter(layout);
            if(iter != nullptr){
                do {
                    PangoLayoutRun *run = pango_layout_iter_get_run_readonly(iter);
                    if(run == nullptr){
                        // NULL run = end of a line; keep iterating.
                        continue;
                    }
                    PangoFont *runFont = run->item->analysis.font;
                    bool matches = false;
                    if(runFont != nullptr){
                        PangoFontDescription *runDesc = pango_font_describe(runFont);
                        if(runDesc != nullptr){
                            const char *runFamily = pango_font_description_get_family(runDesc);
                            if(ourFamily != nullptr && runFamily != nullptr &&
                               g_ascii_strcasecmp(ourFamily, runFamily) == 0){
                                matches = true;
                            }
                            pango_font_description_free(runDesc);
                        }
                    }
                    if(!matches){
                        fallback = true;
                        break;
                    }

                    PangoRectangle logical {};
                    pango_layout_iter_get_run_extents(iter, nullptr, &logical);
                    int baseline = pango_layout_iter_get_baseline(iter);
                    double penX = (double)logical.x;
                    const double baseY = (double)baseline;

                    PangoGlyphString *glyphs = run->glyphs;
                    for(int i = 0; i < glyphs->num_glyphs; ++i){
                        const PangoGlyphInfo &gi = glyphs->glyphs[i];
                        PangoGlyph glyph = gi.glyph;
                        if(glyph != PANGO_GLYPH_EMPTY &&
                           !(glyph & PANGO_GLYPH_UNKNOWN_FLAG)){
                            const double gx = penX + (double)gi.geometry.x_offset;
                            const double gy = baseY + (double)gi.geometry.y_offset;
                            result.glyphIds.push_back((std::uint32_t)glyph);
                            result.positions.push_back(Composition::Point2D{
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

            if(fallback){
                result.requiresFallback = true;
                result.glyphIds.clear();
                result.positions.clear();
            }

            if(textTraceEnabled()){
                std::cout << "[wtk-text] HarfBuzzGlyphRun::shape -> "
                          << (result.requiresFallback ? "FALLBACK" : "MSDF")
                          << ", glyphs=" << result.glyphIds.size() << std::endl;
                for(std::size_t k = 0; k < result.glyphIds.size() && k < 10; ++k){
                    std::cout << "[wtk-text]   shaped gid=" << result.glyphIds[k]
                              << " pos=(" << result.positions[k].x << ","
                              << result.positions[k].y << ")" << std::endl;
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

    class HarfBuzzFontEngine : public FontEngine {
    public:
        HarfBuzzFontEngine() = default;
        ~HarfBuzzFontEngine() override = default;

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
        static void probeAndInstallMsdf(HarfBuzzFont &font) {
#ifdef OMEGAWTK_HAVE_MSDFGEN
            PangoFont *resolved = font.getResolvedPangoFont();
            if(resolved == nullptr || !PANGO_IS_FC_FONT(resolved)){
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] HarfBuzzFont: '"
                              << font.desc.family << "' size=" << font.desc.size
                              << " could not resolve PangoFcFont; using BitmapFallback"
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
            const bool scalable  = (probeFace->face_flags & FT_FACE_FLAG_SCALABLE) != 0;
            const bool hasColor  = FT_HAS_COLOR(probeFace);
            pango_fc_font_unlock_face(fcFont);

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

            // The lambda captures the WTK Font's resolved PangoFont
            // pointer (lifetime tied to the Font). Each call locks the
            // FT_Face, walks the outline, runs msdfgen, unlocks.
            PangoFont *capturedResolved = resolved;
            const unsigned descSize = font.desc.size;
            font.atlas().setRasterizeFn(
                    [capturedResolved, descSize](std::uint32_t glyphId,
                                                 GlyphAtlas::RasterizedGlyph &out) -> bool {
                if(capturedResolved == nullptr || !PANGO_IS_FC_FONT(capturedResolved)){
                    return false;
                }
                PangoFcFont *fc = PANGO_FC_FONT(capturedResolved);
                FT_Face face = pango_fc_font_lock_face(fc);
                if(face == nullptr){
                    return false;
                }

                // Set the face to the requested point size so the
                // outline metrics map to pixel-space at the same DPI
                // chunk-3 will use to author quads.
                if(FT_Set_Pixel_Sizes(face, 0, descSize) != 0){
                    pango_fc_font_unlock_face(fc);
                    return false;
                }
                if(FT_Load_Glyph(face, glyphId,
                                 FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0){
                    pango_fc_font_unlock_face(fc);
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
                    pango_fc_font_unlock_face(fc);
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
                    pango_fc_font_unlock_face(fc);
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

                // Quantize float → uint8, straight through. msdfgen
                // emits a Y-up tile; the upload flip in `GlyphAtlas`
                // reconciles that with the GTE sampler's row-0-is-top
                // convention. (No extra +0.5 bias — `generateMSDF`
                // already maps the signed distance to [0,1] with the
                // glyph edge at 0.5.)
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

                // Metrics in pixel space (after FT_Set_Pixel_Sizes): the
                // bearing maps from the pen baseline up to the glyph
                // top, and `advance.x` is in 26.6 fixed-point.
                out.metrics.advance  = static_cast<float>(face->glyph->advance.x) / 64.f;
                out.metrics.bearingX = static_cast<float>(face->glyph->metrics.horiBearingX) / 64.f;
                out.metrics.bearingY = static_cast<float>(face->glyph->metrics.horiBearingY) / 64.f;

                // MSDF tile placement (Phase 6.7-c3). `l`/`b` are the
                // padded bbox origin in font-pixel space; `scale` is
                // tile-pixels per font-pixel. `inkPx*` is the *exact*
                // (un-ceiled) content size in tile pixels — the render
                // path and atlas address this rather than the ceil'd
                // `tileW/tileH` so the ceil sliver can't displace the
                // glyph.
                out.metrics.tileOriginX = static_cast<float>(l);
                out.metrics.tileOriginY = static_cast<float>(b);
                out.metrics.tileScale   = static_cast<float>(scale);
                out.metrics.inkPxW      = static_cast<float>((r - l) * scale);
                out.metrics.inkPxH      = static_cast<float>((t - b) * scale);

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

                pango_fc_font_unlock_face(fc);
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
            // time, not on the first drawText call. Index lookup uses
            // FT_Get_Char_Index against a freshly locked face.
            FT_Face smokeFace = pango_fc_font_lock_face(fcFont);
            if(smokeFace != nullptr){
                FT_UInt gid = FT_Get_Char_Index(smokeFace, 'A');
                pango_fc_font_unlock_face(fcFont);
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
    };

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
