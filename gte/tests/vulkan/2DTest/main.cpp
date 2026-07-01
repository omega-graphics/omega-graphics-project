#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <gtk/gtk.h>

// GTK 4: the GDK backend headers moved under per-backend subdirectories
// (gdk/wayland/, gdk/x11/) from GTK 3's flat gdk/ layout. Independent #ifs (not
// #if/#else): a co-build defines both VULKAN_TARGET_* and needs both backend
// headers so the runtime surface dispatch below compiles either arm.
#ifdef VULKAN_TARGET_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#ifdef VULKAN_TARGET_X11
#include <gdk/x11/gdkx.h>
#endif

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
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue = nullptr;
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

    size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4, OMEGASL_FLOAT4});
    std::cout << "Struct size: " << structSize << std::endl;

    OmegaGTE::BufferDescriptor bufferDescriptor{OmegaGTE::BufferDescriptor::Upload, 6 * structSize, structSize};
    vertexBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);
    bufferWriter->setOutputBuffer(vertexBuffer);

    {
        auto &mesh = rect_mesh.mesh;
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

    auto commandBuffer = commandQueue->getAvailableBuffer();

    OmegaGTE::GERenderPassDescriptor renderPass;
    renderPass.nRenderTarget = nativeRenderTarget.get();
    using ColorAttachment = OmegaGTE::GERenderPassDescriptor::ColorAttachment;
    renderPass.colorAttachments.push_back(ColorAttachment(
        ColorAttachment::ClearColor(1.f, 1.f, 1.f, 1.f),
        ColorAttachment::Clear));

    OmegaGTE::GEViewport viewport{0, 0, (float)viewWidth, (float)viewHeight, 0, 1.f};
    OmegaGTE::GEScissorRect scissorRect{0, 0, (float)viewWidth, (float)viewHeight};

    commandBuffer->startRenderPass(renderPass);
    commandBuffer->setRenderPipelineState(renderPipeline);
    commandBuffer->bindResourceAtVertexShader(vertexBuffer, 0);
    commandBuffer->setViewports({viewport});
    commandBuffer->setScissorRects({scissorRect});
    commandBuffer->drawPolygons(OmegaGTE::GECommandBuffer::Triangle, 6, 0);
    commandBuffer->finishRenderPass();

    commandQueue->submitCommandBuffer(commandBuffer);
    commandQueue->commitToGPU();
    nativeRenderTarget->present();

    std::cout << "Frame presented" << std::endl;
}

// Architecture (developer direction): WTK does NOT use a GtkWindow for the
// render surface. A GtkWindow/GtkWidget realizes a GSK renderer onto its
// GdkSurface, and on GTK 4 that GSK renderer fights our own Vulkan swap chain
// for the same wl_surface — the probe's first cut got VK_ERROR_SURFACE_LOST on
// vkCreateSwapchainKHR plus a Wayland protocol error, because GTK 4 removed the
// GTK-3 app-paintable / double-buffer knobs that used to tell GTK to keep its
// hands off. So instead we create a *bare* toplevel GdkSurface directly
// (gdk_surface_new_toplevel) — it has no GtkWidget and therefore no GskRenderer
// attached, leaving the surface entirely ours to drive with Vulkan. GTK/GDK is
// still used, but only for windowing lifecycle (present/close) and input: the
// GdkSurface "event" signal delivers the same GdkEvents a GtkEventController
// would, with nothing painting over us.

static OmegaCommon::String g_backendName = "?";
static bool g_rendered = false;
static GMainLoop *g_loop = nullptr;

// Latest logical size from the "layout" signal — the bare toplevel has no
// decorations, so user resize is implemented client-side and needs the current
// extent to hit-test the edges.
static int g_logicalW = 0;
static int g_logicalH = 0;

// Thickness (logical px) of the edge/corner band that initiates a resize drag.
static const double kResizeBorder = 12.0;

// Map a pointer position to the toplevel edge whose resize band it lands in.
// Returns false when the point is in the interior (no resize).
static bool edgeForPoint(double x, double y, int w, int h, GdkSurfaceEdge *outEdge){
    const bool left   = x <= kResizeBorder;
    const bool right  = x >= w - kResizeBorder;
    const bool top    = y <= kResizeBorder;
    const bool bottom = y >= h - kResizeBorder;
    if(top && left)        *outEdge = GDK_SURFACE_EDGE_NORTH_WEST;
    else if(top && right)  *outEdge = GDK_SURFACE_EDGE_NORTH_EAST;
    else if(bottom && left)  *outEdge = GDK_SURFACE_EDGE_SOUTH_WEST;
    else if(bottom && right) *outEdge = GDK_SURFACE_EDGE_SOUTH_EAST;
    else if(left)   *outEdge = GDK_SURFACE_EDGE_WEST;
    else if(right)  *outEdge = GDK_SURFACE_EDGE_EAST;
    else if(top)    *outEdge = GDK_SURFACE_EDGE_NORTH;
    else if(bottom) *outEdge = GDK_SURFACE_EDGE_SOUTH;
    else return false;
    return true;
}

