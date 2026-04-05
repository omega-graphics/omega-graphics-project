# Layout API Current-Use Evaluation

## Goal

Evaluate `wtk/include/omegaWTK/UI/Layout.h` from the perspective of a real OmegaWTK user today:

- what parts are actually being used
- what parts are realistically useful to downstream code
- what parts are public but not yet paying for their complexity

This document is based on the current headers, current implementation, and current tests in this repository.

## Audit Scope

Reviewed:

- `wtk/include/omegaWTK/UI/Layout.h`
- `wtk/src/UI/Layout.cpp`
- `wtk/include/omegaWTK/UI/Widget.h`
- `wtk/src/UI/Widget.cpp`
- `wtk/include/omegaWTK/UI/View.h`
- `wtk/include/omegaWTK/UI/UIView.h`
- `wtk/src/UI/UIView.cpp`
- `wtk/src/Widgets/Containers.cpp`
- `wtk/tests/LayoutUnitTest/main.cpp`
- `wtk/tests/LayoutResizeStressTest/main.cpp`
- `wtk/tests/TextCompositorTest/main.cpp`
- `wtk/tests/ContainerClampAnimationTest/main.cpp`

## Short Conclusion

`Layout.h` currently mixes three different things:

1. genuinely useful low-level sizing primitives
2. future-facing layout API surface that is not yet wired deeply enough to help users
3. internal stylesheet/layout bridge types that should not need to be public for app code

The useful part today is the small, deterministic resolver layer:

- `LayoutLength`
- `LayoutEdges`
- `LayoutClamp`
- `LayoutContext`
- `resolveLength(...)`
- `clampValue(...)`
- `resolveClampedRect(...)`
- `computeLayoutDelta(...)`

The rest of the file is mostly ahead of actual user adoption or only meaningful to framework internals.

## What Users Actually Do Today

Looking at the current tests and examples, users are not really using `Layout.h` directly for widget layout.

The current user-facing patterns are mostly:

- `Container`, `HStack`, and `VStack`-style widget composition
- direct geometry via `setRect(...)` / `requestRect(...)`
- `UIViewLayout` plus `StyleSheet` for text/shape composition inside a `UIView`

The typical current pattern looks more like this:

```cpp
OmegaWTK::UIViewLayout layout {};
layout.shape("accent_rect",OmegaWTK::Shape::Rect(redRect));
layout.text("accent_label",U"UI",textRect);
accentView->setLayout(layout);

auto style = OmegaWTK::StyleSheet::Create();
style = style->backgroundColor("text_accent_view",OmegaWTK::Composition::Color::Transparent);
style = style->elementBrush("accent_rect",brush,true,0.30f);
style = style->textColor("accent_label",textColor,true,0.30f);
accentView->setStyleSheet(style);
accentView->update();
```

That is a real current usage style.

By contrast, direct widget-level layout API adoption is effectively absent right now.

## Usage Snapshot

From a repository-wide search of current call sites:

- `Widget::setLayoutStyle(...)`: `0` call sites outside its own declaration/definition
- `Widget::setLayoutBehavior(...)`: `0` call sites outside its own declaration/definition
- `UIViewLayoutV2` / `layoutV2()` / `setLayoutV2(...)`: `0` call sites outside `UIView` itself
- flex/grid/alignment fields from `LayoutStyle`: `0` references outside `Layout.h` itself

What *is* used:

- low-level resolver helpers in `LayoutUnitTest` and `LayoutResizeStressTest`
- `LayoutTransitionSpec` indirectly through `StyleSheet::layoutTransition(...)` in docs and internal `UIView` flow
- `ViewResizeCoordinator::clampRectToParent(...)` from `Containers.cpp`, but that lives in `View.h`, not `Layout.h`

## API Surface Evaluation

| Surface | Current usefulness | Notes |
| --- | --- | --- |
| `LayoutLength`, `LayoutEdges`, `LayoutClamp` | High | Small, understandable, deterministic. Good public value types. |
| `LayoutContext` | Medium | Useful for engine code and tests. Probably not something most app authors touch directly. |
| `resolveLength`, `clampValue`, `resolveClampedRect` | High | These are the clearest, most testable parts of the API today. |
| `LayoutStyle` | Medium-low | Useful as a data bag, but only a subset of fields currently affect behavior. |
| `LayoutDisplay`, `FlexDirection`, `FlexWrap`, `justifyContent`, `alignItems`, `alignSelfMain`, `alignSelfCross`, `gap` | Low | Publicly exposed, but effectively inert in current runtime behavior. |
| `LayoutBehavior` | Low | Public hook, but not realistically usable by downstream code today. |
| `LegacyResizeCoordinatorBehavior`, `StackLayoutBehavior` | Low | Public concrete types with no external adoption and limited implementation value today. |
| `LayoutTransitionSpec`, `LayoutDelta`, `computeLayoutDelta` | Medium | Reasonable building blocks, but mostly useful inside `UIView` transition plumbing. |
| `LayoutDiagnosticSink`, `VectorDiagnosticSink` | Low-medium | Useful for engine debugging and tests; not a common app-level API. |
| `StyleRule` + stylesheet bridge helpers | Low | These are framework internals exposed in a public header. |

