# Omega Graphics Project

Omega Graphics Project is a multi-module C++ codebase for graphics, UI, build tooling, and engine work. The root build currently wires together five primary modules: `AUTOM`, `OmegaCommon`, `OmegaGTE`, `OmegaWTK`, and `AQUA`.

## Modules

### [AUTOM](./autom/README.md)

`AUTOM` is the project's native-code build system generator. It includes the `autom` and `autom-install` command-line tools, a parser and execution engine for `AUTOM.build` files, standard library modules such as filesystem support, and generators for Ninja, Xcode, Visual Studio, and Gradle.

### [OmegaCommon](./common/README.md)

`OmegaCommon` is the shared runtime and utility layer used across the suite. It provides cross-platform support code for areas such as filesystem access, formatting, JSON/assets, networking, and multithreading, and it also houses utility tools including `omega-wrapgen`, `omega-ebin`, and `omegawtk-assetc`.

### [OmegaGTE](./gte/README.md)

`OmegaGTE` is the low-level graphics and tessellation engine. It builds platform-specific rendering backends for Direct3D 12, Metal, or Vulkan depending on the target platform, and it also includes the `omegaslc` shader compiler together with the `OmegaSL` shading language sources and tests.

### [OmegaWTK](./wtk/README.md)

`OmegaWTK` is a cross-platform native UI toolkit built on top of the graphics stack. Its source tree includes core utilities, composition/rendering systems, widgets, higher-level UI APIs, media support, and platform-specific native layers for macOS, Windows, Linux/GTK, iOS, and Android.

### [AQUA](./aqua/README.md)

`AQUA` is an early-stage game engine module that already builds as a separate library in this repo. It currently contains engine and editor scaffolding, including core and scene code, an editor entry point, and design/implementation planning documents for a larger engine intended to build on `OmegaGTE` and `OmegaWTK`.

Related subprojects inside the main modules include `OmegaSL` under `gte/omegasl` and `omega-wrapgen` under `common/wrapgen`.

## Want To Contribute?

Omega Graphics is a growing open source project and is always looking for new members and contributions. Fill out the quick form here:

https://forms.gle/3akUSgjMj3txz2jD8

And you will be given access to our libraries.
