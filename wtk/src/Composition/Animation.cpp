#include "omegaWTK/Composition/Animation.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cassert>
#include <cmath>
#include <limits>

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

    StateBlock(AnimationId id,AnimationState state):
    animationId(id),
    animationState(state){}
};

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



LayerAnimator::LayerAnimator(Layer &layer,ViewAnimator & parentAnimator):CompositorClient(parentAnimator), targetLayer(layer) ,parentAnimator(parentAnimator){
    
}

AnimationHandle LayerAnimator::animate(const LayerClip & clip,const TimingOptions & timing){
    (void)timing;
    if(!clip.rect && !clip.transform && !clip.shadow && !clip.opacity){
        return {};
    }
    return AnimationHandle::Create(g_animationIdSeed.fetch_add(1),AnimationState::Pending);
}


void LayerAnimator::transition(SharedHandle<CanvasFrame> &from, SharedHandle<CanvasFrame> &to, unsigned duration,
                                   const SharedHandle<AnimationCurve> &curve) {
    (void)from;
    (void)to;
    (void)curve;
    assert(duration > 0 && "Cannot have null duration");
    auto totalFrames = parentAnimator.calculateTotalFrames(duration);

    

    Timestamp timestamp = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(duration/totalFrames);
    Timestamp deadline = timestamp + frameInterval;
    for(;totalFrames > 0;totalFrames--){
        
        {

        }

        deadline += frameInterval;
    }

}

void LayerAnimator::resizeTransition(unsigned int delta_x, unsigned int delta_y, unsigned int delta_w,
                                         unsigned int delta_h, unsigned duration,
                                         const SharedHandle<AnimationCurve> &curve) {
    (void)curve;
    assert(duration > 0 && "Cannot have null duration");
    const auto totalFrames = parentAnimator.calculateTotalFrames(duration);
    if(totalFrames == 0){
        return;
    }
    Timestamp timestamp = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(duration/totalFrames);
    Timestamp deadline = timestamp + frameInterval;
    for(unsigned i = 0; i < totalFrames; i++){
        pushLayerResizeCommand(&targetLayer,
                               delta_x/totalFrames,
                               delta_y/totalFrames,
                               delta_w/totalFrames,
                               delta_h/totalFrames,
                               timestamp,
                               deadline);
        deadline += frameInterval;
    }
}

void LayerAnimator::animate(const SharedHandle<AnimationTimeline> &timeline,
                                unsigned duration) {
    (void)timeline;
    assert(duration > 0 && "Cannot have null duration");

}

 void LayerAnimator::pause() {
     cancelCurrentJobs();
 }

void LayerAnimator::resume() {
    // Layer animation commands are queue-driven and resume is currently no-op.
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
    const auto totalFrames = parentAnimator.calculateTotalFrames(duration);
    if(totalFrames == 0){
        return;
    }

    auto easing = curve ? curve : AnimationCurve::Linear(0.f,1.f);
    Timestamp start = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(duration / totalFrames);
    Timestamp deadline = start + frameInterval;

    for(unsigned i = 0; i < totalFrames; i++){
        const float t = static_cast<float>(i + 1) / static_cast<float>(totalFrames);
        const float eased = easing->sample(t);
        auto params = detail::KeyframeLerp<LayerEffect::DropShadowParams>::apply(from,to,detail::clamp01(eased));
        auto effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::DropShadow});
        effect->dropShadow = params;
        pushLayerEffectCommand(&targetLayer,effect,start,deadline);
        deadline += frameInterval;
    }
}

void LayerAnimator::transformationTransition(const LayerEffect::TransformationParams &from,
                                             const LayerEffect::TransformationParams &to,
                                             unsigned duration,
                                             const SharedHandle<AnimationCurve> &curve) {
    assert(duration > 0 && "Cannot have null duration");
    const auto totalFrames = parentAnimator.calculateTotalFrames(duration);
    if(totalFrames == 0){
        return;
    }

    auto easing = curve ? curve : AnimationCurve::Linear(0.f,1.f);
    Timestamp start = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(duration / totalFrames);
    Timestamp deadline = start + frameInterval;

    for(unsigned i = 0; i < totalFrames; i++){
        const float t = static_cast<float>(i + 1) / static_cast<float>(totalFrames);
        const float eased = easing->sample(t);
        auto params = detail::KeyframeLerp<LayerEffect::TransformationParams>::apply(from,to,detail::clamp01(eased));
        auto effect = std::make_shared<LayerEffect>(LayerEffect{LayerEffect::Transformation});
        effect->transform = params;
        pushLayerEffectCommand(&targetLayer,effect,start,deadline);
        deadline += frameInterval;
    }
}

unsigned int ViewAnimator::calculateTotalFrames(unsigned int &duration) {
    assert(duration > 0 && "Cannot have null duration");
    auto framesPerSec = (float)framePerSec;
    auto totalFrames = ((float)duration/1000.f) * framesPerSec;
    float extra;
    std::modf(totalFrames,&extra);
    assert((extra == 0.f) && "Cannot have animation with incomplete frame intervals");
    return (unsigned) totalFrames;
}

ViewAnimator::ViewAnimator(CompositorClientProxy & _client):CompositorClient(_client), _client(_client),framePerSec(30){

}

AnimationHandle ViewAnimator::animate(const ViewClip & clip,const TimingOptions & timing){
    (void)timing;
    if(!clip.rect && !clip.opacity){
        return {};
    }
    return AnimationHandle::Create(g_animationIdSeed.fetch_add(1),AnimationState::Pending);
}

void ViewAnimator::pause() {
    cancelCurrentJobs();
}

void ViewAnimator::resume() {
    // View animation commands are queue-driven and resume is currently no-op.
}

void ViewAnimator::setFrameRate(unsigned int _framePerSec) {
    assert(_framePerSec > 1 && "Must have more than 1 frame per second");
    framePerSec = _framePerSec;
}

void ViewAnimator::resizeTransition(unsigned int delta_x, unsigned int delta_y, unsigned int delta_w,
                                        unsigned int delta_h, unsigned int duration,
                                        const SharedHandle<AnimationCurve> &curve) {
    (void)curve;
    const auto totalFrames = calculateTotalFrames(duration);
    if(totalFrames == 0){
        return;
    }
    Timestamp timestamp = std::chrono::high_resolution_clock::now();
    auto frameInterval = std::chrono::milliseconds(duration/totalFrames);
    Timestamp deadline = timestamp + frameInterval;
    for(unsigned i = 0; i < totalFrames; i++){
        pushViewResizeCommand(nativeView,
                              delta_x/totalFrames,
                              delta_y/totalFrames,
                              delta_w/totalFrames,
                              delta_h/totalFrames,
                              timestamp,
                              deadline);
        deadline += frameInterval;
    }
}

SharedHandle<LayerAnimator> ViewAnimator::layerAnimator(Layer &layer){
    auto animator = SharedHandle<LayerAnimator>(new LayerAnimator(layer,*this));
    layerAnims.push_back(animator);
    return animator;
}


};
