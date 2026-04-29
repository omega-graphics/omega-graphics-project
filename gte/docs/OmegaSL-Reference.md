# OmegaSL Language Reference

OmegaSL is a cross-platform shading language that transpiles to HLSL (DirectX), MSL (Metal), and GLSL (Vulkan). The compiler `omegaslc` produces `.omegasllib` archives containing compiled shader bytecode and resource layout metadata.

### Notes:

the writer uses raw size_t directly to disk, which means a library written
   on a 32-bit build would be unreadable by a 64-bit build and vice versa. If that ever matters, pin widths explicitly
  (e.g. uint64_t) on both sides. Not urgent for a single-platform test.

### Known failing tests (Metal backend)

Snapshot of `ctest` failures as of 2026-04-23, after the `discard` / lexer-`%` / cast-parse fixes landed. All are pre-existing issues in codegen and unrelated to the recent parser work.

**1. Float-literal emission stripped trailing `.0`.** ~~Affects `omegasl_compile_math_builtins`, `omegasl_compile_blinn_phong`, `omegasl_compile_sdf_shapes`, `omegasl_compile_pbr_metallic`, `omegasl_compile_post_process`.~~ Fixed.

The actual root cause was not a Sema-level coercion gap — the failing tests already used `0.0` / `1.0` in source. The bug was that all three codegens (`MetalCodeGen`, `HLSLCodeGen`, `GLSLCodeGen`) emitted `f_num` via `ostream << float`, which drops a trailing `.0`. So `0.0` became `0` in the generated shader, and Metal's `max(float, int)` / `clamp(float, int, int)` overloads were flagged as ambiguous. GLSL would have rejected the same code as a hard type error; HLSL would have silently picked the int overload. The fix adds a `formatFloatLit` helper to each backend that appends `.0` when no decimal/exponent is present in the formatted output.

`blinn_phong` exposed a second, independent issue at the same time: its user-defined `saturate(float)` collided with `metal::saturate`. Resolved by mangling all user-defined function names in `MetalCodeGen` with an `osl_user_` prefix so user helpers cannot shadow Metal stdlib symbols. See `OmegaSL-Feature-Gap-Survey.md` §5.1.1 for the cross-backend mapping of `saturate` and the broader name-collision policy.

**2. `texture2d.read(int2(...))` mismatch.** ~~Affects `omegasl_compile_gaussian_blur`.~~ Fixed.

MetalCodeGen lowered `read(tex, coord)` to `tex.read(coord)` and `write(tex, coord, val)` to `tex.write(val, coord)`. Metal's `texture<T>::read` / `::write` take `ushort2`/`uint2` (or the 1d/3d equivalents), not `int2`. OmegaSL code that built the coordinate via `int2(x, y)` produced unresolvable overloads in both paths.

Fix: a `metalUintCoordTypeForTexture` helper in `MetalCodeGen` looks up the texture argument's resolved type (or its `ResourceDecl` for a bare identifier) and returns the matching unsigned coord type — `uint`, `uint2`, or `uint3`. The `BUILTIN_READ` and `BUILTIN_WRITE` emitters wrap the coordinate argument in that cast unconditionally based on the texture's dimensionality. `uint2(uint2_v)` is a no-op, so this is safe for shaders that already use unsigned coords (e.g. `texture_write.omegasl`).

OmegaSL semantics still allow either `intN` or `uintN` for `read`/`write` coords (Sema validates both); the cast is purely a Metal-codegen concern.

**3. Metal hull/domain stages are unsupported.** ~~Affects `omegasl_compile_tessellation`.~~ Held: `MetalCodeGen` now rejects hull/domain with a clean diagnostic; the test is marked `WILL_FAIL` on Apple builds.

The full picture is bigger than a codegen typo. Three layers were broken:
1. **Codegen.** `MetalCodeGen` was emitting `kernel HullOutput triHull(... vid [[vertex_id]])` for `hull` stages — Metal kernels cannot return a user struct and cannot consume `[[vertex_id]]`. The metal compiler rejected this with a confusing error that masked the deeper problem.
2. **Runtime.** `GEMetalPipeline.mm` has no tessellation plumbing — no `tessellationFactorBuffer`, no `tessellationOutputWindingOrder`, no compute pass for patch factors. Even if the codegen produced valid `.metal` source, the runtime had no way to drive a tessellated draw.
3. **Apple's tessellation model is structurally different from D3D's.** OmegaSL's `hull` is one D3D-style function that runs per control point. Metal expects a **compute kernel** that writes per-patch `MTLTriangleTessellationFactorsHalf` / `MTLQuadTessellationFactorsHalf`, then a **post-tessellation vertex** function that consumes `[[patch(...)]]` / `[[patch_id]]` / `[[position_in_patch]]`. Mapping one OmegaSL hull body to two Metal entry points is a real codegen redesign, not a small patch.

