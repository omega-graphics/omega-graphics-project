# OmegaSL CodeGen → Target Refactor Plan

Refactor the OmegaSL code-generation pipeline from "three sibling subclasses
of a virtual `CodeGen`" to "one concrete `CodeGen` driven by a polymorphic
`Target`." Compose, don't inherit.

## Why

`gte/omegasl/src/HLSLCodeGen.cpp` (848 LoC), `MetalCodeGen.cpp` (978 LoC),
and `GLSLCodeGen.cpp` (1280 LoC) — about **3,100 lines** total — duplicate
the same AST-walking shape (block emission, expression dispatch, decl
dispatch) and diverge only at small, well-bounded decisions:

- Type spellings (`float2` vs `vec2`).
- Attribute spellings (`SV_VertexID` vs `vertex_id` vs `gl_VertexIndex`).
- Builtin-call renaming (`lerp` → `mix`, `frac` → `fract`).
- Texture-op syntax (`tex.Sample(s,c)` vs `tex.sample(s,c)` vs
  `texture(sampler2D(t,s),c)`).
- Resource binding boilerplate (`register(t0)` / `[[buffer(0)]]` /
  `layout(set=,binding=)`).
- Stage entry-point signature.
- Discard keyword (`discard;` vs `discard_fragment();`).
- File extension and compile-driver invocation.

Today these target-specific decisions are sprinkled inline through the
1000-LoC subclass bodies; adding a new feature (e.g. the §1.2/1.3/1.4 work
in `OmegaSL-Feature-Gap-Survey.md`) means re-implementing the same
control-flow shape in three files and remembering to keep them in sync.
Bugs 1–6 in `OmegaSL-Reference.md` are all *one* of these three sites
diverging from the others.

After the refactor:

- `CodeGen` (non-virtual) owns the AST walk and shared rendering rules.
- `Target` (abstract) declares the divergence points as small named hooks.
- `HLSLTarget` / `MSLTarget` / `GLSLTarget` (in their own `.cpp` files)
  fill in those hooks.
- New language features land in `CodeGen` once and pick up the hooks for
  free; backend-specific deviations live in one place per target.

## Architecture target

```
                  ┌─────────────────────────────┐
                  │         CodeGen             │
                  │  (concrete, non-virtual)    │
                  │                             │
                  │   AST walk:                 │
                  │   - generateBlock           │
                  │   - generateExpr            │
                  │   - generateDecl            │
                  │   - generateInterfaceAnd…   │
                  │                             │
                  │   Shared rendering rules:   │
                  │   - user-func emit shape    │
                  │   - struct emit shape       │
                  │   - shader-map bookkeeping  │
                  │   - linkShaderObjects       │
                  └──────────────┬──────────────┘
                                 │ owns Target *
                                 ▼
                  ┌─────────────────────────────┐
                  │          Target             │
                  │   (abstract, in Target.h)   │
                  │                             │
                  │   Hooks:                    │
                  │   - typeName(Type, ptr)     │
                  │   - attributeName(name,idx) │
                  │   - renameBuiltin(name)     │
                  │   - emitTextureSample(...)  │
                  │   - emitTextureRead(...)    │
                  │   - emitTextureWrite(...)   │
                  │   - discardStatement()      │
                  │   - emitResourceBinding(…)  │
                  │   - emitShaderEntry(…)      │
                  │   - shaderFileExt(stage)    │
                  │   - compileShader(…)        │
                  │   - compileShaderRuntime(…) │
                  │   - supportsStage(stage)    │
                  └──────────────┬──────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              ▼                  ▼                  ▼
     ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
     │   HLSLTarget    │ │   MSLTarget     │ │   GLSLTarget    │
     │ HLSLTarget.cpp  │ │ MSLTarget.cpp   │ │ GLSLTarget.cpp  │
     └─────────────────┘ └─────────────────┘ └─────────────────┘
```

The subclass interface is **only** the `Target` abstract class. `CodeGen`
itself becomes concrete and final.

## Why composition over inheritance

Three concrete reasons:

1. **The AST walk doesn't vary per target.** Every backend visits the same
   nodes in the same order. Inheriting and overriding `generateExpr` /
   `generateDecl` lets each subclass diverge silently mid-walk; expressing
   the walk once in `CodeGen` and consulting `Target` at the small
   decision points makes the divergence enumerable.
2. **Cross-target invariants are testable.** A `Target` hook like
   `attributeName(name, idx)` is a pure string-returning function that can
   be unit-tested. Today's `writeAttribute` is buried inside an output
   stream and tested only via end-to-end compile.
3. **A new target costs less.** Adding (say) a WGSL backend under the
   inheritance model means a fourth ~1000-LoC file. Under the composition
   model, it's a `WGSLTarget.cpp` that fills in ~15 hooks.

## Phasing principles

Each phase below is a **thin slice**:

- Compiles cleanly after the phase.
- `ctest` passes (no behavior change unless explicitly noted).
- Reviewable in one sitting.
- Independently revertable — if phase N is a mistake, phases 0…N-1 stand
  on their own.
