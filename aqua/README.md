# AQUA Physics Engine

The physics / simulation engine for the Omega Graphics suite. Omega kREATE (the
3D game engine) consumes AQUA for collision and dynamics. Like OmegaGTE, AQUA
keeps its backend hidden behind its public API, so callers never depend on the
underlying simulation implementation.

**Status — early scaffold.** Today AQUA provides an `AQSpace` that holds rigid
bodies and advances them with a placeholder gravity integrator (no collision).
Collision detection, constraints, queries, and the real solver are upcoming —
see `kreate/.plans/Engine-Roadmap.md` (Phase 8) for how physics slots into the
engine.
