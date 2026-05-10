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

    class  OMEGAGTE_EXPORT GECommandQueue : public GTEResource {
        unsigned size;
    protected:
        unsigned currentlyOccupied = 0;
        explicit GECommandQueue(unsigned size);
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GECommandQueue")

        /// @brief Gets the next usable command buffer allocated for this command queue.
        /// @note This method returns the first command buffer that has not been yet used by the user.
        /// @returns A SharedHandle<GECommandBuffer>.
        virtual SharedHandle<GECommandBuffer> getAvailableBuffer() = 0;
        unsigned getSize();

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

        /// @brief Signal a fence on this queue (e.g. after waitForGPU) for cross-queue sync when no command buffer is being submitted.
        virtual void signalFence(SharedHandle<GEFence> & fence) { (void)fence; }

        /// @brief CPU wait until the fence reaches or exceeds the given value (e.g. before using a texture from another queue).
        virtual void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value) { (void)fence; (void)value; }

        virtual ~GECommandQueue() = default;
    };
_NAMESPACE_END_

#endif
