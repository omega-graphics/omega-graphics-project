#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "backend/GeometryConvert.h"
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

ScalarTraverse::ScalarTraverse(Point2D start, Point2D end, unsigned int speed) :
start_pt(start),
end_pt(end),
cur(start_pt),speed(speed)
{
    auto slope = (end_pt.y - start_pt.y)/(end_pt.x - start_pt.x);
    auto alpha = std::atan(slope);
    delta_x = std::cos(alpha) * float(speed);
    delta_y = slope * delta_x;
    

}

Point2D ScalarTraverse::get() {
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

void ScalarTraverse::changeScalar(Point2D start, Point2D end) {
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

SharedHandle<AnimationCurve> AnimationCurve::Quadratic(Point2D a){
    auto curve = SharedHandle<AnimationCurve>(new AnimationCurve{Type::QuadraticBezier,0.f,1.f});
    curve->a = a;
    return curve;
}

SharedHandle<AnimationCurve> AnimationCurve::Cubic(Point2D a,Point2D b){
    return CubicBezier(a,b,0.f,1.f);
}

SharedHandle<AnimationCurve> AnimationCurve::CubicBezier(Point2D a,
                                                         Point2D b,
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
            Point2D {0.f,curve.start_h * space_h},
            Point2D {space_w,curve.end_h * space_h}
            )
        };
        initState = malloc(sizeof(AnimationCurveLinearTraversal));
        memcpy(initState,data,sizeof(AnimationCurveLinearTraversal));
    }
    else if(curve.type == Type::QuadraticBezier){
        data = new AnimationCurveQuadraticTraversal{
            ScalarTraverse(
                Point2D{0.f,curve.start_h * space_h},
                Point2D{curve.a.x * space_w,curve.a.y * space_h}
            ),
            ScalarTraverse(
                Point2D{curve.a.x * space_w,curve.a.y * space_h},
                Point2D{space_w,curve.end_h * space_h}
            ),


            ScalarTraverse(
                Point2D{0.f,curve.start_h * space_h},
                Point2D{curve.a.x * space_w,curve.a.y * space_h}
            )
        };
        initState = malloc(sizeof(AnimationCurveQuadraticTraversal));
        memcpy(initState,data,sizeof(AnimationCurveQuadraticTraversal));
    }
    else {
        data = new AnimationCurveCubicTraversal{
                    ScalarTraverse(
                        Point2D{0.f,curve.start_h * space_h},
                        Point2D{curve.a.x * space_w,curve.a.y * space_h}
                    ),
                    ScalarTraverse(
                        Point2D{curve.a.x * space_w,curve.a.y * space_h},
                        Point2D{curve.b.x * space_w,curve.b.y * space_h}
                    ),
                    ScalarTraverse(
                        Point2D{curve.a.x * space_w,curve.a.y * space_h},
                        Point2D{space_w,curve.end_h * space_h}
                    ),


                    ScalarTraverse(
                        Point2D{0.f,curve.start_h * space_h},
                        Point2D{curve.a.x * space_w,curve.a.y * space_h}
                    ),
                    ScalarTraverse(
                        Point2D{curve.a.x * space_w,curve.a.y * space_h},
                        Point2D{curve.b.x * space_w,curve.b.y * space_h}
                    ),

                    ScalarTraverse(
                        Point2D{0.f,curve.start_h * space_h},
                        Point2D{curve.a.x * space_w,curve.a.y * space_h}
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

Point2D AnimationCurve::Traversal::get() {
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


// Phase 4.8: `namespace detail { ... }` block deleted. It held the
// pre-scheduler animation runtime - `AnimationRuntimeRegistry`,
// the `AnimationInstance` lifecycle struct, the lane worker /
// telemetry / shutdown plumbing the old `ViewAnimator` /
// `LayerAnimator` posted into. The `AnimationScheduler` in
// `wtk/src/UI/AnimationScheduler.{h,cpp}` is the only live
// animation runtime post-4.4; the legacy registry is dead
// (no callers, no tests, no backend touches).


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


// Phase 4.8: `LayerAnimator` / `ViewAnimator` member implementations
// deleted alongside the class declarations (see `Animation.h`). The
// pre-scheduler animation runtime no longer has a producer or
// consumer in the tree.


};
