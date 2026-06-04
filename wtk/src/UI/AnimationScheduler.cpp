#include "AnimationScheduler.h"

#include "omegaWTK/UI/AppWindow.h"   // AppWindow — stored by ref; not dereferenced until 4.4/4.7.
#include "FrameBuilder.h"             // Phase 4.4: paint-purity asserts read the active phase.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace OmegaWTK {

// Phase 4.4: NodeId allocator. `View::nodeId()` reads from this on
// construction; UIView allocates one per `(UIView, UIElementTag)` pair
// lazily. Starts at 1 so 0 stays a sentinel "no node" value.
NodeId allocateNodeId(){
    static std::atomic<NodeId> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

namespace {

// Phase 4.4 paint-purity guard: registration is only valid outside the
// Paint / Commit passes; Paint is a pure reader of the side table. No-op
// when no frame is in flight (headless paths, tests).
void assertNotInPaintOrCommit(const char * call){
    if(auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr){
        const auto phase = fb->currentPhase();
        assert(phase != FramePhase::Paint && phase != FramePhase::Commit &&
               "AnimationScheduler registration during Paint/Commit");
        (void)phase;
    }
    (void)call;
}

// Phase 4.4 paint-purity guard: the only legal writer of the side table
// is tick(). No-op outside an active frame for the same reason as above.
void assertSideTableWriteInTick(){
    if(auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr){
        const auto phase = fb->currentPhase();
        assert(phase == FramePhase::Tick &&
               "AnimationScheduler::setTableValue outside Tick");
        (void)phase;
    }
}

std::uint64_t steadyNowNs(){
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Map a per-iteration fraction [0,1] to the sample point, honouring the
// playback direction and which iteration (0-based) we are on.
float directionAdjust(Composition::Direction dir, double frac, std::uint64_t iterIndex){
    const bool oddIter = (iterIndex % 2ULL) != 0ULL;
    switch(dir){
        case Composition::Direction::Normal:           return static_cast<float>(frac);
        case Composition::Direction::Reverse:          return static_cast<float>(1.0 - frac);
        case Composition::Direction::Alternate:        return static_cast<float>(oddIter ? 1.0 - frac : frac);
        case Composition::Direction::AlternateReverse: return static_cast<float>(oddIter ? frac : 1.0 - frac);
    }
    return static_cast<float>(frac);
}

} // namespace

// ---- Impl --------------------------------------------------------------

struct AnimationScheduler::Impl {
    // One live animation. Property anims write the side table; callback
    // anims fire apply(). The two kinds share everything but `sample`'s
    // body and where they are keyed.
    struct Active {
        Composition::AnimationId     id = 0;
        Composition::AnimationHandle handle;
        Composition::TimingOptions   timing;
        std::function<void(float)>   sample;
        bool             isProperty      = false;
        PropertyTableKey propKey;             // valid iff isProperty
        bool             layoutAffecting = false;
        bool             started         = false;
        std::uint64_t    startNs         = 0; // timeline origin (after delay), absolute
    };

    AppWindow &   window;
    std::uint64_t idSeed = 1;

    std::unordered_map<PropertyTableKey, Active, PropertyTableKeyHash> propertyAnims;
    std::unordered_map<Composition::AnimationId, Active>              callbackAnims;
    std::unordered_map<PropertyTableKey, AnimatedValue, PropertyTableKeyHash> table;

    Stats lastStats {};

    explicit Impl(AppWindow & w): window(w) {}
};

// ---- ctor / dtor -------------------------------------------------------

AnimationScheduler::AnimationScheduler(AppWindow & window)
    : impl_(std::make_unique<Impl>(window)) {}

AnimationScheduler::~AnimationScheduler() = default;

// ---- registration (type-erased, shared by the templated API) -----------

Composition::AnimationHandle AnimationScheduler::registerProperty(
        const PropertyTableKey & key, std::function<void(float)> sample,
        Composition::TimingOptions timing, bool layoutAffecting){
    assertNotInPaintOrCommit("registerProperty");
    // Re-registering the same (node,key,sub) REPLACES the prior animation
    // — Animation-Scheduler-Plan §6 Q3: tweenProperty replaces (cancels
    // the prior, starts fresh); retargeting is transition-only behaviour
    // and lands with the StyleResolver friend hook in Tier D.
    auto existing = impl_->propertyAnims.find(key);
    if(existing != impl_->propertyAnims.end()){
        existing->second.handle.setStateInternal(Composition::AnimationState::Cancelled);
    }

    const auto id = impl_->idSeed++;
    auto handle = Composition::AnimationHandle::Create(id, Composition::AnimationState::Pending);

    Impl::Active active;
    active.id              = id;
    active.handle          = handle;
    active.timing          = timing;
    active.sample          = std::move(sample);
    active.isProperty      = true;
    active.propKey         = key;
    active.layoutAffecting = layoutAffecting;

    impl_->propertyAnims[key] = std::move(active);
    return handle;
}

Composition::AnimationHandle AnimationScheduler::registerCallback(
        std::function<void(float)> sample, Composition::TimingOptions timing){
    assertNotInPaintOrCommit("registerCallback");
    const auto id = impl_->idSeed++;
    auto handle = Composition::AnimationHandle::Create(id, Composition::AnimationState::Pending);

    Impl::Active active;
    active.id         = id;
    active.handle     = handle;
    active.timing     = timing;
    active.sample     = std::move(sample);
    active.isProperty = false;

    impl_->callbackAnims[id] = std::move(active);
    return handle;
}

// ---- side table --------------------------------------------------------

void AnimationScheduler::setTableValue(const PropertyTableKey & key, AnimatedValue value){
    assertSideTableWriteInTick();
    impl_->table[key] = std::move(value);
}

void AnimationScheduler::seedTableFromStyle(const PropertyTableKey & key, AnimatedValue value){
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    // friend-only seed used by `transition<T>` to install the prev
    // value into the side table during Phase 2 (Style). The public
    // `setTableValue` asserts Phase==Tick because Tick is the only
    // legal writer in steady state; the prev-value seed is the one
    // documented exception (Animation-Scheduler-Plan §3.7) — without
    // it, the very first frame after a transition starts paints the
    // post-transition target instead of the pre-transition value.
    if(auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr){
        const auto phase = fb->currentPhase();
        assert(phase == FramePhase::Style &&
               "AnimationScheduler::seedTableFromStyle outside Style");
        (void)phase;
    }
    impl_->table[key] = std::move(value);
}

const AnimatedValue * AnimationScheduler::lookup(const PropertyTableKey & key) const{
    auto it = impl_->table.find(key);
    return (it != impl_->table.end()) ? &it->second : nullptr;
}

bool AnimationScheduler::hasAnyAnimationFor(NodeId node) const{
    for(const auto & entry : impl_->propertyAnims){
        if(entry.first.node == node){
            return true;
        }
    }
    return false;
}

// ---- tick --------------------------------------------------------------

void AnimationScheduler::tick(FrameTime now){
    const std::uint64_t tickStart = steadyNowNs();
    Stats s {};

    // Advance one active animation against `now`. Returns true if the
    // animation should be reaped (completed / cancelled / failed).
    auto advance = [&](Impl::Active & a) -> bool {
        const auto state = a.handle.state();
        if(state == Composition::AnimationState::Cancelled ||
           state == Composition::AnimationState::Completed ||
           state == Composition::AnimationState::Failed){
            return true;   // already terminal — reap
        }
        if(state == Composition::AnimationState::Paused){
            return false;  // held; do not advance
        }

        if(!a.started){
            a.started = true;
            a.startNs = now.monotonicNs +
                        static_cast<std::uint64_t>(a.timing.delayMs) * 1'000'000ULL;
        }
        if(now.monotonicNs < a.startNs){
            a.handle.setStateInternal(Composition::AnimationState::Pending);
            return false;  // still inside the start delay
        }

        const double rate       = std::max(1.0e-6f, a.handle.playbackRate());
        const double durNs       = static_cast<double>(a.timing.durationMs) * 1.0e6;
        const double elapsedNs   = static_cast<double>(now.monotonicNs - a.startNs) * rate;
        const double iterations  = (a.timing.iterations > 0.f)
                                       ? static_cast<double>(a.timing.iterations) : 1.0;

        double totalT = (durNs > 0.0) ? (elapsedNs / durNs) : iterations;
        const bool finished = totalT >= iterations;
        if(finished){
            totalT = iterations;
        }

        double iterIndex = std::floor(totalT);
        double frac      = totalT - iterIndex;
        if(finished && frac == 0.0 && totalT > 0.0){
            // Land exactly on the final endpoint of the last iteration
            // rather than on the start of a phantom next one.
            iterIndex = std::max(0.0, iterIndex - 1.0);
            frac = 1.0;
        }

        const float sampleT = directionAdjust(a.timing.direction, frac,
                                               static_cast<std::uint64_t>(iterIndex));

        // Property: writes the side table. Callback: fires apply().
        // NOTE (4.4/4.7 seam): the plan also has tick mark the target
        // node dirty here — DirtyBit::Layout|Paint when `a.layoutAffecting`,
        // else DirtyBit::Paint. There is no NodeId->View registry yet, so
        // that marking is deferred; `a.layoutAffecting` is already carried
        // so 4.7 only has to resolve the node and OR the bits.
        a.sample(sampleT);
        ++s.ticksThisFrame;
        if(!a.isProperty){
            ++s.appliesFired;
        }

        a.handle.setProgressInternal(
            static_cast<float>(std::min(1.0, iterations > 0.0 ? totalT / iterations : 1.0)));

        if(finished){
            a.handle.setStateInternal(Composition::AnimationState::Completed);
            // Clear the side-table cell on completion (Animation-Scheduler-
            // Plan §2): Paint then falls back to the resolved style, which
            // the just-completed animation has already driven to its end
            // value. (FillMode-based "hold final value" retention was
            // considered, but WML/transition semantics aren't pinned down
            // yet — clear-on-completion is the agreed behaviour for now.)
            if(a.isProperty){
                impl_->table.erase(a.propKey);
            }
            return true;   // reap; the handle keeps Completed for the caller
        }

        a.handle.setStateInternal(Composition::AnimationState::Running);
        return false;
    };

    for(auto it = impl_->propertyAnims.begin(); it != impl_->propertyAnims.end(); ){
        if(advance(it->second)){
            it = impl_->propertyAnims.erase(it);
        } else {
            ++it;
        }
    }
    for(auto it = impl_->callbackAnims.begin(); it != impl_->callbackAnims.end(); ){
        if(advance(it->second)){
            it = impl_->callbackAnims.erase(it);
        } else {
            ++it;
        }
    }

    s.activeProperty = static_cast<std::uint32_t>(impl_->propertyAnims.size());
    s.activeCallback = static_cast<std::uint32_t>(impl_->callbackAnims.size());
    s.tickElapsedNs  = steadyNowNs() - tickStart;
    impl_->lastStats = s;
}

// ---- lifecycle ---------------------------------------------------------

void AnimationScheduler::cancelAllForNode(NodeId node){
    for(auto it = impl_->propertyAnims.begin(); it != impl_->propertyAnims.end(); ){
        if(it->first.node == node){
            it->second.handle.setStateInternal(Composition::AnimationState::Cancelled);
            it = impl_->propertyAnims.erase(it);
        } else {
            ++it;
        }
    }
    for(auto it = impl_->table.begin(); it != impl_->table.end(); ){
        if(it->first.node == node){
            it = impl_->table.erase(it);
        } else {
            ++it;
        }
    }
}

void AnimationScheduler::pauseAll(){
    for(auto & entry : impl_->propertyAnims){ entry.second.handle.pause(); }
    for(auto & entry : impl_->callbackAnims){ entry.second.handle.pause(); }
}

void AnimationScheduler::resumeAll(){
    for(auto & entry : impl_->propertyAnims){ entry.second.handle.resume(); }
    for(auto & entry : impl_->callbackAnims){ entry.second.handle.resume(); }
}

void AnimationScheduler::cancelAll(){
    for(auto & entry : impl_->propertyAnims){ entry.second.handle.cancel(); }
    for(auto & entry : impl_->callbackAnims){ entry.second.handle.cancel(); }
    impl_->propertyAnims.clear();
    impl_->callbackAnims.clear();
    impl_->table.clear();
}

AnimationScheduler::Stats AnimationScheduler::stats() const{
    Stats s = impl_->lastStats;
    // Active counts are live (registration may have happened since the
    // last tick); per-tick counters carry from the last tick.
    s.activeProperty = static_cast<std::uint32_t>(impl_->propertyAnims.size());
    s.activeCallback = static_cast<std::uint32_t>(impl_->callbackAnims.size());
    return s;
}

} // namespace OmegaWTK
