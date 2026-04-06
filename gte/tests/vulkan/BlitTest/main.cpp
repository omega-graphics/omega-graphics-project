#include <OmegaGTE.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <iostream>

// ----- Shader sources -----
// Pass 1: render a colored triangle to an offscreen texture.
// Pass 2: blit that texture onto the swapchain via a fullscreen quad.

OmegaCommon::String shaders = R"(

struct ColorVertex {
    float4 pos;
    float4 color;
};

struct ColorRaster internal {
    float4 pos : Position;
    float4 color : Color;
};

buffer<ColorVertex> colorBuf : 0;

[in colorBuf]
vertex ColorRaster colorVertex(uint v_id : VertexID){
    ColorVertex v = colorBuf[v_id];
    ColorRaster r = { v.pos, v.color };
    return r;
}

fragment float4 colorFragment(ColorRaster r){
    return r.color;
}

struct CopyVertex {
    float4 pos;
    float2 texCoord;
};

struct CopyRaster internal {
    float4 pos : Position;
    float2 texCoord : TexCoord;
};

buffer<CopyVertex> copyBuf : 1;
texture2d copyTex : 2;
static sampler2d copySampler(filter=linear);

[in copyBuf]
vertex CopyRaster copyVertexFunc(uint v_id : VertexID){
    CopyVertex v = copyBuf[v_id];
    CopyRaster r = { v.pos, v.texCoord };
    return r;
}

[in copyTex, in copySampler]
fragment float4 copyFragFunc(CopyRaster r){
    return sample(copySampler, copyTex, r.texCoord);
}

)";

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
static SharedHandle<OmegaGTE::GEBuffer> quadBuffer;

// Sync
static SharedHandle<OmegaGTE::GEFence> fence;

static void writeColorVertex(float x, float y, float r, float g, float b){
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

static void writeCopyVertex(float x, float y, float u, float v){
    auto pos = OmegaGTE::FVec<4>::Create();
    pos[0][0] = x; pos[1][0] = y; pos[2][0] = 0.f; pos[3][0] = 1.f;
    auto tc = OmegaGTE::FVec<2>::Create();
    tc[0][0] = u; tc[1][0] = v;
    bufferWriter->structBegin();
    bufferWriter->writeFloat4(pos);
    bufferWriter->writeFloat2(tc);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
}

static void renderAndBlit(int w, int h){
    // ---- Pass 1: render colored triangle to offscreen texture ----
    std::cout << "[BlitTest] Pass 1: render triangle to offscreen texture" << std::endl;
    {
        auto cb = textureTarget->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc rp {};
        rp.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
            {0.f, 0.f, 0.f, 1.f},
            OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear);
        rp.depthStencilAttachment.disabled = true;

        OmegaGTE::GEViewport vp {0, 0, (float)w, (float)h, 0, 1.f};
        OmegaGTE::GEScissorRect sr {0, 0, (float)w, (float)h};

        cb->startRenderPass(rp);
        cb->setRenderPipelineState(colorPipeline);
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        cb->bindResourceAtVertexShader(triangleBuffer, 0);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 3, 0);
        cb->endRenderPass();

        textureTarget->submitCommandBuffer(cb, fence);
        textureTarget->commit();
        std::cout << "[BlitTest] Pass 1: committed" << std::endl;
    }

    // ---- Pass 2: blit offscreen texture to swapchain ----
    std::cout << "[BlitTest] Pass 2: blit texture to swapchain" << std::endl;
    {
        // Wait for pass 1 to finish
        auto waitCb = nativeTarget->commandBuffer();
        nativeTarget->notifyCommandBuffer(waitCb, fence);
        nativeTarget->submitCommandBuffer(waitCb);

        auto cb = nativeTarget->commandBuffer();

        OmegaGTE::GERenderTarget::RenderPassDesc rp {};
        rp.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
            {1.f, 0.f, 1.f, 1.f},  // magenta clear — visible if quad doesn't draw
            OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear);
        rp.depthStencilAttachment.disabled = true;

        OmegaGTE::GEViewport vp {0, 0, (float)w, (float)h, 0, 1.f};
        OmegaGTE::GEScissorRect sr {0, 0, (float)w, (float)h};

        auto tex = textureTarget->underlyingTexture();
        std::cout << "[BlitTest] offscreen tex ptr=" << (tex ? tex->native() : nullptr) << std::endl;

        cb->startRenderPass(rp);
        cb->setRenderPipelineState(copyPipeline);
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        cb->bindResourceAtVertexShader(quadBuffer, 1);
        cb->bindResourceAtFragmentShader(tex, 2);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
        cb->endRenderPass();

        nativeTarget->submitCommandBuffer(cb);
        nativeTarget->commitAndPresent();
        std::cout << "[BlitTest] Pass 2: presented" << std::endl;
    }
}

