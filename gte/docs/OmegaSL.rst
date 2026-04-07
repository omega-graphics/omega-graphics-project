=======
OmegaSL
=======

    (Omega Shading Language)

OmegaSL is a cross-platform shading language that transpiles to HLSL (DirectX 12),
MSL (Metal), and GLSL (Vulkan / SPIR-V). It is designed for use with OmegaGTE and
lets you write one shader that runs on all supported platforms without modification.

The compiler ``omegaslc`` takes ``.omegasl`` source files, runs the preprocessor,
lexing, parsing, and semantic analysis with structured error diagnostics, performs
constant folding, and emits platform-specific shader code. Compiled shaders together
with their resource layout metadata are packaged into ``.omegasllib`` binary archives
that OmegaGTE loads at runtime. Runtime compilation is also supported via the
``OmegaSLCompiler`` class.

.. contents:: On this page
   :local:
   :depth: 2

----

Compilation
-----------

Offline via ``omegaslc``
~~~~~~~~~~~~~~~~~~~~~~~~

The command-line compiler ``omegaslc`` compiles a single ``.omegasl`` file into an
``.omegasllib`` archive::

    omegaslc -t <temp_dir> -o <output.omegasllib> <input.omegasl>

**Flags**

``-t <dir>``
    Temporary directory used for intermediate files produced during compilation.

``-o <path>``
    Output path for the finished ``.omegasllib`` archive.

``--tokens-only``
    Dump the raw token stream to stdout and exit without generating any output.
    Useful for debugging lexer issues.

The backend is selected automatically from the build platform:

* **Windows** → HLSL (``D3DCompile``)
* **macOS / iOS** → MSL (``newLibraryWithSource:``)
* **Linux / Android** → GLSL (``shaderc`` / SPIR-V)

**Example**

.. code-block:: bash

    # Compile myshader.omegasl into myshader.omegasllib
    omegaslc -t /tmp/omegasl -o myshader.omegasllib myshader.omegasl

Runtime via ``OmegaSLCompiler``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For hot-reload workflows or procedurally generated shaders you can compile OmegaSL
source at runtime using the ``OmegaSLCompiler`` class:

.. cpp:class:: OmegaGTE::OmegaSLCompiler

    The in-process runtime compiler. Parses OmegaSL, generates target-language source,
    and compiles it via the platform API (``D3DCompile`` on Windows,
    ``newLibraryWithSource:`` on Metal, ``shaderc`` on Vulkan).

.. code-block:: cpp

    #include <OmegaGTE/OmegaSLCompiler.h>

    OmegaGTE::OmegaSLCompiler compiler;

    // Pass the active GTEDevice so the compiler can target the right backend
    compiler.setDevice(device);

    // Compile from a string — second argument is a name used in error messages
    auto lib = compiler.compile(sourceString, "myshader.omegasl");

    // lib can now be passed to pipeline creation functions

----

Preprocessor
------------

OmegaSL includes a lightweight preprocessor that runs before lexing. It supports
simple text macros, conditional compilation, and file inclusion.

Macros
~~~~~~

``#define NAME VALUE`` performs word-boundary-aware text substitution throughout the
rest of the file.

.. code-block:: omegasl

    #define PI      3.14159
    #define TWO_PI  6.28318
    #define MAX_LIGHTS 8

    float circumference(float r){
        return TWO_PI * r;
    }

Macros defined without a value act as presence flags for conditional compilation.

.. code-block:: omegasl

    #define USE_NORMAL_MAP

Conditional Compilation
~~~~~~~~~~~~~~~~~~~~~~~~

``#ifdef`` / ``#ifndef`` blocks let you include or exclude code based on whether a
macro name has been defined. Blocks are nestable.

.. code-block:: omegasl

    #define ENABLE_FOG

    fragment float4 myFrag(VertexRaster raster){
        float4 color = sample(linearSampler, diffuseMap, raster.uv);

    #ifdef ENABLE_FOG
        float depth  = raster.pos.z;
        float fogAmt = clamp((depth - 10.0) / 90.0, 0.0, 1.0);
        color        = lerp(color, make_float4(0.7, 0.7, 0.7, 1.0), fogAmt);
    #endif

        return color;
    }

    // Code guarded by a flag that is NOT defined
    #ifndef DISABLE_LIGHTING
        // lighting code always included here
    #endif

File Inclusion
~~~~~~~~~~~~~~

``#include "path"`` inserts another ``.omegasl`` file at the include site. Paths are
relative to the file that contains the directive. Inclusion is limited to a depth of
10 to prevent circular includes.

.. code-block:: omegasl

    // common.omegasl — shared utilities
    #define GAMMA 2.2

    float linearToSRGB(float c){
        return pow(c, 1.0 / GAMMA);
    }

.. code-block:: omegasl

    // main.omegasl
    #include "common.omegasl"

    fragment float4 myFrag(VertexRaster raster){
        float4 color = sample(linearSampler, diffuseMap, raster.uv);
        color.r = linearToSRGB(color.r);
        color.g = linearToSRGB(color.g);
        color.b = linearToSRGB(color.b);
        return color;
    }

----

Types
-----

Scalar Types
~~~~~~~~~~~~

