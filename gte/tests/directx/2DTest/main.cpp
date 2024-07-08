#include <OmegaGTE.h>
#include <windows.h>
#include <pathcch.h>

#include <wrl.h>
#include <wincodec.h>

#pragma comment(lib,"Pathcch.lib")
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"runtimeobject.lib")

LRESULT CALLBACK   WndProc(HWND, UINT, WPARAM, LPARAM);

#define VERTEX_SHADER "vertexFunc"
#define FRAGMENT_SHADER "fragFunc"


static OmegaGTE::GTE gte;
static OmegaGTE::SharedHandle<OmegaGTE::GTEShaderLibrary> library;
static OmegaGTE::SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
static OmegaGTE::SharedHandle<OmegaGTE::OmegaTessellationEngineContext> tessContext;
static OmegaGTE::SharedHandle<OmegaGTE::GERenderPipelineState> renderPipelineState;
static OmegaGTE::SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;
static OmegaGTE::SharedHandle<OmegaGTE::GETexture> texture;
static OmegaGTE::SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;

void formatGPoint3D(std::ostream & os,OmegaGTE::GPoint3D & pt){
    os << "{ x:" << pt.x << ", y:" << pt.y << ", z:" << pt.z << "}";
};

void writeVertex(OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<2> & coord){
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

void tessalate(){

    OmegaGTE::GRect rect {};
    rect.h = 100;
    rect.w = 100;
    rect.pos.x = 0;
    rect.pos.y = 0;
    auto rect_mesh = tessContext->tessalateSync(OmegaGTE::TETessellationParams::Rect(rect));

    std::cout << "Tessalated GRect" << std::endl;
    auto coord = OmegaGTE::FVec<2>::Create();
    coord[0][0] = 0.f;
    coord[1][0] = 0.f;

    std::cout << "Created Matrix GRect" << std::endl;

    size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT2});

    std::cout << "StructSize:" << structSize << std::endl;

    OmegaGTE::BufferDescriptor bufferDescriptor {OmegaGTE::BufferDescriptor::Upload,6 *structSize,structSize};

    vertexBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

    bufferWriter->setOutputBuffer(vertexBuffer);

    bool otherSide = true;

    for(auto & mesh : rect_mesh.meshes){
        std::cout << "Mesh 1:" << std::endl;
        for(auto &tri : mesh.vertexPolygons){
            std::ostringstream ss;
            ss << "Triangle: {\n  A:";
            formatGPoint3D(ss,tri.a.pt);
            ss << "\n  B:";
            formatGPoint3D(ss,tri.b.pt);
            ss << "\n  C:";
            formatGPoint3D(ss,tri.c.pt);
            ss << "\n}";
            std::cout << ss.str() << std::endl;
            std::cout << "Create Vertex" << std::endl;

            writeVertex(tri.a.pt,coord);

            coord[0][0] = 0.f;
            coord[1][0] = 1.f;

            writeVertex(tri.b.pt,coord);

            coord[0][0] = 1.f;
            coord[1][0] = 0.f;


            writeVertex(tri.c.pt,coord);

            coord[0][0] = 1.f;
            coord[1][0] = 1.f;
            otherSide = true;
        };
    };

    bufferWriter->flush();

};


