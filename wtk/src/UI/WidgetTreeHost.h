#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeEvent.h"
#include <type_traits>
#include <cstdint>
#include <chrono>

#ifndef OMEGAWTK_UI_WIDGETTREEHOST_H
#define OMEGAWTK_UI_WIDGETTREEHOST_H

namespace OmegaWTK {

    namespace Native {
        class NativeItem;
        typedef SharedHandle<NativeItem> NativeItemPtr;
    }

    namespace Composition {
        class Compositor;
        class CompositorSurface;
        struct CompositeFrame;
        class ViewRenderTarget;
        struct ResizeGovernorMetadata;
    }


    class AppWindow;
    OMEGACOMMON_SHARED_CLASS(AppWindow);
    class Widget;
    OMEGACOMMON_SHARED_CLASS(Widget);
    class View;
    class NativeViewHost;
    enum class PaintReason : std::uint8_t;

    struct OMEGAWTK_EXPORT ResizeDynamicsSample {
        double timestampMs = 0.0;
        float width = 0.f;
        float height = 0.f;
        float velocityPxPerSec = 0.f;
        float accelerationPxPerSec2 = 0.f;
    };

    enum class OMEGAWTK_EXPORT ResizePhase : std::uint8_t {
        Idle,
        Active,
        Settling,
        Completed
    };

    struct OMEGAWTK_EXPORT ResizeSessionState {
        std::uint64_t sessionId = 0;
        ResizePhase phase = ResizePhase::Idle;
        ResizeDynamicsSample sample {};
        bool animatedTree = false;
    };

    class OMEGAWTK_EXPORT ResizeDynamicsTracker {
        std::uint64_t currentSessionId = 0;
        bool inSession = false;
        float lastWidth = 0.f;
        float lastHeight = 0.f;
        float lastVelocity = 0.f;
        double lastSignificantChangeMs = 0.0;
        std::chrono::steady_clock::time_point lastTick {};
        static std::uint64_t nextSessionId();
    public:
        ResizeSessionState begin(float width,float height,double tMs);
        ResizeSessionState update(float width,float height,double tMs);
        ResizeSessionState end(float width,float height,double tMs);
        bool active() const { return inSession; }
    };
    /**
     @brief Owns a widget tree. (Owns the Widget tree's Compositor, and the Compositor's Scheduler)
     @paragraph An instance of this class gets attached to an AppWindow directly and the root widget
     the host is assigned to the AppWindow.
     NOTE: The first WidgetTreeHost attached to an AppWindow provides the
     Compositor that manages composition for the window's single surface.
    */
    class OMEGAWTK_EXPORT WidgetTreeHost {
        /** The Widget Tree's Compositor.
         NOTE: The instance of this class that was first attached to an
         AppWindow provides the Compositor for the window's single surface.
         */
        Composition::Compositor * compositor;
        uint64_t syncLaneId;
        /// The Root Widget
        WidgetPtr root;
        /// Window's shared render target (Phase 3). Propagated to all
        /// Views in the tree during initWidgetTree().
        SharedHandle<Composition::ViewRenderTarget> windowRenderTarget_;
        /// Root native item for this window (Phase 5). Used by
        /// NativeViewHost to embed/unembed real native views.
        Native::NativeItemPtr rootNativeItem_;
        ResizeDynamicsTracker resizeTracker;
        ResizeSessionState lastResizeSessionState {};
        std::uint64_t resizeCoordinatorGeneration = 0;
        struct ResizeValidationSession {
            bool active = false;
            std::uint64_t sessionId = 0;
            std::uint32_t sampleCount = 0;
            double beginTimestampMs = 0.0;
            double endTimestampMs = 0.0;
            float peakVelocityPxPerSec = 0.f;
            float peakAccelerationPxPerSec2 = 0.f;
            std::uint64_t baseSubmittedPackets = 0;
            std::uint64_t basePresentedPackets = 0;
            std::uint64_t baseDroppedPackets = 0;
            std::uint64_t baseFailedPackets = 0;
            std::uint64_t baseEpochDrops = 0;
            std::uint64_t baseStaleCoordinatorPackets = 0;
        };
        ResizeValidationSession resizeValidationSession {};

