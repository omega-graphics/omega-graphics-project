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

    struct OMEGAGTE_EXPORT GECommandBufferCompletionInfo {
        enum class CompletionStatus : std::uint8_t {
            Completed,
            Error
        } status = CompletionStatus::Completed;
        // Backend-specific GPU timeline values in seconds when available.
        double gpuStartTimeSec = 0.0;
        double gpuEndTimeSec = 0.0;
    };

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
        using ColorAttachment = GERenderTarget::RenderPassDesc::ColorAttachment;
        using DepthStencilAttachment = GERenderTarget::RenderPassDesc::DepthStencilAttachment;
        /// Per-color-attachment load/clear state. Index 0 is the primary
        /// color attachment (falls back to the render target's native texture
        /// when no per-attachment texture is supplied). Indices > 0 require an
        /// explicit texture.
        OmegaCommon::Vector<ColorAttachment> colorAttachments;
        DepthStencilAttachment depthStencilAttachment;
        bool multisampleResolve = false;
        using MultisampleResolveDesc = GERenderTarget::RenderPassDesc::MultisampleResolveDesc;
        MultisampleResolveDesc resolveDesc;
    };

    /// @brief Describes a Compute Pass or Ray Tracing Pass.
    struct  OMEGAGTE_EXPORT GEComputePassDescriptor {};

    /// @brief Describes a Blit Pass
    struct  OMEGAGTE_EXPORT GEBlitPassDescriptor {
        
    };


    /**
     @brief A Reusable interface for directly uploading data and commands to a GTEDevice.
     */
    class  OMEGAGTE_EXPORT GECommandBuffer :
            public GTEResource {

        friend class GERenderTarget::CommandBuffer;
    protected:
        using RenderPassDrawPolygonType = GERenderTarget::CommandBuffer::PolygonType;
        using RenderPassIndexType = GERenderTarget::CommandBuffer::IndexType;
    private:
         /**
         Render Pass (For usage, please use the GERenderTarget::CommandBuffer instead.)
         */
        virtual void startRenderPass(const GERenderPassDescriptor & desc) = 0;
        virtual void setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState) = 0;
        //
        virtual void setVertexBuffer(SharedHandle<GEBuffer> & buffer) = 0;
        
        virtual void bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;
        virtual void bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned id,
                                                const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;

        virtual void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;
        virtual void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned id,
                                                  const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;

        virtual void setStencilRef(unsigned ref) = 0;
        
        virtual void setViewports(std::vector<GEViewport> viewport) = 0;
        virtual void setScissorRects(std::vector<GEScissorRect> scissorRects) = 0;
        
        virtual void drawPolygons(RenderPassDrawPolygonType polygonType,unsigned vertexCount,size_t startIdx) = 0;

        virtual void setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType) = 0;
        virtual void drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                         unsigned indexCount, size_t startIndex,
                                         int baseVertex) = 0;
        virtual void drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                           unsigned vertexCount, size_t startIdx,
                                           unsigned instanceCount, unsigned firstInstance) = 0;
        virtual void drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                   unsigned indexCount, size_t startIndex,
                                                   int baseVertex, unsigned instanceCount,
                                                   unsigned firstInstance) = 0;

        virtual void drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                           SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) = 0;
        virtual void drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                  SharedHandle<GEBuffer> & argumentBuffer,
                                                  size_t argumentBufferOffset) = 0;

        virtual void finishRenderPass() = 0;
        /**
         Compute Pass
        */
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GECommandBuffer")

        /**
         @brief Start a Blit Pass
        */
        virtual void startBlitPass() = 0;
        /**
          @brief Copy a GETexture from one texture to another.
          @param[in] src The Source Texture
          @param[in] dest The Destination Texture
         */
        virtual void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest) = 0;

        /**
         @brief Copy a region of a GETexture from one texture to another.
         @param[in] src The Source Texture
         @param[in] dest The Destination Texture
         @param[in] region The Region of the Source Texture to Copy
        */
        virtual void copyTextureToTexture(SharedHandle<GETexture> & src,SharedHandle<GETexture> & dest,const TextureRegion & region,const GPoint3D & destCoord) = 0;

        /**
         @brief Copy bytes between two buffers.
         @param[in] src The source buffer.
         @param[in] dest The destination buffer.
         @param[in] size Number of bytes to copy. When 0, the entire source buffer is copied.
         @param[in] srcOffset Byte offset into the source buffer.
         @param[in] destOffset Byte offset into the destination buffer.
         */
        virtual void copyBufferToBuffer(SharedHandle<GEBuffer> & src,
                                         SharedHandle<GEBuffer> & dest,
                                         size_t size = 0,
                                         size_t srcOffset = 0,
                                         size_t destOffset = 0) = 0;

        /**
         @brief Copy texel data from a buffer into a texture region.
         @param[in] src The source buffer containing tightly-arranged texel data.
         @param[in] dest The destination texture.
         @param[in] bytesPerRow Bytes per row of texel data in the source buffer.
         @param[in] bytesPerImage Bytes per image slice in the source buffer (0 for 2D textures).
         @param[in] destRegion Target region within the destination texture.
         @param[in] srcBufferOffset Byte offset into the source buffer.
         */
        virtual void copyBufferToTexture(SharedHandle<GEBuffer> & src,
                                          SharedHandle<GETexture> & dest,
                                          size_t bytesPerRow,
                                          size_t bytesPerImage,
                                          const TextureRegion & destRegion,
                                          size_t srcBufferOffset = 0) = 0;

        /**
         @brief Copy texel data from a texture region into a buffer (GPU readback).
         @param[in] src The source texture.
         @param[in] dest The destination buffer (should allow CPU readback).
         @param[in] bytesPerRow Bytes per row in the destination buffer.
         @param[in] bytesPerImage Bytes per image slice in the destination buffer (0 for 2D textures).
         @param[in] srcRegion Source region within the texture.
         @param[in] destBufferOffset Byte offset into the destination buffer.
         */
        virtual void copyTextureToBuffer(SharedHandle<GETexture> & src,
                                          SharedHandle<GEBuffer> & dest,
                                          size_t bytesPerRow,
                                          size_t bytesPerImage,
                                          const TextureRegion & srcRegion,
                                          size_t destBufferOffset = 0) = 0;

        /**
         @brief Generate mipmaps for a texture.
         @param[in,out] texture The texture to populate mip levels for. Must have been created
                                with `mipLevels > 1`. Mip 0 is treated as the source; mips 1..N are filled
                                using linear box filtering.
         @paragraph
         Metal uses the native `generateMipmapsForTexture:` command. Vulkan uses a chain of
         `vkCmdBlitImage` calls with `VK_FILTER_LINEAR`, transitioning each mip level as
         needed. D3D12 requires an internal shader chain (see Extension 3 Blit Pipeline);
         until that infrastructure lands this call is a no-op on D3D12 and emits a warning.
         */
        virtual void generateMipmaps(SharedHandle<GETexture> & texture) = 0;

        /**
         @brief Fill a buffer region with a repeating 32-bit value.
         @param[in,out] buffer The buffer to fill.
         @param[in] value The 32-bit pattern to fill with.
         @param[in] offset Byte offset into the buffer (must be 4-byte aligned).
         @param[in] size Bytes to fill (must be 4-byte aligned; 0 = fill to end of buffer).
         @paragraph
         Vulkan maps directly to `vkCmdFillBuffer`. D3D12 uses `ClearUnorderedAccessViewUint`
         on buffers with UAV flags; otherwise emits a warning. Metal's blit encoder only
         supports an 8-bit fill pattern natively, so when all four bytes of `value` are
         identical the backend uses `fillBuffer:range:value:`; otherwise it emits a warning
         and fills with the low byte (use a compute shader for non-uniform patterns).
         */
        virtual void fillBuffer(SharedHandle<GEBuffer> & buffer,
                                 uint32_t value,
                                 size_t offset = 0,
                                 size_t size = 0) = 0;

        /**
         @brief Finish a Blit Pass.
         */
        virtual void finishBlitPass() = 0;

        /**
         @brief Accleration Pass.
        */
        virtual void beginAccelStructPass() = 0;

        virtual void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,const GEAccelerationStructDescriptor & desc) = 0;
        
        virtual void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest) = 0;

        virtual void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest,const GEAccelerationStructDescriptor & desc) = 0;

        /**
         @brief Accleration Pass.
        */
        virtual void finishAccelStructPass() = 0;

        /// @brief Starts a Compute Pass.
        /// @param desc A GEComputePassDescriptor describing the Compute Pass.
        /// @paragraph This method must be called before dispatch commands can be encoded.
        virtual void startComputePass(const GEComputePassDescriptor & desc) = 0;

        /// @brief Sets a Compute Pipeline State in a Compute Pass.
        /// @param pipelineState A GEComputePipelineState
        /// @paragraph This method can be called after startComputePass() has been called and must be invoked before a dispatch command is encoded.
        virtual void setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState) = 0;

        /// @brief Binds a Buffer Resource to a Descriptor in the scope of the Compute Shader.
        /// @param buffer The Resource to bind.
        /// @param id The OmegaSL Binding id.
        virtual void bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;

        /// @brief Binds a Texture Resource to a Descriptor in the scope of the Compute Shader.
        /// @param texture The Resource to bind.
        /// @param id The OmegaSL Binding id.
        /// @param swizzle Optional channel remap applied at descriptor write time. Identity (the default) reuses the texture's primary view; non-identity routes through the per-texture swizzled-view cache.
        virtual void bindResourceAtComputeShader(SharedHandle<GETexture> & texture,unsigned id,
                                                 const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;


         /// @brief Binds an Acceleration Structure Resource to a Descriptor in the scope of the Compute Shader.
        /// @param texture The Resource to bind.
        /// @param id The OmegaSL Binding id.
        virtual void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned id) = 0;

        /// @brief Dispatches threadgroups in a Compute Pass.
        /// @param x The number of threadgroups in the `x` direction.
        /// @param y The number of threadgroups in the `y` direction.
        /// @param z The number of threadgroups in the `z` direction.
        virtual void dispatchThreadgroups(unsigned x,unsigned y,unsigned z) = 0;

        /// @brief Dispatches threads by total thread count in a Compute Pass.
        /// The backend divides by the pipeline's threadgroup size to derive threadgroup counts.
        /// @param x Total threads in the `x` direction.
        /// @param y Total threads in the `y` direction.
        /// @param z Total threads in the `z` direction.
        virtual void dispatchThreads(unsigned x,unsigned y,unsigned z) = 0;

        /// @brief Dispatches threadgroups using arguments stored in a GPU buffer.
        /// The buffer must contain a `GEDispatchIndirectCommand` (three `uint32_t`
        /// values: groupCountX, groupCountY, groupCountZ) at the given byte offset.
        /// @param argumentBuffer The buffer containing dispatch arguments.
        /// @param argumentBufferOffset Byte offset into the argument buffer.
        virtual void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                   size_t argumentBufferOffset) = 0;


         /// @brief Executes a Ray Tracing Pipeline (Encodes a dispatch command in the Compute Pass).
        /// @param x The Number of ThreadGroups dispatched in the `x` direction.
        /// @param y The Number of ThreadGroups dispatched in the `y` direction.
        /// @param z The Number of ThreadGroups dispatched in the `z` direction.
        virtual void dispatchRays(unsigned x,unsigned y,unsigned z) = 0;


        /// @brief Finish encoding a Compute Pass.
        /// @paragraph This method must be invoked when a dispatch command has been encoded.
        virtual void finishComputePass() = 0;

        /// @brief Reset and deallocate commands in this command buffer.
        /// @note This method MUST be called after it is committed before uploading more commands.
        virtual void reset() = 0;

        /// @brief Register a completion callback for this command buffer.
        /// @note Default implementation is a no-op; backends can override to provide GPU completion telemetry.
        virtual void setCompletionHandler(const GECommandBufferCompletionHandler & handler){
            (void)handler;
        }

        virtual ~GECommandBuffer() = default;
    };

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
