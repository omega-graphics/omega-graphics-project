#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Media/ImgCodec.h"
#include "omegaWTK/Widgets/WidgetTypes.h"

#ifndef OMEGAWTK_WIDGETS_PRIMATIVES_H
#define OMEGAWTK_WIDGETS_PRIMATIVES_H

namespace OmegaWTK {

// --- Display Primitives (Phase 1) ---

struct OMEGAWTK_EXPORT RectangleProps {
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    float strokeWidth = 0.f;
};

class OMEGAWTK_EXPORT Rectangle : public Widget {
    RectangleProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Rectangle(Core::Rect rect, const RectangleProps & props = {});
    void setProps(const RectangleProps & props);
};

struct OMEGAWTK_EXPORT RoundedRectangleProps {
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    float strokeWidth = 0.f;
    float topLeft = 0.f;
    float topRight = 0.f;
    float bottomLeft = 0.f;
    float bottomRight = 0.f;
};

class OMEGAWTK_EXPORT RoundedRectangle : public Widget {
    RoundedRectangleProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit RoundedRectangle(Core::Rect rect, const RoundedRectangleProps & props = {});
    void setProps(const RoundedRectangleProps & props);
};

struct OMEGAWTK_EXPORT EllipseProps {
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    float strokeWidth = 0.f;
};

class OMEGAWTK_EXPORT Ellipse : public Widget {
    EllipseProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Ellipse(Core::Rect rect, const EllipseProps & props = {});
    void setProps(const EllipseProps & props);
};

struct OMEGAWTK_EXPORT PathProps {
    OmegaGTE::GVectorPath2D path {OmegaGTE::GPoint2D{0.f, 0.f}};
    SharedHandle<Composition::Brush> fill = nullptr;
    SharedHandle<Composition::Brush> stroke = nullptr;
    unsigned strokeWidth = 1;
    bool closePath = false;
};

class OMEGAWTK_EXPORT Path : public Widget {
    PathProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Path(Core::Rect rect, const PathProps & props);
    void setProps(const PathProps & props);
};

struct OMEGAWTK_EXPORT SeparatorProps {
    Orientation orientation = Orientation::Horizontal;
    float thickness = 1.f;
    float inset = 0.f;
    SharedHandle<Composition::Brush> brush = nullptr;
};

class OMEGAWTK_EXPORT Separator : public Widget {
    SeparatorProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Separator(Core::Rect rect, const SeparatorProps & props = {});
    void setProps(const SeparatorProps & props);
};

// --- Text and Image Primitives (Phase 2) ---

struct OMEGAWTK_EXPORT LabelProps {
    OmegaCommon::UString text {};
    SharedHandle<Composition::Font> font = nullptr;
    Composition::Color textColor {0.f, 0.f, 0.f, 1.f};
    Composition::TextLayoutDescriptor::Alignment alignment =
        Composition::TextLayoutDescriptor::LeftUpper;
    Composition::TextLayoutDescriptor::Wrapping wrapping =
        Composition::TextLayoutDescriptor::WrapByWord;
    unsigned lineLimit = 0;
};

class OMEGAWTK_EXPORT Label : public Widget {
    LabelProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
    MeasureResult measureSelf(const LayoutContext & ctx) override;
public:
    explicit Label(Core::Rect rect, const LabelProps & props = {});
    void setText(const OmegaCommon::UString & text);
    void setProps(const LabelProps & props);
};

/// Icon widget. Currently renders the token string as a glyph via UIView.
/// Phase 2B will add image-based and SVG-based icon rendering.
struct OMEGAWTK_EXPORT IconProps {
    OmegaCommon::String token {};
    float size = 16.f;
    Composition::Color tintColor {0.f, 0.f, 0.f, 1.f};
};

class OMEGAWTK_EXPORT Icon : public Widget {
    IconProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Icon(Core::Rect rect, const IconProps & props = {});
    void setProps(const IconProps & props);
};

enum class ImageFitMode : uint8_t {
    Contain,
    Cover,
    Fill,
    Center,
    Crop
};

struct OMEGAWTK_EXPORT ImageProps {
    SharedHandle<Media::BitmapImage> source = nullptr;
    ImageFitMode fitMode = ImageFitMode::Contain;
};

class OMEGAWTK_EXPORT Image : public Widget {
    ImageProps props_;
protected:
    void onThemeSet(Native::ThemeDesc & desc) override { (void)desc; }
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    explicit Image(Core::Rect rect, const ImageProps & props = {});
    void setProps(const ImageProps & props);
};

}

#endif //OMEGAWTK_WIDGETS_PRIMATIVES_H
