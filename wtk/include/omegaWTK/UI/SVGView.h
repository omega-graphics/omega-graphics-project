#ifndef OMEGAWTK_UI_SVGVIEW_H
#define OMEGAWTK_UI_SVGVIEW_H

#include "View.h"
#include "omegaWTK/Core/XML.h"

#include <iosfwd>

namespace OmegaWTK {

typedef Core::XMLDocument SVGDocument;

enum class SVGScaleMode : int { None, Meet, Slice };

struct SVGViewRenderOptions {
    SVGScaleMode scaleMode = SVGScaleMode::Meet;
    bool antialias = true;
    bool enableAnimation = true;
};

class OMEGAWTK_EXPORT SVGViewDelegate {
public:
    virtual void onSVGLoaded() {}
    virtual void onSVGParseError(const OmegaCommon::String & message) {}
    virtual ~SVGViewDelegate() = default;
};

struct SVGDrawOpList;

/**
 @brief Parses and renders SVG documents to a Canvas.
*/
class OMEGAWTK_EXPORT SVGView : public View {
    SharedHandle<Composition::Canvas> svgCanvas;
    SVGViewDelegate *delegate_ = nullptr;
    SVGViewRenderOptions options_ {};
    Core::Optional<Core::XMLDocument> sourceDoc_;
    Core::UniquePtr<SVGDrawOpList> drawOps_;
    bool needsRebuild_ = true;

    void rebuildDisplayList();
    friend class Widget;
public:
    explicit SVGView(const Composition::Rect & rect,ViewPtr parent);
    OMEGACOMMON_CLASS("OmegaWTK.UI.SVGView")

    ~SVGView();
    void setDelegate(SVGViewDelegate *delegate);
    void setRenderOptions(const SVGViewRenderOptions & options);
    const SVGViewRenderOptions & renderOptions() const;

    bool setSourceDocument(Core::XMLDocument doc);
    bool setSourceString(const OmegaCommon::String & svgString);
    bool setSourceStream(std::istream & stream);

    void renderNow();

    void resize(Composition::Rect newRect) override;
};

}

#endif
