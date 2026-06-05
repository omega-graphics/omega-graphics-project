=======
Widgets
=======

The Widgets layer is the component library most application code
builds directly against. A ``Widget`` is the toolkit's name for "a
piece of UI" — a rectangle, a label, a button, a stack, a custom
component you author yourself. Every widget owns exactly one root
``View`` (described in the **UI** document), participates in the
layout pass, and receives the framework's lifecycle callbacks.

This document covers four things: the base ``Widget`` class and
its lifecycle, the reactive ``WidgetState`` helper, the layout
vocabulary (length units, ``LayoutStyle``, ``LayoutTransitionSpec``,
the ``LayoutManager`` strategies), and the built-in widget catalogue
(containers and primitives).

Every symbol in this document lives in the ``OmegaWTK`` namespace
unless otherwise noted.

.. contents:: On this page
   :local:
   :depth: 2

----

The Widget Lifecycle
====================

Every widget the toolkit ships overrides three lifecycle hooks:

* ``onMount`` — runs once when the widget is first attached to a
  live widget tree.
* ``resize`` — runs whenever the widget's rect changes.
* ``rebuildContent`` (a convention, not a virtual) — a helper most
  widgets call from both ``onMount`` and ``resize`` so the layout
  and style authoring lives in one place.

The framework's central paint walker handles every subsequent
redraw — ``Widget::onPaint`` is **not** in the API any more.
External code that overrides ``onPaint(PaintReason)`` will see a
compile error pointing at this migration pattern; move the
override's body into the ``rebuildContent`` shape above.

Widget
------

.. cpp:class:: OmegaWTK::Widget

    The base class for every UI component. A Widget owns exactly
    one root ``View``, manages paint scheduling, participates in
    the layout pass, and observes platform theme changes.

    **Construction.** Subclasses call one of the protected
    constructors:

    * ``Widget(Composition::Rect rect)`` — creates a new
      ``CanvasView`` sized to ``rect``.
    * ``Widget(ViewPtr view)`` — wraps an existing view (the
      shape every widget that needs a ``UIView`` root uses, to
      pass a freshly constructed ``UIView`` with the right tag).

    The ``WIDGET_CONSTRUCTOR(...)`` macro declares a public
    static ``Create`` factory for concrete subclasses.

    **Geometry.**

    .. cpp:function:: Composition::Rect & rect()

        Returns a reference to the widget's current bounding rect
        (in window coordinates).

    .. cpp:function:: void setRect(const Composition::Rect & newRect)

        Sets the widget's rect immediately, without animation.
        Triggers a paint if the size changed.

    .. cpp:function:: bool requestRect(const Composition::Rect & requested, GeometryChangeReason reason)

        Requests a geometry change from the widget's parent (or
        from the tree host if this is the root). Returns ``true``
        if the parent accepted the new rect. Prefer this over
        ``setRect`` when the widget is inside a managed layout.

    **Layout.**

    .. cpp:function:: void setLayoutStyle(const LayoutStyle & style)

        Attaches a ``LayoutStyle`` describing how the layout
        engine should size and position this widget relative to
        its siblings.

    .. cpp:function:: void requestLayout()

        Marks the widget's subtree as needing a layout pass.
        Scheduled asynchronously and coalesced with any pending
        paints.

    **Paint scheduling.**

    .. cpp:function:: void setPaintMode(PaintMode mode)

        ``PaintMode::Automatic`` (the default) — the toolkit
        repaints the widget whenever it is invalidated.
        ``PaintMode::Manual`` — the application is responsible
        for calling ``invalidate`` to schedule repaints.

    .. cpp:function:: void invalidate(PaintReason reason)

        Schedules a repaint on the next frame. Safe to call from
        any thread. Internally marks the widget's view dirty and
        forwards a ``requestFrame`` to the owning ``AppWindow``.

    .. cpp:function:: void invalidateNow(PaintReason reason)

        Forces an immediate synchronous repaint. Use sparingly;
        prefer ``invalidate`` for normal state changes.

    **Visibility.**

    .. cpp:function:: void show()

        Makes the widget visible if it was hidden.

    .. cpp:function:: void hide()

        Hides the widget without removing it from the hierarchy.

    **Observers.**

    .. cpp:function:: void addObserver(WidgetObserverPtr observer)
    .. cpp:function:: void removeObserver(WidgetObserverPtr observer)

    **Subclass overrides.** Override what your widget actually
    needs; the defaults are empty.

    .. cpp:function:: virtual void onMount()

        Called once when the widget is first attached to a live
        widget tree and its GPU resources are ready. Perform
        one-time initialisation here — font loading, child-view
        creation, the initial layout / style authoring.

    .. cpp:function:: virtual void resize(Composition::Rect & newRect)

        Called when the widget's rect changes. The convention is
        to call the same ``rebuildContent`` helper that
        ``onMount`` calls so the layout authoring lives in one
        place. Modify ``newRect`` to clamp or veto the resize.

    .. cpp:function:: virtual MeasureResult measureSelf(const LayoutContext & ctx)

        Override to report the widget's intrinsic size in dp
        units. The layout engine calls this during the measure
        pass.

    .. cpp:function:: virtual void onLayoutResolved(const Composition::Rect & finalRectPx)

        Called after the layout engine has committed a final
        pixel rect for the widget. Use this to update sub-views
        that depend on the resolved size.

    .. cpp:function:: virtual void onThemeSet(Native::ThemeDesc & desc)

        Called when the platform theme changes (light / dark
        mode, accent color shift, etc.). The default is a no-op.

