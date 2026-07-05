===
API
===

This page documents AQUA's public C++ API — every type and function a consumer
(Omega kREATE, a tool, a test) can use. The whole surface lives in
``include/aqua/`` and, per the house style in ``AGENTS.md``, uses **no
namespace**: every public name carries the ``AQ`` prefix (``AQContext``,
``AQRigidBody``, ``AQBodyDesc``). The one deliberate exception is the math
layer, which borrows ``OmegaGTE::Matrix`` and ``OmegaGTE::Quaternion`` directly
rather than re-deriving numerically sensitive linear algebra — vectors at the
public surface are ``OmegaGTE::FVec<3>`` and orientations are
``OmegaGTE::FQuaternion``.

.. contents:: On this page
   :local:
   :depth: 2

----

Headers
=======

AQUA has no umbrella ``aqua.h``. Each translation unit includes only the
headers it needs. The two you reach for first are ``AQContext.h`` (the engine
entry point) and ``AQSpace.h`` (the simulation world) — and because
``AQSpace.h`` transitively includes the body, collision, contact, joint, query,
and debug headers, those two cover almost every consumer.

.. code-block:: cpp

    #include <aqua/AQContext.h>     // AQContext, AQExecPath — start here
    #include <aqua/AQSpace.h>       // AQSpace + (transitively) everything below
    #include <aqua/AQRigidBody.h>   // AQRigidBody, AQBodyDesc, AQBodyType
    #include <aqua/AQCollision.h>   // AQShape, AQShapeHandle, filters, broadphase pairs
    #include <aqua/AQContact.h>     // contact manifolds, constraint rows, material combine
    #include <aqua/AQJoint.h>       // joint descriptors + handles, CCD / activation enums
    #include <aqua/AQQuery.h>       // raycast / trigger result types
    #include <aqua/AQDebug.h>       // drainable debug-line stream
    #include <aqua/AQMath.h>        // AQUA-owned rigid-body math (header-only)
    #include <aqua/AQIntegrator.h>  // the templated integrator (header-only)
    #include <aqua/AQBase.h>        // export / nodiscard macros only

The natural reading order for a first-time user is:

1. :ref:`aqua-api-context` — create the engine, choose CPU or GPU, drive time.
2. :ref:`aqua-api-space` — create a world, add bodies and shapes, read results.
3. :ref:`aqua-api-body` — the per-body descriptor and live handle.
4. Then the supporting value types: :ref:`aqua-api-collision`,
   :ref:`aqua-api-contact`, :ref:`aqua-api-joints`, :ref:`aqua-api-queries`,
   and :ref:`aqua-api-debug`.
5. The header-only :ref:`aqua-api-math` and :ref:`aqua-api-integrator` layers
   are lower-level; most consumers touch only the inertia builders in the math
   header.

A minimal end-to-end use looks like this:

.. code-block:: cpp

    #include <aqua/AQContext.h>
    #include <aqua/AQSpace.h>

    // CPU-only context (tests / headless). For the GPU path, pass a live
    // OmegaGTE engine + command queue to AQContext::Create instead.
    auto context = AQContext::CreateCPUOnly();
    auto space   = context->createSpace();

    OmegaGTE::FVec<3> down = OmegaGTE::FVec<3>::Create();
    down[1][0] = -9.81f;
    space->setGravity(down);

    // A ground plane and a falling box.
    AQShapeHandle groundShape = space->createPlaneShape(
        [] { auto n = OmegaGTE::FVec<3>::Create(); n[1][0] = 1.f; return n; }(), 0.f);
    AQShapeHandle boxShape = space->createBoxShape(
        [] { auto h = OmegaGTE::FVec<3>::Create(); h[0][0]=h[1][0]=h[2][0]=0.5f; return h; }());

    AQBodyDesc ground;
    ground.type  = AQBodyType::Static;
    ground.shape = groundShape;
    space->addBody(ground);

    AQBodyDesc boxDesc;
    boxDesc.type          = AQBodyType::Dynamic;
    boxDesc.mass          = 1.f;
    boxDesc.shape         = boxShape;
    boxDesc.position[1][0] = 5.f;
    auto box = space->addBody(boxDesc);

    // Advance real frame time; the context runs fixed sub-steps internally.
    for (int frame = 0; frame < 300; ++frame)
        context->advance(1.f / 60.f);

    OmegaGTE::FVec<3> rest = box->position();   // box has settled on the plane

.. note::

    ``OmegaGTE::Matrix`` (and therefore ``FVec<3>``) has a private default
    constructor and no ``{x, y, z}`` component constructor — you build one with
    the ``Create()`` factory and assign components (``v[i][0] = …``), or use the
    ``AQvec3(x, y, z)`` helper from :ref:`aqua-api-math`. That is why the
    examples above spell vectors the long way.

----

.. _aqua-api-context:

AQContext — the engine entry point
==================================

``AQContext`` (``AQContext.h``) is the central object and the owner of all
physics state. It holds the OmegaGTE graphics engine + command queue that GPU
physics is dispatched through, creates and retains the simulation spaces, and
keeps simulation time with a fixed-timestep accumulator. Every context is
reference-counted and handed back as a ``SharedHandle<AQContext>``.

