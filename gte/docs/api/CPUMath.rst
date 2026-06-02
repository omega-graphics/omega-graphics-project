==============
CPU Math Types
==============

OmegaGTE's math types are split between two worlds. **CPU math types** live
in your application code: you build the data here and hand it to
:cpp:struct:`GEBufferWriter` for upload, or to the
:doc:`Triangulation Engine <Triangulation>` to convert geometric shapes
into mesh data. **GPU math types** live in OmegaSL shader code and are
documented in :doc:`../OmegaSL`. This page covers the CPU side.

The CPU types come in three flavours: **geometric primitives** (points,
rectangles, prisms, spheres, …) used to describe shapes; **vector paths**
(2-D and 3-D poly-lines, plus their builders) used to describe stroked or
filled paths; and **matrix and vector types** used for transforms and
shader input data.

.. contents:: On this page
   :local:
   :depth: 2

Geometric Primitives
====================

These are the value types used to describe shapes to the
:doc:`Triangulation Engine <Triangulation>` and to define regions for
buffer writes. They are deliberately simple aggregates — no
constructors, no methods, just data — so brace-initialisation is the
idiomatic way to build them.

.. cpp:struct:: OmegaGTE::GPoint2D

    A 2-D point.

    .. cpp:member:: float x
    .. cpp:member:: float y

.. cpp:struct:: OmegaGTE::GPoint3D

    A 3-D point.

    .. cpp:member:: float x
    .. cpp:member:: float y
    .. cpp:member:: float z

.. cpp:struct:: OmegaGTE::GRect

    A 2-D axis-aligned rectangle.

    .. cpp:member:: GPoint2D pos

        Upper-left corner.

    .. cpp:member:: float w
    .. cpp:member:: float h

        Width and height.

.. cpp:struct:: OmegaGTE::GRoundedRect

    A rectangle with uniform corner rounding.

    .. cpp:member:: GPoint2D pos
    .. cpp:member:: float w
    .. cpp:member:: float h
    .. cpp:member:: float rad_x
    .. cpp:member:: float rad_y

        Corner radii on the x and y axes.

.. cpp:struct:: OmegaGTE::GRectangularPrism

    A 3-D box.

    .. cpp:member:: GPoint3D pos
    .. cpp:member:: float w
    .. cpp:member:: float h
    .. cpp:member:: float d

.. cpp:struct:: OmegaGTE::GCylinder

    A cylinder aligned to the y-axis.

    .. cpp:member:: GPoint3D pos
    .. cpp:member:: float r

        Radius.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GPyramid

    A pyramid.

    .. cpp:member:: float x
    .. cpp:member:: float y
    .. cpp:member:: float z

        Base centre.

    .. cpp:member:: float w
    .. cpp:member:: float d

        Base width and depth.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GCone

    A cone aligned to the y-axis.

    .. cpp:member:: float x
    .. cpp:member:: float y
    .. cpp:member:: float z
    .. cpp:member:: float r

        Base radius.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GEllipsoid

    An ellipsoid. Use equal radii for a sphere — or reach for
    :cpp:struct:`GSphere` below, which is typically faster to triangulate
    and clearer at the call site.

    .. cpp:member:: float x
    .. cpp:member:: float y
    .. cpp:member:: float z

        Centre.

    .. cpp:member:: float rad_x
    .. cpp:member:: float rad_y
    .. cpp:member:: float rad_z

        Semi-axis radii.

.. cpp:struct:: OmegaGTE::GSphere

    An explicit sphere.

    .. cpp:member:: GPoint3D center
    .. cpp:member:: float radius

.. cpp:struct:: OmegaGTE::GTorus

    A torus.

    .. cpp:member:: GPoint3D center

        Centre of the torus.

    .. cpp:member:: float majorRadius

        Distance from the torus's centre to the centre of the tube
        ("R").

    .. cpp:member:: float minorRadius

        Radius of the tube itself ("r").

