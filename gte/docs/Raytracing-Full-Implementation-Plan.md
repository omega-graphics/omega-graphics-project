# Full Raytracing Implementation Plan

## Goal

Bring raytracing from its current partial state to a fully functional, cross-platform feature across all three backends (D3D12, Vulkan, Metal) and the OmegaSL shader language. Additionally, remove the compile-time `OMEGAGTE_RAYTRACING_SUPPORTED` macro and replace it with runtime device feature checks via `GTEDeviceFeatures.raytracing`.

## Current State

Raytracing exists in a half-built state across the codebase. What works and what doesn't:

### What's Built

| Component | D3D12 | Vulkan | Metal |
|---|---|---|---|
| Accel struct creation | Yes | Yes | Yes |
| Accel struct building (cmd) | Yes | Yes | Yes |
| Accel struct copy (cmd) | Yes | Partial | Stub |
| Accel struct refit (cmd) | No | Yes | Yes |
| `dispatchRays()` | Stub (empty SBT) | Stub (empty SBT) | Stub (redirects to compute) |
| Ray tracing pipeline | **No** | **No** | **No** |
| Shader binding table | **No** | **No** | **No** |
| RT shader types in OmegaSL | **No** | **No** | **No** |

### What's Missing Everywhere

1. **Ray tracing pipeline state objects** — No `ID3D12StateObject`, no `VkPipeline` with `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`, no Metal intersection function tables.
2. **Shader binding tables (SBT)** — `dispatchRays()` on D3D12 and Vulkan creates empty `D3D12_DISPATCH_RAYS_DESC` / `VkStridedDeviceAddressRegionKHR` with all shader table pointers zeroed.
3. **OmegaSL ray tracing shader types** — The shader language only knows `vertex`, `fragment`, `compute`, `hull`, `domain`. No ray generation, closest hit, any hit, miss, or intersection shaders.
4. **Public API for RT pipelines** — `GEPipeline.h` has `RenderPipelineDescriptor` and `ComputePipelineDescriptor` but no `RayTracingPipelineDescriptor`.

### The Macro Problem

`OMEGAGTE_RAYTRACING_SUPPORTED` is a compile-time gate defined in `GE.h` based on SDK version/platform:
- **D3D12**: `NTDDI_VERSION >= NTDDI_WIN10_RS5` (Windows 10 1809+)
- **Metal**: `__MAC_11_0` / `__IPHONE_14_0`
- **Vulkan**: Always defined

This gates the **public API** (`GEAccelerationStruct`, `GEAccelerationStructDescriptor`, `dispatchRays`, accel struct commands), meaning downstream code must also `#ifdef` around raytracing usage. This is fragile — a binary compiled on Windows 10 RS5+ includes the symbols, but the GPU may not support DXR at all. The correct approach is always-present API with runtime `features.raytracing` checks.

The macro appears in **17 files** across public headers, all three backend headers and implementations, and downstream docs.

## Non-Goals

- Inline ray tracing (DXR 1.1 `DispatchRays` from graphics command lists). We support ray tracing from compute/ray pass only.
- Ray queries in non-RT shaders (Vulkan `GL_EXT_ray_query` / HLSL `RayQuery`). This is a follow-up.
- GPU work graphs or shader execution reordering (SER).
- Changes to the triangulation engine (TE).

---

## Phase 0: Remove `OMEGAGTE_RAYTRACING_SUPPORTED` Compile-Time Gate

### 0.1 Make all raytracing types and methods unconditionally present

Remove every `#ifdef OMEGAGTE_RAYTRACING_SUPPORTED` / `#endif` pair from:

**Public headers:**
- `gte/include/omegaGTE/GE.h` — `GERaytracingBoundingBox`, `GEAccelerationStructDescriptor`, `GEAccelerationStruct`, engine methods `createBoundingBoxesBuffer()`, `allocateAccelerationStructure()`
- `gte/include/omegaGTE/GECommandQueue.h` — `beginAccelStructPass()`, `buildAccelerationStructure()`, `copyAccelerationStructure()`, `refitAccelerationStructure()`, `finishAccelStructPass()`, `bindResourceAtComputeShader(GEAccelerationStruct)`, `dispatchRays()`

**D3D12 backend:**
- `gte/src/d3d12/GED3D12.h` — `GED3D12AccelerationStruct`, engine method declarations
- `gte/src/d3d12/GED3D12.cpp` — method implementations
- `gte/src/d3d12/GED3D12CommandQueue.h` — command buffer declarations
- `gte/src/d3d12/GED3D12CommandQueue.cpp` — command buffer implementations

