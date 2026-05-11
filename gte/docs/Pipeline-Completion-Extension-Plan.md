# Pipeline Completion Extension Plan

## Goal

Close every gap in OmegaGTE's three pipeline types (Render, Compute). This document inventories what each pipeline can and cannot do today, what each backend (D3D12, Metal, Vulkan) supports natively, and proposes the minimal set of extensions to bring all three to feature parity.

Ray tracing is covered separately in `Raytracing-Full-Implementation-Plan.md` and is not repeated here.

---

## Current State

### Render Pipeline

| Capability | Public API | D3D12 | Metal | Vulkan |
|---|---|---|---|---|
| Non-indexed draw | `drawPolygons()` | Yes | Yes | Yes |
| Indexed draw | **No** | Native | Native | Native |
| Instanced draw | **No** | Native | Native | Native |
| Indirect draw | **No** | Native | Native | Native |
| Index buffer binding | **No** | Native | Native | Native |
| Vertex input layout | **Implicit** (hardcoded per backend) | Hardcoded `InputLayout` | Hardcoded vertex descriptor | Hardcoded `VkVertexInputState` |
| Blend state | **Hardcoded** (alpha blend on D3D12, default elsewhere) | Hardcoded in PSO creation | No blend state in descriptor | Hardcoded in pipeline create |
| Multiple color attachments | **No** (single `colorAttachment` pointer) | Native (8 MRTs) | Native (8 MRTs) | Native (per spec limit) |
| Stencil reference | `setStencilRef()` | Yes | Yes | Yes |
| Dynamic viewport/scissor | `setViewports()` / `setScissorRects()` | Yes | Yes | Yes |
| Polygon types | Triangle, TriangleStrip | Yes | Yes | Yes |
| Point / Line primitives | **No** | Native | Native | Native |
| Depth bias (dynamic) | In `RenderPipelineDescriptor` (static) | Static only | Static only | Static only |

**Summary**: The render pipeline works for basic triangle rendering with a single color attachment and hardcoded blend state. It cannot do indexed draws, instanced draws, indirect draws, or multi-render-target output. Vertex input layout and blend state are baked into each backend rather than driven by the descriptor.

### Compute Pipeline

| Capability | Public API | D3D12 | Metal | Vulkan |
|---|---|---|---|---|
| Dispatch by threadgroup count | `dispatchThreadgroups()` | Yes | Yes | Yes |
| Dispatch by total thread count | `dispatchThreads()` | Yes | Yes | Yes |
| Indirect dispatch | **No** | Native | Native | Native |
| Buffer resource binding | `bindResourceAtComputeShader(buffer)` | Yes | Yes | Yes |
| Texture resource binding | `bindResourceAtComputeShader(texture)` | Yes | Yes | Yes |
| Accel struct binding | `bindResourceAtComputeShader(accelStruct)` | Yes | Yes | Yes |
| Push constants / root constants | **No** | Native (root constants) | Native (setBytes) | Native (push constants) |

**Summary**: The compute pipeline is nearly complete. The main gaps are indirect dispatch and push constants (small, frequently-updated uniforms without a buffer allocation).

### Blit Pass

| Capability | Public API | D3D12 | Metal | Vulkan |
|---|---|---|---|---|
| Texture → Texture (full) | `copyTextureToTexture()` | Yes | Yes | Yes |
| Texture → Texture (region) | `copyTextureToTexture(region)` | Yes | Incorrect* | Yes |
| Buffer → Buffer | **No** | Native | Native | Native |
| Buffer → Texture | **No** | Native | Native | Native |
| Texture → Buffer (readback) | **No** | Native | Native | Native |
| Mipmap generation | **No** | Manual (compute/blit chain) | `generateMipmapsForTexture:` | Manual (blit chain) |
| Texture clear / fill | **No** | `ClearRenderTargetView` | `MTLBlitEncoder fill` | `vkCmdClearColorImage` |
| Buffer fill | **No** | Manual | `MTLBlitEncoder fillBuffer:` | `vkCmdFillBuffer` |
| Texture resolve (MSAA) | Only via render pass `multisampleResolve` | `ResolveSubresource` | Render pass resolve | `vkCmdResolveImage` |
| Scaled blit / format convert | **No** | Manual (render quad) | Manual (render quad) | `vkCmdBlitImage` |

\* Metal's region copy currently copies entire mipmaps rather than the specified region.

**Summary**: The blit pass only supports texture-to-texture copies. All buffer transfers, readback, mipmap generation, clears, fills, and format-converting blits are missing.

---

## Non-Goals

- Ray tracing pipelines, SBT, OmegaSL RT shader types (covered in `Raytracing-Full-Implementation-Plan.md`)
- Mesh shading pipelines (requires OmegaSL mesh/amplification shader support first)
- Tessellation hull/domain shader pipeline extensions (existing tessellation works; hull/domain are OmegaSL-level concerns)
- Async compute queue types (the command queue abstraction already supports multiple queues; queue type selection is a separate concern)
- Bindless / descriptor indexing (covered in `GTEDeviceFeatures-Extension-Plan.md` as a device feature; pipeline changes would follow)

---

## Extension 1: Render Pipeline Completion

### 1.1 Vertex Input Layout in `RenderPipelineDescriptor` ✅ Implemented

Today each backend hardcodes the vertex input layout during PSO creation. The descriptor should describe vertex attributes so pipelines can be created for arbitrary vertex formats.

**Add to `GEPipeline.h`:**

```cpp
enum class VertexFormat : uint8_t {
    Float,       // 4 bytes
    Float2,      // 8 bytes
    Float3,      // 12 bytes
    Float4,      // 16 bytes
    Int,         // 4 bytes
    Int2,        // 8 bytes
    Int3,        // 12 bytes
    Int4,        // 16 bytes
    UNorm8x4,   // 4 bytes, normalized
    SNorm8x4,   // 4 bytes, normalized
    UShort2,    // 4 bytes
    UShort4,    // 8 bytes
    Half2,      // 4 bytes
    Half4,      // 8 bytes
};

enum class VertexStepFunction : uint8_t {
    PerVertex,
    PerInstance
};

struct VertexBufferLayout {
    unsigned stride = 0;
    VertexStepFunction stepFunction = VertexStepFunction::PerVertex;
    unsigned stepRate = 1;  // 1 for per-vertex; instance divisor for per-instance
};

struct VertexAttribute {
    unsigned bufferIndex = 0;   // which vertex buffer slot
    unsigned offset = 0;        // byte offset within the buffer element
    VertexFormat format = VertexFormat::Float4;
    unsigned shaderLocation = 0; // maps to OmegaSL input location
};

struct VertexInputDescriptor {
    OmegaCommon::Vector<VertexBufferLayout> bufferLayouts;
    OmegaCommon::Vector<VertexAttribute> attributes;
};
```

**Add to `RenderPipelineDescriptor`:**

```cpp
VertexInputDescriptor vertexInputDescriptor;
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `VertexFormat` | `DXGI_FORMAT` | `MTLVertexFormat` | `VkFormat` |
| `VertexBufferLayout` | `D3D12_INPUT_LAYOUT_DESC` stride + classification | `MTLVertexBufferLayoutDescriptor` | `VkVertexInputBindingDescription` |
| `VertexAttribute` | `D3D12_INPUT_ELEMENT_DESC` | `MTLVertexAttributeDescriptor` | `VkVertexInputAttributeDescription` |
| `PerInstance` step | `D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA` | `MTLVertexStepFunctionPerInstance` | `VK_VERTEX_INPUT_RATE_INSTANCE` |

All three backends support this identically. No compatibility concerns.

### 1.2 Blend State in `RenderPipelineDescriptor` ✅ Implemented

The current D3D12 backend hardcodes SrcAlpha/InvSrcAlpha blending. The descriptor should expose blend configuration.

**Add to `GEPipeline.h`:**

```cpp
enum class BlendFactor : uint8_t {
    Zero,
    One,
    SrcColor,
    InvSrcColor,
    SrcAlpha,
    InvSrcAlpha,
    DestColor,
    InvDestColor,
    DestAlpha,
    InvDestAlpha,
    SrcAlphaSaturated,
    // Dual-source (requires GTEDeviceFeatures::dualSourceBlending)
    Src1Color,
    InvSrc1Color,
    Src1Alpha,
    InvSrc1Alpha
};

enum class BlendOperation : uint8_t {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max
};

enum ColorWriteMask : uint8_t {
    ColorWriteNone  = 0,
    ColorWriteRed   = 1 << 0,
    ColorWriteGreen = 1 << 1,
    ColorWriteBlue  = 1 << 2,
    ColorWriteAlpha = 1 << 3,
    ColorWriteAll   = 0xF
};

struct BlendDescriptor {
    bool blendEnabled = false;
    BlendFactor srcColorFactor = BlendFactor::SrcAlpha;
    BlendFactor destColorFactor = BlendFactor::InvSrcAlpha;
    BlendOperation colorOp = BlendOperation::Add;
    BlendFactor srcAlphaFactor = BlendFactor::One;
    BlendFactor destAlphaFactor = BlendFactor::InvSrcAlpha;
    BlendOperation alphaOp = BlendOperation::Add;
    uint8_t writeMask = ColorWriteAll;
};
```

**Add to `RenderPipelineDescriptor`:**

```cpp
/// Per-attachment blend state. Index 0 corresponds to color attachment 0, etc.
/// If empty, blending is disabled (opaque).
OmegaCommon::Vector<BlendDescriptor> colorBlendDescriptors;
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `BlendFactor` | `D3D12_BLEND` | `MTLBlendFactor` | `VkBlendFactor` |
| `BlendOperation` | `D3D12_BLEND_OP` | `MTLBlendOperation` | `VkBlendOp` |
| `ColorWriteMask` | `D3D12_COLOR_WRITE_ENABLE` | `MTLColorWriteMask` | `VkColorComponentFlags` |
| Per-attachment | `BlendState.RenderTarget[i]` | `colorAttachments[i]` | `VkPipelineColorBlendAttachmentState[i]` |