Holding pattern (current state):
- `MetalCodeGen` detects `hull`/`domain` `SHADER_DECL` nodes before any file output and prints `error: Metal backend does not support \`hull\`/\`domain\` shaders ('<name>')…` to stderr.
- A `bool hasFatalErrors` flag on `CodeGen` is set; the driver checks it after parsing and exits nonzero. `generateInterfaceAndCompileShader` short-circuits so the metal compiler isn't invoked on a missing source file.
- `omegasl_compile_tessellation` is marked `WILL_FAIL` on `APPLE` in the tests `CMakeLists.txt` — the test now verifies the diagnostic fires rather than that the (nonexistent) Metal pipeline works. HLSL and GLSL backends still compile the same source normally.
- `GTEDEVICE_FEATURE_TESSELLATION_SHADER` is no longer advertised by the Metal device. The runtime no longer claims a feature it cannot deliver.

Future work (not done here):
- Codegen: split a `hull` decl into (a) a compute kernel that writes patch factors and (b) a post-tessellation vertex entry point. The OmegaSL `domain` shader maps onto the patch-vertex stage; `hull` body becomes the factor-computing kernel.
- Runtime: extend `GEMetalRenderPipelineState` to configure tessellation, and add a `dispatchTessellated*` path that runs the factor compute pass before each tessellated draw.
- Metadata: the `omegasl_shader` map needs to expose both Metal entry points produced from one logical hull stage so the runtime can bind them. Likely the cleanest design is per-backend stage-expansion in the shader-map writer rather than leaking the split to the public API.

Re-advertise `GTEDEVICE_FEATURE_TESSELLATION_SHADER` only when all three of these land.

### Cross-backend texture/sampler coord audit

Coord-type expectations differ by backend. OmegaSL accepts either `intN` or `uintN` at the language level; each codegen casts to the form its target API requires. Reference matrix:

| OmegaSL op | HLSL | MSL | GLSL |
|---|---|---|---|
| `sample(s, t, c)` | `t.Sample(s, floatN c)` | `t.sample(s, floatN c)` | `texture(samplerND(t,s), floatN c)` |
| `read(t, c)` | `t.Load(intN+1)` — signed (mip slot) | `t.read(uintN c)` — **unsigned only** | `texelFetch(samplerND, ivecN c, int lod)` — signed |
| `write(t, c, v)` | `RWTextureND[uintN] = v` — unsigned | `t.write(v, uintN c)` — **unsigned only** | `imageStore(imageND, ivecN c, vec4 v)` — signed |

`sample` is consistent (float coords) and needs no per-backend coord casting. `read` and `write` require a backend-specific cast on the coord. The fix in bug 2 covers the Metal side. The remaining gaps surfaced by the audit:

**4. HLSL `BUILTIN_WRITE` emits no coord cast.** Latent — `tex[coord] = val` for a `RWTextureND` operator-`[]` takes `uintN`. fxc accepts `intN` with implicit-conversion warnings (which is why `texture_write.omegasl` passes today on signed coords), but DXC and stricter HLSL configurations can reject it. Fix: in `HLSLCodeGen` BUILTIN_WRITE, resolve the texture type the same way BUILTIN_READ already does and emit `uint`/`uint2`/`uint3` around the coord. The helper from bug 2 has a direct HLSL analogue.

**5. GLSL `BUILTIN_READ` and `BUILTIN_WRITE` hardcode `ivec2`.** Real bug — `GLSLCodeGen` emits `texelFetch(tex, ivec2(coord), 0)` and `imageStore(tex, ivec2(coord), val)` regardless of texture dimensionality. For a `texture1d` this passes an `ivec2` to `texelFetch(sampler1D, int, int)` (compile error); for a `texture3d` it drops the z-axis (compile error or silently-wrong write). No current test exercises 1D/3D textures on GLSL so it ships green, but the codegen is wrong. Fix: in `GLSLCodeGen`, look up the texture dimensionality (mirroring HLSL/Metal) and emit `int` / `ivec2` / `ivec3` to match `texture1d` / `texture2d` / `texture3d`. Also revisit `imageStore` for write: for `texture3d` write, the coord must be `ivec3`.

**6. Sema rejects valid `texture1d.write` / `texture3d.write` coords.** ~~Real bug in `Sema.cpp`.~~ Fixed.

