=======
OmegaSL
=======

    (Omega Shading Language)

OmegaSL is a cross-platform shading language that transpiles to HLSL (DirectX 12),
MSL (Metal), and GLSL (Vulkan / SPIR-V). It is designed for use with OmegaGTE.

The compiler ``omegaslc`` parses ``.omegasl`` source files, runs semantic analysis
with structured error diagnostics, performs constant folding, and generates
platform-specific shader code. Compiled shaders and resource layout metadata are
packaged into ``.omegasllib`` binary archives. Runtime compilation is also supported
via the ``OmegaSLCompiler`` class.

Features
--------

- **Types**: ``bool``, ``int``, ``uint``, ``float``, vectors (``float2/3/4``, ``int2/3/4``, ``uint2/3/4``),
  matrices (``float2x2/3x3/4x4``), resources (``buffer<T>``, ``texture1d/2d/3d``, ``sampler1d/2d/3d``),
  fixed-size arrays, boolean literals (``true``/``false``).

- **Shader stages**: ``vertex``, ``fragment``, ``compute``, ``hull`` (tessellation control),
  ``domain`` (tessellation evaluation). Hull and domain shaders accept configurable
  descriptors (domain, partitioning, output topology, control point count).

- **Expressions**: Arithmetic, comparison, assignment, compound assignment (``+=``, ``-=``, ``*=``, ``/=``),
  prefix/postfix unary (``-``, ``!``, ``++``, ``--``), function calls, member access,
  index access, C-style casts.

- **Control flow**: ``if`` / ``else if`` / ``else``, ``for``, ``while``, bare ``return;``.

- **Builtins**: Vector constructors (``make_float2/3/4``, ``make_int2/3/4``, ``make_uint2/3/4``,
  ``make_float2x2/3x3/4x4``), ``dot``, ``cross``, ``sample``, ``read``, ``write``.
  Standard math functions (``sin``, ``cos``, ``sqrt``, etc.) pass through to the target language.

- **User-defined functions**: Non-shader helper functions callable from shaders and other functions.

- **Preprocessor**: ``#define``, ``#ifdef`` / ``#ifndef`` / ``#endif``, ``#include "file"``.

- **Diagnostics**: Structured error types with source location tracking and
  code-view output (``^`` underline spans).

- **Constant folding**: Compile-time evaluation of numeric literal expressions.

- **Resource maps**: ``[in x, out y, inout z]`` syntax for binding GPU resources to shaders.

- **Static samplers**: ``static sampler2d name(filter=linear, address_mode=wrap);``

Language Reference
------------------

See ``OmegaSL-Reference.md`` for the complete language reference covering types,
declarations, statements, expressions, builtins, attributes, resource maps,
compilation, and backend mapping tables.

Compilation
-----------

Via ``omegaslc``::

    omegaslc -t <temp_dir> -o <output.omegasllib> <input.omegasl>

Via runtime API:

.. cpp:class:: OmegaGTE::OmegaSLCompiler

    The runtime interface for compiling OmegaSL shaders in-process.
    Supports Metal (``newLibraryWithSource:``), DirectX (``D3DCompile``),
    and Vulkan (``shaderc``).
