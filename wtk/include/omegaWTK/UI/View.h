/**
 @file View.h
 
 */

#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeItem.h"

#include <cstdint>
#include <limits>

#ifndef OMEGAWTK_UI_VIEW_H
#define OMEGAWTK_UI_VIEW_H

namespace OmegaWTK {
    namespace Composition {
        class Compositor;
        class CompositorClientProxy;
        class ViewRenderTarget;
        class LayerTree;
        class Layer;
        class Canvas;
        class ViewAnimator;
    }

    namespace Native {
        class NativeEvent;
        typedef SharedHandle<NativeEvent> NativeEventPtr;
    }

    
    class Container;
    class Widget;
    class ViewDelegate;
    class ScrollView;
    class View;
    OMEGACOMMON_SHARED_CLASS(View);

    struct OMEGAWTK_EXPORT ResizeClamp {
        float minWidth = 1.f;
        float minHeight = 1.f;
        float maxWidth = std::numeric_limits<float>::infinity();
        float maxHeight = std::numeric_limits<float>::infinity();
    };

    enum class ChildResizePolicy : std::uint8_t {
        Fixed,
        Fill,
        FitContent,
        Proportional
    };

    struct OMEGAWTK_EXPORT ChildResizeSpec {
        bool resizable = true;
        ChildResizePolicy policy = ChildResizePolicy::FitContent;
        ResizeClamp clamp {};
        float growWeightX = 1.f;
        float growWeightY = 1.f;
    };

    class OMEGAWTK_EXPORT ViewResizeCoordinator {
        struct ChildState {
            ChildResizeSpec spec {};
            Core::Rect baselineParentRect {Core::Position{0.f,0.f},1.f,1.f};
            Core::Rect baselineChildRect {Core::Position{0.f,0.f},1.f,1.f};
            bool hasBaseline = false;
        };
        View * parentView = nullptr;
        std::uint64_t activeSessionId = 0;
        OmegaCommon::Map<View *,ChildState> childState;
    public:
        void attachView(View * parent);
        void registerChild(View * child,const ChildResizeSpec & spec);
        void updateChildSpec(View * child,const ChildResizeSpec & spec);
        void unregisterChild(View * child);
        void beginResizeSession(std::uint64_t sessionId);
        Core::Rect resolveChildRect(View * child,
                                    const Core::Rect & requested,
                                    const Core::Rect & parentContentRect);
        void resolve(const Core::Rect & parentContentRect);
        static Core::Rect clampRectToParent(const Core::Rect & requested,
                                            const Core::Rect & parentContentRect,
                                            const ChildResizeSpec & spec);
    };
    

    /**
        @brief Controls all the basic functionality of a Widget!
        Sometimes referred to as the CanvasView.
        @relates Widget
     */ 
    class OMEGAWTK_EXPORT View : public Native::NativeEventEmitter {
    protected:
        Composition::CompositorClientProxy & compositorProxy();
        const Composition::CompositorClientProxy & compositorProxy() const;
        friend class Widget;
    private:
        struct Impl;
        Core::UniquePtr<Impl> impl_;
        SharedHandle<Composition::ViewRenderTarget> & renderTargetHandle();
        const SharedHandle<Composition::ViewRenderTarget> & renderTargetHandle() const;
        void setFrontendRecurse(Composition::Compositor *frontend);
        void setSyncLaneRecurse(uint64_t syncLaneId);
        virtual bool hasDelegate();
        void addSubView(View *view);
        void removeSubView(View * view);
        void preCreateVisualResources();
        friend class AppWindow;
        friend class Composition::ViewAnimator;
        friend class ScrollView;
        friend class Widget;
        friend class Container;
    protected:
        /**
            Constructs a View using a Rect param and a NativeItem; (With NO Layers!!)
            NOTE:
            This Constructed is only called when making a ScrollView.
            @param rect[in] The Rect to use
            @param nativeItem[in] The Native View to bind to
            @param parent[in] The Parent View
            @returns A View!
         */
        View(const Core::Rect & rect,Native::NativeItemPtr nativeItem,ViewPtr parent);
        /**
            Constructs a View. Creates its own LayerTree with a root Layer.
            @param rect The Rect to use
            @param parent The parent View (nullptr for root views)
            @returns A View!
         */
        View(const Core::Rect & rect,ViewPtr parent = nullptr);
    public:
        OMEGACOMMON_CLASS("OmegaWTK.View")