The `BUILTIN_WRITE` validator required the 2nd arg to be `float` for `texture1d` and `float3` for `texture3d` (only `texture2d` correctly accepted `int2`/`uint2`). This contradicted every backend's expectation and the `BUILTIN_READ` rules in the same file. Fix: the `texture1d` arm now requires `int`/`uint`, the `texture3d` arm requires `int3`/`uint3`, and the diagnostic strings match the read-side wording. With this and bug 2's fix in place, `texture1d.write` and `texture3d.write` work end-to-end on Metal for both signed and unsigned coord variants. HLSL inherits the same Sema fix and emits `tex[coord] = val` (works under fxc with implicit conversion; bug 4 still latent for stricter HLSL configs). GLSL still hits bug 5 — `imageStore(tex, ivec2(coord), val)` is hardcoded to `ivec2` regardless of texture dimensionality, so 1D/3D writes won't compile on GLSL until bug 5 lands.


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
42          // int (decimal)
42u         // uint (explicit suffix)
0xFF        // int (hex)
0xFFu       // uint (hex with suffix)
0xFFFFFFFF  // uint (hex that overflows int → auto-promoted to uint)
3.14        // float
3.14f       // float (explicit)
true        // bool
false       // bool
```

- **Hex literals** use the `0x` / `0X` prefix. Digits are `0-9a-fA-F`.
- **`u` / `U` suffix** forces `uint`. A hex literal that does not fit in a signed `int` is also promoted to `uint` automatically.
- **`f` / `F` suffix** forces `float` on a decimal literal.

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

Functions must be declared (or forward-declared) before use. Forward declarations let a function be referenced before its body is defined — useful for mutual recursion or when a helper is defined later in the file:

```omegasl
float other(float x);            // forward declaration — no body, ends in `;`

float first(float x){
    return other(x) * 2.0;       // calling a function defined below
}

float other(float x){
    return x + 1.0;
}
```

A forward declaration must be matched by a later full definition with an identical return type and parameter types. Functions declared before a shader are emitted as helpers in the generated output.

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
float4 color = float4(1.0, 0.0, 0.0, 1.0);
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

`break;` exits the innermost enclosing `for` or `while` loop. `continue;` skips the rest of the current iteration and proceeds to the next. Both must appear inside a loop body — placing them outside a loop produces a target-backend error.

```omegasl
for(int i = 0; i < 100; i++){
    if(i >= limit){
        break;
    }
    if((i % 2) == 0){
        continue;
    }
    sum += (float)i;
}
```

## 6. Expressions

### Binary operators

| Operator | Description |
|----------|-------------|
| `+`, `-`, `*`, `/` | Arithmetic |
| `==`, `!=` | Equality |
| `<`, `<=`, `>`, `>=` | Comparison |
| `&&`, `\|\|` | Logical AND / OR (short-circuit) |
| `&`, `\|`, `^` | Bitwise AND / OR / XOR |
| `<<`, `>>` | Bitwise left / right shift |
| `=` | Assignment |
| `+=`, `-=`, `*=`, `/=` | Arithmetic compound assignment |
| `&=`, `\|=`, `^=`, `<<=`, `>>=` | Bitwise compound assignment |

Scalar-vector operations are supported (e.g. `float4 * float`).

Precedence (highest to lowest): multiplicative (`* /`), additive (`+ -`), shift (`<< >>`), relational (`< <= > >=`), equality (`== !=`), bitwise AND (`&`), bitwise XOR (`^`), bitwise OR (`|`), logical AND (`&&`), logical OR (`||`), assignment and compound assignment. This matches the C family.

### Unary operators

| Operator | Position | Description |
|----------|----------|-------------|
| `-` | Prefix | Negation |
| `!` | Prefix | Logical NOT |
| `~` | Prefix | Bitwise NOT |
| `++` | Prefix/Postfix | Increment |
| `--` | Prefix/Postfix | Decrement |

### Literal coercion

In variable initializers and binary expressions, a numeric scalar **literal** implicitly takes the type of the adjacent scalar. This avoids the need for a cast on every integer constant:

```omegasl
uint a = input[0].value[0];
uint masked = a & 0xFF;    // `0xFF` is an int literal but coerces to uint
uint shifted = a << 2;     // same — `2` coerces to uint
int  i = 0;                // plain int
float f = 1;               // `1` coerces to float
```

Rules:
- **Integer literals** (including hex) coerce to `int`, `uint`, or `float`.
- **Float literals** coerce to `float` only. Writing `int x = 3.14;` is a type error — use `(int)3.14` if that's intended.
- **Non-literal** operands must match exactly. Mixing a non-literal `int` and `uint` in the same expression, or assigning a `float` variable to a `uint`, requires an explicit cast. This catches accidental sign/width confusion that is a real bug most of the time.

### Other expressions

- **Function call**: `func(arg1, arg2)`
- **Member access**: `obj.field`
- **Index access**: `buf[i]`, `vec[0]`
- **Cast**: `(float)intVal` (C-style) or `float(intVal)` (functional). Functional form is supported for the scalar types `int`, `uint`, `float`, and `bool`.
- **Address-of / dereference**: `&val`, `*ptr`

## 7. Builtin Functions

### Vector constructors

```omegasl
float2 v2 = float2(1.0, 2.0);
float3 v3 = float3(1.0, 2.0, 3.0);
float3 v3b = float3(v2, 3.0);         // float2 + float
float4 v4 = float4(1.0, 2.0, 3.0, 4.0);
float4 v4b = float4(v3, 1.0);         // float3 + float
float4 v4c = float4(v2, 3.0, 4.0);    // float2 + float + float
```

Integer and unsigned constructors follow the same pattern: `int2/3/4`, `uint2/3/4`. The `make_*` forms (`make_float2`, `make_int3`, …) remain supported as aliases for the short names.

### Matrix constructors

```omegasl
float2x2 m2 = float2x2(...);
float3x3 m3 = float3x3(...);
float4x4 m4 = float4x4(...);
```

All `make_floatNxM` forms are also accepted as aliases.

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
float4 texel = read(myTexture, int2(x, y));

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
| `Color` | Internal struct field | Inter-stage color varying (vertex→fragment) |
| `Color(N)` | Internal struct field (fragment output) | Render target N (MRT) |
| `TexCoord` | Internal struct field | Texture coordinate |
| `Depth` | Internal struct field (fragment output, `float`) | Per-fragment depth output |
| `FrontFacing` | Fragment shader parameter (`bool`) | True if the fragment is part of a front-facing primitive |
| `SampleIndex` | Fragment shader parameter (`uint`) | Sample index when running per-sample (MSAA) |
| `Coverage` | Fragment shader parameter (`uint`) | Coverage mask for the current fragment (input only) |

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

A fragment shader can return either a single `float4` (which is bound to render
target 0) or an `internal` struct of `Color(N)` / `Depth` fields for MRT or
depth output:

```omegasl
struct GBuffer internal {
    float4 albedo : Color(0);
    float4 normal : Color(1);
    float  depth  : Depth;
};

