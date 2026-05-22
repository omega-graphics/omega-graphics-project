#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "../GlyphAtlas.h"

#include "omega-common/fs.h"
#include "omega-common/assets.h"

#include <fontconfig/fontconfig.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>

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
#include <memory>
#include <unordered_map>
#include <vector>

namespace OmegaWTK::Composition {

    namespace {
        bool textTraceEnabled() {
            static const bool enabled = []() {
                auto e = OmegaCommon::getEnvVar("OMEGAWTK_TRACE_TEXT");
                return e.has_value() && !e->empty() && (*e)[0] != '0';
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

    class HarfBuzzFont : public Font {
        // FreeType face + HarfBuzz font, opened directly via FontConfig
        // (no Pango descent — that path retired in Phase 7). Owned
        // per-Font; sized once to `desc.size` and never resized
        // afterward, so callers can share the FT_Face without worrying
        // about cross-talk between MSDF rasterization, HB shaping, and
        // the cairo-backed bitmap rasterizer.
        FT_Face ftFace_ = nullptr;
        hb_font_t *hbFont_ = nullptr;

        // Asset-bundle font path: when an FT_Face is opened via
        // `FT_New_Memory_Face`, FreeType does NOT copy the bytes — it
        // reads from the caller-owned buffer for the face's whole
        // lifetime. The HarfBuzzFont takes shared ownership of the
        // blob here so it stays alive at least until the FT_Face is
        // closed in this Font's dtor. Null when the font was loaded
        // from a real file path or FontConfig-resolved.
        std::shared_ptr<std::vector<std::uint8_t>> memoryBlob_;
    public:
        explicit HarfBuzzFont(FontDescriptor &desc): Font(desc) {}

        /// Tie the lifetime of an in-memory font blob to this Font.
        /// Required when the FT_Face was opened via FT_New_Memory_Face,
        /// because FreeType reads from the caller-owned buffer
        /// continuously. The blob is released in this Font's dtor —
        /// strictly after `FT_Done_Face` runs.
        void retainMemoryBlob(std::shared_ptr<std::vector<std::uint8_t>> blob){
            memoryBlob_ = std::move(blob);
        }

        // Phase 7: no Pango-side handle to expose. `getNativeFont`
        // returns the FT_Face so clients that need a native handle
        // (rare; primarily test code) can still get one.
        void * getNativeFont() override {
            return ftFace_;
        }

        // Direct FT/HB accessors. MSDF rasterization uses `ftFace`;
        // HB shaping uses `hbFont`. May return null if FontConfig /
        // FreeType couldn't open the face; the engine then keeps the
        // font on BitmapFallback and the cairo rasterizer (which
        // reopens an FT_Face on its own) takes over.
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

        // Phase 7. Replaces the legacy `HarfBuzzTextRect::drawRun` /
        // Pango bitmap path. Each glyph in `subRun.glyphIds` has an
        // already-positioned baseline origin in `subRun.positions[i]`;
        // we lower them onto a cairo image surface via
        // `cairo_show_glyphs`, then copy the surface into a GETexture.
        // The face is reopened through cairo's FreeType bridge so we
        // never share the FT_Face's pixel-size state with the shaper.
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
            auto hbF = std::dynamic_pointer_cast<HarfBuzzFont>(subRun.resolvedFont);
            if(hbF == nullptr){
                return res;
            }
            FT_Face face = hbF->ftFace();
            if(face == nullptr){
                return res;
            }
            const float scale = (renderScale > 0.f) ? renderScale : 1.f;
            const std::size_t pixW =
                std::max<std::size_t>(1, (std::size_t)std::ceil(rect.w * scale));
            const std::size_t pixH =
                std::max<std::size_t>(1, (std::size_t)std::ceil(rect.h * scale));
            const std::size_t bpr = pixW * 4;
            std::vector<std::uint8_t> data(bpr * pixH, 0);

            cairo_surface_t *surface = cairo_image_surface_create_for_data(
                data.data(),
                CAIRO_FORMAT_ARGB32,
                (int)pixW, (int)pixH, (int)bpr);
            if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS){
                cairo_surface_destroy(surface);
                return res;
            }
            cairo_t *cr = cairo_create(surface);
            if(cairo_status(cr) != CAIRO_STATUS_SUCCESS){
                cairo_destroy(cr);
                cairo_surface_destroy(surface);
                return res;
            }
            cairo_set_antialias(cr, CAIRO_ANTIALIAS_SUBPIXEL);
            cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);

            // Cairo's image surface uses Y-down (matches the layout
            // engine's canvas convention). Apply renderScale via the
            // user-space CTM so layout positions stay in logical
            // pixels.
            cairo_scale(cr, (double)scale, (double)scale);

            // Bridge the FT_Face into cairo. The cairo_font_face_t
            // shares the face — cairo manages its own pixel-size
            // state through `cairo_set_font_matrix`, so we don't
            // disturb the shaper's FT_Set_Pixel_Sizes.
            cairo_font_face_t *cFace =
                cairo_ft_font_face_create_for_ft_face(face, 0);
            if(cairo_font_face_status(cFace) != CAIRO_STATUS_SUCCESS){
                cairo_font_face_destroy(cFace);
                cairo_destroy(cr);
                cairo_surface_destroy(surface);
                return res;
            }
            cairo_set_font_face(cr, cFace);
            cairo_font_face_destroy(cFace);
            cairo_matrix_t fm;
            cairo_matrix_init_scale(&fm,
                                    (double)hbF->desc.size,
                                    (double)hbF->desc.size);
            cairo_set_font_matrix(cr, &fm);

            std::vector<cairo_glyph_t> glyphs;
            glyphs.reserve(subRun.glyphIds.size());
            for(std::size_t i = 0; i < subRun.glyphIds.size(); ++i){
                cairo_glyph_t g;
                g.index = (unsigned long)subRun.glyphIds[i];
                g.x = (double)subRun.positions[i].x;
                g.y = (double)subRun.positions[i].y;
                glyphs.push_back(g);
            }
            cairo_show_glyphs(cr, glyphs.data(), (int)glyphs.size());
            cairo_surface_flush(surface);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);

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
            texture->copyBytes(data.data(), bpr);
            res.texture = texture;
            return res;
        }

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
            // Phase 7: pure FontConfig + FreeType + HarfBuzz path. The
            // old PangoFontDescription + PangoFontMap resolution dance
            // retired with the legacy `HarfBuzzGlyphRun` / Pango
            // bitmap path. The layout engine drives shaping through
            // `hb_font_t`; the MSDF rasterizer walks outlines through
            // `Font::ftFace()`; the cairo bitmap rasterizer reopens
            // the FT_Face via `cairo_ft_font_face_create_for_ft_face`.
            auto font = Core::SharedPtr<HarfBuzzFont>(new HarfBuzzFont(desc));
            FT_Face ftFace = nullptr;
            hb_font_t *hbFont = nullptr;
            if(openFTAndHB(desc, ftFace, hbFont)){
                font->setFTHandles(ftFace, hbFont);
            }
            // Failure of any probe step leaves the font on
            // BitmapFallback (the default installed by Font's base
            // ctor); the cairo rasterizer takes over for that face.
            probeAndInstallMsdf(*font);
            return font;
        }

        /// Decide whether `font`'s underlying face supports MSDF outline
        /// extraction; if so, install a RasterizeFn on its atlas and
        /// flip its mode. Otherwise log once (when traced) and leave
        /// on BitmapFallback.
        ///
        /// Phase 7: pure direct-FT_Face path — the Pango lock fallback
        /// retired with `adoptResolvedFace`. If `openFTAndHB` failed
        /// the face has no FT_Face and the probe early-exits, keeping
        /// the font on BitmapFallback for the cairo rasterizer to
        /// service.
        static void probeAndInstallMsdf(HarfBuzzFont &font) {
#ifdef OMEGAWTK_HAVE_MSDFGEN
            FT_Face directFace = font.ftFace();
            if(directFace == nullptr){
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] HarfBuzzFont: '"
                              << font.desc.family << "' size=" << font.desc.size
                              << " no FT face; using BitmapFallback"
                              << std::endl;
                }
                return;
            }
            const bool scalable = (directFace->face_flags & FT_FACE_FLAG_SCALABLE) != 0;
            const bool hasColor = FT_HAS_COLOR(directFace);

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

            // The lambda captures the directly-opened FT_Face, sized
            // once at Font construction. No locking needed (we own
            // it); no Pango descent.
            FT_Face capturedFace = directFace;
            const unsigned descSize = font.desc.size;
            font.atlas().setRasterizeFn(
                    [capturedFace, descSize](std::uint32_t glyphId,
                                             GlyphAtlas::RasterizedGlyph &out) -> bool {
                FT_Face face = capturedFace;
                // Re-set pixel size defensively — idempotent for the
                // primary face; matters if a future caller shares this
                // FT_Face for a different rendering size.
                if(FT_Set_Pixel_Sizes(face, 0, descSize) != 0){
                    return false;
                }
                if(FT_Load_Glyph(face, glyphId,
                                 FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0){
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
            // time, not on the first drawText call.
            FT_Face smokeFace = font.ftFace();
            if(smokeFace != nullptr){
                FT_UInt gid = FT_Get_Char_Index(smokeFace, 'A');
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

        // Asset-bundle font loading. Opens the FT_Face directly via
        // FT_New_Memory_Face against the bundle's bytes — no temp file,
        // no FontConfig dance (FontConfig works on filesystem paths).
        // The MSDF path lights up via the directly-opened face; the
        // legacy Pango/Cairo BitmapFallback path is NOT supported for
        // asset-loaded fonts (no PangoFontDescription resolves to this
        // face). MSDF mode is the practical contract here.
        Core::SharedPtr<Font> CreateFontFromAsset(
                OmegaCommon::AssetBundle *bundle,
                const OmegaCommon::String &assetName,
                FontDescriptor &desc) override {
            if(bundle == nullptr || ftLibrary_ == nullptr){
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
            auto blob = std::make_shared<std::vector<std::uint8_t>>(
                std::move(loadResult.value()));
            if(blob->empty()){
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                              << "' bundle entry is empty" << std::endl;
                }
                return nullptr;
            }

            // FT_New_Memory_Face does NOT copy the buffer — FreeType
            // reads from it for the FT_Face's whole lifetime. The blob
            // is anchored on the HarfBuzzFont via retainMemoryBlob
            // below so it outlives FT_Done_Face in the dtor.
            FT_Face face = nullptr;
            const FT_Error err = FT_New_Memory_Face(
                ftLibrary_,
                reinterpret_cast<const FT_Byte *>(blob->data()),
                static_cast<FT_Long>(blob->size()),
                0 /*face_index*/, &face);
            if(err != 0 || face == nullptr){
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] CreateFontFromAsset: FT_New_Memory_Face failed err="
                              << err << std::endl;
                }
                return nullptr;
            }
            if(FT_Set_Pixel_Sizes(face, 0, desc.size) != 0){
                FT_Done_Face(face);
                return nullptr;
            }
            hb_font_t *hb = hb_ft_font_create_referenced(face);
            if(hb == nullptr){
                FT_Done_Face(face);
                return nullptr;
            }

            auto font = Core::SharedPtr<HarfBuzzFont>(new HarfBuzzFont(desc));
            font->setFTHandles(face, hb);
            font->retainMemoryBlob(blob);

            probeAndInstallMsdf(*font);
            if(textTraceEnabled()){
                std::cout << "[wtk-text] CreateFontFromAsset: '" << desc.family
                          << "' size=" << desc.size
                          << " loaded from bundle asset '" << assetName
                          << "' (" << blob->size() << " bytes) -> "
                          << (font->mode() == Font::Mode::MSDF ? "MSDF" : "BitmapFallback")
                          << std::endl;
            }
            return font;
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

}
