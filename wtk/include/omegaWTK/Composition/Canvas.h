#include "omegaWTK/Core/Core.h"
#include "omega-common/img.h"
#include "omega-common/unicode.h"
#include "Geometry.h"
#include "GTEForward.h"
#include "Path.h"
#include "FontEngine.h"
#include "Layer.h"

#include "CompositorClient.h"

#include <algorithm>
#include <cstdint>
#include <type_traits>

#ifndef OMEGAWTK_COMPOSITION_CANVAS_H
#define OMEGAWTK_COMPOSITION_CANVAS_H

namespace OmegaWTK {
    class View;
    class AppWindow;

    namespace Media {
        struct BitmapImage;
    }
    namespace Composition {
        struct Brush;
        struct LayerEffect;

        struct OMEGAWTK_EXPORT  Border {
            Core::SharedPtr<Brush> brush;
            unsigned width;

            Border() = delete;

            Border(Core::SharedPtr<Brush> &_brush, unsigned _width) : brush(_brush), width(_width) {};
        };

        /// @brief Nine-slice insets for resizable bitmaps (Phase 6.6.3).
        ///
        /// Specified in *texture pixels* against the bitmap's source-rect
        /// (or full texture when no source-rect is set). The four edges
        /// describe the rows / columns *inside* the texture that mark the
        /// inner stretchable region. Drawing a nine-slice bitmap emits 9
        /// separate `Bitmap` visuals — corners at fixed size, edges and
        /// center stretched — each carrying its own destination rect and
        /// source-rect. Purely a Canvas-side decomposition; the backend
        /// pipeline is unchanged from the single-quad bitmap path.
        struct OMEGAWTK_EXPORT NineSliceInsets {
            float left   = 0.f;
            float top    = 0.f;
            float right  = 0.f;
            float bottom = 0.f;
        };

        struct OMEGAWTK_EXPORT  CanvasEffect {
            enum class Type : OPT_PARAM {
                DirectionalBlur,
                GaussianBlur
            };
            struct GaussianBlurParams {
                float radius = 0.F;
            } ;
            struct DirectionalBlurParams {
                float radius = 0.F;
                float angle = 0.F;
            } ;
            Type type = Type::GaussianBlur;
            /// Legacy optional payload pointer. Prefer gaussianBlur/directionalBlur owned fields.
            void *params = nullptr;
            GaussianBlurParams gaussianBlur {};
            DirectionalBlurParams directionalBlur {};
        };
///   


    class Canvas;



    /// `TextSubRun` lives in `FontEngine.h` (Phase 6.7-c4) and is the
    /// resolved-font-grouped output of the WTK text layout engine.
    /// Visible here through `FontEngine.h`'s inclusion above.

    /// Result of running `Canvas::drawText`'s shaping pipeline without
    /// dispatching anything. Splits the layout output into the two
    /// channels the renderer needs:
    ///
    ///  - `msdfSubRuns` — MSDF runs (one per resolved face). Each can
    ///    be packaged into a `DrawOp::TextRun` (or fed to
    ///    `Canvas::drawTextRun`) and rides the atlas pipeline.
    ///  - `bitmapBlits` — per-sub-run pre-rasterized textures for
    ///    fonts in `BitmapFallback` mode. Each becomes a
    ///    `DrawOp::Bitmap` (or feeds `Canvas::drawGETexture`).
    ///
    /// Used by the legacy `Canvas::drawText` entry point and by the
    /// new DisplayList-emitting paint paths (UIView-Render-Redesign-
    /// Plan Tier 2 Phase 2.1) so they share the same shaping +
    /// fallback semantics. Pure function; no Canvas / GPU state
    /// touched.
    struct OMEGAWTK_EXPORT ShapedTextRun {
        struct BitmapBlit {
            Core::SharedPtr<OmegaGTE::GETexture> texture;
            Core::SharedPtr<OmegaGTE::GEFence> fence;
        };
        OmegaCommon::Vector<TextSubRun> msdfSubRuns;
        OmegaCommon::Vector<BitmapBlit> bitmapBlits;
    };

