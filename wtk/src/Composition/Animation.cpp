#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include "Compositor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace OmegaWTK::Composition {

namespace {
std::atomic<AnimationId> g_animationIdSeed {1};

float clamp01(float v){
    return std::max(0.f,std::min(1.f,v));
}

float lerp(float a,float b,float t){
    return a + ((b - a) * t);
}

float cubicBezier(float p0,float p1,float p2,float p3,float t){
    const float omt = 1.f - t;
    const float omt2 = omt * omt;
    const float t2 = t * t;
    return (omt2 * omt * p0) +
           (3.f * omt2 * t * p1) +
           (3.f * omt * t2 * p2) +
           (t2 * t * p3);
}
}

ScalarTraverse::ScalarTraverse(OmegaGTE::GPoint2D start, OmegaGTE::GPoint2D end, unsigned int speed) :
start_pt(start),
end_pt(end),
cur(start_pt),speed(speed)
{
    auto slope = (end_pt.y - start_pt.y)/(end_pt.x - start_pt.x);
    auto alpha = std::atan(slope);
    delta_x = std::cos(alpha) * float(speed);
    delta_y = slope * delta_x;
    

}

OmegaGTE::GPoint2D ScalarTraverse::get() {
    return cur;
}

bool ScalarTraverse::begin() const {
    return (cur.x == start_pt.x) && (cur.y == start_pt.y);
}

bool ScalarTraverse::end() const {
    return (cur.x == end_pt.x) && (cur.y == end_pt.y);
}

void ScalarTraverse::forward() {
    assert(!end() && "Reached the end of the scalar");
    cur.x += delta_x;
    cur.y += delta_y;
}

void ScalarTraverse::back() {
    assert(!begin() && "Reached the beginning of the scalar");
    cur.x -= delta_x;
    cur.y -= delta_y;
}

void ScalarTraverse::changeScalar(OmegaGTE::GPoint2D start, OmegaGTE::GPoint2D end) {
    start_pt = start;
    end_pt = end;

    auto slope = (end_pt.y - start_pt.y)/(end_pt.x - start_pt.x);

    auto alpha = std::atan(slope);
    delta_x = std::cos(alpha) * float(speed);
    delta_y = slope * delta_x;
}



SharedHandle<AnimationCurve> AnimationCurve::Linear(float start_h,float end_h) {
    auto curve = SharedHandle<AnimationCurve>(new AnimationCurve{Type::Linear,start_h,end_h});
    return curve;
}

SharedHandle<AnimationCurve> AnimationCurve::Linear(){
    return Linear(0.f,1.f);
}

SharedHandle<AnimationCurve> AnimationCurve::EaseIn(){
    return CubicBezier({0.42f,0.0f},{1.0f,1.0f});
}

SharedHandle<AnimationCurve> AnimationCurve::EaseOut(){
    return CubicBezier({0.0f,0.0f},{0.58f,1.0f});
}

SharedHandle<AnimationCurve> AnimationCurve::EaseInOut(){
    return CubicBezier({0.42f,0.0f},{0.58f,1.0f});
}

SharedHandle<AnimationCurve> AnimationCurve::Quadratic(OmegaGTE::GPoint2D a){
    auto curve = SharedHandle<AnimationCurve>(new AnimationCurve{Type::QuadraticBezier,0.f,1.f});
    curve->a = a;
    return curve;
}

SharedHandle<AnimationCurve> AnimationCurve::Cubic(OmegaGTE::GPoint2D a,OmegaGTE::GPoint2D b){
    return CubicBezier(a,b,0.f,1.f);
}

SharedHandle<AnimationCurve> AnimationCurve::CubicBezier(OmegaGTE::GPoint2D a,
                                                         OmegaGTE::GPoint2D b,
                                                         float start_h,
                                                         float end_h){
    auto curve = SharedHandle<AnimationCurve>(new AnimationCurve{Type::CubicBezier,start_h,end_h});
    curve->a = a;
    curve->b = b;
    return curve;
}

float AnimationCurve::sample(float t) const{
    const float x = clamp01(t);
    switch(type){
        case Type::Linear:
            return clamp01(lerp(start_h,end_h,x));
        case Type::QuadraticBezier:
            return clamp01(cubicBezier(start_h,a.y,end_h,end_h,x));
        case Type::CubicBezier:
            return clamp01(cubicBezier(start_h,a.y,b.y,end_h,x));
        default:
            return clamp01(x);
    }
}

struct AnimationCurveLinearTraversal{
    ScalarTraverse traversal;
};
struct AnimationCurveQuadraticTraversal {

    ScalarTraverse start_to_A;
    ScalarTraverse A_to_end;

    ScalarTraverse intermed;
} ;
struct AnimationCurveCubicTraversal {
    ScalarTraverse start_to_A;
    ScalarTraverse A_to_B;
    ScalarTraverse B_to_end;

    ScalarTraverse intermed_0;
    ScalarTraverse intermed_1;
    ScalarTraverse intermed_final;
};


