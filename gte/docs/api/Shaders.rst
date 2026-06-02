=======
Shaders
=======

.. _runtime-compilation:

Every drawable thing in OmegaGTE — every render pipeline, compute pipeline,
blit pipeline, and mesh-shader pipeline — references a *compiled shader entry
point*. Shaders are written in **OmegaSL** (see :doc:`../OmegaSL`), compiled
by the ``omegaslc`` command-line tool into binary ``.omegasllib`` archives,
and then loaded into the engine at runtime. There is also a runtime compiler
on the engine handle, so you can compile shader source the program itself
holds in memory.

This page walks through how libraries are loaded, how the engine reacts when
the device cannot run a particular shader, and the two patterns
(precompiled vs. runtime) you choose between.

.. contents:: On this page
   :local:
   :depth: 2

The library lifecycle
=====================

A shader library is a bundle of named entry points. The bundle is the unit
you load; individual entry points are the things you bind into pipelines.
The journey of one shader looks like this:

1. **Author** an ``.omegasl`` source file with named entry points
   (``vertex VertexRaster myVert(...)`` etc.).
2. **Compile** it offline with ``omegaslc`` to produce a
   ``.omegasllib`` archive — or compile it at runtime through
   ``gte.omegaSlCompiler->compile(source, name)``.
3. **Load** the archive into a :cpp:struct:`OmegaGTE::GTEShaderLibrary` via
   :cpp:func:`OmegaGraphicsEngine::loadShaderLibrary` (file) or
   :cpp:func:`loadShaderLibraryRuntime` (in-memory).
4. **Resolve** named entry points to :cpp:struct:`OmegaGTE::GTEShader`
   handles from the library's ``shaders`` map.
5. **Bind** those handles into a pipeline descriptor and call the matching
   ``makeXxxPipelineState`` on the engine. See :doc:`RenderPipeline`,
   :doc:`ComputePipeline`, and :doc:`Blitting`.

Loading is the only step where I/O happens; everything after is in-process.
The library object owns its shaders, so as long as the library handle is
alive, the entry-point handles you pulled out of it stay alive too.

GTEShader and GTEShaderLibrary
==============================

.. cpp:struct:: OmegaGTE::GTEShader

    Backend-agnostic handle to one compiled entry point. Each backend
    subclasses it internally with the platform-specific bytecode; the public
    surface is opaque — you pass it through to a pipeline descriptor.

    .. cpp:member:: bool isUnsupported

        ``true`` when this handle is a *rejection sentinel* produced because
        the device cannot run the shader (see *Unsupported shaders* below).
        Pipeline factory methods check this and refuse to build a pipeline
        out of a sentinel, surfacing a precise diagnostic instead.

    .. cpp:member:: std::string unsupportedDiagnostic

        Human-readable rejection reason in the form
        ``requires features [A, B]; device lacks [B]``. Populated only when
        ``isUnsupported`` is ``true``.

.. cpp:struct:: OmegaGTE::GTEShaderLibrary

    A loaded archive of compiled shaders.

    .. cpp:member:: std::map<std::string, SharedHandle<GTEShader>> shaders

        Keyed by the function name declared in the OmegaSL source. Look up
        an entry point with ``lib->shaders["myVertex"]``.

    .. cpp:member:: std::map<std::string, std::string> unsupportedDiagnostics

        Per-shader rejection diagnostics, keyed by shader name. Populated at
        load time for any entry point whose required features are not
        satisfied by the active device. Consult this map when a later
        pipeline build fails — the message names the missing feature bits.

Loading a precompiled library
=============================

The common path: compile shaders offline with ``omegaslc`` to a
``.omegasllib`` file, ship that file with the application, and load it once
at startup.

.. cpp:function:: SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibrary(FS::Path path)

    Reads a ``.omegasllib`` archive from disk and returns a populated
    :cpp:struct:`GTEShaderLibrary`. Shaders the device cannot run are
    inserted as rejection sentinels rather than dropping the load; sibling
    shaders in the same archive still resolve normally.

.. code-block:: cpp

    // Precompiled library shipped with the app
    auto shaderLib = gte.graphicsEngine->loadShaderLibrary("assets/myShaders.omegasllib");

    auto vertShader = shaderLib->shaders["myVertex"];
    auto fragShader = shaderLib->shaders["myFragment"];
    auto compShader = shaderLib->shaders["myCompute"];