static void start_application(GtkApplication *app, gpointer){
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Vulkan BlitTest");
    gtk_window_set_default_size(GTK_WINDOW(window), 400, 400);

    GtkWidget *drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, 400, 400);
    gtk_container_add(GTK_CONTAINER(window), drawing_area);
    gtk_widget_show_all(window);
    gtk_widget_realize(drawing_area);

    GdkWindow *gdk_win = gtk_widget_get_window(drawing_area);
    Display *x_display = GDK_WINDOW_XDISPLAY(gdk_win);
    Window x_window = GDK_WINDOW_XID(gdk_win);

    int scale = gdk_window_get_scale_factor(gdk_win);
    int pw = gdk_window_get_width(gdk_win) * scale;
    int ph = gdk_window_get_height(gdk_win) * scale;

    std::cout << "[BlitTest] window pixel size: " << pw << "x" << ph << std::endl;

    // ---- Create native render target (swapchain) ----
    OmegaGTE::NativeRenderTargetDescriptor nDesc {};
    nDesc.x_display = x_display;
    nDesc.x_window = x_window;
    nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(nDesc);
    if(!nativeTarget){
        std::cerr << "[BlitTest] FAILED to create native render target" << std::endl;
        return;
    }
    std::cout << "[BlitTest] native render target created, format="
              << static_cast<int>(nativeTarget->pixelFormat()) << std::endl;

    // ---- Create offscreen texture + texture render target ----
    OmegaGTE::TextureDescriptor texDesc {};
    texDesc.type = OmegaGTE::GETexture::Texture2D;
    texDesc.usage = OmegaGTE::GETexture::RenderTarget;
    texDesc.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
    texDesc.width = pw;
    texDesc.height = ph;
    texDesc.storage_opts = OmegaGTE::GPUOnly;
    offscreenTex = gte.graphicsEngine->makeTexture(texDesc);
    if(!offscreenTex){
        std::cerr << "[BlitTest] FAILED to create offscreen texture" << std::endl;
        return;
    }
    std::cout << "[BlitTest] offscreen texture created: " << offscreenTex->native() << std::endl;

    OmegaGTE::TextureRenderTargetDescriptor trtDesc {};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = offscreenTex;
    textureTarget = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);
    if(!textureTarget){
        std::cerr << "[BlitTest] FAILED to create texture render target" << std::endl;
        return;
    }

    // ---- Create fence ----
    fence = gte.graphicsEngine->makeFence();

    // ---- Fill triangle vertex buffer (3 verts) ----
    {
        size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4, OMEGASL_FLOAT4});
        triangleBuffer = gte.graphicsEngine->makeBuffer(
            {OmegaGTE::BufferDescriptor::Upload, 3 * structSize, structSize});
        bufferWriter->setOutputBuffer(triangleBuffer);

        //  Red triangle covering most of the viewport
        writeColorVertex( 0.0f, -0.8f, 1.f, 0.f, 0.f);  // top-center
        writeColorVertex(-0.8f,  0.8f, 0.f, 1.f, 0.f);  // bottom-left
        writeColorVertex( 0.8f,  0.8f, 0.f, 0.f, 1.f);  // bottom-right
        bufferWriter->flush();
        std::cout << "[BlitTest] triangle buffer filled" << std::endl;
    }

    // ---- Fill fullscreen quad buffer (6 verts, 2 triangles) ----
    {
        size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4, OMEGASL_FLOAT2});
        quadBuffer = gte.graphicsEngine->makeBuffer(
            {OmegaGTE::BufferDescriptor::Upload, 6 * structSize, structSize});
        bufferWriter->setOutputBuffer(quadBuffer);

        // Triangle 1: top-left → bottom-left → bottom-right
        // Vulkan NDC: Y=-1 is top, Y=+1 is bottom
        writeCopyVertex(-1.f, -1.f,  0.f, 0.f);  // top-left
        writeCopyVertex(-1.f,  1.f,  0.f, 1.f);  // bottom-left
        writeCopyVertex( 1.f,  1.f,  1.f, 1.f);  // bottom-right

        // Triangle 2: top-left → bottom-right → top-right
        writeCopyVertex(-1.f, -1.f,  0.f, 0.f);  // top-left
        writeCopyVertex( 1.f,  1.f,  1.f, 1.f);  // bottom-right
        writeCopyVertex( 1.f, -1.f,  1.f, 0.f);  // top-right

        bufferWriter->flush();
        std::cout << "[BlitTest] quad buffer filled" << std::endl;
    }

    // ---- Create pipelines ----
    {
        OmegaGTE::RenderPipelineDescriptor desc {};
        desc.vertexFunc = shaderLib->shaders["colorVertex"];
        desc.fragmentFunc = shaderLib->shaders["colorFragment"];
        desc.colorPixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
        desc.depthAndStencilDesc = {false, false};
        desc.cullMode = OmegaGTE::RasterCullMode::None;
        desc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
        desc.rasterSampleCount = 1;
        colorPipeline = gte.graphicsEngine->makeRenderPipelineState(desc);
        if(!colorPipeline){
            std::cerr << "[BlitTest] FAILED to create color pipeline" << std::endl;
            return;
        }
        std::cout << "[BlitTest] color pipeline created" << std::endl;
    }
    {
        OmegaGTE::RenderPipelineDescriptor desc {};
        desc.vertexFunc = shaderLib->shaders["copyVertexFunc"];
        desc.fragmentFunc = shaderLib->shaders["copyFragFunc"];
        desc.colorPixelFormat = nativeTarget->pixelFormat();
        desc.depthAndStencilDesc = {false, false};
        desc.cullMode = OmegaGTE::RasterCullMode::None;
        desc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
        desc.rasterSampleCount = 1;
        copyPipeline = gte.graphicsEngine->makeRenderPipelineState(desc);
        if(!copyPipeline){
            std::cerr << "[BlitTest] FAILED to create copy pipeline" << std::endl;
            return;
        }
        std::cout << "[BlitTest] copy pipeline created" << std::endl;
    }

    renderAndBlit(pw, ph);
}

int main(int argc, char *argv[]){
    gte = OmegaGTE::InitWithDefaultDevice();

#if RUNTIME_SHADER_COMP_SUPPORT
    auto compiled = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(shaders)});
    shaderLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);
#else
    #error "BlitTest requires runtime shader compilation"
#endif

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    std::cout << "[BlitTest] Shaders loaded: " << shaderLib->shaders.size() << std::endl;
    for(auto &kv : shaderLib->shaders){
        std::cout << "  - " << kv.first << std::endl;
    }

    GtkApplication *app = gtk_application_new("org.omegagraphics.BlitTest", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(start_application), NULL);
    auto status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    // Cleanup
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

    OmegaGTE::Close(gte);
    return status;
}