+----------+---------------------------------------------+
| Type     | Description                                 |
+==========+=============================================+
| ``void`` | No value. Used as a function return type.   |
+----------+---------------------------------------------+
| ``bool`` | Boolean. Literals: ``true`` / ``false``.    |
+----------+---------------------------------------------+
| ``int``  | 32-bit signed integer.                      |
+----------+---------------------------------------------+
| ``uint`` | 32-bit unsigned integer.                    |
+----------+---------------------------------------------+
| ``float``| 32-bit floating-point.                      |
+----------+---------------------------------------------+

.. note::

   ``double`` is **not supported**. Metal Shading Language has no double-precision
   support, and GLSL requires extensions. Use ``float`` for all floating-point work.

.. code-block:: omegasl

    bool  visible = true;
    int   count   = 0;
    uint  index   = 42u;
    float weight  = 0.5;

Vector Types
~~~~~~~~~~~~

OmegaSL has 2-, 3-, and 4-component vectors for ``int``, ``uint``, and ``float``.

+-------------------+------------------------------+
| Type              | Description                  |
+===================+==============================+
| ``int2/3/4``      | Signed integer vectors       |
+-------------------+------------------------------+
| ``uint2/3/4``     | Unsigned integer vectors     |
+-------------------+------------------------------+
| ``float2/3/4``    | Floating-point vectors       |
+-------------------+------------------------------+

**Component access** uses ``.x``, ``.y``, ``.z``, ``.w`` and any swizzle pattern:

.. code-block:: omegasl

    float4 color = make_float4(1.0, 0.5, 0.0, 1.0);

    float  r   = color.x;          // red channel
    float3 rgb = color.xyz;        // swizzle to float3
    float2 rg  = color.xy;         // first two components
    float4 rev = color.wzyx;       // reverse order

**Index access** is also supported:

.. code-block:: omegasl

    float first  = color[0];
    float second = color[1];

**Arithmetic** works component-wise and supports scalar-vector mixing:

.. code-block:: omegasl

    float3 a = make_float3(1.0, 2.0, 3.0);
    float3 b = make_float3(4.0, 5.0, 6.0);

    float3 sum     = a + b;         // (5, 7, 9)
    float3 scaled  = a * 2.0;       // scalar broadcast: (2, 4, 6)
    float3 product = a * b;         // component-wise: (4, 10, 18)

Matrix Types
~~~~~~~~~~~~

+------------------+-------------+
| Type             | Size        |
+==================+=============+
| ``float2x2``     | 2 × 2       |
+------------------+-------------+
| ``float3x3``     | 3 × 3       |
+------------------+-------------+
| ``float4x4``     | 4 × 4       |
+------------------+-------------+

.. code-block:: omegasl

    float4x4 model      = make_float4x4( ... );
    float4x4 view       = make_float4x4( ... );
    float4x4 projection = make_float4x4( ... );
    float4x4 mvp        = projection * view * model;

    // Transform a position
    float4 worldPos = model * make_float4(pos.xyz, 1.0);

Resource Types
~~~~~~~~~~~~~~

Resource types represent GPU-resident data bound to a shader by register number.
They cannot be declared as local variables — they must be top-level declarations.

+-------------------+------------------------------------------------------+
| Type              | Description                                          |
+===================+======================================================+
| ``buffer<T>``     | Structured buffer whose elements are of type ``T``   |
+-------------------+------------------------------------------------------+
| ``texture1d``     | 1D texture                                           |
+-------------------+------------------------------------------------------+
| ``texture2d``     | 2D texture                                           |
+-------------------+------------------------------------------------------+
| ``texture3d``     | 3D texture                                           |
+-------------------+------------------------------------------------------+
| ``sampler1d``     | Sampler paired with a ``texture1d``                  |
+-------------------+------------------------------------------------------+
| ``sampler2d``     | Sampler paired with a ``texture2d``                  |
+-------------------+------------------------------------------------------+
| ``sampler3d``     | Sampler paired with a ``texture3d``                  |
+-------------------+------------------------------------------------------+

Resources are declared at file scope and bound to a register using ``: <n>``:

.. code-block:: omegasl

    struct Vertex {
        float4 pos;
        float2 uv;
    };

    buffer<Vertex>  vertices   : 0;
    texture2d       diffuseMap : 1;
    sampler2d       mySampler  : 2;

Array Types
~~~~~~~~~~~

Fixed-size arrays are declared with C-style ``[N]`` syntax. They may be used as
local variables, struct members, or function parameters.

.. code-block:: omegasl

    // Local array
    float weights[4];
    weights[0] = 0.25;
    weights[1] = 0.25;
    weights[2] = 0.25;
    weights[3] = 0.25;

    // Array in a struct
    struct BoneData {
        float4x4 transforms[64];
    };

    // Iterate over an array
    float total = 0.0;
    for(int i = 0; i < 4; i++){
        total += weights[i];
    }

Literals
~~~~~~~~

.. code-block:: omegasl

    42          // int literal
    3.14        // float literal
    3.14f       // float literal (explicit suffix)
    true        // bool literal
    false       // bool literal

----

Declarations
------------

Structs
~~~~~~~

Structs group related values into a named type. They can be used as function
parameters, return types, buffer element types, and local variables.

.. code-block:: omegasl

    struct Material {
        float4 albedo;
        float  roughness;
        float  metallic;
    };

    float luminance(Material mat){
        float3 rgb = mat.albedo.xyz;
        return dot(rgb, make_float3(0.2126, 0.7152, 0.0722));
    }

**Internal structs** carry data between pipeline stages. Every field must be
decorated with a semantic attribute (see `Attributes`_):