    /// Layout, group, partition, and ensure-residency for an MSDF + bitmap
    /// text run, without emitting any draw call. The MSDF path calls
    /// `Font::ensureGlyphsResident` *here* because atlas uploads are
    /// illegal inside the compositor's frame render pass; the bitmap
    /// path rasterizes through the engine's CPU rasterizer.
    /// `renderScale` is the owning view's render scale (DPI factor).
    OMEGAWTK_EXPORT ShapedTextRun shapeTextForDisplayList(
        const OmegaCommon::UniString & text,
        const Core::SharedPtr<Font> & font,
        const Composition::Rect & rect,
        const Composition::Color & color,
        const TextLayoutDescriptor & layoutDesc,
        float renderScale);

    /// An object drawn by a Compositor.
    struct  OMEGAWTK_EXPORT VisualCommand {
        typedef enum : OPT_PARAM {
            Rect,
            RoundedRect,
            Ellipse,
            VectorPath,
            Text,
            TextRun,
            Bitmap,
            Shadow,
            SetTransform,
            SetOpacity
        } Type;
        Type type;
        struct OMEGAWTK_EXPORT Data {
            struct {
                Composition::Rect rect;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } rectParams;
            
            struct  {
                Composition::RoundedRect rect;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } roundedRectParams;
            
            struct {
                Composition::Ellipse ellipse;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } ellipseParams;
            struct {
                Core::SharedPtr<OmegaGTE::GVectorPath2D> path;
                Core::SharedPtr<Brush> brush;
                Core::SharedPtr<Brush> fillBrush;
                float strokeWidth;
                bool contour;
                bool fill;
            } pathParams;
            struct {
                Core::SharedPtr<OmegaCommon::Img::BitmapImage> img;
                Core::SharedPtr<OmegaGTE::GETexture> texture;
                Core::SharedPtr<OmegaGTE::GEFence> textureFence;
                Composition::Rect rect;
                /// Phase 6.6.2: optional sub-rect (in texture pixels)
                /// to sample. When unset the bitmap samples the full
                /// texture (UV 0..1). Used for sprite atlases and as
                /// the per-slice source-rect for nine-slice draws.
                Core::Optional<Composition::Rect> sourceRect;
                /// Phase 6.6.2: optional RGBA tint multiplied onto the
                /// sampled color. Identity (1,1,1,1) is the default
                /// passthrough. The value is supplied by the caller as
                /// straight-alpha RGBA in `[0,1]`.
                Core::Optional<Composition::Color> tintColor;
            } bitmapParams;
            struct ShadowParamsData {
                LayerEffect::DropShadowParams shadow {};
                Composition::Rect shapeRect {};
                float cornerRadius = 0.f;
                bool isEllipse = false;
            } shadowParams {};
            /// Phase 6.7.2: per-glyph quads + atlas binding(s) for an
            /// MSDF text run. `subRuns` is one entry per resolved face
            /// after layout-engine fallback. The render path issues
            /// one draw call per sub-run because each sub-run's atlas
            /// texture differs. Empty in chunk 1 — no caller emits
            /// `TextRun` yet.
            struct {
                OmegaCommon::Vector<TextSubRun> subRuns;
                Composition::Rect rect;
                Composition::Color color;
            } textRunParams {};
            Matrix4x4 transformMatrix = Matrix4x4::Identity();
            float opacityValue = 1.f;


            Data(const Composition::Rect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border);

            Data(const Composition::RoundedRect & rect,Core::SharedPtr<Brush> brush,Core::Optional<Border> border);

            Data(const Composition::Ellipse & ellipse,Core::SharedPtr<Brush> brush,Core::Optional<Border> border);