AnimationCurve::Traversal::Traversal(AnimationCurve &curve,
                                     float & space_w,float & space_h):
                                     curve(curve),data(nullptr)
                                    {
    
    if(curve.type == Type::Linear){
        data = new AnimationCurveLinearTraversal {
            ScalarTraverse( 
            OmegaGTE::GPoint2D {0.f,curve.start_h * space_h},
            OmegaGTE::GPoint2D {space_w,curve.end_h * space_h}
            )
        };
        initState = malloc(sizeof(AnimationCurveLinearTraversal));
        memcpy(initState,data,sizeof(AnimationCurveLinearTraversal));
    }
    else if(curve.type == Type::QuadraticBezier){
        data = new AnimationCurveQuadraticTraversal{
            ScalarTraverse(
                OmegaGTE::GPoint2D{0.f,curve.start_h * space_h},
                OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h}
            ),
            ScalarTraverse(
                OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h},
                OmegaGTE::GPoint2D{space_w,curve.end_h * space_h}
            ),


            ScalarTraverse(
                OmegaGTE::GPoint2D{0.f,curve.start_h * space_h},
                OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h}
            )
        };
        initState = malloc(sizeof(AnimationCurveQuadraticTraversal));
        memcpy(initState,data,sizeof(AnimationCurveQuadraticTraversal));
    }
    else {
        data = new AnimationCurveCubicTraversal{
                    ScalarTraverse(
                        OmegaGTE::GPoint2D{0.f,curve.start_h * space_h},
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h}
                    ),
                    ScalarTraverse(
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h},
                        OmegaGTE::GPoint2D{curve.b.x * space_w,curve.b.y * space_h}
                    ),
                    ScalarTraverse(
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h},
                        OmegaGTE::GPoint2D{space_w,curve.end_h * space_h}
                    ),


                    ScalarTraverse(
                        OmegaGTE::GPoint2D{0.f,curve.start_h * space_h},
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h}
                    ),
                    ScalarTraverse(
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h},
                        OmegaGTE::GPoint2D{curve.b.x * space_w,curve.b.y * space_h}
                    ),

                    ScalarTraverse(
                        OmegaGTE::GPoint2D{0.f,curve.start_h * space_h},
                        OmegaGTE::GPoint2D{curve.a.x * space_w,curve.a.y * space_h}
                    ),
        };
        initState = malloc(sizeof(AnimationCurveCubicTraversal));
        memcpy(initState,data,sizeof(AnimationCurveCubicTraversal));
    }
}

void AnimationCurve::Traversal::next() {
    if(curve.type == Type::Linear){
       auto d = (AnimationCurveLinearTraversal *)data;
       d->traversal.forward();
    }
    else if(curve.type == Type::QuadraticBezier){
        auto d = (AnimationCurveQuadraticTraversal *)data;
        d->start_to_A.forward();
        d->A_to_end.forward();
        d->intermed.changeScalar(d->start_to_A.get(),d->A_to_end.get());
        d->intermed.forward();
    }
    else {
        auto d = (AnimationCurveCubicTraversal *)data;
        d->start_to_A.forward();
        d->A_to_B.forward();
        d->B_to_end.forward();

        d->intermed_0.changeScalar(d->start_to_A.get(),d->A_to_B.get());
        d->intermed_1.changeScalar(d->A_to_B.get(),d->B_to_end.get());
        d->intermed_0.forward();
        d->intermed_1.forward();

        d->intermed_final.changeScalar(d->intermed_0.get(),d->intermed_1.get());
        d->intermed_final.forward();
    }
}

OmegaGTE::GPoint2D AnimationCurve::Traversal::get() {
    if(curve.type == Type::Linear){
       auto d = (AnimationCurveLinearTraversal *)data;
       return d->traversal.get();
    }
    else if(curve.type == Type::QuadraticBezier){
        auto d = (AnimationCurveQuadraticTraversal *)data;
        return d->intermed.get();
    }
    else {
        auto d = (AnimationCurveCubicTraversal *)data;
        return d->intermed_final.get();
    }
}

bool AnimationCurve::Traversal::end() {
    if(curve.type == Type::Linear){
       auto d = (AnimationCurveLinearTraversal *)data;
       return d->traversal.end();
    }
    else if(curve.type == Type::QuadraticBezier){
        auto d = (AnimationCurveQuadraticTraversal *)data;
        return d->intermed.end();
    }
    else {
        auto d = (AnimationCurveCubicTraversal *)data;
        return d->intermed_final.end();
    }
}

void AnimationCurve::Traversal::reset() {
    if(curve.type == Type::Linear){
       auto d = (AnimationCurveLinearTraversal *)data;
       delete d;
       data = malloc(sizeof(AnimationCurveLinearTraversal));
       auto initData = (AnimationCurveLinearTraversal *)initState;
       memcpy(data,initData,sizeof(AnimationCurveLinearTraversal));
    }
    else if(curve.type == Type::QuadraticBezier){
        auto d = (AnimationCurveQuadraticTraversal *)data;
        delete d;
        data = malloc(sizeof(AnimationCurveQuadraticTraversal));
        auto initData = (AnimationCurveQuadraticTraversal *)initState;
        memcpy(data,initData,sizeof(AnimationCurveQuadraticTraversal));
    }
    else {
        auto d = (AnimationCurveCubicTraversal *)data;
        delete d;
        data = malloc(sizeof(AnimationCurveCubicTraversal));
        auto initData = (AnimationCurveCubicTraversal *)initState;
        memcpy(data,initData,sizeof(AnimationCurveCubicTraversal));
    }
}

AnimationCurve::Traversal::~Traversal(){
    if(curve.type == Type::Linear){
        delete (AnimationCurveLinearTraversal *)data;
    }
    else if(curve.type == Type::QuadraticBezier){
        delete (AnimationCurveQuadraticTraversal *)data;
    }
    else {
        delete (AnimationCurveCubicTraversal *)data;
    }
}

AnimationCurve::Traversal AnimationCurve::traverse(float space_w,float space_h) {
    return Traversal(*this,space_w,space_h);
}

AnimationTimeline::Keyframe AnimationTimeline::Keyframe::CanvasFrameStop(float time, SharedHandle<AnimationCurve> curve,
                                                          SharedHandle<CanvasFrame> &frame) {
    Keyframe k {};
    k.time = time;
    k.curve = std::move(curve);
    k.frame = frame;
    k.effect = nullptr;
    return k;
}

AnimationTimeline::Keyframe AnimationTimeline::Keyframe::DropShadowStop(float time, SharedHandle<AnimationCurve> curve,
                                                                        LayerEffect::DropShadowParams &params) {
    Keyframe k {};
    k.time = time;
    k.curve = std::move(curve);
    k.frame = nullptr;

    k.effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::DropShadow});
    k.effect->dropShadow = params;

    return k;
}

AnimationTimeline::Keyframe AnimationTimeline::Keyframe::TransformationStop(float time,
                                                                            SharedHandle<AnimationCurve> curve,
                                                                            LayerEffect::TransformationParams &params) {
    Keyframe k {};
    k.time = time;
    k.curve = std::move(curve);
    k.frame = nullptr;

    k.effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::Transformation});
    k.effect->transform = params;

    return k;
}


