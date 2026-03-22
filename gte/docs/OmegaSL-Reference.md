# OmegaSL Language Reference

OmegaSL is a cross-platform shading language that transpiles to HLSL (DirectX), MSL (Metal), and GLSL (Vulkan). The compiler `omegaslc` produces `.omegasllib` archives containing compiled shader bytecode and resource layout metadata.

## 1. Preprocessor

OmegaSL has a lightweight preprocessor that runs before lexing.

```omegasl
#define PI 3.14159
#define USE_LIGHTING

#ifdef USE_LIGHTING
// included when USE_LIGHTING is defined
#endif

#ifndef FALLBACK
// included when FALLBACK is NOT defined
#endif

#include "common.omegasl"
```

- `#define NAME VALUE` -- simple text macros (word-boundary-aware substitution).
- `#ifdef NAME` / `#ifndef NAME` / `#endif` -- conditional compilation (nestable).
- `#include "path"` -- file inclusion with a depth limit of 10.

## 2. Types

### Scalar types

| Type | Description |
|------|-------------|
| `void` | No value |
| `bool` | Boolean (`true` / `false`) |
| `int` | 32-bit signed integer |
| `uint` | 32-bit unsigned integer |
| `float` | 32-bit floating-point |

`double` is **not supported** — Metal Shading Language has no double precision, and GLSL requires extensions. Use `float` for all floating-point operations.

### Vector types

| Type | Components |
|------|------------|
| `int2`, `int3`, `int4` | Signed integer vectors |
| `uint2`, `uint3`, `uint4` | Unsigned integer vectors |
| `float2`, `float3`, `float4` | Floating-point vectors |

Vectors support component access via `.x`, `.y`, `.z`, `.w` and swizzle patterns (`.xy`, `.xyz`, `.xyzw`, etc.). Index access is also supported: `v[0]`, `v[1]`, etc.

### Matrix types

| Type | Size |
|------|------|
| `float2x2` | 2x2 matrix |
| `float3x3` | 3x3 matrix |
| `float4x4` | 4x4 matrix |

### Resource types

| Type | Description |
|------|-------------|
| `buffer<T>` | Structured buffer of element type `T` |
| `texture1d` | 1D texture |
| `texture2d` | 2D texture |
| `texture3d` | 3D texture |
| `sampler1d` | 1D texture sampler |
| `sampler2d` | 2D texture sampler |
| `sampler3d` | 3D texture sampler |

### Array types

Fixed-size arrays are supported in variable declarations:

```omegasl
float arr[4];
int indices[16];
```

### Literals

```omegasl
42          // int
3.14        // float
3.14f       // float (explicit)
true        // bool
false       // bool
```

## 3. Declarations

### Structs

```omegasl
struct MyVertex {
    float4 pos;
    float4 color;
};
```

**Internal structs** are used to transfer data between shader stages. All fields must have attributes:

```omegasl
struct VertexRaster internal {
    float4 pos : Position;
    float4 color : Color;
    float2 uv : TexCoord;
};
```

### Resources

Resources are GPU-allocated data bound by register number:

```omegasl
buffer<MyVertex> vertices : 0;
texture2d diffuseMap : 1;
sampler2d mySampler : 2;
```

### Static samplers

Samplers can be declared with a static configuration using function-call syntax:

```omegasl
static sampler2d linearSampler(filter=linear, address_mode=wrap);
static sampler2d pointClamp(filter=point, address_mode=clamp_to_edge);
```

**Filter modes**: `linear`, `point`, `anisotropic`.
**Address modes**: `wrap`, `clamp_to_edge`, `mirror`, `mirror_wrap`.
**Other properties**: `max_anisotropy=N`.

### User-defined functions

```omegasl
float square(float x){
    return x * x;
}

float lerp(float a, float b, float t){
    return a + (b - a) * t;
}
```

Functions must be declared before use (no forward declarations). Functions declared before a shader are emitted as helpers in the generated output.

### Shaders

Shaders are declared with a stage keyword, an optional resource map, and a function signature.

**Vertex shader**:

```omegasl
buffer<MyVertex> vbuf : 0;

[in vbuf]
vertex VertexRaster myVertex(uint vid : VertexID){
    MyVertex v = vbuf[vid];
    VertexRaster out;
    out.pos = v.pos;
    out.color = v.color;
    return out;
}
```

**Fragment shader**:

```omegasl
[in diffuseMap, in linearSampler]
fragment float4 myFragment(VertexRaster raster){
    return sample(linearSampler, diffuseMap, raster.uv);
}
```

**Compute shader** (requires threadgroup dimensions):

```omegasl
[in src, out dst]
compute(x=64, y=1, z=1)
void myCompute(uint3 tid : GlobalThreadID){
    dst[tid[0]].value = src[tid[0]].value * 2.0;
}
```

**Hull shader** (tessellation control):

