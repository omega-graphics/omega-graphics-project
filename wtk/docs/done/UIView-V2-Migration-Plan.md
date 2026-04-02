# UIView V2 Layout Migration Plan

Migrate UIView from dual rendering paths (legacy `UIViewLayout` + `UIViewLayoutV2`) to a single V2 path. Delete the legacy path and its dead code.

---

## Why

UIView has two fully duplicated rendering paths (~200 lines each) selected by a `useLayoutV2_` bool. They share the same shape/text/effect rendering logic but diverge in layout resolution, dirty tracking, frame ordering, and root frame handling. Bugs fixed in one path (e.g. the no-op root frame fix) must be manually applied to the other. No external caller uses V2 today — all three tests and the ContainerClampAnimationTest use the legacy path.

---

## Current state

### Legacy path (`UIViewLayout`)
- Flat list of `Element` entries: `{tag, text-or-shape}`.
- No layout resolution — shapes carry absolute coordinates, text fills view bounds.
- Caller positions everything.
- Dirty tracking: per-element `ElementDirtyState` with 5 flags, `syncElementDirtyState` diffing, coherent-frame logic.
- Frame order: root canvas FIRST, element canvases SECOND.

### V2 path (`UIViewLayoutV2`)
- Element specs with `LayoutStyle`: width, height, margin, padding, clamps, z-index.
- Layout resolution via `resolveClampedRect`, `mergeLayoutRulesIntoStyle`, `resolveLayoutTransition`.
- Frame order: element canvases FIRST, root canvas SECOND.
- No per-element dirty tracking — redraws all elements every `update()`.
- Layout transition emission via `computeLayoutDelta`.

### Callers to migrate

| Caller | Elements | What it uses |
|--------|----------|-------------|
| `TextCompositorTest` | 1 shape (`accent_rect`), 1 text (`accent_label`) | `UIViewLayout::shape()`, `UIViewLayout::text()` with explicit rect, `setLayout()` |
| `EllipsePathCompositorTest` — RoundedFrameWidget | 2 shapes (`rounded_outer`, `rounded_inner`) | `UIViewLayout::shape()`, `setLayout()` |
| `EllipsePathCompositorTest` — EllipseOnlyWidget | 1 shape (`ellipse_shape`) | `UIViewLayout::shape()`, `setLayout()` |
| `EllipsePathCompositorTest` — PathOnlyWidget | 1 shape (`path_shape`) | `UIViewLayout::shape()`, `setLayout()` |
| `ContainerClampAnimationTest` | shapes | `UIViewLayout::shape()`, `setLayout()` |

All callers use absolute coordinates. None rely on layout resolution (Px/Dp/Percent).

---

## Migration strategy

### Step 1 — Make `setLayout()` write to V2 internally

Instead of migrating every caller at once, make the legacy `setLayout(UIViewLayout)` method convert its elements to `UIElementLayoutSpec` entries and write them to `currentLayoutV2_`. This is a shim: callers don't change, but all rendering goes through the V2 path.

```cpp
void UIView::setLayout(const UIViewLayout & layout){
    UIViewLayoutV2 v2 {};
    for(const auto & element : layout.elements()){
        UIElementLayoutSpec spec {};
        spec.tag = element.tag;
        if(element.shape){
            spec.shape = element.shape;
        }
        if(element.str){
            spec.text = element.str;
            if(element.textStyleTag){
                spec.textStyleTag = element.textStyleTag;
            }
        }
        // Absolute positioning — the shape already carries its coordinates.
        // Use Auto layout so resolveClampedRect returns the full available rect,
        // and the shape coordinates are used as-is during rendering.
        spec.style.width = LayoutLength::Auto();
        spec.style.height = LayoutLength::Auto();
        v2.element(spec);
    }
    setLayoutV2(v2);
    setUseLayoutV2(true);
}
```

The V2 rendering path already handles `Shape` and text the same way the legacy path does — `drawRect`, `drawEllipse`, `drawPath`, `drawText` with the shape's own coordinates. The `LayoutStyle` defaults to Auto, so `resolveClampedRect` returns the full available rect and the shape coordinates pass through unchanged.

**Risk**: The legacy `text()` overload with an explicit `Core::Rect` sets `element.textRect`. The V2 path uses `localBounds` for text rect instead of `element.textRect`. This needs to be carried through — either via a new field on `UIElementLayoutSpec` or by encoding the text rect into the `LayoutStyle` as absolute position + size.

### Step 2 — Add text rect support to V2

The legacy `UIViewLayout::Element` has an optional `textRect` field used by `TextCompositorTest`. The V2 path currently ignores this and uses `localBounds` for all text elements.

Option A: Add `Core::Optional<Core::Rect> textRect` to `UIElementLayoutSpec`. The V2 rendering path checks this before falling back to localBounds.

