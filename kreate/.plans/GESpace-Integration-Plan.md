# GESpace Integration (Kreate side of GESpace Plan Phase 5)

Kreate's `Scene`/`Object` now delegate their transform + projection to GTE's `GESpace`, so
there is one transform authority (GTE-owned). This is the Kreate-side breakdown of
`gte/.plans/GESpace-Implementation-Plan.md` **Phase 5**, which holds the full contract and
the design decisions. Implemented, not proposed — this note records what shipped.

## What changed and why

- **`Mat4` is now row-major with a standard `operator*`** (`Math.h` / `Math.cpp`). It used
  to be column-major and route `operator*` through GTE's `FMatrix::operator*`, which composes
  in reverse (GESpace plan Finding A) — so `projection * view * world` silently reached the
  shader reversed, invisible only because everything was identity. Row-major + a direct
  multiply makes Kreate's math conventional; the row↔column transpose is isolated to the GTE
  boundary (`toFMatrix` in `MathConvert.h`, the renderer's push-constant flatten). This is
  the fix the engine owner specified.

- **`Scene` owns a `GESpace`.** `setViewMatrix` / `setProjectionMatrix` forward into it (the
  space owns the camera and composes `projection · view · model`); `add`/`remove` register /
  retire each object as a `GESpaceObjectID`; `render` pushes each object's transform into its
  slot and draws with `objectTransform(id)`. The old hand-rolled MVP multiply is gone. The
  space's viewport is re-anchored to the window each frame (only feeds `spaceToNDC`, which the
  perspective projection replaces — it matters to a future 2D scene, not this one).

- **`Object` is TRS-only.** `setTransform(Mat4)` / `transform()` are removed; the API is
  `setPosition` / `setRotationEuler` / `setRotationAxis` / `setScale`. The transform is stored
  as GTE's `GESpaceTransform` and read by the Scene through the internal `ObjectAccess` bridge
  (`src/ObjectAccess.h`), so no GTE type leaks onto the public `Object` header. Rotation
  setters **replace** orientation; compose your own delta to spin.

- **The renderer takes the MVP as an `FMatrix<4,4>`** (`objectTransform` output, already
  column-major), flattened straight into the push constant — no `Mat4` in the hot path.

## Deferred

- **Parent-child hierarchy.** GESpace objects are flat. Composing a hierarchical world
  transform correctly needs a GPU-order matrix multiply Kreate can't spell without
  re-exposing the reversed `operator*`, so `render` treats every object as a root and logs a
  one-time note if a caller parents one. True hierarchy belongs to the Scene-model phase
  (Engine-Roadmap Phase 6); the natural hook is a `GESpace` helper that composes the camera
  with an externally-supplied world matrix.

## Open

- **Visual verification** against a non-identity view (the spinning `BasicGame` cube) is a
  screenshot handoff per AGENTS.md Visual Debugging — pending at time of writing.
- **Bundle packaging gap (pre-existing, not from this work):** `BasicGame.app` embeds
  `OmegaGTE.framework` but not `libKREATE.dylib`, so the bundle only launches with
  `DYLD_LIBRARY_PATH` pointing at `build/lib`. Worth its own fix.