.. code-block:: omegasl

    // Passed from vertex shader → fragment shader
    struct VertexRaster internal {
        float4 pos   : Position;
        float4 color : Color;
        float2 uv    : TexCoord;
    };

Resources
~~~~~~~~~

Top-level resource declarations bind GPU data to a named variable at a fixed
register slot.

.. code-block:: omegasl

    struct Particle {
        float3 position;
        float3 velocity;
        float  lifetime;
    };

    buffer<Particle> particles : 0;    // register 0 — structured buffer
    texture2d        spriteMap : 1;    // register 1 — 2D texture
    sampler2d        spriteSmp : 2;    // register 2 — texture sampler

The same resource can be included in multiple shaders by listing it in each
shader's resource map.

Static Samplers
~~~~~~~~~~~~~~~

A static sampler bakes its filter and address configuration into the pipeline
state at compile time, so no separate ``GESampler`` object is needed at runtime.

.. code-block:: omegasl

    static sampler2d linearWrap(filter=linear,    address_mode=wrap);
    static sampler2d pointClamp(filter=point,      address_mode=clamp_to_edge);
    static sampler2d anisoWrap (filter=anisotropic, address_mode=wrap,
                                 max_anisotropy=8);

**Filter modes**

+-------------------+-------------------------------------------------------+
| Value             | Description                                           |
+===================+=======================================================+
| ``linear``        | Bilinear / trilinear filtering                        |
+-------------------+-------------------------------------------------------+
| ``point``         | Nearest-neighbour (no interpolation)                  |
+-------------------+-------------------------------------------------------+
| ``anisotropic``   | Anisotropic filtering (use with ``max_anisotropy``)   |
+-------------------+-------------------------------------------------------+

**Address modes**

+--------------------+-----------------------------------------------------+
| Value              | Description                                         |
+====================+=====================================================+
| ``wrap``           | Tile the texture (default)                          |
+--------------------+-----------------------------------------------------+
| ``clamp_to_edge``  | Stretch the edge pixel beyond the boundary          |
+--------------------+-----------------------------------------------------+
| ``mirror``         | Alternate flipped / normal tiles                    |
+--------------------+-----------------------------------------------------+
| ``mirror_wrap``    | Mirror once, then repeat                            |
+--------------------+-----------------------------------------------------+

**Other properties**

+--------------------+-----------------------------------------------------+
| Property           | Description                                         |
+====================+=====================================================+
| ``max_anisotropy`` | Maximum anisotropy ratio (integer, 1–16)            |
+--------------------+-----------------------------------------------------+

Static samplers are used with the ``sample`` builtin exactly like regular
sampler resources — just list them in the resource map:

.. code-block:: omegasl

    texture2d albedo : 0;

    static sampler2d linearWrap(filter=linear, address_mode=wrap);

    [in albedo, in linearWrap]
    fragment float4 myFrag(VertexRaster raster){
        return sample(linearWrap, albedo, raster.uv);
    }

User-Defined Functions
~~~~~~~~~~~~~~~~~~~~~~

Helper functions are declared with a return type, a name, and a parameter list.
They must be defined before the shader that calls them (no forward declarations).
Any number of helper functions may be declared; they are emitted before shader
entry points in the generated output.

.. code-block:: omegasl

    // Scalar helpers
    float square(float x){
        return x * x;
    }

    float lerp01(float a, float b, float t){
        return a + (b - a) * clamp(t, 0.0, 1.0);
    }

    // Vector helpers
    float3 toLinear(float3 srgb){
        return make_float3(
            pow(srgb.x, 2.2),
            pow(srgb.y, 2.2),
            pow(srgb.z, 2.2)
        );
    }

    // Struct helpers
    struct LightResult {
        float3 diffuse;
        float3 specular;
    };

    LightResult computeLight(float3 normal, float3 lightDir, float3 viewDir,
                             float roughness){
        float diff = max(dot(normal, lightDir), 0.0);
        float3 h   = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, h), 0.0), (1.0 - roughness) * 128.0);

        LightResult r;
        r.diffuse  = make_float3(diff, diff, diff);
        r.specular = make_float3(spec, spec, spec);
        return r;
    }

Shaders
~~~~~~~

Shaders are declared with a stage keyword, an optional `Resource Maps`_, and a
function signature. A shader may not be called from other functions — it is an
entry point for the GPU pipeline.

**Vertex shader**

Runs once per vertex. Reads from vertex buffers and outputs an internal struct
passed to the fragment stage.

.. code-block:: omegasl

    struct MyVertex {
        float4 pos;
        float4 color;
        float2 uv;
    };

    struct VertexRaster internal {
        float4 pos   : Position;
        float4 color : Color;
        float2 uv    : TexCoord;
    };

    buffer<MyVertex> vbuf : 0;

    float4x4 mvpMatrix : 1;   // constant buffer — single-element buffer<float4x4>

    [in vbuf]
    vertex VertexRaster myVertex(uint vid : VertexID){
        MyVertex v  = vbuf[vid];
        VertexRaster out;
        out.pos   = mvpMatrix[0] * v.pos;   // transform position
        out.color = v.color;
        out.uv    = v.uv;
        return out;
    }

**Fragment shader**

Runs once per rasterised fragment. Receives the interpolated internal struct from
the vertex stage and returns a ``float4`` colour.

