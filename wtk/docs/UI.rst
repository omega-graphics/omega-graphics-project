==
UI
==

The UI layer is the application shell of OmegaWTK. It owns the
process-wide ``AppInst`` singleton, the platform windows
(``AppWindow``), the view hierarchy that paints inside those
windows, and the cross-cutting concerns — menus, dialogs,
notifications, media playback — that the toolkit exposes to
application code. Most of the symbols an application actually
constructs come from this layer.

This document also covers the Style cascade — the ``StyleSheet``,
``StyleResolver``, and ``ThemeVars`` surfaces that drive how a
window resolves visual state across every view it owns. The
cascade is the modern path for theming, transitions, and keyframe
animations; the widget-side ``setStyle`` inline aggregate (covered
under ``UIView``) is still supported as a per-view override layer
on top of it.

Every symbol in this document lives in the ``OmegaWTK`` namespace
unless otherwise noted. Style-cascade types live in the nested
``OmegaWTK::StyleSheets`` namespace.

.. contents:: On this page
   :local:
   :depth: 2

----

Application Entry
=================

The Entry Point
---------------

OmegaWTK does not use ``main``. Instead, you provide a function
named ``omegaWTKMain`` and use the platform entry-point helpers
from ``AppEntryPoint.h`` to wire it into the platform's native
startup path. The three C functions declared in that header are
``extern "C"`` so that platform-specific stubs (``WinMain``,
``NSApplicationMain``, etc.) can call them without name-mangling.

.. code-block:: cpp

    #include <omegaWTK/AppEntryPoint.h>

    // OmegaWTKCreateApp, OmegaWTKRunApp, OmegaWTKDestroyApp are
    // declared in AppEntryPoint.h. Your entry TU defines:
    int omegaWTKMain(OmegaWTK::AppInst * app);

``OmegaWTKCreateApp(argc, argv)``
    Initialises the platform layer and returns an opaque pointer
    to the ``AppInst`` singleton.

``OmegaWTKRunApp(app)``
    Calls your ``omegaWTKMain`` and returns its exit code.

``OmegaWTKDestroyApp(app)``
    Tears down the platform layer after ``omegaWTKMain`` returns.

AppInst
-------

.. cpp:class:: OmegaWTK::AppInst

    The application singleton. Owns the ``AppWindowManager``, the
    active ``ThemeVars`` handle, and provides the run-loop entry
    and termination points. Obtain it via ``AppInst::inst()``; do
    not construct directly.

    .. cpp:member:: UniqueHandle<AppWindowManager> windowManager

        The single ``AppWindowManager`` for this application. Wire
        every window through this object before calling
        ``start()``.

    .. cpp:function:: static AppInst * inst()

        Returns the global ``AppInst`` pointer. Only valid after
        ``OmegaWTKCreateApp`` has been called.

    .. cpp:function:: static int start()

        Enters the platform run loop. Blocks until ``terminate()``
        is called. Returns the application exit code as soon as
        the native loop exits — it does **not** run framework
        teardown itself. Teardown happens later, when the
        ``AppInst`` object is destroyed (see ``~AppInst`` and the
        *Platform shutdown semantics* note below).

    .. cpp:function:: static void terminate()

        Asks the native event loop to stop on its next turn.
        Returns immediately — control flows back to whichever UI
        callback called it, then out through the native loop, then
        out of ``start()``. The actual framework teardown does not
        happen here; it runs in ``~AppInst`` after the caller's
        ``omegaWTKMain`` returns and every user-held AppWindow
        ``SharedHandle`` has dropped. Calling ``terminate()`` more
        than once is a no-op — the loop only needs the first
        signal.

        **Closing a window does not call ``terminate()`` for you.**
        Wire ``terminate()`` to a Quit menu item (or the platform's
        equivalent — Cmd+Q on macOS is automatically rerouted
        through this entry point). See the *Platform shutdown
        semantics* note below for the per-platform behaviour.

    .. cpp:function:: SharedHandle<ThemeVars> themeVars() const

        Returns the currently installed ``ThemeVars`` handle, or a
        null handle if no theme has been set. The Style cascade
        substitutes ``StyleSheets::Var{name}`` rule values against
        this theme during the Style phase.

    .. cpp:function:: void setThemeVars(SharedHandle<ThemeVars> theme)

        Swaps the active theme. The replacement takes effect on
        the next frame: every known ``AppWindow`` is marked
        Style-dirty so the resolver re-walks the cascade with the
        new bindings. Passing a null handle clears the active
        theme — every ``Var`` reference falls through to inline
        styles or the user-agent default sheet.

**A minimal application skeleton.**

.. code-block:: cpp

    #include <omegaWTK/AppEntryPoint.h>
    #include <omegaWTK/UI/App.h>
    #include <omegaWTK/UI/AppWindow.h>

    using namespace OmegaWTK;

    class MyWindowDelegate : public AppWindowDelegate {
    public:
        void windowWillClose(Native::NativeEventPtr) override {
            // Do not call AppInst::terminate() here. windowWillClose
            // is a per-window event ("this window is going away");
            // it is not a request to quit the application. Wire
            // termination to the Quit menu item instead — Cmd+Q on
            // macOS is rerouted through AppInst::terminate()
            // automatically.
        }
    };

    int omegaWTKMain(AppInst * app) {
        Composition::Rect windowRect{{0, 0}, 800, 600};
        auto window = make<AppWindow>(windowRect, new MyWindowDelegate());
        window->setTitle("My App");

        // ... attach root widget here ...

        app->windowManager->setRootWindow(window);
        app->windowManager->displayRootWindow();
        return AppInst::start();
    }