All three backends support per-attachment blend state natively. D3D12 and Vulkan call it "independent blend"; Metal always supports it.

### 1.3 Indexed Drawing ✅ Implemented

**Add to `GECommandBuffer` (private, render pass section):**

```cpp
virtual void setIndexBuffer(SharedHandle<GEBuffer> & buffer, 
                            IndexType indexType = IndexType::UInt32) = 0;
virtual void drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                 unsigned indexCount, size_t startIndex,
                                 int baseVertex = 0) = 0;
```

**Add to `GERenderTarget::CommandBuffer` (public):**

```cpp
enum class IndexType : uint8_t { UInt16, UInt32 };

void setIndexBuffer(SharedHandle<GEBuffer> & buffer, IndexType indexType = IndexType::UInt32);
void drawIndexedPolygons(PolygonType polygonType, unsigned indexCount, 
                         size_t startIndex, int baseVertex = 0);
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `setIndexBuffer()` | `IASetIndexBuffer()` | `drawIndexedPrimitives(...indexBuffer:)` (passed at draw time) | `vkCmdBindIndexBuffer()` |
| `drawIndexedPolygons()` | `DrawIndexedInstanced(count, 1, start, base, 0)` | `drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:` | `vkCmdDrawIndexed()` |
| `UInt16` | `DXGI_FORMAT_R16_UINT` | `MTLIndexTypeUInt16` | `VK_INDEX_TYPE_UINT16` |
| `UInt32` | `DXGI_FORMAT_R32_UINT` | `MTLIndexTypeUInt32` | `VK_INDEX_TYPE_UINT32` |

All three backends support indexed drawing identically. No compatibility concerns.

### 1.4 Instanced Drawing ✅ Implemented

**Add to `GECommandBuffer` (private):**

```cpp
virtual void drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                    unsigned vertexCount, size_t startIdx,
                                    unsigned instanceCount, unsigned firstInstance = 0) = 0;
virtual void drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                           unsigned indexCount, size_t startIndex,
                                           int baseVertex, unsigned instanceCount,
                                           unsigned firstInstance = 0) = 0;
```

**Add to `GERenderTarget::CommandBuffer` (public):**

```cpp
void drawPolygonsInstanced(PolygonType polygonType, unsigned vertexCount, size_t start,
                           unsigned instanceCount, unsigned firstInstance = 0);
void drawIndexedPolygonsInstanced(PolygonType polygonType, unsigned indexCount,
                                   size_t startIndex, int baseVertex,
                                   unsigned instanceCount, unsigned firstInstance = 0);
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Non-indexed instanced | `DrawInstanced()` | `drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:` | `vkCmdDraw(vert, inst, firstVert, firstInst)` |
| Indexed instanced | `DrawIndexedInstanced()` | `drawIndexedPrimitives:...:instanceCount:baseVertex:baseInstance:` | `vkCmdDrawIndexed(idx, inst, firstIdx, baseVtx, firstInst)` |

All three backends support instancing natively. The `firstInstance` parameter is universally supported on desktop-class hardware (Vulkan guarantees it via `drawIndirectFirstInstance` on desktop).

### 1.5 Indirect Drawing ✅ Implemented

Indirect drawing reads draw parameters from a GPU buffer, enabling GPU-driven rendering pipelines.

**Add to `GECommandBuffer` (private):**

```cpp
virtual void drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                   SharedHandle<GEBuffer> & argumentBuffer,
                                   size_t argumentBufferOffset) = 0;
virtual void drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                          SharedHandle<GEBuffer> & argumentBuffer,
                                          size_t argumentBufferOffset) = 0;
```

**Add to `GERenderTarget::CommandBuffer` (public):**

```cpp
void drawPolygonsIndirect(PolygonType polygonType,
                          SharedHandle<GEBuffer> & argumentBuffer,
                          size_t argumentBufferOffset);
void drawIndexedPolygonsIndirect(PolygonType polygonType,
                                  SharedHandle<GEBuffer> & argumentBuffer,
                                  size_t argumentBufferOffset);
```

**Argument buffer layout**: Must match the native indirect draw structure. All three APIs use the same layout:

```cpp
struct DrawIndirectCommand {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};

struct DrawIndexedIndirectCommand {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  baseVertex;
    uint32_t firstInstance;
};
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Non-indexed indirect | `ExecuteIndirect()` with draw command sig | `drawPrimitives:indirectBuffer:indirectBufferOffset:` | `vkCmdDrawIndirect()` |
| Indexed indirect | `ExecuteIndirect()` with draw-indexed command sig | `drawIndexedPrimitives:...indirectBuffer:...` | `vkCmdDrawIndexedIndirect()` |

**Compatibility notes:**
- **D3D12**: Requires an `ID3D12CommandSignature` created with `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW` / `_DRAW_INDEXED`. This is a one-time setup cost per engine instance.
- **Metal**: Direct support via `MTLRenderCommandEncoder` indirect draw methods. No additional setup.
- **Vulkan**: Direct support via `vkCmdDrawIndirect` / `vkCmdDrawIndexedIndirect`. Multi-draw-indirect (multiple draws from one buffer) requires `multiDrawIndirect` feature — propose single-draw-indirect first.

### 1.6 Multiple Color Attachments (MRT)  ✅

**Modify `GERenderPassDescriptor`:**

```cpp
struct GERenderPassDescriptor {
    GENativeRenderTarget *nRenderTarget = nullptr;
    GETextureRenderTarget *tRenderTarget = nullptr;
    typedef GERenderTarget::RenderPassDesc::ColorAttachment ColorAttachment;
    typedef GERenderTarget::RenderPassDesc::DepthStencilAttachment DepthStencilAttachment;

    // Change from: ColorAttachment *colorAttachment;
    // To:
    OmegaCommon::Vector<ColorAttachment> colorAttachments;

    DepthStencilAttachment depthStencilAttachment;
    bool multisampleResolve = false;
    typedef GERenderTarget::RenderPassDesc::MultisampleResolveDesc MultisampleResolveDesc;
    MultisampleResolveDesc resolveDesc;
};
```

**Modify `RenderPipelineDescriptor`:**

```cpp
// Change from: PixelFormat colorPixelFormat = PixelFormat::RGBA8Unorm;
// To:
OmegaCommon::Vector<PixelFormat> colorPixelFormats = { PixelFormat::RGBA8Unorm };
```

This pairs with the `colorBlendDescriptors` vector from 1.2 — one blend state per color attachment.

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Multiple color attachments | `D3D12_GRAPHICS_PIPELINE_STATE_DESC.NumRenderTargets` + `RTVFormats[i]` | `MTLRenderPipelineDescriptor.colorAttachments[i]` | `VkRenderPass` with multiple color attachment refs |
| Max attachments | 8 | 8 | Device-dependent (typically 8) |

All three backends support at least 8 simultaneous color attachments.

### 1.7 Additional Polygon Types  ✅

**Extend `GERenderTarget::CommandBuffer::PolygonType`:**

```cpp
using PolygonType = enum : uint8_t {
    Triangle,
    TriangleStrip,
    Line,
    LineStrip,
    Point
};
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `Line` | `D3D_PRIMITIVE_TOPOLOGY_LINELIST` | `MTLPrimitiveTypeLine` | `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` |
| `LineStrip` | `D3D_PRIMITIVE_TOPOLOGY_LINESTRIP` | `MTLPrimitiveTypeLineStrip` | `VK_PRIMITIVE_TOPOLOGY_LINE_STRIP` |
| `Point` | `D3D_PRIMITIVE_TOPOLOGY_POINTLIST` | `MTLPrimitiveTypePoint` | `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` |

All three backends support all primitive types natively.

**Compatibility note**: Wide lines (width > 1.0) are **not** supported on D3D12 or Metal. Only Vulkan supports them, gated by `VkPhysicalDeviceFeatures.wideLines`. Point size is controllable via the vertex shader on all three backends.

---

## Extension 2: Compute Pipeline Completion

### 2.1 Indirect Dispatch ✅ Implemented

**Add to `GECommandBuffer` (public, compute pass section):**