        /// Creates a View. Public factory for use by Widget subclass constructors.
        static ViewPtr Create(const Core::Rect & rect,ViewPtr parent = nullptr){
            return ViewPtr(new View(rect,parent));
        }

        /**
         * @brief Create A Layer
         * @param rect The Rectangle defining the bounds of the layer.
         * @returns Layer*/
        SharedHandle<Composition::Layer> makeLayer(Core::Rect rect);

        /**
         * @brief Create a Canvas that renders to CanvasFrames compatible with a Layer.
         * @param targetLayer The Layer to target.
         * @returns Canvas*/
        SharedHandle<Composition::Canvas> makeCanvas(SharedHandle<Composition::Layer> & targetLayer);

        /// @brief Retrieves the Rect that defines the position and bounds of the View.
        Core::Rect & getRect();
        /// @brief Retrieves the View's own LayerTree.
        Composition::LayerTree * getLayerTree();
        /// @brief Checks to see if this View is the root View of a Widget.
        bool isRootView();
        /// @brief Returns the resize coordinator associated with this view.
        ViewResizeCoordinator & getResizeCoordinator();
        const ViewResizeCoordinator & getResizeCoordinator() const;

        /// @brief Sets the object to recieve View related events.
        virtual void setDelegate(ViewDelegate *_delegate);

        /// @brief Resize the view synchronously.
        /// @note If you wish to animate the View resize, please use the ViewAnimator to perform that action.
        virtual void resize(Core::Rect newRect);

        /// @brief Starts a Composition Session for this View.
        /// @paragraph Upon invocation, this will allow Canvases to render to child Layers in the View's LayerTree
        /// and it will allow submission of render and animation commands from the child LayerAnimators and ViewAnimator.
        /// If one attempts to try animate or render to the View or any child Layers without calling this method FIRST, will recieve an access error.
        void startCompositionSession();

        /// @brief Ends a Composition Session for this View.
        /// @paragraph This method closes the submission queue of all render commands and submits them to the Compositor.
        /// Any commands posted to the CompositorClientProxy after invocation of this method will be ignored and an access error will be thrown.
        void endCompositionSession();

        /// @brief Make the View visible.
        void enable();

        /// @brief Make the View invisible.
        void disable();

        void applyLayoutDelta(const struct LayoutDelta & delta,
                              const struct LayoutTransitionSpec & spec);

        /// Called by Widget::executePaint after onPaint. CanvasView sends its
        /// root canvas frame. Specialized views do nothing (they already sent
        /// their frames in their own rendering methods).
        virtual void submitPaintFrame(int submissions) { (void)submissions; }

        virtual ~View();
    };

    /**
        @brief The Root View delegate class!
     */
    INTERFACE OMEGAWTK_EXPORT ViewDelegate : public Native::NativeEventProcessor {
        void onRecieveEvent(Native::NativeEventPtr event);
        ViewDelegate *forwardDelegate = nullptr;
        friend class View;
        protected:
        View * view;

        void setForwardDelegate(ViewDelegate *delegate);
        /**
            Called when the Mouse Enters the View
         */
        virtual void onMouseEnter(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Mouse Exits the View
         */
        virtual void onMouseExit(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is pressed
         */
        virtual void onLeftMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is raised after being pressed
         */
        virtual void onLeftMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is pressed
         */
        virtual void onRightMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is raised after being pressed
         */
        virtual void onRightMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is pressed
         */
        virtual void onKeyDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is raised after being pressed
         */
        virtual void onKeyUp(Native::NativeEventPtr event) DEFAULT;
        public:
        ViewDelegate();
        ~ViewDelegate();
    };


};

#endif