            Data(const Core::SharedPtr<OmegaGTE::GVectorPath2D> & path,
                 Core::SharedPtr<Brush> brush,
                 Core::SharedPtr<Brush> fillBrush,
                 float strokeWidth,
                 bool contour,
                 bool fill);

            Data(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,const Composition::Rect &rect);

            Data(Core::SharedPtr<OmegaGTE::GETexture> texture,Core::SharedPtr<OmegaGTE::GEFence> textureFence,const Composition::Rect &rect);

            /// Phase 6.6.2: bitmap with optional sub-rect and tint.
            Data(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,
                 const Composition::Rect & rect,
                 Core::Optional<Composition::Rect> sourceRect,
                 Core::Optional<Composition::Color> tintColor);

            Data(const LayerEffect::DropShadowParams & shadow,const Composition::Rect & shapeRect,float cornerRadius,bool isEllipse);

            /// Phase 6.7.2: TextRun ctor. Takes ownership of the
            /// sub-run vector via move.
            Data(OmegaCommon::Vector<TextSubRun> subRuns,
                 const Composition::Rect & rect,
                 const Composition::Color & color);

            explicit Data(const Matrix4x4 & matrix);

            explicit Data(float opacityVal);

            void _destroy(Type t);

            ~Data() DEFAULT;
            
         } params;
        VisualCommand() = delete;

        #define VISUAL_COMMAND_ARGS_CHECK(SUBJECT,...) std::enable_if_t<std::is_same_v<std::tuple<std::remove_cv_t<std::remove_reference_t<SUBJECT>>...>,std::tuple<__VA_ARGS__>>,int> = 0

        template<class ..._Args,VISUAL_COMMAND_ARGS_CHECK(_Args,Composition::Rect,Core::SharedPtr<Brush>,Core::Optional<Border>)>
        VisualCommand(_Args && ...args):type(Rect),params(args...){};

        template<class ..._Args,VISUAL_COMMAND_ARGS_CHECK(_Args,Composition::RoundedRect,Core::SharedPtr<Brush>,Core::Optional<Border>)>
        VisualCommand(_Args && ...args):type(RoundedRect),params(args...){};

        template<class ..._Args,VISUAL_COMMAND_ARGS_CHECK(_Args,Composition::Ellipse,Core::SharedPtr<Brush>,Core::Optional<Border>)>
        VisualCommand(_Args && ...args):type(Ellipse),params(args...){};
        VisualCommand(const Core::SharedPtr<OmegaGTE::GVectorPath2D> & path,
                      Core::SharedPtr<Brush> brush,
                      Core::SharedPtr<Brush> fillBrush,
                      float strokeWidth,
                      bool contour,
                      bool fill):
        type(VectorPath),
        params(path,brush,fillBrush,strokeWidth,contour,fill){};

        template<class ..._Args,VISUAL_COMMAND_ARGS_CHECK(_Args,Core::SharedPtr<OmegaCommon::Img::BitmapImage>,Composition::Rect)>
        VisualCommand(_Args && ...args):type(Bitmap),params(args...){};

        template<class ..._Args,VISUAL_COMMAND_ARGS_CHECK(_Args,Core::SharedPtr<OmegaGTE::GETexture>,Core::SharedPtr<OmegaGTE::GEFence>,Composition::Rect)>
        VisualCommand(_Args && ...args):type(Bitmap),params(args...){};

        /// Phase 6.6.2: bitmap with optional sub-rect and RGBA tint.
        VisualCommand(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,
                      const Composition::Rect & rect,
                      Core::Optional<Composition::Rect> sourceRect,
                      Core::Optional<Composition::Color> tintColor):
        type(Bitmap),
        params(img,rect,sourceRect,tintColor){};

        VisualCommand(const LayerEffect::DropShadowParams & shadow,
                      const Composition::Rect & shapeRect,
                      float cornerRadius,
                      bool isEllipse):
        type(Shadow),
        params(shadow,shapeRect,cornerRadius,isEllipse){};

