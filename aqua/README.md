# AQUA Physics Engine

The physics / simulation engine for the Omega Graphics suite. Omega kREATE (the
3D game engine) consumes AQUA for collision and dynamics. Like OmegaGTE, AQUA
keeps its backend hidden behind its public API, so callers never depend on the
underlying simulation implementation.

**Status.** AQUA ships full rigid-body dynamics: linear and rotational motion,
collision shapes with a broadphase, a sequential-impulse contact solver,
joints, gameplay queries (raycast / shapecast / overlap), triggers, sleeping,
and continuous collision. The hot stages run either on the GPU through OmegaSL
compute kernels or on an equivalent CPU reference path, chosen from device
capability. The non-rigid pillars — particles, cloth and ropes, deformable
solids, and fluids — are the work ahead (see `aqua/.plans/`, Phases 6–10). The
public C++ API is documented in `docs/API.rst`; `kreate/.plans/Engine-Roadmap.md`
(Phase 8) covers how physics slots into the engine.
