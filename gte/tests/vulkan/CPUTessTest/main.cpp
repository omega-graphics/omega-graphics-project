#include <OmegaGTE.h>
#include <omegaGTE/GTEShader.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <iostream>
#include <cassert>
#include <cmath>

static OmegaGTE::GTE gte;
static int exitCode = 1;

static void runTests(SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> &teCtx){
    OmegaGTE::GEViewport vp{0, 0, 800, 600, 0, 1};
    bool allPassed = true;

    {
        std::cout << "\n=== Pyramid Tessellation ===" << std::endl;
        OmegaGTE::GPyramid pyramid{};
        pyramid.x = 0; pyramid.y = 0; pyramid.z = 0;
        pyramid.w = 100; pyramid.d = 100; pyramid.h = 150;
        auto params = OmegaGTE::TETriangulationParams::Pyramid(pyramid);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Meshes: " << result.meshes.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if(result.totalVertexCount() == 0){
            std::cerr << "  FAIL: Pyramid produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== Cylinder Tessellation ===" << std::endl;
        OmegaGTE::GCylinder cylinder{};
        cylinder.pos = {0, 0, 0};
        cylinder.r = 50;
        cylinder.h = 200;
        auto params = OmegaGTE::TETriangulationParams::Cylinder(cylinder);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Meshes: " << result.meshes.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if(result.totalVertexCount() == 0){
            std::cerr << "  FAIL: Cylinder produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== Cone Tessellation ===" << std::endl;
        OmegaGTE::GCone cone{};
        cone.x = 0; cone.y = 0; cone.z = 0;
        cone.r = 50;
        cone.h = 150;
        auto params = OmegaGTE::TETriangulationParams::Cone(cone);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Meshes: " << result.meshes.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if(result.totalVertexCount() == 0){
            std::cerr << "  FAIL: Cone produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== GraphicsPath3D Tessellation ===" << std::endl;
        OmegaGTE::GVectorPath3D path3d(OmegaGTE::GPoint3D{0, 0, 0});
        path3d.append(OmegaGTE::GPoint3D{100, 0, 0});
        path3d.append(OmegaGTE::GPoint3D{100, 100, 0});
        path3d.append(OmegaGTE::GPoint3D{0, 100, 0});
        path3d.append(OmegaGTE::GPoint3D{0, 0, 0});
        auto params = OmegaGTE::TETriangulationParams::GraphicsPath3D(1, &path3d);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Meshes: " << result.meshes.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if(result.totalVertexCount() == 0){
            std::cerr << "  FAIL: GraphicsPath3D produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if(allPassed){
        std::cout << "\n=== ALL CPU TESSELLATION TESTS PASSED ===" << std::endl;
        exitCode = 0;
    } else {
        std::cout << "\n=== SOME CPU TESSELLATION TESTS FAILED ===" << std::endl;
        exitCode = 1;
    }
}

static void activate(GtkApplication *app, gpointer user_data){
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "CPUTessTest");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 800, 600);
    gtk_container_add(GTK_CONTAINER(window), area);
    gtk_widget_show_all(window);
    gtk_widget_realize(area);

    GdkWindow *gdk_win = gtk_widget_get_window(area);
    Display *x_display = GDK_WINDOW_XDISPLAY(gdk_win);
    Window x_window = GDK_WINDOW_XID(gdk_win);

    OmegaGTE::NativeRenderTargetDescriptor rtDesc{};
    rtDesc.x_display = x_display;
    rtDesc.x_window = x_window;
    auto renderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc);

    auto teCtx = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);
    assert(teCtx && "Failed to create TE context");

    runTests(teCtx);

    g_application_quit(G_APPLICATION(app));
}

int main(int argc, char *argv[]){
    gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "GTE Initialized" << std::endl;

    GtkApplication *app = gtk_application_new("org.omegagraphics.CPUTessTest", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    OmegaGTE::Close(gte);
    return exitCode;
}
