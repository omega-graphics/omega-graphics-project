===========
Composition
===========

The Composition layer is the drawing foundation of OmegaWTK. It owns
the colors, brushes, shape primitives, fonts, layer effects, and the
animation timing types that the rest of the toolkit composes on top
of. Every drawable thing a window paints — a button background, a
label glyph, a drop shadow, a video frame — passes through types in
this layer at some point.

Most application code does not call into Composition directly. The
Widgets layer and the Style cascade described in the **UI** document
sit on top, and they are the recommended entry points for almost
every UI task. You will reach into Composition when you need to
build a brush by hand, sample a font, describe a drop shadow, or
configure animation timing — the building blocks that the higher
layers consume.

Every symbol in this document lives in the ``OmegaWTK::Composition``
namespace unless otherwise noted. The corresponding headers all sit
under ``omegaWTK/Composition/``.

.. contents:: On this page
   :local:
   :depth: 2

----

Geometry and Color
==================

Color
-----

.. cpp:struct:: OmegaWTK::Composition::Color

    A four-channel RGBA color stored as floats. ``Color`` is the
    universal "what color" value in the rest of the toolkit — every
    brush, every text color, every shadow tint goes through it.
    Construct with one of the static factories rather than poking
    the fields directly; the factories accept the precision the
    incoming value is naturally expressed in.

    .. cpp:function:: static Color create8Bit(uint8_t r, uint8_t g, uint8_t b, uint8_t a)

        Builds a color from per-channel 8-bit values. The most
        common factory for hand-authored colors.

    .. cpp:function:: static Color create8Bit(uint32_t rgb, uint8_t alpha = 0xFF)

        Builds a color from a packed 24-bit RGB hex value plus a
        separate alpha. Convenient when you want to paste a hex
        triple from a design tool.

    .. cpp:function:: static Color create16Bit(uint16_t r, uint16_t g, uint16_t b, uint16_t a)

        16-bit per-channel factory for HDR inputs.

    .. cpp:function:: static Color create32Bit(uint32_t r, uint32_t g, uint32_t b, uint32_t a)

        32-bit per-channel factory for wide-gamut sources.

    **Named 8-bit constants.** ``Color::Black8``, ``White8``,
    ``Red8``, ``Green8``, ``Blue8``, ``Yellow8``, ``Orange8``,
    ``Purple8``. Each is a 24-bit hex triple suitable for the
    second ``create8Bit`` overload.

    **Special value.** ``Color::Transparent`` is the alpha-zero
    color you reach for when you want to clear something without
    actually painting on top of it.

.. code-block:: cpp

    using namespace OmegaWTK::Composition;

    auto red   = Color::create8Bit(Color::Red8);
    auto white = Color::create8Bit(0xFF, 0xFF, 0xFF, 0xFF);
    auto navy  = Color::create8Bit(0x002244, 0xFF);

Gradient
--------

.. cpp:struct:: OmegaWTK::Composition::Gradient

    Describes a linear or radial color ramp made of one or more
    ``GradientStop`` entries. A gradient is the input to
    ``GradientBrush``; you do not paint with a ``Gradient`` directly.

    .. cpp:function:: static Gradient Linear(std::initializer_list<GradientStop> stops, float angle)

        A linear gradient at the given angle in degrees. ``0``
        is left-to-right; ``90`` is top-to-bottom.

    .. cpp:function:: static Gradient Radial(std::initializer_list<GradientStop> stops, float radius)

        A radial gradient with the given outer radius (the same
        unit space as the geometry you draw it onto).

    .. cpp:function:: static GradientStop Stop(float pos, Color color)

        A single stop at normalised position ``pos`` (``0`` is the
        start of the ramp, ``1`` is the end).

.. code-block:: cpp

    auto grad = Gradient::Linear({
        Gradient::Stop(0.f, Color::create8Bit(Color::Blue8)),
        Gradient::Stop(1.f, Color::create8Bit(Color::Purple8))
    }, 90.f);

