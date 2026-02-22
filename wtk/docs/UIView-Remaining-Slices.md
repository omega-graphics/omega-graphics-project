# UIView Remaining Slices (Sync-Engine Aligned Re-evaluation)

This document tracks remaining `UIView` work after compositor sync-engine and animation runtime updates.

## Current State (What Is Already Landed)

1. Core `UIView` model is in place:
- `layout`, `setLayout`, `setStyleSheet`, `update`.
- `Widget::makeUIView(...)` integration is in use across tests.

2. Shape rendering support is broader than the original plan:
- `Rect`
- `RoundedRect`
- `Ellipse` (`Core::Ellipse` and `GEllipsoid` input support)
- `Path` (stroke + optional close)

3. Per-element layer allocation exists:
- one layer/canvas bundle per `UIElementTag`
- inactive tags are disabled rather than hard-deleted

4. Sync-engine and animation foundation exists in compositor:
- lane packet telemetry and pacing
- `TimingOptions` clock modes
- clip animation runtime (`animate(...)`, `animateOnLane(...)`)

5. Test migration is underway and active:
- `BasicAppTest`, `TextCompositorTest`, `EllipsePathCompositorTest` are using `UIView` content paths.

## Remaining Gaps

1. `UIView` dirty tracking is not yet efficient:
- `layoutDirty`/`styleDirty` are set but `update()` still repaints/submits broadly.

2. Style transition fields are mostly declarative today:
- `transition`, `duration`, `elementBrushAnimation`, `elementAnimation`, `elementPathAnimation` are not fully executed by a concrete `UIView` animation driver.

3. `UIViewLayout::Element::Text` is declared but not rendered by `UIView::update()`.

4. Layer ordering and first-frame effect consistency still need hard guarantees under resize/load.

## Slice B: Render Determinism + Dirty Submission

### Goals

1. Make `UIView` frame output deterministic on frame 1 and during active resize.
2. Reduce redundant submissions by only touching dirty root/element targets.
3. Preserve stable z-order for element tags across add/remove/reorder.

### Scope

1. Dirty pipeline
- Introduce per-element dirty state keyed by tag (`layoutDirty`, `styleDirty`, `contentDirty`).
- Repaint only dirty targets plus root when needed.
- Skip `sendFrame()` for unchanged element canvases.

2. Deterministic ordering
- Re-assert layout-order z-index every update pass (not only on creation).
- Keep disabled-layer behavior for removed tags, plus optional reclaim policy.

3. First-frame readiness
- Ensure root + active element layers are materialized and effect-ready before first visible commit.
- Avoid "first-frame missing effects" by forcing one coherent initial packet per view.

### Acceptance Criteria

1. First visible frame contains expected element set and effects.
2. Repeated resize does not cause element pop-in or delayed layer activation.
3. No extra packet churn for unchanged elements in steady state.

## Slice C: UIView Animation Driver (On Sync Runtime)

### Goals

1. Execute `UIView` style/layout transitions through the new lane-aware animation runtime.
2. Keep animation writes packet-coherent with WidgetTree sync-lane pacing.

### Scope

1. Transition interpretation
- Map style entries with `transition=true`/`duration` into animation clips.
- Support brush channel interpolation:
  - `ColorRed`, `ColorGreen`, `ColorBlue`, `ColorAlpha`
- Support geometry keys:
  - `Width`, `Height`
  - path node keys (`PathNodeX`, `PathNodeY`) where path tags exist

2. Lane binding
- Bind each `UIView` animation to host sync lane.
- Use `animateOnLane(...)` for deterministic pacing when explicit lane is required.

3. Runtime state
- Add per-element animation state cache so concurrent updates merge cleanly.
- Cancel/replace previous animations on same target/property (last-writer semantics).

### Acceptance Criteria

1. Style transitions are visibly smooth and packet-coherent under resize load.
2. No stale intermediate frames replay when lane drops packets.
3. Animated properties land exactly on target values at completion.

## Slice D: UIView Text Element Support

### Goals

1. Make `UIViewLayout::text(...)` functional in `UIView::update()`.
2. Support mixed text+shape content per view with deterministic layering.

### Scope

1. Text rendering path
- Render `Element::Text` through canvas `drawText(...)`.
- Add minimal text style descriptors (font/color/alignment/wrap) for `UIView`.

2. Ordering
- Respect layout order between text and shapes using same per-tag layer model.

### Acceptance Criteria

1. `UIView` can render both text and shapes in one layout.
2. Text remains stable and synchronized during resize/animation.

## Slice E: Effects and Quality Controls in UIView Styles

### Goals

1. Expose compositor effects (drop shadow/blur) as first-class `UIView` style hooks.
2. Keep effects stable across first frame and active resize pressure.

### Scope

1. Style API extensions
- Add per-view/per-element effect entries:
  - drop shadow
  - gaussian blur / directional blur
- Route to existing compositor layer/frame effect application paths.

2. Sync-engine compatibility
- Respect lane budget behavior during resize.
- If adaptive quality policies are enabled at compositor level, ensure `UIView` effects degrade/restore coherently.

### Acceptance Criteria

1. Effects apply consistently on first frame and during resize.
2. No black/opaque fallback artifacts from effect-enabled layers.

## Slice F: API/Docs Stabilization + Regression Suite

### Goals

1. Freeze `UIView` authoring semantics before `SVGView`/`VideoView` adoption.
2. Add targeted regression coverage for the bugs seen during compositor iteration.

### Scope

1. API semantics
- Document selector matching behavior and precedence.
- Keep builder model explicit (`copy()` chain semantics) and document expected usage.
- Add helper shortcuts for common shape/style patterns.

2. Regression tests
- Add/extend tests for:
  - first-frame effect presence
  - resize jitter/position drift
  - mixed shape+text `UIView`
  - animated `UIView` style transitions

3. Docs/examples
- Add a compact "UIView cookbook" with:
  - static layout
  - animated style updates
  - effect-enabled elements

### Acceptance Criteria

1. API behavior is documented and deterministic.
2. Known regressions are covered by repeatable tests.

## Non-Goals (Current Remaining Slices)

1. CSS-complete selector grammar.
2. Rich text shaping/layout engine beyond existing compositor text primitives.
3. Backend-specific `UIView` forks; keep backend-neutral through compositor contracts.

## Suggested Rollout

1. Slice B first (determinism + dirty submission + first-frame guarantees).
2. Slice C next (animation driver on sync runtime).
3. Slice D and E in parallel where feasible (text + effects).
4. Slice F last (final API/docs + regression suite hardening).
