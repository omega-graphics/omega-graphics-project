# Agents Code

## Constraints

This repo uses no namespace. All public objects carry the `AQ` prefix instead
(e.g. `AQSpace`, `AQRigidBody`, `AQBodyDesc`, `AQBodyType`). The math type `Vec3`
is the deliberate exception and stays unprefixed.

AQUA is the physics / simulation engine — the simulation counterpart to OmegaGTE
(graphics). It is consumed by Omega kREATE (the 3D game engine).

- AQUA's public API (`include/aqua/`) exposes no graphics-engine types; the
  simulation backend stays hidden behind the public surface (pimpl), the same
  way OmegaGTE hides its backends.
