===
API
===

This page documents every public class, struct, and free function in OmegaWTK.
All symbols live in the ``OmegaWTK`` namespace unless otherwise noted. Pull in
the relevant headers as shown in each section, or include the top-level
umbrella header::

    #include <omegaWTK/UI/App.h>
    #include <omegaWTK/UI/AppWindow.h>
    #include <omegaWTK/UI/Widget.h>

.. contents:: On this page
   :local:
   :depth: 2

----

Application Entry
-----------------

Entry Point
~~~~~~~~~~~

OmegaWTK does not use ``main``. Instead, you provide a function named
``omegaWTKMain`` and use the platform entry-point helpers from
``AppEntryPoint.h`` to wire it in. The three C functions below are declared
``extern "C"`` so that platform-specific entry stubs (``WinMain``,
``NSApplicationMain``, etc.) can call them without name-mangling.

.. code-block:: cpp

    #include <omegaWTK/AppEntryPoint.h>

    // OmegaWTKCreateApp, OmegaWTKRunApp, OmegaWTKDestroyApp are declared here.
    // Your entry TU defines:
    int omegaWTKMain(OmegaWTK::AppInst *app);

``OmegaWTKCreateApp(argc, argv)``
    Initialises the platform layer and returns an opaque pointer to the
    ``AppInst`` singleton.

``OmegaWTKRunApp(app)``
    Calls your ``omegaWTKMain`` implementation and returns its exit code.

``OmegaWTKDestroyApp(app)``
    Tears down the platform layer after ``omegaWTKMain`` returns.

----

``AppInst``
~~~~~~~~~~~

.. cpp:class:: OmegaWTK::AppInst

    The application singleton. Owns the ``AppWindowManager`` and provides the
    run-loop entry and termination points. Obtain the instance via
    ``AppInst::inst()``; do not construct directly.

    .. cpp:member:: UniqueHandle<AppWindowManager> windowManager

        The single ``AppWindowManager`` for this application. Set up all
        windows through this object before calling ``start()``.

    .. cpp:function:: static AppInst *inst()

        Returns the global ``AppInst`` pointer. Only valid after
        ``OmegaWTKCreateApp`` has been called.

    .. cpp:function:: static int start()

        Enters the platform run loop. Blocks until ``terminate()`` is called
        or the last window closes. Returns the application exit code.

    .. cpp:function:: static void terminate()

        Requests orderly shutdown. Closes all managed windows and unblocks
        ``start()``.

**Full application entry example:**

.. code-block:: cpp

    #include <omegaWTK/AppEntryPoint.h>
    #include <omegaWTK/UI/App.h>
    #include <omegaWTK/UI/AppWindow.h>

    using namespace OmegaWTK;

    class MyWindowDelegate : public AppWindowDelegate {
    public:
        void windowWillClose(Native::NativeEventPtr) override {
            AppInst::terminate();
        }
    };

    int omegaWTKMain(AppInst *app) {
        Core::Rect windowRect{{0, 0}, 800, 600};

        auto window = make<AppWindow>(windowRect, new MyWindowDelegate());
        window->setTitle("My App");

        // ... attach root widget ...

        app->windowManager->setRootWindow(window);
        app->windowManager->displayRootWindow();

        return AppInst::start();
    }

----

App Window
----------

``AppWindow``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::AppWindow

    A top-level OS window. Holds a root widget, an optional menu bar
    (desktop), and exposes dialog APIs.

    .. cpp:function:: explicit AppWindow(Core::Rect rect, AppWindowDelegate *delegate = nullptr)

        Constructs the window at the given screen rect. ``delegate`` receives
        resize and close events; pass ``nullptr`` if not needed.

    .. cpp:function:: void setTitle(OmegaCommon::StrRef title)

        Sets the window title bar string.

    .. cpp:function:: void setRootWidget(WidgetPtr widget)

        Attaches the root widget to the window. The widget's rect is stretched
        to fill the window's content area. Call this before
        ``displayRootWindow()``.

    .. cpp:function:: void setMenu(SharedHandle<Menu> &menu)

        *(Desktop only.)* Attaches a ``Menu`` hierarchy as the window's menu
        bar. On macOS the menu bar is shared across all windows.

    .. cpp:function:: void setEnableWindowHeader(bool enable)

        *(Desktop only.)* Shows or hides the native title bar / chrome.
        Set to ``false`` for fully custom window frames.

    .. cpp:function:: void close()

        Programmatically closes the window and triggers
        ``AppWindowDelegate::windowWillClose``.

    .. cpp:function:: SharedHandle<Native::NativeFSDialog> openFSDialog(const Native::NativeFSDialog::Descriptor &desc)

        Opens a platform file-system dialog (open or save). Returns a handle
        whose ``getResult()`` async method yields the selected path.

    .. cpp:function:: SharedHandle<Native::NativeNoteDialog> openNoteDialog(const Native::NativeNoteDialog::Descriptor &desc)

        Opens a platform alert/note dialog with a title and message string.

----

``AppWindowManager``
~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::AppWindowManager

    Tracks the window hierarchy and controls display order.

    .. cpp:function:: WindowIndex addWindow(AppWindowPtr handle)

        Adds a window to the manager. The order of insertion determines the
        stacking priority. Returns the window's index.

    .. cpp:function:: void setRootWindow(AppWindowPtr handle)

        Designates one window as the primary (root) window.

    .. cpp:function:: AppWindowPtr getRootWindow()

        Returns the current root window.

    .. cpp:function:: void displayRootWindow()

        Makes the root window visible on screen. Call this after all widgets
        and menus have been attached.

----

``AppWindowDelegate``
~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::AppWindowDelegate

    Interface for responding to window-level native events.

    .. cpp:function:: virtual void windowWillClose(Native::NativeEventPtr event)

        Called when the OS is about to close the window (e.g. the user clicked
        the close button). The typical action is ``AppInst::terminate()``.

    .. cpp:function:: virtual void windowWillResize(Core::Rect &nRect)

        Called during a live resize. ``nRect`` contains the new proposed
        dimensions. Modify ``nRect`` to constrain the resize.

----

Core Types
----------

.. cpp:namespace:: OmegaWTK::Core

The geometry types used throughout OmegaWTK live in ``OmegaWTK::Core`` and
are typedefs of their OmegaGTE equivalents.

.. cpp:struct:: Position

    A 2D screen position ``{float x, float y}``.

.. cpp:struct:: Rect

    An axis-aligned rectangle ``{Position pos, float w, float h}``.

    .. code-block:: cpp

        Core::Rect r{{10.f, 20.f}, 300.f, 200.f};
        // x=10, y=20, width=300, height=200

.. cpp:struct:: RoundedRect

    A ``Rect`` extended with per-corner radii::

        Core::RoundedRect rr{{{0,0}, 200, 60}, 8.f};   // uniform radius

.. cpp:struct:: Ellipse

    An ellipse defined by a bounding rect.

.. cpp:namespace:: OmegaWTK

----

Widget
------