```cpp
/// @brief Dispatches threadgroups using arguments stored in a GPU buffer.
/// The buffer must contain three uint32_t values: groupCountX, groupCountY, groupCountZ.
/// @param argumentBuffer The buffer containing dispatch arguments.
/// @param argumentBufferOffset Byte offset into the argument buffer.
virtual void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) = 0;
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `dispatchThreadgroupsIndirect()` | `ExecuteIndirect()` with dispatch command sig | `dispatchThreadgroups:indirectBuffer:indirectBufferOffset:` | `vkCmdDispatchIndirect()` |

**Argument buffer layout** (identical across all three APIs):

```cpp
struct DispatchIndirectCommand {
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
```

**Compatibility notes:**
- **D3D12**: Requires `ID3D12CommandSignature` with `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH`. Same pattern as indirect draw.
- **Metal**: Native support on all Metal devices.
- **Vulkan**: Native support on all Vulkan 1.0 devices.

No compatibility concerns. All three backends support this universally.

### 2.2 Push Constants

Small, frequently-updated constants (≤128 bytes) that avoid buffer allocation overhead.

**Add to `GECommandBuffer` (public, compute pass section):**

```cpp
/// @brief Sets push constant data for the current compute pipeline.
/// @param data Pointer to the constant data.
/// @param size Size in bytes (max 128).
/// @param offset Byte offset into the push constant range.
virtual void setComputeConstants(const void *data, unsigned size, unsigned offset = 0) = 0;
```

**Add to `GERenderTarget::CommandBuffer` (public, for render pass):**

```cpp
/// @brief Sets push constant data for the current render pipeline.
/// @param data Pointer to the constant data.
/// @param size Size in bytes (max 128).
/// @param offset Byte offset into the push constant range.
void setRenderConstants(const void *data, unsigned size, unsigned offset = 0);
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Push constants | Root constants (`SetGraphicsRoot32BitConstants` / `SetComputeRoot32BitConstants`) | `setBytes:length:atIndex:` on encoder | `vkCmdPushConstants()` |
| Max size | 64 DWORDs (256 bytes) via root signature | 4KB per `setBytes` call | `maxPushConstantsSize` (min 128 bytes, typically 256) |

**Compatibility notes:**
- **D3D12**: Root constants consume root signature space. The pipeline descriptor must reserve a root parameter slot. This interacts with the existing resource binding model — root signature layout needs to know push constants exist at PSO creation time.
- **Metal**: `setBytes:` is trivial and has no PSO interaction. The buffer index must not conflict with bound resources.
- **Vulkan**: Push constants are declared in `VkPipelineLayout`. The pipeline layout must include a `VkPushConstantRange` at creation time.

**OmegaSL integration**: Push constants need a way to be declared in the shader language. Propose a `constant` keyword:

```
constant PerFrameData {
    float4x4 viewProjection;
    float time;
};
```

This compiles to a root constant range (D3D12), a `setBytes` buffer index (Metal), or a push constant block (Vulkan). The compiler's layout extraction already knows about resource bindings; push constants would be a new layout category. The OmegaSL lib will get use the omegasl_shader_constant_desc to describe the layout.

**Recommendation**: Push constants require OmegaSL changes. Propose as a paired feature with OmegaSL push constant support rather than a standalone command buffer extension.

### 2.3 `ComputePipelineDescriptor` — Threadgroup Size Override

Add a Device Feature bit, supports threadgroup size override.

Currently the threadgroup size is defined in the OmegaSL shader via `numthreads` attributes. Some use cases need to override the compiled threadgroup size at pipeline creation time (e.g., tuning for different GPU architectures).

**Add to `ComputePipelineDescriptor`:**

```cpp
struct ComputePipelineDescriptor {
    OmegaCommon::String name;
    SharedHandle<GTEShader> computeFunc;

    // Optional threadgroup size override. If all zero, uses the shader's declared size.
    unsigned overrideThreadgroupSizeX = 0;
    unsigned overrideThreadgroupSizeY = 0;
    unsigned overrideThreadgroupSizeZ = 0;
};
```

**Backend mapping:**

| Backend | Support |
|---|---|
| D3D12 | No native override — would require shader recompilation or specialization constants |
| Metal | `MTLComputePipelineDescriptor` does not support override; threadgroup size is in the shader |
| Vulkan | Specialization constants (`VkSpecializationInfo`) can override `local_size_x/y/z` if declared |

**Compatibility concern**: Only Vulkan supports this cleanly via specialization constants. D3D12 and Metal would require shader recompilation. **Recommend deferring** this until OmegaSL supports specialization constants, then expose threadgroup size as a specialization parameter.

---

## Extension 3: Blits

### Design Rationale

Today, blits are raw device operations — `startBlitPass()` encodes hardware copy commands directly without pipeline state. This is correct for simple copies: all three APIs provide fixed-function copy commands that don't need a pipeline.

However, several common GPU operations require a *programmable* blit — a full-screen pass that reads from one texture and writes to another with a shader in between:

- **Format conversion** (e.g., RGBA16Float → RGBA8Unorm with tone mapping)
- **Scaled blit** (e.g., render at 2x resolution, downsample to display)
- **Mipmap generation** with custom filtering (e.g., Kaiser filter for texture atlases)
- **Color space conversion** (e.g., linear → sRGB, HDR → SDR)
- **Post-process resolve** (custom MSAA resolve with temporal weighting)

These operations are traditionally implemented by creating a render pipeline with a full-screen triangle and a fragment shader. The `BlitPipeline` formalizes this pattern: it's a lightweight pipeline descriptor that pairs a fragment shader with source/destination formats, avoiding the boilerplate of setting up render targets, viewports, vertex buffers, and pipeline state for what is conceptually a blit.

### 3.1 Public API

**Add to `GEPipeline.h`:**

```cpp
struct OMEGAGTE_EXPORT BlitPipelineDescriptor {
    OmegaCommon::String name;
    
    /// The fragment shader that transforms the source texture.
    /// Receives a single `texture2d` input at binding 0 and UV coordinates.
    /// Writes to SV_Target0 (or `[[color(0)]]` on Metal).
    SharedHandle<GTEShader> fragmentFunc;
    
    /// Source texture pixel format (for validation and pipeline compatibility).
    PixelFormat srcPixelFormat = PixelFormat::RGBA8Unorm;
    
    /// Destination texture pixel format.
    PixelFormat destPixelFormat = PixelFormat::RGBA8Unorm;
    
    /// Optional: multisample count of the source (for MSAA resolve blits).
    unsigned srcSampleCount = 1;
};

using GEBlitPipelineState = struct __GEBlitPipelineState;
```

**Add to `OmegaGraphicsEngine` in `GE.h`:**

```cpp
virtual SharedHandle<GEBlitPipelineState> makeBlitPipelineState(
    BlitPipelineDescriptor &desc) = 0;
```

**Add to `GECommandBuffer` (public, blit pass section):**

```cpp
/// @brief Execute a programmable blit using a BlitPipelineState.
/// Reads from src, runs the pipeline's fragment shader, writes to dest.
/// The blit covers the full extent of dest (or the specified region).
/// @param pipelineState The blit pipeline to use.
/// @param src Source texture.
/// @param dest Destination texture.
virtual void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                               SharedHandle<GETexture> & src,
                               SharedHandle<GETexture> & dest) = 0;

/// @brief Execute a programmable blit to a subregion of the destination.
/// @param pipelineState The blit pipeline.
/// @param src Source texture.
/// @param dest Destination texture.
/// @param srcRegion Region of the source to sample from.
/// @param destRegion Region of the destination to write to.
virtual void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                               SharedHandle<GETexture> & src,
                               SharedHandle<GETexture> & dest,
                               const TextureRegion & srcRegion,
                               const TextureRegion & destRegion) = 0;
```

### 3.2 Implementation Strategy

Under the hood, a `BlitPipeline` is a render pipeline with a built-in full-screen triangle vertex shader. Each backend implements it as:

1. **Internal vertex shader**: A minimal shader that outputs a full-screen triangle from vertex ID (no vertex buffer needed):
   ```hlsl
   // HLSL (D3D12)
   float4 vs_main(uint vid : SV_VertexID) : SV_Position {
       float2 uv = float2((vid << 1) & 2, vid & 2);
       return float4(uv * 2.0 - 1.0, 0.0, 1.0);
   }
   ```
   ```glsl
   // GLSL (Vulkan)
   void main() {
       vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
       gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
   }
   ```
   ```metal
   // Metal
   vertex float4 vs_main(uint vid [[vertex_id]]) {
       float2 uv = float2((vid << 1) & 2, vid & 2);
       return float4(uv * 2.0 - 1.0, 0.0, 1.0);
   }
   ```

2. **User-supplied fragment shader**: Provided via the descriptor. Receives the source texture as a bound resource.

3. **Internal render pass**: The `blitWithPipeline()` command:
   - Creates (or caches) a temporary render pass targeting `dest`
   - Sets the blit pipeline state
   - Binds `src` as a texture resource at slot 0
   - Draws 3 vertices (one triangle covering the screen)
   - Ends the render pass

4. **Viewport/scissor**: Automatically set from `destRegion` (or full texture extent).

### 3.3 Backend Implementation Details

**D3D12:**
```cpp
struct GED3D12BlitPipelineState : public GEBlitPipelineState {
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12RootSignature> rootSignature;
    // Built with: no input layout, internal VS, user FS, no depth, no blend
};
```
- The internal VS is compiled once at engine init and reused across all blit pipelines.
- Root signature: one SRV (source texture) + one sampler.
- PSO: Rasterizer with no cull, no depth test, single RT matching `destPixelFormat`.

**Metal:**
```cpp
struct GEMetalBlitPipelineState : public GEBlitPipelineState {
    NSSmartPtr renderPipeline;  // id<MTLRenderPipelineState>
    // Built with: internal VS function, user FS function, single color attachment
};
```
- Internal VS compiled from the Metal library at engine init.
- `MTLRenderPipelineDescriptor` with no vertex descriptor (vertex ID only).

**Vulkan:**
```cpp
struct GEVulkanBlitPipelineState : public GEBlitPipelineState {
    VkPipeline pipeline;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout descriptorSetLayout;
    // Built with: internal VS, user FS, dynamic viewport/scissor,
    //   no vertex input, single color attachment
};
```
- Internal VS compiled to SPIR-V at engine init.
- Pipeline layout: one combined image sampler descriptor.
- Render pass: dynamically created or compatible with a cached render pass for `destPixelFormat`.
- **Note**: Vulkan also has `vkCmdBlitImage` for simple scaled blits with linear/nearest filtering. The programmable blit pipeline is for cases where a shader is needed. The existing `copyTextureToTexture()` should prefer `vkCmdCopyImage` for exact copies and `vkCmdBlitImage` for scaled copies without a shader.

### 3.4 Built-In Blit Shaders

Keep this. This will be very helpful to people.
The engine should ship a small set of pre-compiled blit fragment shaders for common operations, so users don't have to write OmegaSL for standard blits:

| Shader | Purpose |
|---|---|
| `blit_copy` | Passthrough (same as hardware copy, but allows format conversion) |
| `blit_linear` | Bilinear filtered downsample/upsample |
| `blit_srgb_encode` | Linear → sRGB conversion |
| `blit_srgb_decode` | sRGB → Linear conversion |
| `blit_tonemap_reinhard` | HDR → SDR via Reinhard tone mapping |

These would be OmegaSL sources compiled at build time and bundled with the engine.

---

## Extension 4: Blit Pass Completion (Fixed-Function)

Independent of the programmable blit pipeline, the existing fixed-function blit pass needs these additional commands:

### 4.1 Buffer-to-Buffer Copy ✅ Implemented

**Add to `GECommandBuffer` (public, blit pass section):**

```cpp
/// @brief Copy data between buffers.
/// @param src Source buffer.
/// @param dest Destination buffer.
/// @param size Bytes to copy (0 = entire source buffer).
/// @param srcOffset Byte offset in source.
/// @param destOffset Byte offset in destination.
virtual void copyBufferToBuffer(SharedHandle<GEBuffer> & src,
                                 SharedHandle<GEBuffer> & dest,
                                 size_t size = 0,
                                 size_t srcOffset = 0,
                                 size_t destOffset = 0) = 0;
```

| D3D12 | Metal | Vulkan |
|---|---|---|
| `CopyBufferRegion()` | `copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:` | `vkCmdCopyBuffer()` |

### 4.2 Buffer ↔ Texture Transfers ✅ Implemented

**Add to `GECommandBuffer`:**

```cpp
/// @brief Copy buffer contents into a texture.
/// @param src Source buffer containing texel data.
/// @param dest Destination texture.
/// @param bytesPerRow Bytes per row of texel data in the buffer.
/// @param bytesPerImage Bytes per image slice (for 3D/array textures; 0 for 2D).
/// @param destRegion Target region within the texture.
virtual void copyBufferToTexture(SharedHandle<GEBuffer> & src,
                                  SharedHandle<GETexture> & dest,
                                  size_t bytesPerRow,
                                  size_t bytesPerImage,
                                  const TextureRegion & destRegion) = 0;

/// @brief Copy texture contents into a buffer (readback).
/// @param src Source texture.
/// @param dest Destination buffer (must be CPU-readable).
/// @param bytesPerRow Bytes per row in the destination buffer.
/// @param bytesPerImage Bytes per image slice (0 for 2D).
/// @param srcRegion Source region within the texture.
virtual void copyTextureToBuffer(SharedHandle<GETexture> & src,
                                  SharedHandle<GEBuffer> & dest,
                                  size_t bytesPerRow,
                                  size_t bytesPerImage,
                                  const TextureRegion & srcRegion) = 0;
```

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| Buffer → Texture | `CopyTextureRegion()` with `D3D12_TEXTURE_COPY_LOCATION` (buffer as Placed footprint) | `copyFromBuffer:toTexture:` on blit encoder | `vkCmdCopyBufferToImage()` |
| Texture → Buffer | `CopyTextureRegion()` with buffer dest | `copyFromTexture:toBuffer:` on blit encoder | `vkCmdCopyImageToBuffer()` |

**Compatibility notes:**
- **D3D12**: Row pitch must be aligned to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256 bytes). The implementation must validate or pad `bytesPerRow`.
- **Metal**: No alignment requirement on `bytesPerRow` (Metal handles it internally).
- **Vulkan**: `bufferRowLength` in `VkBufferImageCopy` can be 0 (tightly packed) or a specific value. No special alignment required by spec, but optimal transfer may benefit from device-specific alignment.

