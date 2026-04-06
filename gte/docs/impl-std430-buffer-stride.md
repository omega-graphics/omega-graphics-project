# std430 Buffer Stride Fix — Vulkan Buffer Writer/Reader

## Problem

GLSL `std430` layout requires SSBO array elements to be padded to the struct's
alignment boundary. The struct alignment equals its largest member's alignment.

| Struct                                | Raw Size | Struct Align | std430 Stride |
|---------------------------------------|----------|--------------|---------------|
| `{ vec4 pos; vec4 color; }`           | 32       | 16           | 32 (no-op)    |
| `{ vec4 pos; vec2 texCoord; }`        | 24       | 16           | **32**        |
| `{ float radius; uint w; uint h; float angle; }` | 16 | 4     | 16 (no-op)    |
| `{ float x; vec4 v; }`               | 20       | 16           | **32**        |
| `{ vec2 a; vec2 b; }`                | 16       | 8            | 16 (no-op)    |
| `{ vec4 a; vec3 b; }`                | 28       | 16           | **32**        |

Today `GEVulkanBufferWriter::sendToBuffer` writes fields back-to-back (raw
size). `omegaSLStructSize` returns the raw size. When the raw size is not a
multiple of the struct alignment, the GPU reads subsequent array elements at the
wrong offsets — producing garbage data and invisible geometry.

D3D12 (`StructuredBuffer<T>`) and Metal (`device T*`) use natural C packing
where the stride equals the raw size. **This fix is Vulkan-only.**

## std430 Alignment Rules Reference

| GLSL Type          | Alignment (bytes) | Size (bytes) |
|--------------------|--------------------|--------------|
| `float`, `int`, `uint` | 4             | 4            |
| `vec2`, `ivec2`, `uvec2` | 8           | 8            |
| `vec3`, `ivec3`, `uvec3` | 16          | 12           |
| `vec4`, `ivec4`, `uvec4` | 16          | 16           |
| `mat2`             | 16 (col = vec2, but array-of-vec2 align = 16) | 32 |
| `mat3`             | 16                 | 48           |
| `mat4`             | 16                 | 64           |

Struct alignment = `max(member alignments)`.
Array element stride = `ceil(struct_raw_size / struct_alignment) * struct_alignment`.

## Design

### 1. New helper: `std430Alignment` and `std430Stride`

Add to `GTEBase.cpp` alongside `omegaSLStructSize`:

```
size_t omegaSLStd430Alignment(omegasl_data_type type);
size_t omegaSLStd430StructStride(Vector<omegasl_data_type> fields);
```

`omegaSLStd430Alignment` maps each `omegasl_data_type` to its std430 alignment:

| Type                   | Alignment |
|------------------------|-----------|
| `FLOAT`, `INT`, `UINT` | 4         |
| `FLOAT2`, `INT2`, `UINT2` | 8      |
| `FLOAT3`, `INT3`, `UINT3` | 16     |
| `FLOAT4`, `INT4`, `UINT4` | 16     |
| `FLOAT2x2`            | 16        |
| `FLOAT3x3`            | 16        |
| `FLOAT4x4`            | 16        |
| All other matrix types | 16       |

`omegaSLStd430StructStride` computes:

```
structAlign = max(omegaSLStd430Alignment(f) for f in fields)
rawSize     = omegaSLStructSize(fields)   // existing function, returns packed size
stride      = ((rawSize + structAlign - 1) / structAlign) * structAlign
return stride
```

### 2. `GEVulkanBufferWriter::sendToBuffer` — pad to stride

After writing all field data blocks, advance `currentOffset` to the next
multiple of the struct alignment.

The writer already accumulates `blocks` between `structBegin`/`structEnd`, so it
can derive the struct alignment from the block types at `sendToBuffer` time:

