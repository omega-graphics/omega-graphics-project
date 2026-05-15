========
UI Model
========

.. contents:: On this page
   :local:
   :depth: 3

----

Status
------

This page describes the **proposed** OmegaWTK UI model, as defined by:

- ``Widget-View-Paint-Lifecycle-Plan.md``
- ``UIView-Render-Redesign-Plan.md``
- ``Native-API-Completion-Proposal.md``
- ``Style-StyleSheet-Refactor-Plan.md``

The plans are scheduled to land but are not all in tree yet. Where the
current implementation diverges from this document, the plans are
authoritative — this page tracks where the system is going, not where
it is today. Sections marked *Migration note* call out the deltas
between the in-tree code and the model described here.

----

Overview
--------

OmegaWTK is inspired by Chromium's renderer (Blink) and Aura view
engine. It fully virtualizes the view tree and rendering pipeline, so
that a single layout, style, and paint description produces the same
visual result on every platform.

There are some differences however.
OmegaWTK uses a bottom-left coordinate space.

The architecture is built on a strict separation between the **scene
tree** (the retained virtual UI) and the **platform surface** (a
single OS-owned drawing target per window).

There are five collaborators, and nothing else:

1. **Widgets** — application-facing handles that bundle a View with
   behavior (input delegates, model state, child composition). Under
   the new lifecycle, ``Widget`` is a light wrapper around ``View`` and
   most of its former responsibilities (paint orchestration, frame
   submission) move to ``FrameBuilder``.
2. **Views (SceneNodes)** — the retained scene tree. ``View`` *is* the
   ``SceneNode``: every node carries bounds, a transform, a style
   handle, an optional ``LayoutManager``, children, and a
   ``DirtyBits`` field. Views do **not** own platform items, layer
   trees, canvases, or composition sessions.
3. **DisplayList and DrawOps** — a frame-scoped, flat sequence of
   draw ops that the Paint phase appends to. One DisplayList per
   window per frame.
4. **FrameBuilder** — the per-window engine that runs the five-phase
   lifecycle (Tick → Style → Layout → Paint → Commit) and hands the
   completed DisplayList to the compositor.
5. **The Compositor** — backend-agnostic GPU scheduler that consumes
   DisplayLists, executes them on a dedicated thread, and presents
   the result through a single platform surface per window.

Data flows downward: the developer mutates model state on a Widget;
the Widget sets ``DirtyBits`` on its View subtree; the next frame
boundary runs ``FrameBuilder``, which produces one DisplayList; the
compositor executes it and presents. Control flows upward through
``LayerTreeObserver`` callbacks and ``CompositorClient`` status
futures, surfacing resize, GPU completion, and lifecycle events back
to the scene tree.

----

The Five-Phase Frame Lifecycle
------------------------------

Every frame on every window runs the same five phases, in strict
order, driven by ``FrameBuilder::buildFrame()``. Phases are sequential
on the UI thread; the compositor runs in parallel on its own thread,
consuming DisplayList snapshots.

::

    ┌──────────────────────── Frame Boundary ────────────────────────┐
    │  vsync tick or AppWindow::invalidate() requesting next frame   │
    └────────────────────────────────────────────────────────────────┘
            │
            ▼
    ┌────────────────┐
    │ 1. Tick        │  Animator::tick() per node. Writes resolved
    │                │  animation values into a side table. May set
    │                │  DirtyBit::Paint on animated nodes.
    └────────────────┘
            │
            ▼
    ┌────────────────┐
    │ 2. Style       │  StyleResolver::resolve(node) for nodes with
    │                │  DirtyBit::Style. Cascade global StyleSheet
    │                │  rules, layer inline Style, resolve theme
    │                │  vars, write ComputedStyle. Detect transitions
    │                │  and queue them on the Animator.
    └────────────────┘
            │
            ▼
    ┌────────────────┐
    │ 3. Layout      │  Bottom-up Measure, then top-down Arrange,
    │                │  for nodes with DirtyBit::Layout. Writes
    │                │  finalRect on each node. Sets DirtyBit::Paint
    │                │  where rect changed. Fires onLayoutResolved.
    └────────────────┘
            │
            ▼
    ┌────────────────┐
    │ 4. Paint       │  Top-down walk for nodes with DirtyBit::Paint.
    │                │  Each node appends DrawOps to the window-wide
    │                │  DisplayList. Pure: reads (model + layout +
    │                │  style + animation values), writes only ops.
    └────────────────┘
            │
            ▼
    ┌────────────────┐
    │ 5. Commit      │  Hand the DisplayList to the compositor. One
    │                │  CompositeFrame per window, one deposit. All
    │                │  DirtyBits cleared.
    └────────────────┘

Phase enforcement
^^^^^^^^^^^^^^^^^

``FrameBuilder`` exposes a ``FramePhase`` flag that is checked by
debug-mode assertions in any code that mutates phase-restricted state:

.. list-table::
   :header-rows: 1
   :widths: 35 20 45

   * - Action
     - Legal during
     - Asserts in any other phase
   * - ``Animator::tick()``
     - Tick
     - yes
   * - ``StyleSheet::resolve()``
     - Style
     - yes
   * - ``LayoutManager::measure / arrange``
     - Layout
     - yes
   * - ``DisplayList::append(DrawOp)``
     - Paint
     - yes
   * - ``invalidate()`` / set ``DirtyBits``
     - any
     - never asserts (always deferred)

The critical contract: ``invalidate()`` **never** runs work
synchronously. It sets ``DirtyBits`` and asks the window for a frame.
The next vsync (or explicit ``AppWindow::invalidate()``) runs the
phases.

----

Widgets
-------

