# Animation API Evaluation: Mock Test + Usability Audit

## 1. Mock Animation Test

This test exercises every layer of the animation API to expose the developer experience end-to-end. It's written as pseudo-compilable code against the real headers — not a unit test, but a usability pressure test.

### Scenario

A card widget animates in three phases:
1. **Enter**: Fades in, slides up from below, shadow grows
2. **Hover**: Color transitions, shadow deepens, slight scale
3. **Exit**: Fades out, slides down, shadow shrinks

This exercises: StyleSheet transitions, KeyframeTrack, LayerClip, TimingOptions, AnimationHandle control, layout transitions, and the composition clock.

```cpp
#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/UIView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Composition/Canvas.h>
#include <omegaWTK/Composition/Animation.h>
#include <omegaWTK/Main.h>
#include <iostream>

using namespace OmegaWTK;
using namespace OmegaWTK::Composition;

// -----------------------------------------------------------------------
//  PHASE 1: Simple StyleSheet transition (the "easy" path)
// -----------------------------------------------------------------------

void phaseOneStyleSheetTransition(UIView *cardView) {
    // Goal: Fade background from transparent to blue over 400ms.
    //
    // This is the most basic animation the API offers.
    // It works, but notice:

    auto style = StyleSheet::Create();

    // Q: What curve does this use? The API doesn't say.
    // A: You have to read UIView.cpp to find out it's linear by default.
    //    There is NO way to pass a curve to a StyleSheet transition.
    style = style->backgroundColor(
        "card_view",
        Color::create8Bit(0x33, 0x66, 0xCC, 0xFF),
        true,     // transition = true
        0.4f      // duration (seconds? milliseconds? the type is `float`)
    );

    // Q: Can I chain multiple transitions on different properties?
    style = style->elementBrush(
        "card_rect",
        ColorBrush(Color::create8Bit(Color::Blue8)),
        true,
        0.4f
    );

    // Q: What if I want the brush to animate AFTER the background?
    // A: You can't. There's no delay parameter. Both start simultaneously.
    //    The only delay mechanism is TimingOptions.delayMs, which belongs
    //    to the LayerAnimator/ViewAnimator path — a completely different API.

    // Q: Can I animate border width from 0 to 2?
    style = style->borderWidth("card_view", 2.f, true, 0.3f);
    // Sure — but border() takes a bool, not transition params:
    style = style->border("card_view", true);
    // So the border snaps on instantly, then the width animates. Jarring.

    cardView->setStyleSheet(style);
    cardView->update();

    // VERDICT: Works for trivial cases. Falls apart the moment you want
    // sequencing, custom easing, or coordinated multi-property animation.
}


// -----------------------------------------------------------------------
//  PHASE 2: Element-level animation with curves
// -----------------------------------------------------------------------

void phaseTwoElementAnimation(UIView *cardView) {
    // Goal: Animate the card rect's alpha channel with an ease-out curve.
    //
    // Now we need to use the elementAnimation / elementBrushAnimation path.

    auto curve = AnimationCurve::EaseOut();
    auto style = StyleSheet::Create();

    // elementBrushAnimation — this animates a brush property by key.
    style = style->elementBrushAnimation(
        ColorBrush(Color::create8Bit(0x33, 0x66, 0xCC, 0xFF)),
        ElementAnimationKeyColorAlpha,  // animate the alpha channel
        curve,
        0.5f                            // duration (seconds)
    );

    // PROBLEM: Where's the element tag?
    // elementBrushAnimation doesn't take an elementTag parameter.
    // Compare with elementAnimation which does:
    style = style->elementAnimation(
        "card_rect",
        ElementAnimationKeyWidth,
        curve,
        0.5f
    );

    // So elementBrushAnimation applies to... what element? The last one?
    // All of them? You have to read the source to find out. The signature
    // itself is ambiguous.

    // PROBLEM: elementAnimation animates a single key (width OR height OR
    // colorRed OR colorAlpha). To animate width AND height together, you
    // need TWO entries. But will they be synchronized? Will they use the
    // same composition clock? The API doesn't guarantee it.

    style = style->elementAnimation("card_rect", ElementAnimationKeyWidth, curve, 0.5f);
    style = style->elementAnimation("card_rect", ElementAnimationKeyHeight, curve, 0.5f);

    // PROBLEM: What are the "from" and "to" values?
    // elementAnimation doesn't take from/to. It relies on UIView's internal
    // startOrUpdateAnimation() to infer the "from" from the current rendered
    // state. So the animation target is... the current stylesheet value?
    // The previous one? It's implicit and invisible to the caller.

    cardView->setStyleSheet(style);
    cardView->update();
}


// -----------------------------------------------------------------------
//  PHASE 3: Drop shadow transition via StyleSheet
// -----------------------------------------------------------------------

void phaseThreeShadowTransition(UIView *cardView) {
    // Goal: Animate shadow from subtle to prominent.

    LayerEffect::DropShadowParams subtleShadow {};
    subtleShadow.x_offset = 0.f;
    subtleShadow.y_offset = 2.f;
    subtleShadow.radius = 2.f;
    subtleShadow.blurAmount = 4.f;
    subtleShadow.opacity = 0.3f;
    subtleShadow.color = Color::create8Bit(Color::Black8);

    LayerEffect::DropShadowParams prominentShadow {};
    prominentShadow.x_offset = 0.f;
    prominentShadow.y_offset = 8.f;
    prominentShadow.radius = 4.f;
    prominentShadow.blurAmount = 16.f;
    prominentShadow.opacity = 0.6f;
    prominentShadow.color = Color::create8Bit(Color::Black8);

    // Via StyleSheet:
    auto style = StyleSheet::Create();
    style = style->elementDropShadow("card_rect", prominentShadow, true, 0.4f);

    // Q: Does this transition FROM the current shadow TO prominentShadow?
    // A: Yes — UIView tracks the last resolved effect state and interpolates.
    //    But this only works if you previously SET a shadow. If the element
    //    had no shadow before, what's the "from"? Zero shadow? The API
    //    doesn't document this.

    // Q: Can I control the easing of this shadow transition?
    // A: No. StyleSheet transitions have no curve parameter. Linear only.

    cardView->setStyleSheet(style);
    cardView->update();

    // ALTERNATIVE: Use LayerAnimator for shadow with custom easing.
    // But this requires a completely different code path — see Phase 5.
}


// -----------------------------------------------------------------------
//  PHASE 4: Keyframe-based layer animation
// -----------------------------------------------------------------------

void phaseFourKeyframeAnimation(UIView *cardView, View *parentView) {
    // Goal: Multi-keyframe opacity animation with different easing per segment.
    //
    // Now we leave StyleSheet land entirely and enter the LayerAnimator world.

    // Step 1: Get a ViewAnimator. But how?
    // ViewAnimator's constructor takes a CompositorClientProxy&.
    // That's an internal type. You can't construct one from user code.
    //
    // The actual way to get one is... unclear from the headers.
    // ViewAnimator is constructed internally by View.
    // There's no View::getAnimator() or View::animator() method exposed.
    //
    // Let's assume we somehow have one (through a UIView internal):
    // SharedHandle<ViewAnimator> viewAnim = ???;

    // Step 2: Get a LayerAnimator for a specific layer.
    // SharedHandle<LayerAnimator> layerAnim = viewAnim->layerAnimator(*someLayer);
    //
    // But which layer? UIView creates layers internally per-element in
    // UIRenderer::buildLayerRenderTarget(). There's no public API to get
    // "the layer for element 'card_rect'".

    // Step 3: Build keyframes (assuming we have the animator).
    auto ease = AnimationCurve::EaseInOut();

    OmegaCommon::Vector<KeyframeValue<float>> opacityKeys;
    opacityKeys.push_back({0.0f, 0.0f, AnimationCurve::EaseOut()});   // start invisible
    opacityKeys.push_back({0.3f, 1.0f, AnimationCurve::Linear()});    // fade in by 30%
    opacityKeys.push_back({0.7f, 1.0f, AnimationCurve::EaseIn()});    // hold at full
    opacityKeys.push_back({1.0f, 0.0f, nullptr});                     // fade out

    auto opacityTrack = KeyframeTrack<float>::From(opacityKeys);

    // Build a LayerClip
    LayerClip clip {};
    clip.opacity = opacityTrack;

    // Timing
    TimingOptions timing {};
    timing.durationMs = 2000;
    timing.fillMode = FillMode::Forwards;     // Hold final value
    timing.direction = Direction::Normal;
    timing.clockMode = ClockMode::Hybrid;     // Blend wall/presented clock
    timing.iterations = 1.0f;
    timing.playbackRate = 1.0f;

    // Fire it:
    // AnimationHandle handle = layerAnim->animate(clip, timing);

    // Step 4: Track and control:
    // if (handle.valid()) {
    //     auto state = handle.state();        // Pending/Running/Completed...
    //     auto progress = handle.progress();  // 0.0 - 1.0
    //     handle.pause();
    //     handle.resume();
    //     handle.seek(0.5f);                  // Jump to midpoint
    //     handle.setPlaybackRate(2.0f);       // Double speed
    //     handle.cancel();
    // }

    // VERDICT: The KeyframeTrack + LayerClip + TimingOptions system is
    // actually well-designed. It's a proper animation primitive.
    // The problem is ACCESS: you can't reach it from user code without
    // going through internal plumbing.
}


// -----------------------------------------------------------------------
//  PHASE 5: Shadow transition via LayerAnimator
// -----------------------------------------------------------------------

void phaseFiveShadowViaAnimator(/* LayerAnimator *layerAnim */) {
    // The LayerAnimator has direct shadow transition methods:
    LayerEffect::DropShadowParams from {};
    from.y_offset = 2.f;
    from.blurAmount = 4.f;
    from.opacity = 0.3f;
    from.color = Color::create8Bit(Color::Black8);

    LayerEffect::DropShadowParams to {};
    to.y_offset = 8.f;
    to.blurAmount = 16.f;
    to.opacity = 0.6f;
    to.color = Color::create8Bit(Color::Black8);

    auto curve = AnimationCurve::EaseOut();

    // layerAnim->shadowTransition(from, to, 400, curve);
    //                                        ^^^ duration in ms (unsigned)

    // PROBLEM: StyleSheet uses float seconds (0.4f).
    //          LayerAnimator uses unsigned milliseconds (400).
    //          TimingOptions uses uint32 milliseconds (300).
    //          There are THREE different duration conventions in one API.

    // ALSO: shadowTransition() takes from/to explicitly.
    //       StyleSheet transitions infer from/to implicitly.
    //       These are two mental models for the same operation.
}


// -----------------------------------------------------------------------
//  PHASE 6: 3D Transform animation
// -----------------------------------------------------------------------

void phaseSixTransformAnimation() {
    // Build keyframes for a 3D flip effect:
    OmegaCommon::Vector<KeyframeValue<LayerEffect::TransformationParams>> transformKeys;

    LayerEffect::TransformationParams identity {};
    identity.translate = {0, 0, 0};
    identity.rotate = {0, 0, 0};
    identity.scale = {1, 1, 1};

    LayerEffect::TransformationParams flipped {};
    flipped.translate = {0, 0, 50};
    flipped.rotate = {0, 180, 0};   // 180 degree Y rotation
    flipped.scale = {1, 1, 1};

    transformKeys.push_back({0.0f, identity, AnimationCurve::EaseInOut()});
    transformKeys.push_back({0.5f, flipped, AnimationCurve::EaseInOut()});
    transformKeys.push_back({1.0f, identity, nullptr});

    auto transformTrack = KeyframeTrack<LayerEffect::TransformationParams>::From(transformKeys);

    LayerClip clip {};
    clip.transform = transformTrack;

    TimingOptions timing {};
    timing.durationMs = 800;
    timing.direction = Direction::Alternate;  // Ping-pong
    timing.iterations = 3.0f;                 // 3 cycles
    timing.fillMode = FillMode::Both;

    // This is clean. The data model works well for this case.
    // The issue is still: how do you get a LayerAnimator to call animate() on?
}


// -----------------------------------------------------------------------
//  PHASE 7: Layout transition
// -----------------------------------------------------------------------

void phaseSevenLayoutTransition(UIView *cardView) {
    // Goal: Animate an element's width and height when layout changes.

    LayoutTransitionSpec spec {};
    spec.enabled = true;
    spec.durationSec = 0.3f;                    // seconds (yet another unit)
    spec.curve = AnimationCurve::EaseInOut();
    spec.properties = {
        LayoutTransitionProperty::Width,
        LayoutTransitionProperty::Height
    };

    auto style = StyleSheet::Create();
    style = style->layoutTransition("card_rect", spec);
    style = style->layoutWidth("card_rect", LayoutLength::Px(300));
    style = style->layoutHeight("card_rect", LayoutLength::Px(200));

    // Q: Does setting layoutWidth + layoutTransition in the SAME stylesheet
    //    cause the transition to fire? Or does the transition only fire
    //    on subsequent layout changes?
    // A: You have to read UIView.cpp to know. The answer depends on whether
    //    UIView detects a delta between the old and new resolved rect.
    //    If this is the first frame, there's no "old" — so no transition.

    cardView->setStyleSheet(style);
    cardView->update();

    // To actually trigger the transition, you'd set a NEW size later:
    auto style2 = StyleSheet::Create();
    style2 = style2->layoutTransition("card_rect", spec);
    style2 = style2->layoutWidth("card_rect", LayoutLength::Px(400));
    cardView->setStyleSheet(style2);
    cardView->update();

    // PROBLEM: You have to re-specify the layoutTransition every time
    // because StyleSheet is immutable-ish (method chaining returns new ptr).
    // There's no way to say "this element always transitions on layout change."
}


// -----------------------------------------------------------------------
//  PHASE 8: Coordinated multi-element animation via sync lanes
// -----------------------------------------------------------------------

void phaseEightSyncLanes() {
    // Goal: Two layers animate together, synchronized to the same
    // compositor lane so they never drift apart.

    // LayerAnimator has animateOnLane(clip, timing, syncLaneId).
    // The syncLaneId is a uint64_t — you pick it yourself.

    // But: how do you coordinate a STYLESHEET animation with a LAYER animation?
    // You can't. The StyleSheet transition system and the LayerAnimator system
    // are completely independent. If you want a background color to transition
    // in sync with a shadow animation, you either:
    //   a) Do both through LayerAnimator (but you can't animate color via LayerAnimator)
    //   b) Do both through StyleSheet (but you can't control easing or sync)
    //   c) Accept they'll be approximately synchronized by wall clock

    // VERDICT: Sync lanes are powerful for layer-to-layer coordination,
    // but useless for the most common case: coordinating visual property
    // changes across the two animation systems.
}


// -----------------------------------------------------------------------
//  PHASE 9: AnimationTimeline (the "complex sequence" path)
// -----------------------------------------------------------------------

void phaseNineTimeline() {
    // AnimationTimeline lets you define multi-stop sequences with
    // CanvasFrames, drop shadows, and transforms at specific time offsets.

    // But CanvasFrame is a SharedHandle<CanvasFrame> — an opaque snapshot
    // of rendered content. You'd need to pre-render frames to create stops:

    // SharedHandle<CanvasFrame> frame1 = ...; // How do you get one?
    // SharedHandle<CanvasFrame> frame2 = ...;

    // auto timeline = AnimationTimeline::Create({
    //     AnimationTimeline::Keyframe::CanvasFrameStop(0.0f, AnimationCurve::Linear(), frame1),
    //     AnimationTimeline::Keyframe::CanvasFrameStop(1.0f, AnimationCurve::EaseOut(), frame2),
    // });
    //
    // layerAnim->animate(timeline, 1000);

    // PROBLEM: CanvasFrame capture is not exposed in any public API.
    // You'd need to manually construct one from Canvas render commands.
    // This makes AnimationTimeline effectively unusable for cross-fades.

    // The DropShadowStop and TransformationStop variants are more practical:
    LayerEffect::DropShadowParams shadow1 {};
    shadow1.opacity = 0.2f;
    shadow1.blurAmount = 4.f;

    LayerEffect::DropShadowParams shadow2 {};
    shadow2.opacity = 0.8f;
    shadow2.blurAmount = 20.f;

    // auto timeline = AnimationTimeline::Create({
    //     AnimationTimeline::Keyframe::DropShadowStop(0.0f, AnimationCurve::EaseIn(), shadow1),
    //     AnimationTimeline::Keyframe::DropShadowStop(0.5f, AnimationCurve::EaseOut(), shadow2),
    //     AnimationTimeline::Keyframe::DropShadowStop(1.0f, AnimationCurve::EaseIn(), shadow1),
    // });

    // But wait — KeyframeTrack<DropShadowParams> already does exactly this,
    // with a cleaner API. So what's AnimationTimeline for? When would you
    // choose it over KeyframeTrack?
    //
    // Answer: AnimationTimeline can mix CanvasFrame and effect keyframes
    // in the same sequence. KeyframeTrack is typed to one value type.
    // But since CanvasFrame capture isn't accessible... AnimationTimeline
    // is a dead API path.
}


// -----------------------------------------------------------------------
//  PHASE 10: Path node animation
// -----------------------------------------------------------------------

void phaseTenPathAnimation(UIView *cardView) {
    // Goal: Animate a specific control point of a vector path.

    auto curve = AnimationCurve::EaseInOut();
    auto style = StyleSheet::Create();

    // Animate node 2's X coordinate:
    style = style->elementPathAnimation("wave_path", curve, 2, 0.8f);

    // PROBLEM: What does "animate node 2" actually do?
    // - From what value to what value?
    // - In which direction?
    // - How much displacement?
    // None of this is in the signature. The animation target is inferred
    // internally. The caller has zero control over the destination.

    // PROBLEM: You can only animate X or Y per call (the key is picked
    // internally based on... what exactly?). To animate both X and Y
    // of the same node, you'd need... there's no API for that through
    // elementPathAnimation. You'd need two separate elementAnimation calls
    // with ElementAnimationKeyPathNodeX and ElementAnimationKeyPathNodeY,
    // but elementAnimation doesn't take a nodeIndex parameter.
    // elementPathAnimation does take a nodeIndex but doesn't let you
    // pick X vs Y.

    // This is a dead end.
    cardView->setStyleSheet(style);
    cardView->update();
}
```

