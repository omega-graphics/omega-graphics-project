====================
Triangulation Engine
====================

The Triangulation Engine (TE) converts high-level geometric descriptions
— rectangles, ellipsoids, vector paths — into indexed triangle meshes
ready to draw with the graphics engine. It is the bridge between "this is
a rounded rect with a 4 px stroke" and a vertex buffer the rasteriser can
sample.

You go through it in three steps:

1. Pick a **shape** (or path) and describe it with a CPU math type from
   :doc:`CPUMath` (``GRect``, ``GEllipsoid``, ``GTorus``, ``GVectorPath2D``,
   …).
2. Wrap it in a :cpp:struct:`OmegaGTE::TETriangulationParams` via the
   matching static factory (``TETriangulationParams::Rect``,
   ``::Torus``, ``::GraphicsPath2D``, …) and optionally add per-vertex
   colour or texture attachments.
3. Hand the params to a context's ``triangulateSync`` (current thread),
   ``triangulateAsync`` (worker thread), or ``triangulateOnGPU`` (a
   compute pipeline). You get back a
   :cpp:struct:`OmegaGTE::TETriangulationResult` carrying one ``TEMesh``
   you can upload to a vertex buffer.

.. contents:: On this page
   :local:
   :depth: 2

OmegaTriangulationEngine
========================

The top-level engine is accessed as ``gte.triangulationEngine`` (see
:doc:`Initialization`). It is a factory for *contexts* — one context per
render target you intend to triangulate against. The context binds to the
render target's viewport and, for GPU triangulation, to a command queue.

.. cpp:class:: OmegaGTE::OmegaTriangulationEngine

    .. cpp:function:: SharedHandle<OmegaTriangulationEngineContext> createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget)

        Create a context bound to a native (window) render target. The
        context internally binds to the target's
        :cpp:func:`presentQueue` for any GPU-side triangulation work.

    .. cpp:function:: SharedHandle<OmegaTriangulationEngineContext> createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget, SharedHandle<GECommandQueue> & queue)

        Create a context bound to a texture render target. Texture render
        targets are queue-free (see :doc:`RenderTargets`), so the caller
        supplies the queue that GPU triangulation work runs on.

OmegaTriangulationEngineContext
===============================

A context is the thing you actually triangulate against. It carries the
viewport, an optional GPU pipeline for ``triangulateOnGPU``, and a small
amount of per-context state (currently just the arc-step resolution for
curved shapes).

.. cpp:class:: OmegaGTE::OmegaTriangulationEngineContext

    .. cpp:function:: void setArcStep(float newArcStep)

        Angular resolution used for arcs, circles, and rounded corners
        (radians). Smaller values produce smoother curves at the cost of
        more triangles. Default ``0.01``.

    .. cpp:function:: TETriangulationResult triangulateSync(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr)

        Triangulate on the calling thread. Blocks until done. ``viewport
        = nullptr`` uses the context's default viewport. The
        ``frontFaceRotation`` argument controls how triangles are wound
        — match this to your pipeline's :cpp:member:`polygonFrontFaceRotation`
        (see :doc:`RenderPipeline`) so culling does not silently throw
        away the result.

    .. cpp:function:: std::future<TETriangulationResult> triangulateAsync(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr)

        Triangulate on a worker thread. Returns a future that resolves
        to the result. Use when you can afford to wait one task-system
        tick — most UI workloads.

    .. cpp:function:: std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr)

        Triangulate using a GPU compute pipeline. Returns a future. The
        compute path is the fastest at scale (many large shapes) but
        adds latency at small scale because every call submits to a
        command queue.

Picking sync vs. async vs. GPU
==============================

A short rule of thumb:

* **``triangulateSync``** — one-off shapes during initialisation, simple
  UIs, anything where the calling thread can spend a millisecond on it.
* **``triangulateAsync``** — many shapes per frame, fan out to a thread
  pool. Useful when the shape geometry depends on per-frame data the GPU
  cannot yet see.
* **``triangulateOnGPU``** — at-scale workloads (procedural geometry,
  vector-graphics rendering with many paths), or when you want the
  results to land directly in GPU memory without round-tripping through
  the CPU.

Shapes
======

Each shape factory returns a :cpp:struct:`TETriangulationParams` you can
pass to any of the three triangulation entry points. The shape types
themselves live in :doc:`CPUMath`.

