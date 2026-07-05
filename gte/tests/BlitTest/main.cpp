#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>

#include <omega-common/fs.h>

#include <iostream>

// BlitTest — shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md,
// Phase 4). Two render passes: pass 1 renders a colored triangle to an
// offscreen texture; pass 2 blits that texture onto the swapchain via a
// fullscreen quad. Vulkan-only today. Resolves Open Decision #2 for this
// test: the shader used to be compiled at runtime from an inline R"(...)"
// string (`omegaSlCompiler->compile` + `loadShaderLibraryRuntime`); it is now
// the canonical precompiled `assets/BlitTest/shaders.omegasl`, loaded the same
// way every other migrated test loads its shader library.

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLib;
static SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;

// Pass 1 state
static SharedHandle<OmegaGTE::GERenderPipelineState> colorPipeline;
static SharedHandle<OmegaGTE::GETextureRenderTarget> textureTarget;
static SharedHandle<OmegaGTE::GETexture> offscreenTex;
static SharedHandle<OmegaGTE::GEBuffer> triangleBuffer;

// Pass 2 state
static SharedHandle<OmegaGTE::GERenderPipelineState> copyPipeline;
static SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
static SharedHandle<OmegaGTE::GEBuffer> quadBuffer;

// Sync
static SharedHandle<OmegaGTE::GEFence> fence;

static void writeColorVertex(float x, float y, float r, float g, float b) {
    auto pos = OmegaGTE::FVec<4>::Create();
    pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
    auto col = OmegaGTE::FVec<4>::Create();
    col[0][0] = r; col[1][0] = g; col[2][0] = b; col[3][0] = 1.f;
    bufferWriter->structBegin();
    bufferWriter->writeFloat4(pos);
    bufferWriter->writeFloat4(col);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
}

static void writeCopyVertex(float x, float y, float u, float v) {
    auto pos = OmegaGTE::FVec<4>::Create();
    pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
    auto tc = OmegaGTE::FVec<2>::Create();
    tc[0][0] = u; tc[1][0] = v;
    auto pad = OmegaGTE::FVec<2>::Create();
    pad[0][0] = 0.f; pad[1][0] = 0.f;
    bufferWriter->structBegin();
    bufferWriter->writeFloat4(pos);
    bufferWriter->writeFloat2(tc);
    bufferWriter->writeFloat2(pad);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
}

static void renderAndBlit(int w, int h) {
    using ColorAttachment = OmegaGTE::GERenderPassDescriptor::ColorAttachment;

    // ---- Pass 1: render colored triangle to offscreen texture ----
    std::cout << "[BlitTest] Pass 1: render triangle to offscreen texture" << std::endl;
    {
        auto cb = commandQueue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor rp{};
        rp.tRenderTarget = textureTarget.get();
        rp.colorAttachments.push_back(ColorAttachment(
            {0.f, 0.f, 0.f, 1.f},
            ColorAttachment::Clear));
        rp.depthStencilAttachment.disabled = true;

        OmegaGTE::GEViewport vp{0, 0, (float)w, (float)h, 0, 1.f};
        OmegaGTE::GEScissorRect sr{0, 0, (float)w, (float)h};

        cb->startRenderPass(rp);
        cb->setRenderPipelineState(colorPipeline);
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        cb->bindResourceAtVertexShader(triangleBuffer, 0);
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 3, 0);
        cb->finishRenderPass();

        commandQueue->submitCommandBuffer(cb, fence);
        commandQueue->commitToGPU();
        std::cout << "[BlitTest] Pass 1: committed" << std::endl;
    }

    gte.graphicsEngine->waitForGPUIdle();

    // ---- Pass 2: blit offscreen texture to swapchain ----
    std::cout << "[BlitTest] Pass 2: blit texture to swapchain" << std::endl;
    {
        auto cb = commandQueue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor rp{};
        rp.nRenderTarget = nativeTarget.get();
        rp.colorAttachments.push_back(ColorAttachment(
            {1.f, 0.f, 1.f, 1.f},  // magenta clear — visible if quad doesn't draw
            ColorAttachment::Clear));
        rp.depthStencilAttachment.disabled = true;

        OmegaGTE::GEViewport vp{0, 0, (float)w, (float)h, 0, 1.f};
        OmegaGTE::GEScissorRect sr{0, 0, (float)w, (float)h};

        auto tex = textureTarget->underlyingTexture();
        std::cout << "[BlitTest] offscreen tex ptr=" << (tex ? tex->native() : nullptr) << std::endl;

        cb->startRenderPass(rp);
        cb->setRenderPipelineState(copyPipeline);
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        cb->bindResourceAtVertexShader(quadBuffer, 1);
        cb->bindResourceAtFragmentShader(tex, 2);
        cb->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);
        cb->finishRenderPass();

        commandQueue->submitCommandBuffer(cb);
        commandQueue->commitToGPU();
        nativeTarget->present();
        std::cout << "[BlitTest] Pass 2: presented" << std::endl;
    }
}