- Inverts no observable behavior — every phase before phase 10 leaves the
  three `*CodeGen` classes alive and producing identical output to today.

The plan is **additive until phase 10**. Hooks get added to `Target` and
called from the existing subclasses; the subclasses keep working until
they're collapsed at the end. There is no flag-day cutover.

---

## Phase 0 — Scaffold the `Target` boundary

**Goal.** Introduce `Target.h` and three empty `Target` subclasses, wire
each existing `*CodeGen` to own a corresponding `Target *`. No behavior
moves yet.

**Files:**

- New: `gte/omegasl/src/Target.h` — abstract base class skeleton.
- New: `gte/omegasl/src/HLSLTarget.cpp` — `HLSLTarget : Target` empty stub.
- New: `gte/omegasl/src/MSLTarget.cpp` — `MSLTarget : Target` empty stub.
- New: `gte/omegasl/src/GLSLTarget.cpp` — `GLSLTarget : Target` empty stub.
- Edit: `gte/omegasl/src/CodeGen.h` — add `protected: std::unique_ptr<Target> target;` on `CodeGen` and a constructor parameter.
- Edit: `gte/omegasl/src/HLSLCodeGen.cpp` / `MetalCodeGen.cpp` / `GLSLCodeGen.cpp` — pass a freshly-constructed `*Target` instance to the base `CodeGen` constructor.
- Edit: `gte/omegasl/CMakeLists.txt` — add the three new `.cpp` files to the omegaslc target.

**`Target.h` skeleton:**

```cpp
namespace omegasl {
    struct Target {
        enum Kind { HLSL, MSL, GLSL };
        Kind kind() const { return _kind; }
        virtual ~Target() = default;
    protected:
        explicit Target(Kind k) : _kind(k) {}
    private:
        Kind _kind;
    };
}
```

**Exit criteria.**

- Build green on all three backends.
- `ctest` green.
- `git grep "class HLSLCodeGen" gte/omegasl/src/HLSLCodeGen.cpp` shows the
  class still owns its existing methods; only the constructor changes.

**Rollback.** Remove the three new `.cpp` files and revert the
`CodeGen` constructor signature. No call-site changes outside the three
codegens.

---

## Phase 1 — Move type-name mapping

**Goal.** Move `writeTypeExpr` (the `float` → `float`/`vec`/`float`
mapping) to `Target::typeName`.

**Files:**

- Edit: `Target.h` — declare `virtual void writeTypeName(ast::Type *t, bool pointer, std::ostream &out) = 0;`.
- Edit: `HLSLTarget.cpp` / `MSLTarget.cpp` / `GLSLTarget.cpp` — implement, copy-pasted from each codegen's existing `writeTypeExpr` body.
- Edit: each `*CodeGen.cpp` — replace the body of `writeTypeExpr` with `target->writeTypeName(typeResolver->resolveTypeWithExpr(typeExpr), typeExpr->pointer, out);`.

**Why this hook first.** It's the most-called target-specific decision (~30
sites per backend) and the cleanest in shape — pure type → string. If the
abstraction is wrong, we find out with the smallest possible blast radius.

**Exit criteria.**

- Generated source identical byte-for-byte to before. Verify with a
  scripted re-run of the test corpus and a `diff` against archived output.
- All three `writeTypeExpr` private members reduced to a 1-line
  delegation.

**Rollback.** Inline the `Target::writeTypeName` implementation back into
each codegen's `writeTypeExpr`.

---

## Phase 2 — Move attribute-name mapping

**Goal.** Move `writeAttribute` / `writeAttributeName` to
`Target::writeAttribute`.

**Files:**

- Edit: `Target.h` — declare `virtual void writeAttribute(OmegaCommon::StrRef name, std::optional<unsigned> index, std::ostream &out) = 0;`.
- Edit: `HLSLTarget.cpp` — copy the body of HLSL's `writeAttribute`.
- Edit: `MSLTarget.cpp` — copy MSL's `writeAttributeName`.
- Edit: `GLSLTarget.cpp` — fill in for the GLSL builtin globals
  (`gl_VertexIndex`, `gl_FrontFacing`, etc.). The existing GLSL code
  emits these inline in the SHADER_DECL handler; this phase consolidates
  them.
- Edit: each `*CodeGen.cpp` — call `target->writeAttribute(...)` at every
  current call site.

**Exit criteria.**

- The §1.2/§1.3/§1.4 fragment-output / fragment-input attributes (already
  landed in `OmegaSL-Feature-Gap-Survey.md`) flow through cleanly.
- HLSL's `writeAttribute(name, index, out, attributeIndex)` overload —
  the one that takes the optional `Color(N)` index — is the canonical
  shape on `Target`; HLSL/MSL/GLSL all accept it.

**Rollback.** Inline back into each codegen.

---

## Phase 3 — Move builtin renaming

**Goal.** Replace the inline `BUILTIN_LERP` → `mix`, `BUILTIN_FRAC` →
`fract`, `BUILTIN_ATAN2` → `atan` mapping with `Target::renameBuiltin`.

**Files:**