fragment GBuffer myFrag(Raster r){
    GBuffer o;
    o.albedo = float4(1.0, 1.0, 1.0, 1.0);
    o.normal = float4(0.0, 0.0, 1.0, 0.0);
    o.depth  = 0.5;
    return o;
}
```

Per-fragment scalar inputs are declared as additional fragment-shader
parameters:

```omegasl
fragment float4 myFrag(Raster r, bool ff : FrontFacing, uint si : SampleIndex){
    if(!ff){
        return r.color * 0.5;
    }
    return r.color;
}
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
| `float4(...)` / `make_float4(...)` | `float4(...)` | `float4(...)` | `vec4(...)` |
| `sample(s,t,c)` | `t.Sample(s,c)` | `t.sample(s,c)` | `texture(sampler2D(t,s),c)` |
| `read(t,c)` | `t.Load(c)` | `t.read(c)` | `texelFetch(t,c,0)` |
| `buffer<T>` (in) | `StructuredBuffer<T>` | `constant T*` | `layout(std430) buffer` |
| `buffer<T>` (out) | `RWStructuredBuffer<T>` | `device T*` | `layout(std430) buffer` |
| `vertex` | `vs_5_0` | `vertex` | `.vert` |
| `fragment` | `ps_5_0` | `fragment` | `.frag` |
| `compute` | `cs_5_0` | `kernel` | `.comp` |
| `hull` | `hs_5_0` | `kernel` | `.tesc` |
| `domain` | `ds_5_0` | `[[patch(...)]] vertex` | `.tese` |

## 11. Feature Status

A snapshot of what is implemented, what is partial, and what is intentionally absent. Useful when planning shaders around the language's current capabilities.

### Working

