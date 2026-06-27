# Super-Resolution & Frame-Generation — OmegaGTE Extension Plan

Status: **FUTURE / PROPOSAL** — not yet scheduled. Follows the Code-Authoring
flow in `AGENTS.md` (research → synthesize → refine → phase → implement
incrementally). Nothing here is built yet.

**Consumer:** the **kREATE** game engine (3D), driven by the **AQUA** physics
engine. OmegaGTE provides the GPU primitives; kREATE owns camera jitter, motion
vectors (sourced from AQUA object transforms + camera), render-scale, and
present pacing. This plan stays inside `gte`; kREATE/AQUA wiring is called out at
module boundaries but lives in those modules' own plans.

## Two orthogonal features

This plan covers **two distinct, separately-available capabilities**. They are
not the same feature and must be queried and toggled independently:

1. **Super Resolution (SR / upscaling)** — render the scene at a lower internal
   resolution and reconstruct a full display-resolution image. Spatial or
   temporal.
2. **Frame Generation (FG / multi-frame)** — synthesize one or more
   *interpolated* frames between rendered frames to raise presented frame rate.
   "Multi-frame" = a factor > 2× (more than one generated frame per rendered
   frame). FG is wanted **wherever possible**.

They compose (render low-res → **upscale** → **frame-gen** → present) but ship
and gate separately. A device may offer both, one, or neither.

### Capability matrix (target)

| Backend | Vendor | Super Resolution | Frame Generation |
|---------|--------|------------------|------------------|
| Metal | Apple | **MetalFX** (Temporal + Spatial) | **none** — MetalFX is upscaling-only |
| D3D12 / Vulkan | NVIDIA `0x10DE` | **Streamline → DLSS-SR** | **DLSS-G**: Multi-Frame (2×/3×/4×) on RTX 50, 2× on RTX 40 |
| D3D12 / Vulkan | Intel `0x8086` | **XeSS** (XMX + DP4a fallback) | **XeSS-FG (XeFG)**, 2× (XeSS 2) |
| D3D12 / Vulkan | AMD `0x1002` / any / fallback | **FidelityFX FSR 2/3** (cross-vendor) | **FSR 3 Frame Generation**, 2× (cross-vendor) |
| Any (no SDK / SwiftShader / old HW) | — | **OmegaSL reference** (FSR1-style EASU+RCAS) | **none** (see §3.4) |

Notes that drive the design:
- **MetalFX has no frame generation.** On Apple you get upscaling only; FG caps
  report unavailable.
- **No reference (in-house) frame generator.** FG needs optical-flow + a proxy
  present path that is impractical to reimplement well in OmegaSL; FG is
  SDK-only. SR always has the OmegaSL reference fallback, FG does not.
- **Multi-frame (>2×) is NVIDIA-only today** (DLSS-G MFG). The FG API still
  carries a `factor`, but caps clamp it per provider (AMD/Intel → 2×).

---

## 0. Resolved scope & remaining decisions

**Resolved by the developer:**
- Use case = kREATE (3D game engine) + AQUA physics → full **temporal** SR
  pipeline applies (motion vectors + depth + jitter are available from the
  scene/physics). Spatial-only is the strict subset / fallback.
- AMD upscaler = **FSR** (FidelityFX), confirmed.
- Frame generation is a **first-class goal**, separate from SR, enabled wherever
  the device supports it.

**Confirmed by the developer (these decisions are now settled, not open):**
1. **Latency-reduction coupling — IN SCOPE.** FG meaningfully *raises* input
   latency and every vendor pairs it with a latency reducer — **DLSS-G requires
   NVIDIA Reflex**; FSR3 FG pairs with **AMD Anti-Lag**; XeFG with **Intel
   XeLL**. A small `GELatencyReduction` abstraction is a **prerequisite** of the
   FG phases and lands as **Phase 6** (before any FG provider).
2. **Present-path ownership — ACCEPTED.** DLSS-G and FSR3 FG provide a **proxy
   swapchain** that owns interpolated-frame presentation and pacing.
   `GENativeRenderTarget` will gain an `enableFrameGen(...)` seam so a provider
   can **wrap/replace** present (Phase 7). This is the most invasive part of FG
   and the reason FG is phased after SR.