.. note::

    **Platform shutdown semantics (post-2026-06).** WTK no longer
    couples window-close to application termination at the
    framework layer. The window-close lifecycle is now:

    - **macOS.** Clicking the close traffic-light disposes of the
      window but leaves the app running with its menu bar —
      ``applicationShouldTerminateAfterLastWindowClosed:`` returns
      ``NO`` per Apple HIG. The user exits with Cmd+Q, the Apple
      menu's *Quit*, or an explicit Quit menu item.
      ``-[NSApplication terminate:]`` is intercepted by WTK's
      application delegate (``applicationShouldTerminate:``
      returns ``NSTerminateCancel`` and reroutes through
      ``AppInst::terminate()``) so that the post-loop
      ``doShutdown`` always runs — Apple's path would otherwise
      ``exit()`` directly and skip the GTE / Composition cleanup.
      Internally, ``AppInst::terminate()`` on Cocoa uses
      ``[NSApp stop:]`` plus a posted wake event rather than
      ``[NSApp terminate:]`` so that ``[NSApp run]`` returns to
      C++ where the shutdown code lives.
    - **Windows and GTK.** ``terminate()`` issues
      ``PostQuitMessage(0)`` / ``g_application_quit(...)``
      respectively; ``runEventLoop`` returns and ``start()`` runs
      the teardown. Closing the window via the X / titlebar
      button raises ``AppWindowDelegate::windowWillClose`` but does
      **not** quit the app on its own — application code is
      responsible for calling ``AppInst::terminate()`` from a Quit
      menu item or from inside ``windowWillClose`` if that
      platform convention is desired.

    The previous convention (every test calling
    ``AppInst::terminate()`` from ``windowWillClose``) was
    incorrect on macOS — closing the only window quit the entire
    app — and contributed to the GTE-not-closing report by tearing
    framework subsystems down while the native run loop was still
    draining events.

    **Teardown ordering.** Framework subsystems must outlive every
    user-held window reference. Callers typically write::

        int omegaWTKMain(AppInst * app) {
            auto window = make<AppWindow>(...);   // local handle
            app->windowManager->setRootWindow(window);
            app->windowManager->displayRootWindow();
            return AppInst::start();              // blocks; returns rc
        }

    When ``start()`` returns, ``window`` is still alive — its
    ``SharedHandle`` only drops as ``omegaWTKMain`` itself returns.
    For that reason WTK does **not** tear FontEngine, the
    compositor pools, or GTE down inside ``start()``. Those
    subsystems are released in ``~AppInst``, which runs after
    ``omegaWTKMain`` (and therefore after every caller-held
    ``AppWindow``) has gone out of scope. Doing it earlier closed
    the GPU device under a still-live AppWindow and produced
    D3D12MA validation errors on Windows; the Metal and Vulkan
    paths exhibited the same race but failed more quietly.

    Inside the destructor the order is fixed:
    ``windowManager->closeAllWindows()`` (drops the manager's
    rootWindow ref, which combined with the now-dropped caller
    local actually destroys the AppWindow — its backend resources
    are released back to the still-live device);
    ``FontEngine::Destroy()``; ``Composition::CleanupEngine()``
    (pools release their D3D12 / Metal / Vulkan handles);
    ``OmegaGTE::Close(gte)`` (device released last). Application
    code should not call any of these directly — invoke
    ``AppInst::terminate()`` and let the lifecycle handle the
    rest.

----

App Window
==========

AppWindow
---------

.. cpp:class:: OmegaWTK::AppWindow

    A top-level OS window. Owns a root widget, an optional menu
    bar (on desktop platforms), the per-window style-sheet stack,
    and the platform dialog APIs. The window's compositor is wired
    up on construction; the user-agent default style sheet is
    installed at the bottom of the cascade stack so widget code
    does not have to authorise common defaults.

    .. cpp:function:: explicit AppWindow(Composition::Rect rect, AppWindowDelegate * delegate = nullptr)

        Constructs the window at the given screen rect.
        ``delegate`` receives resize and close events; pass
        ``nullptr`` if you do not need to react to those.

    .. cpp:function:: void setTitle(OmegaCommon::StrRef title)

        Sets the title bar string.

    .. cpp:function:: void setRootWidget(WidgetPtr widget)

        Attaches the root widget to the window. The widget's rect
        is stretched to the window's content area. Call this
        before ``displayRootWindow()``.

    .. cpp:function:: void setMenu(SharedHandle<Menu> & menu)

        *(Desktop only.)* Attaches a ``Menu`` hierarchy as the
        window's menu bar.

    .. cpp:function:: void setEnableWindowHeader(bool enable)

        *(Desktop only.)* Shows or hides the native title bar /
        chrome. Set to ``false`` for fully custom window frames.

    .. cpp:function:: void close()

        Programmatically closes the window and triggers
        ``AppWindowDelegate::windowWillClose``.

The Style Cascade Stack
~~~~~~~~~~~~~~~~~~~~~~~

Every ``AppWindow`` carries an ordered stack of ``StyleSheet``
handles. The Style cascade walks this stack front to back when
resolving each cell; sheets added later beat earlier sheets on
specificity ties, matching the conventional CSS "later wins"
rule. The user-agent default sheet is automatically installed at
index 0 so application code and inline widget styles always win
overrides.

.. cpp:function:: void AppWindow::addStyleSheet(SharedHandle<StyleSheets::StyleSheet> sheet)

    Pushes a sheet onto the top of the cascade stack and re-runs
    the cascade on every widget in the window. Use this to install
    a theme, a feature flag overlay, or a transient highlight
    state.

.. cpp:function:: void AppWindow::removeStyleSheet(const SharedHandle<StyleSheets::StyleSheet> & sheet)

    Removes the first stack entry equal to ``sheet`` and re-runs
    the cascade. No-op if the sheet is not on the stack.