Creation
--------

.. code-block:: cpp

    // Production factory. The graphics engine is REQUIRED — kREATE is GPU-first
    // and always has one. A null engine is a contract violation: it is reported
    // loudly and degrades to a CPU-only context.
    static SharedHandle<AQContext> Create(
        SharedHandle<OmegaGTE::OmegaGraphicsEngine> engine,
        SharedHandle<OmegaGTE::GECommandQueue>      commandQueue);

    // Engine-less factory: a context that always runs the CPU reference path.
    // Keeps pure-CPU unit tests and headless tools free of GTE device init.
    static SharedHandle<AQContext> CreateCPUOnly();

Execution path
--------------

AQUA runs its hot stages — integration, broadphase, contact solve — on either
the GPU (OmegaSL compute kernels dispatched through the held command queue) or
an equivalent CPU reference path. Which one is used is *data*, selected from
device capability and kernel availability, never a compile-time ``#ifdef``.

.. code-block:: cpp

    enum class AQExecPath : std::uint8_t {
        Auto,  // GPU if a usable compute backend exists, else CPU  (default)
        CPU,   // force the CPU reference path (the parity oracle / fallback)
        GPU,   // prefer the GPU path; falls back to CPU (loud) if unusable
    };

    void       setExecutionPath(AQExecPath path);
    AQExecPath executionPath() const;   // the RESOLVED path — never Auto

``setExecutionPath`` records what the caller *asked* for; ``executionPath()``
reports what the next ``advance`` will actually run. Lockstep consumers (kREATE
netcode) pin a single path because CPU↔GPU agreement is equivalent-within-
tolerance, not bitwise.

Spaces and time
---------------

.. code-block:: cpp

    SharedHandle<AQSpace> createSpace();   // owned and stepped by this context

    void   advance(float realDt);          // feed real elapsed frame time
    float  fixedTimestep() const;          // default 1/120 s
    void   setFixedTimestep(float dt);     // non-positive values ignored
    double elapsed() const;                // total simulated time, seconds

Timekeeping uses a fixed-timestep accumulator: ``advance(realDt)`` runs as many
fixed sub-steps as fit into the banked time and carries the remainder forward.
This decouples the simulation rate from the caller's frame rate and keeps
stepping deterministic. Individual spaces are never stepped directly — all
timekeeping lives in one place.

.. warning::

    **Sub-step size is a correctness knob for fast rotation, not just
    performance.** The integrator conserves angular momentum and energy only to
    first order in the sub-step, so drift is O(dt) and accumulates: a fast
    spinner is visibly worse at 1/120 s than at 1/2000 s. Set
    ``setFixedTimestep`` small enough for the angular rates in your scene.

----

.. _aqua-api-space:

AQSpace — the simulation world
==============================

``AQSpace`` (``AQSpace.h``) holds the bodies a context advances each sub-step
and owns the per-step debug stream, the shape table, and the contact / joint /
query machinery. Spaces are created by ``AQContext::createSpace`` and owned by
that context. The simulation backend is hidden behind a pimpl.

Gravity and bodies
------------------

.. code-block:: cpp

    void              setGravity(const OmegaGTE::FVec<3> &g);
    OmegaGTE::FVec<3> gravity() const;

    SharedHandle<AQRigidBody> addBody(const AQBodyDesc &desc);
    bool                      removeBody(const SharedHandle<AQRigidBody> &body);
    std::size_t               bodyCount() const;

``removeBody`` returns ``false`` if the body was not in this space.

Shape factories
---------------

Shapes are owned and instanced by the space and referenced by handle from body
descriptors (see :ref:`aqua-api-collision`). Each factory returns an
``AQShapeHandle``; a malformed shape (zero radius, empty hull) yields an invalid
handle (generation 0).

.. code-block:: cpp

    AQShapeHandle createSphereShape(float radius);
    AQShapeHandle createBoxShape(const OmegaGTE::FVec<3> &halfExtents);
    AQShapeHandle createCapsuleShape(float radius, float halfHeight);   // axis = local +Y
    AQShapeHandle createPlaneShape(const OmegaGTE::FVec<3> &normal, float offset);
    AQShapeHandle createConvexHullShape(const OmegaGTE::FVec<3> *pts, std::size_t n);

Reading the simulation
----------------------

These accessors expose the internal state produced by the most recent
``advance``. The returned containers are value-type copies carrying body
*indices* (a body's stable slot in the space's SoA arrays), not pointers, so
they are safe to hold across ``advance`` boundaries.

.. code-block:: cpp

    // Broadphase candidate pairs (a < b), refreshed once per advance.
    OmegaCommon::Vector<AQBroadphasePair>   candidatePairs() const;

    // Narrowphase manifolds from the most recent sub-step.
    OmegaCommon::Vector<AQContactManifold>  contactManifolds() const;

    // Live joints, refreshed per advance.
    OmegaCommon::Vector<AQJointDesc>        joints() const;

Material combine and the solver
-------------------------------