**Implementing a custom widget — the canonical pattern.**

.. code-block:: cpp

    class MyCard : public OmegaWTK::Widget {
        OmegaWTK::UIViewPtr uiView_;

        void ensureUIView(const OmegaWTK::Composition::Rect & bounds){
            OmegaWTK::Composition::Rect local{{0.f, 0.f}, bounds.w, bounds.h};
            if (uiView_ == nullptr) {
                uiView_ = makeSubView<OmegaWTK::UIView>(local, "my_card_view");
            } else {
                uiView_->resize(local);
            }
        }

        void rebuildContent(){
            auto bounds = rect();
            ensureUIView(bounds);

            OmegaWTK::UIViewLayout layout;
            layout.shape("bg", OmegaWTK::Shape::RoundedRect(
                OmegaWTK::Composition::RoundedRect{
                    {{0.f, 0.f}, bounds.w, bounds.h}, 8.f, 8.f}));
            uiView_->setLayout(layout);
            // The cascade authored on the owning AppWindow paints
            // the rest. No setStyle() needed here.
        }

    protected:
        void onMount() override {
            rebuildContent();
        }

        void resize(OmegaWTK::Composition::Rect & newRect) override {
            (void)newRect;
            rebuildContent();
        }

    public:
        explicit MyCard(OmegaWTK::Composition::Rect rect)
            : OmegaWTK::Widget(rect) {}
    };

WidgetObserver
--------------

.. cpp:class:: OmegaWTK::WidgetObserver

    Receives lifecycle and geometry notifications for a single
    Widget. Subclass and override the methods you care about; all
    have empty default implementations.

    .. cpp:function:: virtual void onWidgetAttach(WidgetPtr parent)

        Called when the observed widget is added to a tree.

    .. cpp:function:: virtual void onWidgetDetach(WidgetPtr parent)

        Called when the widget is removed.

    .. cpp:function:: virtual void onWidgetChangeSize(Composition::Rect oldRect, Composition::Rect & newRect)

        Called when the widget's rect changes. ``newRect`` can be
        modified to veto or clamp the resize.

    .. cpp:function:: virtual void onWidgetDidHide()
    .. cpp:function:: virtual void onWidgetDidShow()