Brush
-----

.. cpp:struct:: OmegaWTK::Composition::Brush

    A fill descriptor that every shape-drawing call accepts. You do
    not construct a ``Brush`` directly — use the factories below.
    Brushes are reference-counted and freely shareable; a single
    brush can fill any number of shapes.

.. cpp:function:: SharedHandle<Brush> OmegaWTK::Composition::ColorBrush(const Color & color)

    Returns a solid-color brush. This is the brush you reach for
    when you have a single ``Color`` and want to paint with it.

.. cpp:function:: SharedHandle<Brush> OmegaWTK::Composition::GradientBrush(const Gradient & gradient)

    Returns a brush that paints with the given gradient ramp.

.. code-block:: cpp

    auto blue   = ColorBrush(Color::create8Bit(Color::Blue8));
    auto fade   = GradientBrush(Gradient::Linear({
        Gradient::Stop(0.f, Color::create8Bit(Color::Black8)),
        Gradient::Stop(1.f, Color::create8Bit(Color::White8))
    }, 90.f));

Shapes
------

The toolkit's shape types live in the ``OmegaWTK::Composition``
namespace. They are pure value types — small structs that pack a
position, a size, and any shape-specific parameters.

.. cpp:struct:: OmegaWTK::Composition::Point2D

    A 2D position: ``{float x, float y}``.

.. cpp:struct:: OmegaWTK::Composition::Rect

    An axis-aligned rectangle: ``{Point2D pos, float w, float h}``.
    ``pos`` is the top-left corner.

.. cpp:struct:: OmegaWTK::Composition::RoundedRect

    A rectangle with corner radii. The struct extends ``Rect`` with
    a uniform corner-radius pair (``rad_x``, ``rad_y``). For
    per-corner radii build a ``Rectangle`` widget with the
    ``RoundedRectangleProps`` shape instead — that authoring
    surface accepts four independent corner radii.

.. cpp:struct:: OmegaWTK::Composition::Ellipse

    An ellipse defined by a centre point and two radii.

.. code-block:: cpp

    Composition::Rect r{{10.f, 20.f}, 300.f, 200.f};
    Composition::RoundedRect rr{{{0,0}, 200, 60}, 8.f, 8.f};

----

Layers, Canvases, and Effects
=============================

The Layer / Canvas / LayerTree surface is the low-level drawing
substrate the toolkit composites windows out of. After Tier 4 of
the render redesign almost no application code reaches in here —
the **UIView** path (described in the **UI** document) is the
recommended way to author content, and the framework wires the
layers behind the scenes. The types below remain documented because
custom integration code (e.g. drawing a GTE texture into a layer)
occasionally needs them.

Layer
-----

.. cpp:class:: OmegaWTK::Composition::Layer

    A rectangular GPU surface within a ``LayerTree``. At most one
    ``Canvas`` may be bound to a layer at a time.

    .. cpp:function:: Composition::Rect & getLayerRect()

        Returns the layer's current bounds.

    .. cpp:function:: void resize(Composition::Rect & newRect)

        Resizes the layer and notifies the owning ``LayerTree``.

    .. cpp:function:: void setEnabled(bool state)

        Shows (``true``) or hides (``false``) the layer.

    .. cpp:function:: bool hasCanvas() const

        Returns ``true`` if a canvas is currently bound.

LayerTree
---------

.. cpp:class:: OmegaWTK::Composition::LayerTree

    Owns the root layer and any sublayers for a single view.
    Observers (``LayerTreeObserver``) receive structural-change
    notifications. The window-level tree is owned by ``AppWindow``;
    per-view trees are owned by ``UIView`` for compatibility but
    most paint flows through the window-level tree directly.

Canvas
------

