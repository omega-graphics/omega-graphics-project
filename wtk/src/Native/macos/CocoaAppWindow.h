#include "omegaWTK/Native/NativeWindow.h"
#import <Cocoa/Cocoa.h>
#include "NativePrivate/macos/CocoaItem.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

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

    // NativeWindow-Ready-Signal-Plan step 4: macOS realize-gate state.
    // nativeReady_ is the atomic boolean isNativeReady() returns; read
    // on the CompositorFrameWorker thread, written on the AppKit main
    // thread. The two subscriber vectors back onFirstRealize /
    // onRealize and are guarded by realizeCallbacksMutex_.
    // firstRealizeFired_ tracks the singleton transition.
    std::atomic<bool> nativeReady_ {false};
    mutable std::mutex realizeCallbacksMutex_;
    std::vector<std::function<void()>> firstRealizeSubscribers_;
    std::vector<std::function<void()>> realizeSubscribers_;
    bool firstRealizeFired_ = false;
public:
    void requestFrameFlush() override;
    bool isNativeReady() const override;
    void onFirstRealize(std::function<void()> cb) override;
    void onRealize(std::function<void()> cb) override;

    // Internal: invoked from the AppKit "show window" entry points
    // (initialDisplay / enable / orderFront) and from the delegate's
    // re-realize triggers. handleFirstRealize is idempotent — only
    // the first call drains the firstRealize list; subsequent calls
    // no-op. handleReRealize fires the sticky realize list every
    // time, treating the surface as transiently re-realized.
    void handleFirstRealize();
    void handleReRealize();
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
    // §2.9: scaleFactor() is now a base-class forwarder to
    // currentScreen().scaleFactor. The backing-scale-change emit path
    // (-windowDidChangeBackingProperties:) stays here unchanged.
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
    CocoaAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter,const NativeScreenDesc *screen);
};
};

#endif
