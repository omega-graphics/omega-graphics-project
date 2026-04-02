# Canvas-Layer Exclusivity and PaintContext Scope

## Bug: SVGViewRenderTest shows white, no SVG

### Trace

`Widget::executePaint` runs on the SVGWidget. The SVGWidget's root view IS the SVGView (passed directly as the Widget's ViewPtr via `SVGView::Create`).

```
Widget::executePaint
  getRootPaintCanvas()
    view->getLayerTree()->getRootLayer()       // the SVGView's root layer
    view->makeCanvas(rootLayer)                // Canvas A — targets root layer
  PaintContext context(this, canvasA, reason)
  view->startCompositionSession()              // SVGView proxy: depth 0→1
    onPaint(context, reason)
      context.clear(White)                     // writes white background to Canvas A's frame
      svgView->renderNow()
        startCompositionSession()              // depth 1→2 (nested, no submit)
          draw rect, circle, ellipse...        // all drawn to svgCanvas (Canvas B)
          svgCanvas->sendFrame()               // queues SVG frame (Canvas B → root layer)
        endCompositionSession()                // depth 2→1 (no submit)
    canvas->sendFrame()                        // queues white frame (Canvas A → root layer)
  view->endCompositionSession()                // depth 1→0 → SUBMIT
```

The packet contains two frames for the same root layer:
1. SVG frame (from `svgCanvas->sendFrame()`)
2. White frame (from `canvas->sendFrame()`)

Both `svgCanvas` (created at SVGView.cpp:444) and `rootPaintCanvas` (created at Widget.cpp:175) target the same `getLayerTree()->getRootLayer()`. The white frame arrives last and overwrites the SVG content. Result: white screen.

## Root cause

Two Canvases rendering to the same Layer. `Widget::executePaint` always creates a `rootPaintCanvas` on the root layer and always calls `sendFrame()` on it, regardless of whether the View is a plain CanvasView, an SVGView, a UIView, or a VideoView. The specialized views have their own Canvases already bound to that same root layer.

### Where each Canvas is created

| Canvas | Created at | Targets |
|--------|-----------|---------|
| `Widget::rootPaintCanvas` | Widget.cpp:175 `getRootPaintCanvas()` | `view->getLayerTree()->getRootLayer()` |
| `SVGView::svgCanvas` | SVGView.cpp:444 constructor | `getLayerTree()->getRootLayer()` |
| `UIView::rootCanvas` | UIView constructor | `getLayerTree()->getRootLayer()` |
| `VideoView::videoCanvas` | VideoView constructor | `getLayerTree()->getRootLayer()` |

Every specialized View creates its own Canvas on the root layer at construction. Then `Widget::executePaint` creates a second Canvas on the same layer. Two Canvases, one Layer, last frame wins.

## Principle: One Canvas per Layer

A Layer is the atomic rendering surface. It maps to one `BackendRenderTargetContext` with one GPU texture. When two Canvases both call `sendFrame()` targeting the same Layer, the compositor receives two `CompositionRenderCommand`s for the same render target context. The last one processed overwrites the first. There is no blending or compositing between frames on the same Layer -- the frame IS the layer content.

This should be a structural invariant, not a convention that callers must remember.

## Principle: PaintContext is for CanvasView widgets only

`PaintContext` wraps `rootPaintCanvas`. It provides `clear()`, `drawRect()`, `drawText()`, etc. -- direct Canvas drawing operations. These make sense when the Widget's View is a plain CanvasView (no specialized rendering system).

For widgets backed by specialized Views:
- **SVGView** has its own drawing system (`renderNow()` with `svgCanvas`)
- **UIView** has its own drawing system (`update()` with layout + stylesheet + element layers)
- **VideoView** has its own drawing system (`presentCurrentFrame()` with `videoCanvas`)

Handing these widgets a PaintContext that wraps a second Canvas on their root layer is actively harmful -- it creates the dual-Canvas bug. PaintContext should not exist for widgets whose View already owns its rendering.

## Design

### A. Layer enforces single-Canvas ownership

The Layer class tracks whether a Canvas is already bound to it. Creating a second Canvas on the same Layer is an error.

