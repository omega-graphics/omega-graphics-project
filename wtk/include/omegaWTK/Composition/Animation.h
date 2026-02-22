#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/MultiThreading.h"

#include "omegaWTK/Native/NativeItem.h"
#include "CompositorClient.h"

#include "Layer.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <type_traits>


#ifndef OMEGAWTK_COMPOSITION_ANIMATION_H
#define  OMEGAWTK_COMPOSITION_ANIMATION_H

namespace OmegaWTK {
    class View;
}
namespace OmegaWTK::Composition {
    struct CanvasFrame;
    namespace detail {
        class AnimationRuntimeRegistry;
    }

    /// @brief Traverse any 2D scalar.
    class OMEGAWTK_EXPORT ScalarTraverse {
        OmegaGTE::GPoint2D start_pt;
        OmegaGTE::GPoint2D end_pt;
        OmegaGTE::GPoint2D cur;
        float delta_x;
        float delta_y;
        unsigned speed;
    public:
        /// @brief Start a Traversal along the provided scalar.
        /// @param start Start Point of Scalar.
        /// @param end End Point of Scalar.
        /// @param speed The Speed of Traversal. (In units per step.)
        explicit ScalarTraverse(OmegaGTE::GPoint2D start,OmegaGTE::GPoint2D end,unsigned speed = 1);

        /// @brief Retrieve the current position of the traversal along the scalar.
        /// @returns A 2D Point.
        OmegaGTE::GPoint2D get();

        /// @brief Step forward in the curve by `speed` number of units.
        void forward();

        /// @brief Step backward in the curve by `speed` number of units.
        void back();

        /// @brief Check if current position in traversal is at start point.
        bool begin() const;

        /// @brief Check if current position in traversal is at end point.
        bool end() const;

        /// @brief Modify the scalar being traversed.
        /// @note Will only change scalar if current position intersects new scalar.
        /// @param start The New Start Point.
        /// @param end The New End Point.
        void changeScalar(OmegaGTE::GPoint2D start,OmegaGTE::GPoint2D end);
    };

    /// @brief Represents a generic mathematical linear or bezier curve used for animation.
    /// It is defined in 1 x 1 float-based coordinate space allowing it to be transposed to any 2D coordinate space.
    /// @paragraph There are three types of curves.
    /// \n - Linear -> A double point curve with a start and end point. (0,0 --- 1,1)
    /// \n - Quadratic -> A bezier curve with one control point in addition to a start and end.
    /// \n - Cubic -> A bezier with two additional control points.
    struct OMEGAWTK_EXPORT AnimationCurve {
        enum class Type : int {
            Linear,
            CubicBezier,
            QuadraticBezier
        } type;

        float start_h;
        float end_h;

        OmegaGTE::GPoint2D a = {0,0},b = {0,0};
    public:

        /// @brief Traversal of an AnimationCurve in a scaled integral 2D coordinate space.
        class Traversal {
            AnimationCurve & curve;
            void *data, *initState;

        public:
            OmegaGTE::GPoint2D get();
            void next();
            bool end();
            void reset();
            explicit Traversal(AnimationCurve & curve,float & space_w,float & space_h);
            ~Traversal();
        };
        /// @brief Create a TraversalContext using this AnimationCurve.
        /// @param st The Start Point.
        /// @param end The End Point.
        Traversal traverse(float space_w,float space_h);

        /// @brief Samples the curve at normalized time [0,1].
        /// @returns A normalized interpolant [0,1].
        float sample(float t) const;

        /// @brief Create a Linear AnimationCurve.
        /// @returns AnimationCurve
        static SharedHandle<AnimationCurve> Linear(float start_h,float end_h);
        static SharedHandle<AnimationCurve> Linear();
        static SharedHandle<AnimationCurve> EaseIn();
        static SharedHandle<AnimationCurve> EaseOut();
        static SharedHandle<AnimationCurve> EaseInOut();

        /// @brief Create a Quadratic Bezier AnimationCurve.
        /// @param a The 'A' control point used in the curve.
        /// @returns AnimationCurve
        static SharedHandle<AnimationCurve> Quadratic(OmegaGTE::GPoint2D a);