.. cpp:struct:: OmegaGTE::TETriangulationParams

    Built only via the static factories below. Add attachments via
    :cpp:func:`addAttachment` before triangulating.

    .. cpp:function:: static TETriangulationParams Rect(GRect & rect)

        Axis-aligned 2-D rectangle.

    .. cpp:function:: static TETriangulationParams RoundedRect(GRoundedRect & roundedRect)

        2-D rectangle with rounded corners.

    .. cpp:function:: static TETriangulationParams RectangularPrism(GRectangularPrism & rectPrism)

        3-D box.

    .. cpp:function:: static TETriangulationParams Pyramid(GPyramid & pyramid)

        3-D pyramid.

    .. cpp:function:: static TETriangulationParams Ellipsoid(GEllipsoid & ellipsoid)

        Ellipsoid — equal radii give a sphere. For an explicit sphere see
        :cpp:func:`Sphere`.

    .. cpp:function:: static TETriangulationParams Cylinder(GCylinder & cylinder)

        Cylinder aligned to the y-axis.

    .. cpp:function:: static TETriangulationParams Cone(GCone & cone)

        Cone aligned to the y-axis.

    .. cpp:function:: static TETriangulationParams Torus(GTorus & torus)

        Torus.

    .. cpp:function:: static TETriangulationParams Sphere(GSphere & sphere)

        Sphere as a first-class type — typically faster to triangulate
        than an equal-radius ellipsoid and clearer at the call site.

    .. cpp:function:: static TETriangulationParams Capsule(GCapsule & capsule)

        Capsule (cylinder with two hemispherical caps).

    .. cpp:function:: static TETriangulationParams GraphicsPath2D(GVectorPath2D & path, float strokeWidth = 1.f, bool contour = false, bool fill = false, StrokeJoin join = StrokeJoin::Miter, StrokeCap cap = StrokeCap::Butt)

        2-D vector path — stroke, fill, or both.

        * ``strokeWidth`` — width of the stroke in path units. ``0``
          disables the stroke.
        * ``contour`` — close the stroke back to the first point.
        * ``fill`` — triangulate the interior. Uses the second attachment
          for vertex colour / texcoord when present.
        * ``join`` — how interior vertices connect (see *Stroke joins
          and caps* below).
        * ``cap`` — how endpoints of an *open* path are terminated.

    .. cpp:function:: static TETriangulationParams GraphicsPath3D(unsigned vectorPathCount, GVectorPath3D * const vectorPaths, float strokeWidth = 1.f)

        One or more 3-D vector paths swept into a surface.

    .. cpp:function:: void addAttachment(const Attachment & attachment)

        Add a colour or texture-coordinate attachment to the params. See
        *Attachments* below.

Stroke joins and caps
=====================

When you stroke a 2-D path with width ``> 0``, the triangulator has to
decide what to do at *interior* vertices (where two segments meet) and at
*endpoints* of an open path. These are controlled by the ``join`` and
``cap`` parameters of :cpp:func:`GraphicsPath2D`.

.. cpp:enum-class:: OmegaGTE::StrokeJoin

    +-----------+----------------------------------------------------------+
    | ``Miter`` | Extend the outer edges until they meet. Falls back to    |
    |           | ``Bevel`` past the miter limit (sharp interior angles).  |
    +-----------+----------------------------------------------------------+
    | ``Round`` | Arc of triangles between the two segment normals.        |
    +-----------+----------------------------------------------------------+
    | ``Bevel`` | A single triangle bridging the outer endpoints.          |
    +-----------+----------------------------------------------------------+

.. cpp:enum-class:: OmegaGTE::StrokeCap

    +------------+----------------------------------------------------------+
    | ``Butt``   | No extension past the endpoint.                          |
    +------------+----------------------------------------------------------+
    | ``Round``  | Semicircle fan, radius ``strokeWidth/2``.                |
    +------------+----------------------------------------------------------+
    | ``Square`` | Rectangular extension by ``strokeWidth/2``.              |
    +------------+----------------------------------------------------------+

Attachments
===========

Attachments associate per-vertex data with the triangulated mesh so the
resulting vertex buffer carries that data directly. Currently three
attachment types: a flat colour, a 2-D UV coordinate generator, and a
3-D UVW coordinate generator. A texture attachment can optionally bind
its own :cpp:class:`GETexture` so the shader knows which texture the UVs
were authored for.

.. cpp:struct:: OmegaGTE::TETriangulationParams::Attachment

    .. cpp:function:: static Attachment makeColor(const FVec<4> & color)

        Assign a flat colour to every vertex.

    .. cpp:function:: static Attachment makeTexture2D(unsigned width, unsigned height, SharedHandle<GETexture> texture = nullptr)

        Assign 2-D UV coordinates derived from the supplied extent.
        Optionally associate the source texture for use by the
        downstream pipeline.

    .. cpp:function:: static Attachment makeTexture3D(unsigned width, unsigned height, unsigned depth, SharedHandle<GETexture> texture = nullptr)

        3-D UVW coordinates with an optional source 3-D texture.

TETriangulationResult
=====================

A triangulation produces exactly one :cpp:struct:`TEMesh`. The result
struct lets you move / rotate / scale the mesh after the fact (a CPU-side
transform — call before uploading to a vertex buffer).

