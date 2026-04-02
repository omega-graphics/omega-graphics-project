# Frontend/Backend LayerTree Sync Plan

## Goal
Keep frontend `Composition::LayerTree` and backend visual tree synchronized per `WidgetTree` lane so resize/layout updates do not present mixed epochs and jitter.

## Sync Contract
1. Frontend `LayerTree` is authoritative.
2. Backend tree is a mirror updated from explicit mutation deltas.
3. Every mutation belongs to a monotonically increasing per-tree epoch.
4. Render packets should eventually be gated by required tree epoch (future slices).

## Slices

### Slice A: Observer Bridge + Delta Model + Epoch Generation
1. Register compositor as a `LayerTreeObserver`.
2. Track observed trees and detach safely.
3. Convert frontend events into deltas:
   - `TreeAttached`
   - `TreeDetached`
   - `LayerResized`
   - `LayerEnabled`
   - `LayerDisabled`
4. Increment per-tree epoch for every delta and queue it for later backend apply.
5. Expose a lightweight snapshot API for diagnostics (`lastIssuedEpoch`, pending delta count).

### Slice B: Delta Packetization
1. Bind queued layer-tree deltas to sync lane packet metadata.
2. Coalesce duplicate layer mutations inside the same packet window.
3. Preserve structural ordering (`attach` before `resize`, `detach` last).

### Slice C: Backend Mirror Apply
1. Introduce backend mirror store keyed by stable layer identity.
2. Apply queued deltas before render pass encoding.
3. Remove lazy backend structure creation from render hot path.

### Slice D: Epoch Gating
1. Render command carries `requiredTreeEpoch`.
2. Scheduler/admission blocks render until backend mirror has applied required epoch.
3. Drop stale render packets for superseded epochs in same lane.

### Slice E: Resize/Animation Coalescing
1. Frame-cadence coalescing of geometry deltas during live resize.
2. Lane-pressure aware coalescing policy for effect-heavy trees.
3. Keep last complete epoch visible while newer epoch is still applying.

### Slice F: Diagnostics + Tests
1. Add trace hooks for `deltaQueued`, `deltaApplied`, `renderWaitEpoch`, `epochDropped`.
2. Add stress tests for:
   - live resize on multi-layer `UIView` content
   - animation + effects + resize on same lane
3. Add assertions for monotonic epoch progression per tree.

## Slice A Implementation Notes (Current)
1. Compositor now implements `LayerTreeObserver`.
2. `WidgetTreeHost` and `Widget` register/unregister layer trees with compositor observer bridge.
3. Per-tree epoch and pending delta queues are tracked in compositor memory.
4. A snapshot method is available for runtime inspection:
   - `Compositor::getLayerTreeSyncSnapshot(LayerTree *tree)`
5. Backend apply/gating is intentionally not changed in Slice A.

## Slice B-C Implementation Notes (Current)
1. Layer trees are now lane-bound when observed:
   - `Compositor::observeLayerTree(LayerTree *tree, uint64_t syncLaneId)`
2. On packet queue, compositor packetizes pending deltas for that lane+packet:
   - `Compositor::bindPendingLayerTreeDeltasToPacketLocked(...)`
3. Packet-window coalescing is active:
   - Duplicate layer/type mutations are collapsed.
   - Structural ordering is normalized as `attach -> mutate -> detach`.
4. Packet metadata carries `requiredEpochByTree` and the coalesced delta payload.
5. Backend mirror store has been introduced in compositor state:
   - Tree-level attached/epoch state.
   - Layer-level rect/enabled/epoch state keyed by stable `Layer*`.
6. Before render encoding, compositor applies packet deltas into the backend mirror and primes backend visual surfaces from mirror state:
   - `Compositor::applyLayerTreePacketDeltasToBackendMirror(...)`
7. Packet metadata is released after packet/standalone command processing to avoid unbounded growth:
   - `Compositor::releaseLayerTreePacketMetadata(...)`

## Slice D-F Implementation Notes (Current)
1. `CompositorCommand` now carries `requiredTreeEpoch`.
2. On schedule, packet metadata required epochs are stamped onto packet/render commands:
   - `Compositor::stampCommandRequiredEpochLocked(...)`
3. Scheduler admission now gates render-like commands on backend mirror epoch readiness:
   - `Compositor::waitForRequiredTreeEpoch(...)`
4. Backend mirror apply can be invoked in a mirror-only pass (no render target) to satisfy epoch gate:
   - `Compositor::applyLayerTreePacketDeltasToBackendMirror(..., target=nullptr)`
5. Stale packet dropping now includes epoch-dominance detection per lane:
   - `Compositor::isPacketEpochSupersededLocked(...)`
   - drop reason: `EpochSuperseded`
6. Resize/effect coalescing policy now uses both lane pressure and packet epoch metadata:
   - geometry packets are coalesced more aggressively during resize bursts
   - effect-heavy packets can be coalesced when lane is under pressure
7. Trace hooks were added for slice diagnostics:
   - `deltaQueued`
   - `deltaApplied`
   - `renderWaitEpoch`
   - `epochDropped`
8. Monotonic epoch assertions are now enforced when applying backend mirror deltas.
9. Lane diagnostics include epoch-wait/epoch-drop telemetry:
   - `epochWaitCount`, `epochWaitTotalMs`, `epochDropCount`