## What In `LayoutStyle` Actually Works Today

The current `resolveClampedRect(...)` implementation meaningfully handles:

- `width`
- `height`
- `clamp`
- `aspectRatio`
- `margin.left`
- `margin.top`
- `position == Absolute`
- `insetLeft`
- `insetTop`

The following `LayoutStyle` fields are currently declared but not meaningfully resolved in the current simple layout path:

- `display`
- `padding`
- `margin.right`
- `margin.bottom`
- `insetRight`
- `insetBottom`
- `gap`
- `alignSelfMain`
- `alignSelfCross`
- `flexGrow`
- `flexShrink`
- `flexDirection`
- `flexWrap`
- `justifyContent`
- `alignItems`

That means `LayoutStyle` currently presents a much richer model than the runtime actually honors.

## Important Mismatches In The Public API

### 1. `LayoutBehavior` is public, but not really usable

`LayoutBehavior` looks like an extension point:

```cpp
class LayoutBehavior {
public:
    virtual MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) = 0;
    virtual void arrange(LayoutNode & node,const LayoutContext & ctx) = 0;
};
```

But `LayoutNode` is only forward-declared in public headers. There is no public `LayoutNode` definition to program against.

So a downstream user can technically derive from `LayoutBehavior`, but cannot meaningfully inspect or mutate the node tree through the public surface.

### 2. Custom `LayoutBehavior` is not actually invoked in the widget layout driver

Current `runWidgetLayout(...)` contains this flow:

- if the widget has no explicit layout style, fall back to `ViewResizeCoordinator`
- if the widget has a custom behavior, return immediately
- otherwise resolve a single `LayoutStyle` rect and commit it

That means `layoutBehavior()` is currently stored but not actually used to perform measure/arrange work.

From a user perspective, this makes `setLayoutBehavior(...)` look available but functionally unfinished.

### 3. `requestLayout()` does not currently provide a satisfying user story

`Widget::requestLayout()` currently just forwards upward to `parent->requestLayout()` and stops at the root.

That means:

- calling `requestLayout()` on a root widget does not itself run layout
- calling `setLayoutStyle(...)` does not automatically trigger a layout pass
- the main visible layout path today is still tied to resize handling and internal update flow

This makes the direct widget layout API feel passive rather than actionable.

### 4. `LayoutStyle` exposes a future flex/grid model that is not yet active

`LayoutDisplay::Flex`, `LayoutDisplay::Grid`, `flexDirection`, `flexWrap`, `justifyContent`, `alignItems`, and related alignment fields all suggest a substantial layout engine.

Today, there is no evidence of actual runtime use outside declarations. So this portion of the surface reads more like a design target than a mature public API.

### 5. `StyleRule` and related helpers are internal concepts in a public header

`Layout.h` itself labels this section as:

- `Internal StyleRule structure`

That label is honest. The mismatch is that the type is still public and installed.

`StyleRule`, `convertEntriesToRules(...)`, `mergeLayoutRulesIntoStyle(...)`, and `resolveLayoutTransition(...)` are useful for framework plumbing and testing, but they are not the layout API most users should build against.

## Mock Current-User Layout Test

The following test is intentionally written the way a reasonable user might try to use the current widget layout API after reading `Layout.h`.

It is not trying to be ideal. It is trying to be honest about the current developer experience.

```cpp
#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/Widgets/BasicWidgets.h>

using namespace OmegaWTK;

class ColorBlock final : public Widget {
public:
    explicit ColorBlock(Core::Rect rect) : Widget(rect) {}
};

static void mockCurrentUserLayoutTest() {
    auto root = make<Container>(Core::Rect{{0,0},800,600});
    auto header = make<ColorBlock>(Core::Rect{{0,0},1,1});
    auto content = make<ColorBlock>(Core::Rect{{0,0},1,1});

    LayoutStyle rootStyle {};
    rootStyle.display = LayoutDisplay::Flex;
    rootStyle.flexDirection = FlexDirection::Column;
    rootStyle.gap = LayoutLength::Dp(12.f);
    rootStyle.padding = LayoutEdges::All(LayoutLength::Dp(24.f));
    rootStyle.width = LayoutLength::Percent(1.0f);
    rootStyle.height = LayoutLength::Percent(1.0f);
    root->setLayoutStyle(rootStyle);

    LayoutStyle headerStyle {};
    headerStyle.width = LayoutLength::Percent(1.0f);
    headerStyle.height = LayoutLength::Dp(72.f);
    headerStyle.margin = LayoutEdges::Symmetric(LayoutLength::Dp(0.f),LayoutLength::Dp(8.f));
    header->setLayoutStyle(headerStyle);

    LayoutStyle contentStyle {};
    contentStyle.width = LayoutLength::Percent(1.0f);
    contentStyle.height = LayoutLength::Auto();
    contentStyle.flexGrow = 1.f;
    contentStyle.alignSelfCross = LayoutAlign::Stretch;
    content->setLayoutStyle(contentStyle);

    root->addChild(header);
    root->addChild(content);

    root->requestLayout();
}
```