        /// @brief Create a Cubic Bezier AnimationCurve.
        /// @param a The 'A' control point used in the curve.
        /// @param b The 'B' control point used in the curve.
        /// @returns AnimationCurve
        static SharedHandle<AnimationCurve> Cubic(OmegaGTE::GPoint2D a,OmegaGTE::GPoint2D b);
        static SharedHandle<AnimationCurve> CubicBezier(OmegaGTE::GPoint2D a,
                                                        OmegaGTE::GPoint2D b,
                                                        float start_h = 0.f,
                                                        float end_h = 1.f);

    };

    /// @brief A generic keyframe-based animation timeline for any given duration of time.
    class OMEGAWTK_EXPORT AnimationTimeline {
    public:
        struct Keyframe;
    private:
        OmegaCommon::Vector<Keyframe> keyframes;
    public:
        /// @brief A Keyframe in a timeline.
        /// @paragraph The `time` field can ONLY be between 0 - 1.0. (0 to 100%).
        struct Keyframe {
            float time;
            SharedHandle<AnimationCurve> curve;

            SharedHandle<CanvasFrame> frame;
            SharedHandle<LayerEffect> effect;

            static Keyframe CanvasFrameStop(float time,SharedHandle<AnimationCurve> curve,SharedHandle<CanvasFrame> & frame);
            static Keyframe DropShadowStop(float time,SharedHandle<AnimationCurve> curve,LayerEffect::DropShadowParams & params);
            static Keyframe TransformationStop(float time,SharedHandle<AnimationCurve> curve,LayerEffect::TransformationParams & params);
        };

        static SharedHandle<AnimationTimeline> Create(const OmegaCommon::Vector<Keyframe> & keyframes);
    };

    using AnimationId = std::uint64_t;

    enum class AnimationState : std::uint8_t {
        Pending,
        Running,
        Paused,
        Completed,
        Cancelled,
        Failed
    };

    enum class FillMode : std::uint8_t {
        None,
        Forwards,
        Backwards,
        Both
    };

    enum class Direction : std::uint8_t {
        Normal,
        Reverse,
        Alternate,
        AlternateReverse
    };

    enum class ClockMode : std::uint8_t {
        WallClock,
        PresentedClock,
        Hybrid
    };

    struct TimingOptions {
        std::uint32_t durationMs = 300;
        std::uint32_t delayMs = 0;
        float playbackRate = 1.0f;
        float iterations = 1.0f;
        std::uint16_t frameRateHint = 60;
        FillMode fillMode = FillMode::Forwards;
        Direction direction = Direction::Normal;
        ClockMode clockMode = ClockMode::Hybrid;
        std::uint8_t maxCatchupSteps = 1;
        bool preferResizeSafeBudget = true;
    };

    class OMEGAWTK_EXPORT AnimationHandle {
        struct StateBlock;
        SharedHandle<StateBlock> stateBlock;
        explicit AnimationHandle(const SharedHandle<StateBlock> & stateBlock);
        friend class LayerAnimator;
        friend class ViewAnimator;
        friend class detail::AnimationRuntimeRegistry;
        void setStateInternal(AnimationState state);
        void setProgressInternal(float normalized);
        void setSubmittedPacketIdInternal(std::uint64_t packetId);
        void setPresentedPacketIdInternal(std::uint64_t packetId);
        void incrementDroppedPacketCountInternal();
        void setFailureReasonInternal(const OmegaCommon::String & reason);
    public:
        AnimationHandle();
        static AnimationHandle Create(AnimationId id,AnimationState initialState = AnimationState::Pending);
        AnimationId id() const;
        AnimationState state() const;
        float progress() const;
        float playbackRate() const;
        std::uint64_t lastSubmittedPacketId() const;
        std::uint64_t lastPresentedPacketId() const;
        std::uint32_t droppedPacketCount() const;
        Core::Optional<OmegaCommon::String> failureReason() const;
        bool valid() const;
        void pause();
        void resume();
        void cancel();
        void seek(float normalized);
        void setPlaybackRate(float rate);
    };

    template<typename T>
    struct KeyframeValue {
        float offset = 0.f;
        T value {};
        SharedHandle<AnimationCurve> easingToNext = nullptr;
    };

    namespace detail {
        inline float clamp01(float v){
            return std::max(0.f,std::min(1.f,v));
        }

        inline float lerp(float a,float b,float t){
            return a + ((b - a) * t);
        }

