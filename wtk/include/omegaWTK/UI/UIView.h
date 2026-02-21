#include "Widget.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Animation.h"
#include "View.h"

#ifndef OMEGAWTK_UI_UIVIEW_H
#define OMEGAWTK_UI_UIVIEW_H

namespace OmegaWTK {

typedef OmegaCommon::String UIElementTag;
typedef UIElementTag UIViewTag;

struct StyleSheet;
OMEGACOMMON_SHARED_CLASS(StyleSheet);

/**
 * @brief A generic shape descriptor for UIView elements.
 * @note Slice A supports Rect and RoundedRect rendering.
 */
struct OMEGAWTK_EXPORT Shape {
    enum class Type : uint8_t {
        Rect,
        RoundedRect,
        Ellipse,
        Path
    };

    Type type = Type::Rect;
    Core::Rect rect {};
    Core::RoundedRect roundedRect {};
    OmegaGTE::GEllipsoid ellipse {};
    Core::Optional<OmegaGTE::GVectorPath2D> path {};

    static Shape Scalar(int width,int height);
    static Shape Rect(const Core::Rect & rect);
    static Shape RoundedRect(const Core::RoundedRect & rect);
    static Shape Ellipse(const OmegaGTE::GEllipsoid & ellipse);
    static Shape Path(const OmegaGTE::GVectorPath2D & path);
};

enum ElementAnimationKey : int {
    ElementAnimationKeyColorRed,
    ElementAnimationKeyColorGreen,
    ElementAnimationKeyColorBlue,
    ElementAnimationKeyColorAlpha,
    ElementAnimationKeyWidth,
    ElementAnimationKeyHeight,
    ElementAnimationKeyPathNodeX,
    ElementAnimationKeyPathNodeY,
};

struct OMEGAWTK_EXPORT StyleSheet {
    struct Entry {
        enum class Kind : uint8_t {
            BackgroundColor,
            BorderEnabled,
            BorderColor,
            BorderWidth,
            ElementBrush,
            ElementBrushAnimation,
            ElementAnimation,
            ElementPathAnimation
        };

        Kind kind = Kind::BackgroundColor;
        UIViewTag viewTag {};
        UIElementTag elementTag {};
        Core::Optional<Composition::Color> color {};
        Core::Optional<bool> boolValue {};
        Core::Optional<float> floatValue {};
        SharedHandle<Composition::Brush> brush = nullptr;
        ElementAnimationKey animationKey = ElementAnimationKeyColorAlpha;
        SharedHandle<Composition::AnimationCurve> curve = nullptr;
        int nodeIndex = -1;
        bool transition = false;
        float duration = 0.f;
    };

    OmegaCommon::Vector<Entry> entries;

    static StyleSheetPtr Create();
    StyleSheetPtr copy();

    StyleSheetPtr backgroundColor(UIViewTag tag,
                                  const Composition::Color & color,
                                  bool transition = false,
                                  float duration = 0.f);

    StyleSheetPtr border(UIViewTag tag,bool use);

    StyleSheetPtr borderColor(UIViewTag tag,
                              const Composition::Color & color,
                              bool transition = false,
                              float duration = 0.f);

    StyleSheetPtr borderWidth(UIViewTag tag,
                              float width,
                              bool transition = false,
                              float duration = 0.f);

    StyleSheetPtr elementBrush(UIElementTag elementTag,
                               SharedHandle<Composition::Brush> brush,
                               bool transition = false,
                               float duration = 0.f);

    StyleSheetPtr elementBrushAnimation(SharedHandle<Composition::Brush> brush,
                                        ElementAnimationKey key,
                                        SharedHandle<Composition::AnimationCurve> curve,
                                        float duration);

    StyleSheetPtr elementAnimation(UIElementTag elementTag,
                                   ElementAnimationKey key,
                                   SharedHandle<Composition::AnimationCurve> curve,
                                   float duration);

    StyleSheetPtr elementPathAnimation(UIElementTag elementTag,
                                       SharedHandle<Composition::AnimationCurve> curve,
                                       int nodeIndex,
                                       float duration);

    StyleSheet();
    ~StyleSheet() = default;
};

class UIViewLayout;
typedef SharedHandle<UIViewLayout> UIViewLayoutPtr;

class UIViewLayout {
public:
    struct Element {
        enum class Type : uint8_t {
            Text,
            Shape
        };

        Type type = Type::Text;
        UIElementTag tag;
        Core::Optional<OmegaCommon::UString> str;
        Core::Optional<Shape> shape;
    };

private:
    OmegaCommon::Vector<Element> _content;
public:
    void text(UIElementTag tag,OmegaCommon::UString content);
    void shape(UIElementTag tag,const Shape & shape);
    bool remove(UIElementTag tag);
    void clear();
    const OmegaCommon::Vector<Element> & elements() const;
};

class UIRenderer {
protected:
    struct RenderTargetBundle {
        SharedHandle<Composition::Layer> layer;
        SharedHandle<Composition::Canvas> canvas;
    };

    int framesPerSec = 60;
    UIView *view;
    OmegaCommon::Map<UIElementTag,RenderTargetBundle> renderTargetStore;
    RenderTargetBundle & buildLayerRenderTarget(UIElementTag tag);
public:
    explicit UIRenderer(UIView *view);
    void handleElement(UIElementTag tag);
    void handleTransition(UIElementTag tag,ElementAnimationKey k,float duration);
    void handleAnimation(UIElementTag tag,
                         ElementAnimationKey k,
                         float duration,
                         SharedHandle<Composition::AnimationCurve> & curve);
};

class OMEGAWTK_EXPORT UIView : public CanvasView, UIRenderer {
    UIViewTag tag;
    UIViewLayout currentLayout;
    StyleSheetPtr currentStyle;
    bool layoutDirty = true;
    bool styleDirty = true;
    SharedHandle<Composition::Canvas> rootCanvas;
public:
    explicit UIView(const Core::Rect & rect,Composition::LayerTree *layerTree,ViewPtr parent,UIViewTag tag);
    UIViewLayout & layout();
    void setLayout(const UIViewLayout & layout);
    void setStyleSheet(const StyleSheetPtr & style);
    StyleSheetPtr getStyleSheet() const;
    void update();
};

}

#endif // OMEGAWTK_UI_UIVIEW_H
