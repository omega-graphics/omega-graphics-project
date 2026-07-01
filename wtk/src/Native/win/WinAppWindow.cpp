#include "WinAppWindow.h"
#include "HWNDFactory.h"
#include "WinMenu.h"

#include "omegaWTK/UI/App.h"
#include "omegaWTK/Native/NativeTheme.h"

#include <cmath>
#include <dwmapi.h>
#include <memory>
#include <windowsx.h>

#pragma comment(lib,"dwmapi.lib")

namespace OmegaWTK::Native::Win {

namespace {
    // Per-process unique window message ID for the deferred frame-flush
    // dispatch. RegisterWindowMessage returns the same UINT (in the
    // 0xC000-0xFFFF range) for every call with the same name within the
    // process, so a single lazily-initialised static is correct. Lives
    // here rather than as a class static so RegisterWindowMessage runs
    // the first time it is needed (after the Win32 user32 module is
    // loaded), not at static-init time.
    UINT customFlushFrameMessage(){
        static const UINT id =
            RegisterWindowMessageA("OmegaWTK.FrameBuilder.FlushFrame");
        return id;
    }

    // Native-Theme-Application-Plan Tier 2 / §5.2 (2026-07-01): drive the
    // HWND's non-client area (title bar / frame) from the resolved OS
    // appearance bit. A bare HWND stays light-mode regardless of the
    // system setting until the app opts in via DWMWA_USE_IMMERSIVE_DARK_MODE;
    // macOS/GTK do this for free, so this is what brings Windows to chrome
    // parity. Called on window creation and on every theme-observer fire.
    //
    // The attribute index moved: 20 on 2004+/Windows 11, 19 on 1809–1903.
    // Try the modern index, fall back to the legacy one; both no-op (a
    // failed HRESULT we ignore) on builds < 17763. Own constants so we
    // don't depend on the SDK's dwmapi.h version. WRITE-ONLY on this
    // macOS host — needs a Windows build check.
    void applyImmersiveDarkMode(HWND hwnd){
        if(hwnd == nullptr){
            return;
        }
        auto * app = OmegaWTK::AppInst::inst();
        const bool dark = (app != nullptr) &&
            (app->nativeTheme().appearance == OmegaWTK::Native::ThemeAppearance::Dark);
        BOOL useDark = dark ? TRUE : FALSE;
        constexpr DWORD kImmersiveDarkModeNew = 20; // 2004+/Win11
        constexpr DWORD kImmersiveDarkModeOld = 19; // 1809–1903
        if(FAILED(DwmSetWindowAttribute(hwnd, kImmersiveDarkModeNew,
                                        &useDark, sizeof(useDark)))){
            DwmSetWindowAttribute(hwnd, kImmersiveDarkModeOld,
                                  &useDark, sizeof(useDark));
        }
    }
}