```cpp
class Layer {
    Canvas * boundCanvas_ = nullptr;   // non-owning; set by Canvas constructor
    friend class Canvas;
public:
    bool hasCanvas() const { return boundCanvas_ != nullptr; }
};
```

Canvas constructor asserts:
```cpp
Canvas::Canvas(CompositorClientProxy & proxy, Layer & layer)
    : CompositorClient(proxy), layer(layer), rect(layer.getLayerRect()) {
    assert(layer.boundCanvas_ == nullptr &&
           "Layer already has a Canvas bound -- one Canvas per Layer");
    layer.boundCanvas_ = this;
    ...
}
```

This makes the invariant structural. The SVGViewRenderTest would assert at `getRootPaintCanvas()` when it tries to create a second Canvas on the root layer that SVGView already claimed.

### B. Widget::executePaint skips rootPaintCanvas for specialized Views

`executePaint` should not create a `rootPaintCanvas` or call `sendFrame()` on it when the View already owns its rendering. The View knows whether it owns its root layer rendering:

```cpp
class View {
public:
    /// Returns true if this View's root layer already has a Canvas
    /// (specialized Views create their own at construction).
    /// When true, Widget::executePaint must not create a rootPaintCanvas.
    bool rootLayerOwnedByView() const {
        auto * root = ownLayerTree ? ownLayerTree->getRootLayer().get() : nullptr;
        return root != nullptr && root->hasCanvas();
    }
};
```

`executePaint` becomes:

```cpp
void Widget::executePaint(PaintReason reason, bool immediate) {
    ...
    bool viewOwnsRootLayer = view->rootLayerOwnedByView();
    PaintReason activeReason = reason;
    while(true) {
        SharedHandle<Composition::Canvas> canvas = nullptr;
        if(!viewOwnsRootLayer) {
            canvas = getRootPaintCanvas();
        }
        PaintContext context(this, canvas, activeReason);
        view->startCompositionSession();
        onPaint(context, activeReason);
        if(canvas != nullptr) {
            int submissions = 1;
            if(activeReason == PaintReason::Initial &&
               !initialDrawComplete &&
               options.autoWarmupOnInitialPaint) {
                submissions = std::max<int>(1, options.warmupFrameCount);
            }
            for(int i = 0; i < submissions; i++) {
                canvas->sendFrame();
            }
        }
        view->endCompositionSession();
        ...
    }
    ...
}
```

### C. PaintContext becomes a no-op when canvas is null

When the View owns its root layer, PaintContext is constructed with a null canvas. Drawing operations become no-ops:

```cpp
void PaintContext::clear(const Composition::Color & color) {
    if(mainCanvas == nullptr) return;
    auto & background = rootCanvas().getCurrentFrame()->background;
    ...
}

void PaintContext::drawRect(const Core::Rect & rect, const SharedHandle<Composition::Brush> & brush) {
    if(mainCanvas == nullptr) return;
    ...
}

// Same for drawRoundedRect, drawImage, drawText, etc.
```

This means for SVGWidget's `onPaint`:
```cpp
void onPaint(PaintContext & context, PaintReason reason) override {
    context.clear(White);      // no-op -- SVGView owns the root layer
    svgView->renderNow();      // SVG renders through its own Canvas -- works
}
```

The `context.clear(White)` silently does nothing. The SVGView's `renderNow()` is the only thing that writes to the root layer. No dual-Canvas conflict.

### D. PaintContext::rootCanvas() asserts on null

For debugging: if user code on a specialized-view widget tries to use the canvas (not just `clear`), it should fail loudly rather than silently:

```cpp
Composition::Canvas & PaintContext::rootCanvas() {
    assert(mainCanvas != nullptr &&
           "PaintContext has no canvas -- this Widget's View owns its own rendering. "
           "Use the View's drawing API (renderNow, update, etc.) instead.");
    return *mainCanvas.get();
}
```

`clear()`, `drawRect()`, etc. are safe no-ops (they return early). But `rootCanvas()` direct access asserts, since the caller is trying to get a raw Canvas handle to a layer they don't own.

## SVGViewRenderTest: what changes

The test code itself doesn't change:

```cpp
void onPaint(PaintContext & context, PaintReason reason) override {
    context.clear(White);           // becomes no-op (SVGView owns root layer)
    svgView->renderNow();           // SVG renders normally
}
```