        /// Phase 6.7.2: TextRun visual command.
        VisualCommand(OmegaCommon::Vector<TextSubRun> subRuns,
                      const Composition::Rect & rect,
                      const Composition::Color & color):
        type(TextRun),
        params(std::move(subRuns),rect,color){};

        explicit VisualCommand(const Matrix4x4 & matrix):
        type(SetTransform),
        params(matrix){};

        explicit VisualCommand(float opacity):
        type(SetOpacity),
        params(opacity){};

        ~VisualCommand();

        #undef VISUAL_COMMAND_ARGS_CHECK
    };

    class Layer;

    /// @brief A frozen state of visual items drawn to a Canvas.
    struct CanvasFrame {
        Layer *targetLayer;
        /// Snapshot of the layer rect at the time the frame was recorded.
        /// Previously a live reference to Layer::surface_rect, which caused
        /// a size mismatch when the layer resized between paint and execution.
        Composition::Rect rect;
        /// Position of the owning View relative to the window origin.
        /// Set by Canvas::nextFrame() at paint time so the backend can
        /// render this frame's commands at the correct offset within the
        /// window's single shared surface (Phase 3, Native View Architecture).
        Composition::Point2D windowOffset {0.f, 0.f};
        struct {
            float r = 0.f,g = 0.f,b = 0.f,a = 0.f;
        } background;
        OmegaCommon::Vector<VisualCommand> currentVisuals;
        OmegaCommon::Vector<CanvasEffect> currentEffects;
    };
   

    /**
     @brief Renders 2D vector graphics to a Layer.
    */
    class OMEGAWTK_EXPORT Canvas : public CompositorClient {

        Composition::Rect & rect;

        SharedHandle<CanvasFrame> current;

        Layer &layer;

        /// Non-owning back-pointer to the View that created this Canvas.
        /// Used to compute windowOffset at paint time (Phase 3).
        ::OmegaWTK::View *ownerView_;

        friend class ::OmegaWTK::View;
        friend class ::OmegaWTK::AppWindow;

        Canvas(CompositorClientProxy &proxy,Layer &layer,::OmegaWTK::View *owner);

    public:
        OMEGACOMMON_CLASS("OmegaWTK.Composition.Canvas")

        Layer & getCorrespondingLayer();
        /**
         @brief Draw a Path.
         */
        void drawPath(Path & path);

        /**
         @brief Draw a Path with a fill and an optional border in a single
         draw call.
         @param path The Path. Its `pathBrush` is used as the fill brush
         (a null `pathBrush` means no fill).
         @param border Optional border (brush + width) drawn as the stroke
         band along the path silhouette. Routed through the same
         dual-attachment tessellation pass as the fill.
         */
        void drawPath(Path & path, Core::Optional<Border> border);

        /**
         @brief Draw a Rect.
         @param rect The Rect.
         @param brush The Brush to fill the Rect with.
         @param border Optional border (brush + width) drawn on top of the fill.
         */
        void drawRect(Composition::Rect & rect,
                      Core::SharedPtr<Brush> & brush,
                      Core::Optional<Border> border = std::nullopt);

        /**
         @brief Draw a Rounded Rect.
         @param rect The Rounded Rect.
         @param brush The Brush to fill the Rect with.
         @param border Optional border (brush + width) drawn on top of the fill.
         */
        void drawRoundedRect(Composition::RoundedRect & rect,
                             Core::SharedPtr<Brush> & brush,
                             Core::Optional<Border> border = std::nullopt);

        /**
         @brief Draw an Ellipse.
         @param ellipse The Ellipse.
         @param brush The Brush to fill the Ellipse with.
         @param border Optional border (brush + width) drawn on top of the fill.
         */
        void drawEllipse(Composition::Ellipse & ellipse,
                         Core::SharedPtr<Brush> & brush,
                         Core::Optional<Border> border = std::nullopt);

