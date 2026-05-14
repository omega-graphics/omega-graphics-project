#include "omegaWTK/Core/Core.h"
#include "omega-common/unicode.h"
#include "Brush.h"
#include "Geometry.h"
#include "GTEForward.h"

#include <cstdint>
#include <memory>


#ifndef OMEGAWTK_COMPOSITION_FONTENGINE_H
#define OMEGAWTK_COMPOSITION_FONTENGINE_H

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
 class OMEGAWTK_EXPORT GlyphRun {
 public:

     /// Result of shaping a `GlyphRun` for the MSDF render path
     /// (Phase 6.7-c3). `glyphIds` and `positions` run in parallel:
     /// `positions[i]` is the pen-origin (baseline) position the
     /// layout engine assigned to `glyphIds[i]`, in logical pixels
     /// relative to the owning text rect's origin. Glyph IDs are
     /// valid against the requested `Font`'s atlas.
     ///
     /// `requiresFallback` is set when the layout engine substituted a
     /// face that does not match the requested font (e.g. CJK text in
     /// a Latin-only font). Chunk 3 ships a single sub-run per run, so
     /// any fallback routes the whole string back to the bitmap path;
     /// chunk 4 lights up multi-atlas sub-runs.
     struct ShapedTextRun {
         bool requiresFallback = false;
         OmegaCommon::Vector<std::uint32_t> glyphIds;
         OmegaCommon::Vector<Composition::Point2D> positions;
     };

     virtual Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) = 0;

     /**
      @brief Shapes the run against its font and produces positioned
      glyph IDs for the MSDF render path (Phase 6.7-c3).
      @param rect[in] The destination rect (logical pixels). Drives
             wrap width and vertical alignment.
      @param layoutDesc[in] Alignment / wrapping / line-limit.
      @returns A `ShapedTextRun`. When `requiresFallback` is true the
               caller should render via the legacy bitmap path.
     */
     virtual ShapedTextRun shape(const Composition::Rect & rect,
                                 const TextLayoutDescriptor & layoutDesc) = 0;

     /**
      @brief Creates a Glyph Run from a Unicode String.
      @param str[in] The Unicode String.
      @returns SharedPtr<GlyphRun>
     */
     static Core::SharedPtr<GlyphRun> fromUStringAndFont(const OmegaCommon::UniString & str,Core::SharedPtr<Font> & font);
     virtual ~GlyphRun() = default;
 };

 /**
  @brief A rectangular container that holds text drawn with a Font created by the FontEngine.
  @paragraph Description
  When creating an instance of this class, invoke the static method `Create` method,
  which will return an instance of the appropriate platform specific subclass implementing the platform specific
  features and bindings.
 */

 class OMEGAWTK_EXPORT  TextRect {
 public:
    struct BitmapRes {
        SharedHandle<OmegaGTE::GETexture> s;
        Core::SharedPtr<OmegaGTE::GEFence> textureFence;
        };
     virtual BitmapRes toBitmap() = 0;

     Composition::Rect rect;

     virtual void *getNative() = 0;

     /**
      @brief Draws a Glyph Run to the Rect.
      @param glyphRun[in]
      @paragraph Starts the next glyph at the prior ending position of the last run.
     */
     virtual void drawRun(Core::SharedPtr<GlyphRun> &glyphRun,const Composition::Color &color) = 0;

     /**
      @brief Creates an empty TextRect from a Rect and a TextLayoutDescriptor.
      @param rect[in] The Rect to draw the text in (logical pixels / DIPs).
      @param layoutDesc[in]
      @param renderScale[in] Logical-to-physical pixel scale factor (1.0 = 96 DPI).
             The backend allocates a physical-sized offscreen surface and
             configures the device context so font sizes stay in DIPs.
      @returns SharedPtr<TextRect>
     */
     static Core::SharedPtr<TextRect> Create(Composition::Rect rect,const TextLayoutDescriptor & layoutDesc, float renderScale = 1.f);
     virtual ~TextRect() = default;
 protected:
     float renderScale = 1.f;
     TextRect(Composition::Rect & rect):rect(rect){};
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
     /// `BitmapFallback` keeps the existing `TextRect` →
     /// `drawGETexture` path. Sticky for the font's lifetime.
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
      which can be used global context or local context such as a within the context of a CanvasView.
      @returns SharedPtr<Font>
     */
     INTERFACE_METHOD Core::SharedPtr<Font> CreateFont(FontDescriptor & desc) ABSTRACT;
     INTERFACE_METHOD Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path,FontDescriptor & desc) ABSTRACT;
     static FontEngine *inst();
     virtual ~FontEngine() = default;
 private:
     friend class ::OmegaWTK::AppInst;
     static void Create();
     static void Destroy();
 };

 };

#endif