Widgets are application-level building blocks. Each Widget bundles a
View (with its scene subtree) and an input ``ViewDelegate``. Widget
subclasses configure layout content and style in response to model
mutations — they do **not** implement custom paint logic.

The Widget paint contract changes substantially from the legacy model:

- The legacy ``onPaint(PaintReason)`` virtual is replaced by an
  optional ``paint(PaintContext &)`` override. For the common case
  (a Widget backed by a ``UIView``), the default ``UIViewNode::paint``
  handles draw-op emission and Widget subclasses do not override.
- Layout element rebuilding moves out of paint and into model
  mutators. ``Rectangle::setProps`` calls ``rebuildContent()`` which
  populates ``UIViewLayoutV2`` and the stylesheet, then sets
  ``DirtyBit::Content | Style | Paint``. The next frame builds.
- ``Widget::executePaint``, the per-Widget composition session dance,
  and the per-Widget ``CompositeFrame`` are deleted.
- ``Widget::invalidate()`` becomes a deferred dirty-flag setter.
  ``invalidateNow()`` survives as an escape hatch for screenshot
  capture and similar synchronous needs, marked deprecated and logged
  in debug builds.

Widget input delegation, focus participation, tooltip text, and child
composition stay on Widget. Tooltip and focus state are virtual —
they live in the scene tree and are committed to the single
per-window OS sink by the ``WidgetTreeHost`` dispatcher.

*Migration note:* current widget subclasses call
``view.update()`` from inside ``onPaint``. The migration tier in
the lifecycle plan retires this pattern: rebuild content in
``setProps``, drop the ``onPaint`` override, let ``FrameBuilder``
drive the frame.

----

Views and the Scene Tree
------------------------

A ``View`` *is* a ``SceneNode``. There is no separate scene-tree type
that wraps Views — the scene tree and the View tree are the same
structure.

A Node carries:

- Parent-relative bounds (``Composition::Rect``) and a
  ``Transform2D``.
- A ``Layout`` struct (the renamed ``LayoutStyle``) — structural
  authoring (``width``, ``padding``, ``flex*``, ``clamp``).
- A ``Style`` struct — per-node inline visual authoring
  (``backgroundColor``, ``border``, ``dropShadow``, ``textFont``,
  ``fillBrush``).
- A ``ComputedStyle`` cache — populated by ``StyleResolver`` in
  Phase 2 and read by Paint in Phase 4.
- An optional ``LayoutManager *`` used to arrange *its own children*.
- A ``DirtyBits`` field (see below).
- Pseudo-class state bits (``:hover`` / ``:pressed`` / ``:focused``
  / ``:disabled``) and an optional set of custom state names, both
  set by the input layer.
- Children, in z-order.
- A virtual ``paint(PaintContext &)``.

A Node does **not** carry: a ``LayerTree``, a ``Canvas``, a
``CompositorClientProxy``, a sync-lane ID, a composition-session
pair, an animation state map, or any per-node platform item.

DirtyBits
^^^^^^^^^

The five legacy UIView dirty flags and the Widget-level paint
guards are collapsed into a single bit field per node:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Bit
     - Meaning
   * - ``Style``
     - Resolved style cache is stale.
   * - ``Layout``
     - Desired or final rect is stale.
   * - ``Content``
     - Element list (shape / text / layout spec) changed.
   * - ``Paint``
     - Subtree must be re-walked in Paint.

Propagation rules:

- Setting any bit propagates ``Paint`` upward to the root, so the
  window knows a frame is needed.
- ``Layout`` propagates upward to the nearest layout boundary.
- ``Style`` does not propagate; each node resolves its own style.

Animation ticks may set ``Paint`` only — they must not touch
``Style`` or ``Layout``.

Public View surface
^^^^^^^^^^^^^^^^^^^

After the migration completes, ``View`` exposes:

- Geometry: ``getRect()``, ``resize()``.
- Visibility: ``enable()`` / ``disable()`` / ``isEnabled()``.
- Hit testing: ``containsPoint(Point2D)``.
- Input: ``setDelegate(ViewDelegate *)``.
- Focus (virtual): ``setFocusable``, ``isFocused``, ``focus``,
  ``blur``.
- Cursor (declarative): ``setCursorShape``, ``cursorShape``.
- Layout: ``setLayoutManager``, ``layout()`` (mutable ``Layout`` ref,
  sets ``DirtyBit::Layout``), ``childCount``, child accessors.
- Style: ``style()`` (mutable ``Style`` ref, sets
  ``DirtyBit::Style | Paint``), ``resolved()`` (read-only
  ``ComputedStyle``).
- Paint: ``virtual void paint(PaintContext &)``.

Removed from the public surface: ``makeLayer``, ``makeCanvas``,
``startCompositionSession`` / ``endCompositionSession``,
``submitPaintFrame``, ``setSyncLaneRecurse``, ``setFrontendRecurse``,
``computeWindowOffset``, ``scrollOffsetContribution``,
``getResizeCoordinator``.

----

LayoutManagers
--------------

Layout is owned by parents, not by children. A node configures
``LayoutManager`` to position its children; the children themselves
have no opinion on where they end up.

::

    class LayoutManager {
    public:
        virtual Size measure(SceneNode & node, Size availableSize) = 0;
        virtual void arrange(SceneNode & node, Rect finalRectLocal) = 0;
        virtual ~LayoutManager() = default;
    };

Built-in implementations:

- ``FillLayout`` — child fills parent content rect.
- ``StackLayout`` — H or V stack using ``Layout`` weights.
- ``AbsoluteLayout`` — child positions itself via its own
  ``Layout`` rect (back-compat path for ``UIViewLayoutV2``).