---

## 2. API Usability Verdict

### What works well

**KeyframeTrack + LayerClip + TimingOptions** is a solid animation primitive. The data model is clean:
- Keyframes with per-segment easing
- Typed interpolation (float, Rect, Transform, Shadow)
- Rich timing: duration, delay, fill mode, direction, iterations, playback rate
- Clock modes (wall, presented, hybrid) for compositor-aware timing
- AnimationHandle with pause/resume/seek/cancel

**AnimationCurve** has good factory methods: `Linear()`, `EaseIn()`, `EaseOut()`, `EaseInOut()`, `CubicBezier()`.

**AnimationHandle** is well-designed for tracking and controlling running animations.

### What's broken

#### Two incompatible animation systems with no bridge

The API has two completely separate animation paths:

| | StyleSheet Path | LayerAnimator Path |
|---|---|---|
| **Easing** | None (linear only) | Full AnimationCurve support |
| **Delay** | None | TimingOptions.delayMs |
| **Sequencing** | None | Via iterations + direction |
| **Duration unit** | float (seconds? unclear) | uint32 milliseconds |
| **From/To** | Implicit (inferred internally) | Explicit or via KeyframeTrack |
| **Sync lanes** | Not available | animateOnLane() |
| **User access** | Public (StyleSheet + UIView) | Effectively internal |
| **Target** | String element tags | Layer references |