If the SVG needs a white background, it should be set through SVGView's rendering options or as the first draw op in the SVG document (a full-size white rect). The View owns its background, not PaintContext.

Alternatively, SVGView could expose a `setBackgroundColor()` method that sets the background on its own Canvas's frame -- no second Canvas needed.

## File Change Summary

| File | Change |
|------|--------|
| `Layer.h` | Add `Canvas * boundCanvas_` tracking pointer. Add `hasCanvas()` accessor. |
| `Canvas.h` / `Canvas.cpp` | Canvas constructor sets `layer.boundCanvas_`. Destructor clears it. Assert on double-bind. |
| `View.h` / `View.cpp` | Add `rootLayerOwnedByView()` query method. |
| `Widget.cpp` | `executePaint` checks `rootLayerOwnedByView()`. Skips `rootPaintCanvas` creation and `sendFrame` when true. |
| `Widget.cpp` | `PaintContext::clear`, `drawRect`, `drawRoundedRect`, `drawImage`, `drawText` -- early return when `mainCanvas` is null. |
| `Widget.cpp` | `PaintContext::rootCanvas()` -- assert when null. |

No changes to SVGView, UIView, VideoView, or any test code. The bug is fixed structurally at the Widget/Layer level.

## Option E: CanvasView subclass, remove PaintContext entirely

The sections above (A-D) patch around the problem: PaintContext exists but becomes a no-op for specialized views. Option E goes further -- remove PaintContext from Widget entirely and make canvas drawing a CanvasView concern.

### The idea

`View` is currently aliased as `CanvasView` (`typedef View CanvasView`), but the alias is meaningless -- there is no CanvasView behavior distinct from View. The proposal: make CanvasView a real subclass of View that owns the root-layer Canvas and the drawing API that PaintContext currently provides.

```
View  (base -- no Canvas, no drawing API)
 ├── CanvasView   (owns root-layer Canvas, provides clear/drawRect/drawText/...)
 ├── UIView       (owns root-layer Canvas via its own system, layout + stylesheet)
 ├── SVGView      (owns root-layer Canvas via svgCanvas, SVG rendering)
 └── VideoView    (owns root-layer Canvas via videoCanvas, video playback)
```

### What changes

**View (base class):**
- No longer creates or owns a Canvas on the root layer.
- No `rootPaintCanvas`. No drawing API. Pure rendering surface: native item, render target, proxy, layer tree, subviews, resize coordinator.

**CanvasView (new real subclass of View):**
- Creates a Canvas on the root layer at construction (what `getRootPaintCanvas()` does today, but once, at the right time).
- Exposes the drawing API directly: `clear()`, `drawRect()`, `drawRoundedRect()`, `drawEllipse()`, `drawImage()`, `drawText()`.
- This is the View type that simple canvas-drawing widgets use.

```cpp
class CanvasView : public View {
    SharedHandle<Composition::Canvas> rootCanvas_;
public:
    CanvasView(const Core::Rect & rect, ViewPtr parent = nullptr);

    Composition::Canvas & rootCanvas();
    SharedHandle<Composition::Canvas> makeLayerCanvas(SharedHandle<Composition::Layer> & layer);

    void clear(const Composition::Color & color);
    void drawRect(const Core::Rect & rect, const SharedHandle<Composition::Brush> & brush);
    void drawRoundedRect(const Core::RoundedRect & rect, const SharedHandle<Composition::Brush> & brush);
    void drawEllipse(const Core::Ellipse & ellipse, const SharedHandle<Composition::Brush> & brush);
    void drawImage(const SharedHandle<Media::BitmapImage> & img, const Core::Rect & rect);
    void drawText(const UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Core::Rect & rect,
                  const Composition::Color & color,
                  const Composition::TextLayoutDescriptor & layoutDesc);
    void drawText(const UniString & text,
                  const SharedHandle<Composition::Font> & font,
                  const Core::Rect & rect,
                  const Composition::Color & color);
};
```

**PaintContext: removed.**

PaintContext was a thin wrapper around Canvas that added nothing except indirection. Every method on PaintContext (`clear`, `drawRect`, `drawText`, etc.) just forwarded to `rootCanvas()`. With CanvasView owning those methods directly, PaintContext has no reason to exist.

