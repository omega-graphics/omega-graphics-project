# Widget-View API Cleanup Plan

## Problem

The current Widget API leaks View construction into user-facing code. Every custom Widget subclass must create a View externally and pass it in, even though the Widget-View relationship is 1:1 and the View type is usually determined by the Widget subclass itself.

### Current user-facing patterns and their problems

**Pattern 1 -- Simple canvas widget (most common):**
```cpp
class MyWidget : public Widget {
public:
    explicit MyWidget(ViewPtr view) : Widget(std::move(view)) {}
};
auto w = make<MyWidget>(View::Create(rect));
```
User creates a View just to hand it in. View is an implementation detail leaking into construction.

**Pattern 2 -- Widget with UIView subview:**
```cpp
class RoundedFrameWidget : public Widget {
    UIViewPtr uiView;
    void ensureUIView(const Composition::Rect & bounds) {
        uiView = UIViewPtr(new UIView(localBounds, view, "tag"));
        //                                         ^^^^
        //     Widget's protected `view` member leaks as parent_ptr
    }
};
```

**Pattern 3 -- Specialized view widget:**
```cpp
class SVGWidget : public Widget {
    SVGViewPtr svgView;
public:
    SVGWidget(ViewPtr view, Composition::Rect rect) : Widget(view) {
        svgView = std::dynamic_pointer_cast<SVGView>(view);
        // Cast back to recover the type you already had
    }
};
auto w = make<SVGWidget>(ViewPtr(new SVGView(rect, nullptr)), rect);
```
Type information is created, erased to ViewPtr, then recovered via dynamic_cast. The rect is passed twice.

**Pattern 4 -- Container:**
```cpp
auto stack = make<GeometryHStack>(View::Create(rect), options);
```
Same external View creation ceremony.

## Alternatives considered

### Widget subclasses View (Widget IS-A View)

Eliminates ViewPtr plumbing, but creates a diamond problem. View is subclassed by rendering strategy (UIView, SVGView, VideoView). Widget is subclassed by semantic role (Container, StackWidget, Button). These are orthogonal axes. A Container that wants UIView rendering becomes `class UIContainer : public Container, public UIView` -- diamond inheritance.

