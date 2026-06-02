# Multithreaded Compositor & GPU Rendering Models

## Research Document: Metal, DirectX 12, Vulkan, and Chromium Compositor Architecture

---

## 1. Metal Threading Model

### Command Buffer Creation & Submission
- **`MTLCommandBuffer`** can be created and committed from **any thread**.
- **`MTLCommandQueue`** is thread-safe — multiple threads can request command buffers from the same queue concurrently.
- **`MTLRenderCommandEncoder`** encoding happens on the thread that creates it, but the actual GPU work is submitted when the command buffer is committed.

### CAMetalLayer & Drawable Presentation
- **`CAMetalLayer.nextDrawable`** can be called from any thread, but Apple recommends calling it as late as possible to minimize stalling.
- **`MTLCommandBuffer.presentDrawable(_:)`** schedules presentation, but the actual present happens when the command buffer completes on the GPU.
- **`CAMetalLayer`** is not inherently thread-safe for configuration changes (e.g., changing `drawableSize`, `pixelFormat`). Configuration should happen on the main thread or be externally synchronized.

### Render Pipeline State
- **`MTLRenderPipelineState`** is immutable once created and is **thread-safe** — it can be shared across threads and command encoders.
- **Pipeline state creation** (`makeRenderPipelineState`) can happen on any thread but may be expensive. Best practice: create pipeline states upfront or on a background thread.

### Key Threading Rules
- The **main thread** does **not** need to be the rendering thread in Metal.
- `MTLSharedEvent` and `MTLEvent` provide cross-queue (and cross-process) synchronization primitives.
- Triple-buffering with semaphores (typically `DispatchSemaphore` with value 3) is the standard pattern to avoid CPU-GPU stalls.

### CAMetalLayer with CATransaction
- When using `CAMetalLayer` in a compositor, `CATransaction` commits on the **main thread** govern when layer tree changes become visible.
- For off-screen rendering (compositor back-buffer), `CAMetalLayer` is not required — render to `MTLTexture` directly.

---

## 2. DirectX 12 Threading Model

### Command List Recording
- **`ID3D12CommandList`** recording can happen on **any thread**. This is one of D3D12's primary design advantages over D3D11.
- Multiple command lists can be recorded **in parallel** on different threads.
- However, a single command list is **not thread-safe** — each thread should have its own command list (and typically its own command allocator).

### Command Queue Submission
- **`ID3D12CommandQueue::ExecuteCommandLists`** is **thread-safe** — multiple threads can submit to the same queue.
- Submission order to the queue determines GPU execution order.
- The queue itself is a serialization point for GPU work.

### Swap Chain & Presentation
- **`IDXGISwapChain::Present`** must be called from the thread that created the swap chain, or more precisely, the thread associated with the HWND (typically the main/UI thread).
- Back-buffer acquisition (`GetBuffer`) and rendering can happen on any thread, but the final `Present` call has thread affinity constraints.

### Pipeline State
- **`ID3D12PipelineState`** objects are immutable once created and are **thread-safe** to share across threads.
- Pipeline state creation (`CreateGraphicsPipelineState`) is thread-safe on the device, but can be expensive — pre-create or use pipeline state caches/libraries.

### Fence Synchronization
- **`ID3D12Fence`** is the primary synchronization primitive.
- `Signal` and `Wait` on fences coordinate CPU-GPU and GPU-GPU ordering.
- For compositor patterns: use fences to know when a back-buffer is available for re-use (multi-buffering).

### Resource Barriers
- Resource state transitions (e.g., render target → present) must be recorded into command lists.
- **No implicit synchronization** — the application is fully responsible for resource state tracking.

---

## 3. Vulkan Threading Model

### Command Buffer Recording
- **`vkBeginCommandBuffer` / `vkEndCommandBuffer`**: Command buffers can be recorded on **any thread**.
- Multiple command buffers from different **`VkCommandPool`** instances can be recorded in parallel.
- **Critical rule**: A `VkCommandPool` is **not thread-safe** — each recording thread must have its own command pool (or external synchronization must be applied).

### Queue Submission
- **`vkQueueSubmit`** is **not inherently thread-safe** for the same queue — access to a `VkQueue` must be externally synchronized.
- Multiple queues (if available) can be submitted to in parallel from different threads.
- Queue families (graphics, compute, transfer) enable parallel GPU work across different queue types.

### Swap Chain & Presentation
- **`vkQueuePresentKHR`** must be called on a thread that has access to the presentation queue, and the queue must be externally synchronized.
- **`vkAcquireNextImageKHR`** can be called from any thread.
- The "present" queue need not be the same as the "graphics" queue, enabling architectural separation.

### Pipeline Creation
- **`VkPipeline`** objects are immutable once created and are **thread-safe** to use across threads.
- **`VkPipelineCache`** can be shared across threads for pipeline creation, and `vkCreateGraphicsPipelines` is thread-safe when using different pipeline caches or when the same cache is externally synchronized.

