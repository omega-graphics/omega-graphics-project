# OmegaSL Generics & Automatic Differentiation Plan

> **Lifecycle:** `future/` — forward-looking, not yet greenlit. Captures the
> design before the work is scheduled; move to the top level of `.plans/` when
> implementation begins.

## Goal

Grow OmegaSL from a deliberately-minimal shading language toward a more
general-purpose one by adding two capabilities that today only larger languages
(notably **Slang**) offer:

1. **Compile-time generics** — type-parameterized functions and structs, ideally
   with type constraints (interfaces).
2. **Automatic differentiation** — compiler-generated derivative functions from
   ordinary OmegaSL code, for differentiable rendering and neural/GPU-ML work.

Both are authored in OmegaSL and compile, **unchanged**, to the existing
HLSL / MSL / GLSL back-ends. This document follows the multi-phase rule from
`AGENTS.md`: research, proposal, refinement, plan, then incremental
implementation.

---

## 0. Strategic Note — a Deliberate Shift From "Intentionally Small"

`docs/About.rst` currently states that OmegaSL "is intentionally not a large
general-purpose language" and "excludes features that do not map cleanly across
all three target languages." This plan is a **conscious, scoped move off that
stance** — and it should be recognised as such rather than slipped in quietly.

Two honest points make the move defensible rather than contradictory:

- The About.rst warning is specifically about features **that cannot be reliably
  translated** across HLSL/MSL/GLSL ("a cross-platform bug waiting to happen").
  Neither generics nor autodiff is such a feature — see §1. They add *front-end
  expressiveness*, not *back-end divergence*.
- Making OmegaSL more capable is complementary to, not a reversal of, supporting
  Slang as an alternative language in kREATE (`kreate/.plans/Engine-Roadmap.md`,
  Phase 17). Support Slang for teams who already have it; grow OmegaSL for teams
  who want one integrated in-house stack. The two narrow the same gap from
  opposite directions.

**Doc follow-up (not done here):** once this lands, the "intentionally small"
passage in About.rst must be updated to describe the new stance accurately.
Flagged, not silently changed.

---

## 1. Why These Two Fit OmegaSL's Architecture

The load-bearing insight: **both are front-end features that erase to concrete,
per-target arithmetic before code generation.** Neither adds anything that must
be *spelled differently* on HLSL vs. MSL vs. GLSL.

- **Generics monomorphize.** GLSL has no templates or generics at all, so
  target-level generics are impossible on one of the three back-ends. The only
  workable design is to fully instantiate (monomorphize) generics in the
  front-end and emit concrete, non-generic declarations per target. Codegen
  never sees a type parameter. GLSL's limitation is the forcing function, and it
  points at the correct design rather than blocking it.
- **Autodiff is a source-to-source transform.** A differentiable function is
  rewritten into another function built from the *same* arithmetic the three
  back-ends already support (add, mul, the existing intrinsics). Codegen sees
  ordinary OmegaSL. No target needs a new capability.

So the divergence surface either feature adds **at codegen is ~zero**; essentially
all the work lives in Parser / Sema / a new AST-transformation pass. This is also
how Slang implements them (monomorphization + source-to-source differentiation),
so the shape is proven, not speculative.

---

## 2. Current State (grounded in the tree)

Verified against `gte/omegasl/src/` so the plan does not assume features that
already exist or misname ones that don't:

- **No language-level generics today.** `Parser::parseGenericDecl`
  (`gte/omegasl/src/Parser.cpp:1999`) is the *general declaration dispatcher* —
  it consumes an optional `const`/`threadgroup` prefix and routes to var/struct
  decls. It is unrelated to templates. There is no `<T>` parameter syntax.
- **No language `interface` construct today.** `InterfaceGen` /
  `INTERFACE_FILENAME "interface.h"` (`gte/omegasl/src/CodeGen.h:15`) generates
  the *reflection interface header* that pairs a compiled shader with host code.
  It is not a Slang-style type-constraint `interface`. A constraint mechanism, if
  we add one, is new.
- **`§5.4 Derivatives` is NOT autodiff.** In
  `gte/.plans/OmegaSL-Feature-Gap-Survey.md`, §5.4 covers screen-space hardware
  derivatives (`ddx` / `ddy` / `fwidth`, fragment-stage only). Automatic
  differentiation of arbitrary user functions is a different thing entirely and
  must not be conflated in docs or diagnostics.
- **Compiler pipeline:** Preprocessor → Lexer → Parser → AST → Sema → ConstFold →
  FeatureScanner → CodeGen (+ `HLSLTarget` / `MSLTarget` / `GLSLTarget`). Tokens
  are declared in `Toks.def`; AST node kinds in `AST.def` (both x-macro tables).
  New keywords/nodes are added there, matching the existing
  keyword→AST→Parser→Sema→Target-hook shape used for every prior feature.

---

## Part A — Generics

### A.1 Design

- **Syntax** (illustrative, to be finalised): type parameters on functions and
  structs —
  ```omegasl
  func lerpN<T>(T a, T b, float t) -> T { return a + (b - a) * t; }
  struct Pair<T> { T first; T second; };
  ```