.. cpp:struct:: OmegaGTE::GCapsule

    A capsule — a cylinder with two hemispherical caps.

    .. cpp:member:: GPoint3D pos

        Centre of the bottom hemisphere.

    .. cpp:member:: float radius

        Hemisphere radius (also the cylinder radius).

    .. cpp:member:: float height

        Distance between the two hemisphere centres. The total height
        of the capsule is ``height + 2 * radius``.

.. cpp:struct:: OmegaGTE::GArc

    A 2-D arc.

    .. cpp:member:: GPoint2D center
    .. cpp:member:: float radians

        Sweep angle (radians).

    .. cpp:member:: unsigned radius_x
    .. cpp:member:: unsigned radius_y

        Semi-axis radii on the x and y axes (an arc on an ellipse if
        these differ).

Vector Paths
============

A vector path is a connected polyline made of points. The two basic types
:cpp:class:`GVectorPath2D` and :cpp:class:`GVectorPath3D` store just the
points; you append to them and pass them to the triangulator. The
**builders** :cpp:class:`GPathBuilder2D` and :cpp:class:`GPathBuilder3D`
let you describe paths in the higher-level vocabulary of SVG / PostScript
— ``moveTo``, ``lineTo``, ``quadTo`` (quadratic Bézier), ``cubicTo``
(cubic Bézier), ``arcTo`` — and they flatten the curves to a polyline at
``build()`` time with a configurable tolerance.

Reach for a builder when the path has curves; reach for the bare
``GVectorPath2D`` / ``3D`` when you already have a polyline.

GVectorPath2D / GVectorPath3D
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GVectorPath2D

    A 2-D polyline built by appending :cpp:struct:`GPoint2D` points.

    .. cpp:function:: void append(const GPoint2D & pt)
    .. cpp:function:: void append(GPoint2D && pt)

        Append a point. Each new point creates a segment from the
        previously-last point.

    .. cpp:function:: GPoint2D & firstPt()
    .. cpp:function:: GPoint2D & lastPt()

        First / last point references.

    .. cpp:function:: float mag()

        Total arc length of the polyline.

    .. cpp:function:: std::string toStr()

        Debug serialisation — one line per segment.

.. cpp:class:: OmegaGTE::GVectorPath3D

    The 3-D equivalent of :cpp:class:`GVectorPath2D`, with
    :cpp:struct:`GPoint3D` points.

.. code-block:: cpp

    // A bare polyline triangle outline.
    OmegaGTE::GVectorPath2D path({ 0.f, 1.f });
    path.append({ -0.5f, -0.5f });
    path.append({  0.5f, -0.5f });
    path.append({ 0.f, 1.f });   // close back to the start

    auto params = OmegaGTE::TETriangulationParams::GraphicsPath2D(
        path, /*strokeWidth=*/3.f, /*contour=*/true, /*fill=*/false);

GPathBuilder2D
~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GPathBuilder2D

    Builds a 2-D vector path from mixed linear, Bézier, and arc
    segments. Curves are flattened to a polyline at :cpp:func:`build`
    time using a configurable *tolerance* — the maximum allowed
    deviation (in path units) between a curve and its linear
    approximation. The result is an ordinary :cpp:class:`GVectorPath2D`.

    .. cpp:function:: explicit GPathBuilder2D(float tolerance = 0.25f)

        ``tolerance`` controls how finely curves are subdivided. Smaller
        is smoother but produces more points. Default ``0.25`` path
        units.

    .. cpp:function:: void moveTo(float x, float y)

        Sets the starting point. After points exist, ``moveTo`` behaves
        like ``lineTo`` — a path builder produces one connected subpath.

    .. cpp:function:: void lineTo(float x, float y)

        Straight line to ``(x, y)`` from the current end.

    .. cpp:function:: void quadTo(float cx, float cy, float x, float y)

        Quadratic Bézier with control point ``(cx, cy)`` ending at
        ``(x, y)``.

    .. cpp:function:: void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y)

        Cubic Bézier with control points ``(c1x, c1y)`` and
        ``(c2x, c2y)``.

    .. cpp:function:: void arcTo(float rx, float ry, float rotation, bool largeArc, bool sweep, float x, float y)

        SVG-style elliptical arc from the current point to ``(x, y)``.
        ``rx`` / ``ry`` are the semi-axes; ``rotation`` is the
        x-axis rotation in radians; ``largeArc`` / ``sweep`` pick which
        of the four candidate arcs.

    .. cpp:function:: void close()

        Connect the end back to the starting point.

    .. cpp:function:: GVectorPath2D build() const

        Materialise the flattened polyline as a
        :cpp:class:`GVectorPath2D`.