.. code-block:: omegasl

    texture2d diffuseMap : 0;
    static sampler2d linearSampler(filter=linear, address_mode=wrap);

    [in diffuseMap, in linearSampler]
    fragment float4 myFragment(VertexRaster raster){
        float4 texColor = sample(linearSampler, diffuseMap, raster.uv);
        return texColor * raster.color;
    }

**Compute shader**

Runs in a grid of parallel thread groups. The ``compute(x, y, z)`` descriptor
specifies the thread-group size in each dimension. The shader body receives the
thread's position via built-in attributes.

.. code-block:: omegasl

    struct DataPoint {
        float value;
    };

    buffer<DataPoint> src : 0;
    buffer<DataPoint> dst : 1;

    // 64 threads per group, 1-D dispatch
    [in src, out dst]
    compute(x=64, y=1, z=1)
    void doubleValues(uint3 tid : GlobalThreadID){
        uint i       = tid[0];
        dst[i].value = src[i].value * 2.0;
    }

For 2-D work (e.g. image processing):

.. code-block:: omegasl

    texture2d inputImg  : 0;
    texture2d outputImg : 1;

    [in inputImg, out outputImg]
    compute(x=8, y=8, z=1)
    void brighten(uint3 tid : GlobalThreadID){
        int2   coord = make_int2((int)tid[0], (int)tid[1]);
        float4 pixel = read(inputImg, coord);
        write(outputImg, coord, pixel * 1.2);
    }

**Hull shader** (Tessellation Control)

Controls how the GPU subdivides patches before the domain stage. The ``hull(...)``
descriptor configures the tessellation pipeline.

.. code-block:: omegasl

    struct ControlPoint {
        float4 pos;
    };

    struct HullOutput internal {
        float4 pos : Position;
    };

    buffer<ControlPoint> controlPoints : 0;

    [in controlPoints, out hullOut]
    hull(domain=tri,
         partitioning=fractional_even,
         outputtopology=triangle_cw,
         outputcontrolpoints=3)
    HullOutput myHull(uint vid : VertexID){
        HullOutput o;
        o.pos = controlPoints[vid].pos;
        return o;
    }

**Hull descriptor properties**

+----------------------+--------------------------------------------------+
| Property             | Values                                           |
+======================+==================================================+
| ``domain``           | ``tri``, ``quad``                                |
+----------------------+--------------------------------------------------+
| ``partitioning``     | ``integer``, ``fractional_even``,                |
|                      | ``fractional_odd``                               |
+----------------------+--------------------------------------------------+
| ``outputtopology``   | ``triangle_cw``, ``triangle_ccw``, ``line``      |
+----------------------+--------------------------------------------------+
| ``outputcontrolpoints`` | Integer — number of control points per patch  |
+----------------------+--------------------------------------------------+

**Domain shader** (Tessellation Evaluation)

Runs once for each UV coordinate generated by the tessellator. Takes the
interpolated control-point data from the hull stage and outputs a final vertex.

.. code-block:: omegasl

    struct DomainOutput internal {
        float4 pos : Position;
    };

    [in controlPoints]
    domain(domain=tri)
    DomainOutput myDomain(uint vid : VertexID){
        DomainOutput o;
        o.pos = controlPoints[vid].pos;
        return o;
    }

**Domain descriptor properties**

+------------+----------------------+
| Property   | Values               |
+============+======================+
| ``domain`` | ``tri``, ``quad``    |
+------------+----------------------+

----

Resource Maps
-------------

A resource map appears immediately before the shader keyword and controls which
top-level resources are accessible inside that shader. Only resources listed in the
map are visible to the shader body.

.. code-block:: omegasl

    [in inputBuffer, out outputBuffer, inout readWriteBuffer]

+----------+-------------------------------------------+
| Modifier | Access                                    |
+==========+===========================================+
| ``in``   | Read-only                                 |
+----------+-------------------------------------------+
| ``out``  | Write-only                                |
+----------+-------------------------------------------+
| ``inout``| Read and write                            |
+----------+-------------------------------------------+

.. note::

   ``in``, ``out``, and ``inout`` are **contextual keywords** inside resource maps.
   They can be used freely as variable or parameter names elsewhere in your code.

**Full example — vertex + fragment with shared texture**

.. code-block:: omegasl

    struct Vertex {
        float4 pos;
        float2 uv;
    };

    struct Raster internal {
        float4 pos : Position;
        float2 uv  : TexCoord;
    };

    buffer<Vertex>  vbuf      : 0;
    texture2d       albedo    : 1;
    texture2d       normalMap : 2;
    static sampler2d trilinear(filter=linear, address_mode=wrap);

    // Vertex shader only needs the vertex buffer
    [in vbuf]
    vertex Raster myVert(uint vid : VertexID){
        Raster out;
        out.pos = vbuf[vid].pos;
        out.uv  = vbuf[vid].uv;
        return out;
    }

    // Fragment shader needs the texture and the sampler
    [in albedo, in normalMap, in trilinear]
    fragment float4 myFrag(Raster raster){
        float4 color  = sample(trilinear, albedo,    raster.uv);
        float4 normal = sample(trilinear, normalMap, raster.uv);
        // … lighting …
        return color;
    }

----

Statements
----------

Variable Declaration
~~~~~~~~~~~~~~~~~~~~

Variables are declared with a type name, an optional initialiser, and a semicolon.
Uninitialized variables contain undefined values.

