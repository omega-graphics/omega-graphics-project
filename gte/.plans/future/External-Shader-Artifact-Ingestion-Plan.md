# External Shader-Artifact Ingestion Seam — Plan (STUB)

> **Lifecycle:** `future/` — **stub**, not yet a full plan. Captures the seam
> and the one decision that gates it, so the design is on record. Expand into a
> full multi-phase plan (per `AGENTS.md`) once the ingestion approach in §4 is
> chosen. Do not start implementation from this stub.

## Why this exists

`kreate/.plans/Engine-Roadmap.md` Phase 17 (authoring kREATE materials in
**Slang**) needs OmegaGTE to load a shader whose per-backend module was produced
by a compiler **other than `omegaslc`**. Every load path today assumes the
`omegaslc`-produced `.omegasllib` container, so that assumption is the seam this
plan opens. The same seam serves any future external shader source (hand-written
DXIL/SPIR-V/MSL, a third-party material compiler), not only Slang.

## Current state (grounded)

Verified in `gte/`:

- **Three load entry points**, all on `OmegaGraphicsEngine`
  (`gte/include/omegaGTE/GE.h`):
  - `loadShaderLibrary(FS::Path)` (`GE.cpp:176`)
  - `loadShaderLibraryFromInputStream(std::istream&)` (`GE.cpp:123`)
  - `loadShaderLibraryRuntime(std::shared_ptr<omegasl_shader_lib>&)` (`GE.cpp:198`)
- All three funnel per-shader records through the backend hook
  `_loadShaderFromDesc(omegasl_shader *shaderDesc, bool runtime)`
  (`GE.h:379`), i.e. **every shader that reaches a backend is an
  `omegasl_shader`**.
- The container is `OmegaSLShaderArchive` (`gte/omegasl/src/ShaderArchive.h:30`);
  the runtime library type is `omegasl_shader_lib`.
- **Feature gating already lives on this path.** Each `omegasl_shader` carries a
  `requiredFeatures` mask; the load path masks it against the device
  (`_deviceFeatures`, `GE.h:375`) and substitutes `_makeUnsupportedShaderSentinel`
  (`GE.h:396`) for any shader the hardware can't run, with `_checkPipelineShader`
  (`GE.h:407`) surfacing the diagnostic at pipeline build. See
  `[[project-omegasl-16-metal-tessellation]]` and the `#requires` gate in
  `OmegaSL-Swizzle-Subgroup-Plan.md` Part F for the same machinery.

**Implication:** whatever the ingestion approach, an external shader must end up
as an `omegasl_shader` record (module bytes + reflection/binding layout +
`requiredFeatures`) before it hits `_loadShaderFromDesc`, or that backend hook
must gain a sibling that accepts a raw module. That fork is §4.

## Scope

| In scope | Out of scope |
|---|---|
| A seam to ingest per-backend shader modules not produced by `omegaslc` | The Slang toolchain integration itself (kREATE Phase 17 owns invoking `slangc`) |
| Mapping external reflection → the binding/layout metadata GTE needs | A general shader-reflection *library* |
| Keeping feature-gating on the external path (`requiredFeatures` still honored) | A second, parallel capability model |
| Deciding the container-vs-raw-module fork (§4) | Committing to either until the decision is made |

## 4. The one decision that gates everything

**How does an external artifact enter GTE?** Two options; pick before writing the
full plan.

**Option A — Wrap into `.omegasllib`.** A container *writer* (that is not
`omegaslc`) packs external per-backend modules + a translated reflection/layout
into a valid `OmegaSLShaderArchive`. Then **all three existing load paths and the
whole feature-gating chain work unchanged** — the external shader is just another
`omegasl_shader`.
- *Pro:* maximal reuse; zero new backend hooks; gating/sentinel/pipeline-check
  all free.
- *Con:* requires the `.omegasllib` / `omegasl_shader` layout to be documented
  and exposed as a *writable* spec; someone must translate external reflection
  into `omegasl_shader`'s binding records.

**Option B — Raw-module load path.** A new
`OmegaGraphicsEngine::loadShaderModules(...)`-style API takes per-backend module
bytes + a caller-supplied binding/reflection descriptor + a `requiredFeatures`
mask, bypassing the container and building `GTEShader`s directly (a sibling to
`_loadShaderFromDesc`).
- *Pro:* no coupling of external shaders to OmegaSL's container format; caller
  owns reflection.
- *Con:* must re-plumb feature-gating, the unsupported-sentinel path, and
  pipeline-shader checking for this path so it does not silently diverge from the
  `.omegasllib` path.

**Recommendation to evaluate first: Option A**, because the gating + sentinel +
pipeline-check machinery is the expensive part and A gets it for free. The real
cost in A is a documented, writable container spec — worth pricing before
deciding.

**Secondary decision — who owns reflection.** Slang has its own reflection API;
either translate it into GTE's binding records (A or B) or require the caller
(kREATE material layer) to supply the binding descriptor. This must be pinned
alongside §4 — it is the actual work in either option.

## Boundaries

- **This seam is GTE-owned.** kREATE Phase 17 *consumes* it; the `slangc`
  invocation and material-to-shader wiring stay in kREATE.
- **Feature-gating stays unified.** Whichever option, external shaders honor
  `GTEDeviceFeatures` through the same `requiredFeatures` mask — no second model.

## Open questions (for the full plan)

1. Container writable-spec cost (Option A) vs. gating re-plumb cost (Option B) —
   price both before choosing.
2. Reflection ownership: translate Slang reflection, or caller-supplied descriptor?
3. Binding-slot assignment: does the external artifact carry GTE-compatible
   register/space/binding numbers, or does the seam assign them?
4. Advanced-stage metadata: how do tessellation / mesh-shader descriptors (which
   `omegaslc` emits into the archive) arrive for an external shader that uses
   those stages — or are they out of scope for external artifacts initially?
