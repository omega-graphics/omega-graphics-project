# GTEDeviceFeatures Extension Plan

## Current State

`GTEDeviceFeatures` in `gte/include/OmegaGTE.h:20-24` has three fields:

```cpp
struct GTEDeviceFeatures {
    bool raytracing;
    bool msaa4x;
    bool msaa8x;
};
```

Each backend populates only what it can:
- **Metal** (`GEMetal.mm:66-70`): queries `dev.supportsRaytracing`, `supportsTextureSampleCount:4`, `supportsTextureSampleCount:8`
- **D3D12** (`GED3D12.cpp:69`): queries DXR support via `CheckFeatureSupport(OPTIONS5)`, MSAA fields are default-initialized
- **Vulkan** (`GEVulkan.cpp:197`): queries extension presence (`VK_KHR_acceleration_structure` + `VK_KHR_ray_tracing_pipeline`), MSAA fields are default-initialized

The D3D12 and Vulkan backends don't populate `msaa4x`/`msaa8x` at all — they aggregate-initialize with only the first field. This means those fields are always `false` on non-Metal platforms, which is incorrect (virtually all discrete GPUs support 4x and 8x MSAA).

---

## Design Principles

1. **Only expose what OmegaGTE actually consumes.** Features that have no corresponding engine path (no pipeline option, no command encoding, no resource type) don't belong here yet — they'd be dead metadata.
2. **Queryable on all three backends.** Every field must have a concrete implementation path on Metal, D3D12, and Vulkan. No Metal-only or D3D12-only fields.
3. **Flat booleans and small enums.** Keep the struct trivially copyable. No strings, no heap allocations, no nested structs. A tier enum is fine; a `Vector<String>` of extension names is not.
4. **Conservative defaults.** Fields default to `false` / the lowest tier so that a partially-initialized struct is safe to use.

---

## Proposed Struct