.. code-block:: omegasl

    float  x      = 1.0;
    int    count;               // uninitialized
    float3 origin = make_float3(0.0, 0.0, 0.0);
    bool   done   = false;

    // Arrays
    float weights[4];

Assignment
~~~~~~~~~~

Plain and compound assignment operators are all supported:

.. code-block:: omegasl

    x  = 42.0;
    x += 1.0;    // x = x + 1
    x -= 2.0;    // x = x - 2
    x *= 3.0;    // x = x * 3
    x /= 4.0;    // x = x / 4

Compound assignment works on vectors too:

.. code-block:: omegasl

    float3 accum = make_float3(0.0, 0.0, 0.0);
    accum += make_float3(1.0, 0.0, 0.0);
    accum *= 2.0;

Return
~~~~~~

``return`` exits a function and optionally yields a value:

.. code-block:: omegasl

    float square(float x){
        return x * x;         // return with value
    }

    void doSomething(){
        return;               // bare return from void function
    }

Control Flow
~~~~~~~~~~~~

**if / else if / else**

.. code-block:: omegasl

    float classify(float x){
        if(x > 0.0){
            return 1.0;
        }
        else if(x < 0.0){
            return -1.0;
        }
        else {
            return 0.0;
        }
    }

**for loop**

.. code-block:: omegasl

    // Sum the first N elements of a buffer
    float total = 0.0;
    for(int i = 0; i < 8; i++){
        total += src[i].value;
    }

    // Nested loops for 2-D access
    for(int y = 0; y < 4; y++){
        for(int x = 0; x < 4; x++){
            float4 p = read(tex, make_int2(x, y));
            // …
        }
    }

**while loop**

.. code-block:: omegasl

    float val = 1024.0;
    int   n   = 0;
    while(val > 1.0){
        val /= 2.0;
        n   += 1;
    }

----

Expressions
-----------

Binary Operators
~~~~~~~~~~~~~~~~

+--------------+---------------------------------------------------+
| Operator     | Description                                       |
+==============+===================================================+
| ``+``        | Addition                                          |
+--------------+---------------------------------------------------+
| ``-``        | Subtraction                                       |
+--------------+---------------------------------------------------+
| ``*``        | Multiplication (component-wise for vectors)       |
+--------------+---------------------------------------------------+
| ``/``        | Division (component-wise for vectors)             |
+--------------+---------------------------------------------------+
| ``==``       | Equality                                          |
+--------------+---------------------------------------------------+
| ``!=``       | Inequality                                        |
+--------------+---------------------------------------------------+
| ``<``        | Less than                                         |
+--------------+---------------------------------------------------+
| ``<=``       | Less than or equal                                |
+--------------+---------------------------------------------------+
| ``>``        | Greater than                                      |
+--------------+---------------------------------------------------+
| ``>=``       | Greater than or equal                             |
+--------------+---------------------------------------------------+
| ``=``        | Assignment                                        |
+--------------+---------------------------------------------------+
| ``+=``       | Add and assign                                    |
+--------------+---------------------------------------------------+
| ``-=``       | Subtract and assign                               |
+--------------+---------------------------------------------------+
| ``*=``       | Multiply and assign                               |
+--------------+---------------------------------------------------+
| ``/=``       | Divide and assign                                 |
+--------------+---------------------------------------------------+

Scalar-vector mixed arithmetic broadcasts the scalar to every component:

.. code-block:: omegasl

    float3 v = make_float3(2.0, 4.0, 6.0);
    float3 a = v + 1.0;      // (3, 5, 7)
    float3 b = v * 0.5;      // (1, 2, 3)
    bool   c = (v.x == 2.0); // true

Unary Operators
~~~~~~~~~~~~~~~

+----------+--------------------+---------------------------+
| Operator | Position           | Description               |
+==========+====================+===========================+
| ``-``    | Prefix             | Numeric negation          |
+----------+--------------------+---------------------------+
| ``!``    | Prefix             | Logical NOT               |
+----------+--------------------+---------------------------+
| ``++``   | Prefix or postfix  | Increment by 1            |
+----------+--------------------+---------------------------+
| ``--``   | Prefix or postfix  | Decrement by 1            |
+----------+--------------------+---------------------------+

.. code-block:: omegasl

    float x = 5.0;
    float y = -x;        // -5.0

    bool flag = false;
    bool inv  = !flag;   // true

    int i = 0;
    i++;                 // postfix increment → i == 1
    ++i;                 // prefix  increment → i == 2
    i--;                 // postfix decrement → i == 1

Other Expressions
~~~~~~~~~~~~~~~~~

**Function call**

.. code-block:: omegasl

    float s = sin(angle);
    float3 n = normalize(make_float3(1.0, 0.0, 0.0));

**Member access**

.. code-block:: omegasl

    float3 pos   = myVertex.pos.xyz;
    float  alpha = color.w;

**Index access**

.. code-block:: omegasl

    float first = myBuffer[0].value;
    float comp  = myVec[2];

**C-style cast**

.. code-block:: omegasl

    int   n   = 7;
    float f   = (float)n;     // int → float
    uint  u   = (uint)f;      // float → uint

**Address-of / dereference**

.. code-block:: omegasl

    float  val  = 1.0;
    float* ptr  = &val;       // take address
    float  copy = *ptr;       // dereference

----

Builtin Functions
-----------------

Vector Constructors
~~~~~~~~~~~~~~~~~~~