.. code-block:: cpp

    // Per-space restitution / friction combine rules. Default: Average for both.
    void              setMaterialCombine(AQMaterialCombine restCombine,
                                         AQMaterialCombine fricCombine);
    AQMaterialCombine restitutionCombine() const;
    AQMaterialCombine frictionCombine() const;

    // Sequential-impulse PGS sweep counts. Defaults: 8 velocity, 4 position.
    // positionIters == 0 disables the split-impulse position-correction pass.
    void setSolverIterations(int velocityIters, int positionIters);
    int  velocityIterations() const;
    int  positionIterations() const;

Joints
------

Each factory returns an ``AQJointHandle`` into the space's joint table. A joint
between two infinite-mass bodies (static/static, static/kinematic, …) is a
no-op and returns an invalid handle. **Anchors are in each body's local frame;
axes are body-A local.**

.. code-block:: cpp

    AQJointHandle createDistanceJoint(const SharedHandle<AQRigidBody> &a,
                                      const SharedHandle<AQRigidBody> &b,
                                      const OmegaGTE::FVec<3> &anchorALocal,
                                      const OmegaGTE::FVec<3> &anchorBLocal,
                                      float length);
    AQJointHandle createBallSocketJoint(const SharedHandle<AQRigidBody> &a,
                                        const SharedHandle<AQRigidBody> &b,
                                        const OmegaGTE::FVec<3> &anchorALocal,
                                        const OmegaGTE::FVec<3> &anchorBLocal);
    AQJointHandle createHingeJoint(const SharedHandle<AQRigidBody> &a,
                                   const SharedHandle<AQRigidBody> &b,
                                   const OmegaGTE::FVec<3> &anchorALocal,
                                   const OmegaGTE::FVec<3> &anchorBLocal,
                                   const OmegaGTE::FVec<3> &axisALocal,
                                   const AQJointAxisLimit &limit = AQJointAxisLimit{});
    AQJointHandle createSliderJoint(const SharedHandle<AQRigidBody> &a,
                                    const SharedHandle<AQRigidBody> &b,
                                    const OmegaGTE::FVec<3> &anchorALocal,
                                    const OmegaGTE::FVec<3> &anchorBLocal,
                                    const OmegaGTE::FVec<3> &axisALocal,
                                    const AQJointAxisLimit &limit = AQJointAxisLimit{});
    AQJointHandle createFixedJoint(const SharedHandle<AQRigidBody> &a,
                                   const SharedHandle<AQRigidBody> &b);

    void setJointSoftness(AQJointHandle h, AQJointSoftness s);
    bool destroyJoint(AQJointHandle h);

    // World-frame linear impulse this joint applied in the most recent sub-step
    // (zero for an invalid handle). Divide by the sub-step dt for reaction force.
    OmegaGTE::FVec<3> jointImpulse(AQJointHandle h) const;

Queries
-------

Queries walk the same per-step broadphase grid the simulation builds. They are
valid between ``advance`` calls (stale during one). ``hits`` is cleared then
appended, and results are sorted by ``(fraction, bodyIndex)``.

.. code-block:: cpp

    void raycast(const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FVec<3> &direction,
                 float maxT,
                 const AQQueryFilter &filter,
                 OmegaCommon::Vector<AQRaycastHit> &hits) const;

    void shapecast(AQShapeHandle shape,
                   const OmegaGTE::FVec<3> &origin,
                   const OmegaGTE::FQuaternion &orientation,
                   const OmegaGTE::FVec<3> &direction,
                   float maxT,
                   const AQQueryFilter &filter,
                   OmegaCommon::Vector<AQRaycastHit> &hits) const;

    void overlap(AQShapeHandle shape,
                 const OmegaGTE::FVec<3> &origin,
                 const OmegaGTE::FQuaternion &orientation,
                 const AQQueryFilter &filter,
                 bool exactShapes,
                 OmegaCommon::Vector<std::uint32_t> &bodies) const;

Triggers and sleep tuning
-------------------------

.. code-block:: cpp

    // Drains the per-advance trigger-event queue (ordered by (a, b) ascending).
    OmegaCommon::Vector<AQTriggerEvent> triggerEvents();

    // Space-wide sleep thresholds. Defaults: 0.01 m/s linear, 0.01 rad/s
    // angular, 60 idle sub-steps. Non-zero per-body overrides win. Negatives
    // are clamped to 0.
    void setSleepThresholds(float linearVel, float angularVel,
                            std::uint32_t idleSubsteps);

Debug stream
------------

The space emits structured ``AQDebugLine`` records each step according to the
current flag set; the consumer drains the buffer and the space clears it. It is
off by default (``AQDebugNone``) and zero-cost when unused. See
:ref:`aqua-api-debug`.

.. code-block:: cpp

    void          setDebugFlags(std::uint32_t flags);   // OR of AQDebugFlags
    std::uint32_t debugFlags() const;
    OmegaCommon::Vector<AQDebugLine> drainDebugLines();  // moves out + clears

----

.. _aqua-api-body:

AQRigidBody and AQBodyDesc
==========================

``AQBodyType`` (``AQRigidBody.h``) selects how a body participates:

.. code-block:: cpp

    enum class AQBodyType {
        Static,     // never moves; infinite mass (ground, level geometry)
        Dynamic,    // integrated each step; affected by gravity and forces
        Kinematic,  // user-driven pose, infinite mass, pushes dynamics one-way
    };

AQBodyDesc
----------

