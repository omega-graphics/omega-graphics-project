#include <OmegaGTE.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <iostream>
#include <sstream>

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

#define VERTEX_FUNC "vertexFunc"
#define FRAGMENT_FUNC "fragFunc"

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> funcLib;
static SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;
static SharedHandle<OmegaGTE::GERenderPipelineState> renderPipeline;
static SharedHandle<OmegaGTE::GENativeRenderTarget> nativeRenderTarget = nullptr;
static SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessContext;

static void formatGPoint3D(std::ostream &os, OmegaGTE::GPoint3D &pt){
    os << "{ x:" << pt.x << ", y:" << pt.y << ", z:" << pt.z << "}";
}

static void writeVertex(OmegaGTE::GPoint3D &pt, OmegaGTE::FVec<4> &color){
    auto pos_vec = OmegaGTE::FVec<4>::Create();
    pos_vec[0][0] = pt.x;
    pos_vec[1][0] = pt.y;
    pos_vec[2][0] = pt.z;
    pos_vec[3][0] = 1.f;

    bufferWriter->structBegin();
    bufferWriter->writeFloat4(pos_vec);
    bufferWriter->writeFloat4(color);
    bufferWriter->structEnd();
    bufferWriter->sendToBuffer();
}

static SharedHandle<OmegaGTE::GEBuffer> vertexBuffer;

static void tessellateAndRender(int viewWidth, int viewHeight){
    OmegaGTE::GRect rect {};
    rect.h = 100;
    rect.w = 100;
    rect.pos.x = 0;
    rect.pos.y = 0;
    auto rect_mesh = tessContext->triangulateSync(OmegaGTE::TETriangulationParams::Rect(rect));

    std::cout << "Tessellated GRect" << std::endl;

    auto color = OmegaGTE::makeColor(1.f, 0.f, 0.f, 1.f);

    size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4, OMEGASL_FLOAT4});
    std::cout << "Struct size: " << structSize << std::endl;

    OmegaGTE::BufferDescriptor bufferDescriptor{OmegaGTE::BufferDescriptor::Upload, 6 * structSize, structSize};
    vertexBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);
    bufferWriter->setOutputBuffer(vertexBuffer);

    for(auto &mesh : rect_mesh.meshes){
        std::cout << "Mesh:" << std::endl;
        for(auto &tri : mesh.vertexPolygons){
            std::ostringstream ss;
            ss << "Triangle: {\n  A:";
            formatGPoint3D(ss, tri.a.pt);
            ss << "\n  B:";
            formatGPoint3D(ss, tri.b.pt);
            ss << "\n  C:";
            formatGPoint3D(ss, tri.c.pt);
            ss << "\n}";
            std::cout << ss.str() << std::endl;

            writeVertex(tri.a.pt, color);
            writeVertex(tri.b.pt, color);
            writeVertex(tri.c.pt, color);
        }
    }

    bufferWriter->flush();

    auto commandBuffer = nativeRenderTarget->commandBuffer();

    OmegaGTE::GERenderTarget::RenderPassDesc renderPass;
    using RenderPassDesc = OmegaGTE::GERenderTarget::RenderPassDesc;
    renderPass.colorAttachment = new RenderPassDesc::ColorAttachment(
        RenderPassDesc::ColorAttachment::ClearColor(1.f, 1.f, 1.f, 1.f),
        RenderPassDesc::ColorAttachment::Clear);

    OmegaGTE::GEViewport viewport{0, 0, (float)viewWidth, (float)viewHeight, 0, 1.f};
    OmegaGTE::GEScissorRect scissorRect{0, 0, (float)viewWidth, (float)viewHeight};

    commandBuffer->startRenderPass(renderPass);
    commandBuffer->setRenderPipelineState(renderPipeline);
    commandBuffer->bindResourceAtVertexShader(vertexBuffer, 0);
    commandBuffer->setViewports({viewport});
    commandBuffer->setScissorRects({scissorRect});
    commandBuffer->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
    commandBuffer->endRenderPass();

    nativeRenderTarget->submitCommandBuffer(commandBuffer);
    nativeRenderTarget->commitAndPresent();

    std::cout << "Frame presented" << std::endl;
}

static void start_application(GtkApplication *app, gpointer user_data){
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Vulkan 2DTest");
    gtk_window_set_default_size(GTK_WINDOW(window), 500, 500);

    GtkWidget *drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, 300, 300);
    gtk_container_add(GTK_CONTAINER(window), drawing_area);

    gtk_widget_show_all(window);

    gtk_widget_realize(drawing_area);

    GdkWindow *gdk_win = gtk_widget_get_window(drawing_area);
    Display *x_display = GDK_WINDOW_XDISPLAY(gdk_win);
    Window x_window = GDK_WINDOW_XID(gdk_win);

    int scale = gdk_window_get_scale_factor(gdk_win);
    int pixel_width = gdk_window_get_width(gdk_win) * scale;
    int pixel_height = gdk_window_get_height(gdk_win) * scale;

    OmegaGTE::NativeRenderTargetDescriptor desc{};
    desc.x_display = x_display;
    desc.x_window = x_window;

    nativeRenderTarget = gte.graphicsEngine->makeNativeRenderTarget(desc);
    tessContext = gte.triangulationEngine->createTEContextFromNativeRenderTarget(nativeRenderTarget);

    tessellateAndRender(pixel_width, pixel_height);
}

int main(int argc, char *argv[]){

    gte = OmegaGTE::InitWithDefaultDevice();

#if RUNTIME_SHADER_COMP_SUPPORT
    auto compiledLib = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(shaders)});
    funcLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);
#else
    funcLib = gte.graphicsEngine->loadShaderLibrary("./shaders.omegasllib");
#endif

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    std::cout << "Library shader count: " << funcLib->shaders.size() << std::endl;

    OmegaGTE::RenderPipelineDescriptor pipelineDesc;
    pipelineDesc.vertexFunc = funcLib->shaders[VERTEX_FUNC];
    pipelineDesc.fragmentFunc = funcLib->shaders[FRAGMENT_FUNC];
    pipelineDesc.colorPixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
    pipelineDesc.depthAndStencilDesc.enableDepth = false;
    pipelineDesc.depthAndStencilDesc.enableStencil = false;
    renderPipeline = gte.graphicsEngine->makeRenderPipelineState(pipelineDesc);

    GtkApplication *app = gtk_application_new("org.omegagraphics.OmegaGTEVulkan2DTest", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(start_application), NULL);
    auto status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    OmegaGTE::Close(gte);

    return status;
}