.. cpp:class:: OmegaWTK::Composition::Canvas

    A 2D drawing surface that records draw commands into a
    ``CanvasFrame``. Each canvas is bound to exactly one ``Layer``;
    drawing happens between explicit lifecycle calls on the View
    that owns the layer. For modern code that uses the central
    walker, the ``Canvas`` surface is mostly an implementation
    detail of UIView and the FrameBuilder — the widget primitives
    described in the **Widgets** document author element lists and
    a ``StyleSheet`` instead.

    The full ``Canvas`` reference (every ``drawRect`` /
    ``drawText`` / ``drawImage`` / ``applyEffect`` variant) is in
    the ``omegaWTK/Composition/Compositor.h`` header.

LayerEffect
-----------

.. cpp:struct:: OmegaWTK::Composition::LayerEffect

    A compositor-side effect applied to a layer after its canvas
    frame is composited. Two variants ship today:

    **DropShadow** — ``LayerEffect::DropShadowParams``

    +-----------------+-------------------------------------------+
    | ``x_offset``    | Horizontal shadow offset (dp).            |
    +-----------------+-------------------------------------------+
    | ``y_offset``    | Vertical shadow offset (dp).              |
    +-----------------+-------------------------------------------+
    | ``radius``      | Shadow spread radius (dp).                |
    +-----------------+-------------------------------------------+
    | ``blurAmount``  | Gaussian blur applied to the shadow.      |
    +-----------------+-------------------------------------------+
    | ``opacity``     | Shadow opacity in ``[0, 1]``.             |
    +-----------------+-------------------------------------------+
    | ``color``       | Shadow ``Color``.                         |
    +-----------------+-------------------------------------------+

    **Transformation** — ``LayerEffect::TransformationParams``

    Holds three sub-structs: ``translate {x, y, z}``,
    ``rotate {pitch, yaw, roll}``, and ``scale {x, y, z}``. The
    compositor applies these as a 4×4 transform to the layer.

CanvasEffect
------------

.. cpp:struct:: OmegaWTK::Composition::CanvasEffect

    A canvas-side filter that runs inside the draw pass rather
    than on the composited layer. ``GaussianBlurParams`` blurs the
    entire frame; ``DirectionalBlurParams`` blurs along a
    configurable angle.

----

Animation
=========

The animation primitives below — curves, timing options, keyframe
tracks, and handles — are the value types you compose into a
``StyleSheet`` rule or hand to a custom integration. The actual
runtime that advances animations per frame is the per-window
``AnimationScheduler`` (an internal symbol of the toolkit);
application code does not invoke the scheduler directly.

The way you start an animation is by *declaring* it on a
``StyleSheet`` rule. The Style cascade pass — described in the
**UI** document under "The Style Cascade" — sees the declaration
fire and threads the work through the scheduler on your behalf.
Two declaration forms are supported:

* A ``transition`` on a rule — when a resolved cell changes
  between frames, the scheduler tweens from the old value to the
  new one over the timing you specified.
* An ``animation`` binding that references a named
  ``KeyframeAnimation`` declaration on the sheet — the scheduler
  starts a keyframe animation when the rule begins matching, and
  cancels it when the rule stops matching.

The legacy ``LayerAnimator`` / ``ViewAnimator`` / ``LayerClip`` /
``ViewClip`` classes that earlier versions of this document
referred to no longer exist; they were retired during Phase 4.8
of the UIView render redesign and the cascade-driven path
replaces them in full.

AnimationCurve
--------------

.. cpp:struct:: OmegaWTK::Composition::AnimationCurve

    A 1D easing function mapping normalised time ``[0, 1]`` to a
    normalised interpolant ``[0, 1]``. Pass an ``AnimationCurve``
    handle to a transition spec or to a keyframe value to choose
    how the interpolation eases through the segment.

    .. cpp:function:: static SharedHandle<AnimationCurve> Linear()

        Constant-rate interpolation.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseIn()

        Starts slow, ends at full speed.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseOut()

        Starts at full speed, ends slow.

    .. cpp:function:: static SharedHandle<AnimationCurve> EaseInOut()

        Starts and ends slow, peaks in the middle.

    .. cpp:function:: static SharedHandle<AnimationCurve> Quadratic(OmegaGTE::GPoint2D a)

        Quadratic Bézier with one control point.

    .. cpp:function:: static SharedHandle<AnimationCurve> CubicBezier(OmegaGTE::GPoint2D a, OmegaGTE::GPoint2D b, float start_h = 0.f, float end_h = 1.f)

        Cubic Bézier with two control points and optional
        horizontal start / end tangents.

    .. cpp:function:: float sample(float t) const

        Evaluates the curve at normalised time ``t``. The runtime
        calls this every tick.

