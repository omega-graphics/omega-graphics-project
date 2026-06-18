==============================
Cross-Backend Emulation
==============================

.. contents:: On this page
   :local:
   :depth: 2

----

What "Emulation" Means Here
----------------------------

OmegaSL presents one shading language that compiles to **HLSL** (Direct3D 12),
**MSL** (Metal), and **GLSL** (Vulkan / SPIR-V). For most constructs the three
targets agree and the compiler emits the obvious thing. But the three
languages often spell the *same operation* differently — or one of them is
missing a convenience intrinsic that the other two provide. When that happens
and the operation is still **semantically expressible on every backend**,
``omegaslc`` bridges the gap at code-generation time by lowering the uniform
OmegaSL surface to a backend-specific equivalent. That lowering is what this
document calls *emulation*.

Emulation is invisible to shader authors. You write one construct; the
compiler emits whatever each target actually requires, chosen so that the
observable result is identical on all three. You never branch on the backend
in shader source.

This is distinct from — and complementary to — the constraints described in
:doc:`Limitations`. That page is about **capability** divergence: features one
backend cannot perform *at all* (no API, no extension, a structurally
different model). Those are not emulated; they are gated behind a runtime
capability check and produce a clean rejection on the platforms that lack
them. This page is about **spelling** divergence: features every backend
*can* perform, just with different syntax. The one-line rule:

    *Emulation is for syntax divergence. Gating is for capability divergence.*

When you read in :doc:`Limitations` that "OmegaGTE does not emulate features
that the underlying API does not provide," that refers to capability
emulation — the engine will not silently turn a missing hardware feature into
a slow CPU fallback. The compile-time spelling lowering described here is a
different mechanism: it never changes the cost class of an operation, only how
it is written for each target.

----

The Decision Rule
------------------

Every cross-backend divergence resolves to exactly one of two outcomes. The
rule the project applies when classifying a feature:

#. If every backend can express the operation **with the same observable
   semantics** after a mechanical rewrite — it is *emulated*. The compiler
   hides the divergence and your code stays portable.
#. If at least one backend **cannot** express the operation at all — it is
   *gated* via a ``#requires(...)`` feature directive and produces a
   header-only stub on the unsupported backend. The loader declines to bind
   such a shader on a device that lacks the feature.
#. If a lowering *could* exist but would **silently** change performance
   characteristics (16-bit math demoted to 32-bit) — it is *gated*, not
   emulated. A silent performance cliff is worse than an honest rejection. The
   operative word is *silent*: when the same cliff can be made an explicit,
   opt-in choice — by exposing the cheap primitive alongside the convenience
   form — the convenience form may be emulated even if its lowering is a loop.
   The strong/weak compare-exchange split (see :ref:`emulation-atomics`) is
   exactly this case: ``atomic_compare_exchange`` lowers to a CAS retry loop on
   Metal, but only because the loop-free ``atomic_compare_exchange_weak`` is
   exposed next to it, so the author chooses the loop rather than inheriting it
   unawares.

The authoritative, per-feature catalogue of which constructs are emulated,
how they lower on each backend, and their implementation status lives in
:code:`gte/docs/OmegaSL-Emulation-Matrix.md`. The rationale, priority, and
design history for each entry lives in
:code:`gte/docs/OmegaSL-Feature-Gap-Survey.md`. This page describes the model
and walks through representative cases; the Matrix is the source of truth for
status.

----

Categories of Emulation
------------------------

The emulations in OmegaSL fall into a few recurring shapes. Each example below
is representative; consult the Emulation Matrix for the complete list and the
landed/planned status of each row.

Math intrinsics
~~~~~~~~~~~~~~~~

The most common case is a builtin that one backend names differently or
lacks entirely:

* ``saturate(x)`` has no GLSL builtin, so it lowers to ``clamp(x, 0.0, 1.0)``
  (broadcast across vector lanes).
* ``rsqrt(x)`` is renamed to GLSL's ``inversesqrt(x)``.
* ``degrees(r)`` / ``radians(d)`` have no MSL builtin, so they lower to an
  inline multiply by the conversion constant.
* ``fma(a, b, c)`` maps to HLSL ``mad`` on the fp32 path; the precision
  contract loosens on that target and is documented at the surface.

These are name-and-shape swaps with no runtime cost difference.

Texture and sampling
~~~~~~~~~~~~~~~~~~~~~~

``read`` / ``write`` coordinate types differ by backend (signed ``intN`` on
HLSL/GLSL, ``uintN`` on Metal), so the compiler casts the coordinate to the
form each target's fetch/store expects. Query intrinsics such as
``getDimensions`` and ``calculateLOD`` reconcile out-parameter, accessor, and
return-shape differences (for example, GLSL's ``textureQueryLod`` returns a
``vec2`` and the clamped channel is selected).