int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd){
    WCHAR name[MAX_PATH];
    
    GetModuleFileNameW(hInstance,name,MAX_PATH);
    PathCchRemoveExtension(name,MAX_PATH);
    PathCchRemoveFileSpec(name,MAX_PATH);
    
    SetCurrentDirectoryW(name);
    MessageBoxW(GetForegroundWindow(),(std::wstring(L"Current Dir:") + name).c_str(),L"NOTE",MB_OK);

    CoInitialize(NULL);

    Microsoft::WRL::ComPtr<IWICImagingFactory> imageFactory;

    auto res = CoCreateInstance(CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imageFactory));

    /// Load Png Image

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    imageFactory->CreateDecoderFromFilename(L"./test.png",NULL,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder);

    IWICBitmapFrameDecode *decoded;
    decoder->GetFrame(0,&decoded);

    gte = OmegaGTE::InitWithDefaultDevice();

    UINT w,h;
    decoded->GetSize(&w,&h);

    OmegaGTE::TextureDescriptor textureDescriptor {};
    textureDescriptor.usage = OmegaGTE::GETexture::ToGPU;
    textureDescriptor.type = OmegaGTE::GETexture::Texture2D;
    textureDescriptor.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm_SRGB;
    textureDescriptor.width = w;
    textureDescriptor.height = h;
    textureDescriptor.storage_opts = OmegaGTE::Shared;

    texture = gte.graphicsEngine->makeTexture(textureDescriptor);

    size_t bitmapSize = w * 4 * h;

    auto buffer = new BYTE[bitmapSize];

    decoded->CopyPixels(NULL,w * 4,bitmapSize,buffer);

    texture->copyBytes((void *)buffer,w * 4);




    library = gte.graphicsEngine->loadShaderLibrary("./shaders.omegasllib");

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = NULL;
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = NULL;
    wcex.lpszClassName  = "Test";
    wcex.hIconSm        = NULL;

    ATOM a = RegisterClassEx(&wcex);

    UINT dpi = GetDpiFromDpiAwarenessContext(GetThreadDpiAwarenessContext());

    FLOAT scaleFactor = FLOAT(dpi)/96.f;


    HWND hwnd = CreateWindowA(MAKEINTATOM(a),"",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,500 * scaleFactor,500 * scaleFactor,NULL,NULL,hInstance,NULL);
    if(!IsWindow(hwnd)){
         MessageBoxA(GetForegroundWindow(),"Failed to Create Window","NOTE",MB_OK);
        exit(1);
    };

    WNDCLASSEXA wndclass {};
    wndclass.cbClsExtra = 0;
    wndclass.cbSize = sizeof(wndclass);
    wndclass.cbWndExtra = 0;
    wndclass.hCursor = LoadCursorA(hInstance,IDC_ARROW);
    wndclass.hIcon = NULL;
    wndclass.lpszClassName = "_TestChild";
    wndclass.hIconSm = NULL;
    wndclass.hInstance = hInstance;
    wndclass.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wndclass.lpfnWndProc = WndProc;
    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpszMenuName = NULL;

    a = RegisterClassExA(&wndclass);

    HWND childHwnd = CreateWindowA(MAKEINTATOM(a),"",WS_CHILDWINDOW | WS_VISIBLE,0,(500 - 300) * scaleFactor,300 *scaleFactor,300 * scaleFactor,hwnd,NULL,hInstance,NULL);

    MessageBoxA(GetForegroundWindow(),"App Pre Launch -- Stage 0","NOTE",MB_OK);
    MessageBoxA(GetForegroundWindow(),"App Pre Launch -- Stage 1","NOTE",MB_OK);

    OmegaGTE::NativeRenderTargetDescriptor renderTargetDesc {};

    renderTargetDesc.hwnd = childHwnd;
    renderTargetDesc.isHwnd = true;

    OmegaGTE::RenderPipelineDescriptor pipelineDesc;
    pipelineDesc.rasterSampleCount = 0;
    pipelineDesc.vertexFunc = library->shaders[VERTEX_SHADER];
    pipelineDesc.fragmentFunc = library->shaders[FRAGMENT_SHADER];
    pipelineDesc.depthAndStencilDesc.enableDepth = false;
    pipelineDesc.depthAndStencilDesc.enableStencil = false;

    renderPipelineState = gte.graphicsEngine->makeRenderPipelineState(pipelineDesc);
       
    renderTarget = gte.graphicsEngine->makeNativeRenderTarget(renderTargetDesc);

    tessContext = gte.tessalationEngine->createTEContextFromNativeRenderTarget(renderTarget);

    tessalate();


    auto commandBuffer = renderTarget->commandBuffer();

    OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc;
    using ColorAttachment = OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment;
    renderPassDesc.colorAttachment = new ColorAttachment(ColorAttachment::ClearColor(0.f,1.f,0.f,1.f),ColorAttachment::Clear);

    OmegaGTE::GEViewport viewport {0,0,300,300,0,1.f};
    OmegaGTE::GEScissorRect scissorRect {0,0,300,300};

    commandBuffer->startRenderPass(renderPassDesc);
    commandBuffer->setRenderPipelineState(renderPipelineState);
    commandBuffer->bindResourceAtVertexShader(vertexBuffer,0);
    commandBuffer->bindResourceAtFragmentShader(texture,1);
    commandBuffer->setScissorRects({scissorRect});
    commandBuffer->setViewports({viewport});
    commandBuffer->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle,6,0);
    commandBuffer->endRenderPass();

    renderTarget->submitCommandBuffer(commandBuffer);

    renderTarget->commitAndPresent();

    ShowWindow(hwnd,nShowCmd);

//    UpdateWindow(hwnd);



    /// Message Loop
    MSG msg = {};
    
    while (msg.message != WM_QUIT)
    {
        // Process any messages in the queue.
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }


    OmegaGTE::Close(gte);

    decoded->Release();
    decoder->Release();
    imageFactory->Release();

    CoUninitialize();

    return msg.wParam;
};



LRESULT CALLBACK   WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam){
    LRESULT lr = 0;
    switch (uMsg) {
    case WM_PAINT : {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd,&ps);

        EndPaint(hwnd,&ps);
        break;
    }
    case WM_DESTROY: {
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcA(hwnd,uMsg,wParam,lParam);
    }

    return lr;
};