.. cpp:struct:: OmegaGTE::TETriangulationResult

    .. cpp:member:: TEMesh mesh

        The triangulated mesh.

    .. cpp:function:: unsigned totalVertexCount()

        Sum of vertex counts across the mesh's polygons (multiplied by 3
        for triangle topology).

    .. cpp:function:: void translate(float x, float y, float z, const GEViewport & viewport)

        Translate every polygon in the mesh. ``viewport`` is used to map
        screen-space offsets to NDC where needed.

    .. cpp:function:: void rotate(float pitch, float yaw, float roll)

        Rotate every polygon about the origin.

    .. cpp:function:: void scale(float w, float h, float l)

        Scale every polygon.

.. cpp:struct:: OmegaGTE::TETriangulationResult::TEMesh

    .. cpp:type:: topology

        ``TopologyTriangle`` (triangle list) or ``TopologyTriangleStrip``.

    .. cpp:type:: Polygon

        Three vertices ``a``, ``b``, ``c``, each carrying a :cpp:struct:`GPoint3D`
        position and an optional :cpp:struct:`AttachmentData` payload.

    .. cpp:member:: std::vector<Polygon> vertexPolygons

        The triangle list (or strip).

    .. cpp:function:: unsigned vertexCount()

        Number of distinct vertices in this mesh.

    Per-mesh ``translate``, ``rotate``, and ``scale`` mirrors of the
    parent helpers.

.. cpp:struct:: OmegaGTE::TETriangulationResult::AttachmentData

    Per-vertex payload contributed by the params' attachments.

    .. cpp:member:: FVec<4> color
    .. cpp:member:: FVec<2> texture2Dcoord
    .. cpp:member:: FVec<3> texture3Dcoord
    .. cpp:member:: FVec<3> normal

Complete example
================

.. code-block:: cpp

    auto teCtx = gte.triangulationEngine->createTEContextFromNativeRenderTarget(nativeRT);
    teCtx->setArcStep(0.005f);   // smoother curves

    // 2-D rounded rect with a flat colour.
    OmegaGTE::GRoundedRect r{{ 0.1f, 0.1f }, 0.8f, 0.6f, 0.1f, 0.1f };
    auto rectParams = OmegaGTE::TETriangulationParams::RoundedRect(r);
    rectParams.addAttachment(
        OmegaGTE::TETriangulationParams::Attachment::makeColor(
            OmegaGTE::makeColor(0.2f, 0.6f, 1.0f, 1.0f)));
    auto rectResult = teCtx->triangulateSync(rectParams);

    // 3-D sphere positioned 5 units away from the camera.
    OmegaGTE::GSphere sphere{ 0.f, 0.f, 0.f, 1.f };
    auto sphereParams  = OmegaGTE::TETriangulationParams::Sphere(sphere);
    auto sphereResult  = teCtx->triangulateSync(sphereParams);
    sphereResult.translate(0.f, 0.f, -5.f, viewport);
    sphereResult.scale(2.f, 2.f, 2.f);

    // 2-D path stroked with round joins and round caps.
    OmegaGTE::GVectorPath2D path({ 0.f, 0.5f });
    path.append({ -0.5f, -0.5f });
    path.append({  0.5f, -0.5f });
    auto pathParams = OmegaGTE::TETriangulationParams::GraphicsPath2D(
        path,
        /*strokeWidth=*/2.f,
        /*contour=*/true,
        /*fill=*/false,
        OmegaGTE::StrokeJoin::Round,
        OmegaGTE::StrokeCap::Round);
    auto pathResult = teCtx->triangulateSync(pathParams);

    // GPU-accelerated, async — fire off many shapes and gather later.
    auto fut = teCtx->triangulateOnGPU(sphereParams);
    // …other work…
    auto gpuSphereResult = fut.get();

Common pitfalls
===============

* **``frontFaceRotation`` mismatch.** If the triangulator winds triangles
  one way and the pipeline expects the other, back-face culling discards
  every triangle and the mesh is invisible. Set both consistently.
* **Treating ``triangulateOnGPU`` as zero-cost.** It submits work onto a
  command queue. For one-shape calls the sync path is faster; the GPU
  path wins at scale.
* **Triangulating without an attachment.** A bare mesh has no per-vertex
  colour or UV. Add :cpp:func:`makeColor` or :cpp:func:`makeTexture2D`
  before triangulating if your fragment shader expects those inputs.
* **Confusing ``GSphere`` with ``GEllipsoid``.** Both can produce a
  sphere — :cpp:func:`Sphere` is the explicit-sphere path and is
  typically cheaper. Use ``GEllipsoid`` when the radii really differ.
* **Forgetting that ``arcStep`` is global to the context.** A smaller
  value smooths every curved shape on the context — including ones you
  did not want to subdivide. Set it close to where you triangulate.
