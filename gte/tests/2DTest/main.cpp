#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GETextureAsset.h>
#include <omegaGTE/GTEMath.h>

#include <omega-common/fs.h>

#include <iostream>
#include <sstream>

// 2DTest — shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md,
// Phases 1-2). The per-platform windowing / run-loop boilerplate lives behind
// OmegaGTETests::RunGTETestWindow; the texture upload goes through the portable
// GETextureAsset (DirectXTex on D3D12, MTKTextureLoader on Metal), so this body
// is now fully platform-independent — no #ifdef islands. Both backends load the
// same precompiled `shaders.omegasllib` (Open Decision #2 resolved: precompiled,
// not runtime-string compilation) and sample `test.png`; the canonical copies of
// both live under gte/Tests/assets/2DTest/ and are staged next to each backend's
// executable at build time.

#define VERTEX_SHADER "vertexFunc"
#define FRAGMENT_SHADER "fragFunc"

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> library;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
static SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessContext;
static SharedHandle<OmegaGTE::GERenderPipelineState> renderPipelineState;
static SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
static SharedHandle<OmegaGTE::GETextureAsset> textureAsset;
static SharedHandle<OmegaGTE::GETexture> texture;
static SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;

void formatGPoint3D(std::ostream &os, OmegaGTE::GPoint3D &pt) {
    os << "{ x:" << pt.x << ", y:" << pt.y << ", z:" << pt.z << "}";
};

void writeVertex(OmegaGTE::GPoint3D &pt, OmegaGTE::FVec<2> &coord) {
    auto vertex_pos = OmegaGTE::FVec<4>::Create();
    vertex_pos[0][0] = pt.x;
    vertex_pos[1][0] = pt.y;
    vertex_pos[2][0] = pt.z;
    vertex_pos[3][0] = 1.f;

    bufferWriter->structBegin();
    bufferWriter->writeFloat4(vertex_pos);
    bufferWriter->writeFloat2(coord);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
}

void tessalate() {
    OmegaGTE::GRect rect {};
    rect.h = 100;
    rect.w = 100;
    rect.pos.x = 0;
    rect.pos.y = 0;
    auto rect_mesh = tessContext->triangulateSync(OmegaGTE::TETriangulationParams::Rect(rect));

    std::cout << "Tessalated GRect" << std::endl;
    std::cout << "Created Matrix GRect" << std::endl;

    size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT2});

    std::cout << "StructSize:" << structSize << std::endl;

    OmegaGTE::BufferDescriptor bufferDescriptor {OmegaGTE::BufferDescriptor::Upload, 6 * structSize, structSize};

    vertexBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

    bufferWriter->setOutputBuffer(vertexBuffer);

    // The Rect triangulator emits T1 with corners {a=BL, b=TL, c=BR} and T2
    // with corners {a=TR, b=BR, c=TL}. NDC Y=-1 is the bottom of the screen
    // (translateCoords maps world y=0 to NDC y=-1), and DX texture V=0 is the
    // top of the image — so the bottom screen edge must sample V=1.
    auto uvA = OmegaGTE::FVec<2>::Create();
    auto uvB = OmegaGTE::FVec<2>::Create();
    auto uvC = OmegaGTE::FVec<2>::Create();

    int triIdx = 0;
    {
        auto &mesh = rect_mesh.mesh;
        std::cout << "Mesh 1:" << std::endl;
        for (auto &tri : mesh.vertexPolygons) {
            std::ostringstream ss;
            ss << "Triangle: {\n  A:";
            formatGPoint3D(ss, tri.a.pt);
            ss << "\n  B:";
            formatGPoint3D(ss, tri.b.pt);
            ss << "\n  C:";
            formatGPoint3D(ss, tri.c.pt);
            ss << "\n}";
            std::cout << ss.str() << std::endl;
            std::cout << "Create Vertex" << std::endl;

            if (triIdx == 0) {
                uvA[0][0] = 0.f; uvA[1][0] = 1.f; // BL
                uvB[0][0] = 0.f; uvB[1][0] = 0.f; // TL
                uvC[0][0] = 1.f; uvC[1][0] = 1.f; // BR
            } else {
                uvA[0][0] = 1.f; uvA[1][0] = 0.f; // TR
                uvB[0][0] = 1.f; uvB[1][0] = 1.f; // BR
                uvC[0][0] = 0.f; uvC[1][0] = 0.f; // TL
            }

            writeVertex(tri.a.pt, uvA);
            writeVertex(tri.b.pt, uvB);
            writeVertex(tri.c.pt, uvC);

            ++triIdx;
        };
    };

    bufferWriter->flush();
};