Vectors are constructed with ``make_<type><N>(...)`` functions. Multiple argument
shapes are accepted — you can mix smaller vectors and scalars:

.. code-block:: omegasl

    // float2
    float2 a = make_float2(1.0, 2.0);

    // float3 — three scalars, or float2 + scalar
    float3 b = make_float3(1.0, 2.0, 3.0);
    float3 c = make_float3(a, 3.0);          // extend float2

    // float4 — various combinations
    float4 d = make_float4(1.0, 2.0, 3.0, 4.0);
    float4 e = make_float4(b, 1.0);              // float3 + scalar
    float4 f = make_float4(a, 3.0, 4.0);         // float2 + scalar + scalar

Integer and unsigned constructors follow the same pattern:

.. code-block:: omegasl

    int2  gi = make_int2(10, 20);
    int3  gj = make_int3(1, 2, 3);
    uint4 gu = make_uint4(0u, 1u, 2u, 3u);

Matrix Constructors
~~~~~~~~~~~~~~~~~~~

Matrices are constructed with ``make_float<N>x<N>(...)`` and accept their elements
in row-major order:

.. code-block:: omegasl

    // Identity matrices
    float2x2 id2 = make_float2x2(
        1.0, 0.0,
        0.0, 1.0
    );

    float4x4 id4 = make_float4x4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

    // Scale matrix
    float4x4 scale = make_float4x4(
        2.0, 0.0, 0.0, 0.0,
        0.0, 2.0, 0.0, 0.0,
        0.0, 0.0, 2.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    );

Vector Math
~~~~~~~~~~~

**dot** — dot product of two vectors of any matching type. Returns a scalar.

.. code-block:: omegasl

    float3 a = make_float3(1.0, 0.0, 0.0);
    float3 b = make_float3(0.0, 1.0, 0.0);

    float d      = dot(a, b);             // 0.0 (perpendicular)
    float cosine = dot(normalize(a), normalize(b));  // cos of angle between them

**cross** — cross product of two ``float3`` vectors. Returns a ``float3``
perpendicular to both.

.. code-block:: omegasl

    float3 right   = make_float3(1.0, 0.0, 0.0);
    float3 up      = make_float3(0.0, 1.0, 0.0);
    float3 forward = cross(right, up);    // (0, 0, 1)

    // Build a TBN matrix from geometry
    float3 T = normalize(tangent);
    float3 N = normalize(normal);
    float3 B = cross(N, T);

Texture Operations
~~~~~~~~~~~~~~~~~~

**sample** — sample a texture through a sampler at a UV coordinate.

The sampler and texture dimensions must match.

.. code-block:: omegasl

    // 2D texture — most common case
    texture2d  albedo      : 0;
    sampler2d  mySampler   : 1;

    float4 color  = sample(mySampler, albedo, raster.uv);  // float2 UV

    // 1D texture (e.g. colour ramp lookup)
    texture1d  ramp    : 0;
    sampler1d  rampSmp : 1;

    float4 rampColor = sample(rampSmp, ramp, 0.5);  // float U

    // 3D texture (e.g. volume fog / LUT)
    texture3d  volume    : 0;
    sampler3d  volSampler: 1;

    float4 fogColor = sample(volSampler, volume,
                             make_float3(uv.x, uv.y, depth));  // float3 UVW

**read** — fetch a texel at an integer pixel coordinate, bypassing the sampler.

.. code-block:: omegasl

    int2   coord = make_int2(px, py);
    float4 pixel = read(myTexture, coord);

    // Read each pixel in a compute kernel
    compute(x=8, y=8, z=1)
    void process(uint3 tid : GlobalThreadID){
        int2   coord = make_int2((int)tid[0], (int)tid[1]);
        float4 pixel = read(inputImg, coord);
        write(outputImg, coord, pixel);
    }

**write** — write a value to a texture at an integer pixel coordinate.
Only valid in compute shaders and for ``out`` / ``inout`` resources.

.. code-block:: omegasl

    write(outputImg, make_int2(x, y), make_float4(r, g, b, 1.0));

Math Intrinsics
~~~~~~~~~~~~~~~

All standard math functions are recognised builtins with argument-count validation
and proper type inference. They pass through to the equivalent in the target language.

**Single-argument functions** — return the same type as their input:

+----------------------------+----------------------------------------+
| Function                   | Description                            |
+============================+========================================+
| ``sin(x)``                 | Sine                                   |
+----------------------------+----------------------------------------+
| ``cos(x)``                 | Cosine                                 |
+----------------------------+----------------------------------------+
| ``tan(x)``                 | Tangent                                |
+----------------------------+----------------------------------------+
| ``asin(x)``                | Arc-sine                               |
+----------------------------+----------------------------------------+
| ``acos(x)``                | Arc-cosine                             |
+----------------------------+----------------------------------------+
| ``atan(x)``                | Arc-tangent (single-argument)          |
+----------------------------+----------------------------------------+
| ``sqrt(x)``                | Square root                            |
+----------------------------+----------------------------------------+
| ``abs(x)``                 | Absolute value                         |
+----------------------------+----------------------------------------+
| ``floor(x)``               | Round toward negative infinity         |
+----------------------------+----------------------------------------+
| ``ceil(x)``                | Round toward positive infinity         |
+----------------------------+----------------------------------------+
| ``round(x)``               | Round to nearest integer               |
+----------------------------+----------------------------------------+
| ``frac(x)``                | Fractional part (``fract`` in MSL/GLSL)|
+----------------------------+----------------------------------------+
| ``exp(x)``                 | Natural exponential (e^x)              |
+----------------------------+----------------------------------------+
| ``exp2(x)``                | Base-2 exponential (2^x)               |
+----------------------------+----------------------------------------+
| ``log(x)``                 | Natural logarithm                      |
+----------------------------+----------------------------------------+
| ``log2(x)``                | Base-2 logarithm                       |
+----------------------------+----------------------------------------+
| ``normalize(v)``           | Normalize a vector to unit length      |
+----------------------------+----------------------------------------+
| ``length(v)``              | Vector length (returns ``float``)      |
+----------------------------+----------------------------------------+

