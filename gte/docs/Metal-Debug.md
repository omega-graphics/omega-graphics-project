# Metal Debugging in OmegaGTE

This covers the Metal-specific debug surfaces. For the cross-backend
design (the `GTEInitOptions::debugLayer` toggle, `DEBUG_STREAM`, the
`OMEGAGTE_DEBUG` compile flag) see `Debug-Layer-Plan.md`.

## What `debugLayer` controls on Metal

`GTEInitOptions::debugLayer = Enabled` (or an `OMEGAGTE_DEBUG` build):

- **Gates `DEBUG_STREAM`** — the engine's own lifecycle/diagnostic logging.
- **Gates programmatic GPU capture** — see [GPU capture](#programmatic-gpu-capture).

It does **not** turn on Metal's native API validation. Unlike D3D12
(`ID3D12Debug::EnableDebugLayer`) and Vulkan (`VK_LAYER_KHRONOS_validation`),
Metal has **no public runtime API** to enable API validation — it is read
from the process environment *before* the `MTLDevice` is created. So Metal
API validation is driven by environment variables (or the Xcode scheme),
documented below.

## Metal validation environment variables

We could set these by default upon startup if the build with a macro OMEGAGTE_ENABLE_METAL_VALIDATION.

Set these in the environment (or the Xcode scheme's *Run > Diagnostics* /
*Options* tabs) before launching. They are read at device-creation time.

| Variable | Effect |
|----------|--------|
| `METAL_DEVICE_WRAPPER_TYPE=1` | Enables Metal **API validation** (CPU-side misuse: draw without a pipeline, bad bindings, use-after-free). The primary toggle. |
| `MTL_DEBUG_LAYER=1` | Extended API validation (the Xcode-style "Metal API Validation" layer). |
| `MTL_SHADER_VALIDATION=1` | Shader-side bounds checking (macOS 14+ / iOS 17+). |
| `METAL_ERROR_MODE=5` | Abort on validation error (`5` = assert/break, `3` = print only). |
| `MTL_CAPTURE_ENABLED=1` | Allows programmatic GPU capture (alternative to the Info.plist key). |

Command line:

```sh
METAL_DEVICE_WRAPPER_TYPE=1 METAL_ERROR_MODE=5 ./your_sample
```

Xcode scheme: *Product > Scheme > Edit Scheme… > Run > Diagnostics*, tick
**Metal API Validation** (and **Shader Validation** on supported OSes).

`GTEInitOptions::debugLayer = Enabled` handles the common engine-logging
case automatically on every OS version. The env vars above are needed for
native API/shader validation, for targeting macOS < 11, and whenever the
caller constructs the `id<MTLDevice>` themselves (OmegaGTE never owns the
`MTLCreateSystemDefaultDevice` / device-creation call site in that path).

## Programmatic GPU capture

`OmegaGTE` can start a GPU frame capture at `Init()` and write a
`.gputrace` document you can open in Xcode — no Xcode attach required.

```cpp
OmegaGTE::GTEInitOptions opts;
opts.debugLayer    = OmegaGTE::GTEInitOptions::DebugLayer::Enabled;
opts.captureOnInit = true;                       // opt-in (default false)
opts.captureFilePath = "frame.gputrace";         // optional; see below
auto gte = OmegaGTE::InitWithDefaultDevice(opts);
// … render …
OmegaGTE::Close(gte);                             // capture is stopped here
```

- The capture spans `Init()` → `Close()` (it stops in the Metal engine
  destructor when the graphics-engine handle is released).
- `captureFilePath` is optional. When null/empty, OmegaGTE writes
  `omegagte-<pid>-<timestamp>.gputrace` to the working directory.
- `captureOnInit` is gated behind its **own** flag, not just `debugLayer`,
  because traces grow to multi-GB quickly. It is also ignored unless the
  debug layer resolves on.

### Required: enable capture in the embedding app

Programmatic capture outside Xcode must be explicitly permitted by the
**application** that links OmegaGTE — a library cannot grant it. Add to the
app's `Info.plist`:

```xml
<key>MetalCaptureEnabled</key>
<true/>
```

or set `MTL_CAPTURE_ENABLED=1` in the environment. If neither is set,
`MTLCaptureManager` reports the `GPUTraceDocument` destination as
unsupported; OmegaGTE logs that fact via `DEBUG_STREAM` and skips capture
(no crash, no trace file).

### Version floor

Programmatic capture uses `MTLCaptureDescriptor` /
`startCaptureWithDescriptor:error:`, available on **macOS 10.15+ /
iOS 13+**. On older targets OmegaGTE logs and skips.
