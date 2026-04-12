#include "omegaWTK/Core/Core.h"
#include "Brush.h"
#include "Geometry.h"
#include "GTEForward.h"


#ifndef OMEGAWTK_COMPOSITION_FONTENGINE_H
#define OMEGAWTK_COMPOSITION_FONTENGINE_H

 namespace OmegaWTK {
    class AppInst;
 };

 namespace OmegaWTK::Composition {

 class Font;

 /**
  @brief A continious run of Glyphs (without line wrapping)
  @paragraph
  Create from an Unicode String.
 */
 class OMEGAWTK_EXPORT GlyphRun {
 public:

     virtual Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) = 0;

     /**
      @brief Creates a Glyph Run from a Unicode String.
      @param str[in] The Unicode String.
      @returns SharedPtr<GlyphRun>
     */
     static Core::SharedPtr<GlyphRun> fromUStringAndFont(const OmegaWTK::UniString & str,Core::SharedPtr<Font> & font);
     virtual ~GlyphRun() = default;
 };

 /**
  @brief A rectangular container that holds text drawn with a Font created by the FontEngine.
  @paragraph Description
  When creating an instance of this class, invoke the static method `Create` method,
  which will return an instance of the appropriate platform specific subclass implementing the platform specific
  features and bindings.
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
      @param rect[in] The Rect to draw the text in.
      @param layoutDesc[in]
      @returns SharedPtr<TextRect>
     */
     static Core::SharedPtr<TextRect> Create(Composition::Rect rect,const TextLayoutDescriptor & layoutDesc);
     virtual ~TextRect() = default;
 protected:
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
     FontDescriptor desc;
     Font(FontDescriptor & desc):desc(desc){};
     virtual void *getNativeFont() = 0;
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