.. cpp:function:: const OmegaCommon::Vector<SharedHandle<StyleSheets::StyleSheet>> & AppWindow::styleSheets() const

    Returns the current stack (front = lowest priority, back =
    highest priority).

The Run Loop Surface
~~~~~~~~~~~~~~~~~~~~

.. cpp:function:: void AppWindow::refresh()

    The public idle-context entrypoint that schedules the next
    paint. Use this from a menu callback, timer, or any
    application code that has finished a batch of mutations and
    wants the result to render. Multiple ``refresh()`` calls
    inside the same run-loop turn coalesce to a single paint, so
    over-calling is harmless; under-calling leaves the dirty bit
    parked until something else pumps the loop.

    The View layer mutators (``markDirty``, ``setState``,
    ``setPseudoClassBits``) only record their dirty bits. They do
    not unilaterally request a frame because multiple views may
    flip state in the same idle batch and the window owns the
    "one paint per batch" decision.

.. cpp:function:: void AppWindow::requestFrame()

    The internal entrypoint that the deferred ``Widget::invalidate``
    path drives. ``refresh()`` is its app-facing alias; both
    forward to the same native-window request.

.. cpp:function:: void AppWindow::flushFrame()

    Builds one frame: runs the FrameBuilder's Tick → Style →
    Layout → Paint passes and submits the resulting display list
    to the compositor. The native run loop calls this when the
    coalesced ``requestFrame`` ticket fires.

Dialogs and Notifications
~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:function:: SharedHandle<Native::NativeFSDialog> AppWindow::openFSDialog(const Native::NativeFSDialog::Descriptor & desc)

    Opens a platform file-system dialog (open or save). Returns a
    handle whose ``getResult()`` async method yields the selected
    path.

.. cpp:function:: SharedHandle<Native::NativeNoteDialog> AppWindow::openNoteDialog(const Native::NativeNoteDialog::Descriptor & desc)

    Opens a platform alert/note dialog with a title and message
    string.

AppWindowManager
----------------

.. cpp:class:: OmegaWTK::AppWindowManager

    Tracks the window hierarchy and controls display order. Held
    by ``AppInst`` as ``windowManager``.

    .. cpp:function:: WindowIndex addWindow(AppWindowPtr handle)

        Adds a window to the manager. The order of insertion
        determines stacking priority. Returns the window's index.

    .. cpp:function:: void setRootWindow(AppWindowPtr handle)

        Designates one window as the primary (root) window.

    .. cpp:function:: AppWindowPtr getRootWindow()

        Returns the current root window.

    .. cpp:function:: void displayRootWindow()

        Makes the root window visible. Call this after attaching
        the root widget and menu.

AppWindowDelegate
-----------------