        /**
         @brief Draw a single straight line segment.
         @param from Start point in canvas space.
         @param to End point in canvas space.
         @param brush Stroke brush.
         @param strokeWidth Stroke width in pixels (must be > 0).
         */
        void drawLine(Composition::Point2D from,
                      Composition::Point2D to,
                      Core::SharedPtr<Brush> & brush,
                      float strokeWidth = 1.f);

        /**
         @brief Draw a connected polyline through a sequence of points.
         @param points The vertex list. Needs at least two points.
         @param strokeBrush Stroke brush. Pass nullptr (with no fill) to skip.
         @param strokeWidth Stroke width in pixels.
         @param closed If true, connect the last point back to the first.
         @param fillBrush Optional fill brush; only meaningful when the
         polyline is closed (or self-closing).
         */
        void drawPolyline(const OmegaCommon::Vector<Composition::Point2D> & points,
                          Core::SharedPtr<Brush> & strokeBrush,
                          float strokeWidth,
                          bool closed = false,
                          Core::Optional<Core::SharedPtr<Brush>> fillBrush = std::nullopt);

        /**
         @brief Draw text into a rectangle.
         @param text The UTF text content.
         @param font The font resource to use.
         @param rect The text bounds in canvas space.
         @param color The text color.
         @param layoutDesc Text alignment/wrapping settings.
         */
        void drawText(const OmegaCommon::UniString & text,
                      Core::SharedPtr<Font> font,
                      const Composition::Rect & rect,
                      const Color & color,
                      const TextLayoutDescriptor & layoutDesc);

        /**
         @brief Draw text into a rectangle using default top-left/no-wrap layout.
         @param text The UTF text content.
         @param font The font resource to use.
         @param rect The text bounds in canvas space.
         @param color The text color.
         */
        void drawText(const OmegaCommon::UniString & text,
                      Core::SharedPtr<Font> font,
                      const Composition::Rect & rect,
                      const Color & color);

        /**
            @brief Draw an Image to the corresponding rect.
            @param img The Image.
            @param rect The Rect to bound the image to.
           */
        void drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> & img,const Composition::Rect & rect);

        /**
         @brief Draw a sub-region of an Image with an optional RGBA tint
         (Phase 6.6.2).
         @param img The source bitmap.
         @param destRect The destination rect on the canvas.
         @param sourceRect Sub-rect of the bitmap to sample, in *texture
         pixels* (e.g. `{ 64, 0, 32, 32 }` for the second-row first
         icon of a 32×32 sprite atlas).
         @param tintColor Optional RGBA tint multiplied onto the sampled
         color. Identity `(1,1,1,1)` is straight passthrough; useful for
         recoloring monochrome icons or applying an alpha fade.
         */
        void drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> & img,
                       const Composition::Rect & destRect,
                       Core::Optional<Composition::Rect> sourceRect,
                       Core::Optional<Composition::Color> tintColor = std::nullopt);

        /**
         @brief Draw a nine-slice (resizable) bitmap (Phase 6.6.3).
         @param img The source bitmap.
         @param destRect The destination rect on the canvas. Must be at
         least as large as the sum of the corresponding insets in each
         dimension; otherwise edges / center collapse to zero size.
         @param insets Edge widths in *texture pixels* (against
         `sourceRect` when provided, otherwise the full texture). The
         four corners draw at fixed size; the four edges stretch in one
         dimension; the center stretches in both.
         @param sourceRect Optional sub-rect of the bitmap to slice.
         When unset the full texture is used.
         @param tintColor Optional RGBA tint applied uniformly to all
         nine slices.

         The implementation decomposes the request into nine `Bitmap`
         visual commands; the backend pipeline is unchanged.
         */
        void drawImage(SharedHandle<OmegaCommon::Img::BitmapImage> & img,
                       const Composition::Rect & destRect,
                       const NineSliceInsets & insets,
                       Core::Optional<Composition::Rect> sourceRect = std::nullopt,
                       Core::Optional<Composition::Color> tintColor = std::nullopt);