        /// Per-window surface mailbox (Phase A). Created by AppWindow.
        SharedHandle<Composition::CompositorSurface> windowSurface_;
        /// Composite frame being built during a paint pass.
        SharedHandle<Composition::CompositeFrame> pendingFrame_;

        bool attachedToWindow;
        View * hoveredView_ = nullptr;

        WidgetTreeHost();

        friend class AppWindowManager;
        friend class AppWindow;
        friend class Widget;
        friend class NativeViewHost;

        void initWidgetRecurse(Widget *parent);
        void propagateWindowRenderTargetRecurse(Widget *parent);
        void observeWidgetLayerTreesRecurse(Widget *parent);
        void unobserveWidgetLayerTreesRecurse(Widget *parent);
        void invalidateWidgetRecurse(Widget *parent,PaintReason reason,bool immediate);
        void beginResizeCoordinatorSessionRecurse(Widget *parent,std::uint64_t sessionId);
        void applyResizeGovernorMetadata(const Composition::ResizeGovernorMetadata & metadata);
        void setActiveCompositeFrameRecurse(Widget *parent,Composition::CompositeFrame *frame);
        bool detectAnimatedTreeRecurse(Widget *parent) const;
        View * hitTestWidget(Widget *widget,const Composition::Point2D &point) const;
        void initWidgetTree();
        Composition::Compositor *compPtr(){return compositor;};
        uint64_t laneId() const { return syncLaneId; }
    public:
        OMEGACOMMON_CLASS("OmegaWTK.UI.WidgetTreeHost")


        static SharedHandle<WidgetTreeHost> Create();

        /**
         @brief Set the root of the Widget tree.
         @param[in] widget The root of the Widget Tree
        */
        void setRoot(WidgetPtr widget);

        /**
         @brief Attach this host to an AppWindow
         @param[in] window The AppWindow to attach to.
         @paragraph
         If this instance of this class is the first to be attached to the AppWindow specified,
         its Compositor will be used to manage composition for the window's single surface.
        */
        void attachToWindow(AppWindow * window);

        /// Set the window's shared render target. Called by AppWindow
        /// before initWidgetTree() so the render target can be propagated
        /// to all Views in the tree (Phase 3, single-surface rendering).
        void setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> rt);

        /// Set the window's compositor surface mailbox (Phase A).
        /// Called by AppWindow during creation.
        void setWindowSurface(SharedHandle<Composition::CompositorSurface> surface);

        /// Paint all widgets and deposit the composite frame into the
        /// window's surface mailbox (Phase A).
        void paintAndDeposit(PaintReason reason,bool immediate = false);

        /// Set the root native item for this window (Phase 5).
        /// Called by AppWindow during setRootWidget().
        void setRootNativeItem(Native::NativeItemPtr item);

        /// Embed a real native view as a child of the window's root
        /// native view. Used by NativeViewHost (Phase 5).
        void embedNativeItem(Native::NativeItemPtr item);

        /// Remove a previously embedded native view from the window's
        /// root native view. Used by NativeViewHost (Phase 5).
        void unembedNativeItem(Native::NativeItemPtr item);

        void notifyWindowResizeBegin(const Composition::Rect & rect);
        void notifyWindowResize(const Composition::Rect & rect);
        void notifyWindowResizeEnd(const Composition::Rect & rect);

        /// Walk the virtual widget tree and return the deepest View whose
        /// bounds contain `point` (in window-relative coordinates).
        View * hitTest(const Composition::Point2D &point) const;

        /// Dispatch a native input event (mouse, keyboard) through the
        /// virtual widget tree via hit testing. Mouse events are routed
        /// to the View under the cursor; keyboard events go to the root.
        void dispatchInputEvent(Native::NativeEventPtr event);

        ~WidgetTreeHost();
    };
};

#endif