``Widget`` (base class)
~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Widget

    The base class for every UI component. A Widget owns exactly one root
    ``View``, manages paint scheduling, participates in the layout pass, and
    observes platform theme changes.

    **Construction**

    Subclasses call one of the two protected constructors:

    * ``Widget(Core::Rect rect)`` — creates a new ``CanvasView`` sized to ``rect``.
    * ``Widget(ViewPtr view)`` — wraps an existing View.

    The ``WIDGET_CONSTRUCTOR(...)`` macro declares a public static ``Create``
    factory for concrete subclasses.

    **Geometry**

    .. cpp:function:: Core::Rect &rect()

        Returns a reference to the widget's current bounding rect (in window
        coordinates).

    .. cpp:function:: void setRect(const Core::Rect &newRect)

        Immediately sets the widget's rect without animation. Triggers a
        paint if the size changed.

    .. cpp:function:: bool requestRect(const Core::Rect &requested, GeometryChangeReason reason)

        Requests a geometry change from the widget's parent (or tree host if
        root). Returns ``true`` if the parent accepted the new rect. Prefer
        this over ``setRect`` when the widget is inside a managed layout.

    **Layout**

    .. cpp:function:: void setLayoutStyle(const LayoutStyle &style)

        Attaches a ``LayoutStyle`` describing how the layout engine should
        size and position this widget relative to its siblings.

    .. cpp:function:: void requestLayout()

        Marks the widget's subtree as needing a layout pass. Scheduled
        asynchronously; coalesced with any pending paints.

    **Paint**

    .. cpp:function:: void setPaintMode(PaintMode mode)

        ``PaintMode::Automatic`` (default) — the toolkit calls ``onPaint``
        whenever the widget is invalidated. ``PaintMode::Manual`` — the
        application is responsible for calling ``invalidate`` to schedule
        repaints.

    .. cpp:function:: void invalidate(PaintReason reason)

        Schedules a repaint on the next frame. Safe to call from any thread.

    .. cpp:function:: void invalidateNow(PaintReason reason)

        Forces an immediate synchronous repaint. Use sparingly; prefer
        ``invalidate`` for normal state changes.

    **Visibility**

    .. cpp:function:: void show()

        Makes the widget visible if it was hidden.

    .. cpp:function:: void hide()

        Hides the widget without removing it from the hierarchy.

    **Observers**

    .. cpp:function:: void addObserver(WidgetObserverPtr observer)

        Registers an observer to receive attach, detach, resize, show, and
        hide callbacks.

    .. cpp:function:: void removeObserver(WidgetObserverPtr observer)

        Removes a previously registered observer.

    **Subclass overrides**

    .. cpp:function:: virtual void onMount()

        Called once when the widget is first attached to a live widget tree
        and its GPU resources are ready. Perform one-time initialisation here.

    .. cpp:function:: virtual void onPaint(PaintReason reason)

        Called when the widget needs to redraw. Issue composition commands
        inside a composition session here.

    .. cpp:function:: virtual MeasureResult measureSelf(const LayoutContext &ctx)

        Override to report the widget's intrinsic size in dp units. The
        layout engine calls this during the measure pass.

    .. cpp:function:: virtual void onLayoutResolved(const Core::Rect &finalRectPx)

        Called after the layout engine has committed a final pixel rect for
        the widget. Use this to update sub-views that depend on the resolved
        size.

**Implementing a custom widget:**

.. code-block:: cpp

    class MyWidget : public OmegaWTK::Widget {
        SharedHandle<Composition::Font> font_;
    public:
        explicit MyWidget(Core::Rect rect)
            : Widget(rect) {}

        void onMount() override {
            FontDescriptor fd{"Arial", 14};
            font_ = FontEngine::inst()->CreateFont(fd);
        }

        void onPaint(PaintReason) override {
            auto & cv = viewAs<CanvasView>();
            cv.clear(Composition::Color::create8Bit(0x1E1E1E, 0xFF));
            cv.drawText(U"Hello WTK", font_,
                        rect(),
                        Composition::Color::create8Bit(Composition::Color::White8));
        }
    };

----

``WidgetObserver``
~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::WidgetObserver

    Receives lifecycle and geometry notifications for a single Widget.
    Subclass and override the methods you need; all have empty default
    implementations.

    .. cpp:function:: virtual void onWidgetAttach(WidgetPtr parent)

        Called when the observed widget is added to a widget tree.

    .. cpp:function:: virtual void onWidgetDetach(WidgetPtr parent)

        Called when the widget is removed from a widget tree.

    .. cpp:function:: virtual void onWidgetChangeSize(Core::Rect oldRect, Core::Rect &newRect)

        Called when the widget's bounding rect changes. ``newRect`` can be
        modified to veto or clamp the resize.

    .. cpp:function:: virtual void onWidgetDidHide()

        Called after the widget transitions to the hidden state.

    .. cpp:function:: virtual void onWidgetDidShow()

        Called after the widget transitions to the visible state.

----

``WidgetState<T>`` and ``WidgetStateObserver``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: template<class Ty> OmegaWTK::WidgetState

    A typed reactive state container. Multiple observers are notified
    synchronously whenever ``setValue`` is called.

    .. cpp:function:: static SharedHandle<WidgetState<Ty>> Create(Core::Optional<Ty> initialValue = {})

        Creates a new state container with an optional initial value.

    .. cpp:function:: void setValue(Ty newVal)

        Updates the value and notifies all registered observers.

    .. cpp:function:: void addObserver(WidgetStateObserver<WidgetState<Ty>> *observer)

        Registers an observer pointer (non-owning).

    .. cpp:function:: void removeObserver(WidgetStateObserver<WidgetState<Ty>> *observer)

        Removes a registered observer.

**Example — button enabled state:**

.. code-block:: cpp

    auto enabledState = WidgetState<bool>::Create(true);

    class MyButtonObserver : public WidgetStateObserver<WidgetState<bool>> {
        MyButton *button;
    public:
        explicit MyButtonObserver(MyButton *b) : button(b) {}
        void stateHasChanged(bool &newVal) override {
            newVal ? button->show() : button->hide();
        }
    };

    MyButtonObserver obs(myButton.get());
    enabledState->addObserver(&obs);
    enabledState->setValue(false);  // button hides immediately

----

Layout
------

.. cpp:namespace:: OmegaWTK

Length Units
~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::LayoutLength

    A CSS-inspired length value with an associated unit.

    +-----------------------------+--------------------------------------------------+
    | Factory                     | Meaning                                          |
    +=============================+==================================================+
    | ``LayoutLength::Auto()``    | Size determined by the layout engine.            |
    +-----------------------------+--------------------------------------------------+
    | ``LayoutLength::Px(v)``     | Absolute pixels.                                 |
    +-----------------------------+--------------------------------------------------+
    | ``LayoutLength::Dp(v)``     | Device-independent points (scaled by DPI).       |
    +-----------------------------+--------------------------------------------------+
    | ``LayoutLength::Percent(v)``| Fraction of the available parent dimension.      |
    +-----------------------------+--------------------------------------------------+
    | ``LayoutLength::Fr(v)``     | Fractional unit for grid/flex distributions.     |
    +-----------------------------+--------------------------------------------------+
    | ``LayoutLength::Intrinsic()``| Widget's own measured size.                     |
    +-----------------------------+--------------------------------------------------+

.. cpp:struct:: OmegaWTK::LayoutEdges

    Inset or outset amounts for all four sides of a box.

    .. cpp:function:: static LayoutEdges Zero()
    .. cpp:function:: static LayoutEdges All(LayoutLength value)
    .. cpp:function:: static LayoutEdges Symmetric(LayoutLength horizontal, LayoutLength vertical)

.. cpp:struct:: OmegaWTK::LayoutClamp

    Minimum and maximum constraints applied after the normal layout pass.

----

``LayoutStyle``
~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::LayoutStyle

    Describes how the layout engine sizes and positions a single widget node.
    Attach to a widget via ``Widget::setLayoutStyle()``.

    **Size and position**

    +--------------------------+----------------------------------------------+
    | Field                    | Default                                      |
    +==========================+==============================================+
    | ``width``, ``height``    | ``Auto``                                     |
    +--------------------------+----------------------------------------------+
    | ``clamp``                | unconstrained                                |
    +--------------------------+----------------------------------------------+
    | ``margin``               | zero                                         |
    +--------------------------+----------------------------------------------+
    | ``padding``              | zero                                         |
    +--------------------------+----------------------------------------------+
    | ``position``             | ``Flow`` (participates in normal flow)       |
    +--------------------------+----------------------------------------------+
    | ``insetLeft/Top/…``      | ``Auto`` (used with ``Absolute`` position)   |
    +--------------------------+----------------------------------------------+

    **Display mode**

    ``display`` selects the container algorithm for child widgets:

    * ``Stack`` — children stack along the container's axis (default).
    * ``Flex``  — flexbox-style row or column layout.
    * ``Grid``  — grid-track layout.
    * ``Overlay`` — children share the same rect (z-stacked).
    * ``Custom`` — custom ``LayoutBehavior`` plugin.

    **Flex fields** (used when ``display == Flex``)

    ``flexDirection``, ``flexWrap``, ``justifyContent``, ``alignItems``,
    ``flexGrow``, ``flexShrink``, ``gap``.

