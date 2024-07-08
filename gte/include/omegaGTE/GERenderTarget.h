#include "GTEBase.h"
#include "GEPipeline.h"
                                                   

// #ifdef TARGET_DIRECTX
// #include <windows.h>
// #endif


#ifndef OMEGAGTE_GERENDERTARGET_H
#define OMEGAGTE_GERENDERTARGET_H

_NAMESPACE_BEGIN_
    class GECommandBuffer;
    class GEBuffer;
    class GETexture;
    struct GEViewport;
    struct GEScissorRect;

    class  OMEGAGTE_EXPORT GERenderTarget {
    public:
        struct OMEGAGTE_EXPORT RenderPassDesc {
            struct OMEGAGTE_EXPORT ColorAttachment {
                typedef enum {
                    Load,
                    LoadPreserve,
                    Clear,
                    Discard
                } LoadAction;
                LoadAction loadAction;
                struct OMEGAGTE_EXPORT ClearColor {
                    float r,g,b,a;
                    ClearColor(float r,float g,float b,float a);
                };
                ClearColor clearColor;
                ColorAttachment(ClearColor clearColor,LoadAction loadAction);
            };
            struct OMEGAGTE_EXPORT DepthStencilAttachment {
                bool disabled = true;
                typedef enum {
                    Load,
                    LoadPreserve,
                    Clear,
                    Discard
                } LoadAction;
                LoadAction depthloadAction = Discard;
                LoadAction stencilLoadAction = Discard;
                float clearDepth = 1.f;
                unsigned clearStencil = 0;
            } depthStencilAttachment;
            ColorAttachment * colorAttachment;
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

            typedef enum : uint8_t {
                Native,
                Texture
            } GERTType;
            GERTType renderTargetTy;
            /// Do NOT CALL THIS CONSTRUCTOR!!!
            CommandBuffer(GERenderTarget *renderTarget,GERTType type,SharedHandle<GECommandBuffer> commandBuffer);
          
        public:

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
            void bindResourceAtVertexShader(SharedHandle<GETexture> & texture,unsigned id);

            /// @brief Binds a Buffer Resource to a Descriptor in the scope of the Fragment Shader.
            /// @param buffer The Resource to bind.
            /// @param id The OmegaSL Binding id.
            void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer,unsigned id);

            /// @brief Binds a Texture Resource to a Descriptor in the scope of the Fragment Shader.
            /// @param texture The Resource to bind.
            /// @param id The OmegaSL Binding id.
            void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture,unsigned id);

            /// @brief Dynamically sets the Viewports on the Render Pipeline.
            /// @param viewports The GEViewports
            void setViewports(std::vector<GEViewport> viewports);

            /// @brief Dynamically sets the Scissor Rects on the Render Pipeline.
            /// @param scissorRects The GEScissorRects.
            void setScissorRects(std::vector<GEScissorRect> scissorRects);
        
            /// @brief Defines the Primitive Topology.
            typedef enum : uint8_t {
                Triangle,
                TriangleStrip
            } PolygonType;

            /// @brief Encodes a draw command in the current Render Pass.
            /// @param polygonType The Type of Polygons to draw.
            /// @param vertexCount The Number of Vertices to draw.
            /// @param start The Index of the first Vertex to draw.
            void drawPolygons(PolygonType polygonType, unsigned vertexCount, size_t start);

            /// @brief Finish Encoding a Render Pass.
            /// @paragraph This method must be called once a draw command has been encoded into the Render Pass.
            void endRenderPass();

            /// @see GECommandBuffer
            void startComputePass(SharedHandle<GEComputePipelineState> & computePipelineState);

            /// @see GECommandBuffer
            void bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer,unsigned id);

            /// @see GECommandBuffer
            void bindResourceAtComputeShader(SharedHandle<GETexture> & texture,unsigned id);

            /// @see GECommandBuffer
            void dispatchThreads(unsigned x, unsigned y, unsigned z);

            /// @see GECommandBuffer
            void endComputePass();

            /// @see GECommandBuffer
            void reset();
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
        virtual void commitAndPresent() = 0;
//        virtual void commitAndWait() = 0;
        #ifdef _WIN32 
        /// @returns IDXGISwapChain1 * if D3D11, else IDXGISwapChain3 *
        virtual void *getSwapChain() = 0;
        #endif
     };
     class  OMEGAGTE_EXPORT GETextureRenderTarget : public GERenderTarget {
         public:
         OMEGACOMMON_CLASS("OmegaGTE.GETextureRenderTarget")
         virtual void commit() = 0;
         virtual SharedHandle<GETexture> underlyingTexture() = 0;
     };

_NAMESPACE_END_

#endif
