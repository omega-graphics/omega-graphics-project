========
UI Model
========

.. contents:: On this page
   :local:
   :depth: 3

----

Overview
--------

OmegaWTK's UI model is a layered architecture that separates application
logic from visual presentation and GPU execution. The model has four primary
layers, each with a distinct responsibility:

1. **Widgets** — application-facing objects that define behaviour and
   respond to user input.
2. **Views** — the bridge between widgets and the composition system;
   each View owns a native platform item, a LayerTree, and a
   CompositorClientProxy.
3. **Layers and Canvases** — rectangular drawing surfaces and the
   immediate-mode recording API that produces frames for them.
4. **The Compositor** — a background scheduling engine that receives
   recorded frames, orders them by priority, executes them on the GPU,
   and presents the results to the display.

Data flows downward: a Widget paints by calling Canvas draw methods on
its View's layers, the Canvas records a ``CanvasFrame``, the View submits
that frame through its ``CompositorClientProxy``, and the Compositor
executes it on a background thread. Control flows upward through the
``LayerTreeObserver`` interface and ``CompositorClient`` status futures,
allowing the compositor to notify the rest of the system about resize
events, lifecycle changes, and submission results.

----

Widgets
-------

*Section stub — to be written.*

Widgets are the application-level building blocks. Each Widget owns one
or more Views and defines behaviour through a ``ViewDelegate`` that
receives native input events (mouse enter/exit, mouse down/up, key
down/up). Widget subclasses override ``onPaint`` to draw their visual
content via Canvas calls.

----

Views
-----

*Section stub — to be written.*

A ``View`` is the owner of a platform-native item (``NativeItemPtr``),
a ``LayerTree``, and a ``CompositorClientProxy``. Views are created with
a ``Core::Rect`` defining their position and size. Each View constructs
its own LayerTree with a root Layer at construction time.

Views expose the composition session API:

- ``startCompositionSession()`` — opens the proxy's recording mode,
  allowing Canvas renders and animation commands.
- ``endCompositionSession()`` — closes recording and calls ``submit()``
  on the proxy, packaging all queued commands into a single
  ``CompositorPacketCommand`` and scheduling it with the Compositor.

Views also serve as factories for Layers (``makeLayer``) and Canvases
(``makeCanvas``), enforcing the structural constraint that every Canvas
is bound to exactly one Layer.

----

Layers and LayerTrees
---------------------

*Section stub — to be written.*

A ``LayerTree`` is a per-View tree of ``Layer`` objects. It owns a root
Layer, and child Layers can be added beneath it. The LayerTree notifies
registered ``LayerTreeObserver`` instances (the Compositor) when layers
resize, enable, or disable.

A ``Layer`` is a resizable rectangular surface (``Core::Rect``) that
serves as the render target for a single ``Canvas``. Each Layer may
contain child Layers, forming the sublayer hierarchy. The one-Canvas-
per-Layer invariant is enforced structurally: the ``boundCanvas_``
pointer is set when a Canvas is created for the Layer, and the Canvas
constructor asserts that no other Canvas is already bound.

----

Canvas
------

*Section stub — to be written.*

A ``Canvas`` is the immediate-mode drawing API for a single Layer. It
inherits from ``CompositorClient`` and records ``VisualCommand`` objects
into a ``CanvasFrame``. The drawing primitives are:

- ``drawRect``, ``drawRoundedRect``, ``drawEllipse`` — filled shapes
  with optional borders.
- ``drawPath`` — arbitrary 2D vector paths (stroked and/or filled).
- ``drawText`` — text rendering with font, colour, and layout settings.
- ``drawImage`` / ``drawGETexture`` — bitmap and GPU texture blitting.
- ``drawShadow`` — inline drop shadow geometry for rect, rounded rect,
  and ellipse shapes.
- ``setElementTransform`` — per-element 4x4 transform matrix.
- ``setElementOpacity`` — per-element opacity scalar.
- ``applyEffect`` — canvas-level post-processing (Gaussian blur,
  directional blur).
- ``applyLayerEffect`` — layer-level effects (drop shadow, 3D
  transformation).

Calling ``sendFrame()`` snapshots the current ``CanvasFrame`` — including
the Layer's rect at the time of recording — and pushes it through the
``CompositorClient`` to the ``CompositorClientProxy``'s command queue.

----

The Compositor
--------------