.. code-block:: omegasl

    float  angle   = 1.5708;                  // π/2
    float  s       = sin(angle);              // ≈ 1.0
    float  c       = cos(angle);              // ≈ 0.0
    float3 dir     = make_float3(3.0, 4.0, 0.0);
    float  len     = length(dir);             // 5.0
    float3 unit    = normalize(dir);          // (0.6, 0.8, 0.0)
    float  f       = frac(3.75);              // 0.75
    float  rounded = round(2.6);             // 3.0

**Two-argument functions**:

+-----------------------------+-----------------------------------------------+
| Function                    | Description                                   |
+=============================+===============================================+
| ``atan2(y, x)``             | Arc-tangent of y/x, respects quadrant         |
+-----------------------------+-----------------------------------------------+
| ``pow(base, exp)``          | Raise ``base`` to the power ``exp``           |
+-----------------------------+-----------------------------------------------+
| ``min(a, b)``               | Component-wise minimum                        |
+-----------------------------+-----------------------------------------------+
| ``max(a, b)``               | Component-wise maximum                        |
+-----------------------------+-----------------------------------------------+
| ``step(edge, x)``           | 0 if x < edge, 1 otherwise                   |
+-----------------------------+-----------------------------------------------+
| ``reflect(incident, normal)``| Reflection of incident about normal          |
+-----------------------------+-----------------------------------------------+

.. code-block:: omegasl

    float angle2 = atan2(1.0, 1.0);           // π/4 (45°)
    float gamma  = pow(color.r, 1.0 / 2.2);   // sRGB encode
    float lo     = min(a, b);
    float hi     = max(a, b);
    float mask   = step(0.5, brightness);      // thresholding

    float3 incident = make_float3(0.0, -1.0, 0.0);
    float3 n        = make_float3(0.0,  1.0, 0.0);
    float3 bounced  = reflect(incident, n);    // (0, 1, 0)

**Three-argument functions**:

+-----------------------------+-----------------------------------------------+
| Function                    | Description                                   |
+=============================+===============================================+
| ``clamp(x, lo, hi)``        | Clamp ``x`` to [lo, hi]                       |
+-----------------------------+-----------------------------------------------+
| ``lerp(a, b, t)``           | Linear interpolation (``mix`` in MSL/GLSL)    |
+-----------------------------+-----------------------------------------------+
| ``smoothstep(lo, hi, x)``   | Smooth Hermite interpolation between lo/hi    |
+-----------------------------+-----------------------------------------------+

.. code-block:: omegasl

    float safe    = clamp(value,  0.0, 1.0);
    float3 blend  = lerp(colorA, colorB, 0.5);     // midpoint blend
    float  edge   = smoothstep(0.4, 0.6, sharpness); // anti-aliased threshold

----

Attributes
----------

Attributes decorate struct fields or shader parameters using ``: AttributeName``
syntax. They tell the compiler how to map your variables to GPU pipeline semantics.

Render Pipeline Attributes
~~~~~~~~~~~~~~~~~~~~~~~~~~

+----------------+-------------------------------------------------------+-----------------------------------------------+
| Attribute      | Where to use                                          | Description                                   |
+================+=======================================================+===============================================+
| ``VertexID``   | Vertex / Hull / Domain shader parameter               | Zero-based index of the current vertex        |
+----------------+-------------------------------------------------------+-----------------------------------------------+
| ``InstanceID`` | Vertex shader parameter                               | Zero-based index of the current instance      |
+----------------+-------------------------------------------------------+-----------------------------------------------+
| ``Position``   | ``internal`` struct field                             | Clip-space vertex position (output)           |
+----------------+-------------------------------------------------------+-----------------------------------------------+
| ``Color``      | ``internal`` struct field                             | Fragment colour channel                       |
+----------------+-------------------------------------------------------+-----------------------------------------------+
| ``TexCoord``   | ``internal`` struct field                             | Texture coordinate (UV)                       |
+----------------+-------------------------------------------------------+-----------------------------------------------+

.. code-block:: omegasl

    // Vertex shader with both VertexID and InstanceID
    [in vbuf, in instances]
    vertex VertexRaster myVert(uint vid : VertexID, uint iid : InstanceID){
        MyVertex   v = vbuf[vid];
        InstanceData inst = instances[iid];

        VertexRaster out;
        out.pos   = inst.transform * v.pos;
        out.color = v.color;
        out.uv    = v.uv;
        return out;
    }

    // Internal struct with all render attributes
    struct FullRaster internal {
        float4 pos   : Position;
        float4 color : Color;
        float2 uv    : TexCoord;
    };

Compute Pipeline Attributes
~~~~~~~~~~~~~~~~~~~~~~~~~~~