```cpp
struct GTEDeviceFeatures {
    // ── Raytracing ──────────────────────────────────────────────
    bool raytracing;              // existing

    // ── Multisampling ───────────────────────────────────────────
    bool msaa4x;                  // existing
    bool msaa8x;                  // existing
    bool msaa16x;                 // new — useful for high-quality offline / CAD
    bool msaa32x;                 // new — rare but present on some Vulkan/D3D12 HW

    // ── Shader Model ────────────────────────────────────────────
    /// Highest shader model tier the device supports.
    /// Maps to: Metal GPU family, D3D12 shader model, Vulkan SPIR-V cap.
    /// OmegaSL compilation uses this to select codegen features.
    enum class ShaderModel : uint8_t {
        SM_5_0 = 0,   // baseline (D3D FL 11_0 / Vulkan 1.0 / Metal Common1)
        SM_5_1,        // D3D FL 11_1 / unbounded descriptor arrays
        SM_6_0,        // wave intrinsics
        SM_6_4,        // 16-bit types in shaders
        SM_6_5,        // mesh shaders, DXR 1.1
        SM_6_6,        // 64-bit atomics, dynamic resources
        SM_6_7,        // work graphs
    } shaderModel;

    // ── Texture Capabilities ────────────────────────────────────
    unsigned maxTextureDimension2D;   // e.g. 16384 or 32768
    unsigned maxTextureDimension3D;   // e.g. 2048
    unsigned maxTextureDimensionCube; // e.g. 16384
    bool textureCompressionBC;       // BC1-BC7 (desktop standard)
    bool textureCompressionETC2;     // ETC2 (mobile / Vulkan)
    bool textureCompressionASTC;     // ASTC (Apple / mobile Vulkan)

    // ── Buffer / Memory ─────────────────────────────────────────
    uint64_t maxBufferSize;          // largest single buffer allocation (bytes)
    uint64_t dedicatedVideoMemory;   // VRAM in bytes (0 for unified-memory / integrated)

    // ── Compute ─────────────────────────────────────────────────
    unsigned maxComputeWorkGroupSizeX;  // per-axis max
    unsigned maxComputeWorkGroupSizeY;
    unsigned maxComputeWorkGroupSizeZ;
    unsigned maxComputeWorkGroupInvocations; // total threads per group (e.g. 1024)
    unsigned maxComputeSharedMemorySize;     // threadgroup/shared memory (bytes)

    // ── Rasterizer / Pipeline ───────────────────────────────────
    bool independentBlend;         // per-RT blend state
    bool dualSourceBlending;       // SV_Target0 + SV_Target1 blending
    bool depthClamp;               // clamp instead of clip
    bool depthBiasClamp;           // non-zero depth bias clamp
    bool fillModeNonSolid;         // wireframe fill mode (always true on desktop, check mobile)
    bool wideLines;                // line width > 1.0 (Vulkan-relevant)
    bool conservativeRasterization;

    // ── Sampler ─────────────────────────────────────────────────
    bool samplerAnisotropy;
    unsigned maxSamplerAnisotropy;  // typically 16

    // ── Multi-draw / Indirect ───────────────────────────────────
    bool multiDrawIndirect;
    bool drawIndirectFirstInstance;

    // ── Geometry & Tessellation ──────────────────────────────────
    bool geometryShader;           // not present on Metal; false there
    bool tessellationShader;       // Metal has its own tessellation model; true everywhere

    // ── Mesh Shading ────────────────────────────────────────────
    bool meshShader;               // Metal: object/mesh functions (Apple7+); D3D12: mesh shader tier; Vulkan: VK_EXT_mesh_shader

    // ── Variable Rate Shading ───────────────────────────────────
    bool variableRateShading;      // per-draw or per-primitive shading rate

    // ── Fragment Shader Extras ──────────────────────��────────────
    bool shaderBarycentricCoordinates; // barycentric interpolation in fragment shaders

    // ── Bindless / Descriptor Indexing ───────────────────────────
    /// Indicates the device supports large/unbounded descriptor arrays
    /// and non-uniform indexing into them.
    /// Metal: argument buffers tier 2. D3D12: resource binding tier 3.
    /// Vulkan: descriptorIndexing + runtimeDescriptorArray.
    bool descriptorIndexing;

    // ── 16-bit / Relaxed Precision ──────────────────────────────
    bool shaderFloat16;            // native 16-bit float arithmetic in shaders
    bool shaderInt16;              // native 16-bit integer arithmetic

    // ── 64-bit Types ────────────────────────────────────────────
    bool shaderFloat64;            // double-precision in shaders (rare on mobile)
    bool shaderInt64;              // 64-bit integers in shaders

    // ── Timestamp Queries ───────────────────────────────────────
    bool timestampQueries;         // GPU-side timestamp for profiling
    float timestampPeriod;         // nanoseconds per tick (0 if unsupported)
};
```

---

## Backend Mapping

### Metal

| Field | Metal Query |
|---|---|
| `raytracing` | `device.supportsRaytracing` |
| `msaa4x/8x/16x/32x` | `[device supportsTextureSampleCount:N]` |
| `shaderModel` | `supportsFamily:` — Apple7+ -> SM_6_5, Apple6 -> SM_6_4, Apple4+ -> SM_6_0, else SM_5_0 |
| `maxTextureDimension2D` | Feature set tables (16384 for most families) |
| `maxTextureDimension3D` | Feature set tables (2048) |
| `maxTextureDimensionCube` | Feature set tables (16384) |
| `textureCompressionBC` | `device.supportsBCTextureCompression` (Apple9+ always; earlier macOS only) |
| `textureCompressionETC2` | `supportsFamily(.apple1)` (always on iOS) |
| `textureCompressionASTC` | `supportsFamily(.apple2)` or check family |
| `maxBufferSize` | `device.maxBufferLength` |
| `dedicatedVideoMemory` | `device.recommendedMaxWorkingSetSize` (macOS); 0 on unified |
| `maxComputeWorkGroup*` | `device.maxThreadsPerThreadgroup` (per-axis from Metal feature tables) |
| `maxComputeSharedMemorySize` | `device.maxThreadgroupMemoryLength` |
| `independentBlend` | Always true (Metal supports per-attachment blend) |
| `dualSourceBlending` | `supportsFamily(.apple1)` |
| `depthClamp` | Always true on macOS; check family on iOS |
| `fillModeNonSolid` | Always true on macOS; check on iOS |
| `wideLines` | false (Metal does not support wide lines) |
| `conservativeRasterization` | false (Metal does not expose conservative rasterization) |
| `samplerAnisotropy` | Always true; maxSamplerAnisotropy = 16 |
| `multiDrawIndirect` | `supportsFamily(.apple4)` or higher — `MTLIndirectCommandBuffer` |
| `geometryShader` | false (Metal has no geometry shader stage) |
| `tessellationShader` | true (Metal tessellation via compute+post-tessellation vertex) |
| `meshShader` | `supportsFamily(.apple7)` (object/mesh shader functions) |
| `variableRateShading` | `supportsFamily(.apple7)` — `setVertexAmplificationCount` path |
| `shaderBarycentricCoordinates` | `device.supportsShaderBarycentricCoordinates` |
| `descriptorIndexing` | `argumentBuffersSupport == .tier2` |
| `shaderFloat16` | `supportsFamily(.apple1)` — `half` type is native |
| `shaderInt16` | `supportsFamily(.apple1)` |
| `shaderFloat64` | false (Metal does not support double precision) |
| `shaderInt64` | false (Metal does not support 64-bit integers in shaders) |
| `timestampQueries` | `supportsCounterSampling(.atStageBoundary)` |
| `timestampPeriod` | `sampleTimestamps()` — derive from CPU/GPU delta |

