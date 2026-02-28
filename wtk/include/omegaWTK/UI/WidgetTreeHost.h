#include "omegaWTK/Core/Core.h"
#include <type_traits>
#include <cstdint>
#include <chrono>

#ifndef OMEGAWTK_UI_WIDGETTREEHOST_H
#define OMEGAWTK_UI_WIDGETTREEHOST_H

namespace OmegaWTK {

    namespace Composition {
        class Compositor;
    }


    class AppWindow;
    OMEGACOMMON_SHARED_CLASS(AppWindow);
    class Widget;
    OMEGACOMMON_SHARED_CLASS(Widget);
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
     NOTE: An AppWindow also has a WindowLayer that can be drawn on by a Compositor, so in order
     to guarantee a fast runtime it uses the **first WidgetTreeHost's Compositor**.
    */
    class OMEGAWTK_EXPORT WidgetTreeHost {
        /** The Widget Tree's Compositor
         NOTE: The instance of this class that was first attached to an 
         AppWindow will be used for managing composition of the WindowLayer.
         */
        Composition::Compositor * compositor;
        uint64_t syncLaneId;
        /// The Root Widget
        WidgetPtr root;
        ResizeDynamicsTracker resizeTracker;
        ResizeSessionState lastResizeSessionState {};
        struct StaticSuspendRuntimeVerification {
            bool active = false;
            std::uint64_t sessionId = 0;
            std::uint64_t resizeUpdateCount = 0;
            std::uint64_t deferredPaintCount = 0;
            std::uint64_t deferredResizePaintCount = 0;
            std::uint64_t deferredImmediatePaintCount = 0;
            std::uint64_t authoritativeFlushCount = 0;
            PaintReason lastDeferredReason {};
        } staticSuspendVerification {};
        bool staticResizeSuspendActive = false;
        bool pendingAuthoritativeResizeFrame = false;

        bool attachedToWindow;

        WidgetTreeHost();

        friend class AppWindowManager;
        friend class AppWindow;
        friend class Widget;

        void initWidgetRecurse(Widget *parent);
        void observeWidgetLayerTreesRecurse(Widget *parent);
        void unobserveWidgetLayerTreesRecurse(Widget *parent);
        void invalidateWidgetRecurse(Widget *parent,PaintReason reason,bool immediate);
        void beginResizeCoordinatorSessionRecurse(Widget *parent,std::uint64_t sessionId);
        bool detectAnimatedTreeRecurse(Widget *parent) const;
        void notePaintDeferredDuringResize(PaintReason reason,bool immediate);
        void emitStaticSuspendVerificationSummary(bool flushIssued);
        void initWidgetTree();
        void flushAuthoritativeResizeFrame();
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
         its Compositor will be used to manage composition for its WindowLayer.
        */
        void attachToWindow(AppWindow * window);

        void notifyWindowResizeBegin(const Core::Rect & rect);
        void notifyWindowResize(const Core::Rect & rect);
        void notifyWindowResizeEnd(const Core::Rect & rect);
        bool shouldSuspendPaintDuringResize() const;

        ~WidgetTreeHost();
    };
};

#endif