// Cursor name (CSS) matching the resize affordance for an edge.
static const char *cursorForEdge(GdkSurfaceEdge edge){
    switch(edge){
        case GDK_SURFACE_EDGE_NORTH:
        case GDK_SURFACE_EDGE_SOUTH:      return "ns-resize";
        case GDK_SURFACE_EDGE_WEST:
        case GDK_SURFACE_EDGE_EAST:       return "ew-resize";
        case GDK_SURFACE_EDGE_NORTH_WEST:
        case GDK_SURFACE_EDGE_SOUTH_EAST: return "nwse-resize";
        case GDK_SURFACE_EDGE_NORTH_EAST:
        case GDK_SURFACE_EDGE_SOUTH_WEST: return "nesw-resize";
        default:                          return "default";
    }
}

// Request our preferred initial size during toplevel size negotiation. Without
// this the compositor may map the surface at 0x0 and the "layout" signal never
// reports a usable extent.
static void on_compute_size(GdkToplevel *toplevel, GdkToplevelSize *size, gpointer user_data){
    gdk_toplevel_size_set_size(size, 500, 500);
}

// "layout" fires with the negotiated logical size once the surface is mapped and
// configured — the correct, race-free point to size the swap chain (replacing
// the GtkWindow tick-callback hack the first cut needed). One-shot for the probe.
static void on_layout(GdkSurface *surface, int width, int height, gpointer user_data){
    // Track the current extent every layout so edge hit-testing stays correct
    // across user resizes, not just the first frame.
    g_logicalW = width;
    g_logicalH = height;

    if(g_rendered || width <= 0 || height <= 0)
        return;

    int scale = gdk_surface_get_scale_factor(surface);
    int pixel_width = width * scale;
    int pixel_height = height * scale;
    std::cout << "Bare GdkSurface laid out: " << width << "x" << height
              << " @scale " << scale << " => " << pixel_width << "x" << pixel_height
              << " px (backend " << g_backendName << ")" << std::endl;

    GdkDisplay *display = gdk_surface_get_display(surface);

    OmegaGTE::NativeRenderTargetDescriptor desc{};

#if defined(VULKAN_TARGET_WAYLAND) && defined(VULKAN_TARGET_X11)
    // Co-build: choose the surface path from the live display at runtime,
    // matching WTK's runtime backend dispatch.
    if(GDK_IS_WAYLAND_DISPLAY(display)){
        desc.wl_display = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
        desc.wl_surface = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
        desc.width  = (unsigned)pixel_width;
        desc.height = (unsigned)pixel_height;
    } else {
        // GTK 4.18+ deprecates the whole GdkX11 API (X11-backend sunset); these
        // remain the only way to reach the Xlib Display / XID and still work.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        desc.x_display = gdk_x11_display_get_xdisplay(display);
        desc.x_window  = gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
    }
#elif defined(VULKAN_TARGET_WAYLAND)
    desc.wl_display = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
    desc.wl_surface = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
    // Vulkan WSI reports no surface extent on Wayland (currentExtent ==
    // UINT32_MAX), so the swap chain must be sized from the descriptor.
    // X11 reads the extent from the Window, so it sets no width/height.
    desc.width  = (unsigned)pixel_width;
    desc.height = (unsigned)pixel_height;
#else
    // See the co-build branch above: GdkX11 is deprecated under GTK 4.18+ but
    // is still the only path to the Xlib handle.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    desc.x_display = gdk_x11_display_get_xdisplay(display);
    desc.x_window  = gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
#endif

    OmegaGTE::GECommandQueueDesc commandQueueDesc{};
    commandQueueDesc.maxBufferCount = 64;
    commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
    nativeRenderTarget = gte.graphicsEngine->makeNativeRenderTarget(desc, commandQueue);
    tessContext = gte.triangulationEngine->createTEContextFromNativeRenderTarget(nativeRenderTarget);

    tessellateAndRender(pixel_width, pixel_height);
    g_rendered = true; // one-shot probe: drawn once at the negotiated size.
}