        template<typename T>
        struct KeyframeLerp {
            static T apply(const T & lhs,const T & rhs,float t){
                static_assert(std::is_arithmetic_v<T>, "Keyframe interpolation is not specialized for this type.");
                return static_cast<T>(lerp(static_cast<float>(lhs),static_cast<float>(rhs),t));
            }
        };

        template<>
        struct KeyframeLerp<float> {
            static float apply(const float & lhs,const float & rhs,float t){
                return lerp(lhs,rhs,t);
            }
        };

        template<>
        struct KeyframeLerp<Core::Rect> {
            static Core::Rect apply(const Core::Rect & lhs,const Core::Rect & rhs,float t){
                return Core::Rect{
                        Core::Position{
                                lerp(lhs.pos.x,rhs.pos.x,t),
                                lerp(lhs.pos.y,rhs.pos.y,t)},
                        lerp(lhs.w,rhs.w,t),
                        lerp(lhs.h,rhs.h,t)};
            }
        };

        template<>
        struct KeyframeLerp<LayerEffect::TransformationParams> {
            static LayerEffect::TransformationParams apply(const LayerEffect::TransformationParams & lhs,
                                                           const LayerEffect::TransformationParams & rhs,
                                                           float t){
                LayerEffect::TransformationParams out {};
                out.translate.x = lerp(lhs.translate.x,rhs.translate.x,t);
                out.translate.y = lerp(lhs.translate.y,rhs.translate.y,t);
                out.translate.z = lerp(lhs.translate.z,rhs.translate.z,t);

                out.rotate.pitch = lerp(lhs.rotate.pitch,rhs.rotate.pitch,t);
                out.rotate.yaw = lerp(lhs.rotate.yaw,rhs.rotate.yaw,t);
                out.rotate.roll = lerp(lhs.rotate.roll,rhs.rotate.roll,t);

                out.scale.x = lerp(lhs.scale.x,rhs.scale.x,t);
                out.scale.y = lerp(lhs.scale.y,rhs.scale.y,t);
                out.scale.z = lerp(lhs.scale.z,rhs.scale.z,t);
                return out;
            }
        };

        template<>
        struct KeyframeLerp<LayerEffect::DropShadowParams> {
            static LayerEffect::DropShadowParams apply(const LayerEffect::DropShadowParams & lhs,
                                                       const LayerEffect::DropShadowParams & rhs,
                                                       float t){
                LayerEffect::DropShadowParams out {};
                out.x_offset = lerp(lhs.x_offset,rhs.x_offset,t);
                out.y_offset = lerp(lhs.y_offset,rhs.y_offset,t);
                out.radius = lerp(lhs.radius,rhs.radius,t);
                out.blurAmount = lerp(lhs.blurAmount,rhs.blurAmount,t);
                out.opacity = lerp(lhs.opacity,rhs.opacity,t);
                out.color.r = lerp(lhs.color.r,rhs.color.r,t);
                out.color.g = lerp(lhs.color.g,rhs.color.g,t);
                out.color.b = lerp(lhs.color.b,rhs.color.b,t);
                out.color.a = lerp(lhs.color.a,rhs.color.a,t);
                return out;
            }
        };
    }

    template<typename T>
    class KeyframeTrack {
        OmegaCommon::Vector<KeyframeValue<T>> keys;
    public:
        static KeyframeTrack<T> From(const OmegaCommon::Vector<KeyframeValue<T>> & source){
            KeyframeTrack<T> track {};
            track.keys = source;
            if(track.keys.empty()){
                return track;
            }
            std::sort(track.keys.begin(),track.keys.end(),[](const KeyframeValue<T> & lhs,const KeyframeValue<T> & rhs){
                return lhs.offset < rhs.offset;
            });
            for(auto & key : track.keys){
                key.offset = detail::clamp01(key.offset);
            }
            return track;
        }

        bool empty() const{
            return keys.empty();
        }

        const OmegaCommon::Vector<KeyframeValue<T>> & keyframes() const{
            return keys;
        }