.. code-block:: cpp

    LayoutStyle s;
    s.display = LayoutDisplay::Flex;
    s.flexDirection = FlexDirection::Row;
    s.justifyContent = LayoutAlign::Center;
    s.alignItems = LayoutAlign::Center;
    s.padding = LayoutEdges::All(LayoutLength::Dp(12.f));
    myWidget->setLayoutStyle(s);

----

``LayoutTransitionSpec``
~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::LayoutTransitionSpec

    Describes an animated transition for layout property changes.

    .. cpp:member:: bool enabled

        Must be ``true`` for the transition to fire.

    .. cpp:member:: float durationSec

        Duration in seconds.

    .. cpp:member:: SharedHandle<Composition::AnimationCurve> curve

        The easing curve. Use ``AnimationCurve::EaseInOut()`` for smooth
        transitions.

    .. cpp:member:: OmegaCommon::Vector<LayoutTransitionProperty> properties

        The set of properties to animate. Values:
        ``X``, ``Y``, ``Width``, ``Height``, ``Opacity``,
        ``CornerRadius``, ``Shadow``, ``Blur``.

.. code-block:: cpp

    LayoutTransitionSpec spec;
    spec.enabled = true;
    spec.durationSec = 0.25f;
    spec.curve = Composition::AnimationCurve::EaseInOut();
    spec.properties = {LayoutTransitionProperty::Width,
                       LayoutTransitionProperty::Height};
    myWidget->setLayoutStyle(style);  // style contains the transition spec

----

Containers
----------

``Container``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Container : public Widget

    Base class for widgets that manage a collection of child widgets.
    Subclass ``Container`` to build custom layout containers; or use the
    provided ``VStack`` / ``HStack`` for the common cases.

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr &child)

        Adds a widget to this container. The container's layout algorithm
        determines the child's final position and size.

    .. cpp:function:: bool removeChild(const WidgetPtr &child)

        Removes a child widget. Returns ``false`` if ``child`` is not found.

    .. cpp:function:: Widget *childAt(std::size_t idx) const

        Returns the child at position ``idx``, or ``nullptr`` if out of range.

    .. cpp:function:: std::size_t childCount() const

        Returns the number of direct children.

    .. cpp:function:: void setClampPolicy(const ContainerClampPolicy &policy)

        Configures how the container constrains child positions and sizes
        during layout.

    .. cpp:function:: void relayout()

        Forces an immediate synchronous layout pass for all children.

----

``StackWidget``, ``HStack``, ``VStack``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::StackWidget : public Container

    Lays children out sequentially along a single axis. Controls spacing,
    padding, main-axis alignment, and cross-axis alignment through
    ``StackOptions``. Each child may carry an optional ``StackSlot`` that
    overrides flex grow/shrink and margin per-child.

    .. cpp:function:: StackWidget(StackAxis axis, Core::Rect rect, const StackOptions &options = {})

        ``axis`` is ``StackAxis::Horizontal`` or ``StackAxis::Vertical``.

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr &child)

        Adds a child with default slot settings (no flex grow, default
        shrink).

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr &child, const StackSlot &slot)

        Adds a child with explicit flex and margin settings.

    .. cpp:function:: bool setSlot(const WidgetPtr &child, const StackSlot &slot)

        Updates the slot for an already-added child. Triggers relayout.

    .. cpp:function:: void relayout()

        Forces a synchronous layout pass.

.. cpp:struct:: OmegaWTK::StackOptions

    +-----------------------+--------------------------------------+------------+
    | Field                 | Meaning                              | Default    |
    +=======================+======================================+============+
    | ``spacing``           | Gap between children (dp).           | ``0``      |
    +-----------------------+--------------------------------------+------------+
    | ``padding``           | Insets on all four sides of the      | zero       |
    |                       | container.                           |            |
    +-----------------------+--------------------------------------+------------+
    | ``mainAlign``         | Distribution along the stack axis.   | ``Start``  |
    |                       | Values: ``Start``, ``Center``,       |            |
    |                       | ``End``, ``SpaceBetween``,           |            |
    |                       | ``SpaceAround``, ``SpaceEvenly``.    |            |
    +-----------------------+--------------------------------------+------------+
    | ``crossAlign``        | Alignment on the perpendicular axis. | ``Start``  |
    |                       | Values: ``Start``, ``Center``,       |            |
    |                       | ``End``, ``Stretch``.                |            |
    +-----------------------+--------------------------------------+------------+
    | ``clipOverflow``      | Clip children that exceed bounds.    | ``false``  |
    +-----------------------+--------------------------------------+------------+

.. cpp:struct:: OmegaWTK::StackSlot

    Per-child flex parameters. All fields are optional.

    +-----------------+------------------------------------------------------+
    | ``flexGrow``    | How much free space this child absorbs (default 0). |
    +-----------------+------------------------------------------------------+
    | ``flexShrink``  | How much this child shrinks when space is tight.    |
    +-----------------+------------------------------------------------------+
    | ``basis``       | Override preferred main-axis size (dp).             |
    +-----------------+------------------------------------------------------+
    | ``minMain``     | Minimum size along the stack axis (dp).             |
    +-----------------+------------------------------------------------------+
    | ``maxMain``     | Maximum size along the stack axis (dp).             |
    +-----------------+------------------------------------------------------+
    | ``margin``      | Per-child insets (overrides container padding).     |
    +-----------------+------------------------------------------------------+
    | ``alignSelf``   | Override cross-axis alignment for this child only.  |
    +-----------------+------------------------------------------------------+

.. cpp:class:: OmegaWTK::HStack : public StackWidget

    Convenience subclass: ``StackAxis::Horizontal`` stack.

.. cpp:class:: OmegaWTK::VStack : public StackWidget

    Convenience subclass: ``StackAxis::Vertical`` stack.

**Example — toolbar with a spacer:**

.. code-block:: cpp

    auto toolbar = make<HStack>(
        Core::Rect{{0,0}, 600.f, 48.f},
        StackOptions{
            .spacing = 8.f,
            .padding = {8.f, 4.f, 8.f, 4.f},
            .crossAlign = StackCrossAlign::Center
        });

    toolbar->addChild(iconButton,  StackSlot{.flexGrow = 0.f});
    toolbar->addChild(searchField, StackSlot{.flexGrow = 1.f}); // fills remaining space
    toolbar->addChild(menuButton,  StackSlot{.flexGrow = 0.f});

----

Primitive Widgets
-----------------

All primitives live in ``omegaWTK/Widgets/Primatives.h``. Each takes a
``Core::Rect`` and a props struct. Call ``setProps()`` to update appearance
at runtime; the widget repaints automatically.

``Rectangle``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Rectangle : public Widget

    Draws a filled and/or stroked axis-aligned rectangle.

.. cpp:struct:: OmegaWTK::RectangleProps

    .. cpp:member:: SharedHandle<Composition::Brush> fill

        Fill brush. ``nullptr`` = transparent.

    .. cpp:member:: SharedHandle<Composition::Brush> stroke

        Stroke brush. ``nullptr`` = no stroke.

    .. cpp:member:: float strokeWidth

        Stroke width in dp.

.. code-block:: cpp

    auto rect = make<Rectangle>(
        Core::Rect{{0,0}, 100.f, 60.f},
        RectangleProps{
            .fill = ColorBrush(Color::create8Bit(Color::Blue8)),
            .stroke = ColorBrush(Color::create8Bit(Color::White8)),
            .strokeWidth = 2.f
        });

----

``RoundedRectangle``
~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::RoundedRectangle : public Widget

    Draws a rectangle with independently controllable per-corner radii.

.. cpp:struct:: OmegaWTK::RoundedRectangleProps

    Extends ``RectangleProps`` with ``topLeft``, ``topRight``,
    ``bottomLeft``, ``bottomRight`` corner radii (dp).

.. code-block:: cpp

    auto card = make<RoundedRectangle>(
        Core::Rect{{0,0}, 200.f, 120.f},
        RoundedRectangleProps{
            .fill = ColorBrush(Color::create8Bit(0x2A2A2A, 0xFF)),
            .topLeft = 12.f, .topRight = 12.f,
            .bottomLeft = 12.f, .bottomRight = 12.f
        });