3. **v1 cut line — SR FIRST, then FG.** Ship **all SR phases (1-5)** as a usable
   release; **FG (6-9) follows**. FG is not required in the same release as SR.

**Documented contract (kREATE/AQUA side, no GTE impact):**
- **AQUA determinism vs interpolated frames.** Generated frames are
  *interpolated*, not simulated — they must not feed back into AQUA's fixed
  timestep or input sampling. kREATE keeps simulation/input on rendered frames
  only. Enforced in the kREATE/AQUA plan; `gte` only documents the contract.

---

## 1. Research summary (provider contracts)

### 1.1 Super Resolution
All temporal SR providers converge on the same per-frame contract — what makes
one vendor-neutral abstraction viable.
- **Inputs:** input color (render-res, HDR linear), motion vectors (render-res,
  screen-space px), depth (render-res), jitter `(x,y)`; optional exposure,
  reactive mask, transparency/composition mask. **Output:** display-res color.
- **Init config:** render extent, display extent, quality preset
  (Native-AA/Quality/Balanced/Performance/Ultra-Perf → render:display ratio),
  HDR, MV scale/format, depth inverted/infinite, sharpness. **Per-frame flag:**
  `reset` (camera cut).
- **MetalFX** (`MTLFX`, macOS 13+/iOS 16+): first-party, no SDK.
  `MTLFXTemporalScaler` (color/depth/MV/jitter), `MTLFXSpatialScaler` (color
  only). Encoded onto an `MTLCommandBuffer`.
- **Streamline/DLSS-SR** (RTX/Turing+): `sl.interposer` + `sl.dlss`; abstracts
  D3D12/Vulkan; tag resources then evaluate. Availability = HW + driver
  (runtime query). `nvngx_dlss` blob ships with the app.
- **XeSS:** XMX on Arc; **DP4a fallback** any DX12 GPU at SM 6.4. D3D12
  (`libxess`) + Vulkan (`libxess_vk`). No pure-spatial mode.
- **FSR 2/3** (FidelityFX SDK, MIT): pure compute, runs on **any** DX12/Vulkan
  GPU → also the universal fallback. FSR1 (spatial EASU+RCAS) is simple enough
  to reimplement in OmegaSL.

### 1.2 Frame Generation
- **Common inputs:** the (usually already-upscaled) color, depth, motion
  vectors, and a **UI/HUD mask or a UI-free color** — UI must be excluded from
  interpolation or it smears. The provider computes optical flow internally.
- **Output:** N presented frames per rendered frame (N = factor). Provider owns
  pacing via a proxy swapchain.
- **DLSS-G** (Streamline `sl.dlss_g`): **Multi-Frame** 2×/3×/4× (RTX 50), 2×
  (RTX 40). **Requires Reflex.**
- **FSR 3 FG** (FidelityFX): 2×, cross-vendor; ships a frame-interpolation
  swapchain (`FfxSwapchain`). Pairs with Anti-Lag.
- **XeFG** (XeSS 2): 2×; pairs with XeLL.
- **MetalFX:** none.

### 1.3 OmegaSL readiness (from `OmegaSL-Feature-Gap-Survey.md`)
`gather`/`gatherRed/Green/Blue/Alpha` **LANDED**, and 16-bit types
(`half`/`float16_t`/`int16_t`) **LANDED** (§4.1). These are exactly what an
FSR1-style EASU/RCAS kernel needs, so the OmegaSL reference upscaler (Phase 4) is
largely expressible today; remaining ops are driven through the gap survey.

---

## 2. Current-state grounding (verified, file:line)

- **Device & caps** — `GTEDevice.h`: `GTEDevice` (`:151`, `Integrated`/`Discrete`,
  `name`, `features`); `GTEDeviceFeatures` (`:49`, `uint64_t flags` of
  `GTEDEVICE_FEATURE_*` `:20-44`, `ShaderModel`, limits, `featuresAsBitmask()`
  `:108`). **No vendor/PCI-ID field today.** `enumerateDevices()` (`:178`).
- **Vendor detection gap (confirmed):** D3D12 `GED3D12.cpp` calls
  `adapter->GetDesc1` (`:53`) but ignores `desc.VendorId`; Vulkan
  `queryVulkanFeatures` (`GEVulkan.cpp:390`) never reads `props.vendorID`; Metal
  `queryMetalFeatures` (`GEMetal.mm:83`) implies Apple.