### D3D12

| Field | D3D12 Query |
|---|---|
| `raytracing` | `CheckFeatureSupport(OPTIONS5)` → `RaytracingTier >= 1_0` |
| `msaa4x/8x/16x/32x` | `CheckMultisampleQualityLevels(format, N)` > 0 |
| `shaderModel` | `CheckFeatureSupport(SHADER_MODEL)` → `D3D_SHADER_MODEL_6_x` |
| `maxTextureDimension2D` | `D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION` (16384) |
| `maxTextureDimension3D` | `D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION` (2048) |
| `maxTextureDimensionCube` | `D3D12_REQ_TEXTURECUBE_DIMENSION` (16384) |
| `textureCompressionBC` | Always true (D3D requirement) |
| `textureCompressionETC2` | false (not supported on D3D12) |
| `textureCompressionASTC` | false (not supported on D3D12) |
| `maxBufferSize` | `D3D12_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM` or adapter memory |
| `dedicatedVideoMemory` | `DXGI_ADAPTER_DESC1.DedicatedVideoMemory` |
| `maxComputeWorkGroup*` | Constants: X/Y=1024, Z=64, total=1024 |
| `maxComputeSharedMemorySize` | `D3D12_CS_TGSM_REGISTER_COUNT * 4` (32KB) |
| `independentBlend` | Always true (D3D12 supports per-RT blend) |
| `dualSourceBlending` | Always true (D3D12 requirement) |
| `depthClamp` | Always true |
| `fillModeNonSolid` | Always true |
| `wideLines` | false (D3D12 does not support wide lines) |
| `conservativeRasterization` | `CheckFeatureSupport(OPTIONS)` → `ConservativeRasterizationTier >= 1` |
| `samplerAnisotropy` | Always true; maxSamplerAnisotropy = 16 |
| `multiDrawIndirect` | Always true (`ExecuteIndirect`) |
| `geometryShader` | Always true (D3D12 supports geometry shaders) |
| `tessellationShader` | Always true (D3D12 supports hull/domain shaders) |
| `meshShader` | `CheckFeatureSupport(OPTIONS7)` → `MeshShaderTier >= 1_0` |
| `variableRateShading` | `CheckFeatureSupport(OPTIONS6)` → `VariableShadingRateTier >= 1` |
| `shaderBarycentricCoordinates` | `CheckFeatureSupport(OPTIONS3)` → `BarycentricsSupported` |
| `descriptorIndexing` | `CheckFeatureSupport(OPTIONS)` → `ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3` |
| `shaderFloat16` | `CheckFeatureSupport(OPTIONS4)` → `Native16BitShaderOpsSupported` |
| `shaderInt16` | Same as above |
| `shaderFloat64` | `CheckFeatureSupport(OPTIONS)` → `DoublePrecisionFloatShaderOps` |
| `shaderInt64` | `CheckFeatureSupport(OPTIONS1)` → `Int64ShaderOps` |
| `timestampQueries` | Always true (D3D12 supports timestamp queries) |
| `timestampPeriod` | `commandQueue->GetTimestampFrequency()` → convert to ns/tick |

