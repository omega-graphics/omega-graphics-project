#include "omegaWTK/Native/NativeItem.h"
#include "omegaWTK/Native/NativeEvent.h"
#include <Windows.h>

#ifndef OMEGAWTK_NATIVE_WIN_HWNDITEM_H
#define OMEGAWTK_NATIVE_WIN_HWNDITEM_H
namespace OmegaWTK::Native {
    namespace Win {
        class HWNDFactory;
        class HWNDItem : public NativeItem {
            public:
            HWND hwnd;
            bool enabled = false;
            bool isScrollView;
            protected:
            HWNDItem *parent;
            OmegaCommon::Vector<HWND> raw_children;
            OmegaCommon::Vector<SharedHandle<HWNDItem>> children;
            UINT currentDpi;
            bool isTracking;
            bool hovered;
            /// E2 native follow-up: true while this window holds an app-driven
            /// OS pointer grab (SetCapture). Distinguishes a voluntary
            /// ReleaseCapture (drag finished) from an involuntary capture loss
            /// (Alt-Tab, a system dialog) in the WM_CAPTURECHANGED handler.
            bool pointerCaptured_ = false;
            virtual LRESULT ProcessWndMsg(UINT,WPARAM,LPARAM);
            virtual BOOL ProcessWndMsgImpl(HWND,UINT,WPARAM,LPARAM,LRESULT *);
            ATOM atom;
            void emitIfPossible(NativeEventPtr event);
            friend class HWNDFactory;
            void enable() override{
                enabled = true;
                ShowWindow(hwnd,SW_SHOW);
            };
            void disable() override{
                enabled = false;
                ShowWindow(hwnd,SW_HIDE);
            };
            void resize(const Composition::Rect &newRect) override;
            void addChildNativeItem(NativeItemPtr nativeItem) override;
            void removeChildNativeItem(NativeItemPtr nativeItem) override;
            public:
             /**
            Constructs a null HWNDItem! (Sets the Composition::Rect only)
            */
            HWNDItem(Composition::Rect & rect);
            Composition::Rect wndrect;
            Composition::Rect & getRect() override {
                return wndrect;
            }
            ATOM getAtom();
            HWND getHandle();
            bool isExtended();
            DWORD getStyle();
            DWORD getExtendedStyle();
            // void show(int nCmdShow);
            // void update();
            void destroy();
            RECT getClientRect();
            HDC getDCFromHWND();
            void *getBinding() override{ return (void *)hwnd;};
            /// ScrollView-Interaction-Enhancements-Plan (E2 native follow-up)
            /// — install/remove an OS pointer grab so a thumb drag keeps
            /// tracking once the cursor leaves the window (Windows stops
            /// WM_MOUSEMOVE at the window edge). See HWNDItem.cpp.
            void setPointerCapture(bool capture) override;
            /// @name ScrollView Methods
            /// @{
            void toggleHorizontalScrollBar(bool & state) override;
            void toggleVerticalScrollBar(bool & state) override;
            void setClippedView(SharedHandle<NativeItem> clippedView) override;
            /// @}
            typedef enum : OPT_PARAM {
                View,
                ScrollView
            } Type;
            private:
            Type type;
            public:
             /**
            Constructs/Registers an HWND and returns an HWNDItem!
            */
            HWNDItem(Composition::Rect & rect,Type _type,HWNDItem *parent);
            ~HWNDItem(){
                if(IsWindow(hwnd)){
                    DestroyWindow(hwnd);
                }
            };
        };
    };
};

#endif