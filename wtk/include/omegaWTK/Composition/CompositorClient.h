#include "omegaWTK/Native/NativeItem.h"

#include "Layer.h"

#include <chrono>
#include <cstdint>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITORCLIENT_H
#define OMEGAWTK_COMPOSITION_COMPOSITORCLIENT_H

namespace OmegaWTK::Composition {
    class ViewRenderTarget;
    class Compositor;

    void InitializeEngine();
    void CleanupEngine();

    struct CompositeFrame;
    class DisplayList;


    typedef std::chrono::time_point<std::chrono::high_resolution_clock> Timestamp;

    /// Stub retained for compatibility with Widget.Geometry.cpp /
    /// UIView.Animation.cpp. Real diagnostics were removed with the
    /// queue-based render path — this is now always default-constructed.
    struct SyncLaneDiagnostics {
        std::uint64_t syncLaneId = 0;
        std::uint64_t queuedPacketCount = 0;
        std::uint64_t submittedPacketCount = 0;
        std::uint64_t presentedPacketCount = 0;
        std::uint64_t droppedPacketCount = 0;
        std::uint64_t failedPacketCount = 0;
        std::uint64_t lastSubmittedPacketId = 0;
        std::uint64_t lastPresentedPacketId = 0;
        unsigned inFlight = 0;
        bool startupStabilized = false;
    };

    class OMEGAWTK_EXPORT CompositionRenderTarget {
    public:
        virtual ~CompositionRenderTarget() = default;
    };

    class CompositorClient;

    /** @brief Compositor Client Proxy class for interaction with a Compositor.

        Frame submission goes through the per-window CompositeFrame mailbox
        path (see FrameBuilder, which owns the one CompositeFrame per
        frame). Layer/View geometry mutations are applied synchronously on
        the calling thread — the old asynchronous command queue was
        removed in Tier 1 Phase B.
    */
    class OMEGAWTK_EXPORT CompositorClientProxy {
        friend class CompositorClient;

        Compositor *frontend = nullptr;

        SharedHandle<CompositionRenderTarget> renderTarget;

        uint64_t syncLaneId = 0;
        mutable std::mutex commandMutex;

        /// When non-null, pushFrame() appends to this CompositeFrame.
        /// Set by WidgetTreeHost during the composite paint pass.
        CompositeFrame *activeCompositeFrame_ = nullptr;

    public:
        explicit CompositorClientProxy(SharedHandle<CompositionRenderTarget> renderTarget);
        /// Construct with no render target. The render target must be set
        /// via setRenderTarget() before frames can be deposited.
        CompositorClientProxy();
        /// Replace the render target. Used by the window to propagate
        /// the shared render target to all child Views.
        void setRenderTarget(SharedHandle<CompositionRenderTarget> renderTarget);
        void setSyncLaneId(uint64_t syncLaneId);
        uint64_t getSyncLaneId() const;
        /// Stub retained for compatibility. Always returns a default-
        /// constructed SyncLaneDiagnostics; real telemetry was removed.
        SyncLaneDiagnostics getSyncLaneDiagnostics() const;
        Compositor *getFrontendPtr() const;
        void setFrontendPtr(Compositor *frontend);
        void setActiveCompositeFrame(CompositeFrame *frame);
        /// Tier 4 §4.1: append a window slice carrying a DrawOp
        /// `DisplayList` directly to the active CompositeFrame, bypassing
        /// `Canvas` / `CanvasFrame`. FrameBuilder::endFrame calls this once
        /// per pending submission. `windowOffset` positions the slice (the
        /// GPU viewport origin); `bounds` is the slice's local-coordinate
        /// extent (the viewport size — the window size, local origin). No
        /// background-channel clear and no targetLayer: backgrounds ride
        /// explicit DrawOp rects and per-layer blur is superseded by
        /// DrawOp effect ops (deferred). No-op when no frame is active.
        void submitDisplayList(DisplayList && ops,
                               Composition::Point2D windowOffset,
                               Composition::Rect bounds);
        virtual ~CompositorClientProxy() = default;
    };


    class OMEGAWTK_EXPORT CompositorClient {
        CompositorClientProxy & parentProxy;

    protected:
        /// Apply a layer resize delta synchronously on the caller's
        /// thread. Effects are draw-time Canvas operations and are not
        /// applied here.
        void pushLayerResizeCommand(Layer *target,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline);
        void pushLayerEffectCommand(Layer *target,SharedHandle<LayerEffect> & effect,Timestamp &start,Timestamp & deadline);
        void pushViewResizeCommand(Native::NativeItemPtr nativeView,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline);
    public:
        explicit CompositorClient(CompositorClientProxy & proxy);
        virtual ~CompositorClient() = default;
    };



    /**
     The Compositor's interface for composing a NativeView. (One per AppWindow)
     */
    class OMEGAWTK_EXPORT ViewRenderTarget : public CompositionRenderTarget {
        Native::NativeItemPtr native;
        /// Logical-to-physical pixel scale factor for the owning window.
        /// 1.0 = 96 DPI. Populated by the backend visual tree at creation
        /// time from the native window's DPI.
        float renderScale_ = 1.f;
    public:
        Native::NativeItemPtr getNativePtr();
        OMEGAWTK_NODISCARD float getRenderScale() const;
        void setRenderScale(float scale);
        explicit ViewRenderTarget(Native::NativeItemPtr _native);
        ~ViewRenderTarget() override;
    };


};


#endif
