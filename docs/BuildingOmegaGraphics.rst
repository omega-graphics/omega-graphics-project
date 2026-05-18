=======================
Building Omega Graphics
=======================

Building Omega Graphics can take a while as it as complex project with many modules and dependencies.
This repo can be built using many different toolchains (GCC/LLVM/MVSC)
There are several tools that are within this repo that help bootstrap the build.

Requirements:
 - CMake (3.20+)
 - A C++ compiler that can handle C++17 (GCC 7.3+, Clang 5.0+, MSVC 2017+) We reommend using the latest stable version of your compiler.
 - Python 3.8+
 - Git

Platform Specific Users:

Windows:

- Visual Studio 2022 (minimum, 2026 recommended)

macOS:
- Xcode (latest recommended)


We recommend `ninja` as it one of the fastest build tool (Does many in parallel),
Visual Studio or Xcode are also good too.

  
Initial Setup
=================

Since this repo is quite large, 
We use are own dependency manager `autom` and `autom-deps` to help manage dependencies within the repo.
First clone the repo.
You can use either Git or if you have our tools already installed you can use `autom-clone`.
(This is a Git wrapper, that also execute `autom-deps` after the clone which fetch and sync the depdenecies of the entire repo.)

.. code-block:: bash

   git clone omega-graphics-project.git

After cloning, run `autom-deps`:

.. code-block:: bash

   ./autom/tools/autom-deps

Using `autom-clone`:

.. code-block:: bash

   autom-clone omega-graphics-project.git


This will fetch and sync all the dependencies within the entire project.

On Linux there are a few dependencies you will need to fetch via apt-get before building.
GTK 3.0+
Libnotify-dev
libtool (FOR BUILDING OmegaVA)
autoconf --
automake --
nasm--

Cross-Compiling for Mobile
==========================

Omega Graphics ships toolchain files in ``cmake/toolchains/`` that target
iOS and Android. They configure the right SDK/NDK, set the system name,
and pick sensible defaults you can override on the CMake command line.

When a cross-compile build needs developer tools that run on your
machine — ``omegaslc`` for shader compilation, ``autom`` and
``autom-install`` for the build system itself, ``omega-wrapgen``,
``omega-assetc``, ``omega-ebin``, and ``parse-test`` — CMake
automatically spins up a *host-tools superbuild* under
``<build>/_host_tools/`` and copies the resulting host binaries into
``<build>/bin/`` so the rest of the build can use them. You don't have
to do anything manual for this. If you see a ``_host_tools`` directory
or an ``omega-host-tools`` ExternalProject in your build output, that's
what's happening, and it's expected.


iOS
---

Point CMake at ``cmake/toolchains/LLVM-IOS.cmake``:

.. code-block:: bash

   cmake -S . -B build/ios -G Ninja \
       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/LLVM-IOS.cmake \
       -DCODE_SIGNATURE=<your Apple Developer Team ID>
   cmake --build build/ios

The toolchain understands a few options:

- ``IOS_PLATFORM`` — ``device`` (default) or ``simulator``. The device
  setting builds ``arm64`` against the ``iphoneos`` SDK. The simulator
  setting builds ``arm64;x86_64`` against ``iphonesimulator`` so the
  binary runs on both Apple Silicon and Intel simulators.
- ``IOS_MINIMUM_SUPPORT_VERSION`` — minimum iOS version, defaults to
  ``13.0``. The AQUA runtime uses ``UIScene`` APIs, so going lower is
  not recommended.

``CODE_SIGNATURE`` is required on Apple targets because the build signs
the produced ``.framework`` and ``.app`` bundles. Pass your Apple
Developer Team ID, or ``-`` for an ad-hoc local signature suitable for
test builds.


Android
-------

The Android toolchain needs the NDK. The repo's root ``AUTOMDEPS`` is
set up to download NDK r25c into ``./deps/android-ndk/`` when the
target is ``android``, so the easiest path is:

.. code-block:: bash

   ./autom/tools/autom-deps --target android

The fetch is host-platform-aware: running on macOS pulls the darwin
archive, Linux pulls linux, and Windows pulls windows. ``autom-deps``
skips the NDK entirely on any non-Android target, so this only adds
disk usage when you actually intend to cross-compile.

Then configure and build:

.. code-block:: bash

   cmake -S . -B build/android -G Ninja \
       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/LLVM-ANDROID.cmake
   cmake --build build/android

Override the toolchain's defaults if you need to:

- ``CMAKE_ANDROID_NDK`` — explicit path to an NDK install. The toolchain
  first checks the ``autom-deps`` fetch at ``<repo>/deps/android-ndk``,
  then falls back to the ``ANDROID_NDK``, ``ANDROID_NDK_HOME``, and
  ``ANDROID_NDK_ROOT`` environment variables.
- ``ANDROID_API_VERSION`` — the Android API level, defaulting to ``24``.
- ``CMAKE_ANDROID_ARCH_ABI`` — defaulting to ``arm64-v8a``.

Building for Android produces a shared library (``lib<NAME>.so``) for
each AQUA game. Wrapping that into an APK is a separate, follow-on
concern.