WidgetState and WidgetStateObserver
-----------------------------------

.. cpp:class:: template<class Ty> OmegaWTK::WidgetState

    A typed reactive state container. Every registered observer is
    notified synchronously whenever ``setValue`` is called. Used
    to plumb shared model state into a widget without coupling
    the widget directly to the model owner.

    .. cpp:function:: static SharedHandle<WidgetState<Ty>> Create(Core::Optional<Ty> initialValue = {})

        Creates a new state container with an optional initial
        value.

    .. cpp:function:: void setValue(Ty newVal)

        Updates the value and notifies all observers.

    .. cpp:function:: void addObserver(WidgetStateObserver<WidgetState<Ty>> * observer)

        Registers an observer pointer (non-owning).

    .. cpp:function:: void removeObserver(WidgetStateObserver<WidgetState<Ty>> * observer)

**Example — driving widget visibility from a bool state.**

.. code-block:: cpp

    auto enabledState = OmegaWTK::WidgetState<bool>::Create(true);

    class MyButtonObserver
            : public OmegaWTK::WidgetStateObserver<OmegaWTK::WidgetState<bool>> {
        MyButton * button_;
    public:
        explicit MyButtonObserver(MyButton * b) : button_(b) {}
        void stateHasChanged(bool & newVal) override {
            newVal ? button_->show() : button_->hide();
        }
    };

    MyButtonObserver obs(myButton.get());
    enabledState->addObserver(&obs);
    enabledState->setValue(false);   // button hides immediately

----

Layout
======

OmegaWTK's layout vocabulary borrows the parts of CSS that map
cleanly to a native UI toolkit: a typed length unit, four-edge
insets, a per-widget ``LayoutStyle``, and a small library of
container layout managers (Stack, Fill, Flex, Container, Absolute).

Length Units
------------

.. cpp:struct:: OmegaWTK::LayoutLength

    A CSS-inspired length value with an associated unit.

    +--------------------------------+--------------------------------------------------+
    | Factory                        | Meaning                                          |
    +================================+==================================================+
    | ``LayoutLength::Auto()``       | Size determined by the layout engine.            |
    +--------------------------------+--------------------------------------------------+
    | ``LayoutLength::Px(v)``        | Absolute pixels.                                 |
    +--------------------------------+--------------------------------------------------+
    | ``LayoutLength::Dp(v)``        | Device-independent points (scaled by DPI).       |
    +--------------------------------+--------------------------------------------------+
    | ``LayoutLength::Percent(v)``   | Fraction of the available parent dimension.      |
    +--------------------------------+--------------------------------------------------+
    | ``LayoutLength::Fr(v)``        | Fractional unit for grid/flex distributions.     |
    +--------------------------------+--------------------------------------------------+
    | ``LayoutLength::Intrinsic()``  | Widget's own measured size.                      |
    +--------------------------------+--------------------------------------------------+

.. cpp:struct:: OmegaWTK::LayoutEdges

    Inset or outset amounts for all four sides of a box.

    .. cpp:function:: static LayoutEdges Zero()
    .. cpp:function:: static LayoutEdges All(LayoutLength value)
    .. cpp:function:: static LayoutEdges Symmetric(LayoutLength horizontal, LayoutLength vertical)

.. cpp:struct:: OmegaWTK::LayoutClamp

    Minimum and maximum constraints applied after the normal
    layout pass.

LayoutStyle
-----------

