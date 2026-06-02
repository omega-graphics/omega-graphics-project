# Vulkan Video Encode / Decode — OmegaGTE Enablement Plan

> **Status:** ❌ Not started (drafted 2026-05-17).
>
> **Companion to:** [wtk/docs/Media-API-Completion-Plan.md](../../wtk/docs/Media-API-Completion-Plan.md) — that document's Phase 2 (FFmpeg backend on Linux) and Phase 6 (Media → OmegaVA extraction) both depend on the work scoped here. **OmegaVA does not yet exist** in this tree; the Media plan's Phase 6 extracts it from `wtk/src/Media/`. This plan defines the surface that future module will consume.

## Goal

Expose the GPU device and synchronization primitives OmegaGTE already owns (Vulkan on Linux, Metal on macOS, D3D12 on Windows) so that **OmegaVA** can drive hardware video encode / decode on the *same* device the renderer uses — zero CPU bounce, zero second device, decoded frames land in a `GETexture` the renderer can sample immediately.

Concretely on Linux this means standing up the **Vulkan Video** path:
- Request the video decode / encode queue families and `VK_KHR_video_*` device extensions at `VkDevice` creation time.
- Surface the resulting `VkInstance` / `VkPhysicalDevice` / `VkDevice` / queue handles through a stable C++ accessor so OmegaVA can populate `AVVulkanDeviceContext` and hand it to FFmpeg.
- Provide `OmegaGraphicsEngine::wrapExternalVkImage` (and `wrapExternalMTLTexture` / `wrapExternalD3D12Resource` on the other backends) to wrap a decoder-produced image as a `GETexture` without copying.
- Lift the cross-backend type surface (pixel formats, stream descriptors, fence / semaphore primitives) into shared headers that OmegaVA's public API can reference.

