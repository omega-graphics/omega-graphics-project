#include "omegaWTK/UI/WidgetTreeHost.h"
#include "../Composition/Compositor.h"

#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <sstream>

namespace OmegaWTK {
    namespace {
        std::atomic<uint64_t> g_widgetTreeSyncLaneSeed {1};
        std::atomic<uint64_t> g_resizeSessionSeed {1};

        Composition::Compositor *globalCompositor(){
            static Composition::Compositor compositor;
            return &compositor;
        }

        struct ResizeTrackerTuning {
            float velocitySettlingEpsilon = 20.f;
            float accelerationSettlingEpsilon = 80.f;
            double deadPeriodMs = 120.0;
            float significantDeltaPx = 0.5f;
        };

        struct ResizeValidationTuning {
            bool enabled = false;
            bool failHard = false;
            double maxDropRatio = 0.60;
            std::uint64_t maxFailedPackets = 0;
            std::uint64_t maxEpochDrops = 12;
            std::uint64_t maxStaleCoordinatorPackets = 0;
            std::uint32_t minResizeSamples = 1;
        };

        static double readEnvDoubleClamp(const char *name,double fallback,double minValue,double maxValue){
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
            char *endPtr = nullptr;
            const auto parsed = std::strtod(raw,&endPtr);
            if(endPtr == raw || !std::isfinite(parsed)){
                return fallback;
            }
            return std::clamp(parsed,minValue,maxValue);
        }

        static float readEnvFloatClamp(const char *name,float fallback,float minValue,float maxValue){
            return static_cast<float>(readEnvDoubleClamp(name,fallback,minValue,maxValue));
        }

        static bool readEnvBool(const char *name,bool fallback){
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
            return raw[0] != '0';
        }

        static std::uint64_t readEnvU64Clamp(const char *name,std::uint64_t fallback,std::uint64_t minValue,std::uint64_t maxValue){
            const char *raw = std::getenv(name);
            if(raw == nullptr || raw[0] == '\0'){
                return fallback;
            }
            char *endPtr = nullptr;
            const auto parsed = std::strtoull(raw,&endPtr,10);
            if(endPtr == raw){
                return fallback;
            }
            return std::clamp<std::uint64_t>(parsed,minValue,maxValue);
        }