The Compositor is the central scheduling and execution engine of
OmegaWTK's rendering pipeline. It runs on a dedicated background thread
and is responsible for receiving draw commands from all active Views,
ordering them by priority, executing them against the GPU, and presenting
the results to the display. The Compositor does not know about widgets
or application logic — it operates entirely on the command and layer
abstractions provided by the composition subsystem.

Architecture
^^^^^^^^^^^^

The Compositor class inherits from ``LayerTreeObserver`` and maintains
three key pieces of state:

- **Observed LayerTrees** — a vector of ``LayerTree`` pointers registered
  via ``observeLayerTree()``. The Compositor receives notifications when
  any layer in these trees resizes, enables, or disables.

- **Render target store** — a ``RenderTargetStore`` that maps (View,
  Layer) pairs to ``BackendCompRenderTarget`` instances. Each render
  target contains the GPU texture resources needed to rasterise a
  layer's frame and present it to the display. Render targets are
  created lazily on first use.

- **Priority command queue** — an ``OmegaCommon::PriorityQueueHeap``
  of ``CompositorCommand`` handles, ordered by the ``CompareCommands``
  comparator. This queue is the single point of entry for all work
  destined for the GPU.

Access to the queue is serialised by a ``std::mutex``, and a
``std::condition_variable`` (``queueCondition``) wakes the scheduler
thread when new work arrives.

Command types
^^^^^^^^^^^^^

All compositor commands derive from ``CompositorCommand``, which carries:

- A unique ``id`` and a reference to the originating ``CompositorClient``.
- A ``syncLaneId`` and ``syncPacketId`` identifying the sync lane and
  packet the command belongs to.
- A monotonic ``sequenceNumber`` assigned at scheduling time, used as
  the final FIFO tie-breaker.
- A ``Type`` enum (``Render``, ``View``, ``Layer``, ``Cancel``,
  ``Packet``).
- A ``Priority`` enum (``Low``, ``High``).
- Threshold parameters: an optional timestamp and deadline that the
  scheduler can use to delay execution until a target time.
- A ``Promise<CommandStatus>`` that resolves to ``Ok``, ``Failed``, or
  ``Delayed`` when execution completes.

The concrete command subtypes are:

``CompositionRenderCommand``
  Carries a ``SharedHandle<CanvasFrame>`` and a render target reference.
  This is the primary command type — it draws a frame's visual commands
  to a layer's GPU texture.

``CompositorLayerCommand``
  Carries a ``Layer`` pointer and either resize deltas
  (``delta_x/y/w/h``) or a ``LayerEffect``. Used to resize layers or
  apply layer-level effects from the compositor thread.

``CompositorViewCommand``
  Carries a ``NativeItemPtr`` and resize deltas. Used to resize the
  native platform view.

``CompositorCancelCommand``
  Carries a ``startID`` / ``endID`` range. Removes queued commands
  from the specified client that fall within the ID range.

``CompositorPacketCommand``
  Wraps a ``Vector<SharedHandle<CompositorCommand>>`` — an atomic
  group of commands submitted together from a single
  ``CompositorClientProxy::submit()`` call. The packet is the unit of
  synchronisation in the sync lane system.

Command priority ordering
^^^^^^^^^^^^^^^^^^^^^^^^^

The ``CompareCommands`` comparator defines the total order over the
priority queue. Commands are compared by the following criteria, in
descending precedence:

1. **Structural type priority.** View commands are always dequeued first
   (they affect the native window and must not be delayed by rendering
   work). Cancel commands are next.

2. **Command priority.** Among render-like commands (``Render`` and
   ``Packet``), ``High`` priority commands are dequeued before ``Low``.

3. **Timestamp.** Older commands (earlier ``thresholdParams.timeStamp``)
   are dequeued first.

4. **Threshold presence.** Commands with an explicit execution deadline
   (``hasThreshold == true``) are dequeued before those without.

5. **Sync lane grouping.** Commands from lower-numbered sync lanes are
   dequeued first, keeping per-lane ordering deterministic.

6. **Packet ordering.** Within the same sync lane, lower packet IDs are
   dequeued first.

7. **Sequence number.** The global monotonic sequence number assigned at
   ``scheduleCommand()`` time serves as the final FIFO tie-breaker.

This ordering ensures that structural operations (view resizes) are
never starved by rendering work, that high-priority frames (e.g.
interactive animations) jump ahead of background repaints, and that
within a single lane, packets are always processed in submission order.

Sync lanes and packets
^^^^^^^^^^^^^^^^^^^^^^

