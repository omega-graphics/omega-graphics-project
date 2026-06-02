# Single CAMetalLayer Backend Plan

## Goal
Remove cross-layer present skew by moving macOS backend composition to **one `CAMetalLayer` per `AppWindow`**.  
All widget/view/layer visuals become compositor-managed surfaces rendered into textures and then presented in a single final pass.

## Problem Statement
Current Metal backend creates many `CAMetalLayer` instances (`MTLCALayerTree::Visual`), each with its own drawable/present timing. During live resize and heavy updates, layers can present different epochs, causing:
1. intermittent bottom-left collapse frames
2. stale/mixed geometry snapshots
3. effect/primitive mismatch on first/intermediate frames

## Target Architecture

### Core Rule
1. Window owns one root `CAMetalLayer`.
2. Backend visual nodes do **not** own `CAMetalLayer`s.
3. Each visual node maps to a logical surface (texture + metadata) in backend memory.

### New Backend Components
1. `MTLWindowCompositorTarget`
   - owns root `CAMetalLayer` + native render target
   - one command buffer submission/present per packet
2. `MTLSurfaceStore`
   - allocates/recycles textures for logical visuals
   - tracks size/format/dirty/effect flags
3. `MTLSceneMirror`
   - mirror of frontend `LayerTree` state (rect, z-order, enabled, opacity, transform, effects)
   - epoch-bound (fed by sync engine delta packets)
4. `MTLFinalComposer`
   - encodes final pass: samples all visible surfaces into root target in deterministic order
   - applies clip/scissor/blend rules

## Render Pipeline (Per Packet)
1. Apply layer-tree deltas to `MTLSceneMirror` (required epoch gate).
2. For dirty visuals, render commands into their surface textures (`TextureRenderTarget`).
3. Run per-surface effects (blur/shadow prep) into effect textures.
4. Encode one final window pass into root `CAMetalLayer` drawable:
   - clear window background
   - draw surfaces in z-order (stable sort)
   - resolve opacity, transforms, clipping, effect outputs
5. Submit/present once; emit telemetry for the packet.

## Sync Engine Integration
1. Packet is atomic unit:
   - one lane packet -> one window present
2. `requiredTreeEpoch` must be applied before final composition.
3. Drop stale packets before final pass if superseded by newer lane packet.
4. Animation clock remains lane/packet paced (presented epochs).

## Data Model Changes
1. Replace `MTLCALayerTree::Visual` payload:
   - from: `CAMetalLayer *metalLayer`
   - to: `SurfaceId + BackendRenderTargetContext + visual state`
2. `BackendCompRenderTarget`:
   - add `WindowCompositorTarget` pointer
   - `surfaceTargets` remains, but no per-visual native layer target
3. `Native::Cocoa::CocoaItem::setRootLayer(...)`:
   - attaches only root window layer
   - no sublayer metal tree wiring

## Migration Slices

### Slice A: Root Target Isolation
1. Introduce `MTLWindowCompositorTarget` for single root layer ownership.
2. Keep existing per-visual render contexts temporarily, but stop presenting them directly.
3. Final copy always targets root drawable.

### Slice B: Visuals -> Logical Surfaces
1. Refactor `MTLCALayerTree::makeVisual` to allocate surface-only targets (no `CAMetalLayer`).
2. Remove `addSublayer` calls from backend tree assembly.
3. Keep geometry/effect methods but write state into mirror/surface metadata.

### Slice C: Scene Mirror + Epoch Gate
1. Feed `LayerTree` deltas into `MTLSceneMirror`.
2. Enforce `requiredTreeEpoch` before encoding final pass.
3. Make geometry reads come from mirror, not transient Cocoa frame state.

### Slice D: Unified Final Composer
1. Implement deterministic z-ordered compositor pass to root target.
2. Fold opacity/transform/shadow sampling into final pass inputs.
3. Present once per packet.

### Slice E: Surface Lifetime + Resize Policy
1. Introduce surface pool/recycle to avoid resize thrash allocations.
2. Resize surfaces only on stable dimensions; keep last valid size during transient samples.
3. Add safe clamps for max drawable/texture dimensions.

### Slice F: Cleanup + Compatibility Layer
1. Remove per-visual `CAMetalLayer` codepaths and transform-layer coupling.
2. Keep frontend API unchanged (`Layer`, `Canvas`, `UIView`, stack/container behavior).
3. Keep existing effect and animation APIs mapped to surface/mirror states.

### Slice G: Validation and Regression Gates
1. Add live-resize stress tests:
   - `EllipsePathCompositorTest`
   - `TextCompositorTest`
   - animated `UIView` multi-layer cases
2. Add pass criteria:
   - no bottom-left collapse frames
   - no mixed-epoch primitives/effects
   - first frame includes expected effects

## Risks and Mitigations
1. Risk: GPU cost increase due to extra final pass  
   Mitigation: dirty-region composition and surface reuse.
2. Risk: memory growth from many surfaces  
   Mitigation: pooled textures + LRU eviction.
3. Risk: migration regressions in effects  
   Mitigation: phase effects through compatibility adapters before removing old path.

## Acceptance Criteria
1. Exactly one `CAMetalLayer` is attached per window in Metal backend.
2. One present per lane packet for that window.
3. No cross-layer skew artifacts during active resize.
4. Existing tests render primitives/text/effects correctly before and after resize.