        static const ResizeTrackerTuning & resizeTrackerTuning(){
            static const ResizeTrackerTuning tuning = []{
                ResizeTrackerTuning cfg {};
                cfg.velocitySettlingEpsilon = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_VELOCITY_EPSILON",
                        cfg.velocitySettlingEpsilon,
                        1.f,
                        10000.f);
                cfg.accelerationSettlingEpsilon = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_ACCEL_EPSILON",
                        cfg.accelerationSettlingEpsilon,
                        1.f,
                        20000.f);
                cfg.deadPeriodMs = readEnvDoubleClamp(
                        "OMEGAWTK_RESIZE_DEAD_PERIOD_MS",
                        cfg.deadPeriodMs,
                        10.0,
                        2000.0);
                cfg.significantDeltaPx = readEnvFloatClamp(
                        "OMEGAWTK_RESIZE_SIGNIFICANT_DELTA_PX",
                        cfg.significantDeltaPx,
                        0.05f,
                        20.f);
                return cfg;
            }();
            return tuning;
        }

        static const ResizeValidationTuning & resizeValidationTuning(){
            static const ResizeValidationTuning tuning = []{
                ResizeValidationTuning cfg {};
                cfg.enabled = readEnvBool("OMEGAWTK_RESIZE_VALIDATION",cfg.enabled);
                cfg.failHard = readEnvBool("OMEGAWTK_RESIZE_VALIDATION_FAIL_HARD",cfg.failHard);
                cfg.maxDropRatio = readEnvDoubleClamp(
                        "OMEGAWTK_RESIZE_VALIDATION_MAX_DROP_RATIO",
                        cfg.maxDropRatio,
                        0.0,
                        1.0);
                cfg.maxFailedPackets = readEnvU64Clamp(
                        "OMEGAWTK_RESIZE_VALIDATION_MAX_FAILED_PACKETS",
                        cfg.maxFailedPackets,
                        0,
                        1000000);
                cfg.maxEpochDrops = readEnvU64Clamp(
                        "OMEGAWTK_RESIZE_VALIDATION_MAX_EPOCH_DROPS",
                        cfg.maxEpochDrops,
                        0,
                        1000000);
                cfg.maxStaleCoordinatorPackets = readEnvU64Clamp(
                        "OMEGAWTK_RESIZE_VALIDATION_MAX_STALE_COORD_PACKETS",
                        cfg.maxStaleCoordinatorPackets,
                        0,
                        1000000);
                cfg.minResizeSamples = static_cast<std::uint32_t>(readEnvU64Clamp(
                        "OMEGAWTK_RESIZE_VALIDATION_MIN_SAMPLES",
                        cfg.minResizeSamples,
                        1,
                        100000));
                return cfg;
            }();
            return tuning;
        }

        static const std::string & resizeValidationScenario(){
            static const std::string scenario = []{
                const char *raw = std::getenv("OMEGAWTK_RESIZE_VALIDATION_SCENARIO");
                if(raw == nullptr || raw[0] == '\0'){
                    return std::string("unspecified");
                }
                return std::string(raw);
            }();
            return scenario;
        }

        static const char *resizePhaseName(ResizePhase phase){
            switch(phase){
                case ResizePhase::Idle:
                    return "Idle";
                case ResizePhase::Active:
                    return "Active";
                case ResizePhase::Settling:
                    return "Settling";
                case ResizePhase::Completed:
                    return "Completed";
                default:
                    return "Unknown";
            }
        }

        static Composition::ResizeGovernorPhase toGovernorPhase(ResizePhase phase){
            switch(phase){
                case ResizePhase::Idle:
                    return Composition::ResizeGovernorPhase::Idle;
                case ResizePhase::Active:
                    return Composition::ResizeGovernorPhase::Active;
                case ResizePhase::Settling:
                    return Composition::ResizeGovernorPhase::Settling;
                case ResizePhase::Completed:
                    return Composition::ResizeGovernorPhase::Completed;
                default:
                    return Composition::ResizeGovernorPhase::Idle;
            }
        }

        static double nowMs(){
            using namespace std::chrono;
            return duration<double,std::milli>(steady_clock::now().time_since_epoch()).count();
        }

        static float finiteOrZero(float value){
            return std::isfinite(value) ? value : 0.f;
        }

        static Composition::ResizeGovernorMetadata makeResizeGovernorMetadata(const ResizeSessionState & state){
            Composition::ResizeGovernorMetadata metadata {};
            metadata.sessionId = state.sessionId;
            metadata.active = state.phase == ResizePhase::Active ||
                              state.phase == ResizePhase::Settling;
            metadata.animatedTree = state.animatedTree;
            metadata.velocityPxPerSec = finiteOrZero(state.sample.velocityPxPerSec);
            metadata.accelerationPxPerSec2 = finiteOrZero(state.sample.accelerationPxPerSec2);
            metadata.phase = toGovernorPhase(state.phase);
            return metadata;
        }

        static bool laneValidationPass(const ResizeValidationTuning & tuning,
                                       std::uint32_t sampleCount,
                                       const Composition::Compositor::LaneDiagnosticsSnapshot & before,
                                       const Composition::Compositor::LaneDiagnosticsSnapshot & after,
                                       std::ostringstream & failureReason){
            const auto submittedDelta = after.submittedPacketCount - before.submittedPacketCount;
            const auto droppedDelta = after.droppedPacketCount - before.droppedPacketCount;
            const auto failedDelta = after.failedPacketCount - before.failedPacketCount;
            const auto epochDropDelta = after.epochDropCount - before.epochDropCount;
            const auto staleCoordDelta = after.staleCoordinatorGenerationPacketCount -
                                         before.staleCoordinatorGenerationPacketCount;
            const double dropRatio = submittedDelta == 0
                                     ? 0.0
                                     : static_cast<double>(droppedDelta) / static_cast<double>(submittedDelta);

            bool pass = true;
            if(sampleCount < tuning.minResizeSamples){
                failureReason << "insufficient_samples=" << sampleCount << " ";
                pass = false;
            }
            if(failedDelta > tuning.maxFailedPackets){
                failureReason << "failed_packets_delta=" << failedDelta << " ";
                pass = false;
            }
            if(dropRatio > tuning.maxDropRatio){
                failureReason << "drop_ratio=" << dropRatio << " ";
                pass = false;
            }
            if(epochDropDelta > tuning.maxEpochDrops){
                failureReason << "epoch_drops_delta=" << epochDropDelta << " ";
                pass = false;
            }
            if(staleCoordDelta > tuning.maxStaleCoordinatorPackets){
                failureReason << "stale_coord_delta=" << staleCoordDelta << " ";
                pass = false;
            }
            return pass;
        }
    }

    std::uint64_t ResizeDynamicsTracker::nextSessionId(){
        return g_resizeSessionSeed.fetch_add(1,std::memory_order_relaxed);
    }

    ResizeSessionState ResizeDynamicsTracker::begin(float width,float height,double tMs){
        const auto & tuning = resizeTrackerTuning();
        inSession = true;
        currentSessionId = nextSessionId();
        lastWidth = width;
        lastHeight = height;
        lastVelocity = 0.f;
        lastSignificantChangeMs = tMs;
        lastTick = std::chrono::steady_clock::now();
        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        state.phase = ResizePhase::Active;
        state.sample.timestampMs = tMs;
        state.sample.width = width;
        state.sample.height = height;
        state.sample.velocityPxPerSec = 0.f;
        state.sample.accelerationPxPerSec2 = 0.f;
        (void)tuning;
        return state;
    }

    ResizeSessionState ResizeDynamicsTracker::update(float width,float height,double tMs){
        const auto & tuning = resizeTrackerTuning();
        if(!inSession){
            return begin(width,height,tMs);
        }
        auto nowTick = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration<float>(nowTick - lastTick).count();
        const auto dw = width - lastWidth;
        const auto dh = height - lastHeight;
        const auto delta = std::sqrt((dw * dw) + (dh * dh));
        float velocity = 0.f;
        float acceleration = 0.f;
        if(dt > 1e-6f){
            velocity = delta / dt;
            acceleration = (velocity - lastVelocity) / dt;
        }
        velocity = finiteOrZero(velocity);
        acceleration = finiteOrZero(acceleration);
        if(delta >= tuning.significantDeltaPx){
            lastSignificantChangeMs = tMs;
        }

        ResizeSessionState state {};
        state.sessionId = currentSessionId;
        const bool settlingVelocity = std::fabs(velocity) <= tuning.velocitySettlingEpsilon;
        const bool settlingAcceleration = std::fabs(acceleration) <= tuning.accelerationSettlingEpsilon;
        const bool inDeadPeriod = (tMs - lastSignificantChangeMs) >= tuning.deadPeriodMs;
        if(settlingVelocity && settlingAcceleration){
            state.phase = inDeadPeriod ? ResizePhase::Completed : ResizePhase::Settling;
        }
        else {
            state.phase = ResizePhase::Active;
        }
        state.sample.timestampMs = tMs;
        state.sample.width = width;
        state.sample.height = height;
        state.sample.velocityPxPerSec = velocity;
        state.sample.accelerationPxPerSec2 = acceleration;

        lastTick = nowTick;
        lastWidth = width;
        lastHeight = height;
        lastVelocity = velocity;
        return state;
    }

    ResizeSessionState ResizeDynamicsTracker::end(float width,float height,double tMs){
        ResizeSessionState state = update(width,height,tMs);
        state.phase = ResizePhase::Completed;
        inSession = false;
        return state;
    }

    WidgetTreeHost::WidgetTreeHost():
    compositor(globalCompositor()),
    syncLaneId(g_widgetTreeSyncLaneSeed.fetch_add(1)),
    attachedToWindow(false)
    {

    };

    WidgetTreeHost::~WidgetTreeHost(){
        if(compositor != nullptr && root != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
        compositor = nullptr;
    };

    SharedHandle<WidgetTreeHost> WidgetTreeHost::Create(){
        return SharedHandle<WidgetTreeHost>(new WidgetTreeHost());
    };

    void WidgetTreeHost::initWidgetRecurse(Widget *parent){
        parent->init();
        for(auto & child : parent->children){
            initWidgetRecurse(child);
        }
    }

    void WidgetTreeHost::observeWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->observeLayerTree(parent->layerTree.get(),syncLaneId);
        }
        for(auto & child : parent->children){
            observeWidgetLayerTreesRecurse(child);
        }
    }

    void WidgetTreeHost::unobserveWidgetLayerTreesRecurse(Widget *parent){
        if(parent == nullptr || compositor == nullptr){
            return;
        }
        if(parent->layerTree != nullptr){
            compositor->unobserveLayerTree(parent->layerTree.get());
        }
        for(auto & child : parent->children){
            unobserveWidgetLayerTreesRecurse(child);
        }
    }

    void WidgetTreeHost::invalidateWidgetRecurse(Widget *parent,
                                                 PaintReason reason,
                                                 bool immediate){
        if(parent == nullptr){
            return;
        }
        if(parent->paintMode() == PaintMode::Automatic){
            if(immediate){
                parent->invalidateNow(reason);
            }
            else {
                parent->invalidate(reason);
            }
        }
        for(auto & child : parent->children){
            invalidateWidgetRecurse(child,reason,immediate);
        }
    }

    void WidgetTreeHost::beginResizeCoordinatorSessionRecurse(Widget *parent,std::uint64_t sessionId){
        if(parent == nullptr || parent->rootView == nullptr){
            return;
        }
        parent->rootView->getResizeCoordinator().beginResizeSession(sessionId);
        for(auto & child : parent->children){
            beginResizeCoordinatorSessionRecurse(child,sessionId);
        }
    }

    bool WidgetTreeHost::detectAnimatedTreeRecurse(Widget *parent) const{
        if(parent == nullptr){
            return false;
        }
        if(parent->paintMode() != PaintMode::Automatic){
            return true;
        }
        for(auto & child : parent->children){
            if(detectAnimatedTreeRecurse(child)){
                return true;
            }
        }
        return false;
    }

    void WidgetTreeHost::applyResizeGovernorMetadata(const Composition::ResizeGovernorMetadata & metadata){
        if(root == nullptr || root->rootView == nullptr){
            return;
        }
        root->rootView->setResizeGovernorMetadataRecurse(metadata,resizeCoordinatorGeneration);
    }

    void WidgetTreeHost::initWidgetTree(){
        observeWidgetLayerTreesRecurse(root.get());
        root->setTreeHostRecurse(this);
        initWidgetRecurse(root.get());
        auto repaintRecurse = [&](auto &&self,Widget *widget) -> void {
            if(widget == nullptr){
                return;
            }
            if(widget->paintMode() == PaintMode::Automatic){
                widget->invalidateNow(PaintReason::Initial);
            }
            for(auto & child : widget->children){
                self(self,child);
            }
        };
        repaintRecurse(repaintRecurse,root.get());
    }

    void WidgetTreeHost::notifyWindowResize(const Core::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
        }
        lastResizeSessionState = resizeTracker.update(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        applyResizeGovernorMetadata(makeResizeGovernorMetadata(lastResizeSessionState));
        if(resizeValidationSession.active &&
           resizeValidationSession.sessionId == lastResizeSessionState.sessionId){
            resizeValidationSession.sampleCount += 1;
            resizeValidationSession.peakVelocityPxPerSec = std::max(
                    resizeValidationSession.peakVelocityPxPerSec,
                    finiteOrZero(std::fabs(lastResizeSessionState.sample.velocityPxPerSec)));
            resizeValidationSession.peakAccelerationPxPerSec2 = std::max(
                    resizeValidationSession.peakAccelerationPxPerSec2,
                    finiteOrZero(std::fabs(lastResizeSessionState.sample.accelerationPxPerSec2)));
            resizeValidationSession.endTimestampMs = lastResizeSessionState.sample.timestampMs;
        }
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " v=" << lastResizeSessionState.sample.velocityPxPerSec
               << " a=" << lastResizeSessionState.sample.accelerationPxPerSec2
               << " gen=" << resizeCoordinatorGeneration
               << " policy=Paced";
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeBegin(const Core::Rect &rect){
        lastResizeSessionState = resizeTracker.begin(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        beginResizeCoordinatorSessionRecurse(root.get(),lastResizeSessionState.sessionId);
        if(root != nullptr){
            root->handleHostResize(rect);
        }
        applyResizeGovernorMetadata(makeResizeGovernorMetadata(lastResizeSessionState));
        const auto & validationTuning = resizeValidationTuning();
        if(validationTuning.enabled && compositor != nullptr){
            auto before = compositor->getLaneDiagnosticsSnapshot(syncLaneId);
            resizeValidationSession.active = true;
            resizeValidationSession.sessionId = lastResizeSessionState.sessionId;
            resizeValidationSession.sampleCount = 1;
            resizeValidationSession.beginTimestampMs = lastResizeSessionState.sample.timestampMs;
            resizeValidationSession.endTimestampMs = lastResizeSessionState.sample.timestampMs;
            resizeValidationSession.peakVelocityPxPerSec =
                    finiteOrZero(std::fabs(lastResizeSessionState.sample.velocityPxPerSec));
            resizeValidationSession.peakAccelerationPxPerSec2 =
                    finiteOrZero(std::fabs(lastResizeSessionState.sample.accelerationPxPerSec2));
            resizeValidationSession.baseSubmittedPackets = before.submittedPacketCount;
            resizeValidationSession.basePresentedPackets = before.presentedPacketCount;
            resizeValidationSession.baseDroppedPackets = before.droppedPacketCount;
            resizeValidationSession.baseFailedPackets = before.failedPacketCount;
            resizeValidationSession.baseEpochDrops = before.epochDropCount;
            resizeValidationSession.baseStaleCoordinatorPackets = before.staleCoordinatorGenerationPacketCount;
            std::ostringstream validationConfig;
            validationConfig << "ResizeValidationConfig lane=" << syncLaneId
                             << " scenario=" << resizeValidationScenario()
                             << " minSamples=" << validationTuning.minResizeSamples
                             << " maxDropRatio=" << validationTuning.maxDropRatio
                             << " maxFailedPackets=" << validationTuning.maxFailedPackets
                             << " maxEpochDrops=" << validationTuning.maxEpochDrops
                             << " maxStaleCoord=" << validationTuning.maxStaleCoordinatorPackets
                             << " failHard=" << (validationTuning.failHard ? "yes" : "no");
            OMEGAWTK_DEBUG(validationConfig.str());
        }
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " gen=" << resizeCoordinatorGeneration
               << " policy=Paced";
        OMEGAWTK_DEBUG(stream.str());
    }

    void WidgetTreeHost::notifyWindowResizeEnd(const Core::Rect &rect){
        if(root != nullptr){
            root->handleHostResize(rect);
        }
        lastResizeSessionState = resizeTracker.end(rect.w,rect.h,nowMs());
        lastResizeSessionState.animatedTree = detectAnimatedTreeRecurse(root.get());
        resizeCoordinatorGeneration += 1;
        applyResizeGovernorMetadata(makeResizeGovernorMetadata(lastResizeSessionState));
        std::ostringstream stream;
        stream << "ResizeSession lane=" << syncLaneId
               << " id=" << lastResizeSessionState.sessionId
               << " phase=" << resizePhaseName(lastResizeSessionState.phase)
               << " w=" << lastResizeSessionState.sample.width
               << " h=" << lastResizeSessionState.sample.height
               << " v=" << lastResizeSessionState.sample.velocityPxPerSec
               << " a=" << lastResizeSessionState.sample.accelerationPxPerSec2
               << " gen=" << resizeCoordinatorGeneration
               << " policy=Paced";
        OMEGAWTK_DEBUG(stream.str());

        const auto & validationTuning = resizeValidationTuning();
        if(validationTuning.enabled &&
           resizeValidationSession.active &&
           resizeValidationSession.sessionId == lastResizeSessionState.sessionId &&
           compositor != nullptr){
            auto after = compositor->getLaneDiagnosticsSnapshot(syncLaneId);
            const auto submittedDelta = after.submittedPacketCount - resizeValidationSession.baseSubmittedPackets;
            const auto presentedDelta = after.presentedPacketCount - resizeValidationSession.basePresentedPackets;
            const auto droppedDelta = after.droppedPacketCount - resizeValidationSession.baseDroppedPackets;
            const auto failedDelta = after.failedPacketCount - resizeValidationSession.baseFailedPackets;
            const auto epochDropDelta = after.epochDropCount - resizeValidationSession.baseEpochDrops;
            const auto staleCoordDelta = after.staleCoordinatorGenerationPacketCount -
                                         resizeValidationSession.baseStaleCoordinatorPackets;
            const auto durationMs = std::max(0.0,
                    resizeValidationSession.endTimestampMs - resizeValidationSession.beginTimestampMs);
            std::ostringstream failureReason;
            const bool pass = laneValidationPass(validationTuning,
                                                 resizeValidationSession.sampleCount,
                                                 Composition::Compositor::LaneDiagnosticsSnapshot{
                                                     .submittedPacketCount = resizeValidationSession.baseSubmittedPackets,
                                                     .presentedPacketCount = resizeValidationSession.basePresentedPackets,
                                                     .droppedPacketCount = resizeValidationSession.baseDroppedPackets,
                                                     .failedPacketCount = resizeValidationSession.baseFailedPackets,
                                                     .epochDropCount = resizeValidationSession.baseEpochDrops,
                                                     .staleCoordinatorGenerationPacketCount = resizeValidationSession.baseStaleCoordinatorPackets
                                                 },
                                                 after,
                                                 failureReason);
            std::ostringstream summary;
            summary << "ResizeValidation lane=" << syncLaneId
                    << " session=" << resizeValidationSession.sessionId
                    << " scenario=" << resizeValidationScenario()
                    << " result=" << (pass ? "PASS" : "FAIL")
                    << " samples=" << resizeValidationSession.sampleCount
                    << " durationMs=" << durationMs
                    << " animatedTree=" << (lastResizeSessionState.animatedTree ? "yes" : "no")
                    << " peakV=" << resizeValidationSession.peakVelocityPxPerSec
                    << " peakA=" << resizeValidationSession.peakAccelerationPxPerSec2
                    << " submittedDelta=" << submittedDelta
                    << " presentedDelta=" << presentedDelta
                    << " droppedDelta=" << droppedDelta
                    << " failedDelta=" << failedDelta
                    << " epochDropDelta=" << epochDropDelta
                    << " staleCoordDelta=" << staleCoordDelta
                    << " dropRatio="
                    << (submittedDelta == 0 ? 0.0 :
                        static_cast<double>(droppedDelta) / static_cast<double>(submittedDelta));
            if(!pass){
                summary << " reason=\"" << failureReason.str() << "\"";
            }
            OMEGAWTK_DEBUG(summary.str());
            if(!pass && validationTuning.failHard){
                assert(false && "Resize validation failed. See ResizeValidation log.");
            }
        }
        resizeValidationSession = {};
    }

    void WidgetTreeHost::attachToWindow(AppWindow * window){
        if(!attachedToWindow) {
            attachedToWindow = true;
            window->_add_widget(root.get());
            window->proxy.setFrontendPtr(compositor);
        }
    };

    void WidgetTreeHost::setRoot(WidgetPtr widget){
        if(root == widget){
            return;
        }
        if(root != nullptr && compositor != nullptr){
            unobserveWidgetLayerTreesRecurse(root.get());
        }
        root = widget;
        if(root != nullptr && compositor != nullptr){
            observeWidgetLayerTreesRecurse(root.get());
        }
    };
};
