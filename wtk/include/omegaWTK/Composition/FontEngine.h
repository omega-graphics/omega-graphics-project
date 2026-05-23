#include "omegaWTK/Core/Core.h"
#include "omega-common/unicode.h"
#include "Brush.h"
#include "Geometry.h"
#include "GTEForward.h"

#include <cstdint>
#include <memory>


#ifndef OMEGAWTK_COMPOSITION_FONTENGINE_H
#define OMEGAWTK_COMPOSITION_FONTENGINE_H

 namespace OmegaCommon {
    class AssetBundle;
 };

 namespace OmegaWTK {
    class AppInst;
 };

 namespace OmegaWTK::Composition {

 class Font;
 /// Backend-private MSDF glyph atlas (Phase 6.7.1). Forward-declared
 /// here so `Font::atlas()` can return a reference without dragging the
 /// implementation into public WTK headers. The full type lives in
 /// `wtk/src/Composition/backend/GlyphAtlas.h`.
 class GlyphAtlas;

 /// Forward-declared so `FontEngine::shaper()` can return a reference
 /// without dragging the layout-engine header into this public header.
 /// The full type lives in `omegaWTK/Composition/TextLayoutEngine.h`.
 class ITextShaper;

 /// Per-platform font-fallback interface (Text-Layout-Engine-Plan
 /// §Phase 4). Owned by `FontEngine` and handed to the layout engine
 /// alongside the shaper. The layout engine drives the loop: after
 /// shaping a run with the requested face, any cluster that produced
 /// `.notdef` glyphs triggers a `fallbackForCodepoint` call; the
 /// returned face is re-shaped against the cluster's source text and
 /// spliced into the line with `ShapedGlyph::resolvedFont` set so
 /// the draw path groups it into its own atlas.
 ///
 /// Implementations are expected to **cache** the substitute `Font`
 /// they construct (typically keyed by the resolved native face's
 /// family name) so repeated fallback for the same codepoint returns
 /// the same `Font` and shares its `GlyphAtlas` across the process.
 class OMEGAWTK_EXPORT IFontFallback {
 public:
     virtual ~IFontFallback() = default;
     /// Find a `Font` capable of rendering `codepoint`, styled and
     /// sized to match `requested`. Returns `nullptr` when no
     /// substitute is needed (the requested face already covers the
     /// codepoint) or available (no platform substitute found); the
     /// layout engine then keeps the requested face's `.notdef`
     /// glyph for that cluster.
     virtual Core::SharedPtr<Font> fallbackForCodepoint(
         Core::SharedPtr<Font> requested,
         std::uint32_t codepoint) = 0;
 };

 /**
  @brief A struct that describes how text is laid out within a TextRect.
 */
 struct OMEGAWTK_EXPORT TextLayoutDescriptor {
      using Alignment = enum : OPT_PARAM {
         LeftUpper,
         LeftCenter,
         LeftLower,
         MiddleUpper,
         MiddleCenter,
         MiddleLower,
         RightUpper,
         RightCenter,
         RightLower
     };
     using Wrapping = enum : OPT_PARAM {
         None,
         WrapByWord,
         WrapByCharacter
     };
     Alignment alignment;
     Wrapping wrapping;
     unsigned lineLimit = 0;
 };

 /**
  @brief A continious run of Glyphs (without line wrapping)
  @paragraph
  Create from an Unicode String.
 */
 /// One resolved face's contribution to a shaped text run (Phase
 /// 6.7-c4). The layout engine produces one of these per resolved
 /// face after its font-fallback pass: typically one for Latin
 /// against the requested face plus one per fallback face (CJK,
 /// emoji, etc.). `glyphIds` and `positions` run in parallel —
 /// `positions[i]` is the pen-origin (baseline) position the layout
 /// engine assigned to `glyphIds[i]`, in canvas-space pixels relative
 /// to the owning text rect's origin. Glyph IDs are valid only
 /// against the matching `resolvedFont`'s atlas; that's why the
 /// render path emits one draw call per sub-run.
 struct OMEGAWTK_EXPORT TextSubRun {
     Core::SharedPtr<Font> resolvedFont;
     OmegaCommon::Vector<std::uint32_t> glyphIds;
     OmegaCommon::Vector<Composition::Point2D> positions;
 };

 /// Per-font metrics the text layout engine needs to compose lines
 /// without going back to a platform API. Phase 2 sources these from
 /// `Font::getMetrics()`; backends read them from FreeType / CTFont /
 /// IDWriteFontFace as appropriate. All values are in pixels at the
 /// font's current point size.
 struct OMEGAWTK_EXPORT FontMetrics {
     float ascent  = 0.f;  ///< Pixels above the baseline (positive).
     float descent = 0.f;  ///< Pixels below the baseline (positive).
     float lineGap = 0.f;  ///< Extra leading between consecutive lines.

     /// Default per-line height = ascent + descent + lineGap. Phase
     /// 2's layout engine stacks lines on this stride.
     float lineHeight() const { return ascent + descent + lineGap; }
 };

 /// Phase 7: result of rasterizing one BitmapFallback sub-run to a
 /// CPU bitmap and uploading it as a GETexture. The per-platform
 /// `FontEngine::rasterizeSubRunToTexture` returns this; the canvas
 /// hands the texture to `drawGETexture` as a normal blit.
 ///
 /// When the engine can't (or won't) rasterize — typically because
 /// the backend doesn't ship a CPU rasterizer yet — `texture` is
 /// `nullptr` and the caller skips the draw. Glyphs that should have
 /// been emitted simply disappear; this is the same degenerate
 /// behaviour the pre-Phase-7 bitmap path had when it couldn't load
 /// the requested face.
 struct OMEGAWTK_EXPORT BitmapTextResult {
     SharedHandle<OmegaGTE::GETexture> texture;
     Core::SharedPtr<OmegaGTE::GEFence> fence;
 };

 /**
  @brief A struct that describes a Font that can be created by the FontEngine.
 */
 struct OMEGAWTK_EXPORT  FontDescriptor {
     using FontStyle = enum : OPT_PARAM {
         Regular,
         Italic,
         Bold,
         BoldAndItalic
     };
     OmegaCommon::String family;
     FontStyle style;
     unsigned size;
     FontDescriptor(OmegaCommon::String _family,unsigned size,FontStyle _style = Regular):family(_family),style(_style),size(size){};
     ~FontDescriptor(){};
 };

 /**
  @brief A Font created by the global instance of the FontEngine class.
  Can be used in almost any context.
 */
 class OMEGAWTK_EXPORT  Font {
 public:
     /// Per-font glyph rasterization mode (Phase 6.7.4). Selected once
     /// at construction by the platform `FontEngine` based on whether
     /// the underlying face exposes vector outlines msdfgen can walk.
     /// `MSDF` routes draws through the per-font `GlyphAtlas`;
     /// `BitmapFallback` routes the resolved sub-run through
     /// `FontEngine::rasterizeSubRunToTexture` and a `drawGETexture`
     /// blit (Phase 7). Sticky for the font's lifetime.
     enum class Mode {
         MSDF,
         BitmapFallback
     };

     FontDescriptor desc;
     /// Constructed with `BitmapFallback` mode and an empty atlas. The
     /// concrete `FontEngine` implementation flips the mode to `MSDF`
     /// once the per-platform outline-extraction probe (Phase 6.7.4)
     /// confirms the face is rasterizable; chunk 1 leaves all fonts on
     /// `BitmapFallback`.
     Font(FontDescriptor & desc);
     /// Out-of-line so the `unique_ptr<GlyphAtlas>` member can hold an
     /// incomplete type at the public include site.
     virtual ~Font();
     virtual void *getNativeFont() = 0;
     /// Per-font metrics in pixels at the descriptor's size (Phase 2
     /// of the text-layout-engine plan). Default impl returns zero
     /// metrics — backends override to read FreeType/CTFont/DWrite.
     /// The layout engine uses these for line stride + baseline
     /// placement; zero metrics collapse multi-line text onto one
     /// stripe, which is the correct degenerate behavior while a
     /// backend's MSDF path isn't yet wired through.
     OMEGAWTK_NODISCARD virtual FontMetrics getMetrics() const { return {}; }
     /// Returns the per-font glyph atlas (Phase 6.7.1). Always
     /// constructed alongside the `Font`; carries no GPU texture until
     /// `ensureGlyph` is first called.
     GlyphAtlas & atlas();
     /// Cached MSDF/BitmapFallback choice from construction time.
     Mode mode() const;
     /// Ensure every glyph in `glyphIds` is resident in the atlas
     /// (Phase 6.7-c3). Called from the paint-recording thread when a
     /// `TextRun` is emitted so the MSDF tile uploads happen outside the
     /// compositor's frame render pass. Glyphs that fail to rasterize or
     /// pack are left absent — the render path skips them. Out-of-line
     /// because it touches the incomplete `GlyphAtlas` type.
     void ensureGlyphsResident(const OmegaCommon::Vector<std::uint32_t> & glyphIds);

 protected:
     /// Backend subclasses set this from their own ctor once the
     /// per-platform outline probe runs.
     void setMode(Mode m);

 private:
     Mode mode_;
     std::unique_ptr<GlyphAtlas> atlas_;
 };
 /**
  @brief Font creation engine for Application
  @paragraph Description
   On application startup, a global instance of this class is created and ONLY one may exist throughout the entirety of the app's runtime.
   This is mainly due to preventing the spawning of multiple IDWriteFactory instances (on Windows).
 */
 class OMEGAWTK_EXPORT FontEngine {
      static FontEngine * instance;
 public:
     /**
      @brief Create a Font.
      @param desc[in] The Font Descriptor describing a Font.
      @paragraph Description
      Creates a shared instance of the font based on the FontDescriptor provided,
      which can be used in a global context or a local context such as within the context of a View.
      @returns SharedPtr<Font>
     */
     INTERFACE_METHOD Core::SharedPtr<Font> CreateFont(FontDescriptor & desc) ABSTRACT;
     INTERFACE_METHOD Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path,FontDescriptor & desc) ABSTRACT;
     /**
      @brief Create a Font from a named entry in an AssetBundle.
      @param bundle[in] The bundle to read from. Must be non-null.
      @param assetName[in] The bundle entry name (e.g. "fonts/Inter-Regular.ttf").
      @param desc[in] The Font Descriptor. `desc.family` is used to resolve
             a specific face *within* the loaded font file when the file
             carries multiple faces (TTC); for single-face files it is
             ignored for face selection but still drives weight + style
             matching against the file's own family metadata.
      @returns SharedPtr<Font>, or nullptr if the bundle is null, the
               asset is missing, the bytes don't parse as a font, or the
               descriptor can't be matched against the loaded face(s).

      Default implementation returns nullptr. Each backend overrides
      against its in-memory font loader (DWrite custom IDWriteFontFileLoader,
      Core Text CTFontManagerCreateFontDescriptorsFromData, FreeType
      FT_New_Memory_Face + hb_ft_font_create_referenced).
     */
     virtual Core::SharedPtr<Font> CreateFontFromAsset(
             OmegaCommon::AssetBundle *bundle,
             const OmegaCommon::String &assetName,
             FontDescriptor &desc) {
         (void)bundle; (void)assetName; (void)desc;
         return nullptr;
     }
     /// Per-platform text shaper used by `TextLayoutEngine`
     /// (Text-Layout-Engine-Plan.md Phase 2). The Linux engine
     /// returns a HarfBuzz shaper; macOS / Windows backends return
     /// their per-run shapers once those phases light up. The base
     /// implementation returns `nullptr` — callers must check before
     /// invoking the layout engine.
     virtual ITextShaper * shaper() { return nullptr; }
     /// Per-platform font-fallback driver (Phase 4). Linux uses
     /// FontConfig's substitute chain; macOS uses
     /// `CTFontCreateForString`. Base implementation returns
     /// `nullptr` — callers may invoke the layout engine without a
     /// fallback driver (every `.notdef` cluster then renders as
     /// the requested face's missing-glyph box).
     virtual IFontFallback * fallback() { return nullptr; }
     /// Phase 7. Rasterize one BitmapFallback sub-run to a CPU
     /// bitmap and upload as a GETexture. The layout engine has
     /// already done all line composition / fallback resolution /
     /// glyph positioning — `subRun.glyphIds` and `subRun.positions`
     /// are in canvas-space pixels relative to `rect`. Each backend
     /// drives its native glyph-drawing API (Core Text:
     /// `CTFontDrawGlyphs`; Linux: cairo + FreeType; DWrite: D2D
     /// `DrawGlyphRun`). `renderScale` scales the offscreen surface
     /// for high-DPI output. Base implementation returns an empty
     /// result — backends without a rasterizer skip BitmapFallback
     /// draws (those glyphs disappear, same as the pre-Phase-7
     /// behaviour when the legacy `TextRect` couldn't open the
     /// requested face).
     virtual BitmapTextResult rasterizeSubRunToTexture(
             const TextSubRun & subRun,
             const Composition::Rect & rect,
             const Composition::Color & color,
             float renderScale) {
         (void)subRun; (void)rect; (void)color; (void)renderScale;
         return {};
     }
     static FontEngine *inst();
     virtual ~FontEngine() = default;
 private:
     friend class ::OmegaWTK::AppInst;
     static void Create();
     static void Destroy();
 };

 };

#endif