A developer who starts with the simple path (StyleSheet) hits a wall the moment they need easing, delay, or sequencing. Moving to the powerful path (LayerAnimator) requires understanding compositor internals and layer ownership — and there's no public bridge between them.

#### Duration unit chaos

- `StyleSheet::elementBrush(..., float duration)` — seconds? milliseconds? Unlabeled.
- `TimingOptions::durationMs` — uint32, explicitly milliseconds
- `LayoutTransitionSpec::durationSec` — float, explicitly seconds
- `LayerAnimator::shadowTransition(..., unsigned duration, ...)` — unsigned, unit undocumented

Three different types, three different naming conventions, two different units.

#### No public path from UIView elements to LayerAnimator

UIView creates layers internally per-element. There's no `UIView::layerForElement(tag)` or `UIView::animatorForElement(tag)`. If you want to run a KeyframeTrack animation on an element's layer, you have to either:
- Dig through UIView internals (the `animationLayerAnimators` map is private)
- Abandon UIView and manage layers yourself (defeating the purpose of UIView)

#### elementBrushAnimation is missing its tag parameter

```cpp
StyleSheetPtr elementBrushAnimation(SharedHandle<Brush> brush,
                                    ElementAnimationKey key,
                                    SharedHandle<AnimationCurve> curve,
                                    float duration);
```