Non-goals: implementing the decoder/encoder logic itself (that lives in OmegaVA's FFmpeg backend), or adding any media-layer code to OmegaGTE.

### Note on existing types

The plan below references the live GTE surface (not the abstract `GEDevice` that some drafts of related plans assumed). For clarity:

| What the plan calls it | Where it actually lives | Notes |
|---|---|---|
| Capability struct | `GTEDevice` in `gte/include/OmegaGTE.h` | Holds `type`, `name`, `features`, `native()` returning `VkPhysicalDevice` / `id<MTLDevice>` / `ID3D12Device *` |
| Backend capability subclass | `GTEVulkanDevice` / `GTEMetalDevice` / `GTED3D12Device` | Defined per-backend in `gte/src/<backend>/` |
| Engine (logical device + queues) | `OmegaGraphicsEngine` in `gte/include/omegaGTE/GE.h` | Owns `underlyingNativeDevice()` (returns `void *` — `VkDevice` / `id<MTLDevice>` / `ID3D12Device *`), `makeTexture`, `makeFence`, etc. |
| Backend engine subclass | `GEVulkanEngine` / `GEMetalEngine` / `GED3D12Engine` | Defined per-backend in `gte/src/<backend>/` |

The Phase 2 typed-handles accessor is added to `OmegaGraphicsEngine` (where `underlyingNativeDevice` already lives), **not** to `GTEDevice` — `GTEDevice` is a per-physical-device capability snapshot built once at enumeration, whereas the video-queue handles and enabled-extension list are properties of the logical device created in the engine constructor.

## Why this lives in OmegaGTE, not OmegaVA

The deciding constraint is **one GPU device per process**. Vulkan allows multiple `VkDevice`s on the same physical device, but the moment you have two, every resource handoff between them costs a CPU round-trip (export DMA-BUF / import) and an extra synchronization barrier. The whole point of Vulkan Video over VA-API is to keep the decoded `VkImage` on the renderer's device. That requires OmegaVA to ask OmegaGTE for the device, not stand up its own.

The same pattern applies to macOS (one `MTLDevice` between `VTDecompressionSession` and the renderer) and Windows (one `ID3D12Device` between MFTs and the renderer). The cross-platform shape is identical, so the enabling APIs belong on the cross-platform engine abstraction — `OmegaGraphicsEngine` and its `GEVulkanEngine` / `GEMetalEngine` / `GED3D12Engine` subclasses — not in three different OmegaVA backends.

## Phase 1: Vulkan device creation — request video queues + extensions

**File:** `gte/src/vulkan/GEVulkan.cpp` (device creation in the `GEVulkanEngine(SharedHandle<GTEVulkanDevice>)` constructor — see the queue-family scan around line 960 and the `enableDeviceExtension(...)` calls around line 940).

### 1.0 Prerequisite — fix the queue-family enumeration loop

The existing scan increments `id` only inside the graphics/compute branch:

```cpp
unsigned id = 0;
for(auto & q : queueFamilyProps){
    if(q.queueFlags & VK_QUEUE_GRAPHICS_BIT || q.queueFlags & VK_QUEUE_COMPUTE_BIT){
        queueFamilyIndices.push_back(id);
        // ... build VkDeviceQueueCreateInfo ...
    }
    else {
        continue;   // <-- skips ++id
    }
     ++id;
}
```

So on a device whose family-0 is e.g. transfer-only (or pure-compute on some Mesa configurations), every subsequent recorded family index is off by one. Today's graphics-only consumers haven't been bitten because NVIDIA / AMD / Intel all expose family 0 as graphics-capable, but adding `VK_QUEUE_VIDEO_*` probing means the loop will start *iterating across* video-only families that don't have GRAPHICS/COMPUTE — and the `++id` skip will silently desync every later index.

Fix shape: move `++id` out of the conditional (track an explicit `id` separate from the loop variable), or use index-based iteration over `queueFamilyProps`. Either lands cleanly in a Phase 0 commit before anything below touches the loop. **This is a Phase 1 prerequisite, not a parallel item** — every queue-family lookup after this point assumes the indices are correct.

### 1.1 Queue family selection

Today `GEVulkanEngine` enumerates queue families and populates `queueFamilyProps` / `queueFamilyIndices` (graphics / compute / transfer all collapsed into one bucket). Extend to also locate, in a separate pass once 1.0 is in:

- **Video decode queue family** — first family with `VK_QUEUE_VIDEO_DECODE_BIT_KHR` set.
- **Video encode queue family** — first family with `VK_QUEUE_VIDEO_ENCODE_BIT_KHR` set.

These are typically distinct from the graphics queue on every vendor (NVIDIA, AMD, Intel all expose a dedicated decode queue family; encode is often the same family as decode on AMD/Intel and separate on NVIDIA). Both may be absent on headless / compute-only physical devices — in which case the device is created without them and `useHardwareAccel` callers must degrade to software.

Add fields on `GEVulkanEngine` (same style as the existing `uploadQueue` / `uploadQueueFamily` pair landed by the Vulkan-Texture-Memory plan):
```cpp
std::uint32_t videoDecodeQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
std::uint32_t videoEncodeQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
VkQueue videoDecodeQueue = VK_NULL_HANDLE;
VkQueue videoEncodeQueue = VK_NULL_HANDLE;
bool hasVideoDecode = false;
bool hasVideoEncode = false;
```

When present, request **1** queue from each in `VkDeviceQueueCreateInfo` (FFmpeg only needs one of each; if a future OmegaVA grows parallel decode workloads we revisit).

### 1.2 Device extensions

The engine constructor already has an `enableDeviceExtension(name, required)` helper (used for `VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_KHR_synchronization2`, etc. — see lines ~942–952 of `GEVulkan.cpp`). It probes `vkEnumerateDeviceExtensionProperties`, appends to `extensionNames` if present, and returns the discovered bool. **Reuse it** — every video extension below threads through the same helper for free, no parallel discovery code:

| Extension | Required for |
|---|---|
| `VK_KHR_video_queue` | Base video API — must have for either decode or encode |
| `VK_KHR_video_decode_queue` | Decode operations |
| `VK_KHR_video_decode_h264` | H.264 decode |
| `VK_KHR_video_decode_h265` | H.265 / HEVC decode |
| `VK_KHR_video_decode_av1` | AV1 decode (FFmpeg 7.0+) |
| `VK_KHR_video_encode_queue` | Encode operations |
| `VK_KHR_video_encode_h264` | H.264 encode |
| `VK_KHR_video_encode_h265` | H.265 encode |
| `VK_KHR_video_encode_av1` | AV1 encode (FFmpeg 7.1+) |
| `VK_KHR_synchronization2` | Required by the video extensions (already conditionally enabled — see `hasSynchronization2Ext`) |
| `VK_KHR_sampler_ycbcr_conversion` | Sampling NV12 / P010 decoded images as YUV (or the implicit-format equivalent on 1.1+) |

The codec-specific extensions partition cleanly — request only those the physical device supports, and let the FFmpeg-side `get_format` callback's codec/profile match dictate which are actually exercised at runtime.

Bool flags on `GEVulkanEngine`, matching the existing `hasPushDescriptorExt` / `hasSynchronization2Ext` / `hasExtendedDynamicState` style — each one is the return value of an `enableDeviceExtension(NAME, /*required=*/false)` call:
```cpp
bool hasVideoQueueExt = false;
bool hasVideoDecodeQueueExt = false;
bool hasVideoEncodeQueueExt = false;
bool hasVideoDecodeH264Ext = false;
bool hasVideoDecodeH265Ext = false;
bool hasVideoDecodeAV1Ext = false;
bool hasVideoEncodeH264Ext = false;
bool hasVideoEncodeH265Ext = false;
bool hasVideoEncodeAV1Ext = false;
bool hasSamplerYcbcrConversionExt = false;
```

The base `VK_KHR_video_queue` is the gate — if it's absent, every codec-specific extension is skipped (don't waste enumeration calls when there's nothing to attach them to). `hasVideoDecode` / `hasVideoEncode` resolve to `hasVideoQueueExt && hasVideoDecodeQueueExt && (any codec-decode ext)` and likewise for encode.

### 1.3 Feature struct chain

Vulkan Video's feature negotiation requires chaining `VkPhysicalDeviceVulkanVideoMaintenance1FeaturesKHR` (and the codec-specific feature structs as they stabilize) onto `VkPhysicalDeviceFeatures2.pNext` during device creation. Add them only when the corresponding extension is being enabled.

### 1.4 Soft degradation

If a caller (OmegaVA) requests HW accel on a `GEVulkanEngine` that didn't get the video extensions (headless GPU, old driver, intentionally trimmed device), the handles accessor (Phase 2) returns the engine's handles with `videoDecodeQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED`. OmegaVA reads this and falls back to software with a one-time warning. **Hard-failing device creation when video isn't available is wrong** — OmegaGTE has plenty of consumers that don't touch video.

## Phase 2: Engine-handles accessor for OmegaVA

OmegaVA's `FFmpegAudioVideoProcessor` needs to populate an `AVVulkanDeviceContext`. Today the only handle reach-through is `OmegaGraphicsEngine::underlyingNativeDevice()`, which returns a `void *` to the `VkDevice` (per its doc comment — see `gte/include/omegaGTE/GE.h:336–337`). That's not enough — FFmpeg also needs the `VkInstance`, `VkPhysicalDevice`, queue handles, and the enabled extension list.

Add a typed accessor on `OmegaGraphicsEngine` (same level as `underlyingNativeDevice`, not on `GTEDevice` — the logical device, queues, and enabled-extension list are all properties of the engine, not the per-physical-device capability struct). The accessor returns `std::optional` so non-Vulkan backends naturally return `std::nullopt`:

```cpp
// gte/include/omegaGTE/GE.h — backend-agnostic, on OmegaGraphicsEngine
struct OmegaGraphicsEngineVulkanHandles {
    VkInstance       instance;
    VkPhysicalDevice physicalDevice;
    VkDevice         device;
    std::uint32_t graphicsQueueFamilyIndex;
    std::uint32_t transferQueueFamilyIndex;       // == graphics today; tracked separately for future split
    std::uint32_t videoDecodeQueueFamilyIndex;    // VK_QUEUE_FAMILY_IGNORED if absent
    std::uint32_t videoEncodeQueueFamilyIndex;    // VK_QUEUE_FAMILY_IGNORED if absent
    VkQueue graphicsQueue;
    VkQueue videoDecodeQueue;                     // VK_NULL_HANDLE if absent
    VkQueue videoEncodeQueue;                     // VK_NULL_HANDLE if absent
    OmegaCommon::Vector<const char *> enabledInstanceExtensions;
    OmegaCommon::Vector<const char *> enabledDeviceExtensions;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr;
};

class OmegaGraphicsEngine {
    // ... existing virtuals ...
public:
    // Default returns nullopt; only GEVulkanEngine overrides.
    virtual std::optional<OmegaGraphicsEngineVulkanHandles> vulkanHandles() const { return std::nullopt; }
    // Symmetric per-backend accessors. Each backend overrides exactly
    // one; the other two return nullopt.
    virtual std::optional<OmegaGraphicsEngineMetalHandles> metalHandles() const { return std::nullopt; }
    virtual std::optional<OmegaGraphicsEngineD3D12Handles> d3d12Handles() const { return std::nullopt; }
};
```

The Metal / D3D12 handle structs are smaller and shaped to match what `VTDecompressionSessionCreate` / MFT activation need:

```cpp
struct OmegaGraphicsEngineMetalHandles {
    void *device;             // id<MTLDevice>, opaque to non-Obj-C TUs
    std::uint64_t registryID; // for kVTVideoDecoderSpecification_RequiredDecoderGPURegistryID
};
struct OmegaGraphicsEngineD3D12Handles {
    ID3D12Device  *device;
    ID3D12CommandQueue *graphicsQueue;
};
```

`GEVulkanEngine::vulkanHandles()` populates from already-stored members (`instance`, `physicalDevice`, `device`, the `deviceQueuefamilies` table, and the `extensionNames` vector accumulated during the constructor's `enableDeviceExtension` calls). The constructor must retain a copy of `enabledDeviceExtensions` — today it lives in a local `extensionNames` in the ctor scope and is dropped after `vkCreateDevice`; Phase 2 promotes it to a member.

`underlyingNativeDevice()` keeps its existing void-pointer-to-VkDevice contract for backward compatibility with callers that just want the logical device — no break.

This is the only OmegaGTE surface that's strictly **required** by OmegaVA Phase 2. The rest of the plan below is the path to zero-copy; without it, OmegaVA can still drive Vulkan Video, but every decoded frame round-trips through `av_hwframe_transfer_data` → CPU buffer → `GETexture` upload. That works but throws away most of the Vulkan Video win.

## Phase 3: Shared type surface

`OmegaGTE::PixelFormat` already exists at `gte/include/omegaGTE/GTEBase.h:976`, with five color entries (`RGBA8Unorm`, `RGBA16Unorm`, `RGBA8Unorm_SRGB`, `BGRA8Unorm`, `BGRA8Unorm_SRGB`). OmegaVA's `VideoStreamDesc::pixelFormat` per the Media plan's Phase 0 introduced a parallel enum local to `MediaIO.h`; the work here is to retire that parallel enum and grow the GTE one.

### 3.1 Extend `OmegaGTE::PixelFormat`

Add YUV / planar entries the renderer doesn't sample directly today but the video path needs:

- `NV12` — 8-bit Y plane + interleaved CbCr (the canonical Vulkan Video decode output)
- `P010` — 10-bit Y + interleaved CbCr (HDR video)
- `YUV420P` — fully planar 8-bit
- `YUV422P` / `YUV444P` — chroma-subsampled planar variants
- `RGBA10A2` — 10-bit-per-channel HDR display format

Every site that exhaustively switches on `PixelFormat` must add explicit cases (or fall through) for the new entries. Known sites in this tree:

| File:line | Function | What to do with new entries |
|---|---|---|
| `gte/src/vulkan/GEVulkan.cpp:35` | `pixelFormatToVkFormat` | Map NV12/P010 → `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` / `VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`; planar YUV → multiplanar Vulkan formats |
| `gte/src/vulkan/GEVulkan.cpp:~1640` | makeTexture's ToGPU/FromGPU staging-bytes-per-texel switch (Vulkan-Texture-Memory plan) | YUV formats have *no single* bytesPerTexel; the staging buffer needs per-plane offsets — see Phase 4 note on `VkBufferImageCopy` per-plane |
| `gte/src/vulkan/GEVulkanTexture.cpp:48` | `bytesPerTexelFor` | Same — YUV needs to bypass this path entirely, not return a fake 4 |
| `gte/src/d3d12/...` | DXGI format translator | Future symmetry, not blocking Linux |
| `gte/src/metal/...` | MTLPixelFormat translator | Future symmetry, not blocking Linux |

YUV uploads from CPU through GTE's `copyBytes` are *not* a goal of this plan — the YUV consumer is `wrapExternalVkImage` (Phase 4), where the decoder writes the pixels directly. The exhaustive-switch updates above are about *recognising* the format on the type surface, not implementing CPU upload for it.

### 3.2 Shared `VideoStreamDesc` location

Two options, neither blocking:
1. Keep `VideoStreamDesc` in OmegaVA, just reference `OmegaGTE::PixelFormat` from it.
2. Promote it to OmegaGTE as `OmegaGTE::VideoStreamDesc`.

Option 1 is the default — `VideoStreamDesc` carries codec IDs, frame rate, etc. that are media concerns, not GPU concerns. Only the pixel format crosses the boundary.

### 3.3 Synchronization primitives

`AVVkFrame` carries `VkSemaphore`s for cross-queue synchronization. OmegaGTE already has `GEFence` (timeline-semaphore-backed on Vulkan — see `GEVulkanFence` in `gte/src/vulkan/GEVulkan.h:243`, and the `Retention::FenceGate` plumbing in `gte/src/common/GERetentionQueue.h`). The base `GEFence` in `gte/include/omegaGTE/GE.h:195` exposes only `getLastSignaledValue()`.

Add to the public `GEFence` surface:
- A `nativeHandle()` accessor returning a backend-erased `void *` (already the pattern for `OmegaGraphicsEngine::underlyingNativeDevice`) so the consumer can pull the `VkSemaphore` / `MTLSharedEvent` / `ID3D12Fence`.
- A static factory on `OmegaGraphicsEngine` (e.g. `wrapExternalFence(void *native, std::uint64_t initialValue)`) that builds a `GEFence` from an externally-owned semaphore so the renderer's command queue can wait on FFmpeg-signaled values through the same `GEFence` API it uses for everything else.

The wrap factory is the inverse of `makeFence()` — both return `SharedHandle<GEFence>`, both work with the existing `FenceGate` retention path. The new constructor on `GEVulkanFence` takes the external `VkSemaphore` and a flag indicating it does **not** own the handle (so the destructor skips `vkDestroySemaphore`).

## Phase 4: External image wrapping

The zero-copy path requires turning a decoder-produced `VkImage` into a `GETexture` without copying. Add to `OmegaGraphicsEngine` (alongside `makeTexture`):

```cpp
struct ExternalVkImageDesc {
    VkImage image;
    VkImageView view;            // optional — created lazily if VK_NULL_HANDLE
    VkFormat format;             // e.g. VK_FORMAT_G8_B8R8_2PLANE_420_UNORM for NV12
    VkExtent2D extent;
    VkImageLayout currentLayout; // decoder hands frames in DECODE_DST_KHR; renderer wants SHADER_READ_ONLY
    std::uint32_t sourceQueueFamilyIndex; // for queue-family ownership transfer (decode → graphics)
    // Caller asserts the image lives at least as long as the returned GETexture.
    // No ownership transfer; GTE does not vkDestroyImage on release.
};
virtual SharedHandle<GETexture> wrapExternalVkImage(const ExternalVkImageDesc &) { return nullptr; }
```

**Implementation must be a new subclass `GEVulkanExternalTexture`, not a flag on `GEVulkanTexture`.** The base `GEVulkanTexture` destructor now does three pieces of cleanup that all assume VMA ownership: `vmaDestroyImage`, `vkDestroyImageView`, and (post-Vulkan-Texture-Memory plan) `vmaDestroyBuffer` on the staging companion. A non-owning wrap would need to skip all three on the engine side and skip the staging-buffer-allocation step entirely. Bolting a `wrappedExternal_` flag onto `GEVulkanTexture` would scatter that conditional across `makeTexture`, `releaseNative()`, the destructor's retention-queue closure, and `copyBytes`/`getBytes` (which would have nothing to write into). A subclass concentrates the difference.

Shape:

```cpp
class GEVulkanExternalTexture : public GEVulkanTexture {
    // Override releaseNative to skip vmaDestroyImage / vmaDestroyBuffer.
    // The base img_view is owned only if we created it lazily — track with a small flag.
    void releaseNative() override;
    // copyBytes / getBytes have no staging buffer to point at; return early / 0.
    void copyBytes(void *, size_t) override { /* unsupported on wrapped externals */ }
    size_t getBytes(void *, size_t) override { return 0; }
};
```

Layout transitions are issued on the renderer's graphics queue via the existing pipeline-barrier infrastructure (the `priorShaderAccess` / `priorPipelineAccess` fields on `GEVulkanTexture` already do this for cycled-through sampling). Queue-family ownership transfer (decode → graphics) is **mandatory** for cross-queue resource access and is the easiest thing to get wrong — see Risks. The acquire-side barrier is emitted on the renderer's graphics queue against `srcQueueFamilyIndex = sourceQueueFamilyIndex` and `dstQueueFamilyIndex = graphicsQueueFamilyIndex`; the *release-side* barrier on the decode queue is OmegaVA's responsibility (it lives on the decoder side of the handoff).

Symmetric APIs on the other backends — same `wrapExternal*` virtual on `OmegaGraphicsEngine`:
- macOS: `wrapExternalMTLTexture(id<MTLTexture>, MTLPixelFormat, ...)` — wraps a VideoToolbox-produced `CVPixelBuffer`'s underlying `MTLTexture`. Same non-owning subclass pattern (`GEMetalExternalTexture`).
- Windows: `wrapExternalD3D12Resource(ID3D12Resource *, DXGI_FORMAT, ...)` — wraps an MFT-produced D3D12 resource (depends on Phase 4/5 of the Media plan, which currently runs MFT in CPU mode).

YCbCr sampling: NV12 / P010 require a `VkSamplerYcbcrConversion` for shader access. The wrapped texture should carry one (or expose enough info for the consumer to create one), keyed by format. This is new infrastructure on the GTE side — every existing `GETexture` consumer uses RGBA. The immutable-sampler pipeline path (`GEVulkanEngine::createPipelineLayoutFromShaderDescs`, takes `outImmutableSamplers`) will need extension to thread a `VkSamplerYcbcrConversion` into descriptor-set layout creation, since YCbCr conversion is baked into the sampler at pipeline-layout time. Alternative is the NV12 → RGBA compute-shader path called out in Risks.

## Phase 5: Test surface

Add to `gte/tests/`:
- **`vulkan_video_device_smoke`** — creates a `GEVulkanEngine` via the standard `GTE::Init(device)` path, downcasts via `dynamic_pointer_cast<GEVulkanEngine>` (or calls `vulkanHandles()` if added Phase 2), asserts the video queue family indices are populated on a host with a video-capable GPU, and `VK_QUEUE_FAMILY_IGNORED` with no abort otherwise. No actual decode — just engine-creation coverage.
- **`vulkan_external_image_wrap`** — allocates a `VkImage` directly (bypassing GTE's `makeTexture`), wraps it via `wrapExternalVkImage`, samples it in a compute shader, asserts the contents match. Exercises ownership transfer between the test's "decode" queue and the engine's graphics queue — sampling without the transfer is the silent-corruption failure mode called out in Risks.

Actual end-to-end decode tests live on the OmegaVA side (per the Media plan's Phase 6.9 test split). The GTE-side tests only validate that the surfaces OmegaVA depends on behave as documented.

## Phase 6: macOS / Windows symmetry (deferred)

The Linux Vulkan Video path is the immediate driver because it's the **only** platform where the Media plan currently has zero implementation. macOS and Windows backends already work; they just don't share a GPU device with the renderer today. Tracked as future work; the type surface (Phase 3) lands now so that when macOS / Windows interop comes online, the shape is already there.

The Metal / D3D12 *physical* devices are already reachable today: `GTEDevice::native()` returns `id<MTLDevice>` on `GTEMetalDevice` and `ID3D12Device *` on `GTED3D12Device`. What's missing is the **handoff plumbing** for each platform's video pipeline:

| Backend | What it needs | When |
|---|---|---|
| Metal | `wrapExternalMTLTexture` + an `OmegaGraphicsEngineMetalHandles::registryID` accessor so AVFoundation's `VTDecompressionSessionCreate` can be pinned to the renderer's `MTLDevice` via `kVTVideoDecoderSpecification_RequiredDecoderGPURegistryID`. (`id<MTLDevice>` itself is already reachable via `GTEMetalDevice::native()`.) | After Linux ships, opportunistic |
| D3D12 | `wrapExternalD3D12Resource` + an `OmegaGraphicsEngineD3D12Handles::device` accessor pointing at the existing `ID3D12Device`, so MFTs that support D3D12 backing can be activated against it (newer MFTs do; legacy ones are D3D11-only). | Tied to Media plan Phase 4/5 codec-pair API revamp |

## Migration order

0. **Phase 1.0 prereq** (queue-family enumeration `++id` fix) — independent two-line correction in `GEVulkan.cpp`. Lands first; everything else assumes the fix is in.
1. **Phase 1** (queue families + device extensions) — single PR in `gte/src/vulkan/`. No public API change; net effect is the Vulkan `VkDevice` is created with video bits when the hardware supports them. Verifiable in isolation with a small device-introspection test.
2. **Phase 2** (engine-handles accessor) — extends `OmegaGraphicsEngine` public API. Unblocks OmegaVA Phase 2 from the Media plan, since FFmpeg can now be handed a populated `AVVulkanDeviceContext`. **This is the blocking item for OmegaVA Phase 2.**
3. **Phase 3** (shared type surface) — coordinated with Media plan Phase 6 (the OmegaVA extraction); the `PixelFormat` extension is most naturally done as part of the extraction PR rather than separately, because the exhaustive-switch updates in `pixelFormatToVkFormat` / `bytesPerTexelFor` should land in the same commit as the enum additions.
4. **Phase 4** (external image wrapping) — turns the OmegaVA path from "works, with a CPU bounce" into "zero copy". Can ship after OmegaVA's software path is proven; the FFmpeg integration falls back to `av_hwframe_transfer_data` until this lands.
5. **Phase 5** (tests) — alongside Phases 1 and 4.
6. **Phase 6** (macOS / Windows) — deferred.

## Risks

- **Queue-family enumeration `++id` skip (Phase 1.0).** Today's loop in `GEVulkan.cpp:964` increments `id` only when a family matches GRAPHICS/COMPUTE. Adding video probing while iterating over the same loop will silently mis-index every video family on devices that don't expose family-0 as graphics-capable. The Phase 1.0 prereq is small but **must not be skipped or merged after** the video-extension probing — they're co-dependent.
- **Driver coverage on Linux is uneven.** Mesa (AMD/Intel) shipped Vulkan Video decode for H.264/H.265 in 2024 and AV1 decode in 2025; encode lags. NVIDIA's proprietary driver has had decode support longer but historically prefers NVDEC over Vulkan Video. The soft-degradation path (Phase 1.4) exists for exactly this — on a driver without the extensions, OmegaVA's HW request becomes a software request. The test rig (Phase 5) must run on at least one driver where the path is exercised end-to-end.
- **Synchronization is the easiest thing to get wrong.** Queue-family ownership transfer between the decode queue and graphics queue is mandatory for cross-queue resource access, and forgetting it produces visually-fine-but-occasionally-corrupt frames that won't reproduce in single-frame tests. The Phase 5 wrap test should specifically exercise a layout/ownership transfer, not just a sample.
- **YCbCr sampling is a new code path.** Existing `GETexture` consumers all use RGBA. NV12 sampling requires a `VkSamplerYcbcrConversion` baked into the descriptor set layout, which means the existing immutable-sampler pipeline path (`createPipelineLayoutFromShaderDescs` — see the `outImmutableSamplers` parameter) needs extension to thread a `VkSamplerYcbcrConversion` through `VkSamplerCreateInfo::pNext`. Audit before committing to wrapped NV12; alternative is to insert a one-time NV12 → RGBA compute shader on first sample, trading some GPU work for compatibility with the existing descriptor infrastructure.
- **Staging-buffer destructor assumes VMA ownership.** The Vulkan-Texture-Memory plan landed a `vmaDestroyBuffer(stagingBuffer)` path in `GEVulkanTexture`'s destructor closure. A non-owning wrap of an externally-allocated `VkImage` must use a distinct subclass (`GEVulkanExternalTexture`) rather than a flag — anything that flows through the base destructor will try to free a buffer it never allocated. Phase 4 calls this out; flagged here so the implementation PR doesn't take the flag shortcut.
- **`AVVulkanDeviceContext` ABI.** The struct's layout is part of FFmpeg's ABI but new fields (e.g. `nb_decode_queues`, `queue_family_decode_index`) have been added over recent FFmpeg minor versions. The OmegaVA-side `pkg_check_modules` must pin to `libavutil` ≥ 58 (FFmpeg 7.0) and reject older versions at configure time. Surfaced here because that constraint flows from this plan's design choice.