The value type you fill in and pass to ``AQSpace::addBody``. Every field has a
default, so the minimum is a type and (for dynamics) a mass and shape.

.. code-block:: cpp

    struct AQBodyDesc {
        AQBodyType type = AQBodyType::Dynamic;

        // pose & motion
        OmegaGTE::FVec<3>     position;
        OmegaGTE::FQuaternion orientation;
        OmegaGTE::FVec<3>     linearVelocity;
        OmegaGTE::FVec<3>     angularVelocity;   // world frame

        // mass properties
        float mass = 1.f;                              // ignored for Static bodies
        OmegaGTE::FVec<3> inertiaPrincipalMoments;     // diagonal, body frame; zero ⇒ derive from shape
        AQMat3F           inertiaTensor;               // optional full 3x3; non-zero ⇒ diagonalized
        OmegaGTE::FVec<3> centerOfMass;                // reserved (Phase 2 geometry)

        // robustness controls
        float linearDamping   = 0.f;   // v ← v / (1 + c·dt)
        float angularDamping  = 0.f;   // same shape on body-frame angular velocity
        float gravityScale    = 1.f;   // per-body multiplier on space gravity
        float maxAngularSpeed = 0.f;   // 0 ⇒ unlimited; opt-in safety clamp

        // material (see AQSpace material combine)
        float restitution = 0.f;
        float friction    = 0.5f;

        // collision
        AQShapeHandle     shape;    // invalid (default) ⇒ no shape, broadphase-invisible
        AQCollisionFilter filter;   // default: layer 1, mask all

        // triggers, CCD, sleep
        bool      isTrigger = false;             // emits events instead of collision response
        AQCCDMode ccdMode   = AQCCDMode::Off;
        float     sleepLinearVelocity  = 0.f;    // 0 ⇒ use space default
        float     sleepAngularVelocity = 0.f;
    };

Inertia. If ``inertiaTensor`` has any non-zero entry, ``addBody`` diagonalizes
it and folds the principal-axis rotation into the body orientation
(PhysX/Chaos-style). Otherwise ``inertiaPrincipalMoments`` is used as-is; if
*that* is also zero and the body is dynamic with a valid shape, the moments are
derived from the shape. Until you have a shape, fill the moments with an
:ref:`AQMath inertia builder <aqua-api-math>` such as ``AQinertiaSolidBox``.

AQRigidBody
-----------

The live handle, returned by ``addBody`` and valid until removed or the space is
destroyed. All vectors are world-frame unless noted.

.. code-block:: cpp

    // linear state
    OmegaGTE::FVec<3> position() const;   void setPosition(const OmegaGTE::FVec<3> &p);
    OmegaGTE::FVec<3> velocity() const;   void setVelocity(const OmegaGTE::FVec<3> &v);

    // angular state
    OmegaGTE::FQuaternion orientation() const;  void setOrientation(const OmegaGTE::FQuaternion &q);
    OmegaGTE::FVec<3>     angularVelocity() const;   // world frame
    void                  setAngularVelocity(const OmegaGTE::FVec<3> &w);

    // mass properties
    float                 mass() const;   // 0 ⇒ static
    OmegaGTE::FVec<3>      inertiaPrincipalMoments() const;
    OmegaGTE::FMatrix<3,3> worldInverseInertia() const;   // R · diag(invMoments) · Rᵀ

    // conserved quantities (assert against the engine's own numbers)
    OmegaGTE::FVec<3> linearMomentum()  const;   // m · v
    OmegaGTE::FVec<3> angularMomentum() const;   // R · Ib · ω_b
    float             kineticEnergy()   const;   // ½m‖v‖² + ½ω_b·Ib·ω_b

    // robustness controls
    void setLinearDamping(float c);    float linearDamping()   const;
    void setAngularDamping(float c);   float angularDamping()  const;
    void setGravityScale(float s);     float gravityScale()    const;
    void setMaxAngularSpeed(float s);  float maxAngularSpeed() const;   // 0 ⇒ unlimited

    // material coefficients
    float restitution() const;   void setRestitution(float r);   // [0, 1]
    float friction()    const;   void setFriction(float mu);     // ≥ 0

    // collision geometry & filter
    AQShapeHandle shape() const;   void setShape(const AQShapeHandle &s);
    OmegaGTE::FVec<3> aabbMin() const;   // world fattened AABB; zero if no shape
    OmegaGTE::FVec<3> aabbMax() const;
    AQCollisionFilter collisionFilter() const;   void setCollisionFilter(const AQCollisionFilter &f);

    // activation / sleep
    AQActivationState activation() const;
    void wakeUp();       // force Active (wakes the island); no-op on kinematic
    void putToSleep();   // force Sleeping (clears velocities); no-op on static/kinematic

    // triggers / CCD
    bool      isTrigger() const;
    AQCCDMode ccdMode()   const;   void setCCDMode(AQCCDMode m);

    // kinematic control — only meaningful for Kinematic bodies
    void setKinematicTarget(const OmegaGTE::FVec<3> &p, const OmegaGTE::FQuaternion &q);

    // force / torque / impulse (world space)
    void applyForce(const OmegaGTE::FVec<3> &force);
    void applyForceAtPoint(const OmegaGTE::FVec<3> &force, const OmegaGTE::FVec<3> &worldPoint);
    void applyTorque(const OmegaGTE::FVec<3> &torque);
    void applyImpulse(const OmegaGTE::FVec<3> &impulse);            // instantaneous Δp
    void applyImpulseAtPoint(const OmegaGTE::FVec<3> &impulse, const OmegaGTE::FVec<3> &worldPoint);
    void applyAngularImpulse(const OmegaGTE::FVec<3> &angularImpulse);

    AQBodyType type() const;