**Vulkan backend:**
- `gte/src/vulkan/GEVulkan.h`, `GEVulkan.cpp`
- `gte/src/vulkan/GEVulkanCommandQueue.h`, `GEVulkanCommandQueue.cpp`

**Metal backend:**
- `gte/src/metal/GEMetal.h`, `GEMetal.mm`
- `gte/src/metal/GEMetalCommandQueue.h`, `GEMetalCommandQueue.mm`

### 0.2 Remove the macro definitions

Delete the `#define OMEGAGTE_RAYTRACING_SUPPORTED` lines from `GE.h` (lines 18-20, 35-41, 62).

### 0.3 Add runtime guards in method bodies

Each backend method that touches hardware RT APIs must check at runtime:

```cpp
// D3D12 example:
SharedHandle<GEAccelerationStruct> GED3D12Engine::allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) {
    if (!deviceFeatures.raytracing) {
        DEBUG_STREAM("Raytracing not supported on this device");
        return nullptr;
    }
    // ... existing implementation ...
}
```

The `GTEDeviceFeatures` struct already has `bool raytracing` and it's already populated on all three backends during `enumerateDevices()`. The engine needs access to the features of the device it was created with — thread the `GTEDeviceFeatures` from the `GTEDevice` into the engine at construction time.

### 0.4 Update downstream references

- `gte/docs/API.rst` — Remove `#ifdef` examples, document runtime feature check pattern
- `wtk/docs/OmegaGTEView-Proposal.md` — Replace macro references with `features.raytracing`

**Files**: All 17 files listed above, plus docs.

---

## Phase 1: OmegaSL Ray Tracing Shader Types

This is the prerequisite for everything else — without shader support, there's nothing to put in a pipeline or SBT.

### 1.1 Add ray tracing shader type keywords

In `gte/omegasl/src/Toks.def`, add:

```cpp
#define KW_RAYGEN KW("raygen")
#define KW_CLOSESTHIT KW("closesthit")
#define KW_ANYHIT KW("anyhit")
#define KW_MISS KW("miss")
#define KW_INTERSECTION KW("intersection")
#define KW_CALLABLE KW("callable")
```

In `gte/omegasl/src/Lexer.cpp`, add these to `isKeyword()`.

### 1.2 Extend `ShaderDecl::Type` and `omegasl_shader_type`

In `gte/omegasl/src/AST.h`, extend the enum:

```cpp
struct ShaderDecl : public FuncDecl {
    typedef enum : int {
        Vertex, Fragment, Compute, Hull, Domain,
        RayGeneration, ClosestHit, AnyHit, Miss, Intersection, Callable
    } Type;
    // ...
};
```

In `gte/include/omegasl.h`, extend the public enum:

```cpp
typedef enum : int {
    OMEGASL_SHADER_VERTEX,
    OMEGASL_SHADER_FRAGMENT,
    OMEGASL_SHADER_COMPUTE,
    OMEGASL_SHADER_HULL,
    OMEGASL_SHADER_DOMAIN,
    OMEGASL_SHADER_RAYGEN,
    OMEGASL_SHADER_CLOSESTHIT,
    OMEGASL_SHADER_ANYHIT,
    OMEGASL_SHADER_MISS,
    OMEGASL_SHADER_INTERSECTION,
    OMEGASL_SHADER_CALLABLE
} omegasl_shader_type;
```

### 1.3 Add RT shader parameters descriptor

Ray tracing shaders receive built-in payloads and attributes, not vertex inputs. Add to `omegasl.h`:

```cpp
struct omegasl_raygen_shader_desc {
    // No special inputs beyond resource bindings
};

struct omegasl_hit_shader_desc {
    // Payload and attribute types will be described here
    // once OmegaSL supports payload declarations
};
```

And extend `omegasl_shader` with a union or additional fields for RT shader metadata.

### 1.4 Parser support

In `gte/omegasl/src/Parser.cpp`, add parsing branches for the new keywords following the existing pattern (lines 212-281). RT shaders don't have threadgroup descriptors or vertex inputs; they have resource maps (already supported via `resourceMap`).

Ray tracing shaders use the same resource binding model as compute shaders (buffers, textures, acceleration structures bound by index).

### 1.5 Add RT-specific built-in types and functions

In `AST.h` builtins and `AST.cpp`:

```cpp
// Types
DECLARE_BUILTIN_TYPE(ray_type);                  // Ray description
DECLARE_BUILTIN_TYPE(raypayload_type);           // User-defined ray payload
DECLARE_BUILTIN_TYPE(acceleration_structure_type); // TLAS/BLAS handle
DECLARE_BUILTIN_TYPE(intersection_attributes_type);

// Intrinsics
DECLARE_BUILTIN_FUNC(trace_ray);        // TraceRay / traceRayEXT / intersect
DECLARE_BUILTIN_FUNC(report_hit);       // ReportHit / reportIntersectionEXT
DECLARE_BUILTIN_FUNC(ignore_hit);       // IgnoreHit
DECLARE_BUILTIN_FUNC(accept_hit);       // AcceptHitAndEndSearch
DECLARE_BUILTIN_FUNC(ray_origin);       // WorldRayOrigin()
DECLARE_BUILTIN_FUNC(ray_direction);    // WorldRayDirection()
DECLARE_BUILTIN_FUNC(ray_tmin);         // RayTMin()
DECLARE_BUILTIN_FUNC(ray_tcurrent);     // RayTCurrent()
DECLARE_BUILTIN_FUNC(instance_index);   // InstanceIndex()
DECLARE_BUILTIN_FUNC(primitive_index);  // PrimitiveIndex()
DECLARE_BUILTIN_FUNC(hit_kind);         // HitKind()
```

In `Toks.def`, add corresponding attribute keywords:

```cpp
#define ATTRIBUTE_DISPATCH_RAYS_INDEX "DispatchRaysIndex"
#define ATTRIBUTE_DISPATCH_RAYS_DIMENSIONS "DispatchRaysDimensions"
#define ATTRIBUTE_WORLD_RAY_ORIGIN "WorldRayOrigin"
#define ATTRIBUTE_WORLD_RAY_DIRECTION "WorldRayDirection"
#define ATTRIBUTE_RAY_TMIN "RayTMin"
#define ATTRIBUTE_RAY_TCURRENT "RayTCurrent"
#define ATTRIBUTE_INSTANCE_INDEX "InstanceIndex"
#define ATTRIBUTE_PRIMITIVE_INDEX "PrimitiveIndex"
#define ATTRIBUTE_HIT_KIND "HitKind"
```

### 1.6 Sema validation

In `gte/omegasl/src/Sema.cpp`, extend shader validation (around line 1248) to handle RT shader types:

- `RayGeneration`: May use `DispatchRaysIndex`, `DispatchRaysDimensions`. Must not have vertex inputs.
- `ClosestHit` / `AnyHit`: May access hit attributes, ray intrinsics. Must not call `trace_ray` recursively (enforce max recursion depth).
- `Miss`: May use ray intrinsics, must not access hit attributes.
- `Intersection`: Must call `report_hit`. Must not call `trace_ray`.
- `Callable`: Free-form, similar to a compute helper.

Add `AttributeContext` entries for each RT shader type in the attribute validation code (around line 1308).

**Files**: `Toks.def`, `Lexer.cpp`, `AST.h`, `AST.def`, `AST.cpp`, `Parser.cpp`, `Sema.cpp`, `omegasl.h`

---

## Phase 2: OmegaSL Ray Tracing Code Generation

### 2.1 HLSL code generation (D3D12 / DXC)

In `gte/omegasl/src/HLSLCodeGen.cpp`:

**Shader entry point generation:**
- `RayGeneration` → `[shader("raygeneration")] void Name() { ... }`
- `ClosestHit` → `[shader("closesthit")] void Name(inout Payload p, in Attributes a) { ... }`
- `AnyHit` → `[shader("anyhit")] void Name(inout Payload p, in Attributes a) { ... }`
- `Miss` → `[shader("miss")] void Name(inout Payload p) { ... }`
- `Intersection` → `[shader("intersection")] void Name() { ... }`
- `Callable` → `[shader("callable")] void Name(inout Params p) { ... }`

**Intrinsic mapping:**
- `trace_ray(accel, flags, mask, sbtOffset, sbtStride, missIndex, ray, payload)` → `TraceRay(accel, flags, mask, sbtOffset, sbtStride, missIndex, ray, payload)`
- `report_hit(t, kind, attrs)` → `ReportHit(t, kind, attrs)`
- `ray_origin()` → `WorldRayOrigin()`
- `ray_direction()` → `WorldRayDirection()`
- etc.

**Compilation target:**
- Currently uses `vs_5_0` / `ps_5_0` etc. (line 713-730)
- DXR requires compiling to a **DXIL library** rather than individual shader objects: target `lib_6_3` (or higher)
- All RT shaders in a single OmegaSL library should be compiled into **one DXIL library blob** that DXR loads via `D3D12_DXIL_LIBRARY_DESC`