A **sync lane** is a logically independent stream of compositor commands
associated with a single View. Each ``CompositorClientProxy`` has a
``syncLaneId`` assigned via ``setSyncLaneId()``. When
``View::startCompositionSession()`` / ``endCompositionSession()`` are
called, the proxy batches all commands recorded during the session into
a ``CompositorPacketCommand``, which is a single atomic entry in the
queue identified by ``(syncLaneId, syncPacketId)``.

The sync lane design allows multiple Views to submit work independently
without cross-lane ordering constraints, while guaranteeing strict
ordering within each lane.

Lane admission control
^^^^^^^^^^^^^^^^^^^^^^

The Compositor limits the number of packets in flight per sync lane
to prevent unbounded GPU work accumulation. The
``waitForLaneAdmission()`` method blocks the scheduler thread before
processing a command if the lane's in-flight count has reached its
budget.

The budget is conservative during startup — **one frame in flight** —
to ensure deterministic initial presentation. After the lane has
presented its first renderable content (the ``startupStabilized`` flag),
the budget increases to ``kMaxFramesInFlightNormal``, which is **two
frames**. This matches the standard double-buffering model: one frame
is being displayed while the next is being rendered.

The in-flight counter is decremented when the backend reports GPU
completion via the ``onBackendSubmissionCompleted`` callback, which
signals the queue condition variable to wake the scheduler if it was
blocked on admission.

The CompositorScheduler
^^^^^^^^^^^^^^^^^^^^^^^

The ``CompositorScheduler`` owns the background ``std::thread`` that
continuously processes the command queue. Its lifecycle is:

1. **Wait.** Block on ``queueCondition`` until ``queueIsReady`` is true
   (i.e. the queue is non-empty).

2. **Pop.** Acquire the mutex, pop the highest-priority command from the
   queue, and release the mutex.

3. **Admission check.** Call ``waitForLaneAdmission()`` for the
   command's sync lane. If the lane is over budget, the scheduler blocks
   until a prior packet completes on the GPU.

4. **Threshold wait.** If the command has ``hasThreshold == true``, sleep
   until the deadline timestamp. This supports timed frames for
   animation scheduling.

5. **Execute.** Call ``Compositor::executeCurrentCommand()`` to perform
   the actual GPU work.

6. **Present.** After execution, iterate the render target store and
   present any targets marked ``needsPresent``.

7. **Telemetry.** Advance the packet lifecycle state and update
   per-lane runtime counters.

8. **Loop.** Return to step 1.

Graceful shutdown is handled by ``shutdownAndJoin()``, which sets the
``shutdown`` flag, signals the condition variable, and joins the thread.

Command execution
^^^^^^^^^^^^^^^^^

``executeCurrentCommand()`` dispatches on the command's ``Type``:

**Render commands:**

1. **Locate or create the view render target.** The compositor checks
   ``renderTargetStore`` for an existing ``BackendCompRenderTarget``
   keyed by the command's ``CompositionRenderTarget``. If none exists,
   it looks up pre-created resources from a
   ``PreCreatedResourceRegistry`` (populated during
   ``View::preCreateVisualResources()``). The render target wraps a
   ``BackendVisualTree`` and a native present surface.

2. **Locate or create the layer surface target.** For each layer that
   needs rendering, ``ensureLayerSurfaceTarget()`` obtains or creates a
   ``BackendRenderTargetContext``. For the root layer, this is the
   native present target (a swap chain on DirectX, a ``CAMetalLayer``
   on macOS, or a Vulkan surface). For child layers, this is an
   intermediate GPU texture.

3. **Set render target size.** The render target is resized to match the
   ``CanvasFrame``'s snapshot rect — critically, the rect recorded at
   paint time, not the layer's current live rect. This prevents size
   mismatches when the layer resizes between paint and execution.

4. **Clear.** The target is cleared to the frame's background colour.

5. **Render visual commands.** Each ``VisualCommand`` in the frame is
   passed to ``BackendRenderTargetContext::renderToTarget()``, which
   tessellates geometry and issues GPU draw calls. The visual command
   types are:

   - ``Rect``, ``RoundedRect``, ``Ellipse`` — filled shapes with
     optional borders, rasterised via the tessellation engine.
   - ``VectorPath`` — arbitrary 2D vector paths.
   - ``Text`` — text glyphs rendered through the font engine.
   - ``Bitmap`` — texture blits from CPU images or GPU textures.
   - ``Shadow`` — inline drop shadow geometry.
   - ``SetTransform`` — updates the current 4x4 transform matrix.
   - ``SetOpacity`` — updates the current opacity scalar.

