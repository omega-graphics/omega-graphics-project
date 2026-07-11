
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/UI/Layout.h"
#include <cstdint>

#ifndef OMEGAWTK_UI_WIDGET_H
#define OMEGAWTK_UI_WIDGET_H

namespace OmegaWTK {

class AppWindow;
class AppWindowManager;

class View;
OMEGACOMMON_SHARED_CLASS(View);
class ScrollView;
OMEGACOMMON_SHARED_CLASS(ScrollView);
class VideoView;
OMEGACOMMON_SHARED_CLASS(VideoView);
class SVGView;
OMEGACOMMON_SHARED_CLASS(SVGView);
class UIView;
OMEGACOMMON_SHARED_CLASS(UIView);

typedef OmegaCommon::String UIViewTag;

class WidgetObserver;
class WidgetTreeHost;
class Widget;
OMEGACOMMON_SHARED_CLASS(Widget);
OMEGACOMMON_SHARED_CLASS(WidgetObserver);

enum class PaintMode : uint8_t {
    Automatic,
    Manual
};

enum class PaintReason : uint8_t {
    Initial,
    StateChanged,
    Resize,
    ThemeChanged
};

enum class GeometryChangeReason : uint8_t {
    ParentLayout,
    ChildRequest,
    UserInput
};

struct GeometryProposal {
    Composition::Rect requested {};
    GeometryChangeReason reason = GeometryChangeReason::ChildRequest;
};

struct PaintOptions {
    // Default is opt-out: widgets do NOT re-paint when the window
    // resizes. The AppWindow's native item + render target still
    // resize unconditionally (AppWindowDelegate::syncNativePresentLayer
    // runs on every resize dispatch), so the present surface tracks
    // the window dimensions on its own. Widgets that actually depend
    // on logical-size-driven repaint (text reflow, responsive layout,
    // content-rect-dependent paint output) set this to true
    // explicitly via setPaintOptions / their PaintOptions ctor.
    //
    // Widget-View-Paint-Lifecycle-Plan Tier D / D1 (2026-06-03):
    // the legacy `autoWarmupOnInitialPaint`, `warmupFrameCount`, and
    // `coalesceInvalidates` fields are gone. Shaders are pre-compiled
    // (no warmup needed) and coalescing is the always-on Tier A
    // behavior driven by the run-loop primitive, not a per-widget
    // toggle.
    bool invalidateOnResize = false;
};

class OMEGAWTK_EXPORT WidgetGeometryDelegate {
public:
    virtual ~WidgetGeometryDelegate() = default;
    virtual Composition::Rect clampChildRect(Widget & parent,
                                      Widget & child,
                                      const GeometryProposal & proposal) = 0;
    virtual void onChildRectCommitted(Widget & parent,
                                      Widget & child,
                                      const Composition::Rect & oldRect,
                                      const Composition::Rect & newRect,
                                      GeometryChangeReason reason){
        (void)parent;
        (void)child;
        (void)oldRect;
        (void)newRect;
        (void)reason;
    }
};

/**
 @brief A singular moduler UI component. (Consists usually of one view)
 Can be attached to a WidgetTreeHost or another Widget as a child.

 All OmegaWTK coordinates use bottom-left origin.
 @see AppWindow
*/
class OMEGAWTK_EXPORT  Widget : public Native::NativeThemeObserver {
public:
    struct GeometryTraceContext {
        std::uint64_t syncLaneId = 0;
        std::uint64_t predictedPacketId = 0;
    };
private:
    struct Impl;
    Core::UniquePtr<Impl> impl_;

    void onThemeSetRecurse(Native::ThemeDesc &desc);
    void handleHostResize(const Composition::Rect & rect);
    /// Wires `view->onLayoutResolved` to this widget's `resize()` so a
    /// layout-driven size change re-authors content (stretch-to-fill).
    /// Called once from each constructor. See Widget.Core.cpp.
    void subscribeLayoutRebuild();

    using Native::NativeThemeObserver::onThemeSet;
protected:

    ViewPtr view;
    Widget *parent = nullptr;
    /**
     The WidgetTreeHost that hosts this widget.
    */
    WidgetTreeHost *treeHost = nullptr;
    void setTreeHostRecurse(WidgetTreeHost *host);
    typedef enum : OPT_PARAM {
        Resize,
        Show,
        Hide,
        Detach,
        Attach
    } WidgetEventType;
    struct WidgetEventParams {
        WidgetPtr widget;
        Composition::Rect rect;
    };
    void notifyObservers(WidgetEventType eventType,WidgetEventParams params);

    virtual void onMount(){};
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // `virtual void onPaint(PaintReason)` deleted. Phase 4.7.4
    // stopped dispatching it (`FrameBuilder::buildFrame` walks the
    // `View` tree directly and calls each node's
    // `View::paint(PaintContext&)` — Phase 4.7.0); D1 (2026-06-03)
    // then deleted `Widget::executePaint`, removing the last symbol
    // that could have called it. Until D8 the virtual stayed
    // `[[deprecated]]` on the class so the in-tree overrides
    // compiled with a warning trail; D8 deleted every override
    // (`Rectangle`, `RoundedRectangle`, `Ellipse`, `Path`,
    // `Separator`, `Label`, `Icon`, `Image`, `Container`,
    // `StackWidget`) along with this declaration.
    //
    // **Replacement pattern.** Setup that used to live in `onPaint`
    // moves to `onMount()` (called once when the widget attaches to
    // the tree) plus a `resize(Composition::Rect &)` override
    // (called when the widget rect changes). A shared
    // `rebuildContent()` helper called from both is the canonical
    // shape — see `wtk/tests/TextCompositorTest/main.cpp` for the
    // template. External code that still overrides `onPaint` now
    // produces a hard compile error rather than a silent no-op; the
    // diagnostic points the author at the migration pattern.
    virtual Composition::Rect clampChildRect(const Widget & child,const GeometryProposal & proposal) const;
    virtual void onChildRectCommitted(const Widget & child,
                                      const Composition::Rect & oldRect,
                                      const Composition::Rect & newRect,
                                      GeometryChangeReason reason);
    virtual MeasureResult measureSelf(const LayoutContext & ctx);
    virtual void onLayoutResolved(const Composition::Rect & finalRectPx);
    static bool geometryTraceLoggingEnabled();
    GeometryTraceContext geometryTraceContext() const;

    /**
    @brief Initial render of the Widget
    @note Manual-mode widgets can still override this to use low-level composition APIs.*/
    virtual void init();
private:
    friend class AppWindow;
    friend class AppWindowManager;
    friend class WidgetTreeHost;
    friend void runWidgetLayout(Widget & root, const LayoutContext & ctx);
public:
    OMEGACOMMON_CLASS("OmegaWTK.Widget")
    /**
     Geometry ownership boundary:
     1. Widgets/Containers own synchronous geometry policy only.
     2. Composition animation classes are view-owned (`View`/`UIView`) and are
        intentionally not exposed through `Widget`.
    */
    /**
     Get the Widget's root View's rect
    */
    Composition::Rect & rect();
    void setRect(const Composition::Rect & newRect);
    bool requestRect(const Composition::Rect & requested,
                     GeometryChangeReason reason = GeometryChangeReason::ChildRequest);