- **Preprocessor** — `#define`, `#ifdef`, `#ifndef`, `#endif`, `#include` (depth 10).
- **Scalar / vector / matrix types** — `bool`, `int`, `uint`, `float`; `int2/3/4`, `uint2/3/4`, `float2/3/4`; `float2x2/3x3/4x4`.
- **Swizzles and index access** — `.x/.y/.z/.w`, `.xy`, `.xyz`, `.xyzw`, `v[i]`.
- **Fixed-size arrays** — supported in variable declarations (`float arr[4];`).
- **Structs** — plain data structs and `internal` structs (with attribute fields) for inter-stage data.
- **Structured buffers** — `buffer<T>` with `in` / `out` / `inout` access.
- **Textures / samplers** — `texture1d/2d/3d`, `sampler1d/2d/3d`, static samplers with filter + address mode configuration.
- **User-defined functions** — emitted as helpers ahead of shader entry points.
- **Shader stages** — `vertex`, `fragment`, `compute`, `hull`, `domain` across all three backends.
- **Fragment outputs** — single `float4` return (target 0) or `internal` struct return with `Color(N)` MRT outputs and an optional `Depth` field.
- **Per-fragment scalar inputs** — `bool : FrontFacing`, `uint : SampleIndex`, `uint : Coverage` (input only) as fragment-shader parameters.
- **Statements** — variable declaration, assignment (`=`, `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `<<=`, `>>=`), `return` (bare and with value), `if` / `else if` / `else`, `for`, `while`, **`break`**, **`continue`**.
- **Operators** — arithmetic (`+ - * /`), comparison (`< <= > >=`), equality (`== !=`), logical (`&& || !`), bitwise binary (`& | ^ << >>`), bitwise unary (`~`), prefix/postfix `++`/`--`, prefix `-`, C-style cast, address-of / dereference.
- **Numeric literals** — decimal `int`, hex `int` (`0xFF`), `uint` suffix (`42u`, `0xFFu`), `float` suffix (`3.14f`), auto-promotion of oversized hex literals to `uint`, and literal coercion between numeric scalars (int/uint/float literals adapt to the target scalar type in variable initializers and binary expressions; float literals only coerce to float).
- **Builtins** — vector and matrix constructors, `dot`, `cross`, `sample`, `read`, `write`, full math intrinsic set (1/2/3-argument).
- **Constant folding** — literal arithmetic is folded before code generation.
- **Three backends** — HLSL (`D3DCompile`), MSL (`newLibraryWithSource:`), GLSL (`shaderc`).
- **Runtime and offline compilation** — `omegaslc` CLI and in-process `OmegaSLCompiler`.

### Partial / caveats

- **`break` / `continue` loop-context enforcement** — the parser and codegen accept these statements anywhere. When used outside a loop the target backend (fxc / metal / glslang) is what ultimately rejects the shader. Frontend Sema does not yet produce a friendly OmegaSL-level diagnostic for this case.
- **Shift tokens and nested templates** — the lexer eagerly tokenizes `<<` and `>>` as shift operators. `buffer<buffer<T>>` written without a space between the two `>` characters will mis-lex. Nested buffer templates are not currently supported, so this is a theoretical hazard today; if nesting is ever added, users will need to write `> >` with a space.
- **Non-literal numeric coercion** — only literals coerce implicitly between numeric scalars. Mixing a non-literal `int` and `uint`, or assigning a non-literal across types, still requires an explicit cast. This is intentional to catch sign/width confusion bugs until a dedicated promotion pass lands.
- **Function declarations** — forward declarations are now supported (`return_ty name(params);`). A forward-declared function must still be given a full definition in the same translation unit whose signature matches exactly.
- **Array types** — only valid in local variable declarations. Function parameters, struct fields, and return types cannot be array types.
- **Constant folding** — only folds literal-on-literal binary ops and unary negation; folded identifiers / `#define` constants are not propagated.
- **Static sampler properties** — only `filter`, `address_mode`, and `max_anisotropy` are recognized. No LOD bias, comparison functions, or border colors.

### Not implemented

- **`double` / 64-bit floats** — intentionally omitted. Metal has no `double`, and GLSL requires extensions. Use `float`.
- **`do { } while(...)` loops** — no backend emission.
- **`switch` / `case` / `default` statements** — parse and codegen are absent.
- **Ternary `?:`** — not implemented.
- **Function overloading / default arguments** — a function name uniquely identifies a function.
- **Generic / template functions** — not implemented; builtin math functions are the only polymorphic calls.
- **Atomic operations** — no `atomic_add` / `atomic_exchange` / etc. for compute shaders.
- **Threadgroup / shared memory** — no `groupshared` / `threadgroup` storage class.
- **Barrier intrinsics** — no `GroupMemoryBarrierWithGroupSync` / `threadgroup_barrier` / `barrier`.
- **Derivative intrinsics** — no `ddx` / `ddy` / `fwidth`.
- **Push constants / root constants** — only structured buffers and textures are exposed as resources.
- **Specialization constants** — no runtime specialization path.
- **Raytracing stages** — no `raygeneration`, `closesthit`, `miss`, etc.
- **Mesh / task shaders** — not implemented.