### What a user would reasonably expect

- `root` lays out children in a vertical flex column
- `gap` inserts spacing between `header` and `content`
- `padding` shrinks the content area inward
- `header` gets full width and a fixed height
- `content` grows to fill remaining space because `flexGrow = 1`
- `requestLayout()` triggers layout immediately

### What the current implementation actually provides

- `setLayoutStyle(...)` stores the style, but there are currently no external call sites using it
- `requestLayout()` does not itself run a root layout pass
- `display = Flex` is not wired into the current simple resolver
- `flexDirection`, `gap`, `padding`, `flexGrow`, and `alignSelfCross` are not meaningfully consumed in the current widget layout path
- the simple resolver only knows how to resolve a single box rect from a subset of `LayoutStyle`

### What parts of the mock test are actually useful today

- `LayoutLength::Dp(...)`
- `LayoutLength::Percent(...)`
- `LayoutLength::Auto()`
- `LayoutEdges::All(...)`
- `LayoutEdges::Symmetric(...)`
- `LayoutStyle` as a compact container for width/height/clamp-ish data

### What parts are mostly aspirational today

- flex layout semantics
- padding-driven child layout
- gap-driven child placement
- grow/shrink alignment behavior
- root-driven `requestLayout()` as a complete layout trigger

## A More Realistic Current-Usage Mock

If the goal is to model how users are actually composing UI in this codebase today, a more representative mock test is this:

```cpp
#include <omegaWTK/UI/UIView.h>

using namespace OmegaWTK;

static void mockCurrentUIViewUsage(UIView *view) {
    UIViewLayout layout {};
    layout.shape("card_rect",Shape::RoundedRect(
        Core::RoundedRect{Core::Position{0.f,0.f},280.f,160.f,16.f,16.f}));
    layout.text("card_title",U"Layout Audit",
                Core::Rect{Core::Position{24.f,24.f},232.f,24.f});
    view->setLayout(layout);

    auto style = StyleSheet::Create();
    style = style->backgroundColor("card_view",Composition::Color::Transparent);
    style = style->elementBrush("card_rect",
                                Composition::ColorBrush(Composition::Color::create8Bit(
                                    Composition::Color::White8)));
    style = style->textColor("card_title",
                             Composition::Color::create8Bit(Composition::Color::Black8));
    view->setStyleSheet(style);
    view->update();
}
```

This is closer to the current reality:

- geometry is often explicit
- `UIViewLayout` does the element placement
- `StyleSheet` handles appearance
- `Layout.h` matters mostly indirectly through value types and transition wiring

## Useful vs. Not Yet Paying For It

### Clearly useful today

- keep `LayoutLength`
- keep `LayoutEdges`
- keep `LayoutClamp`
- keep `LayoutContext`
- keep `MeasureResult`
- keep `resolveLength(...)`
- keep `clampValue(...)`
- keep `resolveClampedRect(...)`
- keep `LayoutDelta` and `computeLayoutDelta(...)`

### Useful, but only as framework-facing API

- `LayoutTransitionSpec`
- `LayoutDiagnosticSink`
- `VectorDiagnosticSink`

### Public today, but not yet earning the complexity cost

- `LayoutDisplay`
- `LayoutPositionMode` except for the currently working `Absolute` path
- `LayoutAlign`
- `FlexDirection`
- `FlexWrap`
- the full richer half of `LayoutStyle`
- `LayoutBehavior`
- `LegacyResizeCoordinatorBehavior`
- `StackLayoutBehavior`
- `StyleRule`
- `convertEntriesToRules(...)`
- `mergeLayoutRulesIntoStyle(...)`
- `resolveLayoutTransition(...)`

## Recommendation

If the question is “how much of `Layout.h` is actually useful to users right now?”, the answer is:

- a small core is genuinely useful
- a medium-sized portion is only useful to framework internals
- a large portion is public ahead of real user adoption

The best immediate simplification target is not the low-level resolver layer. That part is coherent.

The best simplification targets are:

- public extension points that are not yet truly usable
- future-facing flex/grid/alignment fields that have no active consumer path yet
- internal stylesheet bridge types living in a public header

In short: the current layout API has a good core, but it exposes substantially more surface area than current users can actually benefit from.