    void setLayoutStyle(const LayoutStyle & style);
    const LayoutStyle & layoutStyle() const;
    void setLayoutBehavior(LayoutBehaviorPtr behavior);
    LayoutBehaviorPtr layoutBehavior() const;
    void requestLayout();
    bool hasExplicitLayoutStyle() const;
    View & viewRef();
    virtual OmegaCommon::ArrayRef<WidgetPtr> childWidgets();
    /// Whether the layout may rewrite this widget's geometry when its
    /// parent resizes. Intrinsic-sized leaves (shape primitives, text,
    /// Button, the input widgets) return false: a window resize
    /// repositions them but never deforms them. Only a scaleFactor / DPI
    /// change rescales them, and that rides AppWindow's onRealize ->
    /// setRenderScale path, which is decoupled from layout resize. Layout
    /// containers override this to true (see Container). Consumed by
    /// FlexLayout through the per-child FlexChildSpec.resizable flag.
    virtual bool isLayoutResizable() const {
        return false;
    }
    /// Whether the parent layout's explicit cross-axis `Stretch` directive
    /// may widen this widget to the cross extent. Default true: a stretch
    /// is an author directive that applies even to frozen leaves (a
    /// Separator spans the cross axis). A widget that owns its own size and
    /// must not be stretched by the page — a nested scroll viewport, a
    /// fixed-size container — overrides this to false. Consumed by
    /// `FlexLayout` through the per-child `FlexChildSpec.honorCrossStretch`.
    virtual bool layoutCrossStretchAllowed() const {
        return true;
    }
    /// Native-API §2.3a T1: virtual tooltip popup. `setTooltip` gives this
    /// widget a hover tooltip; after the cursor rests over the widget for
    /// the hover delay, `WidgetTreeHost`'s dispatcher shows a small text
    /// overlay near the cursor (rendered through the overlay layer,
    /// `OverlayTier::Tooltip`). An empty string (the default, or after
    /// `clearTooltip`) means no tooltip. The text is stored on the widget;
    /// the overlay is dispatcher-owned and rebuilt each time it shows.
    void setTooltip(const OmegaCommon::String & text);
    /// Remove this widget's tooltip. Equivalent to `setTooltip("")`; also
    /// dismisses the tooltip immediately if it is currently showing for
    /// this widget.
    void clearTooltip();
    /// This widget's current tooltip text (empty when none is set).
    const OmegaCommon::String & tooltip() const;
    /**
     Add a WidgetObserver to be notified.
    */
    void addObserver(WidgetObserverPtr observer);
    /**
     Remove a WidgetObserver from the list of observers currently listening.
     @note RARELY USED
    */
    void removeObserver(WidgetObserverPtr observerPtr);
    void setPaintMode(PaintMode mode);
    PaintMode paintMode() const;
    void setPaintOptions(const PaintOptions & options);
    const PaintOptions & paintOptions() const;
    /// Widget-View-Paint-Lifecycle-Plan Tier A: deferred. Sets the
    /// view's dirty bits and requests a frame; the actual paint runs
    /// at the next frame boundary (coalescing a burst of invalidates
    /// into one frame).
    void invalidate(PaintReason reason = PaintReason::StateChanged);
    /// Forces a synchronous paint instead of deferring to the next
    /// frame. Escape hatch only — prefer invalidate().
    [[deprecated("Synchronous paint bypasses the deferred frame "
                 "lifecycle (Widget-View-Paint-Lifecycle-Plan Tier A). "
                 "Use invalidate() unless you truly need paint to "
                 "complete before this call returns.")]]
    void invalidateNow(PaintReason reason = PaintReason::StateChanged);
    // bool & isResizable();
    virtual void resize(Composition::Rect & newRect){
        // std::cout << "THIS WIDGET IS NOT RESIZABLE" << std::endl;
    };
    /**
     Show the Widget if hidden.
    */
    void show();
    /**
     Hide the Widget if shown
    */
    void hide();
protected:
    explicit Widget(Composition::Rect rect);
    explicit Widget(ViewPtr view);

    /// Returns a typed reference to this widget's root view.
    /// The caller is responsible for ensuring the actual View type matches T.
    template<typename T>
    T & viewAs() { return static_cast<T&>(*view); }

    template<typename T>
    const T & viewAs() const { return static_cast<const T&>(*view); }

    /// Create a subview of type T, automatically wired to this widget's
    /// root view as its parent. The first argument is the rect; the parent
    /// ViewPtr is inserted as the second argument to T's constructor.
    template<typename T, typename... Args>
    SharedHandle<T> makeSubView(const Composition::Rect & rect, Args&&... args) {
        return SharedHandle<T>(new T(rect, view, std::forward<Args>(args)...));
    }