    WinAppWindow::WinAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter,const NativeScreenDesc *screen):
        NativeWindow(rect, emitter),
        HWNDItem(rect),
        isReady(false){
        this->event_emitter = emitter;
        // MessageBoxA(HWND_DESKTOP,"Creating WinAppWindow!","NOTE",MB_OK);
        atom = HWNDFactory::appFactoryInst->registerAppWindow();
        if(!atom){
            MessageBoxA(HWND_DESKTOP,"Failed to Register Desktop Window",NULL,MB_OK);
        };
        HWNDFactory::appFactoryInst->makeAppWindow(atom,"",rect,WS_OVERLAPPEDWINDOW,WS_EX_LAYERED,(void *)this);
        if(hwnd == NULL){
            MessageBoxA(HWND_DESKTOP,"Failed to Create Desktop Window",NULL,MB_OK);
        }
        // §2.9: seed currentDpi from the chosen screen's scale so DPI-
        // aware coordinate math (WM_GETMINMAXINFO, the wndproc's
        // logical-pixel divisions) is correct before the first
        // WM_DPICHANGED. AppWindow already pre-translated rect to
        // absolute virtual-screen coords, so GetDpiForWindow would also
        // return the right value here — but reading the screen-provided
        // value first avoids one Win32 round-trip and keeps the
        // initialization deterministic across "the wndproc was already
        // delivered a WM_DPICHANGED during CreateWindow" race orderings.
        if(screen != nullptr && screen->scaleFactor > 0.f){
            currentDpi = (UINT)std::lround(screen->scaleFactor * 96.0f);
        } else {
            currentDpi = GetDpiForWindow(hwnd);
        }
        isTracking = false;
        hovered = false;
        // WS_EX_LAYERED is set, but a layered window with no attributes is
        // invisible until SetLayeredWindowAttributes is called at least once.
        SetLayeredWindowAttributes(hwnd, 0, opacityByte_, LWA_ALPHA);
        currentCursor_ = LoadCursor(nullptr, IDC_ARROW);
        savedPlacement_.length = sizeof(WINDOWPLACEMENT);
    };

    NativeItemPtr WinAppWindow::getRootView() {
        return std::static_pointer_cast<NativeItem>(
            std::static_pointer_cast<HWNDItem>(shared_from_this()));
    }

    void WinAppWindow::setMenu(NM menu){
        this->menu = menu;
        auto nm = std::dynamic_pointer_cast<Win::WinMenu>(menu);
        SetMenu(hwnd,(HMENU)nm->getNativeBinding());
    }

    void WinAppWindow::setTitle(OmegaCommon::StrRef title){
        SetWindowTextA(hwnd,title.data());
    }

    void WinAppWindow::setEnableWindowHeader(bool &enable) {
        if(!enable){
            MARGINS margins {0,0,0,0};
            HRESULT hr = DwmExtendFrameIntoClientArea(hwnd,&margins);
            if(FAILED(hr)){
                OMEGAWTK_DEBUG("Failed to Extend Window Frame!")
            }
        }
    }

    LRESULT WinAppWindow::ProcessWndMsg(UINT u_int,WPARAM wParam,LPARAM lParam){
        LRESULT result = 0;
        // MessageBoxA(HWND_DESKTOP,"WinAppWindow WndProc","NOTE",MB_OK);
        if(u_int == WM_NCDESTROY){
            hwnd = nullptr;
        };
        if(!ProcessWndMsgImpl(hwnd,u_int,wParam,lParam,&result))
            result = DefWindowProcA(hwnd,u_int,wParam,lParam);
        return result;
    };


//     LRESULT HitTestNCA(HWND hWnd, WPARAM wParam, LPARAM lParam)
// {
//     // Get the point coordinates for the hit test.
//     POINT ptMouse = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};

//     // Get the window rectangle.
//     RECT rcWindow;
//     GetWindowRect(hWnd, &rcWindow);

//     // Get the frame rectangle, adjusted for the style without a caption.
//     RECT rcFrame = { 0 };
//     AdjustWindowRectEx(&rcFrame, WS_OVERLAPPEDWINDOW & ~WS_CAPTION, FALSE, NULL);

//     // Determine if the hit test is for resizing. Default middle (1,1).
//     USHORT uRow = 1;
//     USHORT uCol = 1;
//     bool fOnResizeBorder = false;

//     // Determine if the point is at the top or bottom of the window.
//     if (ptMouse.y >= rcWindow.top && ptMouse.y < rcWindow.top)
//     {
//         fOnResizeBorder = (ptMouse.y < (rcWindow.top - rcFrame.top));
//         uRow = 0;
//     }
//     else if (ptMouse.y < rcWindow.bottom && ptMouse.y >= rcWindow.bottom)
//     {
//         uRow = 2;
//     }

//     // Determine if the point is at the left or right of the window.
//     if (ptMouse.x >= rcWindow.left && ptMouse.x < rcWindow.left)
//     {
//         uCol = 0; // left side
//     }
//     else if (ptMouse.x < rcWindow.right && ptMouse.x >= rcWindow.right)
//     {
//         uCol = 2; // right side
//     }

//     // Hit test (HTTOPLEFT, ... HTBOTTOMRIGHT)
//     LRESULT hitTests[3][3] = 
//     {
//         { HTTOPLEFT,    fOnResizeBorder ? HTTOP : HTCAPTION,    HTTOPRIGHT },
//         { HTLEFT,       HTNOWHERE,     HTRIGHT },
//         { HTBOTTOMLEFT, HTBOTTOM, HTBOTTOMRIGHT },
//     };