TimingOptions
-------------

.. cpp:struct:: OmegaWTK::Composition::TimingOptions

    Controls how an animation plays. ``TimingOptions`` is a plain
    value type — fill in the fields you care about, leave the rest
    at their defaults.

    +-----------------------+-----------------------------------------------+-------------+
    | Field                 | Meaning                                       | Default     |
    +=======================+===============================================+=============+
    | ``durationMs``        | Total animation duration in milliseconds.     | ``300``     |
    +-----------------------+-----------------------------------------------+-------------+
    | ``delayMs``           | Delay before the animation starts.            | ``0``       |
    +-----------------------+-----------------------------------------------+-------------+
    | ``playbackRate``      | Speed multiplier (``2.0`` = double speed).    | ``1.0``     |
    +-----------------------+-----------------------------------------------+-------------+
    | ``iterations``        | How many times to repeat (``INFINITY`` loops).| ``1.0``     |
    +-----------------------+-----------------------------------------------+-------------+
    | ``fillMode``          | ``None``, ``Forwards``, ``Backwards``,        | ``Forwards``|
    |                       | ``Both``.                                     |             |
    +-----------------------+-----------------------------------------------+-------------+
    | ``direction``         | ``Normal``, ``Reverse``, ``Alternate``,       | ``Normal``  |
    |                       | ``AlternateReverse``.                         |             |
    +-----------------------+-----------------------------------------------+-------------+
    | ``frameRateHint``     | Target frame rate hint for the compositor.    | ``60``      |
    +-----------------------+-----------------------------------------------+-------------+
    | ``clockMode``         | ``WallClock``, ``PresentedClock``, ``Hybrid``.| ``Hybrid``  |
    +-----------------------+-----------------------------------------------+-------------+

AnimationHandle
---------------

.. cpp:class:: OmegaWTK::Composition::AnimationHandle

    A reference-counted handle returned every time the runtime
    starts an animation on your behalf. Custom integrations that
    drive the runtime hold these handles to inspect state and
    control playback; cascade-driven animations track the handles
    internally and cancel them when the rule stops matching.

    .. cpp:function:: AnimationState state() const

        Current state: ``Pending``, ``Running``, ``Paused``,
        ``Completed``, ``Cancelled``, ``Failed``.

    .. cpp:function:: float progress() const

        Normalised animation progress in ``[0, 1]``.

    .. cpp:function:: void pause()

        Pauses the animation. The next ``resume`` continues from
        the paused progress.

    .. cpp:function:: void resume()

        Resumes a paused animation.

    .. cpp:function:: void cancel()

        Cancels the animation. The cell stops updating and the
        handle becomes invalid.

    .. cpp:function:: void seek(float normalized)

        Jumps to a normalised time position.

    .. cpp:function:: void setPlaybackRate(float rate)

        Changes the playback speed without restarting.

    .. cpp:function:: bool valid() const

        Returns ``false`` if the handle was default-constructed or
        the animation was cancelled.

KeyframeTrack and KeyframeValue
-------------------------------