// Input flows through GDK exactly as the developer's "input only thru GTK/GDK"
// direction intends — the bare surface's "event" signal carries every GdkEvent.
// We log the basic input kinds to prove the path and quit on close.
static gboolean on_event(GdkSurface *surface, GdkEvent *event, gpointer user_data){
    switch(gdk_event_get_event_type(event)){
        case GDK_DELETE:
            std::cout << "GDK_DELETE — closing" << std::endl;
            if(g_loop) g_main_loop_quit(g_loop);
            return TRUE;
        case GDK_MOTION_NOTIFY: {
            // Show the resize cursor when hovering an edge band so the bare
            // (decorationless) toplevel still reads as user-resizable.
            double x = 0, y = 0;
            gdk_event_get_position(event, &x, &y);
            GdkSurfaceEdge edge;
            const char *name = edgeForPoint(x, y, g_logicalW, g_logicalH, &edge)
                ? cursorForEdge(edge) : "default";
            GdkCursor *cursor = gdk_cursor_new_from_name(name, nullptr);
            gdk_surface_set_cursor(surface, cursor);
            if(cursor) g_object_unref(cursor);
            return FALSE;
        }
        case GDK_BUTTON_PRESS: {
            double x = 0, y = 0;
            gdk_event_get_position(event, &x, &y);
            GdkSurfaceEdge edge;
            if(gdk_button_event_get_button(event) == GDK_BUTTON_PRIMARY &&
               edgeForPoint(x, y, g_logicalW, g_logicalH, &edge)){
                // Hand the drag to the compositor — it owns the actual resize
                // interaction (and on Wayland it is the only thing that can).
                gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), edge,
                    gdk_event_get_device(event),
                    (int)gdk_button_event_get_button(event),
                    x, y, gdk_event_get_time(event));
                std::cout << "GDK input: edge resize started (edge " << (int)edge << ")" << std::endl;
                return TRUE;
            }
            std::cout << "GDK input: button press @ " << x << "," << y << std::endl;
            return TRUE;
        }
        case GDK_KEY_PRESS:
            std::cout << "GDK input: key press keyval=" << gdk_key_event_get_keyval(event) << std::endl;
            return TRUE;
        default:
            return FALSE;
    }
}

int main(int argc, char *argv[]){

    gte = OmegaGTE::InitWithDefaultDevice();

    auto compiledLib = gte.omegaSlCompiler->compile({OmegaSLCompiler::Source::fromString(shaders)});
    funcLib = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);

    bufferWriter = OmegaGTE::GEBufferWriter::Create();

    std::cout << "Library shader count: " << funcLib->shaders.size() << std::endl;

    OmegaGTE::RenderPipelineDescriptor pipelineDesc;
    pipelineDesc.vertexFunc = funcLib->shaders[VERTEX_FUNC];
    pipelineDesc.fragmentFunc = funcLib->shaders[FRAGMENT_FUNC];
    pipelineDesc.colorPixelFormats = { OmegaGTE::PixelFormat::BGRA8Unorm };
    pipelineDesc.depthAndStencilDesc.enableDepth = false;
    pipelineDesc.depthAndStencilDesc.enableStencil = false;
    renderPipeline = gte.graphicsEngine->makeRenderPipelineState(pipelineDesc);

    // Initialize GTK/GDK (opens the default display + sets up the input event
    // machinery) but do NOT create a GtkApplication/GtkWindow — we manage the
    // toplevel surface ourselves so no GSK renderer is bound to it.
    gtk_init();
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr){
        std::cerr << "No GDK display — cannot open a window" << std::endl;
        return 1;
    }

#if defined(VULKAN_TARGET_WAYLAND) && defined(VULKAN_TARGET_X11)
    g_backendName = GDK_IS_WAYLAND_DISPLAY(display) ? "wayland" : "x11";
#elif defined(VULKAN_TARGET_WAYLAND)
    g_backendName = "wayland";
#else
    g_backendName = "x11";
#endif

    // Bare toplevel GdkSurface — no GtkWidget, hence no GskRenderer competing for
    // the surface. This is the whole point of the pivot.
    GdkSurface *surface = gdk_surface_new_toplevel(display);
    g_signal_connect(surface, "compute-size", G_CALLBACK(on_compute_size), nullptr);
    g_signal_connect(surface, "layout", G_CALLBACK(on_layout), nullptr);
    g_signal_connect(surface, "event", G_CALLBACK(on_event), nullptr);

    GdkToplevelLayout *toplevelLayout = gdk_toplevel_layout_new();
    gdk_toplevel_present(GDK_TOPLEVEL(surface), toplevelLayout);
    gdk_toplevel_layout_unref(toplevelLayout);

    g_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(g_loop);
    g_main_loop_unref(g_loop);

    g_object_unref(surface);

    OmegaGTE::Close(gte);

    return 0;
}