GTE_TEST_ENTRY_POINT {
    (void)argc;

    // Point the working directory at the executable's folder so the relative
    // "./test.png" / "./shaders.omegasllib" loads below resolve regardless of
    // where the test is launched from. Portable across Win32 (GetModuleFileName),
    // macOS (_NSGetExecutablePath, inside the .app bundle's Contents/MacOS), and
    // Linux (/proc/self/exe).
    OmegaCommon::FS::changeCWD(OmegaCommon::FS::getExecutableDir());

    gte = OmegaGTE::InitWithDefaultDevice();

    // Portable texture load: GETextureAsset selects the backend image loader at
    // runtime (DirectXTex on D3D12, MTKTextureLoader on Metal). Match the upload
    // shape the verified DX path used — sRGB color data, no mip chain.
    textureAsset = OmegaGTE::GETextureAsset::Create(gte.graphicsEngine);
    OmegaGTE::GETextureAsset::LoadOptions textureLoadOptions {};
    textureLoadOptions.sRGB = true;
    textureLoadOptions.generateMipmaps = false;
    if (!textureAsset->load("./test.png", textureLoadOptions)) {
        std::cerr << "2DTest: failed to load ./test.png" << std::endl;
        return 1;
    }
    texture = textureAsset->texture();

    library = gte.graphicsEngine->loadShaderLibrary("./shaders.omegasllib");

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    OmegaGTE::RenderPipelineDescriptor pipelineDesc;
    pipelineDesc.rasterSampleCount = 0;
    pipelineDesc.vertexFunc = library->shaders[VERTEX_SHADER];
    pipelineDesc.fragmentFunc = library->shaders[FRAGMENT_SHADER];
    pipelineDesc.depthAndStencilDesc.enableDepth = false;
    pipelineDesc.depthAndStencilDesc.enableStencil = false;

    renderPipelineState = gte.graphicsEngine->makeRenderPipelineState(pipelineDesc);

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE 2DTest";
    desc.width = 300;
    desc.height = 300;

    OmegaGTETests::GTETestWindowDelegate del;

    del.onReady = [](const OmegaGTE::NativeRenderTargetDescriptor &nrt) {
        OmegaGTE::GECommandQueueDesc commandQueueDesc {};
        commandQueueDesc.maxBufferCount = 64;
        commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);

        renderTarget = gte.graphicsEngine->makeNativeRenderTarget(nrt, commandQueue);
        tessContext = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);

        tessalate();

        auto commandBuffer = commandQueue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor renderPassDesc;
        renderPassDesc.nRenderTarget = renderTarget.get();
        using ColorAttachment = OmegaGTE::GERenderPassDescriptor::ColorAttachment;
        renderPassDesc.colorAttachments.push_back(
            ColorAttachment(ColorAttachment::ClearColor(0.f, 1.f, 0.f, 1.f), ColorAttachment::Clear));

        OmegaGTE::GEViewport viewport {0, 0, 300, 300, 0, 1.f};
        OmegaGTE::GEScissorRect scissorRect {0, 0, 300, 300};

        commandBuffer->startRenderPass(renderPassDesc);
        commandBuffer->setRenderPipelineState(renderPipelineState);
        commandBuffer->bindResourceAtVertexShader(vertexBuffer, 0);
        commandBuffer->bindResourceAtFragmentShader(texture, 1);
        commandBuffer->setScissorRects({scissorRect});
        commandBuffer->setViewports({viewport});
        commandBuffer->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);
        commandBuffer->finishRenderPass();

        commandQueue->submitCommandBuffer(commandBuffer);
        commandQueue->commitToGPU();
        renderTarget->present();
    };

    del.onClose = []() {
        // Drain the queue before releasing any GPU-referenced resource: the
        // onReady commitToGPU() is fire-and-forget, so without this wait the
        // final SharedHandle resets below would drop textures/buffers with GPU
        // work still pending (D3D12 has no device-wide wait-idle — the queue
        // fence is the only provable drain). Then reset every GE handle in
        // dependency order so static-storage destructors at process exit see
        // nulled handles, and the D3D12MA allocator tears down with empty
        // blocks rather than asserting on live allocations.
        commandQueue->commitToGPUAndWait();

        bufferWriter.reset();
        vertexBuffer.reset();
        texture.reset();
        textureAsset.reset();
        renderPipelineState.reset();
        tessContext.reset();
        renderTarget.reset();
        library.reset();
        commandQueue.reset();

        OmegaGTE::Close(gte);
    };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
