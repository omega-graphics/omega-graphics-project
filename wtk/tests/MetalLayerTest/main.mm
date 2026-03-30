// Test 9: Buffer-based blit on MAIN thread (to confirm threading is the issue).
// Same blit pipeline as test 8, but everything on main thread.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

static id<MTLDevice> g_device;

static const char *shaderSource = R"(
#include <metal_stdlib>
using namespace metal;
struct ContentOut { float4 position [[position]]; };
vertex ContentOut contentVertex(uint vid [[vertex_id]]) {
    float2 p[6] = { {-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1} };
    return { float4(p[vid], 0, 1) };
}
fragment float4 contentFragment(ContentOut in [[stage_in]]) { return float4(1, 0, 0, 1); }

struct CopyVertex { float4 pos; float2 texCoord; };
struct BlitOut { float4 position [[position]]; float2 uv; };
vertex BlitOut blitVertex(uint vid [[vertex_id]], const device CopyVertex *verts [[buffer(1)]]) {
    BlitOut out; out.position = verts[vid].pos; out.uv = verts[vid].texCoord; return out;
}
constexpr sampler blitSampler(filter::linear);
fragment float4 blitFragment(BlitOut in [[stage_in]], texture2d<float> tex [[texture(2)]]) {
    return tex.sample(blitSampler, in.uv);
}
)";

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic) NSWindow *window;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    g_device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_device newLibraryWithSource:@(shaderSource) options:nil error:&error];

    MTLRenderPipelineDescriptor *cd = [[MTLRenderPipelineDescriptor alloc] init];
    cd.vertexFunction = [lib newFunctionWithName:@"contentVertex"];
    cd.fragmentFunction = [lib newFunctionWithName:@"contentFragment"];
    cd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> contentPSO = [g_device newRenderPipelineStateWithDescriptor:cd error:&error];

    MTLRenderPipelineDescriptor *bd = [[MTLRenderPipelineDescriptor alloc] init];
    bd.vertexFunction = [lib newFunctionWithName:@"blitVertex"];
    bd.fragmentFunction = [lib newFunctionWithName:@"blitFragment"];
    bd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> blitPSO = [g_device newRenderPipelineStateWithDescriptor:bd error:&error];

    // Same vertex buffer as OmegaWTK (with corrected CCW winding)
    struct CV { float pos[4]; float tc[2]; };
    CV verts[6] = {
        {{-1,-1,0,1},{0,1}}, {{ 1,-1,0,1},{1,1}}, {{-1, 1,0,1},{0,0}},
        {{-1, 1,0,1},{0,0}}, {{ 1,-1,0,1},{1,1}}, {{ 1, 1,0,1},{1,0}},
    };
    id<MTLBuffer> blitVB = [g_device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];

    NSRect frame = NSMakeRect(100,100,500,500);
    _window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    _window.delegate = self;

    NSView *view = [[NSView alloc] initWithFrame:frame];
    view.wantsLayer = YES;
    _window.contentView = view;

    CAMetalLayer *ml = [CAMetalLayer layer];
    ml.device = g_device;
    ml.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ml.framebufferOnly = NO;
    ml.contentsScale = [NSScreen mainScreen].backingScaleFactor;
    ml.frame = view.bounds;
    ml.drawableSize = CGSizeMake(view.bounds.size.width * ml.contentsScale, view.bounds.size.height * ml.contentsScale);
    [view.layer addSublayer:ml];
    [_window makeKeyAndOrderFront:nil];

    unsigned W = (unsigned)ml.drawableSize.width;
    unsigned H = (unsigned)ml.drawableSize.height;
    id<MTLCommandQueue> queue = [g_device newCommandQueue];

    // Offscreen texture
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> offscreen = [g_device newTextureWithDescriptor:td];

    // Step 1: render to offscreen
    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    MTLRenderPassDescriptor *rp1 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp1.colorAttachments[0].texture = offscreen;
    rp1.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp1.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    rp1.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e1 = [cb1 renderCommandEncoderWithDescriptor:rp1];
    [e1 setRenderPipelineState:contentPSO];
    [e1 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [e1 endEncoding];
    [cb1 commit];
    [cb1 waitUntilCompleted];

    // Step 2: blit to drawable (buffer-based, texture at index 2)
    id<CAMetalDrawable> drawable = [ml nextDrawable];
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    MTLRenderPassDescriptor *rp2 = [MTLRenderPassDescriptor renderPassDescriptor];
    rp2.colorAttachments[0].texture = drawable.texture;
    rp2.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp2.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
    rp2.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e2 = [cb2 renderCommandEncoderWithDescriptor:rp2];
    [e2 setRenderPipelineState:blitPSO];
    MTLViewport vp = {0,0,(double)W,(double)H,0,1};
    [e2 setViewport:vp];
    MTLScissorRect sc = {0,0,W,H};
    [e2 setScissorRect:sc];
    [e2 setVertexBuffer:blitVB offset:0 atIndex:1];
    [e2 setFragmentTexture:offscreen atIndex:2];
    [e2 drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [e2 endEncoding];
    [cb2 presentDrawable:drawable];
    [cb2 commit];
    [cb2 waitUntilCompleted];
    NSLog(@"Done: status=%lu", (unsigned long)cb2.status);
}
- (void)windowWillClose:(NSNotification *)n { [NSApp terminate:nil]; }
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *d = [[AppDelegate alloc] init];
        app.delegate = d;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