SharedHandle<AnimationTimeline> AnimationTimeline::Create(const OmegaCommon::Vector<Keyframe> &keyframes) {
    auto object = std::make_shared<AnimationTimeline>();
    bool canvasFrameStop = (bool)keyframes.front().frame;

    if(!canvasFrameStop) {
        bool shadowFrameStop = keyframes.front().effect->type == LayerEffect::DropShadow;

        for(auto & k : keyframes){
            if(shadowFrameStop){
                assert(k.effect->type == LayerEffect::DropShadow && "All keyframes must animate the Drop Shadow effect Only");
            }
            else {
                assert(k.effect->type == LayerEffect::Transformation && "All keyframes must animate the Transformation effect Only");
            }
        }
    }
    else {
        for(auto & k : keyframes){
            assert(k.frame && "All keyframes must be a typeof Canvas Frame Stop.");
        }
    }
    object->keyframes = keyframes;
    return object;
}

struct AnimationHandle::StateBlock {
    AnimationId animationId = 0;
    std::atomic<AnimationState> animationState {AnimationState::Pending};
    std::atomic<float> animationProgress {0.f};
    std::atomic<float> animationRate {1.f};
    std::atomic<std::uint64_t> submittedPacketId {0};
    std::atomic<std::uint64_t> presentedPacketId {0};
    std::atomic<std::uint32_t> droppedPacketCount {0};
    std::mutex failureMutex {};
    Core::Optional<OmegaCommon::String> failureReason {};

    StateBlock(AnimationId id,AnimationState state):
    animationId(id),
    animationState(state){}
};

namespace detail {

class AnimationRuntimeRegistry {
public:
    static void setState(AnimationHandle & handle,AnimationState state){
        handle.setStateInternal(state);
    }

    static void setProgress(AnimationHandle & handle,float progress){
        handle.setProgressInternal(progress);
    }

    static void setSubmittedPacketId(AnimationHandle & handle,std::uint64_t packetId){
        handle.setSubmittedPacketIdInternal(packetId);
    }

    static void setPresentedPacketId(AnimationHandle & handle,std::uint64_t packetId){
        handle.setPresentedPacketIdInternal(packetId);
    }

    static void incrementDroppedPacketCount(AnimationHandle & handle){
        handle.incrementDroppedPacketCountInternal();
    }

    static void setFailureReason(AnimationHandle & handle,const OmegaCommon::String & reason){
        handle.setFailureReasonInternal(reason);
    }

    static Native::NativeItemPtr getNativeView(ViewAnimator * animator){
        if(animator == nullptr){
            return nullptr;
        }
        return animator->nativeView;
    }

    static Core::Rect getLayerRect(LayerAnimator * animator){
        if(animator == nullptr){
            return {};
        }
        return animator->targetLayer.getLayerRect();
    }

    static void queueViewResizeDelta(ViewAnimator * animator,int dx,int dy,int dw,int dh){
        if(animator != nullptr){
            animator->queueViewResizeDelta(dx,dy,dw,dh);
        }
    }

    static void queueLayerResizeDelta(LayerAnimator * animator,int dx,int dy,int dw,int dh){
        if(animator != nullptr){
            animator->queueLayerResizeDelta(dx,dy,dw,dh);
        }
    }

    static CompositorClientProxy * getViewProxy(ViewAnimator * animator){
        if(animator == nullptr){
            return nullptr;
        }
        return &animator->_client;
    }

    static CompositorClientProxy * getLayerProxy(LayerAnimator * animator){
        if(animator == nullptr){
            return nullptr;
        }
        return &animator->parentAnimator._client;
    }

    static std::uint16_t getViewFrameRate(ViewAnimator * animator){
        if(animator == nullptr){
            return 60;
        }
        return static_cast<std::uint16_t>(animator->framePerSec);
    }

    static std::uint16_t getLayerFrameRate(LayerAnimator * animator){
        if(animator == nullptr){
            return 60;
        }
        return static_cast<std::uint16_t>(animator->parentAnimator.framePerSec);
    }
};

    struct AnimationInstance {
        enum class Kind : std::uint8_t {
            View,
            Layer
        };

        AnimationId id = 0;
        Kind kind = Kind::View;
        void *owner = nullptr;
        ViewAnimator *viewAnimator = nullptr;
        LayerAnimator *layerAnimator = nullptr;
        AnimationHandle handle {};
        TimingOptions timing {};
        std::uint64_t laneId = 0;
        std::chrono::steady_clock::time_point startedAt {};
        std::chrono::steady_clock::duration pausedDuration {};
        std::chrono::steady_clock::time_point pausedAt {};
        bool pauseLatched = false;
        float lastQueuedProgress = -1.f;
        std::uint32_t expectedFrames = 1;
        std::deque<std::uint64_t> pendingPacketIds {};
        std::uint64_t droppedPacketsBaseline = 0;
        std::uint64_t presentedPacketsBaseline = 0;
        std::uint32_t acknowledgedSteps = 0;
        bool pendingCompletion = false;
        bool markedForRemoval = false;
        LayerClip layerClip {};
        ViewClip viewClip {};
        Core::Rect lastSampledRect {};
        bool hasSampledRect = false;

        void syncFromTelemetry(const Compositor::LaneTelemetrySnapshot & snapshot){
            if(snapshot.syncLaneId == 0){
                return;
            }
            if(snapshot.lastPresentedPacketId > presentedPacketsBaseline){
                while(!pendingPacketIds.empty() &&
                      pendingPacketIds.front() <= snapshot.lastPresentedPacketId){
                    AnimationRuntimeRegistry::setPresentedPacketId(handle,pendingPacketIds.front());
                    pendingPacketIds.pop_front();
                    acknowledgedSteps += 1;
                }
                presentedPacketsBaseline = snapshot.lastPresentedPacketId;
            }

            if(snapshot.droppedPacketCount > droppedPacketsBaseline){
                std::uint64_t droppedDiff = snapshot.droppedPacketCount - droppedPacketsBaseline;
                while(droppedDiff > 0 && !pendingPacketIds.empty()){
                    pendingPacketIds.pop_front();
                    AnimationRuntimeRegistry::incrementDroppedPacketCount(handle);
                    droppedDiff--;
                }
                droppedPacketsBaseline = snapshot.droppedPacketCount;
            }
        }