----

``Ellipse``
~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Ellipse : public Widget

    Draws a filled and/or stroked ellipse inscribed in the widget's rect.

.. cpp:struct:: OmegaWTK::EllipseProps

    ``fill``, ``stroke``, ``strokeWidth`` — same semantics as
    ``RectangleProps``.

.. code-block:: cpp

    auto dot = make<Ellipse>(
        Core::Rect{{0,0}, 24.f, 24.f},
        EllipseProps{.fill = ColorBrush(Color::create8Bit(Color::Green8))});

----

``Path``
~~~~~~~~

.. cpp:class:: OmegaWTK::Path : public Widget

    Draws an arbitrary 2D vector path.

.. cpp:struct:: OmegaWTK::PathProps

    .. cpp:member:: OmegaGTE::GVectorPath2D path

        The path geometry.

    .. cpp:member:: SharedHandle<Composition::Brush> fill

        Fill brush (applied to the interior when ``closePath`` is true).

    .. cpp:member:: SharedHandle<Composition::Brush> stroke

        Stroke brush.

    .. cpp:member:: unsigned strokeWidth

        Stroke width in pixels.

    .. cpp:member:: bool closePath

        If ``true``, a line is drawn from the last point back to the first.

----

``Separator``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Separator : public Widget

    Draws a thin horizontal or vertical rule.

.. cpp:struct:: OmegaWTK::SeparatorProps

    .. cpp:member:: Orientation orientation

        ``Orientation::Horizontal`` or ``Orientation::Vertical``.

    .. cpp:member:: float thickness

        Rule width in dp (default ``1.f``).

    .. cpp:member:: float inset

        How far from each end the rule is inset (dp).

    .. cpp:member:: SharedHandle<Composition::Brush> brush

        Fill brush for the rule.

.. code-block:: cpp

    auto divider = make<Separator>(
        Core::Rect{{0,0}, 400.f, 2.f},
        SeparatorProps{
            .orientation = Orientation::Horizontal,
            .thickness = 1.f,
            .brush = ColorBrush(Color::create8Bit(0x444444))
        });

----

``Label``
~~~~~~~~~

.. cpp:class:: OmegaWTK::Label : public Widget

    Renders a static or dynamic Unicode text string.

.. cpp:struct:: OmegaWTK::LabelProps

    .. cpp:member:: OmegaCommon::UString text

        The text content (UTF-32 string, use the ``U""`` literal prefix).

    .. cpp:member:: SharedHandle<Composition::Font> font

        Font to render with. Obtain from ``FontEngine::inst()->CreateFont()``.

    .. cpp:member:: Composition::Color textColor

        Text color (default opaque black).

    .. cpp:member:: TextLayoutDescriptor::Alignment alignment

        Horizontal and vertical anchor. Values: ``LeftUpper``,
        ``LeftCenter``, ``LeftLower``, ``MiddleUpper``, ``MiddleCenter``,
        ``MiddleLower``, ``RightUpper``, ``RightCenter``, ``RightLower``.

    .. cpp:member:: TextLayoutDescriptor::Wrapping wrapping

        ``None``, ``WrapByWord``, or ``WrapByCharacter``.

    .. cpp:member:: unsigned lineLimit

        Maximum number of lines to render (``0`` = unlimited).

    .. cpp:function:: void setText(const OmegaCommon::UString &text)

        Updates the displayed text and schedules a repaint.

.. code-block:: cpp

    FontDescriptor fd{"SF Pro", 16, FontDescriptor::Regular};
    auto font = FontEngine::inst()->CreateFont(fd);

    LabelProps props;
    props.text      = U"Hello, OmegaWTK";
    props.font      = font;
    props.textColor = Color::create8Bit(Color::White8);
    props.alignment = TextLayoutDescriptor::MiddleCenter;
    props.wrapping  = TextLayoutDescriptor::WrapByWord;

    auto label = make<Label>(Core::Rect{{0,0}, 300.f, 40.f}, props);

----

``Icon``
~~~~~~~~

.. cpp:class:: OmegaWTK::Icon : public Widget

    Renders a named icon token as a glyph.

.. cpp:struct:: OmegaWTK::IconProps

    .. cpp:member:: OmegaCommon::String token

        The icon token string (font-icon glyph name).

    .. cpp:member:: float size

        Glyph render size in dp (default ``16``).

    .. cpp:member:: Composition::Color tintColor

        Glyph color.

----

``Image``
~~~~~~~~~

.. cpp:class:: OmegaWTK::Image : public Widget

    Displays a ``BitmapImage`` with a configurable fit mode.

.. cpp:struct:: OmegaWTK::ImageProps

    .. cpp:member:: SharedHandle<Media::BitmapImage> source

        The bitmap to display.

    .. cpp:member:: ImageFitMode fitMode

        ``Contain``, ``Cover``, ``Fill``, ``Center``, or ``Crop``.

----

Views
-----

Views are the rendering layer beneath Widgets. Normally you do not interact
with Views directly — the Widget API is the preferred entry point. The View
API is needed when implementing custom widgets or low-level composition.

``View``
~~~~~~~~

.. cpp:class:: OmegaWTK::View

    The rendering substrate for a Widget. Owns a ``LayerTree`` (a root
    ``Layer`` plus optional sublayers), receives native input events, and
    mediates the composition session lifecycle.

    .. cpp:function:: static ViewPtr Create(const Core::Rect &rect, ViewPtr parent = nullptr)

        Public factory for widget subclass constructors.

    .. cpp:function:: SharedHandle<Composition::Layer> makeLayer(Core::Rect rect)

        Creates a new sublayer within this view's ``LayerTree``.

    .. cpp:function:: SharedHandle<Composition::Canvas> makeCanvas(SharedHandle<Composition::Layer> &targetLayer)

        Creates a ``Canvas`` targeting the given layer. One ``Canvas`` per
        ``Layer`` is the enforced invariant.

    .. cpp:function:: Core::Rect &getRect()

        Returns the current bounding rect of the view.

    .. cpp:function:: Composition::LayerTree *getLayerTree()

        Returns the ``LayerTree`` that owns this view's layers.

    .. cpp:function:: void startCompositionSession()

        Opens a composition window. All ``Canvas::draw*`` calls and
        ``LayerAnimator``/``ViewAnimator`` commands must happen between
        ``startCompositionSession()`` and ``endCompositionSession()``.

    .. cpp:function:: void endCompositionSession()

        Closes the composition window and submits all queued commands to
        the compositor.

    .. cpp:function:: void resize(Core::Rect newRect)

        Synchronously resizes the view. Use ``ViewAnimator`` for animated
        resize.

    .. cpp:function:: void enable()

        Makes the view visible.

    .. cpp:function:: void disable()

        Hides the view.

    .. cpp:function:: virtual void setDelegate(ViewDelegate *delegate)

        Registers a delegate to receive mouse and keyboard events.

----

``CanvasView``
~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::CanvasView : public View

    A ``View`` with an already-created ``Canvas`` on its root layer.
    Used by widgets that draw entirely through the Canvas API (most custom
    widgets). Exposes convenience draw methods that forward directly to the
    root canvas.

    .. cpp:function:: Composition::Canvas &rootCanvas()

        Returns the canvas bound to the root layer.

    .. cpp:function:: void clear(const Composition::Color &color)

        Clears the canvas to the given background color.

    .. cpp:function:: void drawRect(const Core::Rect &rect, const SharedHandle<Composition::Brush> &brush)
    .. cpp:function:: void drawRoundedRect(const Core::RoundedRect &rect, const SharedHandle<Composition::Brush> &brush)
    .. cpp:function:: void drawImage(const SharedHandle<Media::BitmapImage> &img, const Core::Rect &rect)
    .. cpp:function:: void drawText(const UniString &text, const SharedHandle<Composition::Font> &font, const Core::Rect &rect, const Composition::Color &color)
    .. cpp:function:: void drawText(const UniString &text, const SharedHandle<Composition::Font> &font, const Core::Rect &rect, const Composition::Color &color, const Composition::TextLayoutDescriptor &layoutDesc)

----

``UIView``
~~~~~~~~~~