.. cpp:struct:: OmegaWTK::LayoutStyle

    Describes how the layout engine sizes and positions a single
    widget node. Attach to a widget via
    ``Widget::setLayoutStyle()``.

    **Size and position.**

    +-----------------------------+-----------------------------------------------+
    | Field                       | Default                                       |
    +=============================+===============================================+
    | ``width``, ``height``       | ``Auto``                                      |
    +-----------------------------+-----------------------------------------------+
    | ``clamp``                   | unconstrained                                 |
    +-----------------------------+-----------------------------------------------+
    | ``margin``                  | zero                                          |
    +-----------------------------+-----------------------------------------------+
    | ``padding``                 | zero                                          |
    +-----------------------------+-----------------------------------------------+
    | ``position``                | ``Flow`` (participates in normal flow)        |
    +-----------------------------+-----------------------------------------------+
    | ``insetLeft/Top/...``       | ``Auto`` (used with ``Absolute`` position)    |
    +-----------------------------+-----------------------------------------------+

    **Display mode.** The ``display`` field selects the
    container algorithm for child widgets:

    * ``Stack`` — children stack along the container's axis
      (default).
    * ``Flex`` — flexbox-style row or column.
    * ``Grid`` — grid-track layout.
    * ``Overlay`` — children share the same rect (z-stacked).
    * ``Custom`` — a custom ``LayoutBehavior`` plugin.

    **Flex fields.** ``flexDirection``, ``flexWrap``,
    ``justifyContent``, ``alignItems``, ``flexGrow``,
    ``flexShrink``, ``gap``. Used when ``display == Flex``.

.. code-block:: cpp

    using namespace OmegaWTK;

    LayoutStyle s;
    s.display        = LayoutDisplay::Flex;
    s.flexDirection  = FlexDirection::Row;
    s.justifyContent = LayoutAlign::Center;
    s.alignItems     = LayoutAlign::Center;
    s.padding        = LayoutEdges::All(LayoutLength::Dp(12.f));
    myWidget->setLayoutStyle(s);

LayoutTransitionSpec
--------------------

.. cpp:struct:: OmegaWTK::LayoutTransitionSpec

    Describes an animated transition for layout property changes.
    Set this on a ``LayoutStyle`` to make subsequent layout
    commits animate from the previous rect to the new one.

    .. cpp:member:: bool enabled

        Must be ``true`` for the transition to fire.

    .. cpp:member:: float durationSec

        Duration in seconds.

    .. cpp:member:: SharedHandle<Composition::AnimationCurve> curve

        The easing curve. ``AnimationCurve::EaseInOut()`` is the
        standard pick.

    .. cpp:member:: OmegaCommon::Vector<LayoutTransitionProperty> properties

        Properties to animate. Values: ``X``, ``Y``, ``Width``,
        ``Height``, ``Opacity``, ``CornerRadius``, ``Shadow``,
        ``Blur``.

LayoutManager
-------------

.. cpp:class:: OmegaWTK::LayoutManager

    The base interface every container's layout algorithm
    implements. A widget's ``layoutManager()`` runs during the
    Layout pass with a guaranteed local-origin rect — the
    FrameBuilder's walker translates the parent-space rect to a
    local-origin one before dispatching, so manager
    implementations always see ``pos == (0, 0)``.

    .. cpp:function:: virtual LayoutSize measure(View & node, const Composition::Rect & nodeLocalRect)
    .. cpp:function:: virtual void arrange(View & node, const Composition::Rect & nodeLocalRect)

The toolkit ships several built-in managers:

.. cpp:class:: OmegaWTK::AbsoluteLayout

    Children keep whatever rect their widget owner authored.
    Clamping is optional via ``ChildResizeSpec``.

.. cpp:class:: OmegaWTK::FillLayout

    Every child is stretched to the parent's content rect. Useful
    for single-child containers that need to absorb their parent.

.. cpp:class:: OmegaWTK::StackLayout

    Sequential placement along ``LayoutAxis::Horizontal`` or
    ``LayoutAxis::Vertical``, with a configurable inter-item gap.

.. cpp:class:: OmegaWTK::FlexLayout

    The CSS-flex algorithm. ``StackWidget`` and its
    ``HStack`` / ``VStack`` subclasses use this manager.

