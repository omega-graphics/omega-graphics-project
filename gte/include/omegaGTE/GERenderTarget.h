#include "GTEBase.h"
#include "GEPipeline.h"
#include <functional>
#include <vector>
#include <cstdint>


#ifndef OMEGAGTE_GERENDERTARGET_H
#define OMEGAGTE_GERENDERTARGET_H

_NAMESPACE_BEGIN_
    class GEBuffer;
    class GETexture;
    class GESamplerState;
    class GEMesh;
    class GEFence;
    struct GEAccelerationStruct;
    struct GEAccelerationStructDescriptor;
    struct GEViewport;
    struct GEScissorRect;
    struct GERenderPassDescriptor;
    struct GEComputePassDescriptor;
    struct GEBlitPassDescriptor;

    struct OMEGAGTE_EXPORT GECommandBufferCompletionInfo {
        enum class CompletionStatus : std::uint8_t {
            Completed,
            Error
        } status = CompletionStatus::Completed;
        // Backend-specific GPU timeline values in seconds when available.
        double gpuStartTimeSec = 0.0;
        double gpuEndTimeSec = 0.0;
    };

    using GECommandBufferCompletionHandler =
            std::function<void(const GECommandBufferCompletionInfo &)>;

    /**
     @brief A Reusable interface for directly uploading data and commands to a GTEDevice.
     */
    class  OMEGAGTE_EXPORT GECommandBuffer :
            public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GECommandBuffer")

        /// @brief Defines the Primitive Topology.
        /// @note Line / LineStrip / Point require a compatible pipeline
        /// (see `RenderPipelineDescriptor::primitiveTopologyCategory`).
        /// Wide lines (line width > 1.0) are only supported on Vulkan with
        /// `GTEDeviceFeatures::wideLines`; D3D12 and Metal always use
        /// 1-pixel lines.
        using PolygonType = enum : uint8_t {
            Triangle,
            TriangleStrip,
            Line,
            LineStrip,
            Point
        };

        /// @brief Index element type for indexed draw calls.
        enum class IndexType : uint8_t {
            UInt16,
            UInt32
        };

        /**
         Render Pass
         */
        virtual void startRenderPass(const GERenderPassDescriptor & desc) = 0;
        virtual void setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState) = 0;
        //
        virtual void setVertexBuffer(SharedHandle<GEBuffer> & buffer) = 0;

        virtual void bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;
        virtual void bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned id,
                                                const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;
        /// @brief Bind a runtime sampler state to a non-static `sampler*d : N`
        /// slot in the vertex shader (Extension 8). @p id is the OmegaSL
        /// resource slot. Binding to a slot the shader declared `static` is a
        /// programmer error and asserts in debug builds.
        virtual void bindResourceAtVertexShader(SharedHandle<GESamplerState> & sampler,unsigned id) = 0;

        virtual void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;
        virtual void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned id,
                                                  const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;
        /// @brief Bind a runtime sampler state to a non-static `sampler*d : N`
        /// slot in the fragment shader (Extension 8).
        virtual void bindResourceAtFragmentShader(SharedHandle<GESamplerState> & sampler,unsigned id) = 0;

        /// @brief Set the push-constant data for the currently bound render
        /// pipeline (Pipeline-Completion §2.2 / OmegaSL §10.2 `constant<T>`).
        /// Small (≤128 bytes portable), updated per-draw without a buffer
        /// allocation: D3D12 root constants, Metal `setVertexBytes`/
        /// `setFragmentBytes`, Vulkan `vkCmdPushConstants`. There is no slot
        /// argument — a pipeline may bind at most one push-constant block, and
        /// the command applies it to every stage that declared it (`[in pc]`).
        /// The bytes must already match the shader's std layout (std430 for
        /// the GLSL/Vulkan `push_constant` block); this call does no packing.
        /// Must be called inside a render pass with a pipeline bound.
        /// @param data   Pointer to the constant bytes.
        /// @param size   Size in bytes (max 128 portable).
        /// @param offset Byte offset into the push-constant range (partial
        ///               update). Honored on D3D12/Vulkan; Metal supports only
        ///               offset == 0 (a full-block replace) and asserts otherwise.
        virtual void setRenderConstants(const void *data, unsigned size, unsigned offset = 0) = 0;

        virtual void setStencilRef(unsigned ref) = 0;

        virtual void setViewports(std::vector<GEViewport> viewport) = 0;
        virtual void setScissorRects(std::vector<GEScissorRect> scissorRects) = 0;

        virtual void drawPolygons(PolygonType polygonType,unsigned vertexCount,size_t startIdx) = 0;

        virtual void setIndexBuffer(SharedHandle<GEBuffer> & buffer, IndexType indexType) = 0;
        virtual void drawIndexedPolygons(PolygonType polygonType,
                                         unsigned indexCount, size_t startIndex,
                                         int baseVertex) = 0;
        virtual void drawPolygonsInstanced(PolygonType polygonType,
                                           unsigned vertexCount, size_t startIdx,
                                           unsigned instanceCount, unsigned firstInstance) = 0;
        virtual void drawIndexedPolygonsInstanced(PolygonType polygonType,
                                                   unsigned indexCount, size_t startIndex,
                                                   int baseVertex, unsigned instanceCount,
                                                   unsigned firstInstance) = 0;

        virtual void drawPolygonsIndirect(PolygonType polygonType,
                                           SharedHandle<GEBuffer> & argumentBuffer,
                                           size_t argumentBufferOffset) = 0;
        virtual void drawIndexedPolygonsIndirect(PolygonType polygonType,
                                                  SharedHandle<GEBuffer> & argumentBuffer,
                                                  size_t argumentBufferOffset) = 0;

        /**
         @brief Dispatch a mesh-shader pipeline (Mesh-Shader-Plan Phase 3).
         @paragraph Must be called inside a render pass with a
         mesh-shader render-pipeline (built via
         @c OmegaGraphicsEngine::makeMeshPipelineState) bound through
         the existing @c setRenderPipelineState — mesh PSOs surface as
         @c GERenderPipelineState on every backend. Dispatches
         @c (groupCountX, groupCountY, groupCountZ) mesh threadgroups;
         the per-meshlet workgroup dimensions come from the bound
         mesh shader's @c threadgroupDesc. Lowers to
         @c DispatchMesh (D3D12), @c drawMeshThreadgroups (Metal), and
         @c vkCmdDrawMeshTasksEXT (Vulkan).
         @paragraph Feature-gated behind @c GTEDEVICE_FEATURE_MESH_SHADER:
         on a device that lacks it the call is a no-op + diagnostic,
         matching the raytracing dispatch pattern. No @c #ifdefs in
         this header — the gate lives in each backend's implementation.
         */
        virtual void drawMeshTasks(uint32_t groupCountX,
                                   uint32_t groupCountY,
                                   uint32_t groupCountZ) = 0;

        virtual void finishRenderPass() = 0;

        /// @brief Bind a GEMesh's vertex buffer at the given resource
        /// register, optionally bind its index buffer, and bind every
        /// texture in `mesh->textureBindings` to the fragment shader at
        /// the slot key. Equivalent to a sequence of
        /// `bindResourceAtVertexShader` + `bindResourceAtFragmentShader`
        /// + `setIndexBuffer` calls.
        /// @param mesh The GEMesh to bind.
        /// @param vertexSlot OmegaSL resource register for the vertex
        ///                   buffer (matches the `: N` annotation on
        ///                   the `buffer<T>` declaration in the shader).
        void bindMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot = 0);

        /// @brief Bind and draw a GEMesh in one call. Topology and
        /// vertex/index counts come from the mesh; the call resolves to
        /// `drawPolygons` (non-indexed) or `drawIndexedPolygons`
        /// (indexed) based on `mesh->descriptor.indexType`.
        /// @param mesh The GEMesh to draw.
        /// @param vertexSlot OmegaSL resource register for the vertex
        ///                   buffer.
        void drawMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot = 0);

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
         */
        virtual void copyBufferToBuffer(SharedHandle<GEBuffer> & src,
                                         SharedHandle<GEBuffer> & dest,
                                         size_t size = 0,
                                         size_t srcOffset = 0,
                                         size_t destOffset = 0) = 0;

        /**
         @brief Copy texel data from a buffer into a texture region.
         */
        virtual void copyBufferToTexture(SharedHandle<GEBuffer> & src,
                                          SharedHandle<GETexture> & dest,
                                          size_t bytesPerRow,
                                          size_t bytesPerImage,
                                          const TextureRegion & destRegion,
                                          size_t srcBufferOffset = 0) = 0;

        /**
         @brief Copy texel data from a texture region into a buffer (GPU readback).
         */
        virtual void copyTextureToBuffer(SharedHandle<GETexture> & src,
                                          SharedHandle<GEBuffer> & dest,
                                          size_t bytesPerRow,
                                          size_t bytesPerImage,
                                          const TextureRegion & srcRegion,
                                          size_t destBufferOffset = 0) = 0;

        /**
         @brief Generate mipmaps for a texture.
         */
        virtual void generateMipmaps(SharedHandle<GETexture> & texture) = 0;

        /**
         @brief Execute a programmable blit using a BlitPipelineState (Extension 3).
         @paragraph Internally opens a transient render pass on @p dest, binds
         the pipeline and @p src (at fragment-shader resource slot 0), and
         issues a 3-vertex draw covering the entire destination extent. Must
         NOT be called inside an existing @c startRenderPass /
         @c startBlitPass / @c startComputePass scope; the blit owns its own
         pass for the duration of the call.
         @param[in] pipelineState The blit pipeline to use.
         @param[in] src           Source texture (sampled by the fragment shader).
         @param[in] dest          Destination texture (color attachment 0).
         */
        virtual void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                                      SharedHandle<GETexture> & src,
                                      SharedHandle<GETexture> & dest) = 0;

        /**
         @brief Execute a programmable blit to a subregion of @p dest, reading
         from @p srcRegion of @p src (Extension 3).
         @paragraph Same scope rules as the full-extent overload. The viewport
         and scissor are derived from @p destRegion; the source region is
         currently advisory (sampled UVs cover the whole source texture) — a
         future revision may bake @p srcRegion into per-blit constants.
         @param[in] pipelineState The blit pipeline to use.
         @param[in] src           Source texture.
         @param[in] dest          Destination texture.
         @param[in] srcRegion     Region of the source to sample from.
         @param[in] destRegion    Region of the destination to write to.
         */
        virtual void blitWithPipeline(SharedHandle<GEBlitPipelineState> & pipelineState,
                                      SharedHandle<GETexture> & src,
                                      SharedHandle<GETexture> & dest,
                                      const TextureRegion & srcRegion,
                                      const TextureRegion & destRegion) = 0;

        /**
         @brief Fill a buffer region with a repeating 32-bit value.
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
         @brief Acceleration Pass.
        */
        virtual void beginAccelStructPass() = 0;

        virtual void buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,const GEAccelerationStructDescriptor & desc) = 0;

        virtual void copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest) = 0;

        virtual void refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,SharedHandle<GEAccelerationStruct> &dest,const GEAccelerationStructDescriptor & desc) = 0;

        /**
         @brief Acceleration Pass.
        */
        virtual void finishAccelStructPass() = 0;

        /// @brief Starts a Compute Pass.
        virtual void startComputePass(const GEComputePassDescriptor & desc) = 0;

        /// @brief Sets a Compute Pipeline State in a Compute Pass.
        virtual void setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState) = 0;

        /// @brief Binds a Buffer Resource to a Descriptor in the scope of the Compute Shader.
        virtual void bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer,unsigned id) = 0;

        /// @brief Binds a Texture Resource to a Descriptor in the scope of the Compute Shader.
        virtual void bindResourceAtComputeShader(SharedHandle<GETexture> & texture,unsigned id,
                                                 const TextureSwizzle & swizzle = TextureSwizzle::identity()) = 0;

        /// @brief Bind a runtime sampler state to a non-static `sampler*d : N`
        /// slot in the scope of the Compute Shader (Extension 8).
        virtual void bindResourceAtComputeShader(SharedHandle<GESamplerState> & sampler,unsigned id) = 0;


         /// @brief Binds an Acceleration Structure Resource to a Descriptor in the scope of the Compute Shader.
        virtual void bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned id) = 0;

        /// @brief Set the push-constant data for the currently bound compute
        /// pipeline (Pipeline-Completion §2.2 / OmegaSL §10.2 `constant<T>`).
        /// Compute-pass counterpart of @c setRenderConstants — same one-block,
        /// no-slot, no-packing contract; D3D12 `SetComputeRoot32BitConstants`,
        /// Metal `setBytes`, Vulkan `vkCmdPushConstants`. Must be called inside
        /// a compute pass with a pipeline bound.
        /// @param data   Pointer to the constant bytes.
        /// @param size   Size in bytes (max 128 portable).
        /// @param offset Byte offset into the push-constant range; Metal
        ///               supports only offset == 0.
        virtual void setComputeConstants(const void *data, unsigned size, unsigned offset = 0) = 0;

        /// @brief Dispatches threadgroups in a Compute Pass.
        virtual void dispatchThreadgroups(unsigned x,unsigned y,unsigned z) = 0;

        /// @brief Dispatches threads by total thread count in a Compute Pass.
        virtual void dispatchThreads(unsigned x,unsigned y,unsigned z) = 0;

        /// @brief Dispatches threadgroups using arguments stored in a GPU buffer.
        virtual void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                   size_t argumentBufferOffset) = 0;


         /// @brief Executes a Ray Tracing Pipeline (Encodes a dispatch command in the Compute Pass).
        virtual void dispatchRays(unsigned x,unsigned y,unsigned z) = 0;


        /// @brief Finish encoding a Compute Pass.
        virtual void finishComputePass() = 0;

        /// @brief Reset and deallocate commands in this command buffer.
        virtual void reset() = 0;

        /// @brief Register a completion callback for this command buffer.
        virtual void setCompletionHandler(const GECommandBufferCompletionHandler & handler){
            (void)handler;
        }

        virtual ~GECommandBuffer() = default;
    };
    class OMEGAGTE_EXPORT GERenderTarget { 
        public: 
        virtual ~GERenderTarget() = default; 
    };
    class  OMEGAGTE_EXPORT GENativeRenderTarget : public GERenderTarget {
        public:
         OMEGACOMMON_CLASS("OmegaGTE.GENativeRenderTarget")
        virtual PixelFormat pixelFormat() { return PixelFormat::BGRA8Unorm; }

        /// Queue the swap chain was created against. Required for `present()`.
        /// Non-owning reference held internally; the caller keeps the queue alive.
        /// On D3D12/Vulkan the swap chain is bound to this queue at creation
        /// time; on Metal it is recorded so the engine can encode the
        /// `presentDrawable:` call.
        virtual SharedHandle<GECommandQueue> presentQueue() const = 0;

        /// Submit the engine's internal "transition to PRESENT + Present" work
        /// on the present queue. Replaces the old `commitAndPresent()`. The
        /// caller is responsible for having submitted draw work to the present
        /// queue beforehand.
        virtual void present() = 0;

        /// Wait for GPU to finish, resize swap chain, and recreate RTVs.
        /// Cross-platform: on D3D12, call instead of IDXGISwapChain::ResizeBuffers;
        /// on Vulkan, recreates the VkSwapchainKHR + image views at the new
        /// extent (the native window resize otherwise leaves the swapchain
        /// permanently OUT_OF_DATE and vkAcquireNextImageKHR fails every
        /// frame). Default no-op for backends with platform-managed
        /// drawables (e.g. Metal `CAMetalLayer` auto-resizes).
        virtual void resizeSwapChain(unsigned int width, unsigned int height) { (void)width; (void)height; }

        #ifdef _WIN32
        /// @returns IDXGISwapChain1 * if D3D11, else IDXGISwapChain3 *
        virtual void *getSwapChain() = 0;

        //TODO: Remove these methods as GECommandQueue should have these methods.
        /// Wait for the present queue to finish. Use to serialize cross-context texture pool use.
        virtual void waitForGPU() {}
        /// CPU wait until the given fence has been signaled (e.g. before using a texture produced on another queue).
        virtual void waitForFence(SharedHandle<GEFence> & fence) { (void)fence; }
        #endif
        virtual ~GENativeRenderTarget() = default;
     };
     class  OMEGAGTE_EXPORT GETextureRenderTarget : public GERenderTarget {
         public:
         OMEGACOMMON_CLASS("OmegaGTE.GETextureRenderTarget")
         virtual SharedHandle<GETexture> underlyingTexture() = 0;
        virtual ~GETextureRenderTarget() = default;
     };

_NAMESPACE_END_

#endif