        float wallClockProgress(const std::chrono::steady_clock::time_point & now) {
            if(handle.state() == AnimationState::Paused){
                if(!pauseLatched){
                    pauseLatched = true;
                    pausedAt = now;
                }
                return std::max(0.f,lastQueuedProgress);
            }
            if(pauseLatched){
                pauseLatched = false;
                pausedDuration += now - pausedAt;
            }

            const auto delay = std::chrono::milliseconds(timing.delayMs);
            const auto duration = std::chrono::milliseconds(std::max<std::uint32_t>(1,timing.durationMs));
            auto effectiveStart = startedAt + delay;
            if(now <= effectiveStart){
                return 0.f;
            }
            auto elapsed = now - effectiveStart;
            if(elapsed < std::chrono::steady_clock::duration::zero()){
                elapsed = std::chrono::steady_clock::duration::zero();
            }
            if(pausedDuration > std::chrono::steady_clock::duration::zero() &&
               elapsed > pausedDuration){
                elapsed -= pausedDuration;
            }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            float normalized = static_cast<float>(elapsedMs) / static_cast<float>(duration.count());
            normalized *= std::max(handle.playbackRate(),std::numeric_limits<float>::epsilon());
            return clamp01(normalized);
        }

        float presentedProgress(const Compositor::LaneTelemetrySnapshot & snapshot) const {
            (void)snapshot;
            if(expectedFrames == 0){
                return 0.f;
            }
            return clamp01(static_cast<float>(acknowledgedSteps) /
                           static_cast<float>(expectedFrames));
        }

        float effectiveProgress(const std::chrono::steady_clock::time_point & now,
                                const Compositor::LaneTelemetrySnapshot & snapshot){
            const float wall = wallClockProgress(now);
            const float presented = presentedProgress(snapshot);
            switch(timing.clockMode){
                case ClockMode::WallClock:
                    return wall;
                case ClockMode::PresentedClock:
                    return std::max(presented,wall >= 1.f && pendingPacketIds.empty() ? 1.f : 0.f);
                case ClockMode::Hybrid:
                default: {
                    const std::uint32_t safeExpected = std::max<std::uint32_t>(1,expectedFrames);
                    float allowance = static_cast<float>(std::max<std::uint8_t>(1,timing.maxCatchupSteps)) /
                                      static_cast<float>(safeExpected);
                    if(timing.preferResizeSafeBudget && snapshot.resizeBudgetActive){
                        allowance = std::min(allowance,1.f / static_cast<float>(safeExpected));
                    }
                    float clamped = std::min(wall,presented + allowance);
                    if(wall >= 1.f && pendingPacketIds.empty()){
                        clamped = 1.f;
                    }
                    return clamp01(clamped);
                }
            }
        }

        bool queueSample(float progress){
            progress = clamp01(progress);
            bool queued = false;

            if(kind == Kind::View){
                if(viewAnimator == nullptr){
                    return false;
                }
                auto nativeView = AnimationRuntimeRegistry::getNativeView(viewAnimator);
                if(viewClip.rect && nativeView != nullptr){
                    auto sampledRect = viewClip.rect->sample(progress);
                    if(!hasSampledRect){
                        lastSampledRect = nativeView->getRect();
                        hasSampledRect = true;
                    }

                    const int dx = static_cast<int>(std::lround(sampledRect.pos.x - lastSampledRect.pos.x));
                    const int dy = static_cast<int>(std::lround(sampledRect.pos.y - lastSampledRect.pos.y));
                    const int dw = static_cast<int>(std::lround(sampledRect.w - lastSampledRect.w));
                    const int dh = static_cast<int>(std::lround(sampledRect.h - lastSampledRect.h));
                    if(dx != 0 || dy != 0 || dw != 0 || dh != 0){
                        AnimationRuntimeRegistry::queueViewResizeDelta(viewAnimator,dx,dy,dw,dh);
                        lastSampledRect = sampledRect;
                        queued = true;
                    }
                }
                return queued;
            }

            if(layerAnimator == nullptr){
                return false;
            }
            if(layerClip.rect){
                auto sampledRect = layerClip.rect->sample(progress);
                if(!hasSampledRect){
                    lastSampledRect = AnimationRuntimeRegistry::getLayerRect(layerAnimator);
                    hasSampledRect = true;
                }

                const int dx = static_cast<int>(std::lround(sampledRect.pos.x - lastSampledRect.pos.x));
                const int dy = static_cast<int>(std::lround(sampledRect.pos.y - lastSampledRect.pos.y));
                const int dw = static_cast<int>(std::lround(sampledRect.w - lastSampledRect.w));
                const int dh = static_cast<int>(std::lround(sampledRect.h - lastSampledRect.h));
                if(dx != 0 || dy != 0 || dw != 0 || dh != 0){
                    AnimationRuntimeRegistry::queueLayerResizeDelta(layerAnimator,dx,dy,dw,dh);
                    lastSampledRect = sampledRect;
                    queued = true;
                }
            }

            if(layerClip.shadow){
                auto params = layerClip.shadow->sample(progress);
                layerAnimator->applyShadow(params);
                queued = true;
            }

            if(layerClip.transform){
                auto params = layerClip.transform->sample(progress);
                layerAnimator->applyTransformation(params);
                queued = true;
            }
            return queued;
        }
    };

    struct LaneContext {
        std::uint64_t laneId = 0;
        Compositor *compositor = nullptr;
        std::mutex mutex {};
        std::condition_variable cv {};
        std::map<AnimationId,std::shared_ptr<AnimationInstance>> instances {};
        bool stopping = false;
        std::thread worker {};

        explicit LaneContext(std::uint64_t laneId,Compositor *compositor):
        laneId(laneId),
        compositor(compositor){}
    };

    std::mutex registryMutex {};
    std::unordered_map<std::uint64_t,std::shared_ptr<LaneContext>> lanes {};

    static std::uint32_t estimateExpectedFrames(const TimingOptions & timing){
        const std::uint32_t durationMs = std::max<std::uint32_t>(1,timing.durationMs);
        const std::uint16_t fps = std::max<std::uint16_t>(1,timing.frameRateHint);
        auto frames = (durationMs * fps) / 1000;
        return std::max<std::uint32_t>(1,frames);
    }