### Vulkan

| Field | Vulkan Query |
|---|---|
| `raytracing` | Extension check: `VK_KHR_acceleration_structure` + `VK_KHR_ray_tracing_pipeline` (already implemented) |
| `msaa4x/8x/16x/32x` | `VkPhysicalDeviceLimits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_N_BIT` |
| `shaderModel` | Map from Vulkan API version + feature presence (e.g. 1.2 + shaderFloat16 → SM_6_4) |
| `maxTextureDimension2D` | `VkPhysicalDeviceLimits.maxImageDimension2D` |
| `maxTextureDimension3D` | `VkPhysicalDeviceLimits.maxImageDimension3D` |
| `maxTextureDimensionCube` | `VkPhysicalDeviceLimits.maxImageDimensionCube` |
| `textureCompressionBC` | `VkPhysicalDeviceFeatures.textureCompressionBC` |
| `textureCompressionETC2` | `VkPhysicalDeviceFeatures.textureCompressionETC2` |
| `textureCompressionASTC` | `VkPhysicalDeviceFeatures.textureCompressionASTC_LDR` |
| `maxBufferSize` | `VkPhysicalDeviceMaintenance3Properties.maxMemoryAllocationSize` or `maxStorageBufferRange` |
| `dedicatedVideoMemory` | `VkPhysicalDeviceMemoryProperties` → sum of `DEVICE_LOCAL` heaps without `HOST_VISIBLE` |
| `maxComputeWorkGroupSizeX/Y/Z` | `VkPhysicalDeviceLimits.maxComputeWorkGroupSize[0/1/2]` |
| `maxComputeWorkGroupInvocations` | `VkPhysicalDeviceLimits.maxComputeWorkGroupInvocations` |
| `maxComputeSharedMemorySize` | `VkPhysicalDeviceLimits.maxComputeSharedMemorySize` |
| `independentBlend` | `VkPhysicalDeviceFeatures.independentBlend` |
| `dualSourceBlending` | `VkPhysicalDeviceFeatures.dualSrcBlend` |
| `depthClamp` | `VkPhysicalDeviceFeatures.depthClamp` |
| `depthBiasClamp` | `VkPhysicalDeviceFeatures.depthBiasClamp` |
| `fillModeNonSolid` | `VkPhysicalDeviceFeatures.fillModeNonSolid` |
| `wideLines` | `VkPhysicalDeviceFeatures.wideLines` |
| `conservativeRasterization` | Extension: `VK_EXT_conservative_rasterization` |
| `samplerAnisotropy` | `VkPhysicalDeviceFeatures.samplerAnisotropy`; max = `limits.maxSamplerAnisotropy` |
| `multiDrawIndirect` | `VkPhysicalDeviceFeatures.multiDrawIndirect` |
| `drawIndirectFirstInstance` | `VkPhysicalDeviceFeatures.drawIndirectFirstInstance` |
| `geometryShader` | `VkPhysicalDeviceFeatures.geometryShader` |
| `tessellationShader` | `VkPhysicalDeviceFeatures.tessellationShader` |
| `meshShader` | Extension: `VK_EXT_mesh_shader` → `VkPhysicalDeviceMeshShaderFeaturesEXT.meshShader` |
| `variableRateShading` | Extension: `VK_KHR_fragment_shading_rate` → `pipelineFragmentShadingRate` |
| `shaderBarycentricCoordinates` | Extension: `VK_KHR_fragment_shader_barycentric` → `fragmentShaderBarycentric` |
| `descriptorIndexing` | `VkPhysicalDeviceDescriptorIndexingFeatures.runtimeDescriptorArray` + `shaderSampledImageArrayNonUniformIndexing` |
| `shaderFloat16` | `VkPhysicalDeviceVulkan12Features.shaderFloat16` |
| `shaderInt16` | `VkPhysicalDeviceFeatures.shaderInt16` |
| `shaderFloat64` | `VkPhysicalDeviceFeatures.shaderFloat64` |
| `shaderInt64` | `VkPhysicalDeviceFeatures.shaderInt64` |
| `timestampQueries` | `VkPhysicalDeviceLimits.timestampComputeAndGraphics` |
| `timestampPeriod` | `VkPhysicalDeviceLimits.timestampPeriod` (already in ns) |