Forces and torques accumulate in world space and are consumed at the start of
each sub-step; impulses apply instantaneously to velocity. ``setShape`` does
*not* re-derive inertia — that only happens in ``addBody`` — so to recompute
moments, zero them and re-add the body.

----

.. _aqua-api-collision:

Collision types (``AQCollision.h``)
===================================

Every type here is trivially-copyable / standard-layout so it uploads to a GPU
buffer with no repacking.

.. code-block:: cpp

    enum class AQShapeType : std::uint32_t { Sphere, Box, Capsule, Plane, ConvexHull };

    // Tagged POD union. Primitive params are raw floats (not FVec) so the type
    // stays trivially copyable. Local pose is raw position + quaternion floats.
    struct AQShape {
        AQShapeType type;
        float lpx, lpy, lpz;             // local position (offset from body COM)
        float lqx, lqy, lqz, lqw;        // local orientation
        union {
            struct { float radius; }                           sphere;
            struct { float hx, hy, hz; }                       box;      // half-extents
            struct { float radius, halfHeight; }               capsule;  // axis = local +Y
            struct { float nx, ny, nz, offset; }               plane;    // n·x = offset half-space
            struct { std::uint32_t firstVertex, vertexCount; } hull;
        };
    };

    // Opaque handle into a space's shape table. Generation 0 ⇒ "no shape".
    struct AQShapeHandle {
        std::uint32_t index      = 0;
        std::uint32_t generation = 0;
        bool valid() const;   // generation != 0
    };

    // 32-bit layer + 32-bit mask. Two bodies pair iff
    // (a.layer & b.mask) && (b.layer & a.mask). Default: layer 1, mask all.
    struct AQCollisionFilter {
        std::uint32_t layer = 1u;
        std::uint32_t mask  = ~0u;
    };
    bool AQfilterAccepts(const AQCollisionFilter &a, const AQCollisionFilter &b);

    // A broadphase candidate pair; invariant a < b. Ordered / comparable.
    struct AQBroadphasePair { std::uint32_t a, b; };
    bool operator<(const AQBroadphasePair &, const AQBroadphasePair &);
    bool operator==(const AQBroadphasePair &, const AQBroadphasePair &);

Most consumers create shapes through the ``AQSpace::create*Shape`` factories
rather than filling an ``AQShape`` by hand. The free functions below operate on
a shape worn by a body at a given transform; ``hullVerts`` / ``hullVertCount``
describe the space's vertex pool and are only read for ``ConvexHull`` shapes.

.. code-block:: cpp

    // World-space axis-aligned bound of a shape at bodyXform.
    AQAABB<float> AQshapeAABB(const AQShape &shape,
                             const AQTransform<float> &bodyXform,
                             const OmegaGTE::FVec<3> *hullVerts = nullptr,
                             std::size_t hullVertCount = 0);

    // GJK support point: surface point maximizing dot(p, dirWorld).
    OmegaGTE::FVec<3> AQshapeSupport(const AQShape &shape,
                                    const OmegaGTE::FVec<3> &dirWorld,
                                    const AQTransform<float> &bodyXform,
                                    const OmegaGTE::FVec<3> *hullVerts = nullptr,
                                    std::size_t hullVertCount = 0);

    // Diagonal principal moments for a shape of the given mass.
    OmegaGTE::FVec<3> AQshapeInertiaMoments(const AQShape &shape, float mass,
                                           const OmegaGTE::FVec<3> *hullVerts = nullptr,
                                           std::size_t hullVertCount = 0);

----

.. _aqua-api-contact:

Contact types (``AQContact.h``)
===============================

The read-only manifold surface exposed by ``AQSpace::contactManifolds`` and the
constraint-row PODs the PGS solver consumes. All are standard-layout for GPU
upload.

