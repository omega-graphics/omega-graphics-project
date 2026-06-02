#include "NativePrivate/win/HWNDItem.h"
#include "omegaWTK/Native/NativeWindow.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifndef OMEGAWTK_NATIVE_WIN_WINAPPWINDOW_H
#define OMEGAWTK_NATIVE_WIN_WINAPPWINDOW_H

namespace OmegaWTK::Native::Win {
    class WinAppWindow : public NativeWindow,
                         public HWNDItem,
                         public std::enable_shared_from_this<WinAppWindow> {
        // `isReady` is the pre-existing event-emit gate (set to true in
        // attachWidgets, consumed by WM_DESTROY / WM_SIZE handlers). It
        // is NOT the same as the NativeWindow-Ready-Signal-Plan
        // isNativeReady() predicate, which has its own atomic
        // (nativeReady_) below — keeping them distinct so the
        // event-emit gate doesn't accidentally gate render dispatch
        // and vice versa.
        bool isReady;
        bool resizable_ = true;
        bool isFullscreen_ = false;
        DWORD savedStyle_ = 0;
        WINDOWPLACEMENT savedPlacement_ {};
        BYTE opacityByte_ = 255;
        HCURSOR currentCursor_ = nullptr;
        CursorShape currentCursorShape_ = CursorShape::Arrow;
        Composition::Point2D minSize_ {0.f, 0.f};
        Composition::Point2D maxSize_ {0.f, 0.f};

        // NativeWindow-Ready-Signal-Plan step 5: Win32 realize-gate
        // state. nativeReady_ is the atomic isNativeReady() returns;
        // read on the CompositorFrameWorker thread, written on the
        // GUI thread (wndproc). The two subscriber vectors back
        // onFirstRealize / onRealize and are guarded by
        // realizeCallbacksMutex_. firstRealizeFired_ tracks the
        // singleton transition that distinguishes initial realize
        // from subsequent re-realize cycles.
        std::atomic<bool> nativeReady_ {false};
        mutable std::mutex realizeCallbacksMutex_;
        std::vector<std::function<void()>> firstRealizeSubscribers_;
        std::vector<std::function<void()>> realizeSubscribers_;
        bool firstRealizeFired_ = false;
        public:
        NativeItemPtr getRootView() override;

        void enable() override;

        void disable() override;

        void attachWidgets();

        void initialDisplay() override;

        void close() override;

        void setTitle(OmegaCommon::StrRef title) override;

        void setEnableWindowHeader(bool &enable) override;

        void setMenu(NM menu) override;

        void minimize() override;
        void maximize() override;
        void restore() override;
        void toggleFullscreen() override;
        bool isMinimized() const override;
        bool isMaximized() const override;
        bool isFullscreen() const override;
        bool isVisible() const override;
        Composition::Rect getRect() const override;
        void setRect(const Composition::Rect & r) override;
        float scaleFactor() const override;
        void setMinSize(float w, float h) override;
        void setMaxSize(float w, float h) override;
        void setResizable(bool resizable) override;
        void orderFront() override;
        void orderBack() override;
        void setOpacity(float alpha) override;
        float getOpacity() const override;
        void setCursorShape(CursorShape shape) override;
        bool isKeyWindow() const override;
        void becomeKeyWindow() override;

        LRESULT ProcessWndMsg(UINT,WPARAM,LPARAM) override;
        BOOL ProcessWndMsgImpl(HWND, UINT, WPARAM, LPARAM, LRESULT *) override;

        // NativeWindow-Ready-Signal-Plan step 5 overrides
        bool isNativeReady() const override;
        void onFirstRealize(std::function<void()> cb) override;
        void onRealize(std::function<void()> cb) override;

        // Internal: invoked from the wndproc (WM_SHOWWINDOW/WM_PAINT
        // for first-realize; WM_DPICHANGED/WM_DISPLAYCHANGE for
        // re-realize) and from initialDisplay(). handleFirstRealize
        // is idempotent — only the first call drains the firstRealize
        // list; subsequent calls no-op. handleReRealize fires the
        // sticky realize list every time.
        void handleFirstRealize();
        void handleReRealize();

        WinAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter);
        ~WinAppWindow();
    };
};

#endif
