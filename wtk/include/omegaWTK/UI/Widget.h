
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/UI/Layout.h"
#include <cstdint>

#ifndef OMEGAWTK_UI_WIDGET_H
#define OMEGAWTK_UI_WIDGET_H

namespace OmegaWTK {

namespace Composition {
    class LayerTree;
}

class AppWindow;
class AppWindowManager;

class View;
OMEGACOMMON_SHARED_CLASS(View);
class CanvasView;
OMEGACOMMON_SHARED_CLASS(CanvasView);
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
    Core::Rect requested {};
    GeometryChangeReason reason = GeometryChangeReason::ChildRequest;
};

struct PaintOptions {
    bool autoWarmupOnInitialPaint = true;
    uint8_t warmupFrameCount = 2;
    bool coalesceInvalidates = true;
    bool invalidateOnResize = true;
};

class OMEGAWTK_EXPORT WidgetGeometryDelegate {
public:
    virtual ~WidgetGeometryDelegate() = default;
    virtual Core::Rect clampChildRect(Widget & parent,
                                      Widget & child,
                                      const GeometryProposal & proposal) = 0;
    virtual void onChildRectCommitted(Widget & parent,
                                      Widget & child,
                                      const Core::Rect & oldRect,
                                      const Core::Rect & newRect,
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
 @paragraph

 @see AppWindow
*/
class OMEGAWTK_EXPORT  Widget : public Native::NativeThemeObserver {
public:
    struct GeometryTraceContext {
        std::uint64_t syncLaneId = 0;
        std::uint64_t predictedPacketId = 0;
    };
private:
    bool initialDrawComplete = false;
    bool hasMounted = false;
    bool paintInProgress = false;
    bool hasPendingInvalidate = false;
    PaintReason pendingPaintReason = PaintReason::StateChanged;
    PaintMode mode = PaintMode::Automatic;
    PaintOptions options {};

    LayoutStyle layoutStyle_ {};
    LayoutBehaviorPtr layoutBehavior_ = nullptr;
    bool hasExplicitLayoutStyle_ = false;

    void onThemeSetRecurse(Native::ThemeDesc &desc);
    void executePaint(PaintReason reason,bool immediate);
    void handleHostResize(const Core::Rect & rect);

    using Native::NativeThemeObserver::onThemeSet;
protected:

    ViewPtr view;
    Widget *parent = nullptr;
    /**
     The WidgetTreeHost that hosts this widget.
    */
    WidgetTreeHost *treeHost = nullptr;
    void setTreeHostRecurse(WidgetTreeHost *host);

private:
    /// Observers
    OmegaCommon::Vector<WidgetObserverPtr> observers;
protected:
    typedef enum : OPT_PARAM {
        Resize,
        Show,
        Hide,
        Detach,
        Attach
    } WidgetEventType;
    struct WidgetEventParams {
        WidgetPtr widget;
        Core::Rect rect;
    };
    void notifyObservers(WidgetEventType eventType,WidgetEventParams params);

    virtual void onMount(){};
    virtual void onPaint(PaintReason reason){};
    virtual Core::Rect clampChildRect(const Widget & child,const GeometryProposal & proposal) const;
    virtual void onChildRectCommitted(const Widget & child,
                                      const Core::Rect & oldRect,
                                      const Core::Rect & newRect,
                                      GeometryChangeReason reason);
    virtual MeasureResult measureSelf(const LayoutContext & ctx);
    virtual void onLayoutResolved(const Core::Rect & finalRectPx);
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
    Core::Rect & rect();
    void setRect(const Core::Rect & newRect);
    bool requestRect(const Core::Rect & requested,
                     GeometryChangeReason reason = GeometryChangeReason::ChildRequest);

    void setLayoutStyle(const LayoutStyle & style);
    const LayoutStyle & layoutStyle() const;
    void setLayoutBehavior(LayoutBehaviorPtr behavior);
    LayoutBehaviorPtr layoutBehavior() const;
    void requestLayout();
    bool hasExplicitLayoutStyle() const;
    View & viewRef();
    virtual OmegaCommon::Vector<Widget *> childWidgets() const;
    virtual bool isLayoutResizable() const {
        return true;
    }
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
    void invalidate(PaintReason reason = PaintReason::StateChanged);
    void invalidateNow(PaintReason reason = PaintReason::StateChanged);
    // bool & isResizable();
    virtual void resize(Core::Rect & newRect){
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
    explicit Widget(Core::Rect rect);
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
    SharedHandle<T> makeSubView(const Core::Rect & rect, Args&&... args) {
        return SharedHandle<T>(new T(rect, view, std::forward<Args>(args)...));
    }

    friend class Container;
public:
    ~Widget() override;
};

/**
* Every Widget Constructor comes with one default parameter: The rect.
 @note These macros are used on subclasses of Widget (Widgets that have real implementation rules,
 so that users don't have to specify the View as the Widget subclass already handles it.
 Parent attachment happens afterward via Container::addChild, never at construction.
*/
#define WIDGET_CONSTRUCTOR(...) static SharedHandle<Widget> Create(Core::Rect rect,## __VA_ARGS__);
#define WIDGET_CONSTRUCTOR_IMPL(...) Create(Core::Rect rect,## __VA_ARGS__)



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
    INTERFACE_METHOD void onWidgetChangeSize(Core::Rect oldRect,Core::Rect & newRect) DEFAULT;
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