- **Constraints / interfaces.** Bounded generics need a way to say "`T` must
  support these operations." Two options:
  - **(a) `interface` construct** — declare a set of required members/operations
    a conforming type must provide; generic bodies may only use what the
    constraint guarantees. Best diagnostics; matches Slang; more work.
  - **(b) Unconstrained, checked-at-instantiation** — no constraint syntax; a
    misuse surfaces only when a concrete type fails to compile in the
    instantiated body. Cheaper; worse error messages.
  **Recommendation: (a).** The constraint system is the biggest language-design
  fork here; getting it wrong hurts ergonomics for years, so design it
  deliberately rather than defaulting to duck typing.
- **Monomorphization.** Sema collects every instantiation site, substitutes
  concrete types, dedups identical instantiations, and name-mangles. A post-Sema
  / pre-codegen pass emits one concrete decl per used type set. Codegen is
  untouched — it only ever sees concrete types.

### A.2 Phases

- **A1 — Parser.** Generic-parameter lists on `func` / `struct` decls; new AST
  fields (`AST.def`). No semantics yet; parses and round-trips.
- **A2 — Sema.** Type-parameter environment, substitution, and (if A.1(a))
  constraint checking against declared interfaces.
- **A3 — Monomorphization pass.** Instantiate at use sites, dedup, mangle;
  hand concrete decls to codegen. Verify emitted HLSL/MSL/GLSL is byte-identical
  to a hand-written concrete equivalent.
- **A4 — `interface` construct** (if chosen). New keyword (`Toks.def`),
  conformance checking, diagnostics for unmet constraints.
- **A5 — Tests + docs.** omegaslc conformance corpus (`gte/omegasl/tests/`), a
  new Feature-Gap-Survey section, and an OmegaSL-Reference.md chapter.

---

## Part B — Automatic Differentiation

### B.1 Design

- **Marking.** A `[differentiable]` attribute (or a `diff` qualifier) on a
  function opts it into differentiation and records a `Differentiable`
  type-property in Sema.
- **Forward mode first (dual numbers).** Generate a companion function that
  propagates derivatives alongside values. Mechanically simpler and covers a
  large fraction of real uses. Invoked via a `__fwd_diff(f)`-style expression
  that resolves to the generated function.
- **Reverse mode later (backpropagation).** Needs a tape or recomputation
  strategy, and **arbitrary control flow (loops, dynamic branches) is the genuine
  research-grade difficulty.** Scope this as a separate, later, independently-
  gated effort — do **not** commit to it in the same breath as forward mode.
- **Codegen unchanged.** Every generated derivative is ordinary OmegaSL
  arithmetic; no back-end needs a new capability. This is the clean part and the
  reason autodiff fits at all.
- **Interaction with generics.** General differentiable code wants an
  `IDifferentiable`-style interface (Slang's model), which makes Part A's
  `interface` construct a natural dependency for the *general* case. Forward-mode
  on concrete `float`/`floatN` functions can ship **before** generics land, so
  the two parts are orderable independently.

### B.2 Phases

- **B1 — Marking.** `[differentiable]` attribute/keyword, AST flag, Sema
  `Differentiable` property; no transform yet.
- **B2 — Forward-mode transform.** Dual-number source-to-source generation on
  concrete float functions; emit as ordinary decls into the AST before codegen.
- **B3 — Invocation + tests.** `__fwd_diff(f)`-style resolution; numeric tests
  comparing generated derivatives against finite-difference references.
- **B4 — Reverse mode** *(later, separately gated)*. Tape/checkpoint strategy;
  control-flow handling; the hard problem, tracked on its own timeline.
- **B5 — Tests + docs.** Survey section, Reference chapter, and a parity note vs.
  Slang autodiff so users can compare.

---

## 3. Risks & Open Questions

- **Reverse-mode + control flow** is the hard research problem. Keep it out of
  the initial commitment; forward mode delivers most of the value first.
- **Code-size / compile-time blowup** from monomorphization when many concrete
  instantiations exist. Dedup aggressively; measure.
- **Constraint-system design** (the `interface` construct) is the highest-stakes
  language-design decision in this plan — it shapes diagnostics and ergonomics
  permanently. Prototype before committing syntax.
- **About.rst** "intentionally small" passage needs updating once this lands
  (§0) — a documentation follow-up, not part of the compiler work.
- **Feature-gating.** Neither feature needs a `GTEDeviceFeatures` bit — both are
  fully resolved at compile time and emit baseline shader code. Confirm the
  `FeatureScanner` records nothing new for them (no runtime capability implied).

---

## 4. How This Follows the Repo's Rules

- **"Extend, don't work around"** (`AGENTS.md`): both features land *in the
  compiler* — Parser/Sema/AST-transform — with omegaslc tests, a
  Feature-Gap-Survey entry, and an OmegaSL-Reference.md section each,
  section-by-section, never contorted around at a call site.
- **Multi-phase rule**: research → proposal → this plan → incremental
  implementation, with A- and B-phases independently reviewable and each leaving
  omegaslc's existing conformance corpus green.