- **Engine factory** — `OmegaGraphicsEngine` (`GE.h:361`): `makeTexture` (`:476`),
  `makeTextureRenderTarget` (`:553`), `makeComputePipelineState` (`:498`),
  `makeBlitPipelineState` (`:508`), `makeNativeRenderTarget` (`:542`), `makeFence`
  (`:456`).
- **Command recording / present** — `GECommandBuffer` (`GERenderTarget.h:69`):
  `startComputePass`/`dispatchThreadgroups` (`:323-356`), `blitWithPipeline`
  (`:270`), `copyTextureToTexture` (`:214`). `GENativeRenderTarget::present()`
  (`:412`), `presentQueue()` (`:406`), `resizeSwapChain()` (`:421`);
  `GETextureRenderTarget::underlyingTexture()` (`:438`).
- **Queue/sync** — `GECommandQueue` (`GECommandQueue.h:205`): `submitCommandBuffer`,
  `commitToGPU`, `signalFence`/`waitForFence`.
- **No SR/FG/upscale/post-process abstraction exists** in `gte`/`wtk`/`common`
  (grep confirmed). Greenfield.

---

## 3. Proposed architecture

Two vendor-neutral resources created by the engine, each with one backend impl
per provider behind a runtime selection policy. Public surfaces never name
DLSS/FSR/XeSS/MetalFX — callers ask for a *mode*/*quality*/*factor*, the engine
binds the best provider.

```
   makeUpscaler(desc)                          enableFrameGen(rt, desc)
        │ policy: backend→vendor→fallback           │ policy: backend→vendor (NO reference)
  ┌─────┼───────┬───────┬─────────┬─────────┐  ┌────┼────────┬──────────┬──────────┐
 MetalFX DLSS-SR XeSS  FSR2/3  Reference     DLSS-G(MFG)  XeFG     FSR3-FG   (none on Metal)
                                (OmegaSL)
```

### 3.1 Super Resolution API — `gte/include/omegaGTE/GEUpscaler.h` (new)
- `enum class GEUpscaleMode { SpatialOnly, Temporal };`
- `enum class GEUpscaleQuality { NativeAA, Quality, Balanced, Performance, UltraPerformance };`
- `enum class GEUpscaleProvider { Auto, Reference, MetalFX, DLSS, XeSS, FSR };`
- `struct GEUpscalerDescriptor` — `mode`, `quality`, `displayWidth/Height`,
  `inputColorFormat`, `outputColorFormat`, `hdr`, `autoExposure`,
  `depthInverted`, `depthInfinite`, `enableSharpening`, `provider=Auto`,
  optional `StrRef label`. `renderExtentForQuality()` derives the low-res size.
- `struct GEUpscaleEvalInfo` — `SharedHandle<GETexture> color, depth,
  motionVectors, output;` optional `exposure, reactiveMask, transparencyMask;`
  `float jitterX, jitterY, motionVectorScaleX, scaleY, sharpness, preExposure;
  bool reset;`
- `class GEUpscaler : GTEResource` — `descriptor()`, `activeProvider()`,
  `renderExtent()`, `resize(w,h)`,
  `encode(SharedHandle<GECommandBuffer>&, const GEUpscaleEvalInfo&)` (records its
  own pass, same self-contained-pass contract as `blitWithPipeline`).

### 3.2 Frame Generation API — `gte/include/omegaGTE/GEFrameGenerator.h` (new)
FG is modeled against the **native render target** because it owns presentation.
- `enum class GEFrameGenProvider { Auto, DLSSG, XeFG, FSR3FG };` (no `Reference`,
  no `MetalFX`.)
- `struct GEFrameGenDescriptor` — `uint32_t factor` (2 = one generated frame;
  3/4 = multi-frame), `displayWidth/Height`, color/depth formats, `hdr`,
  `uiHandling` (`SeparateUIMask` | `UIFreeColorPlusComposite`),
  `provider=Auto`, optional `StrRef label`. Caps clamp `factor`.