GTE_TEST_ENTRY_POINT {
    (void)argc;

    OmegaCommon::FS::changeCWD(OmegaCommon::FS::getExecutableDir());

    gte = OmegaGTE::InitWithDefaultDevice();

    shaderLib = gte.graphicsEngine->loadShaderLibrary("./shaders.omegasllib");
    if (!shaderLib) {
        std::cerr << "[BlitTest] failed to load ./shaders.omegasllib" << std::endl;
        return 1;
    }

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    std::cout << "[BlitTest] Shaders loaded: " << shaderLib->shaders.size() << std::endl;
    for (auto &kv : shaderLib->shaders) {
        std::cout << "  - " << kv.first << std::endl;
    }

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE BlitTest";
    desc.width = 400;
    desc.height = 400;

    OmegaGTETests::GTETestWindowDelegate del;

    del.onReady = [&desc](const OmegaGTE::NativeRenderTargetDescriptor &nrt) {
        int w = static_cast<int>(desc.width);
        int h = static_cast<int>(desc.height);

        OmegaGTE::GECommandQueueDesc commandQueueDesc{};
        commandQueueDesc.maxBufferCount = 64;
        commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
        nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(nrt, commandQueue);
        if (!nativeTarget) {
            std::cerr << "[BlitTest] FAILED to create native render target" << std::endl;
            return;
        }
        std::cout << "[BlitTest] native render target created, format="
                  << static_cast<int>(nativeTarget->pixelFormat()) << std::endl;

        // ---- Create offscreen texture + texture render target ----
        OmegaGTE::TextureDescriptor texDesc{};
        texDesc.kind = OmegaGTE::TextureKind::Tex2D;
        texDesc.usage = OmegaGTE::GETexture::RenderTarget;
        texDesc.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
        texDesc.width = w;
        texDesc.height = h;
        texDesc.storage_opts = OmegaGTE::GPUOnly;
        offscreenTex = gte.graphicsEngine->makeTexture(texDesc);
        if (!offscreenTex) {
            std::cerr << "[BlitTest] FAILED to create offscreen texture" << std::endl;
            return;
        }
        std::cout << "[BlitTest] offscreen texture created: " << offscreenTex->native() << std::endl;

        OmegaGTE::TextureRenderTargetDescriptor trtDesc{};
        trtDesc.renderToExistingTexture = true;
        trtDesc.texture = offscreenTex;
        textureTarget = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);
        if (!textureTarget) {
            std::cerr << "[BlitTest] FAILED to create texture render target" << std::endl;
            return;
        }

        // ---- Create fence ----
        fence = gte.graphicsEngine->makeFence();

        // ---- Fill triangle vertex buffer (3 verts) ----
        {
            size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT4});
            triangleBuffer = gte.graphicsEngine->makeBuffer(
                {OmegaGTE::BufferDescriptor::Upload, 3 * structSize, structSize});
            bufferWriter->setOutputBuffer(triangleBuffer);

            // Red triangle covering most of the viewport
            writeColorVertex(0.0f, -0.8f, 1.f, 0.f, 0.f);  // top-center
            writeColorVertex(-0.8f, 0.8f, 0.f, 1.f, 0.f);  // bottom-left
            writeColorVertex(0.8f, 0.8f, 0.f, 0.f, 1.f);   // bottom-right
            bufferWriter->flush();
            std::cout << "[BlitTest] triangle buffer filled" << std::endl;
        }

        // ---- Fill fullscreen quad buffer (6 verts, 2 triangles) ----
        {
            size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT2, OMEGASL_FLOAT2});
            quadBuffer = gte.graphicsEngine->makeBuffer(
                {OmegaGTE::BufferDescriptor::Upload, 6 * structSize, structSize});
            bufferWriter->setOutputBuffer(quadBuffer);

            // Triangle 1: top-left -> bottom-left -> bottom-right
            // Vulkan NDC: Y=-1 is top, Y=+1 is bottom
            writeCopyVertex(-1.f, -1.f, 0.f, 0.f);  // top-left
            writeCopyVertex(-1.f, 1.f, 0.f, 1.f);   // bottom-left
            writeCopyVertex(1.f, 1.f, 1.f, 1.f);    // bottom-right

            // Triangle 2: top-left -> bottom-right -> top-right
            writeCopyVertex(-1.f, -1.f, 0.f, 0.f);  // top-left
            writeCopyVertex(1.f, 1.f, 1.f, 1.f);    // bottom-right
            writeCopyVertex(1.f, -1.f, 1.f, 0.f);   // top-right

            bufferWriter->flush();
            std::cout << "[BlitTest] quad buffer filled" << std::endl;
        }

        // ---- Create pipelines ----
        {
            OmegaGTE::RenderPipelineDescriptor pdesc{};
            pdesc.vertexFunc = shaderLib->shaders["colorVertex"];
            pdesc.fragmentFunc = shaderLib->shaders["colorFragment"];
            pdesc.colorPixelFormats = {OmegaGTE::PixelFormat::RGBA8Unorm};
            pdesc.depthAndStencilDesc = {false, false};
            pdesc.cullMode = OmegaGTE::RasterCullMode::None;
            pdesc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
            pdesc.rasterSampleCount = 1;
            colorPipeline = gte.graphicsEngine->makeRenderPipelineState(pdesc);
            if (!colorPipeline) {
                std::cerr << "[BlitTest] FAILED to create color pipeline" << std::endl;
                return;
            }
            std::cout << "[BlitTest] color pipeline created" << std::endl;
        }
        {
            OmegaGTE::RenderPipelineDescriptor pdesc{};
            pdesc.vertexFunc = shaderLib->shaders["copyVertexFunc"];
            pdesc.fragmentFunc = shaderLib->shaders["copyFragFunc"];
            pdesc.colorPixelFormats = {nativeTarget->pixelFormat()};
            pdesc.depthAndStencilDesc = {false, false};
            pdesc.cullMode = OmegaGTE::RasterCullMode::None;
            pdesc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
            pdesc.rasterSampleCount = 1;
            copyPipeline = gte.graphicsEngine->makeRenderPipelineState(pdesc);
            if (!copyPipeline) {
                std::cerr << "[BlitTest] FAILED to create copy pipeline" << std::endl;
                return;
            }
            std::cout << "[BlitTest] copy pipeline created" << std::endl;
        }

        renderAndBlit(w, h);
    };

    del.onClose = []() {
        fence.reset();
        triangleBuffer.reset();
        quadBuffer.reset();
        colorPipeline.reset();
        copyPipeline.reset();
        offscreenTex.reset();
        textureTarget.reset();
        nativeTarget.reset();
        shaderLib.reset();
        bufferWriter.reset();
        commandQueue.reset();

        OmegaGTE::Close(gte);
    };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