.. code-block:: cpp

    // A rounded-corner rectangle built from a moveTo + lineTo + arcTo chain.
    OmegaGTE::GPathBuilder2D b(0.1f);     // tolerance — smoother corners
    b.moveTo(10, 30);
    b.lineTo(90, 30);
    b.arcTo(20, 20, 0, false, true, 100, 40);
    b.lineTo(100, 90);
    b.cubicTo(100, 110, 80, 110, 60, 100);
    b.close();
    OmegaGTE::GVectorPath2D path = b.build();

GPathBuilder3D
~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GPathBuilder3D

    Builds a 3-D vector path from linear and Bézier segments. Mirrors
    :cpp:class:`GPathBuilder2D`; Bézier curves generalise directly to
    3-D. There is no ``arcTo`` because an elliptical arc requires a
    chosen 2-D plane.

    Same ``moveTo`` / ``lineTo`` / ``quadTo`` / ``cubicTo`` / ``close``
    / ``build`` API as the 2-D builder, with :cpp:struct:`GPoint3D`
    coordinates.

Matrices and column vectors
===========================

The :cpp:class:`OmegaGTE::Matrix` template is a column-major statically
sized matrix used for transforms (MVP matrices) and as the C++-side
representation of OmegaSL ``float4x4`` / ``int4`` / etc. when uploading
through :cpp:struct:`GEBufferWriter`. The actual ``Matrix<C, R, Ty>``
template is verbose; OmegaGTE provides type aliases for every common
shape.

.. cpp:class:: template<class Ty, unsigned column, unsigned row> OmegaGTE::Matrix

    Statically-sized column-major matrix. Common spellings are the
    aliases below — they save typing and read better at call sites.

    +--------------------+--------------------------------------+
    | ``FMatrix<C, R>``  | ``float`` matrix, C columns × R rows |
    +--------------------+--------------------------------------+
    | ``IMatrix<C, R>``  | ``int`` matrix                       |
    +--------------------+--------------------------------------+
    | ``UMatrix<C, R>``  | ``unsigned int`` matrix              |
    +--------------------+--------------------------------------+
    | ``DMatrix<C, R>``  | ``double`` matrix                    |
    +--------------------+--------------------------------------+

    Single-row matrices (column vectors) have shorter aliases:

    +------------------+-----------------------------+
    | ``FVec<N>``      | ``float`` column vector     |
    +------------------+-----------------------------+
    | ``IVec<N>``      | ``int`` column vector       |
    +------------------+-----------------------------+
    | ``UVec<N>``      | ``unsigned int`` column vec |
    +------------------+-----------------------------+
    | ``DVec<N>``      | ``double`` column vector    |
    +------------------+-----------------------------+

    .. cpp:function:: static Matrix Create()

        Zero-initialised matrix.

    .. cpp:function:: static Matrix Identity()

        Identity matrix. Only valid for square shapes.

    .. cpp:function:: row_pointer_wrapper operator[](size_type col)

        Column accessor. Use ``m[col][row]`` to read or write a single
        element.

    .. cpp:function:: Matrix transposed() const

        Transpose.

    .. cpp:function:: const Ty* data() const
    .. cpp:function:: Ty* data()

        Raw element pointer — use for direct upload to a buffer when you
        already know the layout matches.

    **Arithmetic operators**: ``+``, ``-``, ``*`` (matrix-matrix and
    scalar), ``+=``, ``-=``, ``*=``, unary ``-``, ``==``, ``!=``.