    friend class Container;
    // ScrollableContainer wraps a private inner Container whose
    // `treeHost` the framework walk never sets (it is reached only
    // through `childWidgets()`). The friendship lets
    // `ScrollableContainer::addChild` re-thread the real host into a
    // child added after attach — same access Container already has.
    friend class ScrollableContainer;
public:
    ~Widget() override;
};

/**
* Every Widget Constructor comes with one default parameter: The rect.
 @note These macros are used on subclasses of Widget (Widgets that have real implementation rules,
 so that users don't have to specify the View as the Widget subclass already handles it.
 Parent attachment happens afterward via Container::addChild, never at construction.
*/
#define WIDGET_CONSTRUCTOR(...) static SharedHandle<Widget> Create(Composition::Rect rect,## __VA_ARGS__);
#define WIDGET_CONSTRUCTOR_IMPL(...) Create(Composition::Rect rect,## __VA_ARGS__)



#define WIDGET_NOTIFY_OBSERVERS_SHOW() notifyObservers(Widget::Show,{})
#define WIDGET_NOTIFY_OBSERVERS_HIDE() notifyObservers(Widget::Hide,{})
#define WIDGET_NOTIFY_OBSERVERS_RESIZE(rect) notifyObservers(Widget::Resize,{nullptr,rect})


/** 
 @brief Similar to the concept of a Widget Delegate but a Widget can have more than one.
 @paragraph
*/
class OMEGAWTK_EXPORT  WidgetObserver {
    friend class Widget;
    bool hasAssignment;
protected:
    Widget *widget;
public:
    WidgetObserver();
    /// Implement in subclasses!
    /// Called when the Widget has been attached to a WidgetTree.
    INTERFACE_METHOD void onWidgetAttach(WidgetPtr parent) DEFAULT;
     /// Called when the Widget has been dettached from a WidgetTree.
    INTERFACE_METHOD void onWidgetDetach(WidgetPtr parent) DEFAULT;
    /// Called when the Widget has changed size.
    INTERFACE_METHOD void onWidgetChangeSize(Composition::Rect oldRect,Composition::Rect & newRect) DEFAULT;
    /// Called when the Widget has just been Hidden.
    INTERFACE_METHOD void onWidgetDidHide() DEFAULT;
    /// Called when the Widget has just been Shown.
    INTERFACE_METHOD void onWidgetDidShow() DEFAULT;

    INTERFACE_METHOD ~WidgetObserver() FALLTHROUGH;
};

template<class Ty>
class WidgetState;


template<class State_Ty>
class WidgetStateObserver;

template<class Ty>
class WidgetStateObserver<WidgetState<Ty>> {
protected:
    INTERFACE_METHOD void stateHasChanged(Ty & newVal) DEFAULT;
};


template<class Ty>
class OMEGAWTK_EXPORT WidgetState {
    Core::Optional<Ty> val;
    OmegaCommon::Vector<WidgetStateObserver<WidgetState<Ty>> *> observers;
public: 
    void setValue(Ty newVal){
        val = newVal;
        for(auto & observer : observers){
            observer->stateHasChanged(newVal);
        };
    };
    static SharedHandle<WidgetState<Ty>> Create(Core::Optional<Ty> initalValue = {}){
        SharedHandle<WidgetState<Ty>> rc(new WidgetState<Ty>);
        rc->val = initalValue;
        return rc;
    };

    void addObserver(SharedHandle<WidgetStateObserver<WidgetState<Ty>>> observer){
        observers.push_back(observer.get());
    };

    void addObserver(WidgetStateObserver<WidgetState<Ty>> *observer){
        observers.push_back(observer);
    };

    void removeObserver(WidgetStateObserver<WidgetState<Ty>> *observer){
        auto ob_it = observers.begin();
        while(ob_it != observers.end()){
            if(*ob_it == observer){
                observers.erase(ob_it);
                break;
            };
            ++ob_it;
        };
    };
};



};

#endif
