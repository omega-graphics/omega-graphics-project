# UIView Remaining Slices (After Slice A)

This document tracks the remaining implementation work for `UIView` after Slice A landed.

## Status

- Slice A: complete
  - `UIView` state model (`layout`, `style`, `update`) is in place.
  - `Widget::makeUIView(...)` is implemented.
  - Rendering supports `Rect` and `RoundedRect`.
- Remaining: Slice B and Slice C.

## Slice B: Layered Renderer and Resize Semantics

### Goals

- Make element layering deterministic and stable across updates.
- Ensure resize behavior is correct and does not leave stale element geometry.
- Reduce unnecessary frame submission by using dirty-region/dirty-element logic.

### Scope

1. Stable per-element layer ownership
- Keep one `Layer + Canvas` per element tag.
- Preserve z-order based on layout order.
- Disable or reclaim layers when elements are removed.

2. Resize-aware rendering
- On `UIView`/host resize, mark all elements dirty.
- Recompute shape geometry against the updated bounds.
- Repaint only dirty elements and root background/border.

3. Dirty tracking improvements
- Track separate dirty flags for:
  - layout structure changes
  - style changes
  - per-element content/style changes
- Avoid submitting frames for unchanged elements.

4. Rendering capability hardening
- Keep `Rect`/`RoundedRect` as first-class supported primitives.
- Define explicit behavior for unsupported element types (`Text`, `Path`, `Ellipse`) in this slice:
  - no crash
  - clear no-op/logging path

### Acceptance Criteria

- Resizing repeatedly does not leave ghost visuals.
- Add/remove/reorder element tags updates layers consistently.
- No duplicate layers are created for the same tag.
- No crashes when unsupported element types appear in layout.

## Slice C: Animation + API Polish

### Goals

- Add first production-level animation support for `UIView`.
- Improve authoring ergonomics of style/layout APIs.
- Finalize developer documentation for future widget work.

### Scope

1. Transition and animation support
- Implement `elementBrushAnimation(...)` for color channel keys:
  - `ColorRed`, `ColorGreen`, `ColorBlue`, `ColorAlpha`
- Implement size transitions for `Width`/`Height` where shape type supports it.
- Keep path-node animation deferred unless path render backend is completed.

2. Style selector behavior
- Formalize selector precedence:
  - exact tag > wildcard
  - last matching rule wins
- Document matching semantics for view tag + element tag combinations.

3. API cleanup
- Evaluate `StyleSheet` mutability model and finalize one behavior:
  - immutable builder (`copy`/chain) or mutable builder (in-place).
- Add convenience helpers for common style operations:
  - fill-only shape
  - bordered shape
  - state variants (hover/pressed-ready hooks for future input integration)

4. Documentation and examples
- Add an example widget demonstrating:
  - multi-element `UIView` layout
  - style application
  - manual `update()` flow
- Add a short troubleshooting section for unsupported shape/animation keys.

### Acceptance Criteria

- Brush color animations interpolate correctly and complete at target values.
- Width/height transitions update geometry without layer churn.
- Style precedence is deterministic and documented.
- Example compiles and visually updates as expected.

## Non-Goals (for B/C)

- Full text rendering pipeline in `UIView`.
- Full path tessellation integration in `UIView`.
- New compositor primitive types beyond what current backend already renders.

## Suggested Rollout

1. Implement Slice B first and stabilize resize/update behavior.
2. Add Slice C color/size animation support.
3. Ship docs + example together with Slice C to keep usage aligned with final API behavior.