### Synchronization Primitives
- **`VkSemaphore`**: GPU-GPU synchronization (e.g., between acquire and present).
- **`VkFence`**: CPU-GPU synchronization (e.g., knowing when a frame's command buffer has completed).
- **`VkEvent`**: Fine-grained GPU synchronization within or across command buffers.
- **`VkBarrier`** (memory, buffer, image barriers): Explicit resource state transitions within command buffers.

### Key Differences from Metal/D3D12
- Vulkan requires **explicit synchronization everywhere** — there are virtually no implicit guarantees.
- Command pool per-thread design is mandatory, not optional.
- Queue access must be externally synchronized (unlike D3D12 where queue submission is thread-safe).

---

## 4. Chromium's Compositor Architecture

### Main Thread vs Compositor Thread
Chromium separates rendering into two primary threads:
- **Main thread**: Runs JavaScript, layout, paint (producing a **layer tree**). Also called the "Blink" or "renderer" thread.
- **Compositor thread (impl thread)**: Takes the layer tree snapshot, manages tiles, issues GPU draw calls, and handles presentation. Also called the "cc" (Chromium Compositor) thread.

### The "Commit" Mechanism
- The main thread builds/updates a **pending layer tree**.
- A **"commit"** operation transfers (synchronizes) the pending tree to the compositor thread's **active tree**.
- During commit, the main thread is **blocked** (this is the synchronization point). The commit must be fast.
- After commit, the main thread is free to start the next frame, while the compositor thread draws the committed tree.

### Frame Scheduling & Deadlines
- The compositor runs on a **`BeginFrame` / deadline** model.
- A `BeginFrame` signal (from the display, e.g., VSync) triggers the pipeline.
- The compositor thread has a deadline by which it must produce a frame.
- If the main thread hasn't committed in time, the compositor can **draw the previous frame** (or an interpolated version) — this is how Chromium achieves smooth scrolling even when JavaScript is busy.

### Avoiding the Deadlock Pattern
- Chromium avoids the classic deadlock where the main thread and compositor thread wait on each other by using a **single-direction commit flow**:
  1. Main thread produces → commits to compositor.
  2. Compositor consumes → draws to screen.
  3. Main thread never waits on compositor (except during the commit synchronization point).
- The compositor never calls back into the main thread for drawing.

### GPU Thread (Viz / Display Compositor)
- In modern Chromium ("Viz" architecture), there's a third participant: the **GPU process / display compositor**.
- The compositor thread in each renderer process produces **compositor frames** (not raw GPU commands).
- These frames are sent to the **Viz display compositor** (in the GPU process), which does the final compositing of all browser surfaces (tabs, UI, popups) and issues the actual GPU draw calls.
- This architecture means renderer processes never directly access the GPU — they describe what to draw, and Viz executes it.

### Layer Tree Isolation
- Each renderer process has its own layer tree — completely isolated from others.
- The display compositor merges all renderer outputs plus browser UI into the final screen frame.
- This isolation is a security boundary (compromised renderer can't draw over other content) and a stability boundary (renderer crash doesn't affect display compositor).

---

## 5. Recommendations for OmegaWTK

Based on the above research, the recommended multithreaded compositor architecture for OmegaWTK should follow these principles:

### Thread Architecture
1. **Main/UI thread**: Handles user input, widget layout, and layer tree updates. Produces a "pending" layer tree.
2. **Compositor thread**: Consumes the committed layer tree, manages GPU resources, records command buffers, and submits to the GPU.
3. **Optional GPU thread**: If supporting out-of-process compositing (like Chromium's Viz), a separate GPU thread handles final display composition.

### Commit Model (Chromium-inspired)
- Use a **snapshot/commit model** where the main thread periodically commits a layer tree snapshot to the compositor thread.
- The commit should be a **fast, blocking synchronization point** — copy/swap the tree, don't deep-clone.
- After commit, the main thread is free immediately; the compositor works asynchronously.

### Per-API Threading Strategy

| Concern | Metal | DirectX 12 | Vulkan |
|---|---|---|---|
| Command buffer recording thread | Any | Any | Any (pool-per-thread) |
| Queue submission thread safety | Thread-safe | Thread-safe | Requires external sync |
| Presentation thread requirement | Any (but CATransaction on main) | HWND-associated thread | External sync on present queue |
| Pipeline state thread safety | Thread-safe (immutable) | Thread-safe (immutable) | Thread-safe (immutable) |
| Command pool/allocator sharing | Shared via queue | One allocator per thread recommended | **One pool per thread mandatory** |
| Synchronization primitive | MTLSharedEvent, DispatchSemaphore | ID3D12Fence | VkSemaphore, VkFence |

### Avoiding the dispatch_sync Deadlock
- **Never** have the compositor thread synchronously call back to the main thread during frame production.
- **Never** have the main thread synchronously wait on the compositor thread (except during the brief commit window).
- Use a **single-directional data flow**: Main → (commit) → Compositor → (submit) → GPU → (present) → Display.
- For cross-thread signaling, use **non-blocking notifications** (e.g., posting to a run loop, signaling a condition variable) rather than synchronous dispatch.

### Multi-Buffering Strategy
- Use **triple buffering** (3 in-flight frames) to decouple CPU and GPU timing:
  - Frame N: GPU executing
  - Frame N-1: Compositor recording commands
  - Frame N-2: Main thread building next layer tree
- Track frame completion with **fences** (D3D12/Vulkan) or **command buffer completion handlers** (Metal).

### Resource Management
- **Immutable resources** (pipeline states, samplers, static buffers) should be created on a background thread during initialization and shared freely.
- **Per-frame resources** (dynamic uniform buffers, descriptor sets) should use a ring-buffer pattern aligned with the triple-buffering frame count.
- **Resource state transitions** (D3D12 barriers, Vulkan barriers) must be recorded in command buffers on the compositor thread — they cannot be deferred or implicit.

---

## References

- [Apple Metal Best Practices: Triple Buffering](https://developer.apple.com/documentation/metal/resource_synchronization)
- [Microsoft D3D12 Multithreading Documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/multi-engine)
- [Vulkan Specification: Threading Behavior](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/chap4.html#fundamentals-threadingbehavior)
- [Chromium Compositor Architecture (cc/)](https://chromium.googlesource.com/chromium/src/+/main/docs/how_cc_works.md)
- [Chromium Viz Architecture](https://chromium.googlesource.com/chromium/src/+/main/components/viz/README.md)
- [Life of a Pixel (Chromium)](https://chromium.googlesource.com/chromium/src/+/main/docs/life_of_a_pixel.md)
