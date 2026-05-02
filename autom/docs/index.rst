==================
Autom Build System
==================

AUTOM is our custom build system generator for Omega Graphics.
It is capable of generating to several build system including, Ninja, Visual Studio, Xcode and even Gradle.
We developed it not only to make devlopment faster 
(CMake generates a lot of artifacts especially for Xcode projects and Visual Studio solutions.)
But we also wanted to be able bind to Java code so we can build for Android without having as much overhead.


.. toctree::
    :numbered:
    :maxdepth: 2

    Syntax
    Interfaces
    AutomDeps