---

## What's NOT Included (and why)

| Omitted Feature | Reason |
|---|---|
| Sparse / tiled resources | No OmegaGTE resource path uses sparse binding today |
| Sampler feedback | No pipeline or command encoding support in GTE |
| Work graphs | D3D12-only, no cross-platform path |
| Multi-viewport / layered rendering | No command encoding support in GTE |
| Shader subgroup / wave operations detail | OmegaSL doesn't expose subgroup ops yet; add when it does |
| Specific format support queries | Better handled per-format via a `supportsFormat()` method than booleans |
| GPU family / feature-level enum | The `ShaderModel` enum abstracts this; raw family values are backend-specific |
| Extended dynamic state flags | Internal engine concern, not a device-level feature for consumers |

These can be added in future passes as the engine grows to consume them.

---

## Implementation Steps

### Phase 1: Fix existing backends (Bug fix)

D3D12 and Vulkan `enumerateDevices()` don't populate `msaa4x`/`msaa8x`. Fix aggregate initialization to query MSAA support on all three backends.

**Files:** `GED3D12.cpp:69,77`, `GEVulkan.cpp:197`

### Phase 2: Extend the struct

Add all new fields to `GTEDeviceFeatures` in `OmegaGTE.h`. Default-initialize everything to safe values (`false`, `0`, `SM_5_0`).

**Files:** `gte/include/OmegaGTE.h`

### Phase 3: Metal backend

Query all fields in `GEMetal.mm` `enumerateDevices()` using `MTLDevice` properties and `supportsFamily:` checks.

**Files:** `gte/src/metal/GEMetal.mm`

### Phase 4: D3D12 backend

Create a helper that takes `IDXGIAdapter1*` + a temporary `ID3D12Device`, calls `CheckFeatureSupport` for OPTIONS through OPTIONS7, SHADER_MODEL, and populates `GTEDeviceFeatures`. Replace the single `detectDXRSupport` call.

**Files:** `gte/src/d3d12/GED3D12.cpp`

### Phase 5: Vulkan backend

Query `VkPhysicalDeviceFeatures`, `VkPhysicalDeviceLimits`, and relevant extension feature structs via `vkGetPhysicalDeviceFeatures2`. Populate all fields.

**Files:** `gte/src/vulkan/GEVulkan.cpp`

### Phase 6: Update docs

Add all new fields to `gte/docs/API.rst` under `GTEDeviceFeatures`.

**Files:** `gte/docs/API.rst`

### Phase 7: Consumer integration

Update `OmegaGTEView` proposal and any feature-gated code paths (e.g. acceleration structure allocation, pipeline creation with `rasterSampleCount`) to check the new feature fields instead of compile-time `#ifdef`.

---

## Open Questions for You

1. **ShaderModel granularity** — The mapping from Metal GPU families and Vulkan versions to D3D12 shader model tiers is imprecise. Is a coarse enum sufficient, or do you want separate capability bits (e.g. `waveIntrinsics`, `nativeFP16`, `meshShader`) without the tier abstraction?

2. **Dedicated video memory** — On Apple Silicon / unified memory architectures, there's no meaningful "VRAM" number. Should this field exist, or should memory queries be a separate API (`GTEDevice::queryMemoryBudget()`)?

3. **Feature gating strategy** — Today, raytracing is gated by compile-time `#ifdef OMEGAGTE_RAYTRACING_SUPPORTED`. Should new features (mesh shaders, VRS, etc.) follow the same pattern, or should we move toward runtime checks via `GTEDeviceFeatures` fields? Runtime checks are more flexible but require stub implementations.

4. **MSAA granularity** — Should we keep individual `msaaNx` bools, or switch to a bitmask / `maxSampleCount` integer? A single `uint8_t maxMSAASamples` (1, 2, 4, 8, 16, 32) would be more compact and extensible.
