
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Composition/Canvas.h"
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
typedef View CanvasView;
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

struct PaintOptions {
    bool autoWarmupOnInitialPaint = true;
    uint8_t warmupFrameCount = 2;
    bool coalesceInvalidates = true;
    bool invalidateOnResize = true;
};

class OMEGAWTK_EXPORT PaintContext {
    Widget *widget = nullptr;
    SharedHandle<Composition::Canvas> mainCanvas;
    PaintReason paintReason = PaintReason::StateChanged;
    PaintContext(Widget *widget,SharedHandle<Composition::Canvas> mainCanvas,PaintReason reason);
    friend class Widget;
public:
    const Core::Rect & bounds() const;
    PaintReason reason() const;
    Composition::Canvas & rootCanvas();
    SharedHandle<Composition::Canvas> makeCanvas(SharedHandle<Composition::Layer> & targetLayer);
    void clear(const Composition::Color & color);
    void drawRect(const Core::Rect & rect,const SharedHandle<Composition::Brush> & brush);
    void drawRoundedRect(const Core::RoundedRect & rect,const SharedHandle<Composition::Brush> & brush);
    void drawImage(const SharedHandle<Media::BitmapImage> & img,const Core::Rect & rect);
};


/**
 @brief A singular moduler UI component. (Consists usually of one view)
 Can be attached to a WidgetTreeHost or another Widget as a child.
 @paragraph

 @see AppWindow
*/
class OMEGAWTK_EXPORT  Widget : public Native::NativeThemeObserver {
    bool initialDrawComplete = false;
    bool paintInProgress = false;
    bool hasPendingInvalidate = false;
    PaintReason pendingPaintReason = PaintReason::StateChanged;
    PaintMode mode = PaintMode::Automatic;
    PaintOptions options {};

    SharedHandle<Composition::Canvas> rootPaintCanvas;

    void onThemeSetRecurse(Native::ThemeDesc &desc);
    SharedHandle<Composition::Canvas> getRootPaintCanvas();
    void executePaint(PaintReason reason,bool immediate);
    void handleHostResize(const Core::Rect & rect);

    using Native::NativeThemeObserver::onThemeSet;
protected:

    SharedHandle<CanvasView> rootView;
    Widget *parent = nullptr;
    SharedHandle<Composition::LayerTree> layerTree;
    /**
     The WidgetTreeHost that hosts this widget.
    */
    WidgetTreeHost *treeHost = nullptr;
        /**
     Makes a Canvas View attached to this widget and returns it.
     @param rect The Rectangle to use
     @param parent The Parent View (NOTE: This view MUST be within this widget's view heirarchy)
     @returns A standard View
     */
    CanvasViewPtr makeCanvasView(const Core::Rect & rect,ViewPtr parent);

    /**
     Makes a Scroll View attached to this widget and returns it.
     @param rect The Rectangle to use
     @param child The child view to clip and scroll
     @param hasVerticalScrollBar Enable vertical scrollbar
     @param hasHorizontalScrollBar Enable horizontal scrollbar
     @param parent The parent view in this widget view hierarchy
     @returns A Scroll View
     */
    ScrollViewPtr makeScrollView(const Core::Rect & rect,
                                 ViewPtr child,
                                 bool hasVerticalScrollBar,
                                 bool hasHorizontalScrollBar,
                                 ViewPtr parent);

    //    /**
    //  Makes a Canvas View attached to this widget and returns it.
    //  @param rect The Rectangle to use
    //  @param parent The Parent View (NOTE: This view MUST be within this widget's view heirarchy)
    //  @returns A standard View
    //  */
    // TextViewPtr makeTextView(const Core::Rect & rect,View *parent);

    /**
     Makes an SVG View attached to this widget and returns it.
     @param rect The Rectangle to use
     @param parent The Parent View (NOTE: This view MUST be within this widget's view heirarchy)
     @returns A Video View
     */
    SVGViewPtr makeSVGView(const Core::Rect & rect,ViewPtr parent);

    /**
     Makes a Video View attached to this widget and returns it.
     @param rect The Rectangle to use
     @param parent The Parent View (NOTE: This view MUST be within this widget's view heirarchy)
     @returns A Video View
     */
    VideoViewPtr makeVideoView(const Core::Rect & rect,ViewPtr parent);

     /**
     Makes a UI View attached to this widget and returns it.
     @param rect The Rectangle to use
     @param parent The Parent View (NOTE: This view MUST be within this widget's view heirarchy)
     @returns A Video View
     */
    UIViewPtr makeUIView(const Core::Rect & rect,ViewPtr parent,UIViewTag tag = "");
    
private:
    OmegaCommon::Vector<Widget *> children;
    void setTreeHostRecurse(WidgetTreeHost *host);
    void removeChildWidget(Widget *ptr);
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
    virtual void onPaint(PaintContext & context,PaintReason reason){};

    /**
    @brief Initial render of the Widget
    @note Manual-mode widgets can still override this to use low-level composition APIs.*/
    virtual void init();
private:
    void setParentWidgetImpl(Widget *widget,WidgetPtr widgetHandle);
    friend class AppWindow;
    friend class AppWindowManager;
    friend class WidgetTreeHost;
    friend class PaintContext;
public:
    OMEGACOMMON_CLASS("OmegaWTK.Widget")
    /**
     Get the Widget's root View's rect
    */
    Core::Rect & rect();
    void setRect(const Core::Rect & newRect);
    virtual bool isLayoutResizable() const {
        return true;
    }
    void setParentWidget(WidgetPtr widget);
    void setParentWidget(Widget *widget);
    void detachFromParent();
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
    Widget(const Core::Rect & rect,WidgetPtr parent);
public:
    ~Widget() override;
};

// #define WIDGET_TEMPLATE_BEGIN()
// #define WIDGET_TEMPLATE_VIEW(class_name,...)
// #define WIDGET_TEMPLATE_END()


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