Option B: Convert the textRect into LayoutStyle absolute positioning:
```cpp
if(element.textRect){
    spec.style.position = LayoutPositionMode::Absolute;
    spec.style.insetLeft = LayoutLength::Px(element.textRect->pos.x);
    spec.style.insetTop = LayoutLength::Px(element.textRect->pos.y);
    spec.style.width = LayoutLength::Px(element.textRect->w);
    spec.style.height = LayoutLength::Px(element.textRect->h);
}
```

Option A is simpler and more honest. Option B is more principled but requires `resolveClampedRect` to handle absolute positioning correctly (it may not today). **Recommend Option A.**

### Step 3 — Port dirty tracking to V2

The legacy path has per-element dirty tracking (`ElementDirtyState`) that the V2 path doesn't use — V2 redraws everything on every `update()`. For initial migration this is fine (correctness over optimization), but the dirty tracking should be ported to V2 before the migration is complete:

- The V2 path already computes `dirtyActiveTags` implicitly (all elements are "dirty" since it redraws everything).
- Port the `syncElementDirtyState` / `isElementDirty` / `clearElementDirty` logic into the V2 path so that steady-state updates only redraw changed elements.
- The `submitRoot` guard (only send root frame when root state changed) should also be ported.

This is an optimization step, not a correctness step. It can follow the initial migration.

### Step 4 — Delete dead code

Once all callers go through V2:

1. **Delete `UIViewLayout` class** and its methods (`text()`, `shape()`, `remove()`, `clear()`, `elements()`).
2. **Delete `useLayoutV2_` bool** and `useLayoutV2()` / `setUseLayoutV2()` accessors. V2 is the only path.
3. **Delete the legacy rendering block** in `update()` (lines ~2762–3188).
4. **Remove `setLayout(UIViewLayout)`** or make it a thin adapter that constructs V2 specs (if backward compatibility is desired for external callers).
5. **Clean up UIView members**: `currentLayout`, `rootLayoutDirty`/`rootStyleDirty`/`rootContentDirty`/`rootOrderDirty` (consolidate into the element dirty system or remove if V2's approach differs).

### Step 5 — Migrate test callers to V2 API directly (optional)

Once the shim is working and tested, optionally update test callers to use the V2 API directly:

```cpp
// Before (legacy):
UIViewLayout layout {};
layout.shape("ellipse_shape", Shape::Ellipse(ellipse));
uiView->setLayout(layout);

// After (V2):
UIElementLayoutSpec spec {};
spec.tag = "ellipse_shape";
spec.shape = Shape::Ellipse(ellipse);
uiView->layoutV2().element(spec);
```

This is cleanup, not required for correctness. The shim from Step 1 handles the translation.

---

## Dependency graph

```
Step 1: setLayout() shim → V2 path
    └─→ Step 2: Text rect support in V2
            └─→ Step 3: Port dirty tracking (optimization)
                    └─→ Step 4: Delete legacy path
                            └─→ Step 5: Migrate callers (optional cleanup)
```

Steps 1–2 are the migration. Step 3 is optimization. Step 4 is cleanup. Step 5 is polish.

---

## File change summary

| File | Step | Changes |
|------|------|---------|
| `wtk/src/UI/UIView.cpp` | 1 | `setLayout()` converts to V2 and delegates to `setLayoutV2()` |
| `wtk/include/omegaWTK/UI/UIView.h` | 2 | Add `textRect` to `UIElementLayoutSpec` (Option A) |
| `wtk/src/UI/UIView.cpp` | 2 | V2 rendering uses `textRect` when present |
| `wtk/src/UI/UIView.cpp` | 3 | Port dirty tracking into V2 path |
| `wtk/include/omegaWTK/UI/UIView.h` | 4 | Delete `UIViewLayout`, `useLayoutV2_`, `setUseLayoutV2()`, `currentLayout` |
| `wtk/src/UI/UIView.cpp` | 4 | Delete legacy rendering block (~400 lines) |
| `wtk/tests/*.cpp` | 5 | Optional: rewrite to use `UIElementLayoutSpec` directly |

---

## Risks

1. **Text rect handling**: The V2 path doesn't support per-element text rects today. Must be added in Step 2 before TextCompositorTest works through V2.
2. **Animation state**: The legacy path's `prepareElementAnimations` / `advanceAnimations` / `applyAnimatedColor` / `applyAnimatedShape` logic is shared between both paths but only the legacy path calls the `styleUsesAnimation` preparation block. V2 may need the same animation prep.
3. **Dirty tracking regression**: V2 redraws everything every frame. Acceptable for small element counts (tests have 1–3 elements) but wasteful for real UI with many elements. Step 3 addresses this.
4. **Element ordering**: Legacy uses insertion order for z-ordering. V2 uses explicit `zIndex` with insertion order as tiebreaker. The shim sets `zIndex = 0` for all elements, so insertion order governs — matches legacy behavior.