### 4.3 Mipmap Generation ✅ Implemented

**Add to `GECommandBuffer` (public, blit pass section):**

```cpp
/// @brief Generate mipmaps for a texture.
/// Fills all mip levels from level 0 downward using box filtering.
/// @param texture The texture to generate mipmaps for. Must have been created with mip levels > 1.
virtual void generateMipmaps(SharedHandle<GETexture> & texture) = 0;
```

**Backend mapping:**

| Backend | Implementation |
|---|---|
| **D3D12** | No built-in mipmap generation. Must implement as a chain of `CopyTextureRegion` with a compute shader for downsampling, OR use a series of render passes with a downsample shader. Common approach: iterative blit from mip N to mip N+1 using a simple box filter compute shader. |
| **Metal** | `[MTLBlitCommandEncoder generateMipmapsForTexture:]` — native, single call. |
| **Vulkan** | No built-in command. Must implement as a chain of `vkCmdBlitImage` calls from mip N to mip N+1 with `VK_FILTER_LINEAR`, transitioning each mip level's layout as needed. This is the standard Vulkan approach and well-documented. |

**Compatibility notes:**
- Metal is trivial (one API call).
- D3D12 and Vulkan require manual implementation. The Vulkan approach using `vkCmdBlitImage` is well-tested. D3D12 should use a similar approach with render-to-mip or compute downsampling.
- The internal downsample shader (for D3D12) can be shared with the `BlitPipeline` infrastructure — it's the same "read one mip, write to next mip" pattern.

### 4.4 Texture and Buffer Fills ✅ Implemented

**Add to `GECommandBuffer` (public, blit pass section):**

```cpp
/// @brief Fill a buffer region with a repeating 32-bit value.
/// @param buffer The buffer to fill.
/// @param value The 32-bit value to fill with.
/// @param offset Byte offset (must be 4-byte aligned).
/// @param size Bytes to fill (must be 4-byte aligned; 0 = entire buffer).
virtual void fillBuffer(SharedHandle<GEBuffer> & buffer,
                         uint32_t value,
                         size_t offset = 0,
                         size_t size = 0) = 0;
```

| D3D12 | Metal | Vulkan |
|---|---|---|
| No direct equivalent — use `ClearUnorderedAccessViewUint` if UAV, or CPU memset + upload | `[MTLBlitCommandEncoder fillBuffer:range:value:]` (8-bit value only) | `vkCmdFillBuffer()` (32-bit value) |

**Compatibility concern**: Metal's `fillBuffer` only fills with an 8-bit value, while D3D12 and Vulkan use 32-bit values. Options:
1. Expose an 8-bit fill (lowest common denominator) — limits utility.
2. Expose a 32-bit fill and implement Metal's version via a tiny compute shader — adds complexity.
3. Expose both `fillBuffer8` and `fillBuffer32` — exposes the platform difference.

**Recommendation**: Expose a 32-bit fill. Metal implementation uses a small compute shader (4 lines of MSL) to fill with a 32-bit pattern. This is the same approach MoltenVK uses internally.

### 4.5 CPU Texture Upload by Region (`GETexture::copyBytes` overload)

The existing `GETexture::copyBytes(bytes, bytesPerRow)` overwrites the entire mip 0 with a tightly-packed source buffer. There is no way for a CPU caller to update a sub-rect of an existing texture without re-uploading the whole thing, and no way to pick a different area of a source bitmap to read from — `copyBytes` always reads `width × height` contiguous rows starting at `bytes`.

Both gaps surface in real consumers:

- **WTK** routes decoded image data (`BitmapImage`) through `BitmapTextureCache`. The decoders deliver rows bottom-up (legacy GL convention) while every GTE sampler treats row 0 as the top, so naive upload renders upside-down. With a region-aware overload WTK can row-flip in N upload calls (one per source row) without staging a flipped copy of the whole image.
- **Sprite atlas / nine-slice rendering** (planned for WTK Phase 6.6.3) needs to update a sub-rect of a baked atlas as new glyphs / icons get rasterized, without rebuilding the whole atlas.
- **Future blit extension (§3)** introduces programmable blits, but its compatriot — uploading a CPU-side source rect into a sub-rect of a GPU texture — has no command yet. This sub-section covers that gap so the blit landing can stay focused on programmable passes.

The chosen shape adds a destination region; the caller advances its own source pointer to pick which slice of the bitmap to read. This matches `MTLTexture replaceRegion:withBytes:bytesPerRow:`, `D3D12 CopyTextureRegion` (with a staged source), and `vkCmdCopyBufferToImage` semantics directly.

**Add to `gte/include/omegaGTE/GETexture.h`:**

```cpp
/// @brief Upload data to a sub-region of the texture from a CPU buffer.
/// @param bytes Pointer to the source buffer. Must point to at least
///        `bytesPerRow × destRegion.h × max(destRegion.d, 1)` bytes
///        starting at the top-left of the region's source data
///        (caller-side offset is the caller's responsibility — to read
///        from a sub-area of a larger source bitmap, advance the
///        pointer to the desired row/column before calling).
/// @param bytesPerRow Bytes per row in the source buffer (source row
///        pitch, may exceed `destRegion.w × bytesPerPixel` if the
///        caller is passing a non-tight stride).
/// @param destRegion Sub-region of the texture to overwrite. `{x, y, z}`
///        is the destination origin in texel coordinates within mip 0;
///        `{w, h, d}` is the extent. For 2D textures `z = 0` and
///        `d = 1`.
/// @paragraph
/// Only valid for textures created with `ToGPU` usage. The single-arg
/// `copyBytes(bytes, bytesPerRow)` overload remains as a convenience
/// that targets the full mip 0 extent.
virtual void copyBytes(void *bytes,
                       size_t bytesPerRow,
                       const TextureRegion &destRegion) = 0;
```

`TextureRegion` already exists in `GTEBase.h` with `{x, y, z, w, h, d}` fields.

**Backend mapping:**

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `copyBytes(..., region)` | One-shot upload buffer + `CopyTextureRegion` with `D3D12_BOX` matching `region` | `[id<MTLTexture> replaceRegion:MTLRegionMake3D(x,y,z,w,h,d) mipmapLevel:0 withBytes:bytes bytesPerRow:bytesPerRow]` | Temp host-visible staging `VkBuffer` + `vkCmdCopyBufferToImage` with `VkBufferImageCopy.imageOffset / imageExtent` matching `region` |