The library is the lifetime owner. Hold it for as long as any pipeline that
references one of its shaders is in use. Letting the library go while a
pipeline is still bound is undefined behaviour — the shader bytecode the
pipeline points at vanishes.

Compiling and loading at runtime
================================

When shaders are generated at runtime (procedural materials, in-editor
authoring, on-the-fly recompilation for live coding), use the always-present
:cpp:member:`GTE::omegaSlCompiler` to compile a source string into a
shared :cpp:struct:`omegasl_shader_lib` and load that. See :doc:`../OmegaSL`
for the compiler API.

.. cpp:function:: SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibraryRuntime(std::shared_ptr<omegasl_shader_lib> & lib)

    Loads the runtime-compiled library into the engine. Performs the same
    device-feature gating as ``loadShaderLibrary`` — unsupported shaders
    become rejection sentinels with diagnostics.

.. code-block:: cpp

    std::string src = R"(
        vertex VertexRaster myVert(uint vid : VertexID){ … }
        fragment float4 myFrag(VertexRaster r){ … }
    )";

    auto runtimeLib = gte.omegaSlCompiler->compile(src, "myShaders.omegasl");
    auto shaderLib  = gte.graphicsEngine->loadShaderLibraryRuntime(runtimeLib);

    auto vert = shaderLib->shaders["myVert"];
    auto frag = shaderLib->shaders["myFrag"];

Runtime compilation costs noticeably more than loading a precompiled
archive — the cost is OmegaSL's full pipeline (lex → parse → sema → codegen
→ backend driver). Compile once at startup or on a background thread, not
per frame.

Unsupported shaders
===================

OmegaSL shaders declare the device features they need (``raytracing``,
``mesh-shaders``, ``float16``, …). At library load time the engine compares
those required features against the active device's feature mask
(:cpp:func:`GTEDeviceFeatures::featuresAsBitmask`, see :doc:`Initialization`)
and inserts a sentinel handle for any shader whose requirements cannot be
met. The matching diagnostic lands in
:cpp:member:`GTEShaderLibrary::unsupportedDiagnostics`.

Sibling shaders in the same archive load normally. The intent is that a
library shipping both a feature-rich and a fallback variant of a pass loads
on every device — the variant the device can't run just becomes unusable.

Pipeline factory methods (``makeRenderPipelineState``, ``makeComputePipelineState``,
``makeBlitPipelineState``, ``makeMeshPipelineState``) detect a sentinel
bound to one of their slots, write the diagnostic to ``stderr`` naming the
slot (``vertex`` / ``fragment`` / ``compute``) and the pipeline name, and
return ``nullptr``. That lets you handle the missing-feature case at
pipeline construction time rather than chasing a crash inside the backend
later.

.. code-block:: cpp

    auto lib = gte.graphicsEngine->loadShaderLibrary("assets/passes.omegasllib");

    auto shader = lib->shaders["raytracedReflections"];
    if (shader->isUnsupported) {
        std::cerr << "Skipping raytraced reflections: "
                  << shader->unsupportedDiagnostic << "\n";
        // …pick a rasterised fallback…
    } else {
        OmegaGTE::ComputePipelineDescriptor desc;
        desc.computeFunc = shader;
        auto pipeline = gte.graphicsEngine->makeComputePipelineState(desc);
        // pipeline is non-null because shader is supported.
    }

Common pitfalls
===============

* **Discarding the library while pipelines still reference it.** The shader
  bytecode lives in the library; if the library drops, every pipeline that
  uses one of its shaders dangles. Hold the library handle for the lifetime
  of the pipelines.
* **Treating a null pipeline as a "shader missing" error.** ``nullptr`` from
  a ``makeXxxPipelineState`` usually means a sentinel slipped through —
  check ``shaders["name"]->isUnsupported`` first to get a precise reason.
* **Compiling shaders per frame.** ``omegaSlCompiler->compile`` runs the
  full compiler. Compile once, cache the resulting library handle.
* **Looking up a shader by the wrong name.** The map key is the function
  name in OmegaSL source — typos here are silent: ``shaders[...]`` returns
  a default-constructed (null) handle when the name does not exist.
