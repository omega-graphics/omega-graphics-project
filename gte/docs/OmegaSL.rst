=======
OmegaSL
=======

    (Omega Shading Language)

OmegaSL is a cross-platform shading language designed to be used with OmegaGTE.

Syntax:
    StructDecl:
        Declares a data structure to be used on the cpu and the gpu unless declared internal (Struct suffixed with the keyword ``internal`` )
        Internal structures require that all of their fields must annotated with attributes. (For optimal code generation of GLSL and HLSL)

        .. code-block:: omegasl

            struct MyVertex {
                float3 pos;
                float4 color;
            };
            // Used only to transfer data between vertex and fragment shader stages.
            struct MyVertexRasterData internal {
                float4 pos : Position;
                float4 color : Color;
            };

    ResourceDecl:
        Declares a resource to be allocated on the gpu. A resource is suffixed with a unique id (an unsigned 64-bit integer).
        This allows it to be allocated and be accessible to any shader throughout the duration of the program.
        A resource can only be of four of the following types:

            - buffer<struct T>
            - texture1d
            - texture2d
            - texture3d
            - sampler2d
            - sampler3d
            - uint

        Example:

        .. code-block:: omegasl

            struct MyData {
                float4 vec;
            };
            buffer<MyData> data : 0;
            texture2d tex : 1;

    ShaderDecl:
        Declares a shader routine to be executed on the gpu.
        They are prefixed with the keywords ``vertex``, ``fragment`` , or ``compute`` and in many other cases are prefixed with a resource map. (Array syntax placed before.)
        A resource map determines what gpu resources are accessible to the pipeline and what type of access the pipeline has.
        (Determines which type of shader is being declared).

        .. code-block:: omegasl

            struct RasterData internal {
                float4 pos : Position;
                float2 coord;
            };

            texture2d tex : 0;

            [in tex]
            fragment float4 fragShader(RasterData raster){
                sampler2d sampler;
                return sample(sampler,tex);
            }
Builtin-Types:
    There are several builtin data types.
    ``float2``
        Defines a vector with 2 float components.
    ``float3``
        Defines a vector with 3 float components.
    ``float4``
        Defines a vector with 4 float components.
    ``texture2d``
        Defines a 2d texture with four 16bit color channels (Format: RGBA) per sample.
    ``texture3d``
        Defines a 3d texture with four 16bit color channels (Format: RGBA) per sample.
    ``sampler2d``
        Defines a sampler that can sample 2d textures.
        Can either statically or dynamically declared.
    ``sampler3d``
        Defines a sampler that can sample 3d textures.
        Can either statically or dynamically declared.

    Sampler Declaration Example:
        Inline:
            .. code-block:: omegasl


                struct RasterData internal {
                    float4 pos : Position;
                    float2 coord : TexCoord;
                };

                texture2d tex : 1;

                fragment float4 myFragmentFunc(RasterData data){
                    sampler2d<linear> sampler;
                    return sample(sampler,tex,data.coord);
                }

        Static:
            .. code-block:: omegasl


                struct RasterData internal {
                    float4 pos : Position;
                    float2 coord : TexCoord;
                };

                texture2d tex : 1;
                sampler2d<linear> sampler : 2;

                fragment float4 myFragmentFunc(RasterData data){
                    return sample(sampler,tex,data.coord);
                }

Builtin-Functions:
    There are several builtin functions in the OmegaSL language.

    ``float2 make_float2(float x,float y)``
        Constructs a float2 vector.

    ``float2 make_float3(float x,float y,float z)
    float2 make_float3(float2 a,float z)``:
        Constructs a float3 vector.

    ``float2 make_float4(float x,float y,float z,float w)
    float2 make_float4(float2 a,float z,float w)
    float2 make_float4(float3 a,float w)``:
        Constructs a float4 vector.

    ``number dot(vec<number> a,vec<number> b)``:
       Calculates dot product of two vectors

    ``vec<number> cross(vec<number> a,vec<number> b)``:
       Calculates a cross product of two vectors.

    ``float4 sample(sampler2d sampler,texture2d texture,float2 coord)``:
        Samples a texture2d and returns the color at the provided coord.

    ``float4 sample(sampler3d sampler,texture3d texture,float3 coord)``:
        Samples a texture3d and returns the color at the provided coord.

    ``void write(texture2d texture,float2 coord,float4 color)``:
        Samples a texture3d and returns the color at the provided coord.

    ``void write(texture3d texture,float3 coord,float4 color)``:
        Samples a texture3d and returns the color at the provided coord.

Attributes:
    Render Pipeline Attributes:
        ``VertexID``:
            Defines the id of current vertex to draw.
        ``InstanceID``:
            Defines the id of current instance to draw.
        ``Position``:
            Defines the vertex position during the vertex stage in a render pass.
        ``Color``:
            Attributes a vector of 4 components to be used as a fragments color in a render pipeline.
        ``TexCoord``:
            Attributes a vector between 2 and 3 components to be used as coordinate for a 2D or 3D texture in a render pipeline.
    Compute Pipeline Attributes:
        ``GlobalThreadID``:
            The working thread's id in the total number of threads in a compute pipeline.
        ``ThreadGroupID``:
            The current threadgroup's id in the total number of thread groups dispatched in a pipeline.
        ``LocalThreadID``:
            The working thread's id in the total number of threads in its corresponding threadgroup.



Compilation:
    Via ``omegaslc``
        The main compiler for \*.omegasl sources.
        By default it outputs a \*.omegasllib file
        and an interface file (structs.h) in the output dir.

    Via
    .. cpp:class:: OmegaGTE::OmegaSLCompiler

        The runtime interface for handling compilation of OmegaSL shaders.