6. **Apply canvas effects.** Any ``CanvasEffect`` entries (Gaussian
   blur, directional blur) are processed by the
   ``BackendCanvasEffectProcessor``, which performs a post-processing
   pass on the rendered texture.

7. **Commit to GPU.** ``BackendRenderTargetContext::commit()`` submits
   the GPU command buffer. The commit call takes a telemetry callback
   that fires when the GPU reports completion, allowing the compositor
   to advance the packet lifecycle and decrement the lane's in-flight
   counter.

8. **Mark for presentation.** The render target's ``needsPresent`` flag
   is set. The scheduler presents all flagged targets after
   ``executeCurrentCommand()`` returns.

**Layer commands:**

- **Resize:** Applies the delta values to the layer's rect and calls
  ``Layer::resize()``.
- **Effect:** Reserved for future layer-level effects processed at
  compositor time (currently effects are handled at canvas time).

**View commands:**

- **Resize:** Applies delta values to the native platform view item.

**Cancel commands:**

- Filters the command queue to remove all commands from the originating
  client whose IDs fall within the specified range.

Frame optimisation
^^^^^^^^^^^^^^^^^^

The Compositor applies two optimisations to avoid unnecessary GPU work:

**Stale frame coalescing.** When a new render command arrives for a sync
lane that already has an unprocessed render command in the queue for the
same target, the older command is dropped via
``dropQueuedStaleForLaneLocked()``. This ensures that if a Canvas
produces frames faster than the compositor can process them, only the
most recent frame is rendered. The dropped command's lifecycle is marked
with ``PacketDropReason::StaleCoalesced``.

**No-op transparent frame dropping.** After a lane has stabilised
(presented its first renderable content), frames that consist entirely
of transparent background with no visual commands are detected by
``shouldDropNoOpTransparentFrame()`` and dropped with reason
``NoOpTransparent``. Initial frames are never dropped, even if
transparent, to ensure deterministic startup behaviour. Frames that
carry canvas effects or state-only commands (transform, opacity) are
also exempt, as they may carry synchronisation semantics.

Packet lifecycle and telemetry
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The Compositor tracks every packet through a six-phase lifecycle:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Phase
     - Description
   * - ``Queued``
     - The packet has been pushed to the priority queue.
   * - ``Submitted``
     - GPU commands have been issued to the backend.
   * - ``GPUCompleted``
     - The GPU has finished executing the packet's work.
   * - ``Presented``
     - The result is visible on the display.
   * - ``Dropped``
     - The packet was elided (stale coalescing, no-op, or epoch
       superseded).
   * - ``Failed``
     - A backend error occurred during submission or execution.

Per-lane runtime state (``LaneRuntimeState``) accumulates counters for
each phase: ``packetsQueued``, ``packetsSubmitted``,
``packetsGPUCompleted``, ``packetsPresented``, ``packetsDropped``,
``packetsFailed``, ``staleCoalescedCount``, ``noOpTransparentDropCount``,
and ``admissionWaitCount``. These are accessible through
``getLaneTelemetrySnapshot()`` and ``getLaneDiagnosticsSnapshot()``, and
can be dumped as a human-readable string via ``dumpLaneDiagnostics()``.

Backend abstraction
^^^^^^^^^^^^^^^^^^^

The Compositor is backend-agnostic. It delegates all GPU work to the
``BackendRenderTargetContext`` and ``BackendVisualTree`` interfaces,
which have platform-specific implementations:

- **macOS / iOS:** ``CALayerTree`` — backed by Core Animation layers and
  Metal textures via ``CAMetalLayer``.
- **Windows:** ``DCVisualTree`` — backed by DirectComposition visuals
  and Direct3D 12 textures.
- **Linux / Android:** ``VKLayerTree`` — backed by Vulkan surfaces and
  textures.

Each ``BackendVisualTree`` contains a root visual connected to the
native present surface and a vector of body visuals for child layers.
Each visual owns its own ``BackendRenderTargetContext``, which manages
the GPU textures, render targets, tessellation engine context, current
transform matrix, and current opacity for that surface.

This abstraction allows the Compositor's scheduling, priority ordering,
lane admission, and telemetry logic to remain entirely platform-
independent, with only the final rasterisation and presentation step
touching platform-specific APIs.

----

Layout
------

*Section stub — to be written.*

----

Animation
---------

*Section stub — to be written.*

----

Event Routing
-------------

*Section stub — to be written.*
