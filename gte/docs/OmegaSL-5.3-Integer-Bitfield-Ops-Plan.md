# OmegaSL §5.3 — Integer / Bitfield Ops Implementation Plan

Implements the §5.3 row of `OmegaSL-Feature-Gap-Survey.md`:
`countbits`, `firstbitlow`, `firstbithigh`, `reversebits`,
`bitfieldExtract`, `bitfieldInsert`. Needed for hashing, compression
formats, occupancy masks, and Morton/bit-packing on the GPU.

This plan follows the machinery established by §5.1 (core math) and §5.2
(vector math): builtins registered in `AST.def` / `AST.h` / `AST.cpp`,
type-checked through the string-matched math dispatch in
`Sema::performSemForExpr`, and emitted via `Target::renameBuiltin` (simple
spelling swaps) or `Target::tryEmitBuiltinCall` + a shared
`CodeGen::emit*` helper (call-shape rewrites, with `queuePendingStatement`
statement injection where a single expression can't express the lowering).

## Locked design decisions (per Alex)

1. **Operand domain: `int` *and* `uint`** (and their vector forms). Signed
   forms are in scope, which means signed `bitfieldExtract` sign-extends
   and the firstbit ops accept signed input.
2. **Scalars *and* vectors** (`intN` / `uintN`, N∈{2,3,4}). HLSL has no
   vector overload of `countbits` / `firstbit*` / `reversebits`, so the
   HLSL backend expands those component-wise.
3. **`firstbithigh` / `firstbitlow` fully normalized** across backends —
   identical result for *every* input, including 0. See the contract
   below; this requires per-backend index conversion and a zero-input
   branch.

## Why this is more than a rename pass

| Op | HLSL | MSL | GLSL | Lowering needed |
|----|------|-----|------|-----------------|
| `countbits`    | `countbits` (scalar uint only) | `popcount` (scalar+vec) | `bitCount` (returns *signed* `int`/`ivec`) | HLSL: per-component on vectors. GLSL: cast result to match operand signedness. |
| `reversebits`  | `reversebits` (scalar uint only) | `reverse_bits` (scalar+vec) | `bitfieldReverse` (scalar+vec) | HLSL: per-component on vectors. |
| `firstbitlow`  | `firstbitlow` → bit *index*, `0xFFFFFFFF` on 0 | `ctz` → trailing-zero *count*, bitwidth on 0 | `findLSB` → bit *index*, `-1` on 0 | All: normalize to the contract below. MSL `ctz` already equals the index for non-zero. |
| `firstbithigh` | `firstbithigh` → for **uint** = MSB index; for **int** scans for the first bit ≠ sign bit (≠ MSB index) | `clz` → leading-zero *count* (index = bitwidth-1-clz) | `findMSB` → MSB index (int: highest non-sign bit), `-1` on 0 | All: normalize. MSL: `31 - clz`. HLSL signed firstbithigh semantics differ from index — verify and adjust. |
| `bitfieldExtract` | **no named intrinsic** — manual shift+mask (sign-extend for int) | `extract_bits(x,off,cnt)` (native) | `bitfieldExtract` (native; sign-extends int, zero-extends uint) | HLSL: emit manual lowering. |
| `bitfieldInsert` | **no named intrinsic** — manual mask+or | `insert_bits(base,ins,off,cnt)` (native) | `bitfieldInsert` (native) | HLSL: emit manual lowering. |

> The §5.3 survey table lists `bitfieldExtract`/`bitfieldInsert` as
> "yes | yes | yes". That is optimistic for **HLSL** (no such intrinsic;
> it has `firstbit*`/`countbits`/`reversebits` but not bitfield
> extract/insert) and the **MSL** spelling is `extract_bits`/`insert_bits`,
> not `bitfieldExtract`. Same class of doc error as §5.2's "MSL has
> `inverse`" (it doesn't). **Phase C must verify each backend by compiling
> a probe before transcribing the lowering** (`dxc -T cs_6_0`,
> `xcrun metal`, `glslc`).

## Normalized firstbit contract

Both `firstbithigh` and `firstbitlow` return a signed **`int`** (intN for
vector input) giving the **zero-based bit index** of the highest / lowest
set bit, and **`-1` when the operand has no set bits (including 0)**. This
matches GLSL `findMSB`/`findLSB` exactly, so GLSL is the reference and the
other two backends are converted to it:

* **GLSL** — pass through (`findMSB`/`findLSB`), already returns `int`/`-1`.
* **MSL** — `ctz`/`clz` return *counts*, not indices, and return the bit
  width (32) on 0. Lower `firstbitlow(x)` to
  `select(int(ctz(x)), -1, x == 0)` and `firstbithigh(x)` to
  `select(31 - int(clz(x)), -1, x == 0)` (component-wise `select` on
  vectors). 64-bit operands are out of scope (see below), so the `31`
  constant is correct for the in-scope 32-bit `int`/`uint`.
* **HLSL** — `firstbitlow`/`firstbithigh` return `uint` and `0xFFFFFFFF`
  on 0; for **signed** input HLSL's `firstbithigh` scans for the first bit
  differing from the sign bit, which is *not* the MSB index. Lower to: cast
  operand to `uint`, take the native firstbit, then map `0xFFFFFFFF → -1`
  and `cast to int` (`(v == 0xffffffffu) ? -1 : int(v)`). Doing the
  firstbit on the `asuint` bit-pattern makes signed and unsigned agree with
  GLSL's `findMSB(uint)` semantics. Verify against GLSL on a signed probe.

`countbits` returns the operand's own scalar type widened to the result
arity (count per component); `reversebits` returns the operand type.

## Phasing

### Phase A — `countbits`, `reversebits` (1-arg, same-shape result) — **LANDED**

The two ops with no normalization branch — cleanest vertical slice to
confirm the integer-bucket wiring before the harder ops.

**Landed.** Implemented exactly as planned below. Verified: Metal compiles
end-to-end; HLSL emits scalar `(int)countbits(i)` + vector
`uint4(countbits(_bb0.x), …)` with the single-eval temp injected ahead of
use; GLSL emits `uint(bitCount(u))` / `uvec4(bitCount(uv))` +
`bitfieldReverse`. The emitted HLSL **and** GLSL were further compiled by
`dxc -T cs_6_0` and `glslc -fshader-stage=comp` respectively (both present
on this host) — rc=0 for all four shader files. Full omegasl ctest suite
(96 tests) green, including the new `omegasl_compile_bitfield_ops` and
negative `omegasl_invalid_bitfield_ops`.

**Host note:** despite the build-verification memory ("only Metal compiles
on this macOS host"), `dxc` and `glslc` *are* installed at
`/usr/local/bin`, so HLSL/GLSL output can be compile-checked locally — not
only via `-S` source inspection. (D3D12/Vulkan *runtime* still can't run
here; that part of the memory holds.)

* **AST**: `BUILTIN_COUNTBITS`, `BUILTIN_REVERSEBITS` in `AST.def`. **No
  `FuncType` registration** — like `distance`/`inverse`/`length` these are
  matched by *string* in the `func_found == nullptr` math-dispatch block of
  `Sema::performSemForExpr` (confirmed: Sema.cpp ~1268 sets
  `fname = canonicalBuiltinAlias(...)`, then the 1/2/3-arg buckets compare
  `fname == BUILTIN_*`). Add both to the `reserved` set in
  `ast::isReservedBuiltinName` (AST.cpp:634) so a user `func countbits` is
  rejected (the §5.1.0 reservation rule).
* **Sema**: add an **integer-domain 1-arg bucket** to the math dispatch in
  `Sema::performSemForExpr`. Validates the single argument is
  `int`/`uint`/`intN`/`uintN` (reject float/bool/matrix with a `TypeError`)
  and returns the same type (`countbits` per-component count is the same
  arity; result keeps operand signedness for portability — GLSL `bitCount`
  is cast back to the operand type).
* **CodeGen**:
  * MSL `renameBuiltin`: `countbits → popcount`, `reversebits →
    reverse_bits`. GLSL `renameBuiltin`: `countbits → bitCount`,
    `reversebits → bitfieldReverse`; wrap `bitCount`'s `int`/`ivec`
    result in a cast to the operand type when the operand is unsigned.
  * HLSL: scalar passes through; **vector operands** are expanded
    component-wise via a shared `CodeGen::emitComponentwiseUnary` helper
    (`uintN(f(x.x), f(x.y), …)`), invoked from `HLSLTarget::
    tryEmitBuiltinCall`. (New shared helper, modelled on
    `emitVectorCompare`.)
* **Tests**: `bitfield_ops.omegasl` (countbits/reversebits on
  int/uint/int2/uint4), `invalid_bitfield_ops.omegasl` (float arg, bool
  arg, arg count). Register `omegagte_bitfield` shaders in
  `tests/CMakeLists.txt`. Metal end-to-end; HLSL/GLSL via `-S`.

### Phase B — `firstbithigh`, `firstbitlow` (full normalization) — **LANDED**

**Landed.** Normalized contract: signed `int`/`intN`, zero-based index of
the high/low set bit, `-1` when no bit set — identical on all three
backends. Two findings from probing the real toolchains (dxc/glslc/metal,
all installed here) simplified the plan:

* HLSL `firstbithigh`/`firstbitlow` accept scalar, vector, **and signed**
  operands — so, unlike `countbits`, *no* component expansion is needed.
  `(intN)firstbit*(uintN(x))` handles everything, with `0xFFFFFFFF→-1` free
  via the cast.
* The three lowerings were proven host-equivalent for every edge case
  (`0`/`1`/MSB/all-ones) by a throwaway C program *before* transcription,
  then re-verified on real GPU hardware by `gte/tests/bitfield_ops_test.cpp`.

Per backend (see `AST.def` §5.3 comment for the exact forms): GLSL
`findMSB/findLSB(uint(x))` native; HLSL `(intN)firstbit*(uintN(x))`; MSL
`31 - intN(clz(uintN(x)))` and `select(intN(ctz(uintN(x))), -1, x==0)`. A
shared `CodeGen::intOperandShape` classifies operand signedness+arity.

Implemented as planned otherwise:

* **AST/Sema**: `BUILTIN_FIRSTBITHIGH`, `BUILTIN_FIRSTBITLOW`; same integer
  1-arg bucket, but the **return type is `int`/`intN`** regardless of
  operand signedness (the normalized index/`-1` contract).
* **CodeGen**: shared `CodeGen::emitFirstBit(high|low)` helper invoked from
  each backend's `tryEmitBuiltinCall`, emitting the per-backend lowering in
  the contract above. GLSL still routes through a thin path (native, just a
  cast for vectors). Use `queuePendingStatement` only if a single
  expression can't hold the `select`/ternary (scalars fit inline; vectors
  on HLSL need component expansion → likely a queued temp, reusing the
  Phase-A componentwise helper).
* **Tests**: extend `bitfield_ops.omegasl` with firstbit on 0, on a single
  high bit, on a negative `int` (sign-bit case), on vectors. Assert the
  normalized `-1`/index result. GPU runtime check in
  `gte/tests/` (a compute kernel that round-trips known inputs) is the only
  way to actually prove the cross-backend normalization — add one mirroring
  `matrix_ops_test.cpp`.

### Phase C — `bitfieldExtract`, `bitfieldInsert` — **LANDED**

**Landed.** Implemented as planned. The three open verification items from
the plan header all resolved by probing the real toolchains first:

1. HLSL has **no** `bitfieldExtract`/`bitfieldInsert` intrinsic (confirmed:
   dxc errors "use of undeclared identifier"). The doc table's "yes" was
   wrong. → HLSL gets the manual shift+mask lowering.
2. MSL spells them `extract_bits`/`insert_bits` and they're native
   (scalar+vector, signed+unsigned). GLSL `bitfieldExtract`/`Insert` native.
3. The HLSL manual lowering was proven **bit-identical to the GLSL/MSL
   spec across 210 (value, offset, bits) combinations** on the host —
   unsigned zero-extend, signed sign-extend, `bits==0`, and insert masking —
   *before* transcribing it into the backend.

Implemented: `emitHLSLBitfieldExtract`/`emitHLSLBitfieldInsert` shared
helpers on `CodeGen` (single-eval temps via `queuePendingStatement`); GLSL
and MSL emit the native call with offset/bits cast to int/uint respectively.
Sema adds 3-arg / 4-arg integer buckets that validate the operand, the
scalar offset/bits, and (for insert) base==insert type.

**Bug caught by the GPU runtime test (not by `-S`):** a bare `0xFFu` insert
literal emits as `255` (no uint-ness), making MSL's `insert_bits` overload
ambiguous — the runtime Metal compile failed. Fixed by casting the
value/insert operands to their type spelling on both native backends. The
`omegagte_bitfield_ops` GPU test was extended to cover extract (signed +
unsigned) and insert against host references, so the regression is guarded.

Verification: 96-test omegasl ctest suite green; `omegagte_bitfield_ops`
passing on Metal; all six emitted shaders compile under dxc and glslc.

Original plan (all implemented):

* **AST/Sema**: `BUILTIN_BITFIELD_EXTRACT` (3-arg: value, offset, bits),
  `BUILTIN_BITFIELD_INSERT` (4-arg: base, insert, offset, bits). New 3-arg
  and 4-arg integer buckets (offset/bits are scalar `int`/`uint`; value/
  base/insert share the result type). `extract` returns value's type
  (sign-extended for signed); `insert` returns base's type.
* **CodeGen**: GLSL native (`bitfieldExtract`/`bitfieldInsert`). MSL
  `extract_bits`/`insert_bits` via `renameBuiltin` (**verify arg order and
  that signed `extract_bits` sign-extends**). HLSL: shared
  `CodeGen::emitBitfieldExtract`/`Insert` emitting the manual shift+mask
  (extract: `int` → `(x << (32-off-bits)) >> (32-bits)` arithmetic shift
  for sign-extend; `uint` → `(x >> off) & ((1u<<bits)-1)`; insert:
  `(base & ~(mask<<off)) | ((insert & mask) << off)`). Guard `bits == 0`
  (mask `(1u<<32)` is UB) — emit the `bits==0 ? base : …` branch.
* **Tests**: extract/insert on int (sign-extend) and uint, vectors, edge
  offsets/bits (0, full width).

## Cross-cutting

* **No feature bit.** All six are universal on SM5 (HLSL), MSL native, and
  GLSL 4.0 / `ARB_gpu_shader5` (already in the GLSL preamble where the
  swizzle/64-bit work assumes 4.x). They join `discard` / MRT / depth as
  ungated. **64-bit operands** (`countbits(ulong)` etc.) are out of scope —
  they would need the `INT64` gate and the `31`→`63` constant in the
  firstbit lowering; defer until a workload needs them.
* **Reserved names + `osl_user_` prefix** already cover collision safety
  (§5.1.0); just add the six names to `isReservedBuiltinName`.
* **Build verification discipline** (per the repo): only Metal compiles on
  this macOS host — verify MSL end-to-end via the normal compile path and
  the GPU runtime test; verify HLSL/GLSL source via `omegaslc --hlsl -S` /
  `--glsl -S` and, for the lowerings, by hand through `dxc`/`glslc`.

## Open verification items (resolve during implementation, not by assumption)

1. HLSL signed `firstbithigh` exact semantics vs `findMSB(asuint(x))` —
   compile a signed probe and diff.
2. Whether HLSL actually lacks `bitfieldExtract`/`Insert` (doc says yes;
   §5.2 precedent says distrust the doc) and the precise MSL
   `extract_bits`/`insert_bits` signature + sign-extension behavior.
3. GLSL `bitCount` return-type cast: confirm `bitCount(uvec)` returns
   `ivec` and the cast back to `uvec` is well-formed.

## Confirmed edit sites (read from source)

* `omegasl/src/AST.def` — add the six `BUILTIN_*` string macros next to the
  §5.2 block (after line 186).
* `omegasl/src/Sema.cpp` — extend the math dispatch (~1357–1396): add a
  1-arg integer bucket (countbits/reversebits → generic `return
  firstArgType`; firstbit* → custom `int`/`intN` return, modelled on the
  `returnsScalar` and `isVecCompare` special-return paths at ~1546). Add the
  3-arg (`bitfieldExtract`) and 4-arg (`bitfieldInsert`) buckets; 4-arg is a
  *new* `expectedArgs` value. Integer-domain validation mirrors the §5.2
  `isVecCompare` block's `vectorComponentInfo` check.
* `omegasl/src/AST.cpp:634` — add the six names to the `reserved` set.
* `omegasl/src/CodeGen.cpp` — new shared helpers next to `emitVectorCompare`
  (CodeGen.cpp:517) and `emitInverseCall` (447): `emitComponentwiseUnary`
  (HLSL vector expansion), `emitFirstBit`, `emitBitfieldExtract/Insert`.
  Declare them in `CodeGen.h` (~390–400, by the existing two).
* `omegasl/src/MSLTarget.cpp` — `renameBuiltin` (567): `countbits→popcount`,
  `reversebits→reverse_bits`. `tryEmitBuiltinCall` (618): firstbit
  normalization, `extract_bits`/`insert_bits` (verify signed sign-extend).
* `omegasl/src/GLSLTarget.cpp` — `renameBuiltin` (986): `countbits→bitCount`
  (+ signedness cast), `reversebits→bitfieldReverse`. `tryEmitBuiltinCall`
  (1217): firstbit pass-through with vector cast; bitfield ops native.
* `omegasl/src/HLSLTarget.cpp` — `renameBuiltin` (446): scalar passthrough
  for countbits/firstbit*/reversebits. `tryEmitBuiltinCall` (534): vector
  component-expansion for the scalar-only HLSL ops; firstbit normalization;
  manual shift/mask `bitfieldExtract`/`Insert`.
* `omegasl/tests/` + `omegasl/tests/CMakeLists.txt` — `bitfield_ops.omegasl`,
  `invalid_bitfield_ops.omegasl`, and a `gte/tests/` GPU runtime test for
  the firstbit normalization (mirrors `matrix_ops_test.cpp`).

Dispatch reaches the backend hooks via `CodeGen.cpp:239`
(`tryEmitBuiltinCall` → else `renameBuiltin`).
