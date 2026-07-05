#include "../GTETestWindow.h"

#include <omegaGTE/GE.h>
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

// GTK (GTK 4) implementation of the cross-backend GTETestWindow surface
// (GTETestWindow-CrossBackend-Plan.md, Phase 3). The bare-GdkSurface windowing
// and the X11/Wayland descriptor fill are lifted from the working
// vulkan/2DTest/main.cpp so the migrated test reaches visual parity. Only the
// per-platform windowing / run-loop boilerplate lives here; the render body and
// resource teardown stay in the platform-independent test source, reached
// through the delegate callbacks:
//   onReady  fires from the "layout" signal — the first, race-free point the
//            compositor reports a real logical size, which is where the
//            NativeRenderTargetDescriptor can be filled and the swap chain
//            sized (Wayland reports no WSI extent, so it must come from here).
//   onFrame  is NOT pumped on this backend today: the bare toplevel has no
//            GskRenderer (deliberately — see the architecture note below), and
//            the Vulkan swap-chain present persists the last frame, so the
//            one-shot render tests this suite runs need no redraw signal. An
//            animating Vulkan test would drive onFrame off the surface's
//            GdkFrameClock; wiring that is left until such a test exists.
//   onClose  fires once, after the main loop returns and before this function
//            hands control back to the test's main — matching where the old
//            Vulkan test called OmegaGTE::Close(gte). It runs strictly before
//            the GdkSurface (and its wl_surface) is destroyed, so the test body
//            tears down the Vulkan swap chain while its backing surface is
//            still alive.
//
// Architecture (developer direction, carried from vulkan/2DTest/main.cpp): WTK
// does NOT use a GtkWindow for the render surface. A GtkWindow/GtkWidget
// realizes a GSK renderer onto its GdkSurface, and on GTK 4 that GSK renderer
// fights our own Vulkan swap chain for the same wl_surface
// (VK_ERROR_SURFACE_LOST + a Wayland protocol error), because GTK 4 removed the
// GTK-3 app-paintable / double-buffer knobs that told GTK to keep its hands
// off. So we create a *bare* toplevel GdkSurface directly
// (gdk_surface_new_toplevel) — no GtkWidget, hence no GskRenderer — leaving the
// surface entirely ours to drive with Vulkan. GTK/GDK stays in the loop only
// for windowing lifecycle (present/close/resize) and input via the surface's
// "event" signal.

namespace OmegaGTETests {

namespace {

    const GTETestWindowDescriptor *gDesc = nullptr;
    const GTETestWindowDelegate *gDelegate = nullptr;

    // onReady is one-shot: the "layout" signal fires on every configure, but the
    // swap chain is created exactly once, at the first usable size.
    bool g_ready = false;
    GMainLoop *g_loop = nullptr;
    const char *g_backendName = "?";

    // Latest logical size from the "layout" signal — the bare toplevel has no
    // decorations, so user resize is implemented client-side and needs the
    // current extent to hit-test the edges.
    int g_logicalW = 0;
    int g_logicalH = 0;

    // Thickness (logical px) of the edge/corner band that initiates a resize
    // drag.
    const double kResizeBorder = 12.0;

    // Map a pointer position to the toplevel edge whose resize band it lands in.
    // Returns false when the point is in the interior (no resize).
    bool edgeForPoint(double x, double y, int w, int h, GdkSurfaceEdge *outEdge) {
        const bool left   = x <= kResizeBorder;
        const bool right  = x >= w - kResizeBorder;
        const bool top    = y <= kResizeBorder;
        const bool bottom = y >= h - kResizeBorder;
        if (top && left)         *outEdge = GDK_SURFACE_EDGE_NORTH_WEST;
        else if (top && right)   *outEdge = GDK_SURFACE_EDGE_NORTH_EAST;
        else if (bottom && left) *outEdge = GDK_SURFACE_EDGE_SOUTH_WEST;
        else if (bottom && right)*outEdge = GDK_SURFACE_EDGE_SOUTH_EAST;
        else if (left)           *outEdge = GDK_SURFACE_EDGE_WEST;
        else if (right)          *outEdge = GDK_SURFACE_EDGE_EAST;
        else if (top)            *outEdge = GDK_SURFACE_EDGE_NORTH;
        else if (bottom)         *outEdge = GDK_SURFACE_EDGE_SOUTH;
        else return false;
        return true;
    }