Matrix indexing and storage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

OmegaSL matrix indexing is locked to the column-first convention
(``m[col][row]``) so the same subscript reads the same element on every
backend and matches the host-side ``OmegaCommon::Matrix`` layout. HLSL indexes
row-first natively, so the compiler **swaps the subscripts** at emit time and
synthesizes a column vector for the single-level ``m[col]`` form. Matrices are
pinned to column-major storage on HLSL via compiler flags and a
``column_major`` field qualifier. See the :doc:`OmegaSL` matrix section for
the author-facing rules.

User function names
~~~~~~~~~~~~~~~~~~~~

A user-defined helper named like a backend stdlib symbol (``saturate`` on
Metal, for instance) would collide. The compiler mangles user function names
with an ``osl_user_`` prefix so author-chosen names never clash with the
target's standard library.

.. _emulation-atomics:

Atomics: strong compare-exchange
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Most atomic operations are pure spelling swaps: ``atomic_add`` is
``InterlockedAdd`` (HLSL), ``atomic_fetch_add_explicit`` (MSL), or
``atomicAdd`` (GLSL); the fetch-ops return the original value, with HLSL
routing it through the mandatory ``out`` parameter via the same statement
rewrite ``frexp`` uses. Those are name-and-shape swaps with no cost
difference.

Compare-exchange is the one atomic where the cost classes genuinely differ
across backends, and it is the standout illustration of decision rule #3 — the
*silent* clause. OmegaSL exposes **two** CAS builtins:

* ``atomic_compare_exchange(mem, compare, desired)`` — **strong** CAS. Returns
  the original value; never fails spuriously.
* ``atomic_compare_exchange_weak(mem, inout expected, desired)`` — **weak**
  CAS. Returns a ``bool`` (did the store happen) and writes the current value
  back through ``expected`` on failure. May fail spuriously, so it is only
  correct inside a caller retry loop — which is exactly where CAS belongs.

HLSL (``InterlockedCompareExchange``) and GLSL (``atomicCompSwap``) have only a
*strong* CAS. Metal has only a *weak* one
(``atomic_compare_exchange_weak_explicit`` — there is no ``_strong_`` variant).
So each builtin is native on some backends and emulated on others:

.. code-block:: text

    builtin                         HLSL            MSL                     GLSL
    -------                         ----            ---                     ----
    atomic_compare_exchange         native strong   weak-in-a-loop (emul)   native strong
    atomic_compare_exchange_weak    strong (emul)   native weak             strong (emul)

The **strong CAS on Metal** is the emulation the rule #3 discussion refers to.
Metal cannot do a strong CAS directly, so ``omegaslc`` lowers it to the
canonical *strong-from-weak* loop: seed a temporary with ``compare``, then spin
on the weak CAS, retrying only while the failure was *spurious* (the temporary
still equals ``compare``) and exiting on a genuine mismatch (the weak CAS has
loaded the differing value into the temporary). The temporary then holds the
original value the strong form must return:

.. code-block:: text

    // atomic_compare_exchange(mem, compare, desired) on Metal:
    uint _exp = compare;
    while(!atomic_compare_exchange_weak_explicit(&mem, &_exp, desired,
              memory_order_relaxed, memory_order_relaxed) && _exp == compare) { }
    // value of the expression == _exp  (the original value)

This is a loop where HLSL/GLSL emit a single instruction — a real cost
difference. It is allowed as an emulation (not a gate) *only* because the
loop-free ``atomic_compare_exchange_weak`` sits right beside it: a
performance-sensitive author writes their own retry loop with the weak form and
pays for exactly one CAS per iteration, while an author who just wants
"swap-or-tell-me" uses the strong form and opts into the loop knowingly. The
cliff is explicit, not silent.

The **weak CAS on HLSL/GLSL** runs the opposite direction: those backends have
no weak form, but a strong CAS trivially *satisfies* the weak contract (it
simply never reports the spurious failures the contract permits). It lowers to
the strong intrinsic plus the bool/write-back bookkeeping the weak shape
requires.

One portability constraint falls out of that bookkeeping. On HLSL/GLSL the weak
CAS expands to several statements (capture the original, compute the success
bool, write back ``expected``), so it must be called as a **statement** — the
right-hand side of an assignment inside the loop body — not placed directly in
a ``while`` / ``for`` *condition*. In a loop body the expansion re-runs each
iteration; hoisted into a bare condition it would run once. (On Metal the weak
CAS is a single inline call and works in either position; the statement form is
the portable one.) The canonical loop is therefore:

.. code-block:: text

    uint expected = atomic_load(slot);
    bool done = false;
    while(!done){
        uint desired = f(expected);                 // recompute from the live value
        done = atomic_compare_exchange_weak(slot, expected, desired);
    }