Deeper issue: widgets have internal subviews that are not child widgets (e.g. TextCompositorWidget's accentView). If Widget IS View, these internal rendering surfaces can't be Widgets (no Container parent) and you'd reinvent a lightweight View to serve as internal surfaces -- which is what View already is.

### Merge View and Widget into one class

One class with 35+ members spanning two domains: rendering surface (renderTarget, proxy, layerTree, subviews, resizeCoordinator) AND UI lifecycle (paintMode, layoutStyle, observers, treeHost, geometry). Same diamond problem. Container's `children` (widget tree) and View's `subviews` (native rendering hierarchy) become confusingly co-located.

## Design (Option B+)

Keep the composition relationship. Hide View from the surface API. Three changes.

### 1. `Widget(Composition::Rect)` primary constructor

Widget creates its own View internally. The existing `Widget(ViewPtr)` becomes a protected escape hatch for framework internals and specialized-view subclasses.

```cpp
class Widget {
protected:
    // Primary constructor -- creates a plain View internally.
    explicit Widget(Composition::Rect rect);

    // Escape hatch for subclasses that need a specific View type.
    explicit Widget(ViewPtr view);
};
```

User code becomes:
```cpp
class MyWidget : public Widget {
public:
    MyWidget(Composition::Rect rect) : Widget(rect) {}
    void onPaint(PaintContext & ctx, PaintReason reason) override {
        ctx.clear(Color::create8Bit(Color::White8));
    }
};

auto w = make<MyWidget>(Composition::Rect{{0,0},500,500});
```

### 2. Protected `makeSubView<T>()` helper

Eliminates the parent_ptr leak when creating internal UIViews or other subviews.

```cpp
class Widget {
protected:
    /// Create a subview of type T, automatically wired to this widget's
    /// root view as its parent.  Forwards all remaining arguments to T's
    /// constructor after inserting the parent ViewPtr.
    template<typename T, typename... Args>
    SharedHandle<T> makeSubView(Args&&... args);
};
```

The implementation inserts `view` (the widget's own root ViewPtr) as the parent argument so the caller never touches it:

```cpp
template<typename T, typename... Args>
SharedHandle<T> Widget::makeSubView(Args&&... args) {
    return SharedHandle<T>(new T(std::forward<Args>(args)..., view));
}
```

Note: this assumes the View subclass constructors accept `ViewPtr parent` as the last parameter, which UIView, SVGView, and VideoView already do.

User code becomes:
```cpp
// Before:
uiView = UIViewPtr(new UIView(bounds, view, "tag"));

// After:
uiView = makeSubView<UIView>(bounds, "tag");
```

### 3. `viewAs<T>()` typed accessor

For subclasses that create a specialized View in their constructor, provides typed access without dynamic_cast:

```cpp
class Widget {
protected:
    /// Returns a typed reference to this widget's root view.
    /// The caller is responsible for ensuring the actual View type
    /// matches T (guaranteed when the subclass constructor created it).
    template<typename T>
    T & viewAs() { return static_cast<T&>(*view); }

    template<typename T>
    const T & viewAs() const { return static_cast<const T&>(*view); }
};
```

Specialized-view subclasses encapsulate their View type internally:

```cpp
class SVGWidget : public Widget {
public:
    SVGWidget(Composition::Rect rect)
        : Widget(ViewPtr(new SVGView(rect, nullptr))) {}

protected:
    SVGView & svgView() { return viewAs<SVGView>(); }

    void onMount() override {
        svgView().setSourceString(svgString);
    }

    void onPaint(PaintContext & ctx, PaintReason reason) override {
        ctx.clear(Color::create8Bit(Color::White8));
        svgView().renderNow();
    }
};

// Usage -- no ViewPtr, no casting, no duplicate rect:
auto w = make<SVGWidget>(Composition::Rect{{0,0},500,500});
```

## Result

| Pattern | Before | After |
|---------|--------|-------|
| Simple widget | `make<W>(View::Create(r))` | `make<W>(r)` |
| UIView subview creation | `new UIView(b, view, "t")` | `makeSubView<UIView>(b, "t")` |
| SVG widget | `make<SVG>(ViewPtr(new SVGView(r,nil)), r)` | `make<SVG>(r)` |
| Container | `make<Stack>(View::Create(r), opts)` | `make<Stack>(r, opts)` |

## File Change Summary

| File | Change |
|------|--------|
| `Widget.h` | Add `Widget(Composition::Rect)` constructor. Add `makeSubView<T>()` and `viewAs<T>()` protected templates. Existing `Widget(ViewPtr)` stays as protected. |
| `Widget.cpp` | Implement `Widget(Composition::Rect)` constructor: `Widget::Widget(Composition::Rect rect) : view(View::Create(rect)) {}` |
| `BasicWidgets.h` | Update `Container` constructor to add `Container(Composition::Rect)` overload. |
| `BasicWidgets.cpp` | Implement `Container(Composition::Rect)`. |
| `Containers.h` | Update `StackWidget`, `HStack`, `VStack` constructors to accept `Composition::Rect` instead of `ViewPtr`. |
| `Containers.cpp` | Update constructor implementations. |

### Test migration (can be done incrementally)

Each test file replaces `View::Create(rect)` in the constructor call with just `rect`. The `Widget(ViewPtr)` constructor remains available so existing code compiles during migration.

| Test file | Change |
|-----------|--------|
| `BasicAppTest/BasicAppTestRun.cpp` | `MyWidget(Composition::Rect)` constructor, remove `View::Create` at call site |
| `EllipsePathCompositorTest/main.cpp` | `RoundedFrameWidget(Composition::Rect)`, `EllipseOnlyWidget(Composition::Rect)`, `PathOnlyWidget(Composition::Rect)` constructors; `ensureUIView` uses `makeSubView<UIView>`. `GeometryHStack` passes rect to `HStack`. |
| `TextCompositorTest/main.cpp` | `TextCompositorWidget(Composition::Rect)`; `ensureAccentView` uses `makeSubView<UIView>` |
| `SVGViewRenderTest/main.cpp` | `SVGWidget(Composition::Rect)` creates SVGView internally; uses `viewAs<SVGView>()` |
| `VideoViewPlaybackTest/main.cpp` | `VideoWidget(Composition::Rect)` creates VideoView internally; uses `viewAs<VideoView>()` |
| `ContainerClampAnimationTest/main.cpp` | Rect-based constructors |
| `RootWidget/Main.cpp` | Rect-based constructor |

## What stays the same

- View class unchanged -- no new members, no removed members.
- View-Widget 1:1 composition relationship preserved.
- View's subview hierarchy (internal rendering surfaces) remains separate from Container's children (widget tree).
- `setFrontendRecurse` / `setSyncLaneRecurse` on View unchanged.
- `setTreeHostRecurse` on Widget unchanged.
- All compositor, proxy, LayerTree, and render target isolation unchanged.

## Sequencing

1. Add `Widget(Composition::Rect)` constructor, `makeSubView<T>()`, `viewAs<T>()` to Widget.h/cpp.
2. Add `Container(Composition::Rect)` overload.
3. Update `StackWidget`/`HStack`/`VStack` to accept Rect.
4. Migrate tests one at a time (each independently compilable).
5. Once all call sites are migrated, consider making `Widget(ViewPtr)` private or removing it.