Update `compileShader()` and `compileShaderOnRuntime()`:
```cpp
else if(type == ast::ShaderDecl::RayGeneration ||
        type == ast::ShaderDecl::ClosestHit ||
        type == ast::ShaderDecl::AnyHit ||
        type == ast::ShaderDecl::Miss ||
        type == ast::ShaderDecl::Intersection ||
        type == ast::ShaderDecl::Callable) {
    out << "lib_6_3";
}
```

**Note**: This requires **DXC** (the modern HLSL compiler), not the legacy `fxc.exe`. The existing code already uses DXC via `hlslCodeOpts.dxc_cmd`. The SM 5.0 targets for raster shaders should eventually be upgraded to 6.x as well, but that's separate work.

### 2.2 GLSL/SPIR-V code generation (Vulkan / glslc)

In `gte/omegasl/src/GLSLCodeGen.cpp`:

**Shader entry point generation:**
- `RayGeneration` → `#extension GL_EXT_ray_tracing : require` + `layout(...) in; void main() { ... }`
- `ClosestHit` → `layout(location = N) rayPayloadInEXT Payload p;` + `hitAttributeEXT Attributes a;`
- `AnyHit` → Same structure with `rayPayloadInEXT`
- `Miss` → `layout(location = N) rayPayloadInEXT Payload p;`
- `Intersection` → `hitAttributeEXT Attributes a;` + `reportIntersectionEXT()`
- `Callable` → `layout(location = N) callableDataInEXT Params p;`

**Intrinsic mapping:**
- `trace_ray(...)` → `traceRayEXT(...)`
- `report_hit(...)` → `reportIntersectionEXT(...)`
- `ignore_hit()` → `ignoreIntersectionEXT()`
- `accept_hit()` → `terminateRayEXT()`
- `ray_origin()` → `gl_WorldRayOriginEXT`
- `ray_direction()` → `gl_WorldRayDirectionEXT`
- `ray_tmin()` → `gl_RayTminEXT`
- `ray_tcurrent()` → `gl_RayTmaxEXT`
- `instance_index()` → `gl_InstanceCustomIndexEXT`
- `primitive_index()` → `gl_PrimitiveID`

**Compilation:**
- glslc target: `--target-env=vulkan1.2` with `-fshader-stage=rgen` / `rchit` / `rahit` / `rmiss` / `rint` / `rcall`

### 2.3 Metal Shading Language code generation

In `gte/omegasl/src/MetalCodeGen.cpp`:

Metal's ray tracing model differs fundamentally from DXR/Vulkan. Metal uses **intersection functions** and **visible function tables** rather than SBT-based dispatching. Ray tracing shaders in Metal are effectively compute kernels that call `intersect()` on an `intersection_function_table`.

**Approach**: OmegaSL ray generation shaders compile to Metal compute kernels that use Metal's `ray_tracing` header:

```metal
#include <metal_raytracing>
using namespace metal::raytracing;

kernel void rayGenShader(
    instance_acceleration_structure accelStruct [[buffer(0)]],
    texture2d<float, access::write> output [[texture(0)]],
    uint2 tid [[thread_position_in_grid]]
) {
    intersector<triangle_data> inter;
    ray r = { origin, direction, tmin, tmax };
    auto result = inter.intersect(r, accelStruct);
    // ...
}
```

For hit/miss/intersection logic, Metal uses **visible function tables** and **intersection function tables** rather than separate shader stages. The OmegaSL codegen should:
- Compile `closesthit` / `anyhit` / `intersection` as Metal **visible functions** (`[[visible]]`)
- Compile `raygen` as a compute kernel
- Compile `miss` as a visible function
- Generate intersection function table setup code

### 2.4 Shader library serialization

In `CodeGen.h` `linkShaderObjects()` (line 100-172), extend the binary format to serialize RT shader metadata:

```cpp
else if(shader_data.type == OMEGASL_SHADER_RAYGEN ||
        shader_data.type == OMEGASL_SHADER_CLOSESTHIT || /* etc */) {
    // Write hit group association info (which closesthit + anyhit + intersection form a group)
    // Write max payload size
    // Write max attribute size
    // Write max recursion depth
}
```

**Files**: `HLSLCodeGen.cpp`, `GLSLCodeGen.cpp`, `MetalCodeGen.cpp`, `CodeGen.h`

---

## Phase 3: Ray Tracing Pipeline State Objects