        /**
           @brief Draw a GETexture to the corresponding rect.
           @param img The GETExture.
           @param rect The Rect to bound the image to.
           @param fence
          */
        void drawGETexture(SharedHandle<OmegaGTE::GETexture> & img,const Composition::Rect & rect,SharedHandle<OmegaGTE::GEFence> fence = nullptr);

        /**
           @brief Apply an effect to the current frame.
           @param effect The CanvasEffect to apply.
          */
        void applyEffect(SharedHandle<CanvasEffect> & effect);

        /**
           @brief Apply a layer-level effect (e.g. DropShadow/Transformation) to this canvas' layer.
           @param effect The LayerEffect to apply.
          */
        void applyLayerEffect(const SharedHandle<LayerEffect> & effect);

        /**
         @brief Draw a drop shadow for a rect shape (draw-time, inline geometry).
         @param rect The shape to shadow.
         @param shadow Shadow parameters (offset, blur, color, opacity).
         */
        void drawShadow(Composition::Rect & rect,
                        const LayerEffect::DropShadowParams & shadow);

        /**
         @brief Draw a drop shadow for a rounded rect shape.
         */
        void drawShadow(Composition::RoundedRect & rect,
                        const LayerEffect::DropShadowParams & shadow);

        /**
         @brief Draw a drop shadow for an ellipse shape.
         */
        void drawShadow(Composition::Ellipse & ellipse,
                        const LayerEffect::DropShadowParams & shadow);

        /**
         @brief Emit a pre-shaped MSDF `TextRun` visual command.
         @param subRuns One entry per resolved face after layout-engine
         fallback. Shaping is the caller's responsibility — this
         method does no layout, no fallback, and no atlas residency
         work. Used by `DisplayListReplay` to dispatch a
         `DrawOp::TextRun` without re-running shaping; the high-level
         `drawText` path remains the right entry for unshaped UTF
         input.
         @param rect Bounds rect for the run (used by the backend as
         the draw region for atlas-textured quad batches).
         @param color Foreground color.
         */
        void drawTextRun(OmegaCommon::Vector<TextSubRun> subRuns,
                         const Composition::Rect & rect,
                         const Composition::Color & color);

        /**
         @brief Set the per-element transform matrix for subsequent draw calls.
         @param matrix 4x4 transform matrix. Use Identity to reset.
         */
        void setElementTransform(const Matrix4x4 & matrix);

        /**
         @brief Set the per-element opacity for subsequent draw calls.
         @param opacity Opacity scalar [0..1]. Use 1.0 to reset.
         */
        void setElementOpacity(float opacity);

        /**
         @brief Set the background color for the current frame.
         @param color The background color (used where no draw command covers a pixel).
         */
        void setBackground(const Color & color);

        /**
         @brief Clear the current frame's visual commands and optionally set a background color.
         @param color If provided, set as the new background; otherwise reset to transparent.
         */
        void clear(Core::Optional<Color> color = std::nullopt);

        /// @brief Sends current frame to CompositorClientProxy to be drawn.
        void sendFrame();
        /// @brief Retrives current Frame drawn.
        SharedHandle<CanvasFrame> getCurrentFrame();
        /// @brief Retrives current Frame drawn and resets Canvas state.
        SharedHandle<CanvasFrame> nextFrame();

        ~Canvas();
    };


    
    
// #define __COMPOSITION__ ::OmegaWTK::Composition::

// #define VISUAL_RECT(rect,brush) __COMPOSITION__ Visual::RectParams({rect,brush,{}})
// #define VISUAL_RECT_W_FRAME(rect,brush,border) __COMPOSITION__ Visual::RectParams({rect,brush,border})

