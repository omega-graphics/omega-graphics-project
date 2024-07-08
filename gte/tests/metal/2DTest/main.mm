#include <OmegaGTE.h>

#import <Metal/Metal.h>

#import <Cocoa/Cocoa.h>
#include <sstream>
#import <QuartzCore/QuartzCore.h>
#include <iostream>

OmegaCommon::String shaders = R"(

struct Vertex {
    float4 pos;
    float4 color;
};

struct VertexRaster internal {
    float4 pos : Position;
    float4 color : Color;
};

buffer<Vertex> v_buffer : 0;

[in v_buffer]
vertex VertexRaster vertexFunc(uint v_id : VertexID){
    Vertex v = v_buffer[v_id];
    VertexRaster raster;
    raster.pos = v.pos;
    raster.color = v.color;
    return raster;
}


fragment float4 fragFunc(VertexRaster raster){
    return raster.color;
}

)";

static OmegaGTE::GTE gte;
static OmegaGTE::SharedHandle<OmegaGTE::GTEShaderLibrary> funcLib;
static OmegaGTE::SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;
static OmegaGTE::SharedHandle<OmegaGTE::GERenderPipelineState> renderPipeline;
static OmegaGTE::SharedHandle<OmegaGTE::GENativeRenderTarget> nativeRenderTarget = nullptr;
static OmegaGTE::SharedHandle<OmegaGTE::OmegaTessellationEngineContext> tessContext;
//static OmegaGTE::SharedHandle<OmegaGTE::GETexture> texture;


void formatGPoint3D(std::ostream & os,OmegaGTE::GPoint3D & pt){
    os << "{ x:" << pt.x << ", y:" << pt.y << ", z:" << pt.z << "}";
};

static void writeVertex(OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<4> &coord){
    auto pos_vec = OmegaGTE::FVec<4>::Create();
    pos_vec[0][0] = pt.x;
    pos_vec[1][0] = pt.y;
    pos_vec[2][0] = pt.z;
    pos_vec[3][0] = 1.f;

    std::cout << "Write Vertex" << std::endl;

    bufferWriter->structBegin();
    bufferWriter->writeFloat4(pos_vec);
    bufferWriter->writeFloat4(coord);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
    
}

static void render(id<MTLDevice> dev){



    OmegaGTE::GRect rect {};
    rect.h = 100;
    rect.w = 100;
    rect.pos.x = 0;
    rect.pos.y = 0;
    auto rect_mesh = tessContext->tessalateSync(OmegaGTE::TETessellationParams::Rect(rect));


    auto coord = OmegaGTE::makeColor(1.f,0.f,0.f,1.f);

    size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT4});

    std::cout << "STRUCT SIZE:" << structSize << std::endl;

    OmegaGTE::BufferDescriptor bufferDescriptor {OmegaGTE::BufferDescriptor::Upload,6 * structSize,structSize};
    auto vertexBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

    bufferWriter->setOutputBuffer(vertexBuffer);

    bool otherSide = false;

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

            writeVertex(tri.a.pt,coord);

//            coord[0][0] = 0.f;
//            coord[1][0] = 1.f;

            writeVertex(tri.b.pt,coord);

//            coord[0][0] = 1.f;
//            coord[1][0] = 0.f;

            writeVertex(tri.c.pt,coord);

//            otherSide = true;
//            coord[0][0] = 1.f;
//            coord[1][0] = 1.f;
        };
    };

    bufferWriter->flush();



        

    auto commandBuffer = nativeRenderTarget->commandBuffer();
    
    NSLog(@"Command Buffer Created");
    OmegaGTE::GERenderTarget::RenderPassDesc renderPass;
    using RenderPassDesc = OmegaGTE::GERenderTarget::RenderPassDesc;
    renderPass.colorAttachment = new RenderPassDesc::ColorAttachment(RenderPassDesc::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),RenderPassDesc::ColorAttachment::Clear);
    commandBuffer->startRenderPass(renderPass);
    commandBuffer->setRenderPipelineState(renderPipeline);
    commandBuffer->bindResourceAtVertexShader(vertexBuffer,0);
    commandBuffer->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle,6,0);
    commandBuffer->endRenderPass();
    NSLog(@"Ended Render Pass");
    nativeRenderTarget->submitCommandBuffer(commandBuffer);
    NSLog(@"Command Buffer Scheduled for Execution");

    nativeRenderTarget->commitAndPresent();
    NSLog(@"Presenting Frame"); 


};

@interface MyWindowController : NSWindowController<NSWindowDelegate>
@end

@implementation MyWindowController

- (instancetype)init
{
    if(self = [super initWithWindow:[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,500,500) styleMask:NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO]]){
        self.window.delegate = self;
        NSView *rootView = [[NSView alloc] initWithFrame:NSZeroRect];
        NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0,0,200,200)];
        view.wantsLayer = TRUE;
        CAMetalLayer * metalLayer = [CAMetalLayer layer];
        metalLayer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
        metalLayer.frame = view.frame;
        // CALayer *regLayer = [CALayer layer];
        // regLayer.backgroundColor = [NSColor blueColor].CGColor;
        view.layer = metalLayer;
        // view.layer = regLayer;


    
        OmegaGTE::NativeRenderTargetDescriptor desc{};
        desc.metalLayer = metalLayer;

        nativeRenderTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);

        tessContext = gte.tessalationEngine->createTEContextFromNativeRenderTarget(nativeRenderTarget);
        
        MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
        
        NSAssert(metalLayer.device,@"Device is NULL!");
        
        render(metalLayer.device);
    
        [rootView addSubview:view];

        [self.window setContentView:rootView];
        [self.window center];
        [self.window layoutIfNeeded];

        [manager stopCapture];
    }
    return self;
}
- (void)windowWillClose:(NSNotification *)notification{
    [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:0];
};

@end

@interface AppDelegate : NSObject<NSApplicationDelegate>
//@property(nonatomic,retain) NSWindow *window;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification{
    NSLog(@"App Finished Launching");
    auto windowController = [[MyWindowController alloc] init];
    [windowController showWindow:self];
};

- (void)applicationWillTerminate:(NSNotification *)notification {
    OmegaGTE::Close(gte);
};

@end

#define VERTEX_FUNC "vertexFunc"
#define FRAGMENT_FUNC "fragFunc"


int main(int argc,const char * argv[]){
    
    auto dir = OmegaCommon::FS::Path(argv[0]).dir();
    
    chdir(dir.c_str());
    
    gte = OmegaGTE::InitWithDefaultDevice();

    auto compiledLib = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(shaders)});

   funcLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);

   bufferWriter = OmegaGTE::GEBufferWriter::Create();

   std::cout << "LIBRARY SIZE:" << funcLib->shaders.size() << std::endl;

   OmegaGTE::RenderPipelineDescriptor pipelineDesc;
   pipelineDesc.vertexFunc = funcLib->shaders[VERTEX_FUNC];
   pipelineDesc.fragmentFunc = funcLib->shaders[FRAGMENT_FUNC];
   renderPipeline = gte.graphicsEngine->makeRenderPipelineState(pipelineDesc);


    return NSApplicationMain(argc,argv);
};