### 3.1 Public API — `RayTracingPipelineDescriptor`

Add to `gte/include/omegaGTE/GEPipeline.h`:

```cpp
struct OMEGAGTE_EXPORT RayTracingPipelineDescriptor {
    OmegaCommon::String name;
    SharedHandle<GTEShader> rayGenShader;

    struct HitGroup {
        OmegaCommon::String name;
        SharedHandle<GTEShader> closestHitShader;     // required
        SharedHandle<GTEShader> anyHitShader;          // optional (nullptr)
        SharedHandle<GTEShader> intersectionShader;    // optional (nullptr, triangles use built-in)
    };
    OmegaCommon::Vector<HitGroup> hitGroups;

    OmegaCommon::Vector<SharedHandle<GTEShader>> missShaders;
    OmegaCommon::Vector<SharedHandle<GTEShader>> callableShaders;

    unsigned maxPayloadSize = 32;      // bytes
    unsigned maxAttributeSize = 8;     // bytes (float2 barycentrics = 8)
    unsigned maxRecursionDepth = 1;
};

typedef struct __GERayTracingPipelineState GERayTracingPipelineState;
```

### 3.2 Public API — Engine method

Add to `OmegaGraphicsEngine` in `GE.h`:

```cpp
virtual SharedHandle<GERayTracingPipelineState> makeRayTracingPipelineState(
    RayTracingPipelineDescriptor &desc) = 0;
```

### 3.3 Public API — Command buffer

Add to `GECommandBuffer` in `GECommandQueue.h`:

```cpp
/// @brief Sets a Ray Tracing Pipeline State for use with dispatchRays().
/// Must be called between startComputePass() and dispatchRays().
virtual void setRayTracingPipelineState(
    SharedHandle<GERayTracingPipelineState> &pipelineState) = 0;
```

Update `dispatchRays()` to use the bound RT pipeline's SBT.

### 3.4 D3D12 implementation — `ID3D12StateObject`

In `gte/src/d3d12/GED3D12Pipeline.cpp` (or a new `GED3D12RTPipeline.cpp`):

```cpp
struct GED3D12RayTracingPipelineState : public GERayTracingPipelineState {
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;

    // Shader Binding Table
    ComPtr<ID3D12Resource> sbtBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS rayGenRecord;
    D3D12_GPU_VIRTUAL_ADDRESS missTableStart;
    UINT64 missRecordStride;
    UINT missRecordCount;
    D3D12_GPU_VIRTUAL_ADDRESS hitGroupTableStart;
    UINT64 hitGroupRecordStride;
    UINT hitGroupCount;
    D3D12_GPU_VIRTUAL_ADDRESS callableTableStart;
    UINT64 callableRecordStride;
    UINT callableCount;
};
```

**Pipeline creation flow:**
1. Build `D3D12_STATE_SUBOBJECT` array:
   - `D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY` — The compiled DXIL library containing all RT shaders
   - `D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP` — One per hit group from the descriptor
   - `D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG` — Payload + attribute sizes
   - `D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG` — Max recursion depth
   - `D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE` — From shader resource layouts
2. Call `d3d12_device->CreateStateObject(&desc, IID_PPV_ARGS(&stateObject))`
3. Query `ID3D12StateObjectProperties` for shader identifiers
4. Build SBT buffer (see Phase 4)

### 3.5 Vulkan implementation — `vkCreateRayTracingPipelinesKHR`

In a new or existing Vulkan pipeline file:

```cpp
struct GEVulkanRayTracingPipelineState : public GERayTracingPipelineState {
    GEVulkanEngine *engine;
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;

    // SBT buffer
    VkBuffer sbtBuffer;
    VmaAllocation sbtAllocation;
    VkStridedDeviceAddressRegionKHR raygenRegion;
    VkStridedDeviceAddressRegionKHR missRegion;
    VkStridedDeviceAddressRegionKHR hitRegion;
    VkStridedDeviceAddressRegionKHR callableRegion;
};
```

**Pipeline creation flow:**
1. Build `VkRayTracingShaderGroupCreateInfoKHR` array (raygen group, hit groups, miss groups, callable groups)
2. Build `VkRayTracingPipelineCreateInfoKHR` with shader stages and groups
3. Call `vkCreateRayTracingPipelinesKHR()`
4. Query shader group handles via `vkGetRayTracingShaderGroupHandlesKHR()`
5. Build SBT buffer (see Phase 4)

Need to add function pointer:
```cpp
PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKhr = nullptr;
PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKhr = nullptr;
```