**Compatibility notes:**

- **D3D12**: The existing `copyBytes(bytes, bytesPerRow)` writes to the per-texture `cpuSideresource` upload heap and defers the GPU copy to `updateAndValidateStatus`. The region overload does not share that path — it allocates a temp upload buffer sized to `bytesPerRow × destRegion.h × destRegion.d`, memcpys the source rows in, issues a one-shot command list that transitions the destination to `COPY_DEST`, calls `CopyTextureRegion` with a source `D3D12_BOX` covering `(0..w, 0..h, 0..d)` and a destination origin of `(x, y, z)`, transitions back, and waits. Independent of the deferred-upload state machine; safe to call after the texture is already on-GPU. Row pitch in the temp buffer follows the `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` (256-byte) rule — pad as needed.

- **Metal**: One native call. `replaceRegion` performs the upload synchronously from the perspective of subsequent encoded commands; no manual staging buffer required. For array / cube textures the existing single-arg `copyBytes` writes slice 0; this overload preserves that — slice indexing requires a future `TextureRegion` extension (mip + arrayLayer fields).

- **Vulkan**: The current single-arg path memcpys directly into a host-visible linear-tiled `VkImage`. The region overload instead allocates a temp `VkBuffer` (host-visible, `TRANSFER_SRC`), memcpys source rows in tightly packed (no padding — Vulkan's `bufferRowLength = 0` semantics), then issues `vkCmdCopyBufferToImage` on a one-shot command buffer with `VkBufferImageCopy.imageOffset = {x, y, z}` and `imageExtent = {w, h, d}`. Transitions the image layout to `TRANSFER_DST_OPTIMAL`, copies, transitions back to `SHADER_READ_ONLY_OPTIMAL` (or whatever the texture's tracked layout is). Sync via `vkQueueWaitIdle` on the transfer queue.

**WTK consumer — `BitmapTextureCache::acquire`:** loops `h` times, advancing `image->data + (h - 1 - row) × stride` and writing row-by-row into `{x: 0, y: row, w: width, h: 1, ...}`. Functionally correct; `h` driver calls per upload is acceptable for image-decode-time uploads (one-shot, not in the per-frame path).

**Out of scope for this sub-section** (deferred, see Extension 7):

- Mip-level / array-layer indexing on the destination — folded into §7.1's `TextureRegion` extension.
- Source-rect parameter (separate `SourceRect` for sub-region reads). Callers advance the source pointer instead; if a future consumer wants the engine to do source-rect math (e.g. when source `bytesPerRow` ≠ destination row width), revisit.

---

## Extension 5: Fix Existing Bugs ✅ Implemented

These are bugs found during the audit that should be fixed regardless of new features.

### 5.1 Metal Region Copy ✅ Implemented

`GEMetalCommandQueue.mm` copies entire mipmaps instead of the specified region when `copyTextureToTexture(src, dest, region, destCoord)` is called. The Metal blit encoder's `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:` method should be used with the region parameters mapped to `MTLOrigin` + `MTLSize`.

### 5.2 D3D12 Blend State Hardcoding ✅ Implemented

`GED3D12.cpp:790-799` hardcodes SrcAlpha / InvSrcAlpha blending for all render pipelines. This should be driven by `RenderPipelineDescriptor.colorBlendDescriptors` once Extension 1.2 is implemented. Until then, the hardcoded blend is a known limitation.

---

## Extension 6: Texture View Type Extension (OmegaSL §2.1 Phase B)

### Goal

Phase A of OmegaSL §2.1 (`docs/OmegaSL-Feature-Gap-Survey.md`) added six new texture types (`texture1d_array`, `texture2d_array`, `texturecube`, `texturecube_array`, `texture2d_ms`, `texture2d_ms_array`) and one new sampler type (`samplercube`) to the OmegaSL compile path. The compiler accepts shaders using these types and emits valid HLSL/MSL/GLSL source. The runtime cannot yet *bind* a real cube / array / multisample texture view, so a shader that declares one of the new types compiles but fails at pipeline-creation time.

This extension closes that gap. It introduces:

1. A `GETexture` creation API that names face count, array-layer count, and sample count explicitly.
2. View-type plumbing inside D3D12 / Vulkan / Metal so the existing OmegaSL layout-desc values (`OMEGASL_SHADER_TEXTURECUBE_DESC`, `OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC`, ...) drive the right native view dimension.
3. Texture-asset pipeline support for cube faces (loader-side concern; out-of-scope here, listed in §6.5).

After this extension, environment maps, shadow cascades, sprite atlases addressed by layer index, MSAA resolves driven from a shader, and IBL precomputation will all be expressible end-to-end.

### 6.1 `GETexture` API extension

**Add to `gte/include/omegaGTE/GETexture.h`:**

```cpp
enum class TextureKind : uint8_t {
    Tex1D,          // existing dim::OneD
    Tex2D,          // existing dim::TwoD
    Tex3D,          // existing dim::ThreeD
    Tex1DArray,     // new — N layers, 1D each
    Tex2DArray,     // new — N layers, 2D each
    TexCube,        // new — exactly 6 faces, treated as one texture
    TexCubeArray,   // new — 6 * N faces
    Tex2DMS,        // new — 2D, sample_count > 1
    Tex2DMSArray,   // new — 2D array, sample_count > 1
};

struct OMEGAGTE_EXPORT GETextureDescriptor {
    TextureKind kind = TextureKind::Tex2D;
    PixelFormat format = PixelFormat::RGBA8Unorm;
    unsigned width = 1;
    unsigned height = 1;
    unsigned depth = 1;          // 3D only; 1 for non-3D
    unsigned mipLevels = 1;
    unsigned arrayLayers = 1;    // 1 for non-array; 6 implied for cube; 6*N for cube_array
    unsigned sampleCount = 1;    // 1 for non-MS
    GETextureUsage usage = GETextureUsage::ShaderRead;
};

virtual SharedHandle<GETexture> makeTexture(const GETextureDescriptor &desc) = 0;
```

The existing `dim`-based constructors stay as a thin wrapper that forwards to `GETextureDescriptor` — preserves the current call sites without an API break.

**Validation rules (engine-side):**
- `kind == TexCube` ⇒ `arrayLayers == 6` (compiler-enforced default; users do not set this).
- `kind == TexCubeArray` ⇒ `arrayLayers % 6 == 0`.
- `kind` ∈ {`Tex2DMS`, `Tex2DMSArray`} ⇒ `sampleCount > 1` and `mipLevels == 1` (MS textures are single-mip on every backend).
- `kind` ∈ {`Tex1DArray`, `Tex2DArray`, `TexCube`, `TexCubeArray`, `Tex2DMSArray`} ⇒ `arrayLayers >= 1`.

### 6.2 Backend mapping

| Backend | Cube view | 2D-array view | 1D-array view | MS view | MS-array view |
|---|---|---|---|---|---|
| **D3D12** | `D3D12_SRV_DIMENSION_TEXTURECUBE` (or `_TEXTURECUBEARRAY`) | `_TEXTURE2DARRAY` | `_TEXTURE1DARRAY` | `_TEXTURE2DMS` | `_TEXTURE2DMSARRAY` |
| **Metal** | `MTLTextureTypeCube` (or `_CubeArray`) | `MTLTextureType2DArray` | `MTLTextureType1DArray` | `MTLTextureType2DMultisample` | `MTLTextureType2DMultisampleArray` |
| **Vulkan** | `VK_IMAGE_VIEW_TYPE_CUBE` (or `_CUBE_ARRAY`) | `_2D_ARRAY` | `_1D_ARRAY` | `_2D` (with `samples > 1`) | `_2D_ARRAY` (with `samples > 1`) |

Vulkan: Multisample image views use the standard 2D / 2D_ARRAY view types — the multisample-ness is in the underlying `VkImage`'s `samples` field, not the view type. The image creation path needs to pick `VK_SAMPLE_COUNT_*_BIT` from `sampleCount` instead.

D3D12: Cube views are SRV-only on D3D12 hardware that doesn't support `D3D12_FEATURE_DATA_D3D12_OPTIONS3.WriteBufferImmediateSupportFlags` cube UAV. UAV cube writes alias to `RWTexture2DArray` with `arraySize=6`. OmegaSL Sema already rejects `write` to cube textures (Phase A), so the UAV-cube path is unreachable from a generated shader — this is a non-issue but worth noting.

Metal: `MTLTextureType2DMultisampleArray` requires macOS 11+ / iOS 14+. Below that version the runtime should reject `Tex2DMSArray` at `makeTexture` time with a clear diagnostic rather than create a malformed texture.

### 6.3 OmegaSL layout-desc → bound view

The existing layout-desc switch (D3D12 `getRequiredResourceStateForResourceID`, Vulkan `case OMEGASL_SHADER_TEXTURE2D_DESC` arms, Metal argument-buffer encoding) currently buckets all six new types into the same SRV/UAV bucket as plain 2D textures. That's correct for the *resource state* and *descriptor type* but not for the *view*. Phase B adds a per-bind-call view-dimension lookup:

**`gte/src/d3d12/GED3D12CommandQueue.cpp`** — when binding a texture, look up the OmegaSL layout-desc type and pick the SRV/UAV view-dimension:

```cpp
D3D12_SRV_DIMENSION dimensionFromLayoutDesc(omegasl_shader_layout_desc_type t) {
    switch (t) {
        case OMEGASL_SHADER_TEXTURE1D_DESC:           return D3D12_SRV_DIMENSION_TEXTURE1D;
        case OMEGASL_SHADER_TEXTURE2D_DESC:           return D3D12_SRV_DIMENSION_TEXTURE2D;
        case OMEGASL_SHADER_TEXTURE3D_DESC:           return D3D12_SRV_DIMENSION_TEXTURE3D;
        case OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC:     return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        case OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC:     return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        case OMEGASL_SHADER_TEXTURECUBE_DESC:         return D3D12_SRV_DIMENSION_TEXTURECUBE;
        case OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC:   return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        case OMEGASL_SHADER_TEXTURE2D_MS_DESC:        return D3D12_SRV_DIMENSION_TEXTURE2DMS;
        case OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC:  return D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        default: return D3D12_SRV_DIMENSION_UNKNOWN;
    }
}
```

The bind path validates that the bound `GETexture`'s `TextureKind` matches the shader's expected dimension and rejects mismatches at bind time. This is the moment where "shader expects cube" meets "texture is 2D" — failing here, with both source-of-truth values in scope, gives a clean error message.

**Vulkan** equivalent maps to `VkImageViewType`. **Metal** uses `MTLTextureType` and the argument-buffer slot's `MTLTextureReferenceType`.

### 6.4 Sample-count plumbing

For MS textures, the bind path additionally needs to verify the bound texture's sample count matches what the pipeline expects. The OmegaSL layout-desc doesn't currently carry sample count — Phase B adds an optional sample-count field to `omegasl_shader_layout_desc` (defaulted to 1, populated only for MS variants). This lets the runtime reject "shader compiled for `texture2d_ms` but bound texture has sample_count = 1" at bind time.

For the static-sampler path, no change — samplers don't carry shape information; the texture they're paired with at `sample`-call time does.

### 6.5 Texture-asset side (out of scope for this extension, listed for visibility)

Cube-face composition is a separate concern from runtime view binding:

- **Cube loader** — engine-side asset path needs a way to specify six face images and assemble them into a single `TexCube` upload. KTX2 / DDS already encode cube faces; the asset loader currently flattens to 2D. Suggest a `GETextureAssetDescriptor::cubeFaces` variant.
- **Array loader** — same: an array asset is N images-of-the-same-format stacked into a single texture. Either the asset format encodes the layer count or the loader synthesizes it from a manifest.
- **Per-face / per-layer copy** — `copyBufferToTexture` and `copyTextureToTexture` need a face/layer index parameter (already implicit in `D3D12_TEXTURE_COPY_LOCATION::SubresourceIndex`, `MTLBlitEncoder` slice arg, `VkImageCopy::srcSubresource.baseArrayLayer`).

These could land alongside §6 or as a follow-up. They block end-user productivity but not the language↔runtime gap that Phase B is closing.

### 6.6 Implementation order

```
6.1 (GETextureDescriptor + makeTexture overload)
    │
6.2 (per-backend view-dimension picker, no bind-side changes yet)
    │
6.3 (bind-time validation: shader type vs texture kind)
    │
6.4 (MS sample-count match, only relevant once 6.3 lands)
    │
6.5 (asset-side cube/array loader — independent track)
```

### 6.7 Tests

- `gte/tests/<backend>/CubeTexTest` — create a `TexCube`, upload six face colors via `copyBufferToTexture`, sample with a small skybox shader, read pixel back, assert each face was sampled correctly.
- `gte/tests/<backend>/Texture2DArrayTest` — same shape with N=3 layers.
- `gte/tests/<backend>/Texture2DMSTest` — render to an MS render target with sample_count=4, then sample-resolve in a fragment shader using the existing OmegaSL `read(msTex, coord, sample_index)` Phase A path.

### 6.8 Open questions

1. **Format aliasing across faces.** Should every face of a cube be required to share the same `PixelFormat` and dimensions? D3D12 / Vulkan / Metal all require this. Yes — match the platform contract.
2. **Cube `read`/`write`.** Phase A defers cube load/store. If a future use case justifies them, they slot into the same view-dimension lookup as Phase B's sample binding — no further engine work required, just lift the Sema rejection and add the `Load` / `imageStore` / `tex.read` lowerings in each backend.
3. **MS render target.** MSAA color attachments already work via `RenderPassDescriptor.multisampleResolve`. Phase B's MS *texture-as-shader-input* is the inverse — reading an MS render target from a later pass. The render target must be created with `TextureKind::Tex2DMS` and `usage = ShaderRead | RenderTarget`. The existing `multisampleResolve` flow should keep working; this just adds a second path that doesn't resolve before sampling.

---

## Implementation Order

```
Extension 5 (bug fixes) ────── can land independently, first
    │
Extension 1.1 (vertex input) ─── prerequisite for complex geometry
    │
Extension 1.2 (blend state) ──── prerequisite for compositing / MRT
    │
Extension 1.3 (indexed draw) ──── high-value, simple
Extension 1.4 (instanced draw) ── high-value, simple
    │
Extension 1.6 (MRT) ──── depends on 1.2 (blend per attachment)
Extension 1.7 (polygon types) ── trivial, independent
    │
Extension 4.1-4.2 (buffer copy, buffer↔texture) ── high-value, independent
Extension 4.3 (mipmap gen) ── medium complexity
Extension 4.4 (fill) ── low complexity
    │
Extension 1.5 (indirect draw) ─── depends on 1.3 (indexed draw)
Extension 2.1 (indirect dispatch) ── independent
    │
Extension 3 (blit pipeline) ── depends on 1.1 (vertex input) being stable
    │
Extension 2.2 (push constants) ── requires OmegaSL changes, parallel track
Extension 2.3 (threadgroup override) ── deferred
```

### Priority Tiers

**Tier 1 — High Impact, Low Risk** (implement first):
- 5.1, 5.2: Bug fixes ✅
- 1.3: Indexed drawing ✅
- 1.4: Instanced drawing ✅
- 4.1: Buffer-to-buffer copy ✅
- 4.2: Buffer ↔ texture transfers ✅
- 1.7: Additional polygon types  ✅

**Tier 2 — High Impact, Medium Risk** (implement second):
- 1.1: Vertex input layout (touches PSO creation on all backends) ✅
- 1.2: Blend state (touches PSO creation on all backends) ✅
- 2.1: Indirect dispatch ✅
- 4.3: Mipmap generation ✅
- 4.4: Buffer fill ✅

**Tier 3 — Medium Impact, Higher Complexity** (implement third):
- 1.5: Indirect drawing ✅
- 1.6: Multiple color attachments  ✅
- 3.x: Blit pipeline (requires internal shaders, render pass management)

**Tier 4 — Deferred** (requires OmegaSL work):
- 2.2: Push constants
- 2.3: Threadgroup size override

**Tier 5 — Cross-cutting (paired with OmegaSL feature work):**
- Extension 6: Texture View Type Extension (OmegaSL §2.1 Phase B). Phase A of the OmegaSL change shipped the new texture types as a compile-only path; Phase B closes the runtime gap. No dependency on Tiers 1–3 — can land in parallel.

---

## Extension 7: GETexture API Completion (Proposed)

`GETexture` today supports full-mip-0 upload/readback and (after §4.5) sub-region upload. Real consumers — WTK's bitmap path, future asset-streaming systems, BC-compressed game textures, sRGB tone-mapping passes — need a small set of additional capabilities the existing API can't express. This extension proposes them as a unit. None are blocked on OmegaSL changes; all can land independently of Tier 1–4 work.

### 7.1 `TextureRegion` extension — mip level + array layer

`TextureRegion` is `{x, y, z, w, h, d}` today: a 3D box with no awareness of the mip pyramid or array slices. Consequences:

- §4.5's `copyBytes(region)` can only target mip 0 of layer 0.
- §4.2's `copyBufferToTexture(region)` has the same limitation.
- Cube-face uploads (proposed in §6.5 as part of asset-side work) can't address individual faces through the existing region APIs.
- Sub-mip readback for streaming systems and mipmap-cache rebuilds (§4.3 mipmap generation only writes derived mips; explicit overwrites need addressing).

**Proposal:** Add two fields, defaulted so existing call sites stay correct:

```cpp
struct TextureRegion {
    unsigned x, y, z;
    unsigned w, h, d;
    unsigned mipLevel = 0;       // new — mip pyramid level to address
    unsigned arrayLayer = 0;     // new — array slice / cube face index
};
```

Backend mapping (all three already have the primitive):

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `mipLevel`, `arrayLayer` | `D3D12CalcSubresource(mip, arrayLayer, planeSlice, mipLevels, arraySize)` for the destination subresource index in `CopyTextureRegion` | `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:` (slice = arrayLayer) | `VkImageSubresourceLayers.{mipLevel, baseArrayLayer, layerCount = 1}` in `VkBufferImageCopy` |

**Compatibility:** Additive — purely default-constructed call sites keep their current semantics. The §4.5 D3D12 path's `GetCopyableFootprints(...)` already takes a subresource index; current code hardcodes 0, the new field picks the right one. Vulkan's existing `copyBufferToTexture` already passes a `VkImageSubresourceLayers`; current code hardcodes `mipLevel = 0` and `baseArrayLayer = 0`. Metal's existing `replaceRegion` overload that we use for §4.5 already exists with the slice + mipmap parameters; we currently pass 0/0.

### 7.2 `getBytes(bytes, bytesPerRow, srcRegion)` — sub-region readback

Symmetric to §4.5. Current `getBytes(bytes, bytesPerRow)` reads the full mip 0 only — to read one pixel for hit-testing or a 64×64 tile for an editor preview, the caller has to allocate a whole-texture buffer and discard most of it. The cost compounds for high-resolution render targets.

```cpp
virtual size_t getBytes(void *bytes,
                        size_t bytesPerRow,
                        const TextureRegion &srcRegion) = 0;
```