- Edit: `Target.h` — `virtual OmegaCommon::StrRef renameBuiltin(OmegaCommon::StrRef name) = 0;` (returns the input unchanged when the target has no remap).
- Edit: each `*Target.cpp` — implement the per-backend remap table.
- Edit: each `*CodeGen.cpp` `CALL_EXPR` arm — drop the inline
  `if(_id == BUILTIN_LERP) ... else if(_id == BUILTIN_FRAC) ...` ladder
  and emit `target->renameBuiltin(_id)` instead.

**Why now.** Hooks 1+2+3 form the "small string-returning hook" cluster.
Together they cover ~80% of the per-backend divergence in
`generateExpr`. After phase 3, the three backends' `generateExpr` bodies
should be visibly converging.

**Exit criteria.**

- Existing tests pass.
- `git diff` between the three `*CodeGen.cpp` `CALL_EXPR` arms is
  noticeably shorter than before.

**Rollback.** Same shape as phases 1–2.

---

## Phase 4 — Move texture op emission

**Goal.** Move the `BUILTIN_SAMPLE` / `BUILTIN_READ` / `BUILTIN_WRITE`
emission to `Target::emitTextureSample` / `emitTextureRead` /
`emitTextureWrite`.

**Files:**

- Edit: `Target.h` — three hooks taking `(CodeGen &cg, ast::CallExpr *expr, std::ostream &out)`. The `CodeGen &` lets the target call back into the shared expression dispatcher for argument expressions.
- Edit: each `*Target.cpp` — implement, copy from the existing codegens.
  - HLSL: `tex.Sample(s, c)` / `tex.Load(intN+1)` / `tex[uintN] = v`.
  - MSL: `tex.sample(s, c)` / `tex.read(uintN c)` / `tex.write(v, uintN c)`.
  - GLSL: `texture(samplerND(t,s), c)` / `texelFetch(...)` /
    `imageStore(...)`. The latter two need the dimensionality lookup
    (currently hardcoded to `ivec2` — `OmegaSL-Reference.md` bug 5).
    Fix incidentally during this phase or call it out as a follow-up.

**Why.** The texture-op shape is the second-largest source of duplication
and divergence; together with phase 3 it accounts for almost all of
`CALL_EXPR`'s body.