.. cpp:class:: OmegaWTK::ContainerLayout

    The clamping container algorithm. ``Container`` uses it to
    enforce min-size guarantees and to apply
    ``ContainerClampPolicy``.

----

Containers
==========

A container is a Widget whose role is to host other widgets and
arrange them inside its own rect. The toolkit ships ``Container``
(a clamping host) and ``StackWidget`` (a flexbox-style host),
plus the convenience subclasses ``HStack`` and ``VStack``.

Container
---------

.. cpp:class:: OmegaWTK::Container : public Widget

    A widget that hosts a collection of child widgets. Subclass
    ``Container`` to build custom layout hosts, or use the
    provided ``HStack`` / ``VStack`` for the common cases.

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr & child)

        Adds a child. The container's layout algorithm decides
        the child's final position and size.

    .. cpp:function:: bool removeChild(const WidgetPtr & child)

        Removes a child. Returns ``false`` if not found.

    .. cpp:function:: Widget * childAt(std::size_t idx) const

        Returns the child at position ``idx``, or ``nullptr`` if
        out of range.

    .. cpp:function:: std::size_t childCount() const

        Returns the number of direct children.

    .. cpp:function:: void setClampPolicy(const ContainerClampPolicy & policy)

        Configures how the container constrains child positions
        and sizes during layout.

    .. cpp:function:: void relayout()

        Forces an immediate synchronous layout pass for all
        children.

StackWidget, HStack, and VStack
-------------------------------

.. cpp:class:: OmegaWTK::StackWidget : public Container

    Lays children out sequentially along a single axis. Controls
    spacing, padding, main-axis alignment, and cross-axis
    alignment through ``StackOptions``. Each child may carry an
    optional ``StackSlot`` that overrides flex grow / shrink and
    margin per child.

    .. cpp:function:: StackWidget(StackAxis axis, Composition::Rect rect, const StackOptions & options = {})

        ``axis`` is ``StackAxis::Horizontal`` or
        ``StackAxis::Vertical``.

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr & child)

        Adds a child with default slot settings.

    .. cpp:function:: WidgetPtr addChild(const WidgetPtr & child, const StackSlot & slot)

        Adds a child with explicit flex and margin settings.

    .. cpp:function:: bool setSlot(const WidgetPtr & child, const StackSlot & slot)

        Updates the slot for an already-added child. Triggers
        relayout.

    .. cpp:function:: void relayout()

        Forces a synchronous layout pass.

.. cpp:struct:: OmegaWTK::StackOptions

    +-----------------------+--------------------------------------+------------+
    | Field                 | Meaning                              | Default    |
    +=======================+======================================+============+
    | ``spacing``           | Gap between children (dp).           | ``0``      |
    +-----------------------+--------------------------------------+------------+
    | ``padding``           | Insets on all four sides.            | zero       |
    +-----------------------+--------------------------------------+------------+
    | ``mainAlign``         | Distribution along the stack axis.   | ``Start``  |
    |                       | ``Start``, ``Center``, ``End``,      |            |
    |                       | ``SpaceBetween``, ``SpaceAround``,   |            |
    |                       | ``SpaceEvenly``.                     |            |
    +-----------------------+--------------------------------------+------------+
    | ``crossAlign``        | Alignment on the perpendicular axis. | ``Start``  |
    |                       | ``Start``, ``Center``, ``End``,      |            |
    |                       | ``Stretch``.                         |            |
    +-----------------------+--------------------------------------+------------+
    | ``clipOverflow``      | Clip children that exceed bounds.    | ``false``  |
    +-----------------------+--------------------------------------+------------+

