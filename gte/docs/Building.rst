================
Building OmegaGTE
================

This page covers the OmegaGTE-specific concerns when building Omega
Graphics. The project-wide build (CMake versions, cross-compiling,
``autom-deps`` bootstrap, iOS / Android cross-builds) lives in the
top-level guide:

  ``docs/BuildingOmegaGraphics.rst``

Start there if you have not yet built the repo end-to-end. Everything
below assumes the project-wide setup is already in place; it only
documents the extra surface OmegaGTE itself exposes per backend.


Per-backend SDK requirements
============================

Each backend OmegaGTE supports gates its feature detection on the
native SDK headers. The contract differs per backend:

- **D3D12** — these are hard build dependencies. An SDK older than the
  floor below fails the build with a clean "undefined identifier"
  error (see the OPTIONS6 / OPTIONS7 note below). This is intentional:
  prior to 2026-05-30 we used ``#if defined(...)`` guards that
  silently dropped feature detection on every SDK because the symbols
  in question are enum values, not preprocessor macros; that
  silently-dead behavior caused at least one false "my hardware
  doesn't support mesh shaders" report and was removed in favor of
  the hard-floor + clear build failure pattern.
- **Vulkan / Metal** — soft gates. Missing optional headers degrade to
  "feature unavailable" rather than failing the build.


Windows / D3D12 — Windows 10 SDK 10.0.19041.0 or newer
-------------------------------------------------------

D3D12 mesh shaders, variable-rate shading, and the rest of the modern
D3D12 feature surface require a recent Windows SDK. The relevant
declarations are:

- ``D3D12_FEATURE_D3D12_OPTIONS6`` — Variable Rate Shading (VRS).
  Backs ``GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING``.
- ``D3D12_FEATURE_D3D12_OPTIONS7`` — Mesh Shader Tier + Sampler
  Feedback Tier. Backs ``GTEDEVICE_FEATURE_MESH_SHADER``.

Both were added in **Windows 10 SDK 10.0.19041.0** (the Windows 10
version 2004 / May 2020 update). Newer Windows 10 SDKs (e.g.
``10.0.22621.0``) and every Windows 11 SDK contain them.

If your local headers are older, the build now fails cleanly with::

    use of undeclared identifier 'D3D12_FEATURE_D3D12_OPTIONS6'
    use of undeclared identifier 'D3D12_FEATURE_D3D12_OPTIONS7'

at ``gte/src/d3d12/GED3D12.cpp``. The fix is to install a newer SDK
(see below); the probes are now compiled unconditionally because the
prior ``#if defined(...)`` guards were a no-op — the symbols are
``D3D12_FEATURE`` enum values, not preprocessor macros, so
``defined(...)`` always reported false and the probes never ran on
any SDK. Removing the guards trades "silently no detection" for "loud
build failure if the SDK is too old," which matches how every other
feature gate in this file behaves.

Turing (RTX 20-series) and newer NVIDIA, RDNA 2 and newer AMD, and
Intel Arc all ship the underlying mesh-shader capability — so once
the SDK is current, the feature flag will light up on those GPUs.

To install a newer Windows SDK:

1. Open the **Visual Studio Installer**.
2. Pick **Modify** on your VS 2022 / 2026 install.
3. Switch to **Individual components**.
4. Search "Windows 10 SDK" or "Windows 11 SDK" and tick a version
   at or above ``10.0.19041.0``. The latest Windows 11 SDK is the
   recommended choice.
5. Install, then re-configure your CMake build so the new SDK headers
   land on the include path.

After re-configuring, ``GTEDeviceFeatures::hasFeature(GTEDEVICE_FEATURE_MESH_SHADER)``
will return ``true`` on capable devices and
``OmegaGraphicsEngine::makeMeshPipelineState`` will pass its
feature-gate check (it currently returns ``nullptr`` with a
``DEBUG_STREAM`` diagnostic on devices where the flag is off).


Linux / Vulkan — VulkanSDK from AUTOMDEPS
-----------------------------------------

``autom-deps`` fetches the VulkanSDK archive into the repo tree on
Linux; the CMake configure step points the build at it automatically.
You do not need a system-wide Vulkan SDK install. If
``autom-deps`` has not been run or is out of date, the Vulkan
backend's feature probes (mesh shader, raytracing, descriptor
indexing, …) will be unreachable for the same reason: the SDK headers
are missing.

Re-sync at any time::

    ./autom/tools/autom-deps


macOS / Metal — Xcode-shipped headers
-------------------------------------

The Metal backend uses headers that ship with the system Xcode
install; there is no separate fetch. Mesh shaders specifically
require Metal 3 (macOS 13 / iOS 17 or newer) and the **Apple7** GPU
family (M3, A17, and newer). On every Intel Mac, M1, and M2 the
``GTEDEVICE_FEATURE_MESH_SHADER`` flag stays off even with the latest
Xcode — that is a hardware limit, not an SDK one.

Keep Xcode reasonably current (the latest stable release is
recommended; see ``docs/BuildingOmegaGraphics.rst``) so the headers
match the runtime you are targeting.


Verifying the mesh-shader path
==============================

A quick way to confirm the D3D12 path picked up the newer SDK on
Windows:

1. After a clean build, run any OmegaGTE-using app under the debug
   layer (``OMEGAGTE_DEBUG=1`` or build with the debug layer enabled).
2. Construct a ``MeshPipelineDescriptor`` with valid shaders and call
   ``OmegaGraphicsEngine::makeMeshPipelineState``.
3. Watch ``DEBUG_STREAM`` output:

   - "device does not advertise ``GTEDEVICE_FEATURE_MESH_SHADER``" →
     either the SDK is too old (no detection) or the hardware does
     not support mesh shaders. Check ``GTEDeviceFeatures::flags``.
   - "D3D12 mesh PSO build not yet implemented (Phase 3 stub — Phase
     4b will land ``CD3DX12_PIPELINE_MESH_STATE_STREAM``)" → SDK and
     hardware are both fine; you are seeing the Phase-3 public-API
     stub. The real PSO + ``DispatchMesh`` plumbing lands in Phase 4b
     (see ``gte/docs/Mesh-Shader-Implementation-Plan.md``).

The Vulkan path follows the same diagnostic shape: a missing feature
prints "device does not advertise ``GTEDEVICE_FEATURE_MESH_SHADER``",
and a present feature prints the Phase 4a "not yet implemented"
diagnostic.


Further reading
===============

- ``docs/BuildingOmegaGraphics.rst`` — main project build guide
  (CMake, ``autom-deps``, mobile cross-builds).
- ``gte/docs/Mesh-Shader-Implementation-Plan.md`` — per-phase status
  of the mesh-shader feature across OmegaSL and OmegaGTE.
- ``gte/docs/Debugging.rst`` — runtime debug layer + capture tooling.