.. cpp:class:: OmegaWTK::UIView : public View

    A View that renders from a declarative element layout (``UIViewLayout``)
    styled by a ``StyleSheet``. UIView is the rendering model for the
    built-in widget primitives (``Rectangle``, ``Label``, etc.).

    .. cpp:function:: void setLayout(const UIViewLayout &layout)

        Sets the element list to render. Each element has a tag, a shape or
        text content, and an optional text rect.

    .. cpp:function:: void setStyleSheet(const StyleSheetPtr &style)

        Attaches a stylesheet. Changes take effect on the next ``update()``.

    .. cpp:function:: void update()

        Commits any pending layout or style changes and schedules a
        compositor frame.

    .. cpp:function:: void setLayoutV2(const UIViewLayoutV2 &layout)

        Alternative layout path with full ``LayoutStyle`` per element and
        z-index ordering.

----

``UIViewLayout``
~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::UIViewLayout

    A flat list of shape and text elements identified by ``UIElementTag``
    strings.

    .. cpp:function:: void text(UIElementTag tag, OmegaCommon::UString content)

        Adds or replaces a text element with the given tag.

    .. cpp:function:: void text(UIElementTag tag, OmegaCommon::UString content, const Core::Rect &rect)

        Same as above with an explicit bounding rect.

    .. cpp:function:: void shape(UIElementTag tag, const Shape &shape)

        Adds or replaces a shape element.

    .. cpp:function:: bool remove(UIElementTag tag)

        Removes the element with the given tag. Returns ``false`` if not found.

    .. cpp:function:: void clear()

        Removes all elements.

----