- ``FlexLayout`` — main-axis distribution wrapping
  ``resolveClampedRect``.

``ViewResizeCoordinator`` is deleted; its responsibilities collapse
into ``LayoutManager``. ``UIViewLayoutV2`` survives as the *authoring*
surface for declarative element specs inside a single UIView, fed
into the scene tree at content-rebuild time, not at paint time.

----

Styling
-------

OmegaWTK splits the legacy per-``UIView`` ``StyleSheet`` into three
orthogonal authoring surfaces, resolved into a per-node
``ComputedStyle`` cache that the Paint phase reads. The split mirrors
Chromium's ``ComputedStyle`` / ``StyleResolver`` / document
stylesheet model.

Three authoring surfaces
^^^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 18 20 62

   * - Surface
     - Scope
     - Carries
   * - ``Layout``
     - per-node, structural
     - ``width``, ``height``, ``margin``, ``padding``, ``flex*``,
       ``clamp``, ``inset*``, ``gap``. Direct field assignment;
       sets ``DirtyBit::Layout``.
   * - ``Style``
     - per-node, inline visual
     - ``backgroundColor``, ``border``, ``dropShadow``,
       ``gaussianBlur``, ``fillBrush``, ``textFont``, ``textColor``,
       ``textAlignment``, ``textWrapping``. Every field is
       ``Optional<>`` — ``nullopt`` defers to the cascade. Sets
       ``DirtyBit::Style | Paint``.
   * - ``StyleSheet``
     - process-global, selector-matched
     - Rule database keyed by ``Selector`` (kind / id / classes /
       pseudo-classes / custom states), with cascade by specificity
       → source order → ``!important``. Owns transition declarations
       and theme variables.

Layout properties are deliberately **not** authored through
``StyleSheet`` rules in the engine. Layout authoring is direct field
assignment on a node's ``Layout``. The Slate split, not the CSS
conflation. The WML compiler may *expose* layout-as-CSS in
``<style>`` blocks, but compiles those properties to ``Layout`` field
assignments at instantiation time, not to ``StyleRule`` records.

Animation is similarly removed from the style surfaces. ``Style``
and ``StyleSheet`` carry **transitions** (declarative
"interpolate this property over N ms") only; imperative animation
tracks live on the ``Animator``.

The global StyleSheet stack
^^^^^^^^^^^^^^^^^^^^^^^^^^^

``Application::styleSheets()`` exposes a vector of
``StyleSheetPtr`` registered with the application:

- A built-in "user agent" sheet at the bottom (``Button`` defaults,
  ``Label`` defaults, etc.).
- One or more author sheets in the middle.
- An optional theme sheet on top (overrides accent colors and
  similar).

Mutating the stack sets ``DirtyBit::Style`` on the root node, which
re-runs the Style phase across the tree on the next frame.