```omegasl
[in controlPoints, out hullOut]
hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3)
HullOutput myHull(uint vid : VertexID){
    HullOutput o;
    o.pos = controlPoints[vid].pos;
    return o;
}
```

**Domain shader** (tessellation evaluation):

```omegasl
[in controlPoints]
domain(domain=tri)
DomainOutput myDomain(uint vid : VertexID){
    DomainOutput o;
    o.pos = controlPoints[vid].pos;
    return o;
}
```

**Hull descriptor properties**: `domain` (`tri`, `quad`), `partitioning` (`integer`, `fractional_even`, `fractional_odd`), `outputtopology` (`triangle_cw`, `triangle_ccw`, `line`), `outputcontrolpoints` (integer).

**Domain descriptor properties**: `domain` (`tri`, `quad`).

## 4. Resource Maps

Resource maps precede the shader keyword and control which GPU resources are accessible:

```omegasl
[in inputBuffer, out outputBuffer, inout readWriteBuffer]
```

- `in` -- read-only access.
- `out` -- write-only access.
- `inout` -- read-write access.

`in`, `out`, and `inout` are contextual keywords -- they can be used as variable names outside of resource maps.

## 5. Statements

### Variable declaration

```omegasl
float x = 1.0;
int count;
float4 color = make_float4(1.0, 0.0, 0.0, 1.0);
```

### Assignment

```omegasl
x = 42.0;
x += 1.0;
x -= 2.0;
x *= 3.0;
x /= 4.0;
```

### Return

```omegasl
return expr;    // return with value
return;         // void return (bare)
```

### Control flow

```omegasl
if(x > 0.0){
    // ...
}
else if(x < 0.0){
    // ...
}
else {
    // ...
}

for(int i = 0; i < 10; i++){
    // ...
}

while(val > 1.0){
    val = val / 2.0;
}
```

## 6. Expressions

### Binary operators

| Operator | Description |
|----------|-------------|
| `+`, `-`, `*`, `/` | Arithmetic |
| `==`, `!=` | Equality |
| `<`, `<=`, `>`, `>=` | Comparison |
| `=` | Assignment |
| `+=`, `-=`, `*=`, `/=` | Compound assignment |

Scalar-vector operations are supported (e.g. `float4 * float`).

### Unary operators

| Operator | Position | Description |
|----------|----------|-------------|
| `-` | Prefix | Negation |
| `!` | Prefix | Logical NOT |
| `++` | Prefix/Postfix | Increment |
| `--` | Prefix/Postfix | Decrement |

### Other expressions

- **Function call**: `func(arg1, arg2)`
- **Member access**: `obj.field`
- **Index access**: `buf[i]`, `vec[0]`
- **C-style cast**: `(float)intVal`
- **Address-of / dereference**: `&val`, `*ptr`

## 7. Builtin Functions

### Vector constructors

```omegasl
float2 v2 = make_float2(1.0, 2.0);
float3 v3 = make_float3(1.0, 2.0, 3.0);
float3 v3b = make_float3(v2, 3.0);         // float2 + float
float4 v4 = make_float4(1.0, 2.0, 3.0, 4.0);
float4 v4b = make_float4(v3, 1.0);         // float3 + float
float4 v4c = make_float4(v2, 3.0, 4.0);    // float2 + float + float
```

Integer and unsigned constructors follow the same pattern: `make_int2/3/4`, `make_uint2/3/4`.

### Matrix constructors

```omegasl
float2x2 m2 = make_float2x2(...);
float3x3 m3 = make_float3x3(...);
float4x4 m4 = make_float4x4(...);
```

### Vector math

```omegasl
float d = dot(a, b);       // dot product (any vector type)
float3 c = cross(a, b);    // cross product (float3 only)
```

### Texture operations

```omegasl
// Sample a texture using a sampler at the given coordinate
float4 color = sample(mySampler, myTexture, texCoord);

// Read a texel directly by integer coordinate
float4 texel = read(myTexture, make_int2(x, y));

// Write a value to a texture at a coordinate
write(myTexture, coord, value);
```

`sample` supports all sampler/texture dimension combinations: `sampler1d`+`texture1d`+`float`, `sampler2d`+`texture2d`+`float2`, `sampler3d`+`texture3d`+`float3`.

### Math intrinsics

Standard math functions are recognized builtins with argument count validation and proper type inference.

**1-argument** (same type in and out):

| Function | Description |
|----------|-------------|
| `sin(x)`, `cos(x)`, `tan(x)` | Trigonometric |
| `asin(x)`, `acos(x)`, `atan(x)` | Inverse trigonometric |
| `sqrt(x)` | Square root |
| `abs(x)` | Absolute value |
| `floor(x)`, `ceil(x)`, `round(x)` | Rounding |
| `frac(x)` | Fractional part (HLSL: `frac`, MSL/GLSL: `fract`) |
| `exp(x)`, `exp2(x)` | Exponential |
| `log(x)`, `log2(x)` | Logarithm |
| `normalize(v)` | Unit vector |
| `length(v)` | Vector length (returns `float`) |