Returns bytes written (same convention as the single-arg `getBytes`).

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `getBytes(bytes, bpr, srcRegion)` | One-shot `CopyTextureRegion` from texture sub-rect into a temp readback heap (`D3D12_HEAP_TYPE_READBACK`), Map, memcpy out, Unmap | `getBytes:bytesPerRow:fromRegion:mipmapLevel:` (native) | Temp host-visible `VkBuffer` + `vkCmdCopyImageToBuffer` with `VkBufferImageCopy.imageOffset/imageExtent`, then map + memcpy |

**Use cases unblocked:**
- WTK debug overlays / picking
- Editor "save selection as image" flows
- CPU-side validation tests (gte/tests/* already do whole-texture readback; per-region would tighten the test surface)

### 7.3 Texture metadata accessors

The base `GETexture` exposes `getKind()`, `getArrayLayers()`, `getSampleCount()`, but consumers wanting width/height/depth/mipLevels/pixelFormat have to cast to the backend type (`(GED3D12Texture *)tex->resource->GetDesc().Width`, `(__bridge id<MTLTexture>)tex->native().width`, `((GEVulkanTexture *)tex)->descriptor.width`). That leaks backend types through the otherwise platform-neutral consumer code.

**Proposal:** Promote dimensions into `GETexture` and set them at construction time alongside the existing `kind`/`arrayLayers`/`sampleCount` fields:

```cpp
class GETexture : public GTEResource {
protected:
    unsigned width_ = 0;
    unsigned height_ = 0;
    unsigned depth_ = 1;
    unsigned mipLevels_ = 1;
    // (existing) TexturePixelFormat pixelFormat;
public:
    unsigned getWidth()      const { return width_; }
    unsigned getHeight()     const { return height_; }
    unsigned getDepth()      const { return depth_; }
    unsigned getMipLevels()  const { return mipLevels_; }
    TexturePixelFormat getPixelFormat() const { return pixelFormat; }
    // existing: getKind / getArrayLayers / getSampleCount
};
```

Backend `makeTexture` paths already know these values at construction time (they come from `TextureDescriptor`); just call a new `setDimensions(w, h, d, mipLevels)` helper alongside the existing `setShape(...)`. Zero per-frame cost; existing call sites untouched.

**Migration win:** §6.5 cube/array asset loaders and §4.3 mipmap generation both currently reach into backend types to query dimensions. Both become backend-neutral with this.

### 7.4 Compressed pixel formats

`TexturePixelFormat` today covers uncompressed formats (RGBA8Unorm, RGBA16Unorm, BGRA8Unorm and sRGB variants). Real-world UI assets typically run 4–8× larger uncompressed; a 4K texture atlas at RGBA8 is 64 MiB, at BC7 it's 16 MiB. Modern game/UI engines won't budget around uncompressed-only.

**Proposal:** Extend `TexturePixelFormat` (in `GTEBase.h`) with the block-compressed families:

| OmegaGTE addition | Block | Use case | D3D12 | Metal | Vulkan |
|---|---|---|---|---|---|
| `BC1_RGB_Unorm`, `BC1_RGBA_Unorm`, `BC1_RGBA_SRGB` | 4×4, 8 bytes | Diffuse / albedo, low alpha | `DXGI_FORMAT_BC1_UNORM*` | `MTLPixelFormatBC1_RGBA*` | `VK_FORMAT_BC1_RGB_*` / `BC1_RGBA_*` |
| `BC3_Unorm`, `BC3_SRGB` | 4×4, 16 bytes | Diffuse + alpha (legacy DXT5 successor) | `BC3_*` | `BC3_RGBA*` | `BC3_*` |
| `BC4_Unorm`, `BC4_Snorm` | 4×4, 8 bytes | Single-channel (height, mask) | `BC4_*` | `BC4_R*` | `BC4_*` |
| `BC5_Unorm`, `BC5_Snorm` | 4×4, 16 bytes | Normal maps (RG) | `BC5_*` | `BC5_RG*` | `BC5_*` |
| `BC7_Unorm`, `BC7_SRGB` | 4×4, 16 bytes | High-quality RGBA (modern default) | `BC7_*` | `BC7_RGBA*` | `BC7_*` |
| `ASTC_<n>x<m>_Unorm`, `..._SRGB` (block sizes 4×4 through 12×12) | variable | Apple Silicon / mobile high-quality | (driver-emulated only) | `MTLPixelFormatASTC_*` | `VK_FORMAT_ASTC_*` (gated) |
| `ETC2_RGB8_Unorm`, `ETC2_RGBA8_Unorm`, sRGB variants | 4×4 | Linux / mobile / Vulkan defaults | (driver-emulated only) | (Apple Silicon supports) | `VK_FORMAT_ETC2_*` (gated) |

**Compatibility notes:**

- **D3D12:** BC1–BC5, BC7 are universally supported on every desktop GPU since SM5. ASTC and ETC2 are *not* supported on D3D12 — these formats land only when the consumer is willing to gate on `GTEDeviceFeatures::astcSupported` / `etc2Supported` (new bits) and accept that they're effectively Metal/Vulkan-only.
- **Metal:** Apple Silicon Macs support BC1–BC7, ASTC, ETC2 natively. Intel Macs support BC formats only. Gate ASTC/ETC2 on the device feature query.
- **Vulkan:** All three families exist in the spec, gated by `VkPhysicalDeviceFeatures.textureCompressionBC` / `textureCompressionASTC_LDR` / `textureCompressionETC2`. Desktop drivers expose BC; mobile drivers expose ASTC/ETC2; nothing exposes all three. Standard `GTEDeviceFeatures` gating.

**API impact on the new region copy:**
- `bytesPerRow` for compressed formats is bytes per row *of blocks*, not pixels. A 256-pixel-wide BC7 texture has `bytesPerRow = (256 / 4) * 16 = 1024`, not `256 * bytesPerPixel`.
- `TextureRegion.{x, y}` and `.{w, h}` must be block-aligned (multiple of the format's block dimension). Validation: debug-build assert; release-build truncate to nearest block boundary.
- §7.3's metadata accessors should expose a `getBlockSize()` (returning `{blockW, blockH, bytesPerBlock}`) so consumers can do alignment math without backend introspection.

### 7.5 Texture clear / fill

§4.4 covers buffer fill but textures don't have a fill command on the blit pass. `RenderPassDescriptor.ColorAttachment.loadAction = Clear` only handles render-target clears at pass-begin time; non-render-target textures (compute outputs allocated `GPUAccessOnly`, streaming staging textures, debug fill-with-magenta states) need an explicit clear command.

**Proposal:**

```cpp
// On GECommandBuffer (blit pass section):

/// @brief Clear a texture region to a constant color.
/// @param texture Texture to clear. Must have a usage permitting the
///        backend's clear path (UAV on D3D12, render target or compute
///        output on Metal, any image on Vulkan).
virtual void clearTexture(SharedHandle<GETexture> &texture,
                          float r, float g, float b, float a,
                          const TextureRegion &region) = 0;
```

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `clearTexture(...)` | `ClearUnorderedAccessViewFloat` if UAV available, else `ClearRenderTargetView` if RTV available, else compute-shader fallback | `vkCmdClearColorImage`-equivalent does not exist; one-shot render pass with `loadAction = Clear`, OR a small `fillKernel` compute shader (similar to the §4.4 Metal buffer-fill compute fallback) | `vkCmdClearColorImage` (native, requires `TRANSFER_DST` layout) |

**Compatibility concern:** D3D12 requires the texture be UAV-or-RTV-usable to clear without a shader; Metal has no fixed-function image clear; only Vulkan has a universal primitive. Two implementation paths:

1. **Per-backend native where possible, compute fallback elsewhere.** Ship a tiny `clearTextureFill.omegasl` compute kernel (4 lines, write a float4 to a `[[texture(0)]]` UAV per dispatched thread) for Metal and as a D3D12 fallback when the texture lacks UAV/RTV. Vulkan uses `vkCmdClearColorImage` directly.
2. **Validate texture usage at call time.** Reject `clearTexture` on a texture that doesn't permit the chosen path, with a clear diagnostic. Forces consumers to ask for clear-capable textures upfront. Cleaner but less ergonomic.

**Recommendation:** Path 1 (compute fallback). Compute shader is trivial; the alternative is forcing every texture that *might* want clearing to take the UAV-usage overhead.

### 7.6 Async upload with fence completion

Today `copyBytes` is "fire and forget" — synchronous on Metal/Vulkan, deferred-until-first-bind on D3D12 (§4.5 inherits this). For asset-streaming systems and background-loading flows the caller needs to *know* when the upload is GPU-visible without binding the texture.

**Proposal:** Add a fence-returning overload:

```cpp
/// @brief Upload data and signal `fence` when the GPU side is complete.
/// @param fence Fence to signal. Caller waits on it (or chains it into a
///        command buffer's `waitForFence`) before binding the texture.
/// @returns true if the upload was scheduled, false on failure.
virtual bool copyBytesAsync(void *bytes,
                            size_t bytesPerRow,
                            const TextureRegion &destRegion,
                            SharedHandle<GEFence> &fence) = 0;
```

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `copyBytesAsync` | One-shot copy command list submitted to a transient upload queue, `ID3D12CommandQueue::Signal(fence)` after `CopyTextureRegion` | `[MTLCommandBuffer encodeSignalEvent:value:]` on a one-shot blit command buffer | One-shot `VkCommandBuffer` + `vkQueueSubmit` with `signalSemaphores` (or `VkFence`) |

**Compatibility note:** D3D12 currently keeps `GED3D12Texture` decoupled from the engine (no back-pointer). Async upload forces that refactor — `copyBytesAsync` needs to allocate a command list and command queue, which only the engine can provide. The §4.5 deferred-upload-via-`cpuSideresource` path doesn't compose with fence signalling; async needs its own one-shot upload path.

**Recommendation:** Pair with the texture-back-pointer refactor noted in §4.5 D3D12 compatibility. Once the texture holds a `GED3D12Engine *engine_;`, both §7.6 and a (potential) future D3D12 immediate-mode `copyBytes` overload become straightforward.

### 7.7 Format-cast / aliased texture views

Common need: take an `RGBA8Unorm` render target and bind it as `RGBA8Unorm_SRGB` for a tone-mapping pass without re-uploading or copying. The base texture stays; a second view interprets the same memory differently.

**Proposal:**

```cpp
/// @brief Create a lightweight view of this texture with a different
/// pixel format. The view shares the underlying resource memory; the
/// view's lifetime is bounded by the parent texture's.
/// Requires the parent's TextureDescriptor.allowFormatCast = true.
/// @returns A new GETexture handle aliasing the same storage. Returns
///          null if the cast is not compatible (different channel
///          layout, different bit depth) or the parent wasn't marked
///          cast-eligible.
virtual SharedHandle<GETexture> createFormatCastView(PixelFormat newFormat) = 0;
```

| OmegaGTE | D3D12 | Metal | Vulkan |
|---|---|---|---|
| `createFormatCastView(f)` | Separate SRV with the cast `DXGI_FORMAT`; parent resource must have been created with a typeless format (`R8G8B8A8_TYPELESS` etc.) | `[id<MTLTexture> newTextureViewWithPixelFormat:]` | `VkImageView` with `VkImageViewCreateInfo.format = ...` on an image created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` |

**Add to `TextureDescriptor`:**

```cpp
/// When true, the underlying resource is allocated with format-mutable
/// flags so `createFormatCastView` can produce compatible aliases.
/// Allocation overhead is small; opt in only on textures expected to
/// be sampled with multiple format interpretations (e.g. linear-vs-sRGB
/// render targets, structured-buffer-style texture views).
bool allowFormatCast = false;
```

**Compatibility constraints:**

- D3D12: typeless resource → fully-typed views. Only formats sharing channel layout + bit depth are compatible (R8G8B8A8_TYPELESS ↔ {UNORM, UNORM_SRGB, SNORM, UINT, SINT}; R32_TYPELESS ↔ {FLOAT, UINT, SINT}).
- Metal: aliasing compatibility table follows `MTLPixelFormat`'s family rules. Apple documents which pairs work; the implementation should call `[device supportsTextureSampleCount:]`-style validation (or maintain a static table) and return null for unsupported casts.
- Vulkan: `VK_KHR_image_format_list` (core in 1.2) lets the implementation pre-declare the cast set at image creation time, which avoids runtime validation cost. Use it when available; fall back to creating views eagerly and trusting the validation layer otherwise.

**Use cases unblocked:**

- sRGB ⇄ linear render target alias (common for tone-mapping passes)
- "Read as integer" for shader-side bit manipulation of texel data
- WTK Phase 6.6.1's "swap chain format is sRGB" path could allocate one render target and bind it as either linear or sRGB per pass, instead of two separate textures.

### 7.8 Implementation order

```
7.1 (TextureRegion mip/layer)         ─── enables 7.2, unblocks §6.5 cube/array
    │
7.2 (getBytes by region)              ─── depends on 7.1 for parity
    │
7.3 (metadata accessors)              ─── independent; pure additive
    │
7.5 (texture clear)                   ─── independent; needs §4.4-style compute fallback shader
    │
7.4 (compressed pixel formats)        ─── independent; large body of work, gated by GTEDeviceFeatures additions
    │
7.6 (async upload)                    ─── requires texture-back-pointer refactor in D3D12
    │
7.7 (format-cast views)               ─── requires TextureDescriptor.allowFormatCast opt-in flag
```

### 7.9 Priority assignment

**Tier 1 candidates** (low risk, additive, immediate value):
- §7.1 TextureRegion mip/layer
- §7.2 getBytes region overload
- §7.3 metadata accessors

**Tier 2 candidates** (medium risk, requires new infrastructure):
- §7.5 texture clear (needs a compute fallback shader, similar to §4.4)
- §7.4 compressed pixel formats (needs `GTEDeviceFeatures` extension + decoder-side asset support)

**Tier 3 candidates** (deeper structural changes):
- §7.6 async upload (D3D12 engine back-pointer refactor)
- §7.7 format-cast views (allocation-time flag, descriptor migration)

---

## File Change Summary

| File | Extensions |
|---|---|
| **Public API** | |
| `gte/include/omegaGTE/GEPipeline.h` | 1.1, 1.2, 1.7, 3.1 — vertex input, blend, polygon types, blit pipeline descriptor |
| `gte/include/omegaGTE/GECommandQueue.h` | 1.3, 1.4, 1.5, 2.1, 3.1, 4.1-4.4 — indexed/instanced/indirect draw, indirect dispatch, blit pipeline commands, buffer/texture ops |
| `gte/include/omegaGTE/GERenderTarget.h` | 1.3, 1.4, 1.5, 1.6, 1.7 — new draw methods, MRT, polygon types on CommandBuffer |
| `gte/include/omegaGTE/GE.h` | 3.1 — `makeBlitPipelineState()` engine method |
| **D3D12 Backend** | |
| `gte/src/d3d12/GED3D12.cpp` | 1.1, 1.2, 1.5, 3.2, 5.2 — PSO creation with vertex input / blend / command signature / blit PSO / blend fix |
| `gte/src/d3d12/GED3D12CommandQueue.h` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4 — new command declarations |
| `gte/src/d3d12/GED3D12CommandQueue.cpp` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4 — new command implementations |
| **Metal Backend** | |
| `gte/src/metal/GEMetal.mm` | 1.1, 1.2, 3.2 — PSO creation changes, blit PSO |
| `gte/src/metal/GEMetalCommandQueue.h` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4 — new command declarations |
| `gte/src/metal/GEMetalCommandQueue.mm` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4, 5.1 — new commands + region copy fix |
| **Vulkan Backend** | |
| `gte/src/vulkan/GEVulkan.cpp` | 1.1, 1.2, 3.2 — PSO creation changes, blit PSO |
| `gte/src/vulkan/GEVulkanCommandQueue.h` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4 — new command declarations |
| `gte/src/vulkan/GEVulkanCommandQueue.cpp` | 1.3, 1.4, 1.5, 2.1, 3.2, 4.1-4.4 — new command implementations |
| **Shared** | |
| `gte/src/GERenderTarget.cpp` | 1.3, 1.4, 1.5 — CommandBuffer forwarding methods |
| **Docs** | |
| `gte/docs/API.rst` | All extensions — public API documentation |

---

## Open Questions

1. **Vertex input layout scope**: Should the vertex input descriptor be part of the pipeline state (baked at creation), or should vertex buffer formats be set dynamically on the command encoder? Baked is what all three APIs prefer (it's part of the PSO), but dynamic vertex input exists on Vulkan via `VK_EXT_vertex_input_dynamic_state`. Baked is the conservative, portable choice.

ANSWER: Vertex Input Layout should be baked at creation.

2. **Blit pipeline vs. post-process pipeline**: The proposed `BlitPipeline` is essentially a specialized render pipeline with no vertex input. Should it be a distinct pipeline type, or should the render pipeline simply support a "no vertex buffer, full-screen triangle" mode? A distinct type is cleaner for the common case; collapsing into render is more flexible but adds API surface to the render path.

ANSWER: Do proposed BlitPipeline

3. **Mipmap generation shader**: D3D12 has no built-in mipmap generation. Should the engine ship an internal compute shader for this, or should it use the blit pipeline with a downsample fragment shader? Compute is more efficient (one dispatch per mip, no render pass overhead) but requires an internal OmegaSL compute shader compiled at build time.

ANWSER: Sure, include a OmegaSL compute shader for mipmap gen on D3D12. (Please implement this before proceeding to phase 3.)

RESOLUTION: Added `gte/src/shaders/mipmap_gen_2d.omegasl` (canonical OmegaSL source, box-filter 8x8 threadgroup). `GED3D12Engine::ensureMipmapGenPipeline()` embeds that source as a string literal and compiles it at runtime via `OmegaSLCompiler::Create(gteDevice)` + `compiler->compile({Source::fromString(...)})` (per `omegasl.h`), wraps the resulting `omegasl_shader` through the engine's standard `_loadShaderFromDesc(...)` path, and builds the pipeline through `makeComputePipelineState(...)`. `GED3D12CommandQueue::generateMipmaps()` lazily ensures the pipeline, builds per-mip SRV/UAV descriptors, maps OmegaSL bindings 0/1 to root parameter indices via `getRootParameterIndexOfResource(...)`, and dispatches one threadgroup per mip pair with proper subresource state transitions and UAV barriers. Transient descriptor heaps are kept alive on the queue via `retainedDescriptorHeaps` until submitted work completes. No inline HLSL is used.

4. **Multi-draw-indirect**: The current proposal covers single-draw-indirect. Multi-draw-indirect (multiple draws from one buffer in a single call) is a significant GPU-driven rendering feature. It requires `multiDrawIndirect` on Vulkan, `ExecuteIndirect` with count on D3D12, and `MTLIndirectCommandBuffer` on Metal. Should this be included in this plan or deferred?

ANSWER: Add as a later phase in this plan.

5. **Color attachment count in `RenderPassDescriptor`**: The MRT proposal changes `colorAttachment` from a single pointer to a vector. This is a breaking change for existing code that sets `desc.colorAttachment = &attachment`. What migration strategy: deprecate + new field, or break and fix all call sites?

ANSWER: Break and fix call sites. We want a working API without adding zillions of unecessary deprecations everywhere.