**Widget::onPaint signature changes:**

```cpp
// Before:
virtual void onPaint(PaintContext & context, PaintReason reason);

// After:
virtual void onPaint(PaintReason reason);
```

No context parameter. The widget accesses its view directly. For canvas widgets, that view is a CanvasView with drawing methods. For specialized-view widgets, the view is SVGView/UIView/VideoView with their own APIs.

**Widget::executePaint changes:**

```cpp
void Widget::executePaint(PaintReason reason, bool immediate) {
    ...
    view->startCompositionSession();
    onPaint(activeReason);

    // Only CanvasView needs sendFrame -- specialized views
    // send their own frames in their own rendering methods.
    if(auto * cv = dynamic_cast<CanvasView *>(view.get())) {
        int submissions = 1;
        if(activeReason == PaintReason::Initial &&
           !initialDrawComplete &&
           options.autoWarmupOnInitialPaint) {
            submissions = std::max<int>(1, options.warmupFrameCount);
        }
        for(int i = 0; i < submissions; i++) {
            cv->rootCanvas().sendFrame();
        }
    }

    view->endCompositionSession();
    ...
}
```

Alternatively, avoid the `dynamic_cast` by adding a virtual on View:

```cpp
class View {
public:
    /// Called by executePaint after onPaint. CanvasView sends its root
    /// canvas frame. Specialized views do nothing (they already sent
    /// their frames in their own rendering methods).
    virtual void submitPaintFrame(int submissions) {}
};

class CanvasView : public View {
public:
    void submitPaintFrame(int submissions) override {
        for(int i = 0; i < submissions; i++) {
            rootCanvas_.sendFrame();
        }
    }
};
```

Then `executePaint` just calls `view->submitPaintFrame(submissions)` unconditionally.

### What user code looks like

**Simple canvas widget:**
```cpp
class MyWidget : public Widget {
public:
    MyWidget(Core::Rect rect) : Widget(rect) {}  // creates CanvasView internally
protected:
    void onPaint(PaintReason reason) override {
        auto & cv = viewAs<CanvasView>();
        cv.clear(Color::create8Bit(Color::White8));
        cv.drawRect(rect(), myBrush);
    }
};
```

**SVG widget:**
```cpp
class SVGWidget : public Widget {
public:
    SVGWidget(Core::Rect rect)
        : Widget(ViewPtr(new SVGView(rect, nullptr))) {}
protected:
    SVGView & svgView() { return viewAs<SVGView>(); }
    void onPaint(PaintReason reason) override {
        svgView().renderNow();   // SVGView sends its own frame
    }
};
```

**UIView widget:**
```cpp
class MyUIWidget : public Widget {
    UIViewPtr uiView;
protected:
    void onPaint(PaintReason reason) override {
        uiView->setLayout(layout);
        uiView->setStyleSheet(style);
        uiView->update();        // UIView sends its own frames
    }
};
```

No PaintContext anywhere. Each view type owns its drawing. The Canvas-per-Layer invariant is enforced structurally because only CanvasView creates a Canvas on the root layer, and specialized views create their own -- never both.

### Interaction with Widget-View API Cleanup (Option B+)

This composes cleanly. The `Widget(Core::Rect)` constructor from Option B+ would create a CanvasView instead of a plain View:

```cpp
Widget::Widget(Core::Rect rect)
    : view(SharedHandle<CanvasView>(new CanvasView(rect))) {}
```

The `Widget(ViewPtr)` escape hatch accepts any View subclass (SVGView, VideoView, etc.).

### Migration path

1. Create CanvasView as a real subclass of View (move root-layer Canvas creation there).
2. Move PaintContext's drawing methods onto CanvasView.
3. Change `onPaint` signature to drop PaintContext parameter.
4. Update `executePaint` to use `submitPaintFrame` virtual.
5. Update all Widget subclasses: replace `context.clear(...)` / `context.drawRect(...)` with `viewAs<CanvasView>().clear(...)` / etc.
6. Remove PaintContext class.
7. Remove `Widget::rootPaintCanvas` member and `getRootPaintCanvas()`.
8. Remove the `typedef View CanvasView` alias.

Steps 1-4 are the structural changes. Steps 5-8 are mechanical migration.
