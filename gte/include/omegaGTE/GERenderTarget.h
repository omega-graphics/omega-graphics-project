#include "GTEBase.h"
#include "GEPipeline.h"
#include <functional>
                                                   

#ifndef OMEGAGTE_GERENDERTARGET_H
#define OMEGAGTE_GERENDERTARGET_H

_NAMESPACE_BEGIN_
    class GECommandBuffer;
    struct GECommandBufferCompletionInfo;
    using GECommandBufferCompletionHandler =
            std::function<void(const GECommandBufferCompletionInfo &)>;
    class GEBuffer;
    class GETexture;
    class GEMesh;
    struct GEViewport;
    struct GEScissorRect;

    class  OMEGAGTE_EXPORT GERenderTarget {
    public:
        struct OMEGAGTE_EXPORT RenderPassDesc {
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
            } depthStencilAttachment;
            /// Per-color-attachment load/clear state. Index 0 is the primary
            /// color attachment (falls back to the render target's native
            /// texture when no per-attachment texture is supplied). Indices
            /// > 0 enable multi-render-target output and require an explicit
            /// texture on each attachment.
            OmegaCommon::Vector<ColorAttachment> colorAttachments;
            struct OMEGAGTE_EXPORT MultisampleResolveDesc {
                SharedHandle<GETexture> multiSampleTextureSrc = nullptr;
                unsigned level,slice,depth;
            } mutlisampleResolveDesc;
        };
        class OMEGAGTE_EXPORT CommandBuffer : public GTEResource {
            GERenderTarget *renderTargetPtr;
            SharedHandle<GECommandBuffer> commandBuffer;
            friend class GERenderTarget;

            #ifdef TARGET_DIRECTX
            friend class GED3D12NativeRenderTarget;
            friend class GED3D12TextureRenderTarget;
            #endif
            
            #ifdef TARGET_METAL
            friend class GEMetalNativeRenderTarget;
            friend class GEMetalTextureRenderTarget;
            #endif

        #ifdef TARGET_VULKAN
            friend class GEVulkanNativeRenderTarget;
            friend class GEVulkanTextureRenderTarget;
        #endif
        
            /// Do NOT CALL THESE CONSTRUCTORS!!!

            using GERTType = enum : uint8_t {
                Native,
                Texture
            };
            GERTType renderTargetTy;
            /// Do NOT CALL THIS CONSTRUCTOR!!!
            CommandBuffer(GERenderTarget *renderTarget,GERTType type,SharedHandle<GECommandBuffer> commandBuffer);
        public:
            ~CommandBuffer() = default;
            OMEGACOMMON_CLASS("OmegaGTE.GECommandBuffer")

            void setName(OmegaCommon::StrRef name) override;

            void * native() override;

            /// @brief Start a Render Pass
            /// @param desc A RenderPassDesc describing the pass.
            /// @paragraph This method MUST be called before encoding any render commands such as drawPolygons() @see drawPolygons()

            void startRenderPass(const RenderPassDesc & desc);

            /// @brief Sets the Render Pipeline State in the current Render Pass
            /// @param pipelineState A GERenderPipelineState
            /// @paragraph This method can only be called once startRenderPass() has been called and must be called before any draw commands are initiated.
            void setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState);

            /// @brief Binds a Buffer Resource to a Descriptor in the scope of the Vertex Shader.
            /// @param buffer The Resource to bind.
            /// @param id The OmegaSL Binding id.
            void bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer,unsigned id);

            /// @brief Binds a Texture Resource to a Descriptor in the scope of the Vertex Shader.
            /// @param texture The Resource to bind.
            /// @param id The OmegaSL Binding id.
            /// @param swizzle Optional channel remap. Identity (default) keeps existing behavior; see TextureSwizzle.
            void bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned id,
                                            const TextureSwizzle & swizzle = TextureSwizzle::identity());

            /// @brief Binds a Buffer Resource to a Descriptor in the scope of the Fragment Shader.
            /// @param buffer The Resource to bind.
            /// @param id The OmegaSL Binding id.
            void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned id);

            /// @brief Binds a Texture Resource to a Descriptor in the scope of the Fragment Shader.
            /// @param texture The Resource to bind.
            /// @param id The OmegaSL Binding id.
            /// @param swizzle Optional channel remap. Identity (default) keeps existing behavior; see TextureSwizzle.
            void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned id,
                                              const TextureSwizzle & swizzle = TextureSwizzle::identity());

            /// @brief Dynamically sets the Viewports on the Render Pipeline.
            /// @param viewports The GEViewports
            void setViewports(std::vector<GEViewport> viewports);

            /// @brief Dynamically sets the Scissor Rects on the Render Pipeline.
            /// @param scissorRects The GEScissorRects.
            void setScissorRects(std::vector<GEScissorRect> scissorRects);
        
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

            /// @brief Encodes a draw command in the current Render Pass.
            /// @param polygonType The Type of Polygons to draw.
            /// @param vertexCount The Number of Vertices to draw.
            /// @param start The Index of the first Vertex to draw.
            void drawPolygons(PolygonType polygonType, unsigned vertexCount, size_t start);

            /// @brief Binds an index buffer for subsequent indexed draw calls.
            /// @param buffer The GEBuffer containing the index data.
            /// @param indexType The element type (UInt16 or UInt32).
            void setIndexBuffer(SharedHandle<GEBuffer> & buffer, IndexType indexType = IndexType::UInt32);

            /// @brief Encodes an indexed draw command in the current Render Pass.
            /// @param polygonType The Type of Polygons to draw.
            /// @param indexCount The Number of Indices to read.
            /// @param startIndex Index of the first index.
            /// @param baseVertex Value added to the vertex index before fetching vertex attributes.
            void drawIndexedPolygons(PolygonType polygonType, unsigned indexCount,
                                     size_t startIndex, int baseVertex = 0);

            /// @brief Encodes an instanced draw command in the current Render Pass.
            /// @param polygonType The Type of Polygons to draw.
            /// @param vertexCount The Number of Vertices to draw per instance.
            /// @param start The Index of the first Vertex to draw.
            /// @param instanceCount Number of instances.
            /// @param firstInstance Value of gl_InstanceIndex / SV_InstanceID of the first instance.
            void drawPolygonsInstanced(PolygonType polygonType, unsigned vertexCount, size_t start,
                                       unsigned instanceCount, unsigned firstInstance = 0);

            /// @brief Encodes an indexed, instanced draw command in the current Render Pass.
            /// @param polygonType The Type of Polygons to draw.
            /// @param indexCount The Number of Indices to read per instance.
            /// @param startIndex Index of the first index.
            /// @param baseVertex Value added to the vertex index before fetching vertex attributes.
            /// @param instanceCount Number of instances.
            /// @param firstInstance Value of gl_InstanceIndex / SV_InstanceID of the first instance.
            void drawIndexedPolygonsInstanced(PolygonType polygonType, unsigned indexCount,
                                              size_t startIndex, int baseVertex,
                                              unsigned instanceCount, unsigned firstInstance = 0);

            /// @brief Encodes a non-indexed indirect draw command.
            /// @param polygonType The Primitive Topology.
            /// @param argumentBuffer Buffer containing a `GEDrawIndirectCommand`.
            /// @param argumentBufferOffset Byte offset into the argument buffer.
            void drawPolygonsIndirect(PolygonType polygonType,
                                       SharedHandle<GEBuffer> & argumentBuffer,
                                       size_t argumentBufferOffset = 0);

            /// @brief Encodes an indexed indirect draw command.
            /// Requires a prior `setIndexBuffer()` call.
            /// @param polygonType The Primitive Topology.
            /// @param argumentBuffer Buffer containing a `GEDrawIndexedIndirectCommand`.
            /// @param argumentBufferOffset Byte offset into the argument buffer.
            void drawIndexedPolygonsIndirect(PolygonType polygonType,
                                              SharedHandle<GEBuffer> & argumentBuffer,
                                              size_t argumentBufferOffset = 0);

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

            /// @brief Finish Encoding a Render Pass.
            /// @paragraph This method must be called once a draw command has been encoded into the Render Pass.
            void endRenderPass();

            /// @see GECommandBuffer
            void startComputePass(SharedHandle<GEComputePipelineState> & computePipelineState);

            /// @see GECommandBuffer
            void bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer,unsigned id);

            /// @see GECommandBuffer
            void bindResourceAtComputeShader(SharedHandle<GETexture> & texture,unsigned id,
                                             const TextureSwizzle & swizzle = TextureSwizzle::identity());

            /// @see GECommandBuffer
            void dispatchThreadgroups(unsigned x, unsigned y, unsigned z);

            /// @see GECommandBuffer
            void dispatchThreads(unsigned x, unsigned y, unsigned z);

            /// @see GECommandBuffer
            void dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                               size_t argumentBufferOffset = 0);

            /// @see GECommandBuffer
            void endComputePass();

            /// @see GECommandBuffer
            void reset();

            /// @brief Register completion callback for the underlying command buffer.
            void setCompletionHandler(GECommandBufferCompletionHandler handler);
        };
        virtual SharedHandle<CommandBuffer> commandBuffer() = 0;
        virtual void notifyCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence)  = 0;
        virtual void submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer)  = 0;
        virtual void submitCommandBuffer(SharedHandle<CommandBuffer> & commandBuffer,SharedHandle<GEFence> & fence)  = 0;
        virtual void *nativeCommandQueue() = 0;
    };
    class  OMEGAGTE_EXPORT GENativeRenderTarget : public GERenderTarget {
        public:
         OMEGACOMMON_CLASS("OmegaGTE.GENativeRenderTarget")
        virtual PixelFormat pixelFormat() { return PixelFormat::BGRA8Unorm; }
        virtual void commitAndPresent() = 0;
//        virtual void commitAndWait() = 0;
        #ifdef _WIN32
        /// @returns IDXGISwapChain1 * if D3D11, else IDXGISwapChain3 *
        virtual void *getSwapChain() = 0;
        /// Wait for GPU to finish, resize swap chain, and recreate RTVs. Call instead of IDXGISwapChain::ResizeBuffers.
        virtual void resizeSwapChain(unsigned int width, unsigned int height) {}
        /// Wait for this target's command queue to finish. Use to serialize cross-context texture pool use.
        virtual void waitForGPU() {}
        /// CPU wait until the given fence has been signaled (e.g. before using a texture produced on another queue).
        virtual void waitForFence(SharedHandle<GEFence> & fence) { (void)fence; }
        #endif
        virtual ~GENativeRenderTarget() = default;
     };
     class  OMEGAGTE_EXPORT GETextureRenderTarget : public GERenderTarget {
         public:
         OMEGACOMMON_CLASS("OmegaGTE.GETextureRenderTarget")
         virtual void commit() = 0;
         virtual SharedHandle<GETexture> underlyingTexture() = 0;
         /// Wait for this target's command queue to finish. Use before releasing pooled textures.
         virtual void waitForGPU() {}
         /// Signal fence after all texture work is done (e.g. after waitForGPU when effects were applied).
         virtual void signalFence(SharedHandle<GEFence> & fence) { (void)fence; }
        virtual ~GETextureRenderTarget() = default;
     };

_NAMESPACE_END_

#endif
