===
API
===

This section documents OmegaGTE's public C++ API. Every symbol lives in the
``OmegaGTE`` namespace.

OmegaGTE has no umbrella ``OmegaGTE.h`` header — it was removed deliberately
to keep translation-unit compile times short. Each TU includes only the
specific headers it needs:

.. code-block:: cpp

    #include <omegaGTE/GTEDevice.h>      // GTE, GTEDevice, Init/Close, GTEInitOptions
    #include <omegaGTE/GE.h>             // OmegaGraphicsEngine, buffers, heaps, samplers, …
    #include <omegaGTE/GETexture.h>      // textures + descriptors
    #include <omegaGTE/GERenderTarget.h> // render targets + the command-buffer surface
    #include <omegaGTE/GECommandQueue.h> // command queues + pass descriptors
    #include <omegaGTE/GEPipeline.h>     // render / compute / blit / mesh pipeline descriptors
    #include <omegaGTE/GTEShader.h>      // shader libraries, buffer reader / writer
    #include <omegaGTE/GTEMath.h>        // Matrix, Quaternion, vector types
    #include <omegaGTE/GEPathBuilder.h>  // path builders (2D / 3D)
    #include <omegaGTE/TE.h>             // OmegaTriangulationEngine + params + result
    #include <omegaGTE/GEMesh.h>         // GEMesh + asset descriptors

The pages below are organised by concept. The natural order for a
first-time reader is roughly:

1. :doc:`api/Initialization` — start the engine, inspect device features.
2. :doc:`api/Shaders` — load the shaders that pipelines reference.
3. :doc:`api/Buffers` and :doc:`api/Textures` — the resource primitives
   you allocate, fill, and bind.
4. :doc:`api/RenderTargets` — where rendered output ends up.
5. :doc:`api/RenderPipeline` / :doc:`api/ComputePipeline` /
   :doc:`api/Blitting` — the three kinds of GPU work.
6. :doc:`api/GPUSubmission` — command queues, command buffers, fences,
   completion callbacks.
7. Then the specialised topics: :doc:`api/Heaps` (resource pooling),
   :doc:`api/Raytracing`, :doc:`api/Triangulation`, :doc:`api/CPUMath`.

.. toctree::
   :maxdepth: 2
   :caption: Concepts

   api/Initialization
   api/Shaders
   api/Buffers
   api/Textures
   api/Heaps
   api/RenderTargets
   api/RenderPipeline
   api/ComputePipeline
   api/GPUSubmission
   api/Blitting
   api/Raytracing
   api/Triangulation
   api/CPUMath