.. cpp:class:: OmegaWTK::AppWindowDelegate

    Interface for responding to window-level native events.
    Subclass it and pass an instance to ``AppWindow``'s
    constructor.

    .. cpp:function:: virtual void windowWillClose(Native::NativeEventPtr event)

        Called when the OS is about to close the window — the user
        clicked the close button, or the application called
        ``close()``. This is a per-window event ("this window is
        going away"); it is **not** a request to quit the
        application.

        Do not call ``AppInst::terminate()`` from this handler.
        Doing so terminates the entire app on close, which is
        wrong on macOS (HIG: apps stay alive when the last window
        closes) and forces a teardown ordering that races with
        the still-draining native event loop. Wire termination to
        a Quit menu item instead; on macOS Cmd+Q and the Apple
        menu's *Quit* are rerouted through
        ``AppInst::terminate()`` automatically. See *Platform
        shutdown semantics* under :cpp:class:`OmegaWTK::AppInst`
        for the per-platform behaviour.

        Use this hook for per-window cleanup — releasing handles
        held in the delegate, persisting state for that window,
        flushing per-window caches.

    .. cpp:function:: virtual void windowWillResize(Composition::Rect & nRect)

        Called during a live resize. ``nRect`` carries the
        proposed dimensions; modify it in place to clamp or veto
        the resize.

----

Views
=====

A ``View`` is the rendering node that sits inside a ``Widget``. The
modern paint pipeline (Phase 4.7+) walks the view tree once per
frame and dispatches Style, Layout, and Paint passes through
virtual hooks on each node. Application code rarely subclasses
``View`` directly — the widget primitives in the **Widgets**
document do, and ``UIView`` is the everyday rendering model that
the primitives produce.

View
----

.. cpp:class:: OmegaWTK::View

    The base rendering node. Carries geometry, dirty bits, a
    pseudo-class state mask, a custom-state set, and the
    framework's virtual hooks for Style / Layout / Paint.

    **Geometry.**

    .. cpp:function:: Composition::Rect & getRect()

        Returns the view's bounding rect.

    .. cpp:function:: void resize(Composition::Rect newRect)

        Synchronously resizes the view and re-runs layout for its
        subtree on the next frame.

    .. cpp:function:: std::uint64_t nodeId() const

        Stable per-View identity used by the per-window
        ``AnimationScheduler`` to key its side table. Plain
        ``uint64_t`` so the public View surface does not depend on
        the scheduler header.

    **Dirty bits.** The framework propagates four bits up the
    ancestor chain so the FrameBuilder can prune clean subtrees
    out of each pass:

    .. cpp:enum:: View::DirtyBit

        ``Style``, ``Layout``, ``Content``, ``Paint``.

    .. cpp:function:: void markDirty(uint8_t bits)

        Records the bits on this view and ORs them into every
        ancestor's ``descendantDirty`` mask. **Does not request a
        frame.** Idle-context callers — menu callbacks, timers,
        deferred async results — should call
        ``AppWindow::refresh()`` after the batch of mutations to
        commit the cascade re-evaluation.

    .. cpp:function:: uint8_t dirtyBits() const

        Returns the bits set on this view itself.

    .. cpp:function:: uint8_t descendantDirty() const

        Returns the bits set anywhere in this view's subtree.

    **Pseudo-classes.** Bit mask matching the enumerated
    ``StyleSheets::PseudoClass`` values. The input dispatcher
    flips Hover and Pressed on the appropriate target view as
    cursor and mouse events arrive; ``setEnabled(false)`` flips
    Disabled.

    .. cpp:function:: std::uint8_t pseudoClassBits() const

        Returns the current bit mask. ``Hover = 1``,
        ``Pressed = 2``, ``Focused = 4``, ``Disabled = 8``.

    .. cpp:function:: void setPseudoClassBits(std::uint8_t mask, bool on)

        Sets or clears the bits named in ``mask``. If anything
        actually changed, marks the view Style-dirty so the
        cascade re-resolves on the next frame.

    **Custom states.** The open-ended counterpart to the
    enumerated pseudo-class mask. App and widget code names
    states freely — ``loading``, ``selected``, ``expanded``,
    ``error`` — and the cascade subset-matches the name against
    the view's set.

    .. cpp:function:: void setState(const OmegaCommon::String & name)

        Adds ``name`` to the view's custom-state set. Marks the
        view Style-dirty if the set actually changed.

    .. cpp:function:: void clearState(const OmegaCommon::String & name)

        Removes ``name``. Marks the view Style-dirty if the set
        actually changed.

    .. cpp:function:: void setState(const OmegaCommon::String & name, bool on)

        Convenience that picks between ``setState`` and
        ``clearState`` based on ``on``.

    .. cpp:function:: bool hasState(const OmegaCommon::String & name) const

        Returns ``true`` if ``name`` is currently in the set.

    **Tree manipulation.**

    .. cpp:function:: void addSubView(View * view)

        Adds a child view. The framework inherits the parent's
        current dirty bits onto the new child so the child
        participates in whatever passes the parent is dirty for —
        no explicit ``update()`` needed after creating a
        sub-view.

    **Visibility.**

    .. cpp:function:: void enable()

        Makes the view visible and clears the Disabled
        pseudo-class bit.

    .. cpp:function:: void disable()

        Hides the view and sets the Disabled pseudo-class bit.

CanvasView
----------

.. cpp:class:: OmegaWTK::CanvasView : public View

    A ``View`` with an already-bound ``Canvas`` on its root
    layer. Used by widgets that draw entirely through the
    low-level Canvas API. Most modern code prefers ``UIView``
    instead — see below.

UIView
------

.. cpp:class:: OmegaWTK::UIView : public View

    A view that renders from a declarative element list
    (``UIViewLayoutV2``) styled by per-cell resolved property
    values. ``UIView`` is the rendering model the built-in widget
    primitives produce (``Rectangle``, ``Label``, ``Icon``,
    ``Image``, the ``Button`` background, etc.) and the recommended
    base for custom integration code that wants to participate in
    the Style cascade.

    .. cpp:function:: explicit UIView(const Composition::Rect & rect, ViewPtr parent, UIViewTag tag)

        Constructs the view. ``tag`` (an ``OmegaCommon::String``)
        is the view-level selector the Style cascade matches
        against.

    **Authoring layout.**

    .. cpp:function:: void setLayout(const UIViewLayout & layout)

        Sets the legacy element list (a flat list of named shapes
        and text runs).

    .. cpp:function:: void setLayoutV2(const UIViewLayoutV2 & layout)

        Sets the modern element list. Each element carries a
        ``UIElementTag`` (which the cascade matches against), a
        shape or text content, and an optional ``LayoutStyle``.

    Both ``setLayout`` variants mark the view Style / Layout /
    Paint-dirty automatically — application code does not need
    to call ``update()`` after a layout swap.

    **Inline style aggregate.**

    .. cpp:function:: void setStyle(const StylePtr & style)

        Attaches an inline ``Style`` aggregate (the legacy
        property-bag declared in ``omegaWTK/UI/UIView.h``).
        Inline styles take precedence over cascade rules on
        cell overlap — they are the override layer on top of the
        cascade-resolved baseline.

    .. cpp:function:: StylePtr getStyle() const

        Returns the currently attached inline ``Style`` handle.

UIViewLayout
------------

.. cpp:class:: OmegaWTK::UIViewLayout

    A flat list of shape and text elements identified by
    ``UIElementTag`` strings. The legacy authoring surface; new
    code typically uses ``UIViewLayoutV2`` for its richer per-
    element ``LayoutStyle``.

    .. cpp:function:: void text(UIElementTag tag, OmegaCommon::UString content)

        Adds or replaces a text element with the given tag.

    .. cpp:function:: void text(UIElementTag tag, OmegaCommon::UString content, const Composition::Rect & rect)

        Same as above, with an explicit bounding rect.

    .. cpp:function:: void shape(UIElementTag tag, const Shape & shape)

        Adds or replaces a shape element.

    .. cpp:function:: bool remove(UIElementTag tag)

        Removes the element with the given tag. Returns ``false``
        if not present.

    .. cpp:function:: void clear()

        Removes all elements.

ScrollView
----------

.. cpp:class:: OmegaWTK::ScrollView : public View

    Wraps a single child view that may exceed the scroll view's
    visible bounds. Clips the child to its own rect and provides
    native scroll bars (customisable). The scroll offset
    propagates through the layout pipeline so descendants observe
    scroll-shifted ``finalRect`` values.

VideoView
---------

.. cpp:class:: OmegaWTK::VideoView : public View, public Media::FrameSink

    Displays video frames delivered through the ``FrameSink``
    interface. Attach as the frame sink of a
    ``VideoPlaybackSession`` or ``VideoCaptureSession``.

SVGView
-------

.. cpp:class:: OmegaWTK::SVGView : public View

    Renders a static or dynamic SVG graphic. Accepts source from
    a string (``setSourceString``) or the output of an
    ``SVGSession``.

ViewDelegate
------------

.. cpp:class:: OmegaWTK::ViewDelegate

    Interface for handling mouse and keyboard events delivered to
    a single ``View``. Attach via ``View::setDelegate()``;
    override only the events you care about.

    .. cpp:function:: virtual void onMouseEnter(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onMouseExit(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onLeftMouseDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onLeftMouseUp(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onRightMouseDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onRightMouseUp(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onKeyDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onKeyUp(Native::NativeEventPtr event)

**Example — a click handler.**

.. code-block:: cpp

    class ButtonDelegate : public OmegaWTK::ViewDelegate {
        std::function<void()> callback_;
    public:
        explicit ButtonDelegate(std::function<void()> cb)
            : callback_(std::move(cb)) {}
        void onLeftMouseUp(OmegaWTK::Native::NativeEventPtr) override {
            callback_();
        }
    };

    view->setDelegate(new ButtonDelegate([&]{ doAction(); }));

----

The Style Cascade
=================

OmegaWTK's Style cascade is the recommended way to author visual
state. Application code builds one or more ``StyleSheet`` handles
and pushes them onto an ``AppWindow``'s stack; the FrameBuilder
walks the stack during each frame's Style pass, matches rules
against the views in the tree, and writes the resulting cells
into a per-view property table that Paint reads. The shape borrows
heavily from CSS — selectors with specificity, last-wins source
order, transitions, keyframe animations, pseudo-classes, and
theme variables — but the matcher is single-compound only, the
property set is exhaustive (no string-keyed declarations), and
the binding lives in C++ rather than at the parser layer.

The cascade types live in the ``OmegaWTK::StyleSheets`` namespace.

PropertyKey
-----------

.. cpp:enum:: OmegaWTK::PropertyKey

    The enumerated cell that every style declaration writes. The
    enum spans visual cells (``BackgroundColor``, ``BorderColor``,
    ``BorderWidth``, ``Opacity``, ``FillBrush``, ``DropShadow``,
    transform components), text cells (``TextColor``, ``TextFont``,
    ``TextLayout``, ``TextLineLimit``), and layout cells
    (``LayoutWidth``, ``LayoutHeight``, ``LayoutX``, ``LayoutY``).
    Application code starting custom property channels uses the
    range starting at ``UserDefined = 0x8000``.

Selector
--------

.. cpp:struct:: OmegaWTK::StyleSheets::Selector

    A single-compound selector — one optional tag, one optional
    id, zero or more class tokens, a pseudo-class subset, and
    zero or more custom-state names. A view matches when every
    present constraint matches.

    .. cpp:member:: OmegaCommon::String tag

        Matches the view's ``UIViewTag`` (view-scope properties)
        or an element's ``UIElementTag`` (element-scope
        properties). Empty = no tag constraint.

    .. cpp:member:: OmegaCommon::String id

        Matches the view's id. Empty = no id constraint. Id
        authoring is not yet exposed on the View surface, so a
        non-empty id refuses to match in this release.

    .. cpp:member:: OmegaCommon::Vector<OmegaCommon::String> classes

        Class tokens. Empty = no class constraint. As with id,
        class authoring is reserved for a future release.

    .. cpp:member:: StyleSheets::PseudoClass pseudoClasses

        Subset mask — every bit set on the selector must be set
        on the view's ``pseudoClassBits()``. ``Hover = 1``,
        ``Pressed = 2``, ``Focused = 4``, ``Disabled = 8``.

    .. cpp:member:: OmegaCommon::Vector<OmegaCommon::String> customStates

        Subset list of ``:state(name)`` tokens — every name
        listed here must be present in the view's custom-state
        set. Each entry weighs the same as one class in
        specificity.

    .. cpp:function:: int specificity() const

        Returns the CSS-style specificity:
        ``id * 100 + (class + pseudo + customState) * 10 + tag``.
        ``StyleRule::beats`` uses this for cascade ordering.

TransitionSpec
--------------

.. cpp:struct:: OmegaWTK::StyleSheets::TransitionSpec

    Records that a particular property channel on a rule should
    animate when its cell changes between frames.

    .. cpp:member:: PropertyKey key

        The cell to animate.

    .. cpp:member:: Composition::TimingOptions timing

        Duration, delay, iterations, etc. — see the **Composition**
        document.

    .. cpp:member:: SharedHandle<Composition::AnimationCurve> curve

        Easing curve. ``nullptr`` defaults to linear interpolation.

KeyframeAnimation
-----------------

.. cpp:struct:: OmegaWTK::StyleSheets::KeyframeAnimation

    A named keyframe animation declaration on a sheet. Tracks
    one or more properties; the resolver fires it on every node
    whose matched rule carries ``animationName = name``.

    .. cpp:member:: OmegaCommon::String name

        The name app code references via
        ``StyleRule::animationName``.

    .. cpp:member:: OmegaCommon::Vector<KeyframeAnimationProperty> properties

        Per-property keyframe tracks.

    .. cpp:member:: Composition::TimingOptions defaultTiming

        Timing used when the animation fires.

.. cpp:struct:: OmegaWTK::StyleSheets::KeyframeAnimationProperty

    One per-property track of a keyframe animation.

    .. cpp:member:: PropertyKey key

        The cell the track writes.

    .. cpp:member:: Composition::KeyframeTrack<AnimatedValue> track

        Type-erased over the runtime ``AnimatedValue`` variant so
        a single track can carry color, scalar, drop-shadow, or
        any other supported animatable value.

StyleRule
---------

.. cpp:struct:: OmegaWTK::StyleSheets::StyleRule

    A single declaration block: a selector, a property→value map,
    and optional transition / animation metadata.

    .. cpp:member:: Selector selector

        The match constraint.

    .. cpp:member:: OmegaCommon::Map<PropertyKey, StyleValue> declarations

        The property→value cells the rule writes when it matches.

    .. cpp:member:: OmegaCommon::Vector<TransitionSpec> transitions

        Transitions to register for cells the rule writes.

    .. cpp:member:: Core::Optional<OmegaCommon::String> animationName

        References a ``KeyframeAnimation`` declared on the same
        sheet. The resolver fires the named animation when this
        rule starts matching a view and cancels it when the rule
        stops matching.

    **Typed declaration setters.** Each setter writes one cell
    into ``declarations`` and returns ``*this`` so rule authoring
    can chain. The setter family covers the cells Paint reads
    today; sheets needing a cell without a setter can assign
    ``declarations[Key] = StyleValue{...}`` directly.

    .. cpp:function:: StyleRule & setBackgroundColor(Composition::Color color)
    .. cpp:function:: StyleRule & setBorderColor(Composition::Color color)
    .. cpp:function:: StyleRule & setBorderWidth(std::uint32_t widthPx)
    .. cpp:function:: StyleRule & setFillBrush(SharedHandle<Composition::Brush> brush)
    .. cpp:function:: StyleRule & setDropShadow(Composition::LayerEffect::DropShadowParams params)
    .. cpp:function:: StyleRule & setTextFont(SharedHandle<Composition::Font> font)
    .. cpp:function:: StyleRule & setTextColor(Composition::Color color)
    .. cpp:function:: StyleRule & setTextLayout(Composition::TextLayoutDescriptor layout)
    .. cpp:function:: StyleRule & setTextLineLimit(std::uint32_t lineLimit)

StyleSheet and Builder
----------------------

.. cpp:class:: OmegaWTK::StyleSheets::StyleSheet

    An immutable bundle of style rules plus named keyframe
    animations. Once a sheet is built, its contents do not change
    — to add a rule, build a new sibling through ``Builder``. The
    sheet handle is freely shareable across windows.

    .. cpp:function:: static SharedHandle<StyleSheet> Create()

        Returns an empty sheet. Most app code goes through
        ``Builder`` instead.

    .. cpp:function:: const OmegaCommon::Vector<StyleRule> & rules() const

        Returns the rule list in source order.

    .. cpp:function:: const OmegaCommon::Map<OmegaCommon::String, KeyframeAnimation> & keyframeAnimations() const

        Returns the named keyframe animations declared on this
        sheet.

.. cpp:class:: OmegaWTK::StyleSheets::StyleSheet::Builder

    Accumulates rules and keyframe declarations and produces an
    immutable handle.

    .. cpp:function:: Builder & addRule(StyleRule rule)

        Appends a rule. The Builder stamps a per-build source
        order so the cascade tiebreak ("later wins") works as
        expected within one sheet.

    .. cpp:function:: Builder & addKeyframeAnimation(KeyframeAnimation animation)

        Adds a named keyframe animation. Name collisions overwrite
        — the last declaration wins, matching CSS's ``@keyframes``
        redefinition behaviour.

    .. cpp:function:: SharedHandle<StyleSheet> build() const

        Returns the immutable sheet handle. The Builder may be
        reused to seed a sibling.

.. cpp:function:: SharedHandle<StyleSheet> OmegaWTK::StyleSheets::BuildUserAgentStyleSheet()

    Returns the toolkit's built-in default sheet. Every
    ``AppWindow`` installs the result of this call at the bottom
    of its cascade stack on construction, so app code does not
    have to reach for it directly — but you can build extra
    instances to seed a custom default layer, or to inspect what
    the toolkit ships as defaults.

**A complete sheet for a card primitive.**

.. code-block:: cpp

    using namespace OmegaWTK;
    using namespace OmegaWTK::StyleSheets;
    using namespace OmegaWTK::Composition;

    StyleSheet::Builder b;

    // Default — the card always has a dark background and white text.
    {
        StyleRule rule;
        rule.selector.tag = "card";
        rule.setFillBrush(ColorBrush(Color::create8Bit(0x2A2A2A, 0xFF)));
        b.addRule(std::move(rule));
    }

    {
        StyleRule rule;
        rule.selector.tag = "card_title";
        rule.setTextColor(Color::create8Bit(Color::White8));
        b.addRule(std::move(rule));
    }

    // Hover override — same target, higher specificity via the
    // :hover pseudo-class.
    {
        StyleRule rule;
        rule.selector.tag = "card";
        rule.selector.pseudoClasses = PseudoClass::Hover;
        rule.setFillBrush(ColorBrush(Color::create8Bit(0x3A3A3A, 0xFF)));

        TransitionSpec t;
        t.key = PropertyKey::FillBrush;
        t.timing.durationMs = 150;
        t.curve = AnimationCurve::EaseInOut();
        rule.transitions.push_back(t);

        b.addRule(std::move(rule));
    }

    auto sheet = b.build();
    window->addStyleSheet(sheet);

StyleResolver
-------------

.. cpp:class:: OmegaWTK::StyleSheets::StyleResolver

    The cascade walker that the FrameBuilder runs during the
    Style pass. ``StyleResolver`` is not something application
    code constructs; the framework calls it. Knowing what it does
    helps when you are debugging why a cell ended up at a
    particular value.

    .. cpp:function:: static void apply(UIView & view)

        Runs the cascade for ``view``. Walks the owning window's
        sheet stack, evaluates every rule against the view's tag,
        pseudo-class bits, and custom-state set, and writes the
        winning cell for each ``(node, PropertyKey)`` into the
        view's property table. Recorded transition and animation
        bindings are accumulated for the firing passes below.

    .. cpp:function:: static void applyTransitions(UIView & view)

        Fires registered transitions for cells whose resolved
        value changed between the previous frame's snapshot and
        this frame's table. Each fire calls into the per-window
        ``AnimationScheduler`` with the spec's timing and curve.

    .. cpp:function:: static void applyKeyframeBindings(UIView & view)

        Reconciles ``animation: <name>`` bindings against the
        scheduler. Starts animations whose binding became active
        this frame, cancels animations whose binding stopped
        matching, and leaves running animations alone when their
        binding is unchanged (matching CSS's "same name does not
        restart" semantic).

**Specificity in practice.** When two rules match the same cell
the resolver compares ``Selector::specificity()`` values; higher
wins. On a tie, the rule declared later (in source order, then in
sheet-stack order) wins. ``tag`` weighs ``1``, each ``class`` or
pseudo-class bit or custom-state name weighs ``10``, and ``id``
weighs ``100``. So a ``tag = "button"`` rule (specificity 1)
loses to a ``tag = "button", pseudoClasses = Hover`` rule
(specificity 11) when the button is hovered, exactly as you
would expect.

ThemeVars
---------

.. cpp:class:: OmegaWTK::ThemeVars

    A process-wide named-value map referenced by
    ``StyleSheets::Var`` in sheet rules. The active ``ThemeVars``
    handle is held by ``AppInst``; the resolver substitutes
    ``Var{name}`` rule values against it during the Style phase.

    Shape parallels ``StyleSheet``: immutable once built,
    produced through a ``Builder``, shareable across windows.

    .. cpp:function:: static SharedHandle<ThemeVars> Create()

        Returns an empty handle. The common path is to build a
        populated one through ``Builder``.

    .. cpp:function:: Core::Optional<StyleValue> lookup(const OmegaCommon::String & name) const

        Returns the bound ``StyleValue`` if ``name`` is present,
        otherwise an empty optional.

    .. cpp:function:: bool empty() const
    .. cpp:function:: const OmegaCommon::Map<OmegaCommon::String, StyleValue> & values() const

.. cpp:class:: OmegaWTK::ThemeVars::Builder

    Accumulates name→value bindings and produces an immutable
    ``ThemeVars`` handle.

    .. cpp:function:: Builder & set(const OmegaCommon::String & name, StyleValue value)

        Binds ``name`` to ``value``. Reuse the same builder to
        seed a sibling theme handle.

    .. cpp:function:: SharedHandle<ThemeVars> build() const

        Returns the immutable handle. Hand it to
        ``AppInst::setThemeVars`` to swap the active theme.

**Resolution semantics.** A ``Var{name}`` in a rule resolves to
the bound concrete value when the lookup succeeds. If no theme
is installed, or the name is missing, or the bound value is
itself a ``Var`` (chains are not followed in this release), the
resolver skips that cell write — the inline ``Style`` writes
that follow the resolver still get a chance to author it, and
otherwise the cell falls through to the user-agent default sheet.
This matches CSS ``var()`` fallthrough.

**Example — a two-theme app with a swap menu item.**

.. code-block:: cpp

    using namespace OmegaWTK;
    using namespace OmegaWTK::StyleSheets;
    using namespace OmegaWTK::Composition;

    SharedHandle<ThemeVars> buildDarkTheme() {
        return ThemeVars::Builder()
            .set("control.background",
                 StyleValue{ColorBrush(Color::create8Bit(0x2A2A2A, 0xFF))})
            .set("control.text",
                 StyleValue{Color::create8Bit(Color::White8)})
            .build();
    }

    SharedHandle<ThemeVars> buildLightTheme() {
        return ThemeVars::Builder()
            .set("control.background",
                 StyleValue{ColorBrush(Color::create8Bit(0xF5F5F5, 0xFF))})
            .set("control.text",
                 StyleValue{Color::create8Bit(0x101010, 0xFF)})
            .build();
    }

    // In your menu callback:
    AppInst::inst()->setThemeVars(buildLightTheme());
    window->refresh();   // commit the cascade re-resolution

----

Menus
=====

.. cpp:class:: OmegaWTK::MenuItem

    A single item in a menu. Created through the free functions
    below.

    .. cpp:function:: void enable()
    .. cpp:function:: void disable()

.. cpp:class:: OmegaWTK::Menu

    A named collection of ``MenuItem`` instances with an
    optional delegate.

    .. cpp:function:: Menu(OmegaCommon::String name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate * delegate)

.. cpp:class:: OmegaWTK::MenuDelegate

    Interface for receiving item selection callbacks.

    .. cpp:function:: virtual void onSelectItem(unsigned itemIndex)

        Called when the user selects an item. ``itemIndex`` is
        the zero-based position within the enclosing
        ``CategoricalMenu``. Separators count toward the index.

**Free functions for building menus.**

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::CategoricalMenu(const OmegaCommon::String & name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate * delegate = nullptr)

    Creates a top-level category (e.g. ``"File"``, ``"Edit"``).

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::SubMenu(const OmegaCommon::String & name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate * delegate = nullptr)

    Creates a nested submenu.

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::ButtonMenuItem(const OmegaCommon::String & name)

    Creates a clickable item.

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::MenuItemSeperator()

    Creates a visual separator between groups of items.

**Example — a File / Edit menu pair.**

.. code-block:: cpp

    using namespace OmegaWTK;

    class FileDelegate : public MenuDelegate {
    public:
        void onSelectItem(unsigned idx) override {
            if (idx == 0) { /* Open  */ }
            if (idx == 2) { AppInst::terminate(); }
        }
    };
    static FileDelegate fileDelegate;

    auto menu = make<Menu>("MainMenu",
        std::initializer_list<SharedHandle<MenuItem>>{
            CategoricalMenu("File", {
                ButtonMenuItem("Open"),
                MenuItemSeperator(),
                ButtonMenuItem("Quit")
            }, &fileDelegate),
            CategoricalMenu("Edit", {
                ButtonMenuItem("Cut"),
                ButtonMenuItem("Copy"),
                ButtonMenuItem("Paste")
            })
        });

    window->setMenu(menu);

----

Dialogs
=======

File System Dialog
------------------

.. cpp:class:: OmegaWTK::Native::NativeFSDialog

    A platform-native file open/save dialog.

    .. cpp:struct:: Descriptor

        +-------------------+-------------------------------------------+
        | ``type``          | ``Read`` (open) or ``Write`` (save).      |
        +-------------------+-------------------------------------------+
        | ``openLocation``  | Initial directory to display.             |
        +-------------------+-------------------------------------------+

    .. cpp:function:: virtual OmegaCommon::Async<OmegaCommon::String> getResult()

        Returns a future that resolves to the selected file path,
        or an empty string if the user cancelled.

.. code-block:: cpp

    auto dialog = window->openFSDialog(
        {OmegaWTK::Native::NativeFSDialog::Read, "/Users/me/Documents"});
    auto path = dialog->getResult().get();   // blocks until user confirms
    if (!path.empty()) {
        openFile(path);
    }

Note Dialog
-----------

.. cpp:class:: OmegaWTK::Native::NativeNoteDialog

    A platform-native alert dialog.

    .. cpp:struct:: Descriptor

        ``title`` and ``str`` (body text).

.. code-block:: cpp

    window->openNoteDialog({"Unsaved Changes",
                            "Do you want to save before closing?"});

----

Notifications
=============

.. cpp:class:: OmegaWTK::NotificationCenter

    Sends system-level notifications (macOS Notification Center,
    Windows Action Center, etc.).

    .. cpp:function:: void send(NotificationDesc desc)

        Posts a notification with the given title and body text.

.. code-block:: cpp

    OmegaWTK::NotificationCenter nc;
    nc.send({"Export Complete", "Your file has been saved to ~/Desktop."});

----

Media
=====

Media types live in the ``OmegaWTK::Media`` namespace. They cover
audio / video capture and playback; the rendering side hands frames
off to a ``VideoView`` through the ``FrameSink`` interface.

AudioVideoProcessor
-------------------

.. cpp:class:: OmegaWTK::Media::AudioVideoProcessor

    An H.264 / H.265 encoder and decoder. Used to transcode media
    before feeding it to a playback or capture session.

Playback
--------

.. cpp:class:: OmegaWTK::Media::PlaybackDispatchQueue

    An isolated thread that schedules and dispatches media
    playback events. Pass one instance to playback sessions that
    need asynchronous frame delivery.

.. cpp:class:: OmegaWTK::Media::AudioPlaybackDevice

    A physical audio output device (built-in speakers, USB
    headset, etc.). Pass it to ``AudioPlaybackSession`` to select
    the output route.

.. cpp:class:: OmegaWTK::Media::AudioPlaybackSession

    Manages playback of an audio stream from a
    ``MediaInputStream``.

    .. cpp:function:: void setSource(SharedHandle<MediaInputStream> source)
    .. cpp:function:: void setPlaybackDevice(SharedHandle<AudioPlaybackDevice> device)
    .. cpp:function:: void start()
    .. cpp:function:: void stop()

.. code-block:: cpp

    auto audioFile = OmegaWTK::Media::MediaInputStream::fromFile("./track.mp3");
    auto session   = OmegaWTK::Media::AudioPlaybackSession::Create(dispatchQueue);
    session.setSource(audioFile);
    session.setPlaybackDevice(playbackDevice);
    session.start();
    // ... later ...
    session.stop();

.. cpp:class:: OmegaWTK::Media::VideoPlaybackSession

    Manages playback of a video stream. Frames go to a
    ``VideoView`` through the ``FrameSink`` interface; audio
    routes to an ``AudioPlaybackDevice``.

    .. cpp:function:: void setSource(SharedHandle<MediaInputStream> source)
    .. cpp:function:: void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> device)
    .. cpp:function:: void setVideoFrameSink(SharedHandle<VideoView> sink)
    .. cpp:function:: void start()
    .. cpp:function:: void stop()

.. code-block:: cpp

    auto videoFile = OmegaWTK::Media::MediaInputStream::fromFile("./clip.mp4");
    auto session   = OmegaWTK::Media::VideoPlaybackSession::Create(dispatchQueue);
    session.setSource(videoFile);
    session.setAudioPlaybackDevice(audioDevice);
    session.setVideoFrameSink(videoView);
    session.start();

Capture
-------

.. cpp:class:: OmegaWTK::Media::AudioCaptureDevice

    A physical audio input device (microphone, camera mic).

.. cpp:class:: OmegaWTK::Media::AudioCaptureSession

    Records or previews audio from an ``AudioCaptureDevice``.

.. cpp:class:: OmegaWTK::Media::VideoDevice

    A physical video capture device (webcam, capture card).

.. cpp:class:: OmegaWTK::Media::VideoCaptureSession

    Records or previews video from a ``VideoDevice`` paired with
    an ``AudioCaptureDevice``.

    .. cpp:function:: void setPreviewFrameSink(SharedHandle<VideoView> sink)
    .. cpp:function:: void setPreviewAudioOutput(SharedHandle<AudioPlaybackDevice> device)

.. code-block:: cpp

    auto session = videoDevice->createCaptureSession(audioCaptureDevice);
    session->setPreviewFrameSink(videoView);
    session->setPreviewAudioOutput(audioPlaybackDevice);