Compare with:
```cpp
StyleSheetPtr elementAnimation(UIElementTag elementTag,
                               ElementAnimationKey key,
                               SharedHandle<AnimationCurve> curve,
                               float duration);
```

One takes a tag, the other doesn't. This looks like a signature that was copied and modified without updating the parameter list.

#### AnimationTimeline is a dead API

The only use case that justifies AnimationTimeline over KeyframeTrack is cross-fading between CanvasFrames. But CanvasFrame capture isn't exposed publicly. DropShadowStop and TransformationStop duplicate what KeyframeTrack already does with a cleaner interface.

#### Path animation is underspecified

`elementPathAnimation` takes a node index and curve but not from/to values, not an X/Y selector, and not a displacement amount. The developer has no control over what the animation actually does.

#### StyleSheet transitions can't be customized

`bool transition, float duration` — that's the entire transition API per property. No curve. No delay. No fill mode. This means every StyleSheet transition is linear and immediate, which is visually poor for anything beyond the simplest fade.

---

## 3. Recommendations

### Short-term fixes (API consistency)

1. **Add `UIElementTag` to `elementBrushAnimation`** — align with `elementAnimation`.
2. **Standardize duration units** — pick one: `float seconds` everywhere, or `uint32_t milliseconds` everywhere. Name parameters explicitly (`durationSec` or `durationMs`).
3. **Add curve parameter to StyleSheet transitions** — change the signature from `(bool transition, float duration)` to `(bool transition, float durationSec, SharedHandle<AnimationCurve> curve = nullptr)`.