    // Cursor name (CSS) matching the resize affordance for an edge.
    const char *cursorForEdge(GdkSurfaceEdge edge) {
        switch (edge) {
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

    // Request the descriptor's size during toplevel size negotiation. Without
    // this the compositor may map the surface at 0x0 and "layout" never reports
    // a usable extent.
    void on_compute_size(GdkToplevel *toplevel, GdkToplevelSize *size, gpointer) {
        (void)toplevel;
        unsigned w = gDesc ? gDesc->width  : 500;
        unsigned h = gDesc ? gDesc->height : 500;
        gdk_toplevel_size_set_size(size, static_cast<int>(w), static_cast<int>(h));
    }

    // "layout" fires with the negotiated logical size once the surface is mapped
    // and configured — the race-free point to size the swap chain. One-shot: we
    // fill the descriptor and fire onReady exactly once, at the first good size.
    void on_layout(GdkSurface *surface, int width, int height, gpointer) {
        // Track the current extent every layout so edge hit-testing stays
        // correct across user resizes, not just the first frame.
        g_logicalW = width;
        g_logicalH = height;

        if (g_ready || width <= 0 || height <= 0)
            return;

        int scale = gdk_surface_get_scale_factor(surface);
        int pixel_width  = width  * scale;
        int pixel_height = height * scale;
        std::cout << "Bare GdkSurface laid out: " << width << "x" << height
                  << " @scale " << scale << " => " << pixel_width << "x"
                  << pixel_height << " px (backend " << g_backendName << ")"
                  << std::endl;

        GdkDisplay *display = gdk_surface_get_display(surface);

        OmegaGTE::NativeRenderTargetDescriptor nrt {};
        nrt.pixelFormat = gDesc ? gDesc->pixelFormat
                                : OmegaGTE::PixelFormat::BGRA8Unorm;
        nrt.allowDepthStencilTesting = gDesc ? gDesc->allowDepthStencilTesting
                                             : false;

#if defined(VULKAN_TARGET_WAYLAND) && defined(VULKAN_TARGET_X11)
        // Co-build: choose the surface path from the live display at runtime,
        // matching WTK's runtime backend dispatch.
        if (GDK_IS_WAYLAND_DISPLAY(display)) {
            nrt.wl_display = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
            nrt.wl_surface = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
            // Size the swap chain to the LOGICAL extent, not the pixel extent:
            // the shared test body renders through a viewport of the descriptor
            // size (desc.width x desc.height == the logical size), so the swap
            // chain must match it to fill the window — the same 1:1 invariant
            // the Win32 (client rect) and Cocoa (contentsScale = 1) backends
            // hold. Under HiDPI (scale > 1) this trades pixel-crisp rendering
            // for parity with the other backends; a pixel-sized swap chain +
            // pixel-sized viewport is the same retina follow-up left open on
            // Metal.
            nrt.width  = static_cast<unsigned>(width);
            nrt.height = static_cast<unsigned>(height);
        } else {
            // GTK 4.18+ deprecates the whole GdkX11 API (X11-backend sunset);
            // these remain the only way to reach the Xlib Display / XID.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            nrt.x_display = gdk_x11_display_get_xdisplay(display);
            nrt.x_window  = gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
        }
#elif defined(VULKAN_TARGET_WAYLAND)
        nrt.wl_display = gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
        nrt.wl_surface = gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
        // Vulkan WSI reports no surface extent on Wayland (currentExtent ==
        // UINT32_MAX), so the swap chain must be sized from the descriptor.
        // Use the LOGICAL extent (not pixel): the shared body's viewport is the
        // descriptor size (== logical size), so matching the swap chain to it
        // fills the window — the same 1:1 invariant Win32 (client rect) and
        // Cocoa (contentsScale = 1) hold. HiDPI-crisp rendering (pixel-sized
        // swap chain + viewport) is the same follow-up left open on Metal.
        nrt.width  = static_cast<unsigned>(width);
        nrt.height = static_cast<unsigned>(height);
#else
        // See the co-build branch above: GdkX11 is deprecated under GTK 4.18+
        // but is still the only path to the Xlib handle. X11 reads the extent
        // from the Window, so it sets no width/height.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        nrt.x_display = gdk_x11_display_get_xdisplay(display);
        nrt.x_window  = gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
#endif

        if (gDelegate && gDelegate->onReady)
            gDelegate->onReady(nrt);
        g_ready = true;
    }

    // Input flows through GDK exactly as the developer's "input only thru
    // GTK/GDK" direction intends — the bare surface's "event" signal carries
    // every GdkEvent. Close quits the loop (onClose then fires from
    // RunGTETestWindow); the rest reproduces the resizable-decorationless
    // affordance from the working probe.
    gboolean on_event(GdkSurface *surface, GdkEvent *event, gpointer) {
        switch (gdk_event_get_event_type(event)) {
        case GDK_DELETE:
            std::cout << "GDK_DELETE — closing" << std::endl;
            if (g_loop)
                g_main_loop_quit(g_loop);
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
            if (cursor)
                g_object_unref(cursor);
            return FALSE;
        }
        case GDK_BUTTON_PRESS: {
            double x = 0, y = 0;
            gdk_event_get_position(event, &x, &y);
            GdkSurfaceEdge edge;
            if (gdk_button_event_get_button(event) == GDK_BUTTON_PRIMARY &&
                edgeForPoint(x, y, g_logicalW, g_logicalH, &edge)) {
                // Hand the drag to the compositor — it owns the actual resize
                // interaction (and on Wayland it is the only thing that can).
                gdk_toplevel_begin_resize(GDK_TOPLEVEL(surface), edge,
                    gdk_event_get_device(event),
                    static_cast<int>(gdk_button_event_get_button(event)),
                    x, y, gdk_event_get_time(event));
                return TRUE;
            }
            return TRUE;
        }
        case GDK_KEY_PRESS:
            return TRUE;
        default:
            return FALSE;
        }
    }

} // namespace

int RunGTETestWindow(int argc,
                     const char *argv[],
                     const GTETestWindowDescriptor &desc,
                     const GTETestWindowDelegate &delegate) {
    // captureFramePath / argv-driven headless capture is Phase 5; unused here.
    (void)argc;
    (void)argv;

    gDesc = &desc;
    gDelegate = &delegate;
    g_ready = false;

    // Initialize GTK/GDK (opens the default display + sets up the input event
    // machinery) but do NOT create a GtkApplication/GtkWindow — we manage the
    // toplevel surface ourselves so no GSK renderer is bound to it.
    gtk_init();
    GdkDisplay *display = gdk_display_get_default();
    if (display == nullptr) {
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

    // Bare toplevel GdkSurface — no GtkWidget, hence no GskRenderer competing
    // for the surface. This is the whole point of the pivot.
    GdkSurface *surface = gdk_surface_new_toplevel(display);
    if (desc.title)
        gdk_toplevel_set_title(GDK_TOPLEVEL(surface), desc.title);
    g_signal_connect(surface, "compute-size", G_CALLBACK(on_compute_size), nullptr);
    g_signal_connect(surface, "layout", G_CALLBACK(on_layout), nullptr);
    g_signal_connect(surface, "event", G_CALLBACK(on_event), nullptr);

    GdkToplevelLayout *toplevelLayout = gdk_toplevel_layout_new();
    gdk_toplevel_present(GDK_TOPLEVEL(surface), toplevelLayout);
    gdk_toplevel_layout_unref(toplevelLayout);

    g_loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(g_loop);
    g_main_loop_unref(g_loop);
    g_loop = nullptr;

    // onClose fires on the GUI thread, strictly before the GdkSurface (and its
    // wl_surface) is destroyed below, so the test body drains the GPU and tears
    // down the Vulkan swap chain while its backing surface is still alive.
    if (gDelegate && gDelegate->onClose)
        gDelegate->onClose();

    g_object_unref(surface);

    return 0;
}

} // namespace OmegaGTETests