Load these in `GEVulkanEngine` constructor alongside the existing RT function pointers.

### 3.6 Metal implementation — Compute pipeline with visible function tables

Metal doesn't have a dedicated RT pipeline object. Instead:

```cpp
struct GEMetalRayTracingPipelineState : public GERayTracingPipelineState {
    NSSmartPtr computePipeline;        // MTLComputePipelineState (raygen kernel)
    NSSmartPtr intersectionFnTable;    // MTLIntersectionFunctionTable
    NSSmartPtr visibleFnTable;         // MTLVisibleFunctionTable (hit/miss/callable)
};
```

**Creation flow:**
1. Compile raygen as compute kernel
2. Create `MTLComputePipelineDescriptor` with linked functions
3. Build `MTLIntersectionFunctionTable` from intersection functions
4. Build `MTLVisibleFunctionTable` from hit/miss/callable functions
5. Create pipeline via `newComputePipelineStateWithDescriptor:options:reflection:error:`

**Files**: `GEPipeline.h`, `GE.h`, `GECommandQueue.h`, + backend pipeline files

---

## Phase 4: Shader Binding Tables

The SBT is the data structure that connects geometry instances to their shader programs during ray traversal.

### 4.1 D3D12 SBT construction

After creating the `ID3D12StateObject`, build the SBT:

1. Get shader identifiers (32-byte handles) via `stateObjectProps->GetShaderIdentifier(exportName)`
2. Allocate a GPU buffer large enough for all records (raygen + miss table + hit group table + callable table)
3. Write shader identifiers into the buffer at aligned offsets (`D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT` = 64, record stride = `D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT` = 32 aligned up to include local root arguments if any)
4. Store the GPU virtual addresses and strides in the pipeline state object

### 4.2 Vulkan SBT construction

After creating the ray tracing pipeline:

1. Get shader group handles (32 bytes each) via `vkGetRayTracingShaderGroupHandlesKHR`
2. Allocate a buffer with `VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
3. Write group handles at aligned offsets (`shaderGroupHandleAlignment` from `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`)
4. Compute `VkStridedDeviceAddressRegionKHR` for each table (raygen, miss, hit, callable)

### 4.3 Metal "SBT" (visible function tables)

Metal uses function tables instead of an explicit SBT buffer:

1. Create `MTLVisibleFunctionTable` with entries for each hit/miss function
2. Create `MTLIntersectionFunctionTable` for custom intersection functions
3. Set tables on the compute command encoder via `setVisibleFunctionTable:atBufferIndex:` and `setIntersectionFunctionTable:atBufferIndex:`

**Files**: Backend pipeline implementation files

---

## Phase 5: Complete `dispatchRays()` on All Backends

### 5.1 D3D12

Replace the current stub in `GED3D12CommandQueue.cpp:882`:

```cpp
void GED3D12CommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "Must be in a compute pass to dispatch rays");
    assert(currentRTPipeline && "Must bind a ray tracing pipeline before dispatching rays");

    auto *rtPipeline = currentRTPipeline; // GED3D12RayTracingPipelineState*

    commandList->SetPipelineState1(rtPipeline->stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC desc = {};
    desc.Width = x;
    desc.Height = y;
    desc.Depth = z;

    desc.RayGenerationShaderRecord.StartAddress = rtPipeline->rayGenRecord;
    desc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

    desc.MissShaderTable.StartAddress = rtPipeline->missTableStart;
    desc.MissShaderTable.SizeInBytes = rtPipeline->missRecordStride * rtPipeline->missRecordCount;
    desc.MissShaderTable.StrideInBytes = rtPipeline->missRecordStride;

    desc.HitGroupTable.StartAddress = rtPipeline->hitGroupTableStart;
    desc.HitGroupTable.SizeInBytes = rtPipeline->hitGroupRecordStride * rtPipeline->hitGroupCount;
    desc.HitGroupTable.StrideInBytes = rtPipeline->hitGroupRecordStride;

    desc.CallableShaderTable.StartAddress = rtPipeline->callableTableStart;
    desc.CallableShaderTable.SizeInBytes = rtPipeline->callableRecordStride * rtPipeline->callableCount;
    desc.CallableShaderTable.StrideInBytes = rtPipeline->callableRecordStride;

    commandList->DispatchRays(&desc);
}
```

### 5.2 Vulkan

Replace the stub in `GEVulkanCommandQueue.cpp:1062`:

```cpp
void GEVulkanCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "Must be in compute pass to dispatch rays");
    auto *engine = parentQueue->engine;
    auto *rtPipeline = currentRTPipeline; // GEVulkanRayTracingPipelineState*

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline->pipeline);
    // Bind descriptor sets for RT pipeline...

    engine->vkCmdTraceRaysKhr(
        commandBuffer,
        &rtPipeline->raygenRegion,
        &rtPipeline->missRegion,
        &rtPipeline->hitRegion,
        &rtPipeline->callableRegion,
        x, y, z
    );
}
```

### 5.3 Metal

Replace the stub in `GEMetalCommandQueue.mm:508`:

```objc
void GEMetalCommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    auto *rtPipeline = currentRTPipeline; // GEMetalRayTracingPipelineState*
    auto cp = /* current compute command encoder */;

    [cp setComputePipelineState:NSOBJECT_OBJC_BRIDGE(id<MTLComputePipelineState>, rtPipeline->computePipeline.handle())];
    [cp setVisibleFunctionTable:NSOBJECT_OBJC_BRIDGE(id<MTLVisibleFunctionTable>, rtPipeline->visibleFnTable.handle()) atBufferIndex:...];
    [cp setIntersectionFunctionTable:NSOBJECT_OBJC_BRIDGE(id<MTLIntersectionFunctionTable>, rtPipeline->intersectionFnTable.handle()) atBufferIndex:...];

    MTLSize threadsPerGrid = MTLSizeMake(x, y, z);
    MTLSize threadsPerGroup = MTLSizeMake(8, 8, 1); // or from pipeline
    [cp dispatchThreads:threadsPerGrid threadsPerThreadgroup:threadsPerGroup];
}
```

**Files**: `GED3D12CommandQueue.cpp`, `GEVulkanCommandQueue.cpp`, `GEMetalCommandQueue.mm`

---

## Phase 6: Top-Level Acceleration Structures (TLAS) and Instance Descriptors

The current API only supports bottom-level acceleration structures (BLAS). For real-world ray tracing, we need TLAS that reference instances of BLAS with per-instance transforms.

### 6.1 Extend public API

Add to `GE.h`:

```cpp
struct OMEGAGTE_EXPORT GEAccelerationStructInstance {
    float transform[3][4];                           // 3x4 row-major affine transform
    unsigned instanceID : 24;                        // user ID
    unsigned instanceMask : 8;                       // visibility mask
    unsigned instanceContributionToHitGroupIndex : 24;
    unsigned flags : 8;                              // front/back face cull, force opaque, etc.
    SharedHandle<GEAccelerationStruct> blas;
};

