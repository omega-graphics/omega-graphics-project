## OmegaSL Language Reference (Draft)

### 1. Types

- **Scalars**: `void`, `bool`, `int`, `uint`, `float`, `double`.
- **Vectors**:
  - Integer: `int2`, `int3`, `int4`.
  - Unsigned: `uint2`, `uint3`, `uint4`.
  - Floating-point: `float2`, `float3`, `float4`.
- **Matrices**:
  - `float2x2`, `float3x3`, `float4x4`.
- **Resources**:
  - Buffers: `buffer<T>`.
  - Textures: `texture1d<T>`, `texture2d<T>`, `texture3d<T>`.
  - Samplers: `sampler1d`, `sampler2d`, `sampler3d`.

### 2. Declarations

- **Structs**:
  - `struct Name { Type field; };`
  - Optional `internal` keyword for shader-internal structs.
- **Resources**:
  - `resource Type name : register(N) [static];`
  - Optional static sampler descriptor for `sampler*` resources.
- **Shaders**:
  - `shader vertex Name(...) { ... }`
  - `shader fragment Name(...) { ... }`
  - `shader compute Name(...) [numthreads(x,y,z)] { ... }`
  - `shader hull Name(...) { ... }`
  - `shader domain Name(...) { ... }`
- **Functions**:
  - `func ReturnType Name(params) { ... }`

### 3. Statements

- **Variable declaration**:
  - `Type name;`
  - `Type name = expr;`
- **Return**:
  - `return;`
  - `return expr;`
- **Control flow**:
  - `if (cond) { ... } else if (cond) { ... } else { ... }`
  - `for (init; cond; incr) { ... }`
  - `while (cond) { ... }`

### 4. Expressions

- **Arithmetic / comparison / logical**:
  - `+`, `-`, `*`, `/`, `%`
  - `==`, `!=`, `<`, `<=`, `>`, `>=`
  - `&&`, `||`, `!`
- **Calls**:
  - `func(args...)`
- **Member access**:
  - `object.field`
- **Indexing**:
  - `array[index]`
- **Unary**:
  - Prefix: `++x`, `--x`, `!x`, `-x`
  - Postfix: `x++`, `x--`
- **Casts**:
  - C-style / functional: `(Type)expr` or `Type(expr)`.

### 5. Builtins

- **Constructors**:
  - `make_float2/3/4`, `make_int2/3/4`, `make_uint2/3/4`.
  - `make_float2x2/3x3/4x4`.
- **Math passthroughs**:
  - `sin`, `cos`, `sqrt`, etc. (mapped to backend intrinsics).
- **Vector ops**:
  - `dot(a, b)`, `cross(a, b)`.
- **Resource ops**:
  - `sample(sampler, texture, coord)`
  - `read(texture, coord)`
  - `write(texture, coord, value)`

### 6. Attributes

- **Vertex / fragment / compute**:
  - `VertexID`, `Position`, `Color`, `TexCoord`, `InstanceID`.
  - `GlobalThreadID`, `LocalThreadID`, `ThreadGroupID`.

Attributes are attached to struct fields or parameters using square brackets, for example:

```text
struct VSInput {
    float3 position [Position];
    float2 uv       [TexCoord];
};
```

### 7. Resource Maps

- Shader parameters can declare resource maps using `[in name]`, `[out name]`, `[inout name]` to bind resources to shaders.

Example:

```text
shader fragment FS(
    float2 uv [TexCoord]
) [in tex0, in samp0] {
    return sample(samp0, tex0, uv);
}
```

### 8. Static Samplers

- Static samplers are declared as resources with `static` and a sampler descriptor:

```text
resource sampler2d linearClamp : register(0) static {
    filter = linear;
    addressU = wrap;
    addressV = wrap;
};
```

Backends map these to platform-specific static sampler representations.

