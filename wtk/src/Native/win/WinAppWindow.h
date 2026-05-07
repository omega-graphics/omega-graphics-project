#include "NativePrivate/win/HWNDItem.h"
#include "omegaWTK/Native/NativeWindow.h"
#include <memory>

#ifndef OMEGAWTK_NATIVE_WIN_WINAPPWINDOW_H
#define OMEGAWTK_NATIVE_WIN_WINAPPWINDOW_H

namespace OmegaWTK::Native::Win {
    class WinAppWindow : public NativeWindow,
                         public HWNDItem,
                         public std::enable_shared_from_this<WinAppWindow> {
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
        WinAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter);
        ~WinAppWindow();
    };
};

#endif
