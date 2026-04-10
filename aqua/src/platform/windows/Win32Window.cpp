#include <aqua/Window.h>
#include <omegaGTE/GE.h>

#include <windows.h>

static const wchar_t *AQUA_WINDOW_CLASS = L"AquaWindowClass";
static bool sClassRegistered = false;

static LRESULT CALLBACK AquaWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        auto *flag = reinterpret_cast<bool *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (flag) *flag = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

namespace Aqua {

struct Window::Impl {
    HWND hwnd = nullptr;
    bool closeRequested = false;
    unsigned w = 0;
    unsigned h = 0;
};

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window() {
    if (impl->hwnd) {
        DestroyWindow(impl->hwnd);
    }
}

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    if (!sClassRegistered) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = AquaWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = AQUA_WINDOW_CLASS;
        RegisterClassExW(&wc);
        sClassRegistered = true;
    }

    auto window = std::unique_ptr<Window>(new Window());
    window->impl->w = desc.width;
    window->impl->h = desc.height;

    RECT rc = {0, 0, (LONG)desc.width, (LONG)desc.height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    int titleLen = MultiByteToWideChar(CP_UTF8, 0, desc.title, -1, nullptr, 0);
    std::wstring wTitle(titleLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, desc.title, -1, wTitle.data(), titleLen);

    window->impl->hwnd = CreateWindowExW(
        0, AQUA_WINDOW_CLASS, wTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    SetWindowLongPtr(window->impl->hwnd, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(&window->impl->closeRequested));

    ShowWindow(window->impl->hwnd, SW_SHOW);
    UpdateWindow(window->impl->hwnd);

    return window;
}

bool Window::shouldClose() const {
    return impl->closeRequested;
}

void Window::pollEvents() {
    MSG msg;
    while (PeekMessageW(&msg, impl->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

unsigned Window::width() const { return impl->w; }
unsigned Window::height() const { return impl->h; }

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
    desc.isHwnd = true;
    desc.hwnd = impl->hwnd;
    desc.width = impl->w;
    desc.height = impl->h;
}

} // namespace Aqua