.. code-block:: cpp

    auto mvp   = OmegaGTE::FMatrix<4,4>::Identity();
    auto scale = OmegaGTE::FMatrix<4,4>::Create();
    scale[0][0] = 2.f;
    scale[1][1] = 2.f;
    scale[2][2] = 2.f;
    scale[3][3] = 1.f;

    auto result = mvp * scale;

    // float4 column vector
    auto color = OmegaGTE::FVec<4>::Create();
    color[0][0] = 1.0f;   // r
    color[1][0] = 0.5f;   // g
    color[2][0] = 0.0f;   // b
    color[3][0] = 1.0f;   // a

    // Convenience helper that produces the same thing in one call.
    auto red = OmegaGTE::makeColor(1.f, 0.f, 0.f, 1.f);

    // Direct upload — the raw pointer feeds a GEBuffer via writer.setRaw or
    // a direct memcpy where the layout is known to match.
    const float * raw = result.data();

Quaternions
===========

For rotations, use a :cpp:struct:`Quaternion` rather than composing a
chain of 4×4 rotation matrices. Quaternions are cheaper to interpolate
(``slerp`` between two orientations is one of OmegaGTE's primitives) and
do not suffer from gimbal lock the way Euler-angle pipelines do.

The :cpp:struct:`OmegaGTE::Quaternion` template parameterises over the
scalar type; the alias :cpp:type:`FQuaternion` is the float specialisation
you will use 95% of the time.

.. cpp:struct:: template<class Ty> OmegaGTE::Quaternion

    .. cpp:member:: Ty x
    .. cpp:member:: Ty y
    .. cpp:member:: Ty z
    .. cpp:member:: Ty w

    **Construction**

    .. cpp:function:: static Quaternion Identity()

        ``(0, 0, 0, 1)`` — no rotation.

    .. cpp:function:: static Quaternion fromAxisAngle(Ty ax, Ty ay, Ty az, Ty radians)

        Rotation of ``radians`` around the unit axis ``(ax, ay, az)``.

    .. cpp:function:: static Quaternion fromEuler(Ty pitch, Ty yaw, Ty roll)

        Build from Euler angles applied X (pitch) → Y (yaw) → Z (roll).

    .. cpp:function:: static Quaternion fromMatrix(const Matrix<Ty, 4, 4> & m)

        Extract the rotation component from a 4×4 matrix. Uses
        Shepperd's method for numerical stability.

    **Operators**

    ``q1 * q2`` is the Hamilton product (composes rotations — applies
    ``q2`` first, then ``q1``). ``q * scalar``, ``q + q``, ``q - q``,
    unary ``-q`` are all defined.

    **Operations**

    .. cpp:function:: Ty lengthSquared() const
    .. cpp:function:: Ty length() const
    .. cpp:function:: Quaternion normalized() const
    .. cpp:function:: Quaternion conjugate() const
    .. cpp:function:: Quaternion inverse() const
    .. cpp:function:: Ty dot(const Quaternion & o) const

    **Conversion**

    .. cpp:function:: Matrix<Ty,4,4> toMatrix() const

        4×4 rotation matrix (no translation, no scale).

    **Interpolation**

    .. cpp:function:: static Quaternion nlerp(const Quaternion & a, const Quaternion & b, Ty t)

        Normalised linear interpolation. Cheaper than ``slerp`` and
        nearly identical for small angular differences.

    .. cpp:function:: static Quaternion slerp(const Quaternion & a, const Quaternion & b, Ty t)

        Spherical linear interpolation. ``t=0`` → ``a``; ``t=1`` → ``b``.
        Follows the shortest arc.

.. cpp:type:: OmegaGTE::FQuaternion

    Alias for ``Quaternion<float>`` — the default for most code.

.. code-block:: cpp

    auto rest    = OmegaGTE::FQuaternion::Identity();
    auto rolled  = OmegaGTE::FQuaternion::fromAxisAngle(0, 0, 1, M_PI_4);
    auto chained = rolled * rest;                  // compose rotations
    auto m       = chained.toMatrix();             // 4x4 for upload

    // Animate between two orientations.
    auto start  = OmegaGTE::FQuaternion::fromEuler(0, 0, 0);
    auto end    = OmegaGTE::FQuaternion::fromEuler(0, M_PI, 0);
    auto mid    = OmegaGTE::FQuaternion::slerp(start, end, 0.5f);

Heritage 2-D / 3-D vectors
==========================

For code that explicitly works with 2-D / 3-D direction vectors in
polar form (magnitude + angle), OmegaGTE keeps a pair of classic
``Vector`` types separate from the generic ``FVec<N>``. They store
``i, j (, k)`` components, expose dot and cross products, and offer
constructors from polar pairs.

Most new code uses :cpp:type:`FVec\<2\>` / :cpp:type:`FVec\<3\>` — they
upload through :cpp:struct:`GEBufferWriter` more naturally. Reach for
``FVector2D`` / ``FVector3D`` when polar-style construction or the
angle methods are useful.

.. cpp:class:: OmegaGTE::FVector2D

    A 2-D float vector (``i``, ``j`` components).

    .. cpp:function:: static FVector2D FromMagnitudeAndAngle(float mag, float angle)

        Construct from polar coordinates (radians).

    .. cpp:function:: float & getI()
    .. cpp:function:: float & getJ()

    .. cpp:function:: virtual float mag()

        Euclidean length.

    .. cpp:function:: float angle()

        Angle relative to the i-axis (radians).

    .. cpp:function:: float dot(const FVector2D & vec)

    **Operators**: ``+``, ``-``, ``+=``, ``-=``.

.. cpp:class:: OmegaGTE::FVector3D

    A 3-D float vector (``i``, ``j``, ``k`` components).

    .. cpp:function:: static FVector3D FromMagnitudeAndAngles(float mag, float angle_v, float angle_h)

    .. cpp:function:: float & getK()

    .. cpp:function:: virtual float mag()

    .. cpp:function:: float angle_h()

        Horizontal angle measured from ``i`` (radians).

    .. cpp:function:: float angle_v()

        Vertical angle measured from the ``i + k`` plane (radians).

    .. cpp:function:: float dot(const FVector3D & vec)

    .. cpp:function:: FVector3D cross(FVector3D & vec)

        Cross product.

Integer variants :cpp:type:`IVector2D` and :cpp:type:`IVector3D` are
available with the same shape.

.. code-block:: cpp

    OmegaGTE::FVector3D forward(0.f, 0.f, -1.f);
    OmegaGTE::FVector3D up(0.f, 1.f, 0.f);
    OmegaGTE::FVector3D right = forward.cross(up);

    float len = forward.mag();      // 1.0

    // Polar construction — unit vector at 45°.
    auto v = OmegaGTE::FVector2D::FromMagnitudeAndAngle(1.f, 0.7854f);

Common pitfalls
===============

* **Using** ``Matrix::data()`` **to upload a struct field.** The pointer
  is to a tightly-packed column-major float array; this matches std430
  for square matrices but **not** std140 — for uniform-role buffers,
  go through :cpp:func:`GEBufferWriter::writeFloat4x4` so the role-aware
  layout is applied. See :doc:`Buffers`.
* **Composing rotations the wrong way around.** Quaternion multiplication
  composes "right then left": ``q1 * q2`` rotates by ``q2`` first.
* **Mixing** ``GVectorPath2D`` **construction styles.** ``moveTo`` after
  points exist behaves like ``lineTo``; if you need a brand-new subpath,
  build a separate :cpp:class:`GPathBuilder2D`.
* **Picking** ``GEllipsoid`` **for a sphere.** ``GSphere`` is cheaper to
  triangulate and clearer at the call site. Use ``GEllipsoid`` only when
  the three radii really differ.
* **Forgetting that builder tolerance is in path units.** A 0.25 tolerance
  on a path expressed in pixels is fine; on a path in NDC it is a quarter
  of the screen. Scale the tolerance to the units you use.