**2-argument**:

| Function | Description |
|----------|-------------|
| `atan2(y, x)` | Two-argument arctangent |
| `pow(base, exp)` | Power |
| `min(a, b)`, `max(a, b)` | Minimum / maximum |
| `step(edge, x)` | Step function |
| `reflect(incident, normal)` | Reflection vector |

**3-argument**:

| Function | Description |
|----------|-------------|
| `clamp(x, lo, hi)` | Clamp to range |
| `lerp(a, b, t)` | Linear interpolation (HLSL: `lerp`, MSL/GLSL: `mix`) |
| `smoothstep(lo, hi, x)` | Smooth Hermite interpolation |

```omegasl
float s = sin(angle);
float c = cos(angle);
float r = sqrt(x * x + y * y);
float3 n = normalize(direction);
float blend = lerp(colorA, colorB, 0.5);
float clamped = clamp(value, 0.0, 1.0);
```

## 8. Attributes

Attributes are attached to struct fields or shader parameters using `: AttributeName` syntax.

### Render pipeline attributes

| Attribute | Usage | Description |
|-----------|-------|-------------|
| `VertexID` | Vertex/Hull/Domain shader parameter | Index of the current vertex |
| `InstanceID` | Vertex shader parameter | Index of the current instance |
| `Position` | Internal struct field | Vertex position output |
| `Color` | Internal struct field | Fragment color |
| `TexCoord` | Internal struct field | Texture coordinate |

### Compute pipeline attributes

| Attribute | Usage | Description |
|-----------|-------|-------------|
| `GlobalThreadID` | Compute shader parameter (1st) | Thread position in the full dispatch |
| `LocalThreadID` | Compute shader parameter (2nd) | Thread position within its threadgroup |
| `ThreadGroupID` | Compute shader parameter (3rd) | Threadgroup position in the dispatch |

### Examples

```omegasl
vertex VertexRaster myVert(uint vid : VertexID){ ... }

compute(x=64,y=1,z=1)
void myKernel(uint3 gid : GlobalThreadID, uint3 lid : LocalThreadID){ ... }

struct Raster internal {
    float4 pos : Position;
    float2 uv : TexCoord;
};
```

## 9. Compilation

### Offline via `omegaslc`

```
omegaslc -t <temp_dir> -o <output.omegasllib> <input.omegasl>
```

Options:
- `-t <dir>` -- temporary directory for intermediate files.
- `-o <path>` -- output `.omegasllib` archive path.
- `--tokens-only` -- dump token stream and exit.

The compiler selects the backend based on the build platform: HLSL on Windows, MSL on macOS/iOS, GLSL on Linux/Android.

### Runtime via `OmegaSLCompiler`

```cpp
OmegaSLCompiler compiler;
compiler.setDevice(device);  // Metal: MTLDevice*, D3D12: ID3D12Device*
auto lib = compiler.compile(source, "shader.omegasl");
```

Runtime compilation parses OmegaSL, generates target source, and compiles in-process via `D3DCompile` / `newLibraryWithSource:` / `shaderc`.

### Constant folding

The compiler performs constant folding before code generation: binary operations (`+`, `-`, `*`, `/`) and prefix negation on numeric literals are evaluated at compile time. For example, `3.14159 * 2.0` becomes `6.28318` in the generated output.

## 10. Backend Mapping

| OmegaSL | HLSL | MSL (Metal) | GLSL (Vulkan) |
|---------|------|-------------|---------------|
| `float` | `float` | `float` | `float` |
| `float2/3/4` | `float2/3/4` | `float2/3/4` | `vec2/3/4` |
| `int2/3/4` | `int2/3/4` | `int2/3/4` | `ivec2/3/4` |
| `uint2/3/4` | `uint2/3/4` | `uint2/3/4` | `uvec2/3/4` |
| `float2x2/3x3/4x4` | `float2x2/3x3/4x4` | `float2x2/3x3/4x4` | `mat2/3/4` |
| `make_float4(...)` | `float4(...)` | `float4(...)` | `vec4(...)` |
| `sample(s,t,c)` | `t.Sample(s,c)` | `t.sample(s,c)` | `texture(sampler2D(t,s),c)` |
| `read(t,c)` | `t.Load(c)` | `t.read(c)` | `texelFetch(t,c,0)` |
| `buffer<T>` (in) | `StructuredBuffer<T>` | `constant T*` | `layout(std430) buffer` |
| `buffer<T>` (out) | `RWStructuredBuffer<T>` | `device T*` | `layout(std430) buffer` |
| `vertex` | `vs_5_0` | `vertex` | `.vert` |
| `fragment` | `ps_5_0` | `fragment` | `.frag` |
| `compute` | `cs_5_0` | `kernel` | `.comp` |
| `hull` | `hs_5_0` | `kernel` | `.tesc` |
| `domain` | `ds_5_0` | `[[patch(...)]] vertex` | `.tese` |
