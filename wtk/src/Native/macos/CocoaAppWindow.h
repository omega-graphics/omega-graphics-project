#include "omegaWTK/Native/NativeWindow.h"
#import <Cocoa/Cocoa.h>
#include "NativePrivate/macos/CocoaItem.h"


#ifndef OMEGAWTK_NATIVE_COCOA_COCOAAPPWINDOW_H
#define OMEGAWTK_NATIVE_COCOA_COCOAAPPWINDOW_H
@class OmegaWTKNativeCocoaAppWindowDelegate;
@class OmegaWTKNativeCocoaAppWindowController;

namespace OmegaWTK::Native::Cocoa {
class CocoaAppWindow : public NativeWindow {
    SharedHandle<CocoaItem> rootView;
    OmegaWTKNativeCocoaAppWindowController *windowController;
    OmegaWTKNativeCocoaAppWindowDelegate *windowDelegate;
    NSCursor *currentNSCursor;
    CursorShape currentCursorShape = CursorShape::Arrow;
    float lastKnownBackingScale = 1.f;
    /// Widget-View-Paint-Lifecycle-Plan Tier A: guards a single
    /// pending CFRunLoopPerformBlock so a burst of requestFrameFlush
    /// calls collapses into one frame flush per run-loop turn.
    bool frameFlushQueued_ = false;
public:
    void requestFrameFlush() override;
    NativeEventEmitter *getEmitter();
    NativeItemPtr getRootView() override;
    void disable() override;
    void enable() override;
    void initialDisplay() override;
    void close() override;
    void setMenu(NM menu) override;
    void setTitle(OmegaCommon::StrRef title) override;
    void setEnableWindowHeader(bool & enable) override;

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

    /// Emit WindowScaleFactorChanged on backing-scale change.
    void notifyBackingScaleChanged(float oldScale, float newScale);
    /// Apply the current cursor shape to the OS — invoked from
    /// NSWindow's cursor-update tracking.
    void applyCursor();

    __strong NSWindow *getWindow();
    CocoaAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter);
};
};

#endif