.. code-block:: cpp

    // One contact point; up to 4 share a manifold and its normal.
    struct AQContactPoint {
        OmegaGTE::FVec<3> positionWorld;
        float             depth        = 0.f;
        std::uint32_t     featureKey   = 0;   // identifies which shape features met
        float             accumNormal      = 0.f;   // warm-start carriers
        float             accumFriction[2] = {0.f, 0.f};
    };

    // Manifold between bodies a and b (a < b). The normal points FROM A TO B.
    struct AQContactManifold {
        std::uint32_t     a = 0, b = 0;
        OmegaGTE::FVec<3> normalWorld;
        std::uint32_t     pointCount = 0;
        AQContactPoint    points[4];
        float             restitutionCombined = 0.f;
        float             frictionCombined    = 0.f;
    };

    // Which kind of row a constraint carries.
    enum class AQConstraintKind : std::uint32_t {
        ContactNormal,    // λ ≥ 0
        ContactFriction,  // |λ| ≤ μ·λ_n of its peer normal row
        JointAxis,        // bilateral (two-sided); λ unbounded
        JointLimit,       // one-sided at a limit; λ ≥ 0
        JointMotor,       // target velocity; |λ| ≤ motorMaxImpulse
    };

    // One row consumed by the PGS sweep. Self-contained: everything the inner
    // iteration needs is precomputed once per sub-step.
    struct AQConstraintRow {
        AQConstraintKind  kind = AQConstraintKind::ContactNormal;
        std::uint32_t     bodyA = 0, bodyB = 0;
        OmegaGTE::FVec<3> contactPoint, rA, rB, direction;
        float             effectiveMass = 0.f;   // 1 / Keff
        float             bias          = 0.f;    // restitution bias on normal
        float             accumImpulse  = 0.f;    // warm-started across the sweep
        std::uint32_t     peerRow       = 0;      // friction → its normal row index
        float             frictionCoeff = 0.f;    // μ on friction; motor max-impulse on JointMotor
        float             compliance    = 0.f;    // soft-constraint (Catto); 0 ⇒ hard
        bool              isAngular     = false;  // true ⇒ pure-torque row
    };

    // Per-space material-combine rule. Average is the default.
    enum class AQMaterialCombine : std::uint8_t { Average, Min, Max, Multiply };

Most gameplay code reads ``AQContactManifold`` (for hit reactions, sound, VFX)
and never touches ``AQConstraintRow`` — the row layout is exposed mainly so
tooling and the GPU solver share one schema.

----

.. _aqua-api-joints:

Joint types (``AQJoint.h``)
===========================

Value types for the joint factories on ``AQSpace``, plus the per-body CCD and
activation enums (also consumed by ``AQRigidBody``).

.. code-block:: cpp

    enum class AQJointType : std::uint32_t {
        Distance,    // 1 row  — hold ‖anchorA − anchorB‖ at a target length
        BallSocket,  // 3 rows — coincident anchors (spherical)
        Hinge,       // 5 rows — BallSocket + 2 axis rows (revolute)
        Slider,      // 5 rows — 2 perpendicular ball-socket + 3 angular (prismatic)
        Fixed,       // 6 rows — 3 ball-socket + 3 angular
    };

    enum class AQCCDMode : std::uint8_t {
        Off,          // discrete (default); can tunnel through thin shapes
        Speculative,  // fatten broadphase AABB by ‖v‖·dt; cheap
        Continuous,   // conservative-advancement TOI; exact, expensive
    };

    enum class AQActivationState : std::uint8_t { Active, Sleeping, Kinematic };

    // Soft-constraint parameters (Catto 2011). Defaults (0, 0) ⇒ a HARD joint.
    struct AQJointSoftness {
        float frequency = 0.f;   // rad/s
        float damping   = 0.f;   // 1.0 ⇒ critically damped
    };

    // Optional per-axis limit + motor (Hinge: angular; Slider: linear).
    struct AQJointAxisLimit {
        bool  enabled             = false;   // when false the axis is free
        float min                 = 0.f;     // radians (Hinge) or metres (Slider)
        float max                 = 0.f;
        bool  motorEnabled        = false;
        float motorTargetVelocity = 0.f;     // rad/s (Hinge) or m/s (Slider)
        float motorMaxImpulse     = 0.f;     // per-sub-step clamp
    };

    // Full joint descriptor (returned by AQSpace::joints()). Anchors are LOCAL.
    struct AQJointDesc {
        AQJointType       type = AQJointType::BallSocket;
        std::uint32_t     bodyA = 0, bodyB = 0;
        OmegaGTE::FVec<3> anchorA, anchorB;
        OmegaGTE::FVec<3> axisLocalA;         // Hinge/Slider axis (body-A local)
        float             distanceTarget = 0.f;
        AQJointSoftness   softness;           // hard by default
        AQJointAxisLimit  limit;              // Hinge: angular; Slider: linear
    };

    // Opaque joint handle. Generation 0 ⇒ "no joint".
    struct AQJointHandle {
        std::uint32_t index = 0, generation = 0;
        bool valid() const;
    };

----

.. _aqua-api-queries:

Query and trigger types (``AQQuery.h``)
=======================================

.. code-block:: cpp

    // One raycast / shapecast hit. Results come back fraction-sorted.
    struct AQRaycastHit {
        std::uint32_t     bodyIndex = 0;
        float             fraction  = 0.f;   // t in [0, maxT]
        OmegaGTE::FVec<3> position;          // world; witness point for shapecast
        OmegaGTE::FVec<3> normal;            // world
    };

    // Same layer/mask shape as AQCollisionFilter, paired via AQfilterAccepts.
    struct AQQueryFilter {
        std::uint32_t layer = 1u;
        std::uint32_t mask  = ~0u;
    };

    enum class AQTriggerEventKind : std::uint8_t { Enter, Stay, Exit };

    // Drained via AQSpace::triggerEvents(); (a, b) satisfy a < b. At least one
    // of a / b is a body whose descriptor set isTrigger = true.
    struct AQTriggerEvent {
        std::uint32_t      a = 0, b = 0;
        AQTriggerEventKind kind = AQTriggerEventKind::Enter;
    };

