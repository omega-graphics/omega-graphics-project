#include "WinAppWindow.h"
#include "HWNDFactory.h"
#include "WinMenu.h"

#include <dwmapi.h>
#include <memory>
#include <windowsx.h>

#pragma comment(lib,"dwmapi.lib")

namespace OmegaWTK::Native::Win {
    WinAppWindow::WinAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter):
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
        currentDpi = GetDpiForWindow(hwnd);
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
                case WM_PAINT : {
                    PAINTSTRUCT ps;
                    HDC hdc = BeginPaint(hwnd,&ps);

                    // parentLayer->redraw();

                    EndPaint(hwnd,&ps);
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
                    return FALSE;
                    break;
                }
        return TRUE;
    };

    void WinAppWindow::attachWidgets(){
       if(menu) {
           auto hmenu = (HMENU)menu->getNativeBinding();
            if(SetMenu(hwnd,hmenu) == FALSE){
                OMEGAWTK_DEBUG("Failed to Attach Menu to AppWindow");
            };
       };
       isReady = true;
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
    };
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
    float WinAppWindow::scaleFactor() const {
        return float(currentDpi) / 96.f;
    }
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
    NWH make_native_window(Composition::Rect &rect, NativeEventEmitter *emitter){
        return (NWH)new Win::WinAppWindow(rect,emitter);
    }
    
}