// #define VISUAL_ROUNDED_RECT(rect,brush) __COMPOSITION__ Visual::RoundedRectParams({rect,rect.radius_x,rect.radius_y,brush,{}})
// #define VISUAL_ROUNDED_RECT_W_FRAME(rect,brush,border) __COMPOSITION__ Visual::RoundedRectParams({rect,rect.radius_x,rect.radius_y,brush,border})
    
// #define VISUAL_TEXT(textRectRef,brush) __COMPOSITION__ Visual::TextParams({textRectRef,brush})
// #define VISUAL_IMG(img,rect) __COMPOSITION__ Visual::BitmapParams({img,rect})
    
//#undef __COMPOSITION__
    // class BackendImpl;
    
    // class OMEGAWTK_EXPORT OMEGAWTK_DEPRECATED  LayerStyle {
    //     Core::Vector<Visual *> visuals;
    //     Core::Vector<SharedHandle<LayerEffect>> effects;
    //     Color background = Color(Color::White);
    //     template<class _Ty>
    //     void _construct_visual(Visual::Type type,_Ty & params){
    //         visuals.push_back(new Visual(visuals.size(),type,(void *)new _Ty(params)));
    //     };
    //     friend class BackendImpl;
    // public:
    //     LayerStyle();
    //     /**
    //      Adds A Rect to the Style!
    //      */
    //     void add(Visual::RectParams params);
    //     /**
    //      Adds A Rounded Rect to the Style!
    //      */
    //     void add(Visual::RoundedRectParams params);
    //     /**
    //      Adds An Ellipse to the Style!
    //      */
    //     void add(Visual::EllipseParams params);
    //     /**
    //      Adds A Bitmap to the Style!
    //      */
    //     void add(Visual::BitmapParams params);
    //     /**
    //      Adds A Text Object to the Style!
    //      */
    //     void add(Visual::TextParams params);
    //     /**
    //     Changes/Sets the brush for the visual at the provided index.
    //     */
    //     void setBrush(unsigned id,const Core::SharedPtr<Brush> & new_brush);
    //     void setBackgroundColor(const Color & color){ background = color;};
    //     void addEffect(SharedHandle<LayerEffect> & effect);
    //     template<class _Ty>
    //     _Ty * getVisualAtIndex(unsigned idx);


    //     template<>
    //     Visual::BitmapParams * getVisualAtIndex(unsigned idx)
    //     {
    //         return (Visual::BitmapParams *)visuals[idx]->params;
    //     };

    //     template<>
    //     Visual::EllipseParams * getVisualAtIndex(unsigned idx)
    //     {
    //         return (Visual::EllipseParams *)visuals[idx]->params;
    //     };

    //     template<>
    //     Visual::RectParams * getVisualAtIndex(unsigned idx)
    //     {
    //         return (Visual::RectParams *)visuals[idx]->params;
    //     };

    //     template<>
    //     Visual::RoundedRectParams * getVisualAtIndex(unsigned idx)
    //     {
    //         return (Visual::RoundedRectParams *)visuals[idx]->params;
    //     };

    //     template<>
    //     Visual::TextParams * getVisualAtIndex(unsigned idx)
    //     {
    //         return (Visual::TextParams *)visuals[idx]->params;
    //     };
        
    //     template<class _Ty>
    //     _Ty * operator[](unsigned idx){
    //         return getVisualAtIndex<_Ty>(idx);
    //     };
    //     ~LayerStyle();
    // };
    
    // class OMEGAWTK_EXPORT OMEGAWTK_DEPRECATED  WindowStyle {
    //     Color bkgrd;
    //     unsigned frameWidth;
    //     unsigned frameHeight;
    //     Core::String title;
    // public:
    //     ~WindowStyle();
    // };
    
   
    // class OMEGAWTK_EXPORT OMEGAWTK_DEPRECATED  MenuStyle {
        
    // };
    
};
    
};

#endif