//     return hitTests[uRow][uCol];
// }

    BOOL WinAppWindow::ProcessWndMsgImpl(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *lr){
        *lr = 0;
        // Dynamically-registered messages live in the 0xC000-0xFFFF range
        // and cannot appear as `case` labels, so handle the
        // FrameBuilder flush dispatch ahead of the standard switch.
        // Reset the coalescing flag BEFORE invoking the callback so any
        // re-entrant `requestFrame` from inside `flushFrame` (e.g. the
        // FrameBuilder late auto-pump while an animation is still
        // active) enqueues the next frame as a fresh PostMessage rather
        // than dropping silently.
        if(uMsg == customFlushFrameMessage()){
            frameFlushQueued_.store(false, std::memory_order_release);
            if(frameFlushCallback_){
                frameFlushCallback_();
            }
            return TRUE;
        }
        if(!DwmDefWindowProc(hwnd,uMsg,wParam,lParam,lr))
            // MessageBoxA(HWND_DESKTOP,"WinAppWindow Procedure","NOTE",MB_OK);
            switch (uMsg) {
                // case WM_CREATE :
                // {
                //     RECT rcClient;
                //     GetWindowRect(hWnd, &rcClient);

                //     // Inform the application of the frame change.
                //     SetWindowPos(hWnd, 
                //                 NULL, 
                //                 rcClient.left, rcClient.top,
                //                 rcClient.right - rcClient.left,rcClient.bottom - rcClient.top,
                //                 SWP_FRAMECHANGED);
                //     break;
                // }
                
                // case WM_NCCALCSIZE : {
                //     MARGINS margins {-1};
                //     // margins.cxLeftWidth = 0;
                //     // margins.cxRightWidth = 0;
                //     // margins.cyTopHeight = 40;
                //     // margins.cyBottomHeight = 0;
                //     HRESULT hr = S_OK;
                //     hr = DwmExtendFrameIntoClientArea(hwnd,&margins);
                //     if (FAILED(hr))
                //     {
                //         *lr = -1;
                //     }
                    
                //     break;
                // }
                // case WM_NCHITTEST : {
                    
                //     // *lr = HitTestNCA(hwnd,wParam,lParam);
                    
                //     // if (*lr == HTNOWHERE)
                //     // {
                //     //     return FALSE;
                //     // }
                //     // break;
                // }
                case WM_MENUCOMMAND : {
                    /// If the Top level Menu Exists!
                    if(menu){
                        UINT idx = wParam;
                        HMENU hmenu = (HMENU)lParam;

                        MENUINFO info;
                        info.cbSize = sizeof(info);
                        info.fMask = MIM_MENUDATA;
                        GetMenuInfo(hmenu,&info);
                        void * WinMenu = (void *) info.dwMenuData;
                        select_item(WinMenu,idx);
                        return 0;
                    };
                };
                case WM_DESTROY : {
                    if(isReady) {
                        emitIfPossible((NativeEventPtr)new NativeEvent(NativeEvent::WindowWillClose,nullptr));
                    };
                    break;
                }
                case WM_SIZE : {
                    if(isReady) {
                        FLOAT scaleFactor = FLOAT(currentDpi)/96.F;
                        UINT width = LOWORD(lParam);
                        UINT height = HIWORD(lParam);
                        wndrect = OmegaWTK::Composition::Rect {Composition::Point2D {wndrect.pos.x,wndrect.pos.y},FLOAT(width)/scaleFactor,FLOAT(height)/scaleFactor};
                        auto *params = new Native::WindowWillResize(wndrect);
                        emitIfPossible((NativeEventPtr)new NativeEvent(NativeEvent::WindowWillResize,params));
                    };
                    break;
                };
                // case WM_SIZING : {
                //     // if(isReady) {
                //     //     RECT rc = getClientRect();
                //     //     UINT height = rc.bottom - rc.top;
                //     //     updateAllHWNDPos(height,&raw_children);
                //     // };
                //     break;
                // };
                case WM_SHOWWINDOW : {
                    // NativeWindow-Ready-Signal-Plan step 5: wParam=TRUE
                    // means the window is being shown — the HWND is now
                    // visible and its swap chain is presentable. Idempotent
                    // via handleFirstRealize's firstRealizeFired_ guard.
                    if(wParam == TRUE){
                        handleFirstRealize();
                    }
                    break;
                };
                case WM_PAINT : {
                    PAINTSTRUCT ps;
                    HDC hdc = BeginPaint(hwnd,&ps);

                    // parentLayer->redraw();

                    EndPaint(hwnd,&ps);
                    // NativeWindow-Ready-Signal-Plan step 5: belt-and-
                    // suspenders. Some Win32 paths reach the first
                    // WM_PAINT without a preceding WM_SHOWWINDOW (e.g.,
                    // certain WS_CHILD lifecycles); flipping the gate
                    // here ensures isNativeReady() is true by the time
                    // the compositor's first drain runs.
                    handleFirstRealize();
                    break;
                };
                case WM_DISPLAYCHANGE : {
                    // NativeWindow-Ready-Signal-Plan step 5: display
                    // reconfiguration — monitor added/removed, resolution
                    // change, color-depth change. Treat as re-realize
                    // since the swap chain may end up on a different
                    // adapter / require recreation.
                    handleReRealize();
                    break;
                };
                case WM_THEMECHANGED :
                case WM_SETTINGCHANGE : {
                    // Native-Theme-Application-Plan Tier 1 (2026-06-30):
                    // OS light/dark flip. The dark-mode toggle arrives as
                    // WM_SETTINGCHANGE with lParam pointing at the wide
                    // string L"ImmersiveColorSet"; a classic-theme swap
                    // arrives as WM_THEMECHANGED (no lParam string). Only
                    // re-query on those two so unrelated WM_SETTINGCHANGE
                    // broadcasts (fonts, mouse, etc.) don't churn the
                    // theme. queryCurrentTheme() reads the current
                    // AppsUseLightTheme registry value; AppInst caches it
                    // and fans it out to the widget trees.
                    bool immersive = (uMsg == WM_THEMECHANGED);
                    if(uMsg == WM_SETTINGCHANGE && lParam != 0){
                        immersive = (lstrcmpiW(reinterpret_cast<const wchar_t *>(lParam),
                                               L"ImmersiveColorSet") == 0);
                    }
                    if(immersive){
                        OmegaWTK::Native::ThemeDesc desc = OmegaWTK::Native::queryCurrentTheme();
                        if(auto *app = OmegaWTK::AppInst::inst(); app != nullptr){
                            app->onThemeSet(desc);
                        }
                        // Tier 2 / §5.2: re-theme the non-client area to the
                        // new appearance (onThemeSet above refreshed the
                        // cache that applyImmersiveDarkMode reads).
                        applyImmersiveDarkMode(hwnd);
                    }
                    break;
                };
                case WM_DPICHANGED : {
                    UINT newDpi = HIWORD(wParam);
                    UINT oldDpi = currentDpi;
                    float oldScale = float(oldDpi)/96.f;
                    float newScale = float(newDpi)/96.f;
                    currentDpi = newDpi;

                    RECT* const prcNewWindow = (RECT*)lParam;
                    Core::Optional<Composition::Rect> suggested;
                    if(prcNewWindow != nullptr){
                        SetWindowPos(hwnd,
                            nullptr,
                            prcNewWindow ->left,
                            prcNewWindow ->top,
                            prcNewWindow->right - prcNewWindow->left,
                            prcNewWindow->bottom - prcNewWindow->top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        suggested = Composition::Rect{
                            Composition::Point2D{(float)prcNewWindow->left,(float)prcNewWindow->top},
                            (float)(prcNewWindow->right - prcNewWindow->left),
                            (float)(prcNewWindow->bottom - prcNewWindow->top)
                        };
                    }
                    if(NativeWindow::hasEventEmitter()){
                        auto *params = new WindowScaleFactorChangedParams{oldScale, newScale, suggested};
                        NativeWindow::eventEmitter()->emit(NativeEventPtr(new NativeEvent(NativeEvent::WindowScaleFactorChanged, params)));
                    }
                    // NativeWindow-Ready-Signal-Plan step 5: DPI change
                    // is the primary re-realize trigger on Win32. Phase
                    // F of UIView-Render-Redesign-Plan.md consumes this
                    // through onRealize to force a full-tree repaint.
                    handleReRealize();
                    break;
                }
                case WM_GETMINMAXINFO : {
                    MINMAXINFO *info = (MINMAXINFO *)lParam;
                    if(info != nullptr){
                        FLOAT scale = FLOAT(currentDpi)/96.f;
                        if(minSize_.x > 0.f && minSize_.y > 0.f){
                            info->ptMinTrackSize.x = LONG(minSize_.x * scale);
                            info->ptMinTrackSize.y = LONG(minSize_.y * scale);
                        }
                        if(maxSize_.x > 0.f && maxSize_.y > 0.f){
                            info->ptMaxTrackSize.x = LONG(maxSize_.x * scale);
                            info->ptMaxTrackSize.y = LONG(maxSize_.y * scale);
                        }
                    }
                    break;
                }
                case WM_SETCURSOR : {
                    if(LOWORD(lParam) == HTCLIENT && currentCursor_ != nullptr){
                        SetCursor(currentCursor_);
                        *lr = TRUE;
                        return TRUE;
                    }
                    return FALSE;
                }
                default:
                    // On Windows the root NativeItem is the WinAppWindow
                    // itself (see WinAppWindow::getRootView) — there's no
                    // child HWND in front of it to receive mouse input.
                    // Without this delegation, WM_LBUTTONDOWN / WM_LBUTTONUP
                    // / WM_MOUSEMOVE etc. fall straight to DefWindowProc and
                    // no NativeEvent is ever emitted, so widget hit-testing
                    // never runs and Buttons can't be clicked. Chain the
                    // inherited HWNDItem handler so its mouse cases fire.
                    return HWNDItem::ProcessWndMsgImpl(hWnd, uMsg, wParam, lParam, lr);
                }
        return TRUE;
    };

    void WinAppWindow::enable(){
        if(!IsWindowVisible(hwnd)){
            ShowWindow(hwnd,SW_SHOW);
        };
    };
    void WinAppWindow::disable(){
        if(IsWindowVisible(hwnd)){
            ShowWindow(hwnd,SW_HIDE);
        };
    };

    void WinAppWindow::close(){
        if(IsWindow(hwnd))
            DestroyWindow(hwnd);
    };

    void WinAppWindow::initialDisplay(){
        auto it = windowWidgetRootViews.begin();
        while(it != windowWidgetRootViews.end()){
            auto item = std::dynamic_pointer_cast<HWNDItem>(*it);
            if(!IsWindowVisible(item->hwnd))
                ShowWindow(item->hwnd,SW_SHOW);
            ++it;
        };
        ShowWindow(hwnd,SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
        // Native-Theme-Application-Plan Tier 2 / §5.2: theme the non-client
        // area (title bar) to the OS appearance on creation. Without this a
        // dark-mode desktop shows a bright caption over the now-dark
        // content clear — a worse seam than the old all-black content.
        applyImmersiveDarkMode(hwnd);
        // NativeWindow-Ready-Signal-Plan step 5: after ShowWindow +
        // UpdateWindow, the HWND is visible and its swap chain is
        // presentable. The wndproc's WM_SHOWWINDOW(TRUE)/WM_PAINT
        // handlers also call handleFirstRealize so order of arrival
        // doesn't matter — whichever fires first wins; subsequent
        // calls are idempotent no-ops.
        handleFirstRealize();
        // Open the WM_SIZE / WM_DESTROY event-emit gate. AppWindow's
        // setRootWidget (which wires impl_->widgetTreeHost) always runs
        // before AppWindowManager::displayRootWindow → initialDisplay,
        // so the widget tree is in place by the time this flips. The
        // initial WM_SIZE that ShowWindow above fires is intentionally
        // still gated — initWidgetTree (called immediately after this
        // returns, via displayRootWindow) sizes the tree once, so a
        // second resize-driven walk would be redundant. UIView-Render-
        // Redesign-Plan Phase F's `forceFullRepaint` then drives every
        // subsequent user-driven resize through WindowWillResize →
        // AppWindowDelegate::dispatchResize*ToHosts.
        isReady = true;
    };

    // ---- NativeWindow-Ready-Signal-Plan step 5 overrides ----

    bool WinAppWindow::isNativeReady() const {
        return nativeReady_.load(std::memory_order_acquire);
    }

    void WinAppWindow::onFirstRealize(std::function<void()> cb){
        if(!cb){
            return;
        }
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            if(!firstRealizeFired_){
                firstRealizeSubscribers_.push_back(std::move(cb));
                return;
            }
        }
        // Post-realize fast path: fire synchronously. cb was not moved
        // on this path because the if-branch returned early.
        cb();
    }

    void WinAppWindow::onRealize(std::function<void()> cb){
        if(!cb){
            return;
        }
        // Sticky semantics: no synchronous-replay path. The callback
        // fires only on future re-realize transitions (WM_DPICHANGED /
        // WM_DISPLAYCHANGE → handleReRealize).
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        realizeSubscribers_.push_back(std::move(cb));
    }

    void WinAppWindow::handleFirstRealize(){
        std::vector<std::function<void()>> firstCallbacks;
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            nativeReady_.store(true, std::memory_order_release);
            if(firstRealizeFired_){
                return;  // idempotent — initialDisplay / WM_SHOWWINDOW / WM_PAINT may all call
            }
            firstRealizeFired_ = true;
            // Drain + free storage: firstRealize subscribers fire exactly
            // once per NativeWindow lifetime.
            firstCallbacks.swap(firstRealizeSubscribers_);
        }
        for(auto & cb : firstCallbacks){
            if(cb) cb();
        }
    }

    void WinAppWindow::requestFrameFlush(){
        // Dedup a burst of requests into one PostMessage. The wndproc
        // (see `customFlushFrameMessage()` branch in ProcessWndMsgImpl)
        // resets the flag before firing `frameFlushCallback_`, so a
        // re-entrant `requestFrame` from inside the flush can enqueue
        // the next frame.
        bool expected = false;
        if(!frameFlushQueued_.compare_exchange_strong(expected, true)){
            return;
        }
        if(hwnd == nullptr ||
           PostMessageA(hwnd, customFlushFrameMessage(), 0, 0) == FALSE){
            // PostMessage failure (no hwnd, or message queue full /
            // refusing) — release the flag so a later request can try
            // again. Treat as a missed frame, not a hang.
            frameFlushQueued_.store(false, std::memory_order_release);
        }
    }

    void WinAppWindow::handleReRealize(){
        std::vector<std::function<void()>> reCallbacks;
        {
            std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
            // Sticky: copy the subscriber list so it stays intact for
            // subsequent fires. WM_DPICHANGED / WM_DISPLAYCHANGE don't
            // discard the swap chain on Win32; nativeReady_ stays true.
            reCallbacks = realizeSubscribers_;
        }
        for(auto & cb : reCallbacks){
            if(cb) cb();
        }
    }
    WinAppWindow::~WinAppWindow(){
        close();
    };

    void WinAppWindow::minimize(){
        ShowWindow(hwnd, SW_MINIMIZE);
    }
    void WinAppWindow::maximize(){
        ShowWindow(hwnd, SW_MAXIMIZE);
    }
    void WinAppWindow::restore(){
        ShowWindow(hwnd, SW_RESTORE);
    }
    void WinAppWindow::toggleFullscreen(){
        if(!isFullscreen_){
            savedStyle_ = GetWindowLongPtr(hwnd, GWL_STYLE);
            savedPlacement_.length = sizeof(WINDOWPLACEMENT);
            GetWindowPlacement(hwnd, &savedPlacement_);
            MONITORINFO mi { sizeof(MONITORINFO) };
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
            if(GetMonitorInfo(mon, &mi)){
                SetWindowLongPtr(hwnd, GWL_STYLE, savedStyle_ & ~WS_OVERLAPPEDWINDOW);
                SetWindowPos(hwnd, HWND_TOP,
                             mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top,
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                isFullscreen_ = true;
            }
        } else {
            SetWindowLongPtr(hwnd, GWL_STYLE, savedStyle_);
            SetWindowPlacement(hwnd, &savedPlacement_);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            isFullscreen_ = false;
        }
    }
    bool WinAppWindow::isMinimized() const {
        return IsIconic(hwnd) != FALSE;
    }
    bool WinAppWindow::isMaximized() const {
        return IsZoomed(hwnd) != FALSE;
    }
    bool WinAppWindow::isFullscreen() const {
        return isFullscreen_;
    }
    bool WinAppWindow::isVisible() const {
        return IsWindowVisible(hwnd) != FALSE;
    }
    Composition::Rect WinAppWindow::getRect() const {
        RECT r;
        if(!GetWindowRect(hwnd, &r)){
            return wndrect;
        }
        FLOAT scale = FLOAT(currentDpi)/96.f;
        return Composition::Rect{
            Composition::Point2D{(float)r.left/scale,(float)r.top/scale},
            (float)(r.right - r.left)/scale,
            (float)(r.bottom - r.top)/scale
        };
    }
    void WinAppWindow::setRect(const Composition::Rect & r){
        FLOAT scale = FLOAT(currentDpi)/96.f;
        SetWindowPos(hwnd, nullptr,
                     int(r.pos.x * scale), int(r.pos.y * scale),
                     int(r.w * scale), int(r.h * scale),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        wndrect = r;
        rect = r;
    }
    // §2.9: scaleFactor() now lives at NativeWindow and forwards to
    // currentScreen().scaleFactor — no per-backend override.
    void WinAppWindow::setMinSize(float w, float h){
        minSize_ = Composition::Point2D{w, h};
    }
    void WinAppWindow::setMaxSize(float w, float h){
        maxSize_ = Composition::Point2D{w, h};
    }
    void WinAppWindow::setResizable(bool resizable){
        resizable_ = resizable;
        DWORD style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if(resizable){
            style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
        } else {
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    void WinAppWindow::orderFront(){
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    void WinAppWindow::orderBack(){
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    void WinAppWindow::setOpacity(float alpha){
        if(alpha < 0.f) alpha = 0.f;
        if(alpha > 1.f) alpha = 1.f;
        opacityByte_ = BYTE(alpha * 255.f);
        SetLayeredWindowAttributes(hwnd, 0, opacityByte_, LWA_ALPHA);
    }
    float WinAppWindow::getOpacity() const {
        return float(opacityByte_) / 255.f;
    }
    void WinAppWindow::setCursorShape(CursorShape shape){
        currentCursorShape_ = shape;
        LPCSTR cursorId = IDC_ARROW;
        switch(shape){
            case CursorShape::Arrow:           cursorId = IDC_ARROW; break;
            case CursorShape::IBeam:           cursorId = IDC_IBEAM; break;
            case CursorShape::Crosshair:       cursorId = IDC_CROSS; break;
            case CursorShape::PointingHand:    cursorId = IDC_HAND;  break;
            case CursorShape::ResizeLeftRight: cursorId = IDC_SIZEWE; break;
            case CursorShape::ResizeUpDown:    cursorId = IDC_SIZENS; break;
            case CursorShape::ResizeAll:       cursorId = IDC_SIZEALL; break;
            case CursorShape::NotAllowed:      cursorId = IDC_NO;    break;
            case CursorShape::Wait:            cursorId = IDC_WAIT;  break;
            case CursorShape::Custom:          cursorId = IDC_ARROW; break;
        }
        currentCursor_ = LoadCursor(nullptr, cursorId);
        SetCursor(currentCursor_);
    }
    bool WinAppWindow::isKeyWindow() const {
        return GetForegroundWindow() == hwnd;
    }
    void WinAppWindow::becomeKeyWindow(){
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
};

namespace OmegaWTK::Native {
    NWH make_native_window(Composition::Rect &rect, NativeEventEmitter *emitter, const NativeScreenDesc *screen){
        return (NWH)new Win::WinAppWindow(rect,emitter,screen);
    }
    
}