### Medium-term (bridge the two systems)

4. **Expose `UIView::animatorForElement(tag)` -> SharedHandle<LayerAnimator>** — let users drop into the powerful animation path from UIView without abandoning the element tag model.
5. **Add delay to StyleSheet transitions** — `(bool transition, float durationSec, float delaySec = 0.f, SharedHandle<AnimationCurve> curve = nullptr)`.

### Longer-term (unified animation model)

6. Consider a single `Transition` struct that both StyleSheet entries and LayerAnimator can use:
```cpp
struct TransitionSpec {
    float durationSec = 0.f;
    float delaySec = 0.f;
    SharedHandle<AnimationCurve> curve = nullptr;
    FillMode fillMode = FillMode::Forwards;
};
```
7. Deprecate or remove `AnimationTimeline` — `KeyframeTrack<T>` covers the same ground with a better interface.
8. Remove or redesign `elementPathAnimation` — it's unusable in its current form.

---

## 4. Summary

The **low-level animation engine** (KeyframeTrack, LayerClip, TimingOptions, AnimationHandle, sync lanes, clock modes) is well-architected. It handles real-world complexity: compositor backpressure, resize-aware budgeting, multi-track synchronization. This is production-grade infrastructure.

The **user-facing animation API** (StyleSheet transitions, UIView) is a thin, inconsistent wrapper over that engine. It exposes the simplest possible interaction (toggle a bool, set a duration) but provides no path to the features that make the engine valuable (easing, sequencing, synchronization, explicit from/to control). The two systems coexist without interoperating.

The result: a developer who needs anything beyond a linear fade has to abandon the ergonomic API and operate at the compositor level — but the compositor level isn't publicly accessible through UIView.

**The engine is good. The interface needs work.**