----

.. _aqua-api-debug:

Debug stream (``AQDebug.h``)
============================

A neutral, drainable debug-primitive surface — AQUA owns no renderer, it emits
line segments the consumer replays. Header-only.

.. code-block:: cpp

    // One line segment from world a to world b, RGBA in [0, 1].
    struct AQDebugLine {
        OmegaGTE::FVec<3> a, b;
        float             rgba[4] = {1.f, 1.f, 1.f, 1.f};
    };

    // What a space emits each step. Bitfield — combine with bitwise OR.
    enum AQDebugFlags : std::uint32_t {
        AQDebugNone            = 0,
        AQDebugBodyAxes        = 1U << 0,   // RGB principal axes at the COM
        AQDebugVelocity        = 1U << 1,   // linear velocity vector
        AQDebugAngularVel      = 1U << 2,   // angular velocity vector (world)
        AQDebugMomentum        = 1U << 3,   // world angular-momentum L vector
        AQDebugAABB            = 1U << 4,   // per-body fattened world AABB
        AQDebugBroadphasePair  = 1U << 5,   // one line per candidate, COM(a)→COM(b)
        AQDebugBroadphaseGuard = 1U << 6,   // red line when candidate/brute(n²) > 0.5
        AQDebugContactPoint    = 1U << 7,   // cross at each contact point
        AQDebugContactNormal   = 1U << 8,   // contact normal scaled by depth
        AQDebugContactImpulse  = 1U << 9,   // accumulated normal-impulse (post-solve)
        AQDebugJointAnchor     = 1U << 10,  // cross at each joint's two anchors
        AQDebugJointAxis       = 1U << 11,  // hinge/slider axis at anchor A
        AQDebugIsland          = 1U << 12,  // per-island spokes, colored by sleep state
        AQDebugRaycastHit      = 1U << 13,  // last query: origin→hit + normal tick
        AQDebugSleepingBody    = 1U << 14,  // greyed marker at each sleeping body
        AQDebugCCDSweep        = 1U << 15,  // swept-extension segment per opted-in body
    };

Usage: ``space->setDebugFlags(AQDebugAABB | AQDebugContactNormal);`` then, after
each ``advance``, ``auto lines = space->drainDebugLines();`` to pull and clear
the buffer.

----

.. _aqua-api-math:

Rigid-body math (``AQMath.h``)
==============================

AQUA-owned, header-only, and templated on the scalar ``Ty`` so the same code
runs at ``float`` (production) and ``double`` (a reference oracle in tests).
This layer fills the gaps GTE's ``Matrix``/``Quaternion`` leave for physics:
quaternion exp/log, orientation integration, free-vector rotation, skew
matrices, inertia builders, transforms, and AABBs.

Types and construction
-----------------------

.. code-block:: cpp

    template<class Ty> using AQVec3 = OmegaGTE::Matrix<Ty,3,1>;   // a column vector
    template<class Ty> using AQMat3 = OmegaGTE::Matrix<Ty,3,3>;
    using AQMat3F = AQMat3<float>;   // the public mass-property tensor form

    // Ergonomic constructor — GTE's Matrix has a private default ctor.
    template<class Ty> AQVec3<Ty> AQvec3(Ty x, Ty y, Ty z);

Quaternion / rotation helpers
-----------------------------

.. code-block:: cpp

    // Cross-product (skew-symmetric) matrix: AQskew(a) * b == cross(a, b).
    template<class Ty> AQMat3<Ty> AQskew(const AQVec3<Ty> &a);

    // Exponential map: quaternion from a half-angle rotation vector (½·φ).
    // Small-angle stable (sinc series), unlike raw Quaternion::fromAxisAngle.
    template<class Ty> OmegaGTE::Quaternion<Ty> AQquatExp(const AQVec3<Ty> &halfAngle);

    // Inverse of AQquatExp: the ½·φ vector v with AQquatExp(v) ≈ q.
    template<class Ty> AQVec3<Ty> AQquatLog(const OmegaGTE::Quaternion<Ty> &q);

    // Integrate orientation by BODY-frame angular velocity over dt.
    template<class Ty> OmegaGTE::Quaternion<Ty>
    AQintegrate(const OmegaGTE::Quaternion<Ty> &q, const AQVec3<Ty> &omegaBody, Ty dt);

    // Rotate a FREE vector by q (GTE only ships rotatePoint for GPoint3D).
    template<class Ty> AQVec3<Ty> AQrotate(const OmegaGTE::Quaternion<Ty> &q, const AQVec3<Ty> &v);

Inertia builders
----------------

Diagonal principal moments for common shapes — use these to fill
``AQBodyDesc::inertiaPrincipalMoments`` when a body has no collision shape yet.