.. cpp:struct:: OmegaWTK::StackSlot

    Per-child flex parameters. All fields are optional.

    +-----------------+------------------------------------------------------+
    | Field           | Meaning                                              |
    +=================+======================================================+
    | ``flexGrow``    | How much free space this child absorbs (default 0).  |
    +-----------------+------------------------------------------------------+
    | ``flexShrink``  | How much this child shrinks when space is tight.     |
    +-----------------+------------------------------------------------------+
    | ``basis``       | Override preferred main-axis size (dp).              |
    +-----------------+------------------------------------------------------+
    | ``minMain``     | Minimum size along the stack axis (dp).              |
    +-----------------+------------------------------------------------------+
    | ``maxMain``     | Maximum size along the stack axis (dp).              |
    +-----------------+------------------------------------------------------+
    | ``margin``      | Per-child insets (overrides container padding).      |
    +-----------------+------------------------------------------------------+
    | ``alignSelf``   | Override cross-axis alignment for this child only.   |
    +-----------------+------------------------------------------------------+

.. cpp:class:: OmegaWTK::HStack : public StackWidget

    Convenience subclass: ``StackAxis::Horizontal`` stack.

.. cpp:class:: OmegaWTK::VStack : public StackWidget

    Convenience subclass: ``StackAxis::Vertical`` stack.

**Example — a toolbar with a stretchy search field in the middle.**

.. code-block:: cpp

    using namespace OmegaWTK;

    auto toolbar = make<HStack>(
        Composition::Rect{{0, 0}, 600.f, 48.f},
        StackOptions{
            .spacing    = 8.f,
            .padding    = {8.f, 4.f, 8.f, 4.f},
            .crossAlign = StackCrossAlign::Center
        });

    toolbar->addChild(iconButton,  StackSlot{.flexGrow = 0.f});
    toolbar->addChild(searchField, StackSlot{.flexGrow = 1.f}); // absorbs the gap
    toolbar->addChild(menuButton,  StackSlot{.flexGrow = 0.f});

----

Primitive Widgets
=================

All primitives live in ``omegaWTK/Widgets/Primatives.h``. Each
constructor takes a ``Composition::Rect`` and a props struct;
``setProps()`` updates the appearance at runtime and the toolkit
repaints automatically.

Rectangle
---------

.. cpp:class:: OmegaWTK::Rectangle : public Widget

    Draws a filled and / or stroked axis-aligned rectangle.

.. cpp:struct:: OmegaWTK::RectangleProps

    .. cpp:member:: SharedHandle<Composition::Brush> fill

        Fill brush. ``nullptr`` = no fill.

    .. cpp:member:: SharedHandle<Composition::Brush> stroke

        Stroke brush. ``nullptr`` = no stroke.

    .. cpp:member:: float strokeWidth

        Stroke width in dp.

.. code-block:: cpp

    auto rect = make<OmegaWTK::Rectangle>(
        OmegaWTK::Composition::Rect{{0, 0}, 100.f, 60.f},
        OmegaWTK::RectangleProps{
            .fill        = OmegaWTK::Composition::ColorBrush(
                              OmegaWTK::Composition::Color::create8Bit(
                                  OmegaWTK::Composition::Color::Blue8)),
            .stroke      = OmegaWTK::Composition::ColorBrush(
                              OmegaWTK::Composition::Color::create8Bit(
                                  OmegaWTK::Composition::Color::White8)),
            .strokeWidth = 2.f
        });

RoundedRectangle
----------------

.. cpp:class:: OmegaWTK::RoundedRectangle : public Widget

    Draws a rectangle with independently controllable per-corner
    radii.

.. cpp:struct:: OmegaWTK::RoundedRectangleProps

    Extends ``RectangleProps`` with ``topLeft``, ``topRight``,
    ``bottomLeft``, ``bottomRight`` corner radii (dp).

.. code-block:: cpp

    auto card = make<OmegaWTK::RoundedRectangle>(
        OmegaWTK::Composition::Rect{{0, 0}, 200.f, 120.f},
        OmegaWTK::RoundedRectangleProps{
            .fill        = OmegaWTK::Composition::ColorBrush(
                              OmegaWTK::Composition::Color::create8Bit(0x2A2A2A, 0xFF)),
            .topLeft     = 12.f, .topRight    = 12.f,
            .bottomLeft  = 12.f, .bottomRight = 12.f
        });