        T sample(float t) const{
            if(keys.empty()){
                return T{};
            }
            if(keys.size() == 1){
                return keys.front().value;
            }
            const float normalized = detail::clamp01(t);
            if(normalized <= keys.front().offset){
                return keys.front().value;
            }
            if(normalized >= keys.back().offset){
                return keys.back().value;
            }

            for(std::size_t i = 1; i < keys.size(); i++){
                const auto & prev = keys[i - 1];
                const auto & next = keys[i];
                if(normalized > next.offset){
                    continue;
                }
                const float span = std::max(next.offset - prev.offset,std::numeric_limits<float>::epsilon());
                const float local = detail::clamp01((normalized - prev.offset) / span);
                const float eased = prev.easingToNext ? prev.easingToNext->sample(local) : local;
                return detail::KeyframeLerp<T>::apply(prev.value,next.value,detail::clamp01(eased));
            }
            return keys.back().value;
        }
    };

    struct LayerClip {
        Core::Optional<KeyframeTrack<Core::Rect>> rect;
        Core::Optional<KeyframeTrack<LayerEffect::TransformationParams>> transform;
        Core::Optional<KeyframeTrack<LayerEffect::DropShadowParams>> shadow;
        Core::Optional<KeyframeTrack<float>> opacity;
    };

    struct ViewClip {
        Core::Optional<KeyframeTrack<Core::Rect>> rect;
        Core::Optional<KeyframeTrack<float>> opacity;
    };

    class ViewAnimator;

    class OMEGAWTK_EXPORT LayerAnimator : public CompositorClient {
        Layer & targetLayer;
        ViewAnimator &parentAnimator;
        friend class ViewAnimator;
        friend class detail::AnimationRuntimeRegistry;
        void queueLayerResizeDelta(int delta_x,int delta_y,int delta_w,int delta_h);
        explicit LayerAnimator(Layer & layer,ViewAnimator &parentAnimator);
    public:
        AnimationHandle animate(const LayerClip & clip,const TimingOptions & timing = {});
        AnimationHandle animateOnLane(const LayerClip & clip,
                                      const TimingOptions & timing,
                                      std::uint64_t syncLaneId);
        void setFrameRate(unsigned _framePerSec);
        void animate(const SharedHandle<AnimationTimeline> & timeline,unsigned duration);
        void pause();
        void resume();
        void resizeTransition(unsigned delta_x,unsigned delta_y,unsigned delta_w,unsigned delta_h,unsigned duration,
                              const SharedHandle<AnimationCurve> & curve = AnimationCurve::Linear(0.f,1.f));
        void applyShadow(const LayerEffect::DropShadowParams & params);
        void applyTransformation(const LayerEffect::TransformationParams & params);
        void shadowTransition(const LayerEffect::DropShadowParams & from,
                              const LayerEffect::DropShadowParams &to,
                              unsigned duration,
                              const SharedHandle<AnimationCurve> & curve = AnimationCurve::Linear(0.f,1.f));
        void transformationTransition(const LayerEffect::TransformationParams & from,
                                      const LayerEffect::TransformationParams &to,
                                      unsigned duration,
                                      const SharedHandle<AnimationCurve> & curve = AnimationCurve::Linear(0.f,1.f));
        void transition(SharedHandle<CanvasFrame> & from,
                        SharedHandle<CanvasFrame> & to,
                        unsigned duration,
                        const SharedHandle<AnimationCurve> & curve = AnimationCurve::Linear(0.f,1.f));
        ~LayerAnimator();
    };

    class OMEGAWTK_EXPORT ViewAnimator : public CompositorClient {
        OmegaCommon::Vector<SharedHandle<LayerAnimator>> layerAnims;

        CompositorClientProxy & _client;

        Native::NativeItemPtr nativeView;
        friend class ::OmegaWTK::View;
        friend class LayerAnimator;
        friend class detail::AnimationRuntimeRegistry;
        void queueViewResizeDelta(int delta_x,int delta_y,int delta_w,int delta_h);
        unsigned framePerSec;

        unsigned calculateTotalFrames(unsigned & duration);

    public:
        explicit ViewAnimator(CompositorClientProxy & _client);
        AnimationHandle animate(const ViewClip & clip,const TimingOptions & timing = {});
        AnimationHandle animateOnLane(const ViewClip & clip,
                                      const TimingOptions & timing,
                                      std::uint64_t syncLaneId);
        void setFrameRate(unsigned _framePerSec);
        void pause();
        void resume();
        SharedHandle<LayerAnimator> layerAnimator(Layer &layer);
        void resizeTransition(unsigned delta_x,unsigned delta_y,unsigned delta_w,unsigned delta_h,unsigned duration,
                    const SharedHandle<AnimationCurve> & curve);
        ~ViewAnimator();
    };
};

#endif
