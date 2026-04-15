=======================
Building Omega Graphics
=======================

This repo can be built using many different toolchains (GCC/LLVM/MVSC)
There are several tools that are within this repo that help bootstrap the build.

Clone
=====

The first thing is 

AUTOMDEPS
=========

Since this repo is quite large, 
We use are own dependency manager `autom` and `autom-deps` to help manage dependencies within the repo.

.. code-block:: bash

   ./autom/tools/autom-deps [--exec]

This will fetch and sync all the dependencies within the entire project.
If you already have our tools installed beforehand you can just use `autom-clone` and all deps will be taken care of.

.. code-block:: bash

   autom-clone https://github.com/omega-graphics/omega-graphics-project.git


Building the Code
=================

After configuration completes, build from the generated build directory:

.. code-block:: bash

   cmake --build build


Building the Docs
=================