**Subtlety.** The Metal coord-cast helper
(`metalUintCoordTypeForTexture` per
`OmegaSL-Reference.md` bug 2's fix) and the latent HLSL `BUILTIN_WRITE`
coord-cast (bug 4) live in the per-target implementation. Phase 4 is the
right time to address bug 4 since the cast logic is right there.

**Exit criteria.**

- Tests pass.
- `texture_write.omegasl`, `gaussian_blur.omegasl`, and any other
  texture-touching tests still produce the same compiled output.
- After this phase, `generateExpr`'s `CALL_EXPR` arm in each `*CodeGen`
  is small enough that the three are *visibly identical* modulo a few
  `target->...` calls.

---

## Phase 5 — Move statement-level target hooks

**Goal.** Move the small per-statement divergences: the `discard`
keyword, the cast syntax, the address-of/dereference syntax (Metal +
HLSL allow `*` and `&`; GLSL doesn't have raw pointers).

**Files:**

- Edit: `Target.h` — hooks:
  - `virtual OmegaCommon::StrRef discardStatement() = 0;`
    (`"discard"` for HLSL/GLSL, `"discard_fragment()"` for MSL).
  - `virtual void writeCast(ast::TypeExpr *target, std::ostream &out) = 0;`
    (today: each backend writes its own type name; identical to phase 1).
  - `virtual bool supportsPointerExpr() const = 0;` — true for HLSL/MSL,
    false for GLSL (which currently emits raw `&`/`*` but the result is
    invalid GLSL — harmless because nothing uses it yet but worth
    pinning).
- Edit: each `*Target.cpp` — implement.
- Edit: each `*CodeGen.cpp` — call.

**Exit criteria.** Tests pass. The `DISCARD_STMT` arm is now identical
across the three codegens.

---

## Phase 6 — Promote `generateExpr` and `generateBlock` to `CodeGen`

**Status: deferred until after Phase 8.** When this phase was originally
planned the assumption was that phases 1–5 would absorb every divergence
between the three `generateExpr` / `generateBlock` bodies. That turned
out to be optimistic — survey found additional divergences (BINARY_EXPR
parens, ID_EXPR identifier escaping, `MAKE_*` constructor builtins,
user-function mangling, ARRAY_EXPR separator, MEMBER_EXPR fragment-output
rerouting in GLSL, generateBlock indent and stmt-suffix placement).
Phase 7.5 (formatter unification) below addresses most of these. The
final blocker for Phase 6 promotion is the GLSL fragment-output
`internalStructVarMap` rerouting in MEMBER_EXPR, whose state is
populated by ~15 sites in GLSLCodeGen during SHADER_DECL processing.
Those populator sites move to `GLSLTarget` as part of Phase 8 (shader
entry); after that the MEMBER_EXPR override on `GLSLTarget` can read
that state, and Phase 6's promotion to non-virtual on `CodeGen`
becomes unblocked.

**Goal.** After phases 1–5, 7, 7.5, and 8, the three `generateExpr` and
`generateBlock` bodies should be byte-for-byte identical to within
`target->...` calls. Move them to non-virtual `CodeGen` methods.

**Files:**

- Edit: `CodeGen.h` — `void generateExpr(ast::Expr *)` and
  `void generateBlock(ast::Block &)` become non-virtual concrete methods
  on `CodeGen`. Same body for everyone.
- Edit: each `*CodeGen.cpp` — delete the (now redundant) `generateExpr`
  and `generateBlock` overrides.

**Why this is its own phase.** It's the first phase that *removes* code
rather than relocating it.

**Exit criteria.**

- ~150 LoC deleted from each `*CodeGen.cpp`.
- Tests pass.
- The `*CodeGen` classes still own `generateDecl` (for SHADER_DECL
  divergence), the file-output state, and the per-stage compile
  invocation.

---

## Phase 7.5 — Formatter unification (added mid-refactor)

Inserted between phases 7 and 8 to unblock Phase 6's eventual
promotion. Goal: unify cosmetic formatting across backends so
`generateExpr` / `generateBlock` bodies converge, with genuinely
per-platform behavior moved to Target hooks.

**What landed:**

- Universal canonical form: `(lhs op rhs)` for binary expressions,
  `, ` separators in CALL_EXPR / ARRAY_EXPR, 4-space indent in
  `generateBlock`. HLSL adds parens; MSL/GLSL switch indent. GLSL
  drops the dead `finishEarly` flag. HLSL drops `<< std::flush` in
  ID_EXPR.
- `userFuncDecls` / `userFuncNames` / `mangleUserFuncName` /
  `isUserFunc` move from MSL-only state onto shared `CodeGen`. All
  three backends populate `userFuncNames` in their `FUNC_DECL` arm.
- New `Target::needsMangling(StrRef name) const` hook (default
  `false`). Each target ships a curated stdlib collision set;
  `CodeGen::spellUserFuncName` mangles only when the target reports
  a real collision. Output stays clean for non-colliding user names
  on every backend (e.g. MSL no longer prefixes `lambert_diffuse`,
  but HLSL/MSL still prefix user `saturate` because it shadows the
  stdlib).
- `Target::renameBuiltin` extended to map
  `BUILTIN_MAKE_FLOAT2/3/4`, `MAKE_INT2/3/4`, `MAKE_UINT2/3/4`,
  `MAKE_FLOATNxN` to per-backend constructor names
  (`floatN`/`intN`/`uintN`/`floatNxN` for HLSL/MSL,
  `vecN`/`ivecN`/`uvecN`/`matN` for GLSL). The `~18` inline
  constructor arms in each backend's CALL_EXPR collapse.
- New `Target::writeIdentifier(StrRef, ostream&)` hook. Default:
  write raw. `GLSLTarget` overrides with the existing
  `writeGLSLIdent` reserved-word escape (`input`, `output`,
  `shared`, ...). All three `ID_EXPR` arms call
  `target->writeIdentifier(_expr->id, shaderOut)`.
- New `Target::emitMemberExpr(CodeGen&, ast::MemberExpr*, ostream&)`
  hook with a default base-class definition that emits `lhs.rhs`.
  HLSL and MSL switch their MEMBER_EXPR arm to
  `target->emitMemberExpr(...)`.

**Deferred to Phase 8:** the GLSL `MEMBER_EXPR` override that
consults `internalStructVarMap` for fragment-output struct
rerouting. Its state lives in `GLSLCodeGen` and is populated by
~15 sites during SHADER_DECL processing — those move to
`GLSLTarget` along with the shader-entry-header emission in
Phase 8, at which point the override naturally has the state it
needs. GLSL's `MEMBER_EXPR` arm keeps its inline rerouting until
then.

**Exit criteria met.**

- Build green; 28/28 ctest pass.
- Output across the three backends shares 4-space indent, parens,
  separator style, and conditional-mangle behavior.
- `generateExpr` bodies for HLSL/MSL are byte-identical to within
  `target->...` calls. GLSL still differs in `MEMBER_EXPR` (Phase
  8 work) and `generateBlock` (small structural deltas — also
  cleared up in Phase 8 along with the shader-entry brace
  handling).

---

## Phase 7 — Move resource-binding emission

**Goal.** Move the per-resource-decl boilerplate that varies per backend:

- HLSL: `StructuredBuffer<T> name : register(tN, space0)`.
- MSL: `constant T *name [[buffer(N)]]` / `texture2d<float, ...> [[texture(N)]]`.
- GLSL: `layout(std430, set=, binding=) buffer Name_Layout { ... };` /
  `layout(set=, binding=) uniform texture2D name;`.

This is the largest remaining divergent block — currently ~150 LoC per
backend inside `SHADER_DECL`.

**Files:**

- Edit: `Target.h` — `virtual void emitResourceBinding(ast::ResourceDecl *res, ast::ShaderDecl::ResourceMapDesc::Access access, unsigned slot, std::ostream &out, omegasl_shader_layout_desc &layout) = 0;`. The `layout` reference is filled in so the shared `CodeGen` can build up the `omegasl_shader_layout_desc` array uniformly.
- Edit: each `*Target.cpp` — implement, copying from the SHADER_DECL
  resource loops.
- Edit: each `*CodeGen.cpp` `SHADER_DECL` — replace the inline resource
  loop with a single shared loop in `CodeGen` that calls
  `target->emitResourceBinding`.

**Subtlety.** Slot allocation differs: HLSL uses three counters
(`t_resource_count`, `u_resource_count`, `s_resource_count`), MSL uses
three (`bufferCount`, `textureCount`, `samplerCount`), GLSL uses one
(`binding`). Move the counter ownership into the `Target` (each target
keeps its own counters as members) so the shared code doesn't have to
know.

**Exit criteria.**

- Tests pass.
- `SHADER_DECL` in each `*CodeGen` is materially shorter.
- The static-sampler emission for MSL (which interleaves `staticSamplers`
  vector building during the resource loop) is also captured by
  `Target::emitResourceBinding` returning whatever string the shared
  `SHADER_DECL` emits later.

---

## Phase 8 — Move shader-entry signature emission

**Status: split into 8a / 8b / 8c / 8d.** The original plan envisioned
this as a single hook + one diff per backend. In practice the SHADER_DECL
flow is so different across backends that doing it as one phase means a
multi-thousand-line diff. Splitting it lets each sub-step land green.

### Phase 8a — GLSL fragment-output state migration ✅

Move `internalStructs`, `internalStructVarMap`, `structDeclMap`,
`currentShaderType`, `activeReturnReplacement`, `fragmentOutputStruct`,
and `writeInternalFieldRef` from `GLSLCodeGen` to `GLSLTarget`.
`GLSLCodeGen` accesses them via a typed `glslTarget()` accessor that
downcasts the base `target` once. Implement `GLSLTarget::emitMemberExpr`
override that consults `internalStructVarMap` for fragment-output
struct rerouting, then switch GLSL's MEMBER_EXPR arm to call
`target->emitMemberExpr(...)`. After this, GLSL's `MEMBER_EXPR`
behavior matches the abstract Target hook — closing the deferral from
Phase 7.5.

**Verification:** byte-identical output across all three backends;
28/28 ctest pass.

### Phase 8b — HLSL `emitShaderEntryHeader` hook ✅

Add `Target::emitShaderEntryHeader(CodeGen&, ast::ShaderDecl*, omegasl_shader&, std::ostream&)`
as a non-pure virtual with a no-op default. `HLSLTarget` overrides with
the full HLSL header emission (stage decorators including
`[numthreads]` / `[domain]` / `[partitioning]` / `[outputtopology]` /
`[outputcontrolpoints]`, return type, name, parameter list with
attributes, fragment `: SV_TARGET` suffix). HLSLCodeGen's SHADER_DECL
calls `target->emitShaderEntryHeader(...)` then `generateBlock(...)`;
its inline header emission is gone (~80 LoC removed).

MSL and GLSL keep their inline SHADER_DECL header emission for now
because:
- MSL's header interleaves with resource binding (resources go inside
  the function parameter list) and closes with a manual `){` that the
  body block doesn't write — needs `shaderDecl` flag retired first
  (Phase 8d).
- GLSL's header has fragment-output struct decls, per-param `layout(location=N) in` lines,
  and an attribute-bridge `extra_stmts` accumulator that gets flushed
  inside `void main(){}`. Plus a custom RETURN_DECL body loop. Move
  scheduled for Phase 8c.

**Verification:** byte-identical output; 28/28 ctest pass.
HLSLCodeGen.cpp shrinks from ~559 LoC to ~385.

### Phase 6 partial — promote `generateExpr` to non-virtual `CodeGen` ✅

Inserted between Phase 8a and 8c since 8a closed the last GLSL
divergence in `generateExpr`. Mechanics:

- `generateExpr` becomes a concrete method on `CodeGen` whose body
  lives in `CodeGen.cpp`. Stream access is via a virtual
  `shaderOutStream()` accessor that each subclass implements (returning
  its own `shaderOut` reference). Stream redirection up to `CodeGen`
  itself is deferred until Phase 10.
- `formatFloatLit` (previously duplicated as a file-static in each
  `*CodeGen.cpp`) moves to `CodeGen.cpp` as well.
- Each `*CodeGen.cpp` deletes its `generateExpr` override (~115 LoC ×
  3) and adds a one-line `shaderOutStream()` accessor.

`generateBlock` is still per-backend. MSL has the `shaderDecl` flag for
shader-entry brace handling, and GLSL has the bitmask `(stmt->type & DECL) == EXPR`
plus a different newline placement. Both clear up in Phase 8c/8d. Until
then the bodies aren't byte-identical.

**Verification:** byte-identical output; 28/28 ctest pass.

### Phase 8c — GLSL header + body migration ✅

Moved GLSL's full SHADER_DECL header+body emission to
`GLSLTarget::emitShaderEntryHeader` and `GLSLTarget::emitShaderEntryBody`
overrides. Includes:

- Stage-keyword + tessellation `layout(...)` decorators.
- All-used-structs loop with internal-struct varying decls (skipping
  fragment-output struct, which is emitted separately).
- Fragment-output struct decls (`layout(location=N) out vec4 _outColorN`
  per `Color(N)` field, or default `_outColor` for bare-float4 fragments).
- Resource bindings via the new `cg.emitResourcesAndFillLayout` helper
  that runs `target->emitResourceBinding` per resource, accumulates
  the layout vector, and fills `meta.pLayout`.
- Per-param emission: `layout(location=N) in <type>` for non-attributed
  params; attribute-bridge locals (e.g. `vec3 N = gl_VertexIndex;`)
  gathered into the new `extra_stmts` member of `GLSLTarget` for flush
  at the top of the body.
- `void main(){`.
- Custom body loop with RETURN_DECL rerouting for fragment-output struct
  returns (bare `return;` since per-field stores happened earlier via
  member-expr routing) and hull/domain `gl_Position = ...` writes.

Additional state migrated to `GLSLTarget`: `generatedStructs` (the
struct-text cache populated when STRUCT_DECL is parsed), `extra_stmts`
(the attribute-bridge accumulator). GLSLCodeGen accesses the populator
via the typed `glslTarget()` accessor.

`indentLevel` moved from each `*CodeGen` to shared `CodeGen` so the
target's body emission can bump it for nested blocks (which still go
through each subclass's `generateBlock`).

The Phase 7.5 indent-inconsistency in the SHADER_DECL custom body loop
(was 2 spaces while non-entry bodies had been unified to 4) is fixed in
this phase — entry bodies now indent at 4 spaces too.

**Verification:** Build green; 28/28 ctest pass. Byte-output for HLSL
and MSL unchanged. GLSL output indentation in shader-entry bodies
changed 2 spaces → 4 spaces (the Phase 7.5 indent-unification fix).
GLSLCodeGen.cpp dropped from 874 LoC → 508 LoC; GLSLTarget.cpp grew
to 728 LoC.

### Phase 8d — MSL header + body migration ✅

Moved MSL's full SHADER_DECL signature + body emission to
`MSLTarget::emitShaderEntryHeader` and `MSLTarget::emitShaderEntryBody`.
Header writes the stage decorator (`vertex` / `fragment` / `kernel` /
`[[patch(...)]] vertex`), return type, name, and the parameter list
with resources interleaved (resources via `cg.emitResourcesAndFillLayout`
which calls `MSLTarget::emitResourceBinding` per binding and tracks
`paramIndex` for the leading-comma decision). Body writes the opening
`{`, flushes the static-sampler `constexpr sampler ... = sampler(...);`
lines that were gathered during resource emission, walks the user
block at indent+1, and writes `}`.

`MSLTarget::emitStaticPreamble` becomes a no-op — it can't fire
mid-parameter-list (which is where the shared
`emitResourcesAndFillLayout` calls it), so the static-sampler flush
moves into `emitShaderEntryBody` instead. HLSL/GLSL behavior unchanged
(both still no-op there). The `MetalCodeGen::shaderDecl` flag — which
suppressed `generateBlock`'s own `{` because the SHADER_DECL handler
emitted `){` manually — is gone.

`MetalCodeGen::SHADER_DECL` shrinks to: hull/domain rejection
diagnostic, file-output setup, default headers, user-function
prototypes + bodies, used-struct emission, then a single
`emitShaderEntryHeader` + `emitShaderEntryBody` call pair.

**Verification:** Build green; 28/28 ctest pass. Byte-output identical
across all three backends.

### Phase 6 rest — promote `generateBlock` to non-virtual `CodeGen` ✅

Once 8c + 8d landed, all three backends' `generateBlock` bodies
converged. Promoted `generateBlock` to a concrete method on `CodeGen`
whose body lives in `CodeGen.cpp`. Stream access is via
`shaderOutStream()` (same accessor used by `generateExpr`). Each
`*CodeGen.cpp` deletes its `generateBlock` override.

The MSL pre-Phase-8d quirk where `generateBlock` emitted a leading
indent before the opening `{` (producing `if(...)    {`) is dropped:
output now matches the HLSL/GLSL form `if(...){`. Same precedent as
the Phase 7.5 / 8c indent-unification — small cosmetic byte-output
change for MSL only, no semantic change.

**Verification:** Build green; 28/28 ctest pass. HLSL/GLSL output
byte-identical to pre-Phase-6 baseline. MSL output differs only in
the dropped `    ` filler before nested-block `{` (e.g.
`if((x > 10.0))    {` → `if((x > 10.0)){`).

### Phase 8 exit criteria — met:

`*CodeGen.cpp` LoC after Phase 6 rest:

- `MetalCodeGen.cpp`: 342 LoC (was 480 pre-Phase-8d)
- `HLSLCodeGen.cpp`: 359 LoC
- `GLSLCodeGen.cpp`: 486 LoC

Each is now: constructors, `writeTypeExpr`, `generateDecl`,
`shaderOutStream` accessor, `compileShader` / `compileShaderOnRuntime`,
plus the GLSL-specific `glslTarget()` accessor. Phase 10 will collapse
all three into a single `CodeGen.cpp` (the per-backend `generateDecl`
is the last divergent walker; the bulk of the SHADER_DECL handling
already lives on the targets).

---

## Phase 9 — Move file-extension and compile-driver invocation ✅

**What landed.** Four hooks on `Target`:

- `shaderFileExt(stage)` — `".hlsl"` / `".metal"` /
  `".vert"|".frag"|".comp"|".tesc"|".tese"`. GLSL's stage→ext switch
  was previously duplicated in `SHADER_DECL` and `compileShader`;
  consolidated to a single source of truth on the target. The
  `glslc -fshader-stage=` argument derives the stage tag from the
  ext (`".vert"` → `"vert"`).
- `compileShader(stage, name, srcDir, outDir)` — invokes `dxc` /
  `metal` / `glslc` on the file at `<srcDir>/<name><shaderFileExt>`.
- `compileShaderRuntime(stage, name, source, meta)` — in-process
  compile via `D3DCompile` / `compileMTLShader` / `shaderc`. The
  source is now passed by reference; the target updates `meta.data`
  / `meta.dataSize` directly. No more reaching back into the
  codegen's `stringOut`.
- `supportsStage(stage, diagnosticOut)` — friendly stage gate. The
  Metal hull/domain rejection (OmegaSL-Reference.md bug 3) now lives
  on `MSLTarget::supportsStage`. Each `*CodeGen` `SHADER_DECL`
  handler queries it before opening the source file; on refusal it
  prints the captured diagnostic and sets `hasFatalErrors`.

**State migration.** `MSLTarget`, `HLSLTarget`, and `GLSLTarget` each
take their respective `*CodeOpts &` at construction (storing a
reference). The shaderc compiler handle (`shaderc_compiler_t`)
moves from `GLSLCodeGen` onto `GLSLTarget` so the runtime-compile
hook owns its own context.

**`*CodeGen.cpp` reductions.**

- `MetalCodeGen.cpp`: 342 → **300** LoC (hull/domain rejection +
  metal-cmd invocation + Metal-API runtime call all moved out).
- `HLSLCodeGen.cpp`: 359 → **297** LoC (dxc invocation + D3DCompile
  moved out).
- `GLSLCodeGen.cpp`: 486 → **327** LoC (glslc invocation +
  shaderc-compile moved out, plus the redundant stage→ext switch is
  gone).

**Exit criteria — met:**

- Build green; 28/28 ctest pass.
- `compileShader` / `compileShaderOnRuntime` on each `*CodeGen`
  collapsed to 1–3-line delegations into `target->compileShader` /
  `target->compileShaderRuntime`.
- HLSL/GLSL output byte-identical to pre-Phase-9 baseline. MSL
  output unchanged from post-Phase-6 (no new byte changes from
  Phase 9 itself).

---

## Phase 10 — Collapse the three `*CodeGen` subclasses

**Goal.** Delete `HLSLCodeGen`, `MetalCodeGen`, `GLSLCodeGen` classes.
`CodeGen` becomes concrete and final, takes a `std::unique_ptr<Target>`
in its constructor.

**Files:**

- Delete: `gte/omegasl/src/HLSLCodeGen.cpp`,
  `gte/omegasl/src/MetalCodeGen.cpp`,
  `gte/omegasl/src/GLSLCodeGen.cpp`.
- Edit: `CodeGen.h` — remove `virtual` from `~CodeGen`, `generateDecl`,
  `compileShader`, `compileShaderOnRuntime`. Remove the
  `*CodeGenMake` / `*CodeGenMakeRuntime` factory function declarations.
  Add a single `CodeGen::Create(CodeGenOpts &, std::unique_ptr<Target>)`
  / `CodeGen::CreateRuntime(CodeGenOpts &, std::unique_ptr<Target>, std::ostringstream &)`
  factory.
- Edit: `gte/omegasl/src/main.cpp` — replace the three
  `*CodeGenMake(...)` call sites with
  `CodeGen::Create(opts, std::make_unique<HLSLTarget>(hlslOpts))` etc.
- Edit: `CMakeLists.txt` — drop the three deleted files, keep the three
  `*Target.cpp` files.

**Exit criteria.**

- The omegasl backend is one `CodeGen.cpp` plus three `*Target.cpp` files.
- `git ls-files gte/omegasl/src/*CodeGen.cpp` returns just `CodeGen.cpp`.
- No virtuals remain on `CodeGen`. The only virtuals in the codegen
  layer are on `Target`.
- All tests still pass on every backend the CI runs.

**Rollback.** Phase 10 is the only irreversible-ish step (you'd have to
re-create the three subclasses to revert). Phases 0–9 are individually
revertable; if phase 10 turns up trouble, leave the three subclass
constructor-only files in place as thin shims and revisit later.

---

## Cross-cutting notes

### What stays virtual on `Target`

- `writeTypeName`, `writeAttribute`, `renameBuiltin` — small, pure.
- `emitTextureSample/Read/Write` — non-trivial but have a clean signature
  that takes the `CodeGen &` for callbacks.
- `emitResourceBinding`, `emitShaderEntryHeader/Footer` — state-bearing
  but encapsulate the divergence cleanly.
- `discardStatement`, `shaderFileExt` — small string returns.
- `compileShader`, `compileShaderRuntime` — the only I/O-heavy hooks;
  reasonable to keep virtual since they invoke `dxc` / `metal` /
  `shaderc` and that's per-target by definition.
- `supportsStage` — gives the target a clean way to refuse a stage with
  a friendly diagnostic (Metal hull/domain, future GLES limitations).

### What does NOT go on `Target`

- AST visitor shape (`generateExpr` / `generateDecl` / `generateBlock`).
  The walk is identical everywhere; if a future feature needs per-target
  walk reordering, that's a sign the feature should be expressed
  differently.
- `omegasl_shader_layout_desc` accumulation. The shared `CodeGen` builds
  up the `pLayout` array; targets only fill in per-binding fields via
  `emitResourceBinding`'s out-parameter.
- Shader-map bookkeeping, library linking, file output. All shared.
- User-function emission. The protoype/body shape is identical; only the
  *body's* `generateBlock` content differs (which is captured already).

### Testing strategy throughout

- After every phase, the existing `ctest` corpus must pass.
- Add a "byte-for-byte" archive of generated source for a representative
  shader (e.g. `shaders.omegasl`) before phase 0; diff against it after
  every phase. If the diff is non-empty before phase 10, something's off.
- Phase 10 is the only phase where the public `CodeGen.h` API changes;
  add a one-line note in the migration section.

### Risk register

- **Risk: phase 1's `Target::writeTypeName` signature is too narrow.**
  Mitigation: ship phase 1 with all three targets implementing it before
  declaring the hook stable. If a fourth target later needs more
  context, widen the signature; subclasses are few and easy to update.
- **Risk: phase 7 (resource binding) requires the targets to share state
  about resource counters.** Mitigation: counter ownership lives entirely
  inside the `Target` instance; `CodeGen` calls
  `target->emitResourceBinding` once per resource and the target owns its
  own `bufferCount` / `textureCount` / etc. Reset between shaders via
  a `Target::resetForNextShader()` hook added in phase 7.
- **Risk: HLSL/MSL/GLSL CALL_EXPR arms diverge in subtle ways
  (e.g. `mul()` for HLSL matrix×vector multiplication) that aren't
  captured by phases 3–4.** Mitigation: enumerate every divergent
  CALL_EXPR pattern as part of phase 3's prep; if a pattern resists
  expression as `renameBuiltin`, add a dedicated `Target` hook for it
  and document.
- **Risk: existing in-flight feature work (e.g. survey §1.5
  early-depth, §1.6 interpolation modifiers) lands during the refactor
  and re-introduces per-codegen drift.** Mitigation: gate the refactor
  branches against a clean main, and rebase incrementally. Each phase
  is small enough that a rebase is mechanical.

### Out of scope for this plan

- **Sema and Parser changes.** Untouched. The refactor is purely
  code-generation-side.
- **`InterfaceGen`** (currently commented out in `CodeGen.h:203–259`).
  If/when it's revived, it slots cleanly into the same model — it's a
  fourth `Target`-like layer for cross-target C++ headers.
- **Performance.** None of these phases should change runtime
  performance of `omegaslc`. If profiling shows a regression after phase
  10 (e.g. `target->...` virtual dispatch overhead in tight loops),
  revisit by inlining the hot hooks.

---

## Sequencing summary

| Phase | What lands | Net LoC change | Reversible? |
|-------|------------|----------------|-------------|
| 0     | Target.h scaffold, three empty Target stubs, wired in | +50 | yes |
| 1     | `Target::writeTypeName` | -100 (dedup) | yes |
| 2     | `Target::writeAttribute` | -120 | yes |
| 3     | `Target::renameBuiltin` | -60 | yes |
| 4     | `Target::emitTextureSample/Read/Write` | -180 | yes |
| 5     | `Target::discardStatement` and small statement hooks | -40 | yes |
| 6     | Promote `generateExpr` / `generateBlock` to non-virtual `CodeGen` | -450 | yes |
| 7     | `Target::emitResourceBinding` | -300 | yes |
| 8     | `Target::emitShaderEntryHeader/Footer` | -250 | yes |
| 9     | `Target::compileShader` / `shaderFileExt` | -80 | yes |
| 10    | Collapse three `*CodeGen` subclasses | delete 3 files (-2900); add `CodeGen.cpp` (+800); net **~ -2100** | with effort |

Total: roughly **-2,000 LoC** in `gte/omegasl/src/`, with the per-feature
work (every survey §1.x / §2.x / §5.x patch) becoming cheaper to land
forever after.

## Definition of done

- One concrete `CodeGen` class with no virtuals.
- Three `*Target.cpp` implementations of one `Target` abstract class.
- `ctest` green on all three backends.
- `git diff --stat main..refactor-target` shows a net code reduction in
  `gte/omegasl/src/`.
- A new feature added to the language costs one edit to `CodeGen.cpp`
  and N target-specific hooks rather than three parallel edits across
  three subclass files.