Ellipse
-------

.. cpp:class:: OmegaWTK::Ellipse : public Widget

    Draws a filled and / or stroked ellipse inscribed in the
    widget's rect.

.. cpp:struct:: OmegaWTK::EllipseProps

    ``fill``, ``stroke``, ``strokeWidth`` — same semantics as
    ``RectangleProps``.

Path
----

.. cpp:class:: OmegaWTK::Path : public Widget

    Draws an arbitrary 2D vector path.

.. cpp:struct:: OmegaWTK::PathProps

    .. cpp:member:: OmegaGTE::GVectorPath2D path

        The path geometry.

    .. cpp:member:: SharedHandle<Composition::Brush> fill

        Fill brush (used when ``closePath == true``).

    .. cpp:member:: SharedHandle<Composition::Brush> stroke

        Stroke brush.

    .. cpp:member:: unsigned strokeWidth

        Stroke width in pixels.

    .. cpp:member:: bool closePath

        If ``true``, a line is drawn from the last point back to
        the first.

Separator
---------

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

Label
-----

.. cpp:class:: OmegaWTK::Label : public Widget

    Renders a static or dynamic Unicode text string.

.. cpp:struct:: OmegaWTK::LabelProps

    .. cpp:member:: OmegaCommon::UString text

        The text content (UTF-32 string — use the ``U""``
        literal prefix).

    .. cpp:member:: SharedHandle<Composition::Font> font

        Font to render with. Obtain from
        ``FontEngine::inst()->CreateFont()``.

    .. cpp:member:: Composition::Color textColor

        Text color (default opaque black).

    .. cpp:member:: TextLayoutDescriptor::Alignment alignment

        Horizontal and vertical anchor. See the
        ``TextLayoutDescriptor`` section of the **Composition**
        document for the full list.

    .. cpp:member:: TextLayoutDescriptor::Wrapping wrapping

        ``None``, ``WrapByWord``, or ``WrapByCharacter``.

    .. cpp:member:: unsigned lineLimit

        Maximum line count (``0`` = unlimited).

    .. cpp:function:: void setText(const OmegaCommon::UString & text)

        Updates the displayed text and schedules a repaint.

.. code-block:: cpp

    using namespace OmegaWTK;
    using namespace OmegaWTK::Composition;

    FontDescriptor fd{"SF Pro", 16, FontDescriptor::Regular};
    auto font = FontEngine::inst()->CreateFont(fd);

    LabelProps props;
    props.text      = U"Hello, OmegaWTK";
    props.font      = font;
    props.textColor = Color::create8Bit(Color::White8);
    props.alignment = TextLayoutDescriptor::MiddleCenter;
    props.wrapping  = TextLayoutDescriptor::WrapByWord;

    auto label = make<Label>(Composition::Rect{{0, 0}, 300.f, 40.f}, props);

Icon
----

.. cpp:class:: OmegaWTK::Icon : public Widget

    Renders a named icon token as a glyph.

.. cpp:struct:: OmegaWTK::IconProps

    .. cpp:member:: OmegaCommon::String token

        The icon token string (font-icon glyph name).

    .. cpp:member:: float size

        Glyph render size in dp (default ``16``).

    .. cpp:member:: Composition::Color tintColor

        Glyph color.

Image
-----

.. cpp:class:: OmegaWTK::Image : public Widget

    Displays a ``BitmapImage`` with a configurable fit mode.

.. cpp:struct:: OmegaWTK::ImageProps

    .. cpp:member:: SharedHandle<Media::BitmapImage> source

        The bitmap to display.

    .. cpp:member:: ImageFitMode fitMode

        ``Contain``, ``Cover``, ``Fill``, ``Center``, or ``Crop``.
