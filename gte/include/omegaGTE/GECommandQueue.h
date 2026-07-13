#include "GTEBase.h"
#include "GEPipeline.h"
#include "GERenderTarget.h"
#include "GETexture.h"
#include <vector>
#include <cstdint>

#ifndef OMEGAGTE_GECOMMANDQUEUE_H
#define OMEGAGTE_GECOMMANDQUEUE_H

_NAMESPACE_BEGIN_
    class GEBuffer;
    class GEFence;

    struct GEScissorRect;
    struct GEViewport;

    /// @brief Argument layout for non-indexed indirect draw calls.
    /// Matches D3D12_DRAW_ARGUMENTS / VkDrawIndirectCommand / Metal
    /// MTLDrawPrimitivesIndirectArguments.
    struct OMEGAGTE_EXPORT GEDrawIndirectCommand {
        std::uint32_t vertexCount;
        std::uint32_t instanceCount;
        std::uint32_t firstVertex;
        std::uint32_t firstInstance;
    };

    /// @brief Argument layout for indexed indirect draw calls.
    /// Matches D3D12_DRAW_INDEXED_ARGUMENTS / VkDrawIndexedIndirectCommand /
    /// Metal MTLDrawIndexedPrimitivesIndirectArguments.
    struct OMEGAGTE_EXPORT GEDrawIndexedIndirectCommand {
        std::uint32_t indexCount;
        std::uint32_t instanceCount;
        std::uint32_t firstIndex;
        std::int32_t  baseVertex;
        std::uint32_t firstInstance;
    };

    /// @brief Argument layout for indirect compute dispatches.
    /// Matches D3D12_DISPATCH_ARGUMENTS / VkDispatchIndirectCommand / Metal
    /// MTLDispatchThreadgroupsIndirectArguments.
    struct OMEGAGTE_EXPORT GEDispatchIndirectCommand {
        std::uint32_t groupCountX;
        std::uint32_t groupCountY;
        std::uint32_t groupCountZ;
    };

    /// @brief Describes a Render Pass
    struct  OMEGAGTE_EXPORT GERenderPassDescriptor {
        GENativeRenderTarget *nRenderTarget = nullptr;
        GETextureRenderTarget *tRenderTarget = nullptr;
        
        struct OMEGAGTE_EXPORT ColorAttachment {
                using LoadAction = enum {
                    Load,
                    LoadPreserve,
                    Clear,
                    Discard
                };
                LoadAction loadAction;
                struct OMEGAGTE_EXPORT ClearColor {
                    float r,g,b,a;
                    ClearColor(float r,float g,float b,float a);
                };
                ClearColor clearColor;
                /// Optional per-attachment render-to-texture target.
                /// If null, attachment 0 falls back to the render pass's
                /// `nRenderTarget` / `tRenderTarget`. Attachments with index
                /// > 0 must supply a texture.
                SharedHandle<GETexture> texture = nullptr;
                ColorAttachment(ClearColor clearColor,LoadAction loadAction);
                ColorAttachment(ClearColor clearColor,LoadAction loadAction,SharedHandle<GETexture> texture);
            };
            struct OMEGAGTE_EXPORT DepthStencilAttachment {
                bool disabled = true;
                using LoadAction = enum {
                    Load,
                    LoadPreserve,
                    Clear,
                    Discard
                };
                LoadAction depthloadAction = Discard;
                LoadAction stencilLoadAction = Discard;
                float clearDepth = 1.F;
                unsigned clearStencil = 0;
            };
        /// Per-color-attachment load/clear state. Index 0 is the primary
        /// color attachment (falls back to the render target's native texture
        /// when no per-attachment texture is supplied). Indices > 0 require an
        /// explicit texture.
        OmegaCommon::Vector<ColorAttachment> colorAttachments;
        DepthStencilAttachment depthStencilAttachment;
        bool multisampleResolve = false;
        struct OMEGAGTE_EXPORT MultisampleResolveDesc {
                SharedHandle<GETexture> multiSampleTextureSrc = nullptr;
                unsigned level,slice,depth;
        };
        MultisampleResolveDesc resolveDesc;
    };

    /// @brief Describes a Compute Pass or Ray Tracing Pass.
    struct  OMEGAGTE_EXPORT GEComputePassDescriptor {
        
    };

    /// @brief Describes a Blit Pass
    struct  OMEGAGTE_EXPORT GEBlitPassDescriptor {

    };

    /// @brief Backwards-compatible aliases for the polygon-type and index-type
    /// enums on `GECommandBuffer`. Existing backend implementations spell their
    /// override signatures with these names; new code should prefer
    /// `GECommandBuffer::PolygonType` / `GECommandBuffer::IndexType` directly.
    using RenderPassDrawPolygonType = GECommandBuffer::PolygonType;
    using RenderPassIndexType = GECommandBuffer::IndexType;

    /// @brief Describes how a `GECommandQueue` should be created.
    /// @paragraph Replaces the single `unsigned maxBufferCount` parameter on
    /// `OmegaGraphicsEngine::makeCommandQueue`. Carries the queue's logical
    /// type (graphics / compute / transfer), execution priority, initial
    /// command-buffer-pool size, and a debug label. The fields exist on every
    /// backend; whether the backend can honor them depends on the platform
    /// (see the CommandQueue-Typed-Pool plan for the per-backend mapping). In
    /// Phase 1 this descriptor is plumbed end-to-end but only
    /// `maxBufferCount` affects behavior — `type`, `priority`,
    /// `requireDedicated`, and `label` are recorded for the Phase 2 backend
    /// work that will start honoring them.
    struct OMEGAGTE_EXPORT GECommandQueueDesc {
        /// @brief Logical class of work this queue accepts.
        /// @paragraph On Vulkan this picks a queue family; on D3D12 this picks
        /// a `D3D12_COMMAND_LIST_TYPE`; on Metal it is a hint recorded for
        /// introspection / debug-label only (Metal has no native split).
        enum class Type : std::uint8_t {
            /// Render + compute + copy. The default; corresponds to D3D12
            /// DIRECT, Vulkan graphics-capable family, MTLCommandQueue.
            Universal,
            /// Render + compute. No copy-queue separation requested.
            Graphics,
            /// Async compute. D3D12 COMPUTE, Vulkan compute-capable family
            /// (prefers a family without graphics), Metal MTLCommandQueue
            /// (no native split — recorded as a hint).
            Compute,
            /// DMA / transfer. D3D12 COPY, Vulkan transfer-capable family
            /// (prefers a dedicated transfer family), Metal MTLCommandQueue
            /// (hint only).
            Transfer,
        };

        /// @brief Global execution priority of this queue.
        /// @paragraph Maps to `D3D12_COMMAND_QUEUE_PRIORITY` on D3D12 and to
        /// `VK_KHR_global_priority` on Vulkan when the extension is present
        /// (otherwise a `float` priority on the VkDeviceQueueCreateInfo).
        /// Metal does not expose queue priority on its public API — the value
        /// is recorded for introspection only.
        enum class Priority : std::uint8_t {
            Low,       ///< D3D12 NORMAL; Vulkan `VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR`.
            Normal,    ///< D3D12 NORMAL; Vulkan `VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR`.
            High,      ///< D3D12 HIGH;   Vulkan `VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR`.
            Realtime,  ///< D3D12 GLOBAL_REALTIME (entitlement required); Vulkan `VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR` (gated).
        };

        /// @brief Logical queue type. Defaults to `Universal` so the default
        /// descriptor matches today's "give me any queue" behavior.
        Type type = Type::Universal;

        /// @brief Execution priority. Defaults to `Normal`.
        Priority priority = Priority::Normal;

        /// @brief Initial number of in-flight command buffers this queue
        /// tracks. Same semantics as today's `maxBufferCount` argument; in
        /// Phase 3 this becomes a hint for the initial allocation rather than
        /// a hard cap on the pool.
        unsigned maxBufferCount = 16;

        /// @brief If true, the engine refuses to fall back to a less-specific
        /// queue family when the requested `type` has no dedicated family on
        /// the device, and returns `nullptr` instead. Defaults to false
        /// (best-effort with fallback). Has no effect in Phase 1.
        bool requireDedicated = false;

        /// @brief Optional debug label. Plumbed to `ID3D12Object::SetName`,
        /// `VK_EXT_debug_utils`, and `[MTLCommandQueue setLabel:]` in Phase 2.
        /// Recorded but unused in Phase 1.
        OmegaCommon::String label;
    };

    class  OMEGAGTE_EXPORT GECommandQueue : public GTEResource {
        unsigned size;
    protected:
        unsigned currentlyOccupied = 0;
        /// Resolved descriptor after backend fallback. `desc_.type` is the
        /// type the queue actually runs as on this backend (post Vulkan
        /// family-fallback / D3D12 type aliasing). `requestedType_` keeps the
        /// type the user originally asked for so `isDedicated()` can tell
        /// when a request was downgraded.
        GECommandQueueDesc desc_;
        GECommandQueueDesc::Type requestedType_ = GECommandQueueDesc::Type::Universal;

        /// `desc` is the user's requested descriptor and `achievedType`
        /// is the type the backend actually allocated (may differ from
        /// `desc.type` when the backend fell back to a more general
        /// family / list type). This is the only ctor after the
        /// Phase 4 legacy-overload retirement.
        GECommandQueue(const GECommandQueueDesc & desc,
                       GECommandQueueDesc::Type achievedType);
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GECommandQueue")

        /// @brief Gets the next usable command buffer allocated for this command queue.
        /// @note This method returns the first command buffer that has not been yet used by the user.
        /// @returns A SharedHandle<GECommandBuffer>.
        virtual SharedHandle<GECommandBuffer> getAvailableBuffer() = 0;
        unsigned getSize();

        /// @brief Logical type this queue actually runs as on this backend
        /// (post-fallback). Use @ref isDedicated to tell whether this matches
        /// the originally-requested type.
        OMEGA_NODISCARD GECommandQueueDesc::Type type() const { return desc_.type; }

        /// @brief The user's originally-requested type, before any backend
        /// fallback. Equal to @ref type when @ref isDedicated returns true.
        OMEGA_NODISCARD GECommandQueueDesc::Type requestedType() const { return requestedType_; }

        /// @brief Priority the queue was created with. Recorded for
        /// introspection; whether the backend actually honored it is
        /// platform-dependent (D3D12 honors NORMAL/HIGH/GLOBAL_REALTIME with
        /// silent downgrade if the realtime entitlement is missing; Vulkan
        /// honors NORMAL by default and the rest only when
        /// `VK_KHR_global_priority` is present and opted into; Metal records
        /// only).
        OMEGA_NODISCARD GECommandQueueDesc::Priority priority() const { return desc_.priority; }

        /// @brief Debug label the queue was created with. May be empty.
        OMEGA_NODISCARD const OmegaCommon::String & label() const { return desc_.label; }

        /// @brief Returns true iff `type()` matches what the caller originally
        /// asked for (i.e., the backend did not have to fall back to a more
        /// general queue family / list type).
        OMEGA_NODISCARD bool isDedicated() const { return desc_.type == requestedType_; }

        /// @brief Encodes a wait on command buffer using fence.
        /// @param commandBuffer The GECommandBuffer to encode the wait on.
        /// @param waitFence The GEFence to wait for.
        /// @paragraph Allows sync between command buffers in different queues.
        virtual void notifyCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer,SharedHandle<GEFence> & waitFence) = 0;

        /// @brief Submits command buffer to the queue.
        /// @param commandBuffer The GECommandBuffer to submit
        /// @paragraph Does not sync between commandBuffers
        virtual void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer) = 0;

        /// @brief Submits command buffer to the queue and encodes a signal event on completion.
        /// @param commandBuffer The GECommandBuffer to submit
        /// @param signalFence The GEFence to signal.
        /// @paragraph Allows sync between command buffers in different queues.
        virtual void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer,SharedHandle<GEFence> & signalFence) = 0;

        /// @brief Schedules all enqueued command buffers to GTEDevice for sequential execution.
        virtual void commitToGPU() = 0;

        /// @brief Schedules all enqueued command buffers to GTEDevice for sequential execution and waits for all command buffers to be completed.
        virtual void commitToGPUAndWait() = 0;

        /// @brief Commits the enqueued batch and reports its GPU execution time
        /// asynchronously. @p onComplete fires exactly once, after the whole
        /// committed batch finishes on the GPU, with the batch's GPU span (see
        /// @ref GECommitCompletionInfo). Does not block.
        /// @paragraph The aggregate rides the existing per-buffer completion
        /// path, so it composes with any per-buffer handler already set on the
        /// batch. On Metal the GPU times are real immediately; on backends
        /// whose per-buffer timing is not wired yet the status and buffer count
        /// are correct but @ref GECommitCompletionInfo::gpuDurationSec is `0.0`
        /// (see the GPU-Commit-Timing plan). Backends override this; the base
        /// implementation is a safe blocking fallback that reports zero timing.
        virtual void commitToGPU(const GECommitCompletionHandler & onComplete);

        /// @brief Commits the enqueued batch, blocks until it completes on the
        /// GPU, and returns its GPU execution time. Synchronous counterpart of
        /// the @ref commitToGPU(const GECommitCompletionHandler&) overload.
        virtual GECommitCompletionInfo commitToGPUAndWaitTimed();
    protected:
        /// @brief Backend-neutral helper for the
        /// @ref commitToGPU(const GECommitCompletionHandler&) overload. Wraps
        /// each buffer in @p batch with an internal per-buffer completion
        /// handler (composing with any existing one) that accumulates the
        /// per-buffer GPU spans, and invokes @p onComplete once after the last
        /// buffer in the batch reports. A backend calls this with its own batch
        /// immediately before committing. Thread-safe: the per-buffer callbacks
        /// fire on backend-internal threads.
        void installCommitAggregator(const std::vector<SharedHandle<GECommandBuffer>> & batch,
                                     const GECommitCompletionHandler & onComplete);
    public:

        /// @brief Signal a fence on this queue (e.g. after waitForGPU) for cross-queue sync when no command buffer is being submitted.
        virtual void signalFence(SharedHandle<GEFence> & fence) { (void)fence; }

        /// @brief CPU wait until the fence reaches or exceeds the given value (e.g. before using a texture from another queue).
        virtual void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value) { (void)fence; (void)value; }

        virtual ~GECommandQueue() = default;
    };
_NAMESPACE_END_

#endif
