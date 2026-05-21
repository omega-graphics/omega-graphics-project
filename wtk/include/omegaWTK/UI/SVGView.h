#ifndef OMEGAWTK_UI_SVGVIEW_H
#define OMEGAWTK_UI_SVGVIEW_H

#include "View.h"
#include "omegaWTK/Composition/DisplayList.h"
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

/**
 @brief Parses and renders SVG documents to a Canvas.

 UIView-Render-Redesign-Plan Tier 2 Phase 2.3: the parsed document is
 cached as a `Composition::DisplayList` rebuilt from `sourceDoc_` on
 each `setSource*` call. Tier 3 Phase 3.8: `paint()` submits the list
 to the window-scoped `FrameBuilder` bracketing the paint pass.
 Brushes and borders are resolved at parse time, not at paint time.
*/
class OMEGAWTK_EXPORT SVGView : public View {
    SVGViewDelegate *delegate_ = nullptr;
    SVGViewRenderOptions options_ {};
    Core::Optional<Core::XMLDocument> sourceDoc_;
    Core::UniquePtr<Composition::DisplayList> cachedOps_;
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

    /// Replay the parsed DisplayList into the SVGView's canvas and
    /// submit the frame. Replaces the prior `renderNow()` entry point
    /// (Phase 2.3); naming aligns with the SceneNode::paint contract
    /// the SceneNode-era model in §3.8 will adopt.
    void paint();

    void resize(Composition::Rect newRect) override;
};

}

#endif