- `struct GEFrameGenEvalInfo` — `SharedHandle<GETexture> color (post-upscale),
  depth, motionVectors, uiColorOrMask;` `float jitterX/Y, motionVectorScaleX/Y;
  bool reset;`
- `class GEFrameGenerator : GTEResource` — `activeProvider()`, `factor()`,
  `resize(w,h)`, `submitInterpolated(const GEFrameGenEvalInfo&)` (hands the
  provider the inputs for this rendered frame; the provider's proxy swapchain
  paces and presents the generated + real frames).
- **Engine/present seam:** `GENativeRenderTarget` gains an optional
  frame-generator hook so a provider can wrap `present()`. Likely shape:
  `enableFrameGen(const GEFrameGenDescriptor&)` on the render target returning a
  `SharedHandle<GEFrameGenerator>` (or null when unsupported), so the proxy
  swapchain is installed at the right layer. This is the invasive bit (decision
  #2, §0).

### 3.3 Factory + capability queries (`GE.h` additions)
- `virtual SharedHandle<GEUpscaler> makeUpscaler(const GEUpscalerDescriptor&) = 0;`
  (never null for `SpatialOnly` — reference path always exists.)
- `virtual GEUpscalerCaps queryUpscalerCaps(GEUpscaleMode) = 0;`
- `virtual GEFrameGenCaps queryFrameGenCaps() = 0;` —
  `{ bool available; GEFrameGenProvider best; uint32_t maxFactor; bool needsReflexLike; };`
- `struct GEUpscalerCaps { bool spatial, temporal; GEUpscaleProvider best;
  uint32_t supportedQualityMask; bool needsMotionVectors, needsDepth; };`
- Availability is **runtime-queried**, not a static `GTEDEVICE_FEATURE_*` bit
  (DLSS/XeSS/DLSS-G depend on driver + HW state). Add at most coarse advisory
  bits `GTEDEVICE_FEATURE_SUPER_RESOLUTION` / `..._FRAME_GENERATION`.

### 3.4 Selection policy (engine-side, one place)
- **SR `Auto`:** Metal → MetalFX (else Reference). D3D12/Vulkan → by vendor:
  NVIDIA→DLSS-SR, Intel→XeSS, AMD/Unknown→FSR; any init failure falls through to
  **FSR**, then **Reference**. FSR runs on every vendor → universal safety net.
- **FG `Auto`:** Metal → **unavailable** (report caps false). D3D12/Vulkan → by
  vendor: NVIDIA→DLSS-G, Intel→XeFG, AMD/Unknown→FSR3-FG; init failure falls
  through to **FSR3-FG** (cross-vendor), else **unavailable** (no reference FG).
- Forced provider skips policy but still downgrades with a `DEBUG_CRITICAL`
  diagnostic on init failure (mirrors the mesh-shader/raytracing gate pattern).

---

## 4. Phased implementation

SR ships first (independently valuable, far less invasive); FG follows and
builds on the SR output. Each phase is independently verifiable; the OmegaSL
reference SR path (Phase 4) gives a working upscaler on **every** backend before
any SDK is vendored.

### Phase 1 — GPU vendor & capability detection *(precondition, small)*
- `GTEDevice.h`: add `enum class GEGPUVendor : uint32_t { Unknown=0,
  NVIDIA=0x10DE, AMD=0x1002, Intel=0x8086, Apple=0x106B };` + `GEGPUVendor
  vendor; uint32_t vendorId, deviceId;` on `GTEDeviceFeatures` (declared last to
  keep positional aggregate init valid).
- D3D12: read `desc.VendorId`/`DeviceId` from the already-fetched
  `DXGI_ADAPTER_DESC1`. Vulkan: read `props.vendorID`/`deviceID`. Metal: `Apple`.
- One shared raw-ID → `GEGPUVendor` helper.
- **Verify:** test prints vendor per `enumerateDevices()` on Metal host + Vulkan
  CI; D3D12 via user. ~<300 lines → single note, no sub-phasing.

### Phase 2 — Render-resolution decoupling + jitter + motion vectors *(temporal SR precondition)*
- Offscreen low-res color/depth/motion `GETextureRenderTarget`s sized by
  `GEUpscaler::renderExtent()`; upscale → full-res → present.
  (`makeTextureRenderTarget` already takes arbitrary sizes; new piece is
  coordinated sizing + present-time composite.)
- **Jitter** sequence helper (Halton(2,3)); kREATE applies it to projection,
  reports it via `GEUpscaleEvalInfo`.
- **Motion-vector convention** (screen-space px, y-direction, scale). kREATE
  sources per-object MVs from **AQUA** transforms (prev/cur) + camera; ship an
  OmegaSL camera-only-MV kernel (from depth + prev/cur view-proj) for static
  geometry (delivered in Phase 4).
- Sub-phased (large, cross-cutting): 2a target sizing/lifetime, 2b jitter,
  2c MV convention + camera-MV kernel, 2d UI-composited-after-upscale present.
- **Verify:** render at 0.5× into low-res targets, blit 1:1 (no upscaler yet);
  confirm color/depth/MV via visualization blit + user screenshot.

### Phase 3 — `GEUpscaler` API + passthrough impl
- Add `GEUpscaler.h`, descriptor/eval structs, `makeUpscaler` +
  `queryUpscalerCaps` on `OmegaGraphicsEngine`, selection-policy skeleton
  returning a **passthrough** (bilinear `copyTextureToTexture`) so the pipeline
  runs end-to-end before any real algorithm.
- Wire passthrough into all three backends.
- **Verify:** low-res → passthrough → present; blurry but topologically correct.

### Phase 4 — OmegaSL reference spatial upscaler (FSR1 EASU+RCAS) *(always-available SR)*
- Author `EASU` + `RCAS` as OmegaSL compute kernels in `gte/omegasl/` (FSR1 is
  MIT — port the math, no binary). `gather*`/`half` already landed.
- Drive any remaining op through `OmegaSL-Feature-Gap-Survey.md` (keyword → AST →
  parser → Sema → target hook): verify/add packed-fp16 helpers,
  `min3`/`max3`, `rcp`/`saturate` as needed — each its own gap entry.
- `GEReferenceUpscaler` builds via `makeComputePipelineState`, encodes in
  `encode`. Also ship the camera-MV + optional reactive-mask kernels here.
- **Verify (must SEE output):** EASU+RCAS 2× visibly sharper than Phase 3
  bilinear; cross-backend match (Metal host + Vulkan CI; D3D12 via user). Compile
  generated HLSL/GLSL with local `dxc`/`glslc` (backend-build-verification rule).

### Phase 5 — Vendor SR backends
SR-only providers, in fallback-safety order (FSR first → it backstops the
others). Each falls through to FSR then Reference on init failure.
- **5a — MetalFX SR (Apple).** `GEMetalFXUpscaler` over
  `MTLFXTemporalScaler`/`MTLFXSpatialScaler`; link `MetalFX` in
  `gte/CMakeLists.txt` under `TARGET_METAL`; OS-availability gate (macOS
  13+/iOS 16+). **Runnable on this host** → A/B vs Reference, user screenshot.
- **5b — FSR 2/3 SR (D3D12+Vulkan).** Vendor FidelityFX SDK (MIT) via AUTOMDEPS;
  cross-vendor → the policy fallback for all D3D12/Vulkan vendors, so it lands
  before DLSS/XeSS. **Verify:** Vulkan on Linux CI; D3D12 via user.
- **5c — DLSS-SR (NVIDIA).** Vendor Streamline (`sl.interposer`+`sl.dlss`); ship
  `nvngx_dlss` blob. Runtime RTX+driver gate → fall through to FSR. **Confirm
  DLSS license** first. **Verify:** user on NVIDIA HW.
- **5d — XeSS SR (Intel).** Vendor XeSS SDK; XMX + DP4a fallback (SM 6.4 gate via
  `shaderModel`). Falls through to FSR. **Verify:** user on Intel HW; DP4a path
  on any SM-6.4 DX12 GPU.

### Phase 6 — Latency reduction (`GELatencyReduction`) *(FG prerequisite)*
Small abstraction wrapping NVIDIA Reflex / AMD Anti-Lag / Intel XeLL — markers
around the present/sim boundary so FG doesn't degrade input feel. Vendor-mapped
like SR. Standalone-useful even without FG.
- **Verify:** Reflex/Anti-Lag/XeLL markers active (vendor tools / user HW); no
  visual change, latency-only.

### Phase 7 — `GEFrameGenerator` API + present-path seam *(invasive)*
- Add `GEFrameGenerator.h`, descriptor/eval structs, `queryFrameGenCaps`, and the
  `GENativeRenderTarget::enableFrameGen(...)` seam so a provider can install a
  **proxy swapchain** and wrap `present()` (decision #2, §0).
- No real provider yet — a passthrough FG (`factor` ignored, presents the real
  frame) proves the seam end-to-end across backends.
- **Verify:** present path still correct with the seam installed (passthrough).

### Phase 8 — Vendor FG backends
- **8a — FSR 3 FG (D3D12+Vulkan).** FidelityFX frame-interpolation swapchain;
  cross-vendor → the FG fallback. Pairs with Anti-Lag (Phase 6). **Verify:**
  Vulkan CI + user; D3D12 user. 2× factor.
- **8b — DLSS-G incl. Multi-Frame (NVIDIA).** Streamline `sl.dlss_g`; **requires
  Reflex** (Phase 6). `factor` 2× (RTX 40) / 2-4× (RTX 50) clamped by caps.
  **Verify:** user on RTX 40/50.
- **8c — XeFG (Intel).** XeSS 2 FG; pairs with XeLL. 2×. **Verify:** user on Arc.

### Phase 9 — Selection-policy hardening + kREATE integration + sample
- Finalize SR + FG `Auto` policies, forced-provider overrides, downgrade
  diagnostics; ensure SR-then-FG compose (upscaled color feeds FG).
- GTE end-to-end test (`gte/tests/.../SuperResTest`) per
  `gte/Tests/assets/<TestName>/` layout: render low-res → upscale → (optional)
  frame-gen → present.
- **kREATE boundary (separate kREATE plan):** wire render-scale, jitter,
  AQUA-sourced MVs, UI-after-upscale composite, FG UI mask, and sim/input on
  rendered-frames-only. Note the module boundary — do not reach into kREATE/AQUA
  from `gte`.
- **Verify:** full SR×FG provider A/B matrix on available HW.

---

## 5. Cross-cutting concerns
- **SR and FG are independent** — separate caps queries, separate enable paths,
  separate failure/fallback. FG has **no in-house fallback**; SR always does.
- **FG owns presentation** — the proxy-swapchain seam in `GENativeRenderTarget`
  is the single most invasive change; isolate it behind `enableFrameGen`.
- **FG needs latency reduction** (Phase 6) and a **UI exclusion** path — both are
  prerequisites, not polish.
- **Runtime caps over static bits** — DLSS/XeSS/DLSS-G availability is driver+HW
  dependent; gate in code (mesh-shader/raytracing precedent), not in headers.
- **No `#ifdef`s in public headers**; provider gates live in backend `.cpp`/`.mm`.
- **Deps via AUTOMDEPS only**: FidelityFX (MIT), Streamline + `nvngx_dlss`
  (NVIDIA license, prebuilt blob), XeSS (Intel license), MetalFX (system
  framework, link only). Record licenses; binary redistribution affects
  packaging (build/legal task).
- **OmegaCommon ADTs** on engine surfaces; native types only at the SDK boundary
  (AGENTS boundaries caveat). **C++17 ceiling** — named-local descriptor init.
- **Visual verification mandatory** for every pixel-producing phase (AGENTS
  §Visual Debugging) — green tests are not sufficient; hand off for screenshots
  until `omega-debugviz` is signed off.

## 6. Risks / open questions
- **Present-path seam (Phase 7)** is the highest-risk change — it touches the
  swapchain/present timeline every renderer depends on. Prototype on one backend
  (FSR3 on Vulkan) before generalizing.
- **No dev-host HW for NVIDIA/Intel** — Phases 5c/5d/8b/8c verifiable only via
  the user on real RTX/Arc HW.
- **Multi-frame (>2×) is NVIDIA-only** — the `factor` API must degrade cleanly
  (caps clamp) so kREATE can request 4× and transparently get 2× elsewhere.
- **AQUA determinism** — generated frames must not feed simulation/input; a
  kREATE/AQUA-side contract, documented but enforced outside `gte`.
- **Binary redistribution / licensing** for DLSS + XeSS — resolve before 5c/5d/8b/8c.
```