+-------------------+-------------------------------+----------------------------------------------------+
| Attribute         | Parameter position            | Description                                        |
+===================+===============================+====================================================+
| ``GlobalThreadID``| 1st compute parameter         | Thread position in the full dispatch grid          |
+-------------------+-------------------------------+----------------------------------------------------+
| ``LocalThreadID`` | 2nd compute parameter         | Thread position within its local thread group      |
+-------------------+-------------------------------+----------------------------------------------------+
| ``ThreadGroupID`` | 3rd compute parameter         | Thread-group position in the dispatch grid         |
+-------------------+-------------------------------+----------------------------------------------------+

All three are of type ``uint3``. Use ``[0]``, ``[1]``, ``[2]`` to access x/y/z.

.. code-block:: omegasl

    // All three compute attributes
    [in src, out dst]
    compute(x=64, y=1, z=1)
    void myKernel(uint3 gid : GlobalThreadID,
                  uint3 lid : LocalThreadID,
                  uint3 gid2 : ThreadGroupID){

        uint globalIdx = gid[0];    // position in whole grid
        uint localIdx  = lid[0];    // position inside the 64-thread group
        uint groupIdx  = gid2[0];   // which group this thread belongs to

        dst[globalIdx].value = src[globalIdx].value + (float)localIdx;
    }

    // 2-D compute — image processing kernel
    [in inputImg, out outputImg]
    compute(x=8, y=8, z=1)
    void blur(uint3 gid : GlobalThreadID){
        int  x = (int)gid[0];
        int  y = (int)gid[1];

        float4 center = read(inputImg, make_int2(x,   y));
        float4 right  = read(inputImg, make_int2(x+1, y));
        float4 left   = read(inputImg, make_int2(x-1, y));

        float4 avg = (center + right + left) / 3.0;
        write(outputImg, make_int2(x, y), avg);
    }

----

Backend Mapping
---------------

OmegaSL types and builtins map to platform equivalents as follows:

+------------------------------+------------------------+---------------------+------------------------+
| OmegaSL                      | HLSL                   | MSL (Metal)         | GLSL (Vulkan)          |
+==============================+========================+=====================+========================+
| ``float``                    | ``float``              | ``float``           | ``float``              |
+------------------------------+------------------------+---------------------+------------------------+
| ``float2/3/4``               | ``float2/3/4``         | ``float2/3/4``      | ``vec2/3/4``           |
+------------------------------+------------------------+---------------------+------------------------+
| ``int2/3/4``                 | ``int2/3/4``           | ``int2/3/4``        | ``ivec2/3/4``          |
+------------------------------+------------------------+---------------------+------------------------+
| ``uint2/3/4``                | ``uint2/3/4``          | ``uint2/3/4``       | ``uvec2/3/4``          |
+------------------------------+------------------------+---------------------+------------------------+
| ``float2x2/3x3/4x4``         | ``float2x2/3x3/4x4``  | ``float2x2/3x3/4x4``| ``mat2/3/4``           |
+------------------------------+------------------------+---------------------+------------------------+
| ``make_float4(...)``         | ``float4(...)``        | ``float4(...)``     | ``vec4(...)``          |
+------------------------------+------------------------+---------------------+------------------------+
| ``sample(s, t, c)``          | ``t.Sample(s, c)``     | ``t.sample(s, c)``  | ``texture(sampler2D(t,s), c)`` |
+------------------------------+------------------------+---------------------+------------------------+
| ``read(t, c)``               | ``t.Load(c)``          | ``t.read(c)``       | ``texelFetch(t, c, 0)``|
+------------------------------+------------------------+---------------------+------------------------+
| ``write(t, c, v)``           | ``t[c] = v``           | ``t.write(v, c)``   | ``imageStore(t, c, v)``|
+------------------------------+------------------------+---------------------+------------------------+
| ``buffer<T>`` (``in``)       | ``StructuredBuffer<T>``| ``constant T*``     | ``layout(std430) readonly buffer`` |
+------------------------------+------------------------+---------------------+------------------------+
| ``buffer<T>`` (``out``)      | ``RWStructuredBuffer<T>``| ``device T*``     | ``layout(std430) buffer`` |
+------------------------------+------------------------+---------------------+------------------------+
| ``frac(x)``                  | ``frac(x)``            | ``fract(x)``        | ``fract(x)``           |
+------------------------------+------------------------+---------------------+------------------------+
| ``lerp(a, b, t)``            | ``lerp(a, b, t)``      | ``mix(a, b, t)``    | ``mix(a, b, t)``       |
+------------------------------+------------------------+---------------------+------------------------+
| ``vertex`` entry point       | ``vs_5_0``             | ``vertex``          | ``.vert``              |
+------------------------------+------------------------+---------------------+------------------------+
| ``fragment`` entry point     | ``ps_5_0``             | ``fragment``        | ``.frag``              |
+------------------------------+------------------------+---------------------+------------------------+
| ``compute`` entry point      | ``cs_5_0``             | ``kernel``          | ``.comp``              |
+------------------------------+------------------------+---------------------+------------------------+
| ``hull`` entry point         | ``hs_5_0``             | ``kernel``          | ``.tesc``              |
+------------------------------+------------------------+---------------------+------------------------+
| ``domain`` entry point       | ``ds_5_0``             | ``[[patch(...)]] vertex`` | ``.tese``      |
+------------------------------+------------------------+---------------------+------------------------+
