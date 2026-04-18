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

### 1.5 Indirect Drawing

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

### 1.6 Multiple Color Attachments (MRT)

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

### 1.7 Additional Polygon Types

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

### 2.1 Indirect Dispatch

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
virtual void setComputePushConstants(const void *data, unsigned size, unsigned offset = 0) = 0;
```

**Add to `GERenderTarget::CommandBuffer` (public, for render pass):**

```cpp
/// @brief Sets push constant data for the current render pipeline.
/// @param data Pointer to the constant data.
/// @param size Size in bytes (max 128).
/// @param offset Byte offset into the push constant range.
void setRenderPushConstants(const void *data, unsigned size, unsigned offset = 0);
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

**OmegaSL integration**: Push constants need a way to be declared in the shader language. Propose a `pushconst` keyword:

```
pushconst PerFrameData {
    float4x4 viewProjection;
    float time;
};
```

This compiles to a root constant range (D3D12), a `setBytes` buffer index (Metal), or a push constant block (Vulkan). The compiler's layout extraction already knows about resource bindings; push constants would be a new layout category.

**Recommendation**: Push constants require OmegaSL changes. Propose as a paired feature with OmegaSL push constant support rather than a standalone command buffer extension.

### 2.3 `ComputePipelineDescriptor` — Threadgroup Size Override

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

### 4.1 Buffer-to-Buffer Copy

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

### 4.2 Buffer ↔ Texture Transfers

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

### 4.3 Mipmap Generation

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

### 4.4 Texture and Buffer Fills

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

---

## Extension 5: Fix Existing Bugs ✅ Implemented

These are bugs found during the audit that should be fixed regardless of new features.

### 5.1 Metal Region Copy ✅ Implemented

`GEMetalCommandQueue.mm` copies entire mipmaps instead of the specified region when `copyTextureToTexture(src, dest, region, destCoord)` is called. The Metal blit encoder's `copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:` method should be used with the region parameters mapped to `MTLOrigin` + `MTLSize`.

### 5.2 D3D12 Blend State Hardcoding ✅ Implemented

`GED3D12.cpp:790-799` hardcodes SrcAlpha / InvSrcAlpha blending for all render pipelines. This should be driven by `RenderPipelineDescriptor.colorBlendDescriptors` once Extension 1.2 is implemented. Until then, the hardcoded blend is a known limitation.

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
- 4.1: Buffer-to-buffer copy
- 4.2: Buffer ↔ texture transfers
- 1.7: Additional polygon types

**Tier 2 — High Impact, Medium Risk** (implement second):
- 1.1: Vertex input layout (touches PSO creation on all backends) ✅
- 1.2: Blend state (touches PSO creation on all backends) ✅
- 2.1: Indirect dispatch
- 4.3: Mipmap generation

**Tier 3 — Medium Impact, Higher Complexity** (implement third):
- 1.5: Indirect drawing
- 1.6: Multiple color attachments
- 3.x: Blit pipeline (requires internal shaders, render pass management)

**Tier 4 — Deferred** (requires OmegaSL work):
- 2.2: Push constants
- 2.3: Threadgroup size override

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

ANSWER: Just move blit ops into Render Pipeline. (No `BlitPipeline` type)

3. **Mipmap generation shader**: D3D12 has no built-in mipmap generation. Should the engine ship an internal compute shader for this, or should it use the blit pipeline with a downsample fragment shader? Compute is more efficient (one dispatch per mip, no render pass overhead) but requires an internal OmegaSL compute shader compiled at build time.

ANWSER: Sure, include a OmegaSL shader for mipmap gen on D3D12.

4. **Multi-draw-indirect**: The current proposal covers single-draw-indirect. Multi-draw-indirect (multiple draws from one buffer in a single call) is a significant GPU-driven rendering feature. It requires `multiDrawIndirect` on Vulkan, `ExecuteIndirect` with count on D3D12, and `MTLIndirectCommandBuffer` on Metal. Should this be included in this plan or deferred?

ANSWER: Add as a later phase in this plan.

5. **Color attachment count in `RenderPassDescriptor`**: The MRT proposal changes `colorAttachment` from a single pointer to a vector. This is a breaking change for existing code that sets `desc.colorAttachment = &attachment`. What migration strategy: deprecate + new field, or break and fix all call sites?

ANSWER: Break and fix call sites. We want a working API without adding zillions of unecessary deprecations everywhere.