.. code-block:: cpp

    template<class Ty> AQVec3<Ty> AQinertiaSolidBox(Ty mass, Ty hx, Ty hy, Ty hz);
    template<class Ty> AQVec3<Ty> AQinertiaSolidSphere(Ty mass, Ty r);
    template<class Ty> AQVec3<Ty> AQinertiaCapsule(Ty mass, Ty r, Ty h);   // axis = local Y

    // Arbitrary symmetric tensor → principal moments + principal-axis rotation
    // (Jacobi eigendecomposition). outAxis rotates body-local into principal frame.
    template<class Ty> void AQdiagonalizeInertia(const AQMat3<Ty> &I,
                                                 AQVec3<Ty> &outMoments,
                                                 OmegaGTE::Quaternion<Ty> &outAxis);

    // World-space inverse inertia: R · diag(invMomentsBody) · Rᵀ.
    template<class Ty> AQMat3<Ty> AQworldInvInertia(const OmegaGTE::Quaternion<Ty> &q,
                                                    const AQVec3<Ty> &invMomentsBody);

Transforms and bounds
----------------------

.. code-block:: cpp

    // Rigid transform (position + orientation).
    template<class Ty> struct AQTransform {
        AQVec3<Ty>               p;   // translation
        OmegaGTE::Quaternion<Ty> q;   // rotation (identity by default)

        OmegaGTE::Matrix<Ty,4,4> toMatrix() const;
        AQTransform inverse() const;
        AQTransform operator*(const AQTransform &child) const;   // this ∘ child
        AQVec3<Ty>  transformPoint(const AQVec3<Ty> &v) const;
        AQVec3<Ty>  transformVector(const AQVec3<Ty> &v) const;
    };

    // Axis-aligned bounding box. Empty-AABB convention: min=+inf, max=-inf.
    template<class Ty> struct AQAABB {
        AQVec3<Ty> min, max;

        static AQAABB empty();
        static AQAABB fromMinMax(const AQVec3<Ty> &lo, const AQVec3<Ty> &hi);
        static AQAABB fromCenterHalfExtents(const AQVec3<Ty> &c, const AQVec3<Ty> &h);

        bool       overlaps(const AQAABB &o) const;
        AQAABB     merged(const AQAABB &o) const;
        AQAABB     fattened(Ty margin) const;
        AQVec3<Ty> center() const;
        AQVec3<Ty> extents() const;       // full width along each axis
        Ty         surfaceArea() const;
        bool       contains(const AQVec3<Ty> &p) const;
    };
    using FAABB = AQAABB<float>;

    // World AABB of an oriented box (|R|·h bound; always contains all 8 corners).
    template<class Ty> AQAABB<Ty> AQaabbOfOrientedBox(const AQVec3<Ty> &centerW,
                                                      const AQVec3<Ty> &halfExtents,
                                                      const OmegaGTE::Quaternion<Ty> &q);

----

.. _aqua-api-integrator:

Integrator (``AQIntegrator.h``)
===============================

The Phase 1 integrator, written once over the scalar ``Ty``. Header-only,
depends only on ``AQMath.h``, and carries no link-time dependency — a pure-CPU
test can exercise it without building any GPU backend. Most consumers never call
this directly (``AQContext::advance`` drives it internally); it is public so the
CPU reference path and the parity harness share one implementation.

.. code-block:: cpp

    // Per-body dynamics state. Angular velocity is stored in the BODY frame.
    template<class Ty> struct AQBodyState {
        AQVec3<Ty>               position, velocity;
        OmegaGTE::Quaternion<Ty> orientation;
        AQVec3<Ty>               angularVelBody;
        Ty                       invMass = Ty(1);   // 0 ⇒ static
        AQVec3<Ty>               invInertiaBody;     // 1 / principal moments
        AQVec3<Ty>               forceAccum, torqueAccum;   // world frame
        Ty                       linearDamping = Ty(0), angularDamping = Ty(0);
        Ty                       gravityScale = Ty(1), maxAngularSpeed = Ty(0);
        AQVec3<Ty>               comOffset;
        AQActivationState        activation = AQActivationState::Active;
    };

    // Adaptive implicit-gyroscopic iteration policy.
    template<class Ty> constexpr Ty kAQAdaptiveAngle();   // 1e-2
    constexpr int kAQAdaptiveCap = 4;

    // Velocity half-step: external forces + damping + implicit-gyroscopic Newton.
    template<class Ty> void AQStepBodyVelocity(AQBodyState<Ty> &b,
                                               const AQVec3<Ty> &gravity, Ty dt);

    // Position half-step: advance pose with the (solver-corrected) velocity,
    // clear accumulators, debug NaN guard.
    template<class Ty> void AQStepBodyPosition(AQBodyState<Ty> &b, Ty dt);

    // Both halves composed — one body, one fixed sub-step.
    template<class Ty> void AQStepBody(AQBodyState<Ty> &b,
                                       const AQVec3<Ty> &gravity, Ty dt);

The split into velocity / position half-steps is what lets the contact solver
run *between* them: forces predict a velocity, the PGS sweep modifies it via
contact impulses, then the position half-step advances pose with the corrected
velocity. Without contacts the two halves compose into ``AQStepBody``
byte-for-byte.

----

Preprocessor macros (``AQBase.h``)
==================================

``AQBase.h`` defines only two macros, applied to the exported types above:
``AQUA_EXPORT`` (the ``dllexport`` / ``dllimport`` visibility marker on Windows,
empty elsewhere) and ``AQUA_NODISCARD`` (``[[nodiscard]]`` under C++17). Neither
is something consumer code writes directly.