```cpp
void sendToBuffer() override {
    assert(!inStruct && "...");
    assert(mem_map != nullptr && "...");

    // 1. Compute struct alignment from accumulated blocks.
    size_t structAlign = 1;
    for (auto &b : blocks) {
        size_t a = std430AlignmentForType(b.type);
        if (a > structAlign) structAlign = a;
    }

    // 2. Write field data (existing logic).
    for (auto &b : blocks) {
        size_t si = sizeForType(b.type);
        memcpy(mem_map + currentOffset, b.data, si);
        currentOffset += si;
    }

    // 3. Pad to struct alignment boundary.
    size_t remainder = currentOffset % structAlign;
    if (remainder != 0) {
        size_t padding = structAlign - remainder;
        memset(mem_map + currentOffset, 0, padding);
        currentOffset += padding;
    }
}
```

`std430AlignmentForType` is a private helper (or inline switch) that returns the
alignment for a single `omegasl_data_type`:

```cpp
static size_t std430AlignmentForType(omegasl_data_type type) {
    switch (type) {
        case OMEGASL_FLOAT: case OMEGASL_INT: case OMEGASL_UINT:
            return 4;
        case OMEGASL_FLOAT2: case OMEGASL_INT2: case OMEGASL_UINT2:
            return 8;
        // vec3 has alignment 16 in std430
        case OMEGASL_FLOAT3: case OMEGASL_INT3: case OMEGASL_UINT3:
        case OMEGASL_FLOAT4: case OMEGASL_INT4: case OMEGASL_UINT4:
            return 16;
        // Matrix columns are vectors; alignment follows the column type.
        default:
            return 16;
    }
}
```

Note: `sizeForType` is the existing per-block size computation already in
`sendToBuffer`; it should be extracted into a private static helper to avoid
duplication.

### 3. `GEVulkanBufferReader::structEnd` — skip padding

The reader currently has empty `structBegin`/`structEnd` stubs and an unused
`setStructLayout`. Two options:

**Option A — derive from reads (symmetric with writer):**

Track the types read between `structBegin`/`structEnd`. In `structEnd`, compute
the struct alignment from those types and advance `currentOffset` over the
padding:

```cpp
// New member:
OmegaCommon::Vector<omegasl_data_type> readTypes;

void structBegin() override { readTypes.clear(); }

void getFloat4(FVec<4> &v) override {
    // ... existing read logic ...
    readTypes.push_back(OMEGASL_FLOAT4);
}

void structEnd() override {
    size_t structAlign = 1;
    for (auto t : readTypes) {
        size_t a = std430AlignmentForType(t);
        if (a > structAlign) structAlign = a;
    }
    size_t remainder = currentOffset % structAlign;
    if (remainder != 0) {
        currentOffset += structAlign - remainder;
    }
}
```

**Option B — use `setStructLayout`:**

The reader already has `setStructLayout(Vector<omegasl_data_type> fields)`.
Store the fields, precompute the stride, and skip padding in `structEnd`:

```cpp
size_t structStride = 0;
size_t structStartOffset = 0;

void setStructLayout(Vector<omegasl_data_type> fields) override {
    structStride = omegaSLStd430StructStride(fields);
}

void structBegin() override {
    structStartOffset = currentOffset;
}

void structEnd() override {
    if (structStride > 0) {
        currentOffset = structStartOffset + structStride;
    }
}
```

**Recommendation: Option A.** It keeps the reader symmetric with the writer (no
separate layout declaration needed), and the per-struct alignment cost is
negligible.

### 4. `omegaSLStructSize` callers — buffer allocation

All callers that use `omegaSLStructSize` to compute buffer allocation sizes for
Vulkan SSBOs must switch to `omegaSLStd430StructStride`:

| File | Line(s) | Current | After |
|------|---------|---------|-------|
| `wtk/.../RenderTarget.cpp` | 301, 1099 | `omegaSLStructSize({F4,F2,F2})` | `omegaSLStd430StructStride({F4,F2})` |
| `wtk/.../RenderTarget.cpp` | 1106 | `omegaSLStructSize({F4,F4})` | `omegaSLStd430StructStride({F4,F4})` — no-op (32=32) |
| `wtk/.../RenderTarget.cpp` | 742 | `omegaSLStructSize({F})` | `omegaSLStd430StructStride({F})` — no-op (4=4) |
| `wtk/.../RenderTarget.cpp` | 756 | `omegaSLStructSize({F,F4})` | `omegaSLStd430StructStride({F,F4})` — currently 24→32 |
| `wtk/.../RenderTarget.cpp` | 1568, 1621 | `omegaSLStructSize({F,U,U,F})` | `omegaSLStd430StructStride({F,U,U,F})` — no-op (16=16) |
| `gte/.../BlitTest/main.cpp` | 237 | `{F4,F4}` | no-op |
| `gte/.../BlitTest/main.cpp` | 252 | `{F4,F2,F2}` | `omegaSLStd430StructStride({F4,F2})` |
| `gte/.../2DTest/main.cpp` | 80 | `{F4,F4}` | no-op |
| `gte/.../ComputeTest/main.cpp` | 45 | `{F4}` | no-op |
| `gte/.../directx/2DTest/main.cpp` | 61 | `{F4,F2}` | **leave as-is** (D3D12, no std430) |
| `gte/.../directx/ComputeTest/main.cpp` | 46 | `{F4}` | no-op |
| `gte/.../metal/*` | various | various | **leave as-is** (Metal, no std430) |

### 5. Revert explicit `_pad` fields

Once the writer/reader and size callers are updated, the `_pad` workarounds
added in the current session should be removed:

- `gte/tests/vulkan/BlitTest/main.cpp` — remove `float2 _pad` from
  `CopyVertex`, remove `writeFloat2(pad)` calls, change struct size back to
  `{OMEGASL_FLOAT4, OMEGASL_FLOAT2}` (now via `omegaSLStd430StructStride`)
- `wtk/.../compositor.omegasl` — remove `float2 _pad` from
  `OmegaWTKTexturedVertex`
- `wtk/.../RenderTarget.cpp` — remove `writeFloat2(pad)` / `writeFloat2(texPad)`
  calls, change struct sizes back to `{OMEGASL_FLOAT4, OMEGASL_FLOAT2}`

### 6. Platform gating

`omegaSLStd430StructStride` must only apply padding on Vulkan/OpenGL.
Implementation options (pick one):

- **Compile-time `#ifdef TARGET_VULKAN`** in `GTEBase.cpp` — simplest; mirrors
  existing `#if defined(TARGET_METAL)` branches in `omegaSLStructSize`.
- **Runtime parameter** — pass a layout enum (`PackedLayout`, `Std430Layout`).
  More flexible but currently unnecessary.

Recommendation: compile-time gate. On non-Vulkan targets,
`omegaSLStd430StructStride` returns the same value as `omegaSLStructSize`.

## Files Changed

| File | Change |
|------|--------|
| `gte/include/omegaGTE/GTEShader.h` | Declare `omegaSLStd430StructStride` |
| `gte/src/GTEBase.cpp` | Implement `omegaSLStd430Alignment`, `omegaSLStd430StructStride` |
| `gte/src/vulkan/GEVulkan.cpp` | `GEVulkanBufferWriter::sendToBuffer` — add stride padding. Extract `sizeForType`/`std430AlignmentForType` helpers. `GEVulkanBufferReader::structEnd` — skip padding bytes. |
| `gte/src/BufferIO.h` | (no change needed) |
| `wtk/.../compositor.omegasl` | Revert `_pad` field |
| `wtk/.../RenderTarget.cpp` | Revert padding writes. Switch `omegaSLStructSize` → `omegaSLStd430StructStride` for textured vertex buffers. |
| `gte/tests/vulkan/BlitTest/main.cpp` | Revert `_pad` field and padding writes. Switch to `omegaSLStd430StructStride`. |

## Testing

1. **BlitTest** — colored triangle renders through offscreen texture blit
   (verifies `{vec4, vec2}` stride = 32).
2. **2DTest** — colored rect renders to native target (verifies `{vec4, vec4}`
   stride = 32, no regression).
3. **BasicAppTest** — WTK compositor renders visible content.
4. **ComputeTest** — compute pipeline with `{vec4}` buffer (no regression).
5. Verify no change on D3D12/Metal test targets.
