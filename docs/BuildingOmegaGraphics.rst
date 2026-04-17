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

Building the Docs
=================

Omega Graphics uses two layers of documentation. 
Sphinx is used for our human readable guides to understanding each and every module in our project.
Our doxygen provides more detailed, direct API reference, as well as internal data structures.

The documentation is built using one script:

.. code-block:: bash

   ./utils/repo-docs 
   
This will output a folder entiled `doxygen` to the `build` folder.
There's also a project UML that can be generated using `repo-uml`.
(This is an experimental tool that we may end up phasing out and replacing with Doxygen generated UML)