Relatedly, ``expected`` must be a writable **local** variable, never a buffer
field: Metal takes it as a ``thread`` pointer, which a ``device`` buffer field
cannot form. The compiler rejects the non-local case at compile time rather
than letting it diverge.

----

Worked Example: Integer Matrix Types
-------------------------------------

Integer matrices are a complete, recently landed example of the emulation
model, and a good illustration of why the spelling-vs-capability distinction
matters.

OmegaSL exposes ``intCxR`` and ``uintCxR`` matrix types (for
``C, R`` in ``{2, 3, 4}``) alongside the float matrix family. The wrinkle:
**no backend has a native integer matrix type.** GLSL has only ``mat`` /
``dmat`` (floating-point); Metal's ``metal::matrix`` is float/half only; only
HLSL has ``int4x4``. A naive "pass it through to the native type" strategy
would work on exactly one of the three targets.

Because the operation a shader actually needs — declare an integer matrix,
index its columns and elements, upload and download it — *is* expressible
everywhere, this is an emulation, not a gated feature. The chosen lowering is
an **array of integer column vectors**, applied uniformly on all three
backends, including HLSL (native ``int4x4`` is deliberately not used so there
is a single code-generation shape and no need for the HLSL index swap):

.. code-block:: text

    OmegaSL                 HLSL / MSL              GLSL
    -------                 ----------              ----
    int4x4  transform;      int4  transform[4];     ivec4 transform[4];
    uint3x3 mask;           uint3 mask[3];          uvec3 mask[3];
    int3x2  basis;          int2  basis[3];         ivec2 basis[3];

Column-first indexing falls out for free: ``m[col]`` is a plain array index
yielding the column vector, and ``m[col][row]`` reaches the element — natural
array subscripting on every backend, with no row/column swap.

The surface is deliberately restricted to what lowers uniformly. Integer
matrices support declaration, indexing (column and element reads and writes),
and buffer round-trip. Whole-matrix algebra (``m * v``, ``m + m``), equality,
whole-matrix assignment, and inline construction (``int4x4(...)``) are
**rejected at compile time** with a diagnostic pointing at the per-column
form — no backend offers integer matrix algebra, and an array has no portable
rvalue constructor. This follows the project rule that a construct which
cannot be expressed identically on all three backends is rejected rather than
allowed to diverge silently.

On the host side, the buffer read/write API gains matching
``GEBufferWriter::writeInt<C>x<R>`` / ``writeUint<C>x<R>`` and the symmetric
``getInt`` / ``getUint`` accessors. An integer matrix's byte layout is
identical to the same-shape float matrix — ``int``, ``uint``, and ``float``
are all four-byte scalars — so the existing column-padding (``std430`` /
``std140``) math is reused unchanged.

The full design, the backend-asymmetry analysis, and the verification status
are recorded as §12.2.1 of
:code:`gte/docs/OmegaSL-Feature-Gap-Survey.md`, and the lowering appears as a
row in the Matrix-ops section of
:code:`gte/docs/OmegaSL-Emulation-Matrix.md`.

----

Features That Are Gated, Not Emulated
--------------------------------------

For contrast, these are deliberately **not** emulated even though a lowering
could be imagined — they fall under capability gating instead:

* **16-bit (``half`` / ``short``) and 64-bit (``long``) numeric types.**
  Lowering 16-bit math to 32-bit would silently double bandwidth and defeat
  the reason to use the type. Gated via the ``FLOAT16`` / ``INT64`` feature
  bits.
* **Double precision.** Metal has no double type at any width; gated, with a
  stub on MSL. See :doc:`Limitations`.
* **Geometry shaders on Metal, tessellation on Metal, subpass inputs off
  Vulkan.** Structurally different execution models, not spelling
  differences.
* **Cube / multisample image writes, 1D-texture mip sampling on Metal.**
  Backend-asymmetric at the hardware level; gated and stubbed.

These live in the feature-gating system (§14 of the gap survey) and are
surfaced to applications through runtime capability checks, exactly as
described in :doc:`Limitations`.

----

Where to Look Next
------------------

* :code:`gte/docs/OmegaSL-Emulation-Matrix.md` — the authoritative
  per-feature table: every emulated construct, its lowering on each backend,
  and whether it is landed, partial, planned, or intentionally skipped.
* :code:`gte/docs/OmegaSL-Feature-Gap-Survey.md` — the rationale, priority,
  and implementation history behind each row, including the full integer
  matrix write-up (§12.2.1).
* :doc:`OmegaSL` — the author-facing language reference, including the
  backend-mapping table for the common constructs.
* :doc:`Limitations` — the complement to this page: capability divergence,
  feature gating, and the driver quirks the engine works around.
