#include "../GTETestWindow.h"

#include <windows.h>
#include <string>

// Win32 implementation of the cross-backend GTETestWindow surface
// (GTETestWindow-CrossBackend-Plan.md, Phase 1). The window-creation, DPI
// scaling, and PeekMessage/DispatchMessage loop are lifted from the working
// directx/2DTest/main.cpp so the migrated test reaches pixel parity. Only the
// boilerplate that every Win32 test duplicated lives here; the render body and
// resource teardown stay in the (platform-independent) test source.

namespace OmegaGTETests {

namespace {

    /// State threaded to the wndproc through GWLP_USERDATA. `ready` gates
    /// onFrame so a stray WM_PAINT before onReady has populated the render
    /// target cannot fire a redraw against handles that do not exist yet.
    struct Win32WindowState {
        const GTETestWindowDelegate *delegate = nullptr;
        bool ready = false;
    };

    LRESULT CALLBACK GTETestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto *state =
            reinterpret_cast<Win32WindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (state && state->ready && state->delegate && state->delegate->onFrame)
                state->delegate->onFrame();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    std::wstring widen(const char *utf8) {
        if (!utf8 || !*utf8)
            return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (n <= 1)
            return std::wstring();
        std::wstring out(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], n);
        return out;
    }

} // namespace

int RunGTETestWindow(int argc,
                     const char *argv[],
                     const GTETestWindowDescriptor &desc,
                     const GTETestWindowDelegate &delegate) {
    // captureFramePath / argv-driven headless capture is Phase 5; the slot is
    // reserved in the descriptor but unused here.
    (void)argc;
    (void)argv;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);

    Win32WindowState state;
    state.delegate = &delegate;

    static const wchar_t *kClassName = L"GTETestWindow";

    // The project does not define UNICODE, so IDC_ARROW expands to its ANSI
    // (LPSTR) form. The numeric atom is identical for both widths, so the
    // reinterpret_cast hands the W API the wide-pointer it wants.
    WNDCLASSEXW wcex {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = GTETestWndProc;
    wcex.hInstance     = hInstance;
    wcex.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszClassName = kClassName;

    ATOM atom = RegisterClassExW(&wcex);
    if (!atom)
        return 1;

    // DPI scaling, lifted from directx/2DTest/main.cpp:202. The window is sized
    // in physical pixels; on a non-DPI-aware process GetDpiFromDpiAwareness-
    // Context returns 96, so scaleFactor is 1.0 and behavior is unchanged.
    UINT  dpi         = GetDpiFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
    float scaleFactor = static_cast<float>(dpi) / 96.f;

    // Grow the outer window rect so the *client* area equals the requested
    // (scaled) size. D3D12 derives the swap-chain extent from
    // GetClientRect(hwnd) (GED3D12.cpp), so the client area — not the outer
    // frame — is what must match desc.width x desc.height.
    RECT rect {0, 0,
               static_cast<LONG>(desc.width  * scaleFactor),
               static_cast<LONG>(desc.height * scaleFactor)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    std::wstring title = widen(desc.title);

    HWND hwnd = CreateWindowExW(0,
                                kClassName,
                                title.c_str(),
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left,
                                rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, nullptr);
    if (!IsWindow(hwnd))
        return 1;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    // Populate the descriptor from the realized window and hand it to the test.
    OmegaGTE::NativeRenderTargetDescriptor nrt {};
    nrt.pixelFormat              = desc.pixelFormat;
    nrt.allowDepthStencilTesting = desc.allowDepthStencilTesting;
    nrt.isHwnd                   = true;
    nrt.hwnd                     = hwnd;

    RECT client {};
    GetClientRect(hwnd, &client);
    nrt.width  = static_cast<unsigned>(client.right - client.left);
    nrt.height = static_cast<unsigned>(client.bottom - client.top);

    if (delegate.onReady)
        delegate.onReady(nrt);
    state.ready = true;

    ShowWindow(hwnd, SW_SHOWNORMAL);

    // Message pump — identical shape to the loop in directx/2DTest/main.cpp.
    MSG msg {};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // onClose fires on the GUI thread, strictly before this function returns,
    // so the test body drains the GPU and resets its GE handles here while the
    // device is still alive — see the teardown contract in the plan.
    if (delegate.onClose)
        delegate.onClose();

    return static_cast<int>(msg.wParam);
}

void RequestGTETestWindowClose(int exitCode) {
    // Safe whether the message loop above has started yet or not:
    // PostQuitMessage queues WM_QUIT on the calling thread, which is the same
    // thread that will (or already does) run the PeekMessage loop, so a call
    // from onReady (before ShowWindow) is retrieved on the loop's first pump.
    PostQuitMessage(exitCode);
}

} // namespace OmegaGTETests