    void runLane(const std::shared_ptr<LaneContext> & lane){
        constexpr auto tickInterval = std::chrono::milliseconds(16);
        while(true){
            OmegaCommon::Vector<std::shared_ptr<AnimationInstance>> active {};
            {
                std::unique_lock<std::mutex> lk(lane->mutex);
                lane->cv.wait_for(lk,tickInterval,[&]{
                    return lane->stopping || !lane->instances.empty();
                });
                if(lane->stopping && lane->instances.empty()){
                    break;
                }
                for(auto & entry : lane->instances){
                    active.push_back(entry.second);
                }
            }

            if(active.empty()){
                continue;
            }

            Compositor::LaneTelemetrySnapshot telemetry {};
            if(lane->compositor != nullptr){
                telemetry = lane->compositor->getLaneTelemetrySnapshot(lane->laneId);
            }

            for(auto & instance : active){
                if(instance != nullptr){
                    instance->syncFromTelemetry(telemetry);
                }
            }

            std::unordered_map<CompositorClientProxy *,OmegaCommon::Vector<std::pair<std::shared_ptr<AnimationInstance>,float>>> grouped {};
            auto now = std::chrono::steady_clock::now();

            for(auto & instance : active){
                if(instance == nullptr){
                    continue;
                }
                auto state = instance->handle.state();
                if(state == AnimationState::Cancelled || state == AnimationState::Failed){
                    instance->markedForRemoval = true;
                    continue;
                }
                if(state == AnimationState::Completed){
                    if(instance->pendingPacketIds.empty()){
                        instance->markedForRemoval = true;
                    }
                    continue;
                }
                if(state == AnimationState::Paused){
                    continue;
                }
                if(state == AnimationState::Pending){
                    AnimationRuntimeRegistry::setState(instance->handle,AnimationState::Running);
                }

                float progress = instance->effectiveProgress(now,telemetry);
                if(instance->lastQueuedProgress >= 0.f &&
                   progress <= (instance->lastQueuedProgress + 0.0001f) &&
                   progress < 1.f){
                    continue;
                }

                CompositorClientProxy *proxy = nullptr;
                if(instance->kind == AnimationInstance::Kind::View && instance->viewAnimator != nullptr){
                    proxy = AnimationRuntimeRegistry::getViewProxy(instance->viewAnimator);
                }
                else if(instance->kind == AnimationInstance::Kind::Layer && instance->layerAnimator != nullptr){
                    proxy = AnimationRuntimeRegistry::getLayerProxy(instance->layerAnimator);
                }

                if(proxy == nullptr || proxy->getFrontendPtr() == nullptr){
                    AnimationRuntimeRegistry::setFailureReason(instance->handle,"Animation target has no active compositor frontend.");
                    instance->markedForRemoval = true;
                    continue;
                }

                grouped[proxy].push_back({instance,progress});
            }

            for(auto & groupedEntry : grouped){
                auto *proxy = groupedEntry.first;
                if(proxy == nullptr){
                    continue;
                }
                auto & jobs = groupedEntry.second;
                if(jobs.empty()){
                    continue;
                }

                proxy->beginRecord();
                const auto packetId = proxy->peekNextPacketId();
                bool queuedAny = false;
                for(auto & job : jobs){
                    auto & instance = job.first;
                    const float progress = job.second;
                    if(instance == nullptr){
                        continue;
                    }
                    if(instance->queueSample(progress)){
                        instance->lastQueuedProgress = progress;
                        AnimationRuntimeRegistry::setProgress(instance->handle,progress);
                        queuedAny = true;
                    }
                    if(progress >= 1.f){
                        instance->pendingCompletion = true;
                    }
                }
                proxy->endRecord();

                if(queuedAny){
                    for(auto & job : jobs){
                        auto & instance = job.first;
                        if(instance == nullptr){
                            continue;
                        }
                        if(instance->lastQueuedProgress < 0.f){
                            continue;
                        }
                        instance->pendingPacketIds.push_back(packetId);
                        AnimationRuntimeRegistry::setSubmittedPacketId(instance->handle,packetId);
                    }
                }

                for(auto & job : jobs){
                    auto & instance = job.first;
                    if(instance == nullptr){
                        continue;
                    }
                    if(instance->pendingCompletion && instance->pendingPacketIds.empty()){
                        AnimationRuntimeRegistry::setProgress(instance->handle,1.f);
                        AnimationRuntimeRegistry::setState(instance->handle,AnimationState::Completed);
                        instance->markedForRemoval = true;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lk(lane->mutex);
                for(auto it = lane->instances.begin(); it != lane->instances.end(); ){
                    auto & instance = it->second;
                    if(instance == nullptr || instance->markedForRemoval){
                        it = lane->instances.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
        }
    }

    static std::shared_ptr<LaneContext> ensureLane(std::uint64_t laneId,Compositor *compositor){
        std::lock_guard<std::mutex> lk(registryMutex);
        auto it = lanes.find(laneId);
        if(it != lanes.end()){
            if(it->second->compositor == nullptr && compositor != nullptr){
                it->second->compositor = compositor;
            }
            return it->second;
        }

        auto lane = std::make_shared<LaneContext>(laneId,compositor);
        lane->worker = std::thread([lane]{
            runLane(lane);
        });
        lanes[laneId] = lane;
        return lane;
    }

    static TimingOptions sanitizeTiming(const TimingOptions & timing,std::uint16_t fallbackFps){
        TimingOptions result = timing;
        result.durationMs = std::max<std::uint32_t>(1,timing.durationMs);
        result.frameRateHint = std::max<std::uint16_t>(1, timing.frameRateHint == 0 ? fallbackFps : timing.frameRateHint);
        result.playbackRate = std::max(timing.playbackRate,std::numeric_limits<float>::epsilon());
        result.maxCatchupSteps = std::max<std::uint8_t>(1,timing.maxCatchupSteps);
        return result;
    }

    static AnimationHandle registerViewAnimation(ViewAnimator *animator,
                                                 std::uint64_t laneId,
                                                 const ViewClip & clip,
                                                 const TimingOptions & timing){
        if(animator == nullptr || laneId == 0 || (!clip.rect && !clip.opacity)){
            return {};
        }
        auto * proxy = AnimationRuntimeRegistry::getViewProxy(animator);
        auto * compositor = proxy ? proxy->getFrontendPtr() : nullptr;
        auto handle = AnimationHandle::Create(g_animationIdSeed.fetch_add(1),AnimationState::Pending);
        auto instance = std::make_shared<AnimationInstance>();
        instance->id = handle.id();
        instance->kind = AnimationInstance::Kind::View;
        instance->owner = animator;
        instance->viewAnimator = animator;
        instance->handle = handle;
        instance->laneId = laneId;
        instance->viewClip = clip;
        instance->timing = sanitizeTiming(timing,AnimationRuntimeRegistry::getViewFrameRate(animator));
        instance->expectedFrames = estimateExpectedFrames(instance->timing);
        instance->startedAt = std::chrono::steady_clock::now();
        if(compositor != nullptr){
            auto laneSnapshot = compositor->getLaneTelemetrySnapshot(laneId);
            instance->droppedPacketsBaseline = laneSnapshot.droppedPacketCount;
            instance->presentedPacketsBaseline = laneSnapshot.lastPresentedPacketId;
        }

        auto lane = ensureLane(laneId,compositor);
        {
            std::lock_guard<std::mutex> lk(lane->mutex);
            lane->instances[instance->id] = instance;
        }
        lane->cv.notify_all();
        return handle;
    }

    static AnimationHandle registerLayerAnimation(LayerAnimator *animator,
                                                  std::uint64_t laneId,
                                                  const LayerClip & clip,
                                                  const TimingOptions & timing){
        if(animator == nullptr || laneId == 0 ||
           (!clip.rect && !clip.transform && !clip.shadow && !clip.opacity)){
            return {};
        }
        auto * proxy = AnimationRuntimeRegistry::getLayerProxy(animator);
        auto * compositor = proxy ? proxy->getFrontendPtr() : nullptr;
        auto handle = AnimationHandle::Create(g_animationIdSeed.fetch_add(1),AnimationState::Pending);
        auto instance = std::make_shared<AnimationInstance>();
        instance->id = handle.id();
        instance->kind = AnimationInstance::Kind::Layer;
        instance->owner = animator;
        instance->layerAnimator = animator;
        instance->handle = handle;
        instance->laneId = laneId;
        instance->layerClip = clip;
        instance->timing = sanitizeTiming(timing,AnimationRuntimeRegistry::getLayerFrameRate(animator));
        instance->expectedFrames = estimateExpectedFrames(instance->timing);
        instance->startedAt = std::chrono::steady_clock::now();
        if(compositor != nullptr){
            auto laneSnapshot = compositor->getLaneTelemetrySnapshot(laneId);
            instance->droppedPacketsBaseline = laneSnapshot.droppedPacketCount;
            instance->presentedPacketsBaseline = laneSnapshot.lastPresentedPacketId;
        }

        auto lane = ensureLane(laneId,compositor);
        {
            std::lock_guard<std::mutex> lk(lane->mutex);
            lane->instances[instance->id] = instance;
        }
        lane->cv.notify_all();
        return handle;
    }

    static void updateOwnerState(void *owner,AnimationState state){
        if(owner == nullptr){
            return;
        }
        std::lock_guard<std::mutex> lk(registryMutex);
        for(auto & laneEntry : lanes){
            auto & lane = laneEntry.second;
            if(lane == nullptr){
                continue;
            }
            std::lock_guard<std::mutex> laneLock(lane->mutex);
            for(auto & instanceEntry : lane->instances){
                auto & instance = instanceEntry.second;
                if(instance == nullptr || instance->owner != owner){
                    continue;
                }
                AnimationRuntimeRegistry::setState(instance->handle,state);
            }
            lane->cv.notify_all();
        }
    }

    static void cancelOwner(void *owner){
        if(owner == nullptr){
            return;
        }
        std::lock_guard<std::mutex> lk(registryMutex);
        for(auto & laneEntry : lanes){
            auto & lane = laneEntry.second;
            if(lane == nullptr){
                continue;
            }
            std::lock_guard<std::mutex> laneLock(lane->mutex);
            for(auto & instanceEntry : lane->instances){
                auto & instance = instanceEntry.second;
                if(instance == nullptr || instance->owner != owner){
                    continue;
                }
                instance->handle.cancel();
                instance->markedForRemoval = true;
            }
            lane->cv.notify_all();
        }
    }

    static void shutdownRegistry(){
        std::unordered_map<std::uint64_t,std::shared_ptr<LaneContext>> snapshot {};
        {
            std::lock_guard<std::mutex> lk(registryMutex);
            snapshot.swap(lanes);
        }
        for(auto & entry : snapshot){
            auto & lane = entry.second;
            if(lane == nullptr){
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(lane->mutex);
                lane->stopping = true;
            }
            lane->cv.notify_all();
            if(lane->worker.joinable()){
                lane->worker.join();
            }
        }
    }

    struct RegistryShutdown {
        ~RegistryShutdown(){
            shutdownRegistry();
        }
    };

    static RegistryShutdown g_registryShutdown;

}

AnimationHandle::AnimationHandle():
stateBlock(nullptr){
}

AnimationHandle::AnimationHandle(const SharedHandle<StateBlock> & stateBlock):
stateBlock(stateBlock){
}

AnimationHandle AnimationHandle::Create(AnimationId id,AnimationState initialState){
    return AnimationHandle(std::make_shared<StateBlock>(id,initialState));
}

AnimationId AnimationHandle::id() const{
    return stateBlock ? stateBlock->animationId : 0;
}

AnimationState AnimationHandle::state() const{
    return stateBlock ? stateBlock->animationState.load() : AnimationState::Failed;
}

float AnimationHandle::progress() const{
    return stateBlock ? stateBlock->animationProgress.load() : 0.f;
}

float AnimationHandle::playbackRate() const{
    return stateBlock ? stateBlock->animationRate.load() : 0.f;
}

std::uint64_t AnimationHandle::lastSubmittedPacketId() const{
    return stateBlock ? stateBlock->submittedPacketId.load() : 0;
}

std::uint64_t AnimationHandle::lastPresentedPacketId() const{
    return stateBlock ? stateBlock->presentedPacketId.load() : 0;
}

std::uint32_t AnimationHandle::droppedPacketCount() const{
    return stateBlock ? stateBlock->droppedPacketCount.load() : 0;
}

Core::Optional<OmegaCommon::String> AnimationHandle::failureReason() const{
    if(!stateBlock){
        return {};
    }
    std::lock_guard<std::mutex> lk(stateBlock->failureMutex);
    return stateBlock->failureReason;
}

bool AnimationHandle::valid() const{
    return stateBlock != nullptr && stateBlock->animationId != 0;
}

void AnimationHandle::pause(){
    if(!stateBlock){
        return;
    }
    auto current = stateBlock->animationState.load();
    if(current == AnimationState::Running || current == AnimationState::Pending){
        stateBlock->animationState.store(AnimationState::Paused);
    }
}

void AnimationHandle::resume(){
    if(!stateBlock){
        return;
    }
    if(stateBlock->animationState.load() == AnimationState::Paused){
        stateBlock->animationState.store(AnimationState::Running);
    }
}

void AnimationHandle::cancel(){
    if(!stateBlock){
        return;
    }
    stateBlock->animationState.store(AnimationState::Cancelled);
}

void AnimationHandle::seek(float normalized){
    if(!stateBlock){
        return;
    }
    stateBlock->animationProgress.store(clamp01(normalized));
}

void AnimationHandle::setPlaybackRate(float rate){
    if(!stateBlock){
        return;
    }
    const float safeRate = std::max(rate,std::numeric_limits<float>::epsilon());
    stateBlock->animationRate.store(safeRate);
}

void AnimationHandle::setStateInternal(AnimationState state){
    if(stateBlock){
        stateBlock->animationState.store(state);
    }
}

void AnimationHandle::setProgressInternal(float normalized){
    if(stateBlock){
        stateBlock->animationProgress.store(clamp01(normalized));
    }
}

void AnimationHandle::setSubmittedPacketIdInternal(std::uint64_t packetId){
    if(stateBlock){
        stateBlock->submittedPacketId.store(packetId);
    }
}

void AnimationHandle::setPresentedPacketIdInternal(std::uint64_t packetId){
    if(stateBlock){
        stateBlock->presentedPacketId.store(packetId);
    }
}

void AnimationHandle::incrementDroppedPacketCountInternal(){
    if(stateBlock){
        stateBlock->droppedPacketCount.fetch_add(1);
    }
}

void AnimationHandle::setFailureReasonInternal(const OmegaCommon::String & reason){
    if(!stateBlock){
        return;
    }
    {
        std::lock_guard<std::mutex> lk(stateBlock->failureMutex);
        stateBlock->failureReason = reason;
    }
    stateBlock->animationState.store(AnimationState::Failed);
}

LayerAnimator::LayerAnimator(Layer &layer,ViewAnimator & parentAnimator):
CompositorClient(parentAnimator),
targetLayer(layer),
parentAnimator(parentAnimator){

}

void LayerAnimator::queueLayerResizeDelta(int delta_x,int delta_y,int delta_w,int delta_h){
    Timestamp start = std::chrono::high_resolution_clock::now();
    Timestamp deadline = start;
    pushLayerResizeCommand(&targetLayer,delta_x,delta_y,delta_w,delta_h,start,deadline);
}

AnimationHandle LayerAnimator::animate(const LayerClip & clip,const TimingOptions & timing){
    return animateOnLane(clip,timing,parentAnimator._client.getSyncLaneId());
}

AnimationHandle LayerAnimator::animateOnLane(const LayerClip & clip,
                                             const TimingOptions & timing,
                                             std::uint64_t syncLaneId){
    return detail::registerLayerAnimation(this,syncLaneId,clip,timing);
}

void LayerAnimator::transition(SharedHandle<CanvasFrame> &from,
                               SharedHandle<CanvasFrame> &to,
                               unsigned duration,
                               const SharedHandle<AnimationCurve> &curve) {
    (void)from;
    (void)to;
    (void)duration;
    (void)curve;
}

void LayerAnimator::resizeTransition(unsigned int delta_x,
                                     unsigned int delta_y,
                                     unsigned int delta_w,
                                     unsigned int delta_h,
                                     unsigned duration,
                                     const SharedHandle<AnimationCurve> &curve) {
    assert(duration > 0 && "Cannot have null duration");
    auto startRect = targetLayer.getLayerRect();
    auto endRect = startRect;
    endRect.pos.x += static_cast<float>(delta_x);
    endRect.pos.y += static_cast<float>(delta_y);
    endRect.w += static_cast<float>(delta_w);
    endRect.h += static_cast<float>(delta_h);

    KeyframeValue<Core::Rect> startKey {};
    startKey.offset = 0.f;
    startKey.value = startRect;
    startKey.easingToNext = curve ? curve : AnimationCurve::Linear();
    KeyframeValue<Core::Rect> endKey {};
    endKey.offset = 1.f;
    endKey.value = endRect;

    LayerClip clip {};
    clip.rect = KeyframeTrack<Core::Rect>::From({startKey,endKey});
    TimingOptions options {};
    options.durationMs = duration;
    options.frameRateHint = static_cast<std::uint16_t>(parentAnimator.framePerSec);
    options.clockMode = ClockMode::Hybrid;
    animate(clip,options);
}

void LayerAnimator::animate(const SharedHandle<AnimationTimeline> &timeline,
                            unsigned duration) {
    (void)timeline;
    (void)duration;
}

void LayerAnimator::pause() {
    detail::updateOwnerState(this,AnimationState::Paused);
}

void LayerAnimator::resume() {
    detail::updateOwnerState(this,AnimationState::Running);
}

void LayerAnimator::setFrameRate(unsigned int _framePerSec) {
    parentAnimator.setFrameRate(_framePerSec);
}

void LayerAnimator::applyShadow(const LayerEffect::DropShadowParams &params) {
    auto effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::DropShadow});
    effect->dropShadow = params;
    Timestamp start = std::chrono::high_resolution_clock::now();
    Timestamp deadline = start;
    pushLayerEffectCommand(&targetLayer,effect,start,deadline);
}

void LayerAnimator::applyTransformation(const LayerEffect::TransformationParams &params) {
    auto effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::Transformation});
    effect->transform = params;
    Timestamp start = std::chrono::high_resolution_clock::now();
    Timestamp deadline = start;
    pushLayerEffectCommand(&targetLayer,effect,start,deadline);
}

void LayerAnimator::shadowTransition(const LayerEffect::DropShadowParams &from,
                                     const LayerEffect::DropShadowParams &to,
                                     unsigned duration,
                                     const SharedHandle<AnimationCurve> &curve) {
    assert(duration > 0 && "Cannot have null duration");
    KeyframeValue<LayerEffect::DropShadowParams> startKey {};
    startKey.offset = 0.f;
    startKey.value = from;
    startKey.easingToNext = curve ? curve : AnimationCurve::Linear();
    KeyframeValue<LayerEffect::DropShadowParams> endKey {};
    endKey.offset = 1.f;
    endKey.value = to;

    LayerClip clip {};
    clip.shadow = KeyframeTrack<LayerEffect::DropShadowParams>::From({startKey,endKey});
    TimingOptions options {};
    options.durationMs = duration;
    options.frameRateHint = static_cast<std::uint16_t>(parentAnimator.framePerSec);
    options.clockMode = ClockMode::Hybrid;
    animate(clip,options);
}

void LayerAnimator::transformationTransition(const LayerEffect::TransformationParams &from,
                                             const LayerEffect::TransformationParams &to,
                                             unsigned duration,
                                             const SharedHandle<AnimationCurve> &curve) {
    assert(duration > 0 && "Cannot have null duration");
    KeyframeValue<LayerEffect::TransformationParams> startKey {};
    startKey.offset = 0.f;
    startKey.value = from;
    startKey.easingToNext = curve ? curve : AnimationCurve::Linear();
    KeyframeValue<LayerEffect::TransformationParams> endKey {};
    endKey.offset = 1.f;
    endKey.value = to;

    LayerClip clip {};
    clip.transform = KeyframeTrack<LayerEffect::TransformationParams>::From({startKey,endKey});
    TimingOptions options {};
    options.durationMs = duration;
    options.frameRateHint = static_cast<std::uint16_t>(parentAnimator.framePerSec);
    options.clockMode = ClockMode::Hybrid;
    animate(clip,options);
}

LayerAnimator::~LayerAnimator(){
    detail::cancelOwner(this);
}

unsigned int ViewAnimator::calculateTotalFrames(unsigned int &duration) {
    assert(duration > 0 && "Cannot have null duration");
    auto framesPerSec = static_cast<float>(framePerSec);
    auto totalFrames = (static_cast<float>(duration)/1000.f) * framesPerSec;
    float extra = 0.f;
    std::modf(totalFrames,&extra);
    assert((extra == 0.f) && "Cannot have animation with incomplete frame intervals");
    return static_cast<unsigned>(totalFrames);
}

ViewAnimator::ViewAnimator(CompositorClientProxy & _client):
CompositorClient(_client),
_client(_client),
framePerSec(30){

}

void ViewAnimator::queueViewResizeDelta(int delta_x,int delta_y,int delta_w,int delta_h){
    if(nativeView == nullptr){
        return;
    }
    Timestamp start = std::chrono::high_resolution_clock::now();
    Timestamp deadline = start;
    pushViewResizeCommand(nativeView,delta_x,delta_y,delta_w,delta_h,start,deadline);
}

AnimationHandle ViewAnimator::animate(const ViewClip & clip,const TimingOptions & timing){
    return animateOnLane(clip,timing,_client.getSyncLaneId());
}

AnimationHandle ViewAnimator::animateOnLane(const ViewClip & clip,
                                            const TimingOptions & timing,
                                            std::uint64_t syncLaneId){
    return detail::registerViewAnimation(this,syncLaneId,clip,timing);
}

void ViewAnimator::pause() {
    detail::updateOwnerState(this,AnimationState::Paused);
}

void ViewAnimator::resume() {
    detail::updateOwnerState(this,AnimationState::Running);
}

void ViewAnimator::setFrameRate(unsigned int _framePerSec) {
    assert(_framePerSec > 1 && "Must have more than 1 frame per second");
    framePerSec = _framePerSec;
}

void ViewAnimator::resizeTransition(unsigned int delta_x,
                                    unsigned int delta_y,
                                    unsigned int delta_w,
                                    unsigned int delta_h,
                                    unsigned int duration,
                                    const SharedHandle<AnimationCurve> &curve) {
    assert(duration > 0 && "Cannot have null duration");
    if(nativeView == nullptr){
        return;
    }
    auto startRect = nativeView->getRect();
    auto endRect = startRect;
    endRect.pos.x += static_cast<float>(delta_x);
    endRect.pos.y += static_cast<float>(delta_y);
    endRect.w += static_cast<float>(delta_w);
    endRect.h += static_cast<float>(delta_h);

    KeyframeValue<Core::Rect> startKey {};
    startKey.offset = 0.f;
    startKey.value = startRect;
    startKey.easingToNext = curve ? curve : AnimationCurve::Linear();
    KeyframeValue<Core::Rect> endKey {};
    endKey.offset = 1.f;
    endKey.value = endRect;

    ViewClip clip {};
    clip.rect = KeyframeTrack<Core::Rect>::From({startKey,endKey});
    TimingOptions options {};
    options.durationMs = duration;
    options.frameRateHint = static_cast<std::uint16_t>(framePerSec);
    options.clockMode = ClockMode::Hybrid;
    animate(clip,options);
}

SharedHandle<LayerAnimator> ViewAnimator::layerAnimator(Layer &layer){
    auto animator = SharedHandle<LayerAnimator>(new LayerAnimator(layer,*this));
    layerAnims.push_back(animator);
    return animator;
}

ViewAnimator::~ViewAnimator(){
    detail::cancelOwner(this);
}


};