``StyleSheet``
~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::StyleSheet

    A CSS-inspired set of style declarations applied to ``UIView`` elements
    by ``UIElementTag``. All methods return ``StyleSheetPtr`` (a
    ``SharedHandle<StyleSheet>``), enabling method chaining.

    .. cpp:function:: static StyleSheetPtr Create()

        Returns an empty stylesheet.

    .. cpp:function:: StyleSheetPtr backgroundColor(UIViewTag tag, const Composition::Color &color, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr border(UIViewTag tag, bool use)
    .. cpp:function:: StyleSheetPtr borderColor(UIViewTag tag, const Composition::Color &color, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr borderWidth(UIViewTag tag, float width, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr dropShadow(UIViewTag tag, const Composition::LayerEffect::DropShadowParams &params, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr gaussianBlur(UIViewTag tag, float radius, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr directionalBlur(UIViewTag tag, float radius, float angle, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr elementBrush(UIElementTag elementTag, SharedHandle<Composition::Brush> brush, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr textFont(UIElementTag elementTag, SharedHandle<Composition::Font> font)
    .. cpp:function:: StyleSheetPtr textColor(UIElementTag elementTag, const Composition::Color &color, bool transition = false, float duration = 0.f)
    .. cpp:function:: StyleSheetPtr textAlignment(UIElementTag elementTag, Composition::TextLayoutDescriptor::Alignment alignment)
    .. cpp:function:: StyleSheetPtr textWrapping(UIElementTag elementTag, Composition::TextLayoutDescriptor::Wrapping wrapping)
    .. cpp:function:: StyleSheetPtr layoutWidth(UIElementTag elementTag, LayoutLength width)
    .. cpp:function:: StyleSheetPtr layoutHeight(UIElementTag elementTag, LayoutLength height)
    .. cpp:function:: StyleSheetPtr layoutMargin(UIElementTag elementTag, LayoutEdges margin)
    .. cpp:function:: StyleSheetPtr layoutPadding(UIElementTag elementTag, LayoutEdges padding)
    .. cpp:function:: StyleSheetPtr layoutTransition(UIElementTag elementTag, LayoutTransitionSpec spec)

**Example — styled card with hover highlight:**

.. code-block:: cpp

    auto ss = StyleSheet::Create()
        ->backgroundColor("card", Color::create8Bit(0x2A2A2A, 0xFF))
        ->borderColor("card", Color::create8Bit(0x444444), true, 0.15f)
        ->dropShadow("card", {0.f, 4.f, 8.f, 6.f, 0.5f,
                              Color::create8Bit(0x000000, 0xFF)})
        ->textFont("title", titleFont)
        ->textColor("title", Color::create8Bit(Color::White8));

    uiView->setStyleSheet(ss);

----

``ViewDelegate``
~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::ViewDelegate

    Interface for handling mouse and keyboard events from a ``View``.
    Attach via ``View::setDelegate()``. Override only the events you need.

    .. cpp:function:: virtual void onMouseEnter(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onMouseExit(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onLeftMouseDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onLeftMouseUp(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onRightMouseDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onRightMouseUp(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onKeyDown(Native::NativeEventPtr event)
    .. cpp:function:: virtual void onKeyUp(Native::NativeEventPtr event)

**Example — click handler:**

.. code-block:: cpp

    class ButtonDelegate : public ViewDelegate {
        std::function<void()> callback_;
    public:
        explicit ButtonDelegate(std::function<void()> cb)
            : callback_(std::move(cb)) {}

        void onLeftMouseUp(Native::NativeEventPtr) override {
            callback_();
        }
    };

    view->setDelegate(new ButtonDelegate([&]{ doAction(); }));

----

``ScrollView``
~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::ScrollView : public View

    Wraps a single child view that may be larger than the scroll view's
    visible bounds. Clips the child to its own rect and provides native
    scroll bars (customisable).

----

``VideoView``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::VideoView : public View, public Media::FrameSink

    Displays video frames delivered through the ``FrameSink`` interface.
    Attach as the frame sink of a ``VideoPlaybackSession`` or
    ``VideoCaptureSession``.

----

``SVGView``
~~~~~~~~~~~

.. cpp:class:: OmegaWTK::SVGView : public View

    Renders a static or dynamic SVG graphic. Accepts the output of an
    ``SVGSession``.

----

Composition
-----------

All composition types live in ``OmegaWTK::Composition``.

``Color``
~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::Color

    A four-channel (RGBA) color with ``float`` channel storage. Construct
    via the static factory methods.

    .. cpp:function:: static Color create8Bit(uint8_t r, uint8_t g, uint8_t b, uint8_t a)

        Creates a color from 8-bit per-channel values.

    .. cpp:function:: static Color create8Bit(uint32_t rgb, uint8_t alpha = 0xFF)

        Creates a color from a packed 24-bit RGB hex value and an 8-bit
        alpha.

    .. cpp:function:: static Color create16Bit(uint16_t r, uint16_t g, uint16_t b, uint16_t a)

        Creates a color from 16-bit per-channel values (HDR range).

    .. cpp:function:: static Color create32Bit(uint32_t r, uint32_t g, uint32_t b, uint32_t a)

        Creates a color from 32-bit per-channel values (wide gamut).

    **Named 8-bit constants:** ``Black8``, ``White8``, ``Red8``, ``Green8``,
    ``Blue8``, ``Yellow8``, ``Orange8``, ``Purple8``.

    **Named 16-bit constants:** same names with ``16`` suffix.

    **Special:** ``Color::Transparent``.

.. code-block:: cpp

    auto red   = Color::create8Bit(Color::Red8);
    auto white = Color::create8Bit(0xFF, 0xFF, 0xFF, 0xFF);
    auto navy  = Color::create8Bit(0x002244, 0xFF);

----

``Gradient``
~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::Gradient

    Describes a linear or radial color gradient.

    .. cpp:function:: static Gradient Linear(std::initializer_list<GradientStop> stops, float angle)

        Creates a linear gradient at the given angle (degrees).

    .. cpp:function:: static Gradient Radial(std::initializer_list<GradientStop> stops, float radius)

        Creates a radial gradient with the given radius.

    .. cpp:function:: static GradientStop Stop(float pos, Color color)

        Creates a gradient stop at normalised position ``pos`` (``0..1``).

.. code-block:: cpp

    auto grad = Gradient::Linear({
        Gradient::Stop(0.f, Color::create8Bit(Color::Blue8)),
        Gradient::Stop(1.f, Color::create8Bit(Color::Purple8))
    }, 90.f);

----

``Brush``
~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::Brush

    A fill descriptor passed to every draw call. Created via the free
    functions below; do not construct directly.

.. cpp:function:: Core::SharedPtr<Brush> OmegaWTK::Composition::ColorBrush(const Color &color)

    Creates a solid-color brush.

.. cpp:function:: Core::SharedPtr<Brush> OmegaWTK::Composition::GradientBrush(const Gradient &gradient)

    Creates a gradient brush.

----

``Layer``
~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::Layer

    A rectangular GPU surface within a ``LayerTree``. Exactly one ``Canvas``
    may be bound to a layer at a time.

    .. cpp:function:: void resize(Core::Rect &newRect)

        Resizes the layer. Triggers a ``LayerTree`` resize notification.

    .. cpp:function:: Core::Rect &getLayerRect()

        Returns the current layer bounds.

    .. cpp:function:: void setEnabled(bool state)

        Shows (``true``) or hides (``false``) the layer.

    .. cpp:function:: bool hasCanvas() const

        Returns ``true`` if a canvas is currently bound.

    .. cpp:function:: bool isChildLayer() const

        Returns ``true`` if this layer was added as a sublayer.

----

``LayerTree``
~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::LayerTree

    Owns the root layer and all sublayers for a single view. Notifies
    ``LayerTreeObserver`` instances of structural changes.

    .. cpp:function:: SharedHandle<Layer> &getRootLayer()

        Returns the root layer.

    .. cpp:function:: void addLayer(SharedHandle<Layer> layer)

        Adds a child layer (sublayer of the root).

    .. cpp:function:: void enable() / void disable()

        Shows or hides the entire tree.

    .. cpp:function:: void addObserver(LayerTreeObserver *observer)
    .. cpp:function:: void removeObserver(LayerTreeObserver *observer)

----

``Canvas``
~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::Canvas

    Issues 2D draw commands into a ``CanvasFrame`` that is submitted to the
    compositor when ``sendFrame()`` is called. All draw methods must be
    called inside a composition session on the owning view.

    .. cpp:function:: void setBackground(const Color &color)

        Sets the frame background color. Call before any draw commands to
        establish the base color.

    .. cpp:function:: void clear(Core::Optional<Color> color = std::nullopt)

        Discards all pending draw commands. If a color is provided it
        becomes the new background.

    .. cpp:function:: void drawRect(Core::Rect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border = std::nullopt)

        Draws a filled rectangle, with an optional border.

    .. cpp:function:: void drawRoundedRect(Core::RoundedRect &rect, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border = std::nullopt)

        Draws a filled rounded rectangle.

    .. cpp:function:: void drawEllipse(Core::Ellipse &ellipse, Core::SharedPtr<Brush> &brush, Core::Optional<Border> border = std::nullopt)

        Draws a filled ellipse.

    .. cpp:function:: void drawPath(Path &path)

        Draws a vector path.

    .. cpp:function:: void drawText(const UniString &text, Core::SharedPtr<Font> font, const Core::Rect &rect, const Color &color)

        Draws text into a bounding rect with default (top-left, no-wrap)
        layout.

    .. cpp:function:: void drawText(const UniString &text, Core::SharedPtr<Font> font, const Core::Rect &rect, const Color &color, const TextLayoutDescriptor &layoutDesc)

        Draws text with explicit alignment and wrapping settings.

    .. cpp:function:: void drawImage(SharedHandle<Media::BitmapImage> &img, const Core::Rect &rect)

        Draws a bitmap image scaled to the given rect.

    .. cpp:function:: void drawGETexture(SharedHandle<OmegaGTE::GETexture> &img, const Core::Rect &rect, SharedHandle<OmegaGTE::GEFence> fence = nullptr)

        Draws a GTE texture directly. ``fence`` is used to synchronise GPU
        producers (e.g. a GTE render pass) with the compositor.

    .. cpp:function:: void drawShadow(Core::Rect &rect, const LayerEffect::DropShadowParams &shadow)

        Draws an inline drop shadow for a rect shape.

    .. cpp:function:: void drawShadow(Core::RoundedRect &rect, const LayerEffect::DropShadowParams &shadow)
    .. cpp:function:: void drawShadow(Core::Ellipse &ellipse, const LayerEffect::DropShadowParams &shadow)

    .. cpp:function:: void setElementTransform(const OmegaGTE::FMatrix<4,4> &matrix)

        Sets a 4×4 transform applied to all subsequent draw calls. Reset
        with ``FMatrix<4,4>::Identity()``.

    .. cpp:function:: void setElementOpacity(float opacity)

        Sets per-element opacity (``0..1``) for subsequent draw calls.

    .. cpp:function:: void applyEffect(SharedHandle<CanvasEffect> &effect)

        Applies a ``GaussianBlur`` or ``DirectionalBlur`` effect to the
        entire frame.

    .. cpp:function:: void applyLayerEffect(const SharedHandle<LayerEffect> &effect)

        Applies a layer-level effect (``DropShadow`` or ``Transformation``)
        to this canvas' target layer.

    .. cpp:function:: void sendFrame()

        Submits the current frame to the compositor. Must be called before
        ``endCompositionSession()``.

    .. cpp:function:: SharedHandle<CanvasFrame> getCurrentFrame()

        Returns the current frame without advancing state.

    .. cpp:function:: SharedHandle<CanvasFrame> nextFrame()

        Returns the current frame and resets canvas state for the next frame.

**Full composition example:**

.. code-block:: cpp

    void onPaint(PaintReason) override {
        auto & cv = viewAs<CanvasView>();

        auto & canvas = cv.rootCanvas();
        auto  brush   = ColorBrush(Color::create8Bit(0x3A8DFF, 0xFF));
        auto  white   = ColorBrush(Color::create8Bit(Color::White8));
        Border border{white, 2};

        view->startCompositionSession();

        canvas.setBackground(Color::create8Bit(0x1A1A1A, 0xFF));
        canvas.drawRoundedRect(
            Core::RoundedRect{rect(), 8.f},
            brush,
            border);
        canvas.drawText(U"Click me", font_, rect(),
                        Color::create8Bit(Color::White8));
        canvas.sendFrame();

        view->endCompositionSession();
    }

----

``LayerEffect``
~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::LayerEffect

    A layer-level effect applied by the compositor after the canvas frame
    is composited. Two types:

    **DropShadow** — ``LayerEffect::DropShadowParams``

    +-----------------+-------------------------------------------+
    | ``x_offset``    | Horizontal shadow offset (dp).            |
    +-----------------+-------------------------------------------+
    | ``y_offset``    | Vertical shadow offset (dp).              |
    +-----------------+-------------------------------------------+
    | ``radius``      | Shadow spread radius (dp).                |
    +-----------------+-------------------------------------------+
    | ``blurAmount``  | Gaussian blur radius applied to shadow.   |
    +-----------------+-------------------------------------------+
    | ``opacity``     | Shadow opacity ``[0..1]``.                |
    +-----------------+-------------------------------------------+
    | ``color``       | Shadow color.                             |
    +-----------------+-------------------------------------------+

    **Transformation** — ``LayerEffect::TransformationParams``

    Contains ``translate {x, y, z}``, ``rotate {pitch, yaw, roll}``, and
    ``scale {x, y, z}`` sub-structs.

----

Animation
---------

All animation types live in ``OmegaWTK::Composition``.

``AnimationCurve``
~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::AnimationCurve

    A 1D easing function mapping normalised time ``[0,1]`` to a normalised
    interpolant ``[0,1]``. Three curve types are supported: ``Linear``,
    ``QuadraticBezier``, and ``CubicBezier``.

    .. cpp:function:: static SharedHandle<AnimationCurve> Linear()

        Constant-rate linear interpolation.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseIn()

        Starts slow, ends at full speed.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseOut()

        Starts at full speed, ends slow.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseInOut()

        Starts and ends slow, peaks in the middle.

    .. cpp:function:: static SharedHandle<AnimationCurve> Quadratic(OmegaGTE::GPoint2D a)

        Quadratic Bézier with one control point.

    .. cpp:function:: static SharedHandle<AnimationCurve> Cubic(OmegaGTE::GPoint2D a, OmegaGTE::GPoint2D b)
    .. cpp:function:: static SharedHandle<AnimationCurve> CubicBezier(OmegaGTE::GPoint2D a, OmegaGTE::GPoint2D b, float start_h = 0.f, float end_h = 1.f)

        Cubic Bézier with two control points.

    .. cpp:function:: float sample(float t) const

        Evaluates the curve at normalised time ``t``.

----

``TimingOptions``
~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::TimingOptions

    Controls animation playback.

    +-----------------------+-----------------------------------------------+-----------+
    | Field                 | Meaning                                       | Default   |
    +=======================+===============================================+===========+
    | ``durationMs``        | Total animation duration in milliseconds.     | ``300``   |
    +-----------------------+-----------------------------------------------+-----------+
    | ``delayMs``           | Delay before the animation starts.            | ``0``     |
    +-----------------------+-----------------------------------------------+-----------+
    | ``playbackRate``      | Speed multiplier (``2.0`` = double speed).    | ``1.0``   |
    +-----------------------+-----------------------------------------------+-----------+
    | ``iterations``        | How many times to repeat (``INFINITY`` loops).| ``1.0``   |
    +-----------------------+-----------------------------------------------+-----------+
    | ``fillMode``          | ``None``, ``Forwards``, ``Backwards``,        | ``Forwards``|
    |                       | ``Both``.                                     |           |
    +-----------------------+-----------------------------------------------+-----------+
    | ``direction``         | ``Normal``, ``Reverse``, ``Alternate``,       | ``Normal``|
    |                       | ``AlternateReverse``.                         |           |
    +-----------------------+-----------------------------------------------+-----------+
    | ``frameRateHint``     | Target frame rate hint for the compositor.    | ``60``    |
    +-----------------------+-----------------------------------------------+-----------+
    | ``clockMode``         | ``WallClock``, ``PresentedClock``,            | ``Hybrid``|
    |                       | ``Hybrid``.                                   |           |
    +-----------------------+-----------------------------------------------+-----------+

----

``AnimationHandle``
~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::AnimationHandle

    An opaque handle returned by every ``animate()`` call. Used to inspect
    state and control playback.

    .. cpp:function:: AnimationState state() const

        Current state: ``Pending``, ``Running``, ``Paused``,
        ``Completed``, ``Cancelled``, ``Failed``.

    .. cpp:function:: float progress() const

        Normalised animation progress ``[0..1]``.

    .. cpp:function:: void pause()
    .. cpp:function:: void resume()
    .. cpp:function:: void cancel()
    .. cpp:function:: void seek(float normalized)

        Jumps to a normalised time position.

    .. cpp:function:: void setPlaybackRate(float rate)

        Changes the playback speed without restarting the animation.

    .. cpp:function:: bool valid() const

        Returns ``false`` if the handle was default-constructed or the
        animation was cancelled.

----

``KeyframeTrack<T>``
~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: template<typename T> OmegaWTK::Composition::KeyframeTrack

    A sorted list of typed keyframes with per-segment easing. Supported
    interpolation types: ``float``, ``Core::Rect``,
    ``LayerEffect::TransformationParams``, ``LayerEffect::DropShadowParams``.

    .. cpp:function:: static KeyframeTrack<T> From(const OmegaCommon::Vector<KeyframeValue<T>> &source)

        Constructs a track from a list of ``KeyframeValue<T>`` entries.
        Entries are sorted by ``offset`` and clamped to ``[0,1]``.

    .. cpp:function:: T sample(float t) const

        Returns the interpolated value at normalised time ``t``.

.. cpp:struct:: template<typename T> OmegaWTK::Composition::KeyframeValue

    A single keyframe: ``offset`` (normalised time), ``value``, and an
    optional ``easingToNext`` curve for the segment leading to the next key.

----

``LayerClip`` and ``ViewClip``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::LayerClip

    Aggregates the animation tracks for a single layer animation call.

    +-----------------+------------------------------------------+
    | ``rect``        | Animate the layer's bounding rect.       |
    +-----------------+------------------------------------------+
    | ``transform``   | Animate translation, rotation, scale.    |
    +-----------------+------------------------------------------+
    | ``shadow``      | Animate the drop shadow.                 |
    +-----------------+------------------------------------------+
    | ``opacity``     | Animate opacity.                         |
    +-----------------+------------------------------------------+

.. cpp:struct:: OmegaWTK::Composition::ViewClip

    Tracks for view-level animation: ``rect`` and ``opacity``.

----

``LayerAnimator``
~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::LayerAnimator

    Controls animations for a specific ``Layer``. Obtained from a
    ``ViewAnimator`` via ``layerAnimator(layer)``.

    .. cpp:function:: AnimationHandle animate(const LayerClip &clip, const TimingOptions &timing = {})

        Starts a keyframe animation on the layer.

    .. cpp:function:: void resizeTransition(unsigned dx, unsigned dy, unsigned dw, unsigned dh, unsigned duration, const SharedHandle<AnimationCurve> &curve)

        Animates a resize delta over the given duration (ms).

    .. cpp:function:: void shadowTransition(const LayerEffect::DropShadowParams &from, const LayerEffect::DropShadowParams &to, unsigned duration, const SharedHandle<AnimationCurve> &curve)

        Animates the layer's drop shadow from one set of parameters to
        another.

    .. cpp:function:: void transformationTransition(const LayerEffect::TransformationParams &from, const LayerEffect::TransformationParams &to, unsigned duration, const SharedHandle<AnimationCurve> &curve)

        Animates a transformation (translate/rotate/scale).

    .. cpp:function:: void transition(SharedHandle<CanvasFrame> &from, SharedHandle<CanvasFrame> &to, unsigned duration, const SharedHandle<AnimationCurve> &curve)

        Cross-fades between two canvas frames.

    .. cpp:function:: void applyShadow(const LayerEffect::DropShadowParams &params)

        Instantly sets the layer shadow (no animation).

    .. cpp:function:: void applyTransformation(const LayerEffect::TransformationParams &params)

        Instantly sets the layer transformation.

    .. cpp:function:: void pause() / void resume()

        Pauses or resumes all animations on this layer.

----

``ViewAnimator``
~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::ViewAnimator

    Controls animations at the view level (position and opacity) and
    provides access to per-layer animators.

    .. cpp:function:: AnimationHandle animate(const ViewClip &clip, const TimingOptions &timing = {})

        Animates view-level properties.

    .. cpp:function:: SharedHandle<LayerAnimator> layerAnimator(Layer &layer)

        Returns the ``LayerAnimator`` for the given layer. Creates one
        on first call.

    .. cpp:function:: void resizeTransition(unsigned dx, unsigned dy, unsigned dw, unsigned dh, unsigned duration, const SharedHandle<AnimationCurve> &curve)

        Animates the view resize as a delta over ``duration`` ms.

    .. cpp:function:: void pause() / void resume()

        Pauses or resumes all animations in this view.

**Example — fade a layer in on mount:**

.. code-block:: cpp

    void onMount() override {
        auto & v = viewAs<CanvasView>();
        auto & tree = *v.getLayerTree();
        auto & root = *tree.getRootLayer();

        view->startCompositionSession();

        auto & va = /* obtain ViewAnimator from view */;
        auto  la = va.layerAnimator(root);

        LayerClip clip;
        clip.opacity = KeyframeTrack<float>::From({
            {0.f, 0.f, AnimationCurve::EaseOut()},
            {1.f, 1.f, nullptr}
        });

        la->animate(clip, TimingOptions{.durationMs = 250});

        view->endCompositionSession();
    }

----

Fonts
-----

``FontDescriptor``
~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::FontDescriptor

    Describes the font to load.

    .. cpp:member:: OmegaCommon::String family

        Family name, e.g. ``"Arial"``, ``"SF Pro Text"``.

    .. cpp:member:: FontStyle style

        ``Regular``, ``Italic``, ``Bold``, or ``BoldAndItalic``.

    .. cpp:member:: unsigned size

        Point size.

----

``FontEngine``
~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Composition::FontEngine

    The global font creation singleton. Created automatically at startup;
    access via ``FontEngine::inst()``.

    .. cpp:function:: virtual Core::SharedPtr<Font> CreateFont(FontDescriptor &desc)

        Looks up or creates a platform font from the given descriptor.

    .. cpp:function:: virtual Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor &desc)

        Loads a font from a file path (e.g. a bundled ``.ttf``).

    .. cpp:function:: static FontEngine *inst()

        Returns the global ``FontEngine`` instance.

.. code-block:: cpp

    FontDescriptor fd{"Helvetica Neue", 16, FontDescriptor::Regular};
    auto font = FontEngine::inst()->CreateFont(fd);

----

``TextLayoutDescriptor``
~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaWTK::Composition::TextLayoutDescriptor

    Controls how text is positioned and wrapped inside a bounding rect.

    **Alignment values:**
    ``LeftUpper``, ``LeftCenter``, ``LeftLower``,
    ``MiddleUpper``, ``MiddleCenter``, ``MiddleLower``,
    ``RightUpper``, ``RightCenter``, ``RightLower``.

    **Wrapping values:**
    ``None`` (no wrap), ``WrapByWord``, ``WrapByCharacter``.

    **lineLimit:** Maximum line count (``0`` = unlimited).

----

Media
-----

All media types live in ``OmegaWTK::Media``.

``AudioVideoProcessor``
~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Media::AudioVideoProcessor

    A simple encoder and decoder for H.264 and H.265 (HEVC) audio/video.
    Used to transcode media before feeding it to a playback or capture
    session.

----

Playback
~~~~~~~~

.. cpp:class:: OmegaWTK::Media::PlaybackDispatchQueue

    An isolated thread that schedules and dispatches media playback
    events. Pass one instance to playback sessions that need
    asynchronous frame delivery.

.. cpp:class:: OmegaWTK::Media::AudioPlaybackDevice

    Represents a physical audio output device (e.g. built-in speakers, USB
    headset). Passed to ``AudioPlaybackSession`` to select the output route.

.. cpp:class:: OmegaWTK::Media::AudioPlaybackSession

    Manages playback of an audio stream from a ``MediaInputStream``.

    .. cpp:function:: void setSource(SharedHandle<MediaInputStream> source)
    .. cpp:function:: void setPlaybackDevice(SharedHandle<AudioPlaybackDevice> device)
    .. cpp:function:: void start()
    .. cpp:function:: void stop()

.. code-block:: cpp

    auto audioFile = MediaInputStream::fromFile("./track.mp3");
    auto session = AudioPlaybackSession::Create(dispatchQueue);
    session.setSource(audioFile);
    session.setPlaybackDevice(playbackDevice);
    session.start();
    // ... later ...
    session.stop();

.. cpp:class:: OmegaWTK::Media::VideoPlaybackSession

    Manages playback of a video stream. Frames are delivered to a
    ``VideoView`` through the ``FrameSink`` interface; audio is routed to an
    ``AudioPlaybackDevice``.

    .. cpp:function:: void setSource(SharedHandle<MediaInputStream> source)
    .. cpp:function:: void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> device)
    .. cpp:function:: void setVideoFrameSink(SharedHandle<VideoView> sink)
    .. cpp:function:: void start()
    .. cpp:function:: void stop()

.. code-block:: cpp

    auto videoFile = MediaInputStream::fromFile("./clip.mp4");
    auto session = VideoPlaybackSession::Create(dispatchQueue);
    session.setSource(videoFile);
    session.setAudioPlaybackDevice(audioDevice);
    session.setVideoFrameSink(videoView);
    session.start();

----

Capture
~~~~~~~

.. cpp:class:: OmegaWTK::Media::AudioCaptureDevice

    A physical audio input device (microphone, camera mic).

.. cpp:class:: OmegaWTK::Media::AudioCaptureSession

    Records or previews audio from an ``AudioCaptureDevice``.

.. cpp:class:: OmegaWTK::Media::VideoDevice

    A physical video capture device (webcam, capture card).

.. cpp:class:: OmegaWTK::Media::VideoCaptureSession

    Records or previews video from a ``VideoDevice`` with an associated
    ``AudioCaptureDevice``.

    .. cpp:function:: void setPreviewFrameSink(SharedHandle<VideoView> sink)
    .. cpp:function:: void setPreviewAudioOutput(SharedHandle<AudioPlaybackDevice> device)

.. code-block:: cpp

    auto session = videoDevice->createCaptureSession(audioCaptureDevice);
    session->setPreviewFrameSink(videoView);
    session->setPreviewAudioOutput(audioPlaybackDevice);

----

Menus
-----

.. cpp:class:: OmegaWTK::MenuItem

    A single item in a menu. Created through the free functions below.

    .. cpp:function:: void enable()
    .. cpp:function:: void disable()

.. cpp:class:: OmegaWTK::Menu

    A named collection of ``MenuItem`` instances with an optional delegate.

    .. cpp:function:: Menu(OmegaCommon::String name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate *delegate)

.. cpp:class:: OmegaWTK::MenuDelegate

    Interface for receiving item selection callbacks.

    .. cpp:function:: virtual void onSelectItem(unsigned itemIndex)

        Called when the user selects an item. ``itemIndex`` is the
        zero-based position within the enclosing ``CategoricalMenu``.

**Free functions:**

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::CategoricalMenu(const OmegaCommon::String &name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate *delegate = nullptr)

    Creates a top-level menu category (e.g. ``"File"``, ``"Edit"``).

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::SubMenu(const OmegaCommon::String &name, std::initializer_list<SharedHandle<MenuItem>> items, MenuDelegate *delegate = nullptr)

    Creates a submenu nested under a categorical menu item.

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::ButtonMenuItem(const OmegaCommon::String &name)

    Creates a clickable menu item with a label.

.. cpp:function:: SharedHandle<MenuItem> OmegaWTK::MenuItemSeperator()

    Creates a visual separator between groups of items.

**Full menu example:**

.. code-block:: cpp

    class FileDelegate : public MenuDelegate {
    public:
        void onSelectItem(unsigned idx) override {
            if (idx == 0) { /* Open */ }
            if (idx == 2) { AppInst::terminate(); }
        }
    };

    static FileDelegate fileDelegate;

    auto menu = make<Menu>("MainMenu", {
        CategoricalMenu("File", {
            ButtonMenuItem("Open"),
            MenuItemSeperator(),
            ButtonMenuItem("Quit")
        }, &fileDelegate),
        CategoricalMenu("Edit", {
            ButtonMenuItem("Cut"),
            ButtonMenuItem("Copy"),
            ButtonMenuItem("Paste")
        }),
    });

    window->setMenu(menu);

----

Dialogs
-------

File System Dialog
~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Native::NativeFSDialog

    A platform-native file open/save dialog.

    .. cpp:struct:: Descriptor

        +-----------------+-------------------------------------------+
        | ``type``        | ``Read`` (open) or ``Write`` (save).      |
        +-----------------+-------------------------------------------+
        | ``openLocation``| Initial directory to display.             |
        +-----------------+-------------------------------------------+

    .. cpp:function:: virtual OmegaCommon::Async<OmegaCommon::String> getResult()

        Returns a future that resolves to the selected file path, or an
        empty string if the user cancelled.

.. code-block:: cpp

    auto dialog = window->openFSDialog(
        {Native::NativeFSDialog::Read, "/Users/me/Documents"});
    auto path = dialog->getResult().get();   // blocks until user confirms
    if (!path.empty()) {
        openFile(path);
    }

----

Note Dialog
~~~~~~~~~~~

.. cpp:class:: OmegaWTK::Native::NativeNoteDialog

    A platform-native alert/notification dialog.

    .. cpp:struct:: Descriptor

        ``title`` and ``str`` (body text).

.. code-block:: cpp

    window->openNoteDialog({"Unsaved Changes",
                            "Do you want to save before closing?"});

----

Notifications
-------------

.. cpp:class:: OmegaWTK::NotificationCenter

    Sends system-level notifications (macOS Notification Center, Windows
    Action Center, etc.).

    .. cpp:function:: void send(NotificationDesc desc)

        Posts a notification with the given title and body text.

.. code-block:: cpp

    NotificationCenter nc;
    nc.send({"Export Complete", "Your file has been saved to ~/Desktop."});
