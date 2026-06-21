#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>

#include <iostream>
#include <sstream>

// 2DTest — shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md,
// Phase 1). The Win32 windowing / run-loop boilerplate that this file used to
// own now lives behind OmegaGTETests::RunGTETestWindow; what remains here is
// the render body (tessellate a rect, write the vertex buffer, encode one
// render pass, present) plus the per-test resource teardown order.
//
// Phase 1 wires only the DirectX build through this body. The texture upload
// is still WIC/COM (Win32) today — Metal and Vulkan render a flat-colored rect
// with no texture in their current sibling sources, so the portable image path
// that lets all three share this exact body is reconciled in Phases 2-3. That
// is the only backend-specific island left in this file; everything else is
// already pure OmegaGTE public API.

#if defined(TARGET_DIRECTX)
#include <windows.h>
#include <pathcch.h>
#include <wrl.h>
#include <wincodec.h>

#pragma comment(lib, "Pathcch.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "runtimeobject.lib")
#endif

#define VERTEX_SHADER "vertexFunc"
#define FRAGMENT_SHADER "fragFunc"

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> library;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
static SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessContext;
static SharedHandle<OmegaGTE::GERenderPipelineState> renderPipelineState;
static SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
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
#if defined(TARGET_DIRECTX)
    // Point the working directory at the executable's folder so the relative
    // "./test.png" / "./shaders.omegasllib" loads below resolve regardless of
    // where the test is launched from (lifted from the old WinMain body).
    {
        WCHAR moduleDir[MAX_PATH];
        GetModuleFileNameW(GetModuleHandleW(nullptr), moduleDir, MAX_PATH);
        PathCchRemoveFileSpec(moduleDir, MAX_PATH);
        SetCurrentDirectoryW(moduleDir);
    }
#endif

    gte = OmegaGTE::InitWithDefaultDevice();

#if defined(TARGET_DIRECTX)
    // ---- Win32/WIC texture upload (DX-only today; see file header) ----------
    CoInitialize(NULL);
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> imageFactory;
        CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&imageFactory));

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        imageFactory->CreateDecoderFromFilename(L"./test.png", NULL, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder);

        IWICBitmapFrameDecode *decoded = nullptr;
        decoder->GetFrame(0, &decoded);

        // WIC's native frame format for a typical PNG is 32bppBGRA. Our texture
        // is RGBA8Unorm_SRGB, so we run the frame through a format converter to
        // swap channels before upload — otherwise R and B come out swapped.
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        imageFactory->CreateFormatConverter(&converter);
        converter->Initialize(decoded, GUID_WICPixelFormat32bppRGBA,
                              WICBitmapDitherTypeNone, NULL, 0.f,
                              WICBitmapPaletteTypeCustom);

        UINT w, h;
        decoded->GetSize(&w, &h);

        OmegaGTE::TextureDescriptor textureDescriptor {};
        textureDescriptor.usage = OmegaGTE::GETexture::ToGPU;
        textureDescriptor.kind = OmegaGTE::TextureKind::Tex2D;
        textureDescriptor.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm_SRGB;
        textureDescriptor.width = w;
        textureDescriptor.height = h;
        textureDescriptor.storage_opts = OmegaGTE::Shared;

        texture = gte.graphicsEngine->makeTexture(textureDescriptor);

        size_t bitmapSize = w * 4 * h;
        auto *buffer = new BYTE[bitmapSize];
        converter->CopyPixels(NULL, w * 4, bitmapSize, buffer);
        texture->copyBytes(static_cast<void *>(buffer), w * 4);
        delete[] buffer;

        decoded->Release();
    }
    CoUninitialize();
    // -------------------------------------------------------------------------
#endif

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
        renderPipelineState.reset();
        tessContext.reset();
        renderTarget.reset();
        library.reset();
        commandQueue.reset();

        OmegaGTE::Close(gte);
    };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
