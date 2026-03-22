# Plan: Rename Tessellation Engine to Triangulation Engine

## Motivation

The Tessellation Engine (TE) is OmegaGTE's geometric mesh generation system — it takes primitive descriptions (rects, rounded rects, ellipsoids, cylinders, etc.) and produces triangle meshes. This is **not** GPU tessellation (hull/domain shader stages). Now that OmegaSL has full hull/domain shader support with `hull(domain=tri, ...)` and `domain(domain=tri)` syntax, the name "Tessellation Engine" creates confusion between two unrelated systems:

- **Triangulation Engine** (TE) — CPU/GPU mesh generation from primitives
- **Tessellation stages** — hull/domain shader pipeline stages

Renaming to "Triangulation Engine" clarifies that TE produces triangle meshes from geometric descriptions, while tessellation refers exclusively to the shader pipeline.

## Scope

The rename touches **public API**, **internal implementation**, **tests**, **shader files**, and **comments**. File names (`TE.h`, `TE.cpp`, `*TEContext.*`) stay unchanged — "TE" remains the abbreviation, now standing for "Triangulation Engine".

## Symbol Rename Map

### Public API (headers in `include/omegaGTE/`)

| File | Old | New |
|---|---|---|
| `TE.h` | `TETessellationParams` | `TETriangulationParams` |
| `TE.h` | `TETessellationResult` | `TETriangulationResult` |
| `TE.h` | `TETessellationResult::TEMesh` | `TETriangulationResult::TEMesh` (unchanged) |
| `TE.h` | `OmegaTessellationEngineContext` | `OmegaTriangulationEngineContext` |
| `TE.h` | `OmegaTessellationEngine` | `OmegaTriangulationEngine` |
| `TE.h` | `TessalationType` enum | `TriangulationType` |
| `TE.h` | `TESSALATE_RECT` ... `TESSALATE_GRAPHICSPATH3D` | `TRIANGULATE_RECT` ... `TRIANGULATE_GRAPHICSPATH3D` |
| `TE.h` | `tessalateSync()` | `triangulateSync()` |
| `TE.h` | `tessalateOnGPU()` | `triangulateOnGPU()` |
| `TE.h` | `tessalateAsync()` | `triangulateAsync()` |
| `TE.h` | `_tessalatePriv()` | `_triangulatePriv()` |
| `TE.h` | `extractGPUTessParams()` | `extractGPUTriangulationParams()` |
| `TE.h` | `createTEContextFromNativeRenderTarget()` | unchanged (TE abbreviation stays) |
| `TE.h` | `createTEContextFromTextureRenderTarget()` | unchanged |
| `OmegaGTE.h` | `tessalationEngine` member | `triangulationEngine` |

### Core Implementation (`src/`)

| File | Changes |
|---|---|
| `TE.cpp` | All class/method/variable references: `TETessellationParams` → `TETriangulationParams`, `TETessellationResult` → `TETriangulationResult`, `OmegaTessellationEngine` → `OmegaTriangulationEngine`, `OmegaTessellationEngineContext` → `OmegaTriangulationEngineContext`, `_tessalatePriv` → `_triangulatePriv`, `TessalationType` → `TriangulationType`, all `TESSALATE_*` → `TRIANGULATE_*` |
| `OmegaGTE.cpp` | `OmegaTessellationEngine::Create()` → `OmegaTriangulationEngine::Create()`, `gte.tessalationEngine` → `gte.triangulationEngine` |

### Backend Context Files (`src/metal/`, `src/d3d12/`, `src/vulkan/`)

Each backend file follows the same pattern:

| File | Changes |
|---|---|
| `MetalTEContext.mm` | `OmegaTessellationEngineContext` → `OmegaTriangulationEngineContext`, `TETessellationResult` → `TETriangulationResult`, `TETessellationParams` → `TETriangulationParams`, `tessalateSync` → `triangulateSync`, `tessalateOnGPU` → `triangulateOnGPU`, class names `MetalNativeRenderTargetTEContext` / `MetalTextureRenderTargetTEContext` unchanged |
| `D3D12TEContext.cpp` | Same pattern as Metal |
| `VulkanTEContext.cpp` | Same pattern as Vulkan |

### Test Files

| File | Changes |
|---|---|
| `tests/metal/2DTest/main.mm` | `OmegaTessellationEngineContext` → `OmegaTriangulationEngineContext`, `tessalateSync` → `triangulateSync`, `tessalationEngine` → `triangulationEngine`, local variable names |
| `tests/metal/GPUTessTest/main.mm` | Same pattern |
| `tests/directx/2DTest/main.cpp` | Same pattern |
| `tests/directx/GPUTessTest/main.cpp` | Same pattern, rename directory to `GPUTriangulateTest` (optional) |
| `tests/vulkan/2DTest/main.cpp` | Same pattern |
| `tests/vulkan/CPUTessTest/main.cpp` | Same pattern, rename directory to `CPUTriangulateTest` (optional) |
| `tests/vulkan/GPUTessTest/main.cpp` | Same pattern |

### Shader Files (`src/shaders/`)

These are compute shaders that generate triangle meshes. Rename for consistency:

| Old | New |
|---|---|
| `tess_rect.omegasl` | `triangulate_rect.omegasl` |
| `tess_rounded_rect.omegasl` | `triangulate_rounded_rect.omegasl` |
| `tess_ellipsoid.omegasl` | `triangulate_ellipsoid.omegasl` |
| `tess_rect_prism.omegasl` | `triangulate_rect_prism.omegasl` |
| `tess_path2d.omegasl` | `triangulate_path2d.omegasl` |

Internal function names (e.g. `tess_rect_kernel`) should also be renamed to `triangulate_rect_kernel`.

### What Does NOT Change

- **File names**: `TE.h`, `TE.cpp`, `*TEContext.*` — "TE" stays as the abbreviation
- **Include guard**: `OMEGAGTE_TE_H` — stays
- **TEMesh**: Generic mesh output type, no "tessellation" in the name
- **OmegaSL tessellation stages**: `hull(...)`, `domain(...)` — these ARE actual tessellation and keep their names
- **`VulkanTessSpirv.inc`**: Will be replaced entirely when Vulkan tess shaders compile from OmegaSL (plan item 4.5)

## Execution Steps

### Step 1: Public header (`TE.h`)

Rename all public types, enums, methods. This is the API surface — all downstream changes follow from it.

### Step 2: Core implementation (`TE.cpp`, `OmegaGTE.cpp`, `OmegaGTE.h`)

Update implementations and the GTE init struct.

### Step 3: Backend contexts

Update Metal, D3D12, Vulkan context files. Mechanical find-and-replace within each file.

### Step 4: Shader files

Rename `.omegasl` files and internal kernel function names.

### Step 5: Test files

Update all test code. Optionally rename test directories.

### Step 6: Build verification

Rebuild all targets across all platforms. The glob-based CMake picks up renamed shader files automatically.

## Risk

Low for the rename itself — purely mechanical. The main risk is downstream code (OmegaWTK, apps) that uses the public API. A grep for `Tessellation|tessalate|TESSALATE` across the full repository will catch all call sites. The old names should NOT be preserved as aliases — a clean break avoids ongoing confusion.

## Estimated Scope

~20 files, ~300 symbol replacements. No logic changes. Entirely automated via find-and-replace per the symbol map above.