A ``StyleRule`` carries a ``Selector``, computed ``specificity``,
``sourceOrder`` assigned at insertion, an ``important`` flag, a
``Property``, and a typed ``StyleValue``. The Tier-1 selector
grammar is a single compound: ``[Kind] [#Id] [.Class …] [:Pseudo …]``
— enough for ``Button``, ``.primary``, ``Button.primary:disabled``,
``#submit``. CSS combinators (``>``, descendant, ``+``, ``~``) are
deferred to Tier 2.

StyleResolver
^^^^^^^^^^^^^

Phase 2 of the frame lifecycle runs
``StyleResolver::resolve(node)`` for every node carrying
``DirtyBit::Style``::

    void StyleResolver::resolve(SceneNode & node) {
        ComputedStyle out = ComputedStyle::Default();

        // 1. Inherit from parent for inheritable properties
        //    (textColor, font, visibility, cursor — the CSS list).
        if (auto * p = node.parent())
            inheritFrom(out, p->resolved());

        // 2. Cascade matching rules from the global StyleSheet stack.
        //    Honors specificity → source order → !important.
        for (auto & sheet : application().styleSheets())
            for (auto & rule : sheet->matchingRules(node))
                apply(out, rule);

        // 3. Layer inline Style on top — beats any non-!important rule.
        apply(out, node.style());

        // 4. Resolve theme var() references.
        resolveVars(out, application().themeVars());

        node.setResolved(std::move(out));
    }

The cascade order is the standard CSS one: specificity →
source order → ``!important``. Inheritance follows the CSS list
(text-related properties plus ``visibility`` and ``cursor``);
visuals like ``background`` and ``border`` do not inherit.

``matchingRules(node)`` evaluates the selector against the node's
kind, id, class set, pseudo-class state bits, and custom state set.
The Tier-1 implementation is a flat scan; hash bucketing by
tag/kind/class is reserved for when profiling demands it.

Pseudo-classes and state
^^^^^^^^^^^^^^^^^^^^^^^^

Pseudo-class state lives as bits on the node:

- ``:hover`` — set by the hover dispatcher in ``WidgetTreeHost``.
- ``:pressed`` — set by the pointer event router during a press.
- ``:focused`` — set by the virtual ``FocusManager``.
- ``:disabled`` — set by ``View::disable()``.
- ``:checked`` / ``:selected`` — set by widget-specific input handlers.

Custom states (the WML ``:state(name)`` form) are arbitrary string
flags on the node. Flipping any state bit sets ``DirtyBit::Style``
for that node only; the resolver re-runs for the dirty node and any
descendants whose inherited properties changed.

Theme variables
^^^^^^^^^^^^^^^

``Application`` owns a ``ThemeVars`` map (``String → StyleValue``)
plus a ``ThemePtr`` for the active theme. Style values may reference
variables via ``StyleValue::Var("accent")``; the resolver looks up
``accent`` in the theme map at cascade time. Theme swaps replace the
active ``ThemeVars`` and dirty the root with ``DirtyBit::Style``.

Transitions
^^^^^^^^^^^

A ``Transition`` declares "when ``Property`` changes on a matching
node, interpolate from the previous value to the new value over
``durationSec`` using ``curve``"::

    stylesheet->rule(Selector::Class("primary"),
                     Property::Transition,
                     StyleValue::Transitions({
                         {Property::BackgroundColor, 0.16f, easeInOut},
                         {Property::Transform,       0.12f, easeOut},
                     }));

When the resolver detects that a transitioned property changed
between frames, it hands ``(from, to, duration, curve)`` to the
``Animator`` in the next Tick phase. The Animator writes
interpolated values into the per-window animation side table, and
Paint reads the side table when emitting draw ops. This is the CSS
``transition`` model and the only declarative animation surface in
the style system — keyframes and imperative tracks live on the
Animator directly.

ComputedStyle
^^^^^^^^^^^^^

``ComputedStyle`` is a per-node POD whose fields are *resolved* — no
``Optional<>``, every property has a concrete value. Paint reads
``ComputedStyle`` only; it never walks ``StyleSheet`` rules and
never inspects ``Style``. This keeps the Paint phase a pure function
of (model, layout, ``ComputedStyle``, animation values) and is what
makes the Paint walk fast.

----

DisplayList and DrawOps
-----------------------

The DisplayList is a frame-scoped, flat vector of ``DrawOp`` structs
plus a transform / clip / opacity stack. Exactly **one** DisplayList
exists per window per frame, owned by ``FrameBuilder``. Paint
appends; commit hands it to the compositor; the next frame clears
it.

The op set mirrors the post-SDF compositor backend (see
``Direct-To-Drawable-And-SDF-Plan.md``):

- ``Rect``, ``RoundedRect``, ``Ellipse`` — fill brush + optional
  ``Border { color, width }``. The compositor renders these via the
  SDF pipeline as a single draw call per primitive. The DisplayList
  does not emit a separate stroked-path op for the border.
- ``Shadow`` — fill brush + blur amount; soft falloff via SDF.
- ``Path`` — arbitrary ``GVectorPath2D`` (stroke / fill / both),
  triangulated lazily.
- ``Bitmap`` — texture handle + dest rect (+ tint / source-rect /
  nine-slice as later SDF phases land).
- ``Text`` — bitmap-text fallback today, MSDF text run after the
  SDF text phase.
- ``NativeContent`` — a carve-out for an embedded native layer
  (``destRect``, ``hostId``). See *NativeViewHost*.
- State ops: ``PushTransform`` / ``PopTransform``, ``PushClip`` /
  ``PopClip``, ``PushOpacity`` / ``PopOpacity``, ``PushEffect`` /
  ``PopEffect``.

A bordered shape — the most common UIView element — is a single op,
not a fill-op + stroked-path-op pair. A drop shadow on the same shape
is a ``Shadow`` op emitted before the shape op.

DrawOp replaces VisualCommand
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The legacy ``VisualCommand`` type — the per-element command that
``Canvas::draw*`` produced and ``BackendRenderTargetContext::renderToTarget()``
switched on — is **retired** under the new model. ``DrawOp`` is the
new compositor-bound op type. The two are not parallel formats that
need a translation step:

- The post-SDF ``VisualCommand`` shape (one command per primitive,
  fill + border consolidated into the same record, soft shadow as
  its own SDF command) is exactly what ``DrawOp`` was designed to be.
  ``DrawOp::Rect`` / ``RoundedRect`` / ``Ellipse`` / ``Shadow`` map
  1:1 onto the corresponding ``VisualCommand`` types — same fields,
  same semantics, same draw call.
- The legacy ``VisualCommand`` had two producers: per-view ``Canvas``
  (deleted from the public surface) and the SVGView's own canvas
  (also deleted). With both producers gone, ``VisualCommand`` has no
  upstream and no reason to exist as a separate type.
- The compositor backend's ``renderToTarget()`` switch is rewritten
  to dispatch on ``DrawOp`` directly. The migration is a rename of
  the type and a touch-up of the switch arms; backend rasterization
  code (SDF pipeline, triangulator path, bitmap blit, text run) is
  untouched.

In other words: the SDF spine already moved ``VisualCommand`` toward
the shape ``DrawOp`` defines. Retiring the old name and consuming
``DrawOp`` directly removes the duplicate type without changing what
the GPU does. ``CanvasFrame`` (the legacy per-view list of
``VisualCommand``) is similarly retired in favor of the per-window
``DisplayList``.

PaintContext
^^^^^^^^^^^^

``PaintContext`` is the scratch argument threaded through the Paint
walk:

::

    struct PaintContext {
        DisplayList & displayList;
        Transform2D   currentTransform;
        Rect          currentClip;
        float         currentOpacity = 1.0f;
    };

There is no ``Canvas`` on ``PaintContext``, no ``CompositeFrame``,
no composition session. Nodes append, push and pop transform / clip /
opacity, and recurse. Nothing in ``PaintContext`` is stored on the
node.

----

The Per-Window NativeItem and Surface
-------------------------------------

Under the virtual view model, there is **exactly one ``NativeItem``
per window** — the root surface owned by the platform ``AppWindow``
(``CocoaAppWindow`` / ``Win32AppWindow`` / ``GTKAppWindow``). The
virtual View tree composites into this single surface via
``Composition::Layer`` instances managed by the window's single
``LayerTree``.

Consequences:

- Per-View OS features that are really window-level (cursor sink,
  window opacity, key-window state) live on ``NativeWindow`` /
  ``AppWindow``.
- Per-View features that are virtual (which view holds keyboard
  focus, which view's cursor shape is active, which widget's tooltip
  is up) are tracked in the scene tree and committed to the root
  ``NativeItem`` by a dispatcher.
- Hit testing is virtual via ``View::containsPoint``. The hover
  dispatcher in ``WidgetTreeHost`` drives both ``CursorEnter`` /
  ``CursorExit`` events and OS-cursor-shape commits.

DPI scale changes arrive as ``WindowScaleFactorChanged`` events on
the ``NativeWindow`` emitter (Win32 ``WM_DPICHANGED``, macOS
``windowDidChangeBackingProperties:``, GTK
``notify::scale-factor`` / Wayland fractional-scale). The default
``AppWindow`` handler propagates the new scale through
``View::setRenderScale`` so text and bitmaps re-rasterize crisp at
the next frame, without any app-level subscription required.

----

Specialized View Subclasses
---------------------------

Most leaf rendering uses ``UIView`` (declarative element + style).
Four other ``View`` shapes appear in the model.

UIView (UIViewNode)
^^^^^^^^^^^^^^^^^^^

The standard rendering node. Carries a ``UIViewLayoutV2`` element
list, a ``StyleSheetPtr``, and a ``ResolvedStyleCache``. Its
``paint()`` reads the resolved cache and the already-arranged element
rects, and appends one ``DrawOp`` per element to ``pc.displayList``.

Authoring is declarative: ``setLayout`` and ``setStyleSheet`` are
the public mutators; both set ``DirtyBits``. There is no
``UIView::update()`` — that 270-line monolith is replaced by the
phase split above. App code that wants to force a frame calls
``AppWindow::invalidate()``.

SVGView
^^^^^^^

Parses an SVG document into a cached ``DisplayList`` (built from the
source on demand) and bulk-appends it during paint. ``renderNow()``
is removed. Source-document mutators set ``DirtyBit::Content | Paint``;
the rebuild happens during the next ``FrameBuilder`` pass in the
normal Style → Layout → Paint order.

ScrollView
^^^^^^^^^^

The canonical layerization opt-in. Owns a content child and a scroll
offset; its ``paint()`` pushes a clip to the visible bounds, pushes
a translation by ``-scrollOffset``, and lets ``FrameBuilder`` recurse
to children. Scroll bars are emitted as a post-children overlay
(``RoundedRect`` ops). ``scrollOffsetContribution`` and the per-view
overlay layers are deleted; the FrameBuilder transform accumulator
makes descendant paint correct without each descendant knowing
about scroll.

ScrollView returns ``true`` from ``wantsLayer()`` so the compositor
can tag this subtree's DisplayList output for a separate composition
layer in the future. For Tier 3 the tag is a no-op.

NativeViewHost
^^^^^^^^^^^^^^

The umbrella case for every native embed (video, OmegaGTEView, future
WebView, OS-native form controls). NativeViewHost is a ``View``,
participates in layout normally, and emits a single
``DrawOp::NativeContent`` carve-out during paint. The native layer's
position, clip, and visibility are synced from the resolved rect via
the ``onLayoutResolved`` signal — fired by ``FrameBuilder`` at the
end of the Layout phase for any node whose ``finalRect`` changed.

This keeps paint pure: NativeViewHost does not run side effects
during paint. Bounds-sync rides the layout signal, which already
exists.

Airspace contract: native draws on top of any 2D content that would
otherwise be composited inside the host's rect. 2D-over-native
(subtitles, playback overlays) is handled inside the native layer's
own compositing system or by a sibling NativeViewHost the OS
composites above.

CanvasView (deleted)
^^^^^^^^^^^^^^^^^^^^

``CanvasView`` is **deleted** — it does not exist in the new model.
Imperative drawing collapses into UIView with declaratively-authored
elements. Tier 2 of the migration deprecates the imperative methods
to forwarding stubs; Tier 3 removes the header.

VideoView (deleted as a View)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``VideoView`` View subclass is deleted. Video rendering moves to
``VideoViewWidget`` which owns a ``NativeViewHost`` driving a
platform-native video sink (``AVSampleBufferDisplayLayer``,
DXGI video swap chain, GStreamer video sink). See
``NativeViewHost-Adoption-Plan.md``.

----

FrameBuilder
------------

``FrameBuilder`` lives on ``AppWindow`` — one per window — and is the
only entry point for frame production. It replaces
``Widget::executePaint``, ``WidgetTreeHost::paintAndDeposit``,
``UIView::update``, the per-View ``LayerTree``, and the per-View
composition-session dance. A simplified shape:

::

    void FrameBuilder::buildFrame() {
        if (root_ == nullptr || !root_->dirty()) return;

        currentPhase_ = FramePhase::Tick;
        tickSubtree(root_);

        currentPhase_ = FramePhase::Style;
        if (root_->dirty() & DirtyBit::Style)
            resolveStylesSubtree(root_);

        currentPhase_ = FramePhase::Layout;
        if (root_->dirty() & DirtyBit::Layout) {
            measureSubtree(root_, windowRect_.size());
            arrangeSubtree(root_, windowRect_);
        }

        currentPhase_ = FramePhase::Paint;
        displayList_.clear();
        PaintContext pc { displayList_ };
        paintSubtree(root_, pc);

        currentPhase_ = FramePhase::Commit;
        if (!displayList_.empty())
            surface_->deposit(displayList_.replay());
        clearDirtySubtree(root_);

        currentPhase_ = FramePhase::Idle;
    }

``buildFrame()`` is invoked by the window pacer (CVDisplayLink on
macOS, ``IDXGIOutput::WaitForVBlank`` on Windows, frame callbacks on
Wayland). One frame, one ``CompositeFrame``, one deposit.

----

Animation
---------

Animation runs as Phase 1 (Tick) of the frame lifecycle, ahead of
style, layout, and paint. The per-view ``Animator`` (defined by
``Animation-API-Simplification-Plan.md``) ticks active tracks and
writes resolved values into a side table keyed by
``(NodeId, PropertyKey)``.

Paint reads the side table when emitting draw ops; it never writes.
Because the phases are sequential on the UI thread, the Tick → Paint
hand-off needs no locking. This is the Slate model: *Tick advances
state. Paint reads it.*

A node with no active tracks contributes nothing in Tick beyond a
recursion call. If profiling later shows the empty walk to be
expensive, an opt-in ``HasActiveAnimation`` propagation bit can
prune subtrees — deferred until measurement justifies it.

----

Layout
------

Layout runs as Phase 3 of the frame lifecycle. ``FrameBuilder`` runs
two passes for any subtree carrying ``DirtyBit::Layout``:

1. **Measure** (bottom-up). Each ``LayoutManager`` calls
   ``measure(node, available)`` and stores the desired size on the
   node. Children measure before parents so a parent can adapt to
   children's natural sizes when the layout style requests it.
2. **Arrange** (top-down). Each ``LayoutManager`` calls
   ``arrange(node, finalRect)`` to assign the final rect. Sets
   ``DirtyBit::Paint`` on nodes whose rect changed. Fires the
   ``onLayoutResolved`` signal, which ``NativeViewHost`` (and any
   future "tell me when my geometry is settled" subscribers) consume.

The legacy ``LayoutStyle`` + ``resolveClampedRect`` machinery is
preserved (``LayoutStyle`` is renamed to ``Layout`` for symmetry with
``Style``). What changes is *who* calls them — the parent's
``LayoutManager``, never the View itself, and never as a side effect
of paint. ``UIElementLayoutSpec.style`` is renamed to
``UIElementLayoutSpec.layout`` to remove the lexical confusion with
visual ``Style``.

----

Event Routing
-------------

Pointer events
^^^^^^^^^^^^^^

The OS delivers pointer events to the single per-window
``NativeItem``. ``WidgetTreeHost`` walks the virtual scene tree
top-down using ``View::containsPoint`` to find the topmost target
and dispatches ``CursorEnter`` / ``CursorExit`` / ``CursorMove`` /
``MouseDown`` / ``MouseUp`` to that View's delegate. Captured
pointers (drag in progress) bypass the hit walk and route directly.

The hover dispatcher additionally commits the topmost view's
declared ``cursorShape()`` to ``NativeWindow::setCursorShape`` and
arms the tooltip-hover timer.

Keyboard events
^^^^^^^^^^^^^^^

There is one OS keyboard focus per window — the root NativeItem when
the window is key. Per-View focus is virtual and managed by a
``FocusManager`` owned by ``WidgetTreeHost``. ``KeyDown`` / ``KeyUp``
events route to ``focusManager.focusedView()``; ``FocusGained`` /
``FocusLost`` are emitted virtually when the manager changes its
target. Tab traversal (``focusNext`` / ``focusPrevious``) is
host-level keyboard navigation.

DPI / window events
^^^^^^^^^^^^^^^^^^^

``WindowScaleFactorChanged`` arrives on ``NativeWindow``'s emitter
and is consumed by the ``AppWindow`` default handler, which
propagates the new scale into the View tree's render-scale state and
schedules a repaint. App-level subscribers may attach to the same
multi-receiver emitter for custom asset handling.

----

Compositor
----------

The compositor runs on a dedicated background thread and is
backend-agnostic. It does not know about Widgets, Views, the
DisplayList op set in detail, or application logic — it operates on
``CompositorCommand`` handles, render targets, and a priority queue.

State
^^^^^

- **Observed LayerTree** — exactly one ``LayerTree`` per window
  (collapsed from the legacy per-View arrangement). The compositor
  observes one tree per active window.
- **Render target store** — a ``RenderTargetStore`` mapping each
  window's render-target identity to a ``BackendCompRenderTarget``
  that wraps the platform-native present surface.
- **Priority command queue** — an
  ``OmegaCommon::PriorityQueueHeap`` of ``CompositorCommand``
  handles, ordered by ``CompareCommands``. Access serialized by a
  mutex and a condition variable.

Command types
^^^^^^^^^^^^^

All commands derive from ``CompositorCommand`` and carry an id, an
originating client, sync-lane and packet ids, a monotonic sequence
number, a type / priority enum, threshold parameters, and a
``Promise<CommandStatus>``. The concrete subtypes are unchanged from
the legacy architecture:

- ``CompositionRenderCommand`` — replays a DisplayList against a
  layer's GPU texture.
- ``CompositorLayerCommand`` — resize / effect application on a
  layer.
- ``CompositorViewCommand`` — resize on the native window surface.
- ``CompositorCancelCommand`` — drops queued commands by id range.
- ``CompositorPacketCommand`` — atomic group of commands submitted
  by ``FrameBuilder::commit()`` for a single frame.

A frame's DisplayList is wrapped in a ``CompositorPacketCommand`` so
the per-frame sync semantics survive the move from per-View to
per-Window submission. There is now one packet per window per
vsync, rather than N independent per-View submissions racing through
N sync lanes.

Sync lanes
^^^^^^^^^^

A sync lane is a logically independent stream of compositor commands.
Under the new model there is **one sync lane per window**; the
per-View lane fragmentation (and its global atomic) is removed.
Lane admission control still applies: at startup the budget is one
frame in flight; after the first renderable content presents
(``startupStabilized``) it expands to ``kMaxFramesInFlightNormal``
(two frames), matching standard double-buffering.

Command priority ordering
^^^^^^^^^^^^^^^^^^^^^^^^^

The ``CompareCommands`` total order is, in descending precedence:

1. **Structural type priority** — view commands first (window
   resizes must not be starved by render work), then cancels.
2. **Command priority** — among render-like commands, ``High`` ahead
   of ``Low``.
3. **Timestamp** — older commands first.
4. **Threshold presence** — commands with explicit deadlines ahead
   of those without.
5. **Sync lane grouping** — lower-numbered lanes first (deterministic
   per-lane ordering).
6. **Packet ordering** — within a lane, lower packet IDs first.
7. **Sequence number** — global monotonic FIFO tie-breaker.

Scheduler loop
^^^^^^^^^^^^^^

The ``CompositorScheduler`` thread loop:

1. Wait on ``queueCondition`` until the queue is non-empty.
2. Pop the highest-priority command under the queue mutex.
3. ``waitForLaneAdmission()`` — block if the lane is at budget.
4. Threshold wait — sleep until the deadline timestamp if set.
5. Execute via ``Compositor::executeCurrentCommand()``.
6. Present any render targets marked ``needsPresent``.
7. Advance packet lifecycle and per-lane telemetry.
8. Loop.

``shutdownAndJoin()`` sets the shutdown flag, signals the condvar,
and joins.

Render command execution
^^^^^^^^^^^^^^^^^^^^^^^^

Render execution under the new architecture replays a DisplayList
into the window's single render target:

1. Locate or create the window render target via
   ``renderTargetStore`` and ``PreCreatedResourceRegistry``.
2. Resize the target to the DisplayList's snapshot size — recorded
   at frame-build time, not the layer's current live rect.
3. Clear to the frame's background color.
4. For each ``DrawOp`` in the DisplayList, dispatch to
   ``BackendRenderTargetContext::renderToTarget()``. Rect /
   RoundedRect / Ellipse with a populated border field render via
   the SDF pipeline as one draw call. Path goes through the
   triangulator. Bitmap, Text, Shadow, NativeContent, and the state
   ops (PushTransform / PushClip / PushOpacity / PushEffect) each
   have their backend dispatch.
5. Apply any canvas effects via ``BackendCanvasEffectProcessor``.
6. ``commit()`` submits the GPU command buffer with a telemetry
   callback that fires on GPU completion (advances packet lifecycle,
   decrements lane in-flight counter).
7. Mark for presentation; the scheduler presents after the execute
   step returns.

Frame optimizations
^^^^^^^^^^^^^^^^^^^

Two optimizations remain:

- **Stale frame coalescing.** A new render command for a lane that
  already has an unprocessed render command for the same target
  drops the older one (``PacketDropReason::StaleCoalesced``).
- **No-op transparent frame dropping.** After lane stabilization,
  frames consisting entirely of transparent background with no
  visual commands are dropped (``PacketDropReason::NoOpTransparent``).
  Initial frames are exempt; frames with effects or state-only ops
  are exempt.

Packet lifecycle and telemetry
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Every packet flows through the same six-phase lifecycle:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Phase
     - Description
   * - ``Queued``
     - Pushed to the priority queue.
   * - ``Submitted``
     - GPU commands issued to the backend.
   * - ``GPUCompleted``
     - GPU finished executing the packet.
   * - ``Presented``
     - Result is on the display.
   * - ``Dropped``
     - Elided (stale coalescing, no-op, or epoch superseded).
   * - ``Failed``
     - Backend error during submission or execution.

Per-lane telemetry (``LaneRuntimeState``) is unchanged:
``packetsQueued``, ``packetsSubmitted``, ``packetsGPUCompleted``,
``packetsPresented``, ``packetsDropped``, ``packetsFailed``,
``staleCoalescedCount``, ``noOpTransparentDropCount``,
``admissionWaitCount``. Surfaced via
``getLaneTelemetrySnapshot()``, ``getLaneDiagnosticsSnapshot()``,
and ``dumpLaneDiagnostics()``.

Backend abstraction
^^^^^^^^^^^^^^^^^^^

Platform-specific implementations of ``BackendRenderTargetContext``
and ``BackendVisualTree``:

- **macOS / iOS** — ``CALayerTree``: Core Animation layers + Metal
  textures via ``CAMetalLayer``.
- **Windows** — ``DCVisualTree``: DirectComposition visuals + D3D12
  textures.
- **Linux / Android** — ``VKLayerTree``: Vulkan surfaces + textures.

Each backend visual tree contains a root visual connected to the
native present surface and child visuals for any opted-in
composition layers. With one ``LayerTree`` per window, the visual
tree depth and complexity drop sharply compared to the legacy
per-View arrangement.

----

Where the Old Concepts Went
---------------------------

A consolidated mapping for readers familiar with the legacy code:

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Legacy
     - New
   * - ``Widget::executePaint()``
     - Deleted; ``FrameBuilder::buildFrame()`` replaces it.
   * - ``Widget::onPaint(PaintReason)``
     - Replaced by optional ``View::paint(PaintContext &)``.
   * - ``Widget::Impl::paintInProgress`` /
       ``hasPendingInvalidate``
     - Deleted; phase enforcement and deferred invalidation replace
       reentrancy guards.
   * - ``PaintOptions``, ``PaintReason``
     - Deleted; reasons collapse into ``DirtyBits``.
   * - ``UIView::update()`` (270 lines)
     - Split across the Style, Layout, and Paint phases (~80 lines
       in ``UIViewNode::paint``).
   * - ``UIView`` five dirty flags
     - Single ``DirtyBits`` field per node.
   * - ``Container::onPaint → layoutChildren()``
     - ``Container::arrange()`` in the Layout phase.
   * - ``WidgetTreeHost::paintAndDeposit()``
     - ``FrameBuilder::buildFrame()``.
   * - ``View::Impl::ownLayerTree``
     - Deleted; one ``LayerTree`` per window.
   * - ``View::Impl::proxy``
       (per-view ``CompositorClientProxy``)
     - Deleted; one proxy per window.
   * - ``View::startCompositionSession`` /
       ``endCompositionSession``
     - Deleted; ``FrameBuilder`` owns session lifetime.
   * - ``View::makeLayer`` / ``makeCanvas``
     - Removed from the public View surface; ``Canvas`` survives
       only inside the compositor backend as a DrawOp replay target.
   * - ``View::setSyncLaneRecurse`` + global atomic
     - Deleted; one sync lane per window.
   * - ``View::computeWindowOffset`` /
       ``scrollOffsetContribution``
     - Deleted; ``FrameBuilder`` threads a transform stack.
   * - ``UIView::Impl::rootCanvas``
     - Deleted; DisplayList replaces per-View canvases.
   * - ``UIView::Impl`` animation state maps
     - Moved to ``AnimationScheduler`` (per-window) writing a side
       table keyed by ``(NodeId, PropertyKey)``.
   * - ``ViewResizeCoordinator``
     - Deleted; superseded by ``LayoutManager``.
   * - ``localBoundsFromView`` static map
     - Deleted; bounds live on the node.
   * - ``CanvasView``
     - Deleted; imperative drawing folds into UIView.
   * - ``VideoView`` (the View subclass)
     - Deleted; replaced by ``VideoViewWidget`` over
       ``NativeViewHost``.
   * - Per-View ``NativeItem``
     - Deleted; one ``NativeItem`` per window (the root surface).
   * - ``VisualCommand`` (per-element compositor op)
     - Retired; replaced 1:1 by ``DrawOp``. The compositor backend's
       ``renderToTarget()`` switch is rewritten to dispatch on
       ``DrawOp`` directly. No GPU-side changes.
   * - ``CanvasFrame`` (per-view recorded ``VisualCommand`` list)
     - Retired; replaced by the per-window ``DisplayList`` owned by
       ``FrameBuilder``.
   * - ``StyleSheet`` (per-``UIView``, fluent builder)
     - Renamed to ``Style`` — the per-node inline visual surface.
       The name ``StyleSheet`` is reused for the new
       process-global, selector-matched rule set.
   * - ``StyleSheet::Entry`` + ``Entry::Kind``
     - Deleted; ``Style`` is a POD with ``Optional<>`` fields per
       property.
   * - ``StyleSheet::layoutWidth/Height/Margin/Padding/Clamp``
     - Deleted from style authoring; layout authoring is direct
       field assignment on the per-node ``Layout``.
   * - ``StyleSheet::elementAnimation`` /
       ``elementPathAnimation`` / ``elementBrushAnimation``
     - Moved to ``Animator`` tracks. Style sheets retain
       declarative ``Transition`` entries only.
   * - ``UIView::setStyleSheet``
     - ``UIView::setStyle`` (with a deprecated forwarding alias).
   * - ``LayoutStyle``
     - Renamed to ``Layout`` for symmetry with ``Style``.
   * - ``UIElementLayoutSpec.style``
     - Renamed to ``UIElementLayoutSpec.layout``.
   * - ``convertEntriesToRules`` /
       ``mergeLayoutRulesIntoStyle``
     - Folded into ``StyleResolver``; layout authoring is direct
       field assignment, never cascaded.
   * - ``UIView.Style.cpp`` per-view-tag resolver
     - Replaced by process-global ``StyleResolver`` running in
       Phase 2 of the lifecycle.
   * - *(did not exist)*
     - ``Application::styleSheets()`` — global stylesheet stack.
   * - *(did not exist)*
     - ``ComputedStyle`` — per-node resolved style cache that
       Paint reads.
   * - *(did not exist)*
     - ``ThemeVars`` — application-level theme variables resolved
       at cascade time.

Estimated net code reduction across the lifecycle and render-redesign
plans: ~1500 LOC removed, ~800 LOC added. The real win is the
elimination of undefined phase ordering and the collapse of N-ways
fragmented frame paths into one.

----

References
----------

- ``wtk/docs/Widget-View-Paint-Lifecycle-Plan.md`` — the five-phase
  lifecycle, DirtyBits, deferred invalidation, FrameBuilder.
- ``wtk/docs/UIView-Render-Redesign-Plan.md`` — SceneNode, DisplayList,
  LayoutManager, View subclass migrations, per-window LayerTree.
- ``wtk/docs/Native-API-Completion-Proposal.md`` — virtual view
  model, single per-window NativeItem, virtual focus / cursor /
  tooltip dispatch, DPI scale-change handling.
- ``wtk/docs/Direct-To-Drawable-And-SDF-Plan.md`` — SDF-backed
  primitives, the consolidated single-draw-per-shape contract the
  DrawOp set relies on.
- ``wtk/docs/Animation-API-Simplification-Plan.md`` — the per-view
  ``Animator`` whose ``tick()`` runs in Phase 1.
- ``wtk/docs/Style-StyleSheet-Refactor-Plan.md`` — the three-surface
  authoring split (``Layout`` / ``Style`` / ``StyleSheet``),
  ``StyleResolver``, ``ComputedStyle``, theme variables and
  transitions.
- ``wtk/docs/NativeViewHost-Adoption-Plan.md`` — the umbrella
  contract for native embeds (video, GTE, future WebView).
