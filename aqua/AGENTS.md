# Agents Code

## Constraints

In this repo all code is under the namespace: Aqua

AQUA is the physics / simulation engine — the simulation counterpart to OmegaGTE
(graphics). It is consumed by Omega kREATE (the 3D game engine).

- AQUA's public API (`include/aqua/`) exposes no graphics-engine types; the
  simulation backend stays hidden behind the public surface (pimpl), the same
  way OmegaGTE hides its backends.