.. cpp:class:: template<typename T> OmegaWTK::Composition::KeyframeTrack

    A sorted list of typed keyframes with per-segment easing.
    ``T`` must be one of the supported interpolation types:
    ``float``, ``Composition::Color``, ``Composition::Point2D``,
    ``Composition::Rect``, ``LayerEffect::TransformationParams``,
    ``LayerEffect::DropShadowParams``, ``std::uint32_t``.

    .. cpp:function:: static KeyframeTrack<T> From(const OmegaCommon::Vector<KeyframeValue<T>> & source)

        Builds a track from a list of keyframe values. The
        constructor sorts entries by ``offset`` and clamps each
        offset to ``[0, 1]``.

    .. cpp:function:: T sample(float t) const

        Returns the interpolated value at normalised time ``t``.
        The runtime calls this every tick.

.. cpp:struct:: template<typename T> OmegaWTK::Composition::KeyframeValue

    A single keyframe: ``offset`` (the normalised time at which
    this value holds), ``value`` (the value of type ``T``), and an
    optional ``easingToNext`` curve that controls the
    interpolation between this keyframe and the next one.

**Example — a pulse keyframe track on a drop shadow color.**

.. code-block:: cpp

    using namespace OmegaWTK::Composition;

    auto yellow = LayerEffect::DropShadowParams{
        0.f, 12.f, 16.f, 28.f, 0.65f,
        Color::create8Bit(Color::Yellow8)};
    auto cyan = yellow;
    cyan.color = Color::create8Bit(0x00FFFFu);

    auto track = KeyframeTrack<LayerEffect::DropShadowParams>::From({
        KeyframeValue<LayerEffect::DropShadowParams>{
            0.f, yellow, AnimationCurve::EaseInOut()},
        KeyframeValue<LayerEffect::DropShadowParams>{
            1.f, cyan, nullptr}
    });

This track is the kind of value you would attach to a
``StyleSheets::KeyframeAnimationProperty`` on a sheet that
declares a named animation — see the **UI** document's "Keyframe
animations" section for the full sheet authoring pattern.

----

Fonts
=====

FontDescriptor
--------------

.. cpp:struct:: OmegaWTK::Composition::FontDescriptor

    Describes which font to load.

    .. cpp:member:: OmegaCommon::String family

        Family name, e.g. ``"Arial"`` or ``"SF Pro Text"``.

    .. cpp:member:: FontStyle style

        ``Regular``, ``Italic``, ``Bold``, or ``BoldAndItalic``.

    .. cpp:member:: unsigned size

        Point size.

FontEngine
----------

.. cpp:class:: OmegaWTK::Composition::FontEngine

    The process-wide font factory. Created automatically at
    startup; access through ``FontEngine::inst()``.

    .. cpp:function:: virtual Core::SharedPtr<Font> CreateFont(FontDescriptor & desc)

        Loads (or returns a cached handle to) the system font
        matching ``desc``.

    .. cpp:function:: virtual Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor & desc)

        Loads a font from a file path — typical for fonts bundled
        with the application as a ``.ttf`` or ``.otf``.

    .. cpp:function:: static FontEngine * inst()

        Returns the process-wide ``FontEngine`` instance.

.. code-block:: cpp

    using namespace OmegaWTK::Composition;

    FontDescriptor fd{"Helvetica Neue", 16, FontDescriptor::Regular};
    auto font = FontEngine::inst()->CreateFont(fd);

TextLayoutDescriptor
--------------------

.. cpp:struct:: OmegaWTK::Composition::TextLayoutDescriptor

    Controls how text is placed and wrapped inside a bounding
    rect. Every text-bearing widget (``Label``, ``Icon``,
    ``Button``'s label sub-element, ``Button``'s icon sub-element)
    funnels through this descriptor.

    **Alignment values.** ``LeftUpper``, ``LeftCenter``,
    ``LeftLower``, ``MiddleUpper``, ``MiddleCenter``,
    ``MiddleLower``, ``RightUpper``, ``RightCenter``,
    ``RightLower``.

    **Wrapping values.** ``None`` (no wrapping — single-line
    rendering, overflow is clipped), ``WrapByWord``,
    ``WrapByCharacter``.

    **lineLimit.** Maximum line count when wrapping is enabled
    (``0`` = unlimited).