// Extend GEAccelerationStructDescriptor to support TLAS:
struct GEAccelerationStructDescriptor {
    enum Level { BottomLevel, TopLevel };
    Level level = BottomLevel;

    // For BottomLevel: existing geometry data
    OmegaCommon::Vector<Geometry> data;

    // For TopLevel: instance array
    OmegaCommon::Vector<GEAccelerationStructInstance> instances;
};
```

### 6.2 Backend implementations

**D3D12**: Use `D3D12_RAYTRACING_INSTANCE_DESC` written to a GPU buffer, build with `D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL`.

**Vulkan**: Use `VkAccelerationStructureInstanceKHR` written to a device buffer, build with `VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR`.

**Metal**: Use `MTLInstanceAccelerationStructureDescriptor` with `MTLAccelerationStructureInstanceDescriptor` array.

**Files**: `GE.h`, all three backend engine files, all three backend command queue files

---

## Phase 7: Testing

### 7.1 OmegaSL compiler tests

Add test `.omegasl` files under `gte/omegasl/tests/` with ray tracing shaders:
- A simple raygen shader that writes a solid color
- A closesthit/miss pair
- Verify compilation to HLSL, GLSL, and MSL

### 7.2 Integration tests

Add a new test app under `gte/tests/directx/` (and equivalent for Vulkan):
- Create a triangle BLAS
- Build a TLAS with one instance
- Create a simple raygen + closesthit + miss pipeline
- Dispatch rays to a 2D output texture
- Read back and verify non-zero output

### 7.3 Feature check test

Verify that on hardware without RT support:
- `features.raytracing` returns `false`
- `allocateAccelerationStructure()` returns `nullptr`
- `makeRayTracingPipelineState()` returns `nullptr`
- No crash, no undefined behavior

---

## File Change Summary

| File | Changes |
|---|---|
| **Public API** | |
| `gte/include/omegaGTE/GE.h` | Remove `#ifdef` gates; add TLAS instance types; add `makeRayTracingPipelineState` |
| `gte/include/omegaGTE/GEPipeline.h` | Add `RayTracingPipelineDescriptor`, `GERayTracingPipelineState` |
| `gte/include/omegaGTE/GECommandQueue.h` | Remove `#ifdef` gates; add `setRayTracingPipelineState` |
| `gte/include/OmegaGTE.h` | Thread device features into engine |
| `gte/include/omegasl.h` | Add RT shader types to enum; add RT shader metadata structs |
| **OmegaSL Compiler** | |
| `gte/omegasl/src/Toks.def` | Add `KW_RAYGEN` through `KW_CALLABLE`; RT attribute names |
| `gte/omegasl/src/Lexer.cpp` | Register new keywords |
| `gte/omegasl/src/AST.h` | Extend `ShaderDecl::Type`; add RT builtin types/functions |
| `gte/omegasl/src/AST.def` | Add RT builtin macro names |
| `gte/omegasl/src/AST.cpp` | Initialize RT builtins |
| `gte/omegasl/src/Parser.cpp` | Parse RT shader declarations |
| `gte/omegasl/src/Sema.cpp` | Validate RT shader semantics |
| `gte/omegasl/src/HLSLCodeGen.cpp` | HLSL RT codegen; `lib_6_3` compilation target |
| `gte/omegasl/src/GLSLCodeGen.cpp` | GLSL RT codegen with `GL_EXT_ray_tracing` |
| `gte/omegasl/src/MetalCodeGen.cpp` | Metal RT codegen (compute + visible functions) |
| `gte/omegasl/src/CodeGen.h` | RT shader metadata in library serialization |
| **D3D12 Backend** | |
| `gte/src/d3d12/GED3D12.h` | Remove `#ifdef`; store device features |
| `gte/src/d3d12/GED3D12.cpp` | Remove `#ifdef`; add runtime guards; add `makeRayTracingPipelineState`; TLAS support |
| `gte/src/d3d12/GED3D12CommandQueue.h` | Remove `#ifdef`; add `setRayTracingPipelineState` |
| `gte/src/d3d12/GED3D12CommandQueue.cpp` | Remove `#ifdef`; implement `dispatchRays` with populated SBT |
| `gte/src/d3d12/GED3D12Pipeline.cpp` | `ID3D12StateObject` creation and SBT building |
| **Vulkan Backend** | |
| `gte/src/vulkan/GEVulkan.h` | Remove `#ifdef`; add RT pipeline function pointers |
| `gte/src/vulkan/GEVulkan.cpp` | Remove `#ifdef`; load new fn ptrs; add `makeRayTracingPipelineState`; TLAS support |
| `gte/src/vulkan/GEVulkanCommandQueue.h` | Remove `#ifdef`; add `setRayTracingPipelineState` |
| `gte/src/vulkan/GEVulkanCommandQueue.cpp` | Remove `#ifdef`; implement `dispatchRays` with populated SBT |
| **Metal Backend** | |
| `gte/src/metal/GEMetal.h` | Remove `#ifdef` |
| `gte/src/metal/GEMetal.mm` | Remove `#ifdef`; add runtime guards; add `makeRayTracingPipelineState`; TLAS support |
| `gte/src/metal/GEMetalCommandQueue.h` | Remove `#ifdef`; add `setRayTracingPipelineState` |
| `gte/src/metal/GEMetalCommandQueue.mm` | Remove `#ifdef`; implement `dispatchRays` with function tables |
| **Docs** | |
| `gte/docs/API.rst` | Update RT section, remove `#ifdef` examples |
| `wtk/docs/OmegaGTEView-Proposal.md` | Replace macro with runtime check |

## Implementation Order

The phases have dependencies:

```
Phase 0 (remove macro) ─── can be done independently, first
    │
Phase 1 (OmegaSL types) ─── prerequisite for everything below
    │
Phase 2 (OmegaSL codegen) ─── prerequisite for pipeline creation
    │
Phase 3 (RT pipeline objects) ──┐
    │                            │
Phase 4 (SBT construction) ─────┤── these three are tightly coupled
    │                            │   and should be done per-backend
Phase 5 (dispatchRays) ─────────┘
    │
Phase 6 (TLAS / instances) ─── builds on working BLAS + dispatch
    │
Phase 7 (testing) ─── validates everything
```

Phase 0 is safe to land independently. Phases 1-2 are OmegaSL-only changes. Phases 3-5 should be implemented one backend at a time (D3D12 first, since DXR is the most mature, then Vulkan, then Metal).
