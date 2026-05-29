#include "AST.h"

#include "Toks.def"

namespace omegasl::ast {

    namespace builtins {

        bool builtinsOn = false;

        Scope *global_scope;
        Type *void_type;
        Type *bool_type;
        Type *bool2_type;
        Type *bool3_type;
        Type *bool4_type;
        Type *int_type;
        Type *int2_type;
        Type *int3_type;
        Type *int4_type;

        Type *float_type;
        Type *float2_type;
        Type *float3_type;
        Type *float4_type;
        Type *float2x2_type;
        Type *float3x3_type;
        Type *float4x4_type;
        Type *float2x3_type;
        Type *float2x4_type;
        Type *float3x2_type;
        Type *float3x4_type;
        Type *float4x2_type;
        Type *float4x3_type;
        /// §12.2 follow-up — integer matrices (array-lowered per backend).
        Type *int2x2_type;
        Type *int3x3_type;
        Type *int4x4_type;
        Type *int2x3_type;
        Type *int2x4_type;
        Type *int3x2_type;
        Type *int3x4_type;
        Type *int4x2_type;
        Type *int4x3_type;
        Type *uint2x2_type;
        Type *uint3x3_type;
        Type *uint4x4_type;
        Type *uint2x3_type;
        Type *uint2x4_type;
        Type *uint3x2_type;
        Type *uint3x4_type;
        Type *uint4x2_type;
        Type *uint4x3_type;

        Type *uint_type;
        Type *uint2_type;
        Type *uint3_type;
        Type *uint4_type;

        /// §4.1 16-bit numerics
        Type *half_type;
        Type *half2_type;
        Type *half3_type;
        Type *half4_type;
        Type *short_type;
        Type *short2_type;
        Type *short3_type;
        Type *short4_type;
        Type *ushort_type;
        Type *ushort2_type;
        Type *ushort3_type;
        Type *ushort4_type;
        /// §4.2 64-bit ints
        Type *long_type;
        Type *long2_type;
        Type *long3_type;
        Type *long4_type;
        Type *ulong_type;
        Type *ulong2_type;
        Type *ulong3_type;
        Type *ulong4_type;

        Type *buffer_type;
        Type *uniform_type;
        Type *texture1d_type;
        Type *texture2d_type;
        Type *texture3d_type;
        Type *texture1d_array_type;
        Type *texture2d_array_type;
        Type *texturecube_type;
        Type *texturecube_array_type;
        Type *texture2d_ms_type;
        Type *texture2d_ms_array_type;

        Type *sampler1d_type;
        Type *sampler2d_type;
        Type *sampler3d_type;
        Type *samplercube_type;

        FuncType *make_float2;
        FuncType *make_bool2;
        FuncType *make_bool3;
        FuncType *make_bool4;
        FuncType *make_float3;
        FuncType *make_float4;
        FuncType *make_int2;
        FuncType *make_int3;
        FuncType *make_int4;
        FuncType *make_uint2;
        FuncType *make_uint3;
        FuncType *make_uint4;
        FuncType *make_half2;
        FuncType *make_half3;
        FuncType *make_half4;
        FuncType *make_short2;
        FuncType *make_short3;
        FuncType *make_short4;
        FuncType *make_ushort2;
        FuncType *make_ushort3;
        FuncType *make_ushort4;
        FuncType *make_long2;
        FuncType *make_long3;
        FuncType *make_long4;
        FuncType *make_ulong2;
        FuncType *make_ulong3;
        FuncType *make_ulong4;
        FuncType *make_float2x2;
        FuncType *make_float3x3;
        FuncType *make_float4x4;
        FuncType *make_float2x3;
        FuncType *make_float2x4;
        FuncType *make_float3x2;
        FuncType *make_float3x4;
        FuncType *make_float4x2;
        FuncType *make_float4x3;

        FuncType *dot;
        FuncType *cross;

        FuncType *write;
        FuncType *read;

        FuncType *sample;
        FuncType *sampleLOD;
        FuncType *sampleBias;
        FuncType *sampleGrad;
        FuncType *gather;
        FuncType *gatherRed;
        FuncType *gatherGreen;
        FuncType *gatherBlue;
        FuncType *gatherAlpha;
        FuncType *calculateLOD;
        FuncType *getDimensions;


        void Initialize(){
            if(!builtinsOn){
                builtinsOn = true;
                global_scope = Scope::Create("global",nullptr);
                void_type = new Type{KW_TY_VOID,global_scope};
                bool_type = new Type{KW_TY_BOOL,global_scope};
                bool2_type = new Type{KW_TY_BOOL2,global_scope};
                bool3_type = new Type{KW_TY_BOOL3,global_scope};
                bool4_type = new Type{KW_TY_BOOL4,global_scope};
                int_type = new Type{KW_TY_INT,global_scope};
                int2_type = new Type{KW_TY_INT2,global_scope};
                int3_type = new Type{KW_TY_INT3,global_scope};
                int4_type = new Type{KW_TY_INT4,global_scope};

                float_type = new Type{KW_TY_FLOAT,global_scope};
                float2_type = new Type{KW_TY_FLOAT2,global_scope};
                float3_type = new Type{KW_TY_FLOAT3,global_scope};
                float4_type = new Type{KW_TY_FLOAT4,global_scope};
                float2x2_type = new Type{KW_TY_FLOAT2X2,global_scope};
                float2x3_type = new Type{KW_TY_FLOAT2X3,global_scope};
                float2x4_type = new Type{KW_TY_FLOAT2X4,global_scope};
                float3x2_type = new Type{KW_TY_FLOAT3X2,global_scope};
                float3x3_type = new Type{KW_TY_FLOAT3X3,global_scope};
                float3x4_type = new Type{KW_TY_FLOAT3X4,global_scope};
                float4x2_type = new Type{KW_TY_FLOAT4X2,global_scope};
                float4x3_type = new Type{KW_TY_FLOAT4X3,global_scope};
                float4x4_type = new Type{KW_TY_FLOAT4X4,global_scope};
                /// §12.2 follow-up — integer matrices (array-lowered).
                int2x2_type = new Type{KW_TY_INT2X2,global_scope};
                int2x3_type = new Type{KW_TY_INT2X3,global_scope};
                int2x4_type = new Type{KW_TY_INT2X4,global_scope};
                int3x2_type = new Type{KW_TY_INT3X2,global_scope};
                int3x3_type = new Type{KW_TY_INT3X3,global_scope};
                int3x4_type = new Type{KW_TY_INT3X4,global_scope};
                int4x2_type = new Type{KW_TY_INT4X2,global_scope};
                int4x3_type = new Type{KW_TY_INT4X3,global_scope};
                int4x4_type = new Type{KW_TY_INT4X4,global_scope};
                uint2x2_type = new Type{KW_TY_UINT2X2,global_scope};
                uint2x3_type = new Type{KW_TY_UINT2X3,global_scope};
                uint2x4_type = new Type{KW_TY_UINT2X4,global_scope};
                uint3x2_type = new Type{KW_TY_UINT3X2,global_scope};
                uint3x3_type = new Type{KW_TY_UINT3X3,global_scope};
                uint3x4_type = new Type{KW_TY_UINT3X4,global_scope};
                uint4x2_type = new Type{KW_TY_UINT4X2,global_scope};
                uint4x3_type = new Type{KW_TY_UINT4X3,global_scope};
                uint4x4_type = new Type{KW_TY_UINT4X4,global_scope};

                uint_type = new Type{KW_TY_UINT,global_scope};
                uint2_type = new Type{KW_TY_UINT2,global_scope};
                uint3_type = new Type{KW_TY_UINT3,global_scope};
                uint4_type = new Type{KW_TY_UINT4,global_scope};

                /// §4.1 16-bit numerics. The Type struct carries no
                /// width metadata — the per-backend `writeTypeName`
                /// switches on the pointer identity, so the only thing
                /// that matters here is that each name resolves to a
                /// unique builtin Type *.
                half_type = new Type{KW_TY_HALF,global_scope};
                half2_type = new Type{KW_TY_HALF2,global_scope};
                half3_type = new Type{KW_TY_HALF3,global_scope};
                half4_type = new Type{KW_TY_HALF4,global_scope};
                short_type = new Type{KW_TY_SHORT,global_scope};
                short2_type = new Type{KW_TY_SHORT2,global_scope};
                short3_type = new Type{KW_TY_SHORT3,global_scope};
                short4_type = new Type{KW_TY_SHORT4,global_scope};
                ushort_type = new Type{KW_TY_USHORT,global_scope};
                ushort2_type = new Type{KW_TY_USHORT2,global_scope};
                ushort3_type = new Type{KW_TY_USHORT3,global_scope};
                ushort4_type = new Type{KW_TY_USHORT4,global_scope};

                /// §4.2 64-bit ints.
                long_type = new Type{KW_TY_LONG,global_scope};
                long2_type = new Type{KW_TY_LONG2,global_scope};
                long3_type = new Type{KW_TY_LONG3,global_scope};
                long4_type = new Type{KW_TY_LONG4,global_scope};
                ulong_type = new Type{KW_TY_ULONG,global_scope};
                ulong2_type = new Type{KW_TY_ULONG2,global_scope};
                ulong3_type = new Type{KW_TY_ULONG3,global_scope};
                ulong4_type = new Type{KW_TY_ULONG4,global_scope};

                buffer_type = new Type{KW_TY_BUFFER,global_scope,true,{"type"}};
                uniform_type = new Type{KW_TY_UNIFORM,global_scope,true,{"type"}};
                texture1d_type = new Type{KW_TY_TEXTURE1D,global_scope};
                texture2d_type = new Type{KW_TY_TEXTURE2D,global_scope};
                texture3d_type = new Type{KW_TY_TEXTURE3D,global_scope};
                texture1d_array_type = new Type{KW_TY_TEXTURE1D_ARRAY,global_scope};
                texture2d_array_type = new Type{KW_TY_TEXTURE2D_ARRAY,global_scope};
                texturecube_type = new Type{KW_TY_TEXTURECUBE,global_scope};
                texturecube_array_type = new Type{KW_TY_TEXTURECUBE_ARRAY,global_scope};
                texture2d_ms_type = new Type{KW_TY_TEXTURE2D_MS,global_scope};
                texture2d_ms_array_type = new Type{KW_TY_TEXTURE2D_MS_ARRAY,global_scope};

                sampler1d_type = new Type{KW_TY_SAMPLER1D,global_scope};
                sampler2d_type = new Type{KW_TY_SAMPLER2D,global_scope};
                sampler3d_type = new Type{KW_TY_SAMPLER3D,global_scope};
                samplercube_type = new Type{KW_TY_SAMPLERCUBE,global_scope};

                make_float2 = new FuncType{BUILTIN_MAKE_FLOAT2,global_scope,true,{},{

                    },TypeExpr::Create(float2_type)};

                make_float3 = new FuncType{BUILTIN_MAKE_FLOAT3,global_scope,true,{},{

                    },TypeExpr::Create(float3_type)};

                make_float4 = new FuncType{BUILTIN_MAKE_FLOAT4,global_scope,true,{},{

                    },TypeExpr::Create(float4_type)};

                make_bool2 = new FuncType{BUILTIN_MAKE_BOOL2,global_scope,true,{},{},TypeExpr::Create(bool2_type)};
                make_bool3 = new FuncType{BUILTIN_MAKE_BOOL3,global_scope,true,{},{},TypeExpr::Create(bool3_type)};
                make_bool4 = new FuncType{BUILTIN_MAKE_BOOL4,global_scope,true,{},{},TypeExpr::Create(bool4_type)};

                make_int2 = new FuncType{BUILTIN_MAKE_INT2,global_scope,true,{},{},TypeExpr::Create(int2_type)};
                make_int3 = new FuncType{BUILTIN_MAKE_INT3,global_scope,true,{},{},TypeExpr::Create(int3_type)};
                make_int4 = new FuncType{BUILTIN_MAKE_INT4,global_scope,true,{},{},TypeExpr::Create(int4_type)};
                make_uint2 = new FuncType{BUILTIN_MAKE_UINT2,global_scope,true,{},{},TypeExpr::Create(uint2_type)};
                make_uint3 = new FuncType{BUILTIN_MAKE_UINT3,global_scope,true,{},{},TypeExpr::Create(uint3_type)};
                make_uint4 = new FuncType{BUILTIN_MAKE_UINT4,global_scope,true,{},{},TypeExpr::Create(uint4_type)};
                make_half2 = new FuncType{BUILTIN_MAKE_HALF2,global_scope,true,{},{},TypeExpr::Create(half2_type)};
                make_half3 = new FuncType{BUILTIN_MAKE_HALF3,global_scope,true,{},{},TypeExpr::Create(half3_type)};
                make_half4 = new FuncType{BUILTIN_MAKE_HALF4,global_scope,true,{},{},TypeExpr::Create(half4_type)};
                make_short2 = new FuncType{BUILTIN_MAKE_SHORT2,global_scope,true,{},{},TypeExpr::Create(short2_type)};
                make_short3 = new FuncType{BUILTIN_MAKE_SHORT3,global_scope,true,{},{},TypeExpr::Create(short3_type)};
                make_short4 = new FuncType{BUILTIN_MAKE_SHORT4,global_scope,true,{},{},TypeExpr::Create(short4_type)};
                make_ushort2 = new FuncType{BUILTIN_MAKE_USHORT2,global_scope,true,{},{},TypeExpr::Create(ushort2_type)};
                make_ushort3 = new FuncType{BUILTIN_MAKE_USHORT3,global_scope,true,{},{},TypeExpr::Create(ushort3_type)};
                make_ushort4 = new FuncType{BUILTIN_MAKE_USHORT4,global_scope,true,{},{},TypeExpr::Create(ushort4_type)};
                make_long2 = new FuncType{BUILTIN_MAKE_LONG2,global_scope,true,{},{},TypeExpr::Create(long2_type)};
                make_long3 = new FuncType{BUILTIN_MAKE_LONG3,global_scope,true,{},{},TypeExpr::Create(long3_type)};
                make_long4 = new FuncType{BUILTIN_MAKE_LONG4,global_scope,true,{},{},TypeExpr::Create(long4_type)};
                make_ulong2 = new FuncType{BUILTIN_MAKE_ULONG2,global_scope,true,{},{},TypeExpr::Create(ulong2_type)};
                make_ulong3 = new FuncType{BUILTIN_MAKE_ULONG3,global_scope,true,{},{},TypeExpr::Create(ulong3_type)};
                make_ulong4 = new FuncType{BUILTIN_MAKE_ULONG4,global_scope,true,{},{},TypeExpr::Create(ulong4_type)};
                make_float2x2 = new FuncType{BUILTIN_MAKE_FLOAT2X2,global_scope,true,{},{},TypeExpr::Create(float2x2_type)};
                make_float2x3 = new FuncType{BUILTIN_MAKE_FLOAT2X3,global_scope,true,{},{},TypeExpr::Create(float2x3_type)};
                make_float2x4 = new FuncType{BUILTIN_MAKE_FLOAT2X4,global_scope,true,{},{},TypeExpr::Create(float2x4_type)};
                make_float3x2 = new FuncType{BUILTIN_MAKE_FLOAT3X2,global_scope,true,{},{},TypeExpr::Create(float3x2_type)};
                make_float3x3 = new FuncType{BUILTIN_MAKE_FLOAT3X3,global_scope,true,{},{},TypeExpr::Create(float3x3_type)};
                make_float3x4 = new FuncType{BUILTIN_MAKE_FLOAT3X4,global_scope,true,{},{},TypeExpr::Create(float3x4_type)};
                make_float4x2 = new FuncType{BUILTIN_MAKE_FLOAT4X2,global_scope,true,{},{},TypeExpr::Create(float4x2_type)};
                make_float4x3 = new FuncType{BUILTIN_MAKE_FLOAT4X3,global_scope,true,{},{},TypeExpr::Create(float4x3_type)};
                make_float4x4 = new FuncType{BUILTIN_MAKE_FLOAT4X4,global_scope,true,{},{},TypeExpr::Create(float4x4_type)};

                dot = new FuncType {BUILTIN_DOT,global_scope,true,{},{
                    {"a",TypeExpr::Create("MATRIX_TYPE")},
                    {"b",TypeExpr::Create("MATRIX_TYPE")}
                    },TypeExpr::Create("SCALAR_TYPE")};

                cross = new FuncType{BUILTIN_CROSS,global_scope,true,{},{
                    {"a",TypeExpr::Create("VECTOR_TYPE")},
                    {"b",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create("VECTOR_TYPE")};

                write = new FuncType {BUILTIN_WRITE,global_scope,true,{},{
                    {"dest",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")},
                    {"data",TypeExpr::Create("VECTOR_TYPE")},
                    },TypeExpr::Create(void_type)};

                sample = new FuncType {BUILTIN_SAMPLE,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                /// `sampleLOD/Bias/Grad` and `gather*` reuse the (sampler, texture,
                /// coord, ...) shape; the trailing args are validated per-builtin
                /// in Sema rather than encoded in the FuncType field map (which
                /// is just a placeholder — the real check is the per-builtin
                /// branch in `performSemForExpr`, mirroring how `sample` works).
                sampleLOD = new FuncType {BUILTIN_SAMPLE_LOD,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")},
                    {"lod",TypeExpr::Create(float_type)}
                    },TypeExpr::Create(float4_type)};

                sampleBias = new FuncType {BUILTIN_SAMPLE_BIAS,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")},
                    {"bias",TypeExpr::Create(float_type)}
                    },TypeExpr::Create(float4_type)};

                sampleGrad = new FuncType {BUILTIN_SAMPLE_GRAD,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")},
                    {"ddxArg",TypeExpr::Create("VECTOR_TYPE")},
                    {"ddyArg",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                gather = new FuncType {BUILTIN_GATHER,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                gatherRed = new FuncType {BUILTIN_GATHER_RED,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                gatherGreen = new FuncType {BUILTIN_GATHER_GREEN,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                gatherBlue = new FuncType {BUILTIN_GATHER_BLUE,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                gatherAlpha = new FuncType {BUILTIN_GATHER_ALPHA,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                read = new FuncType{BUILTIN_READ,global_scope,true,{},{
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};

                /// §2.3 Phase B — `calculateLOD(sampler, texture, coord)`
                /// returns the LOD the hardware would choose for `coord`,
                /// as a scalar `float`. Reuses the (sampler, texture, coord)
                /// triple validation that `sample` uses.
                calculateLOD = new FuncType{BUILTIN_CALCULATE_LOD,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float_type)};

                /// §2.3 Phase B — `getDimensions(texture, lod)` queries the
                /// mip-level dimensions. The return type is shape-dependent
                /// (`uint`/`uint2`/`uint3` by texture rank) and is synthesized
                /// per-call in Sema, so the FuncType return type here is only
                /// a placeholder. `lod` is required (pass `0` for the base
                /// level).
                getDimensions = new FuncType{BUILTIN_GET_DIMENSIONS,global_scope,true,{},{
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"lod",TypeExpr::Create(uint_type)}
                    },TypeExpr::Create(uint2_type)};
            }
        }

        void Cleanup(){
            if(builtinsOn){
                builtinsOn = false;
                delete global_scope;
                delete void_type;
                delete bool_type;
                delete bool2_type;
                delete bool3_type;
                delete bool4_type;
                delete int_type;
                delete int2_type;
                delete int3_type;
                delete int4_type;
                delete float_type;
                delete float2_type;
                delete float3_type;
                delete float4_type;
                delete float2x2_type;
                delete float2x3_type;
                delete float2x4_type;
                delete float3x2_type;
                delete float3x3_type;
                delete float3x4_type;
                delete float4x2_type;
                delete float4x3_type;
                delete float4x4_type;
                /// §12.2 follow-up — integer matrices.
                delete int2x2_type;
                delete int2x3_type;
                delete int2x4_type;
                delete int3x2_type;
                delete int3x3_type;
                delete int3x4_type;
                delete int4x2_type;
                delete int4x3_type;
                delete int4x4_type;
                delete uint2x2_type;
                delete uint2x3_type;
                delete uint2x4_type;
                delete uint3x2_type;
                delete uint3x3_type;
                delete uint3x4_type;
                delete uint4x2_type;
                delete uint4x3_type;
                delete uint4x4_type;
                delete uint_type;
                delete uint2_type;
                delete uint3_type;
                delete uint4_type;
                delete half_type;
                delete half2_type;
                delete half3_type;
                delete half4_type;
                delete short_type;
                delete short2_type;
                delete short3_type;
                delete short4_type;
                delete ushort_type;
                delete ushort2_type;
                delete ushort3_type;
                delete ushort4_type;
                delete long_type;
                delete long2_type;
                delete long3_type;
                delete long4_type;
                delete ulong_type;
                delete ulong2_type;
                delete ulong3_type;
                delete ulong4_type;
                delete buffer_type;
                delete uniform_type;
                delete texture1d_type;
                delete texture2d_type;
                delete texture3d_type;
                delete texture1d_array_type;
                delete texture2d_array_type;
                delete texturecube_type;
                delete texturecube_array_type;
                delete texture2d_ms_type;
                delete texture2d_ms_array_type;
                delete sampler1d_type;
                delete sampler2d_type;
                delete sampler3d_type;
                delete samplercube_type;
                delete make_float2;
                delete make_float3;
                delete make_float4;
                delete make_bool2;
                delete make_bool3;
                delete make_bool4;
                delete make_int2;
                delete make_int3;
                delete make_int4;
                delete make_uint2;
                delete make_uint3;
                delete make_uint4;
                delete make_half2;
                delete make_half3;
                delete make_half4;
                delete make_short2;
                delete make_short3;
                delete make_short4;
                delete make_ushort2;
                delete make_ushort3;
                delete make_ushort4;
                delete make_long2;
                delete make_long3;
                delete make_long4;
                delete make_ulong2;
                delete make_ulong3;
                delete make_ulong4;
                delete make_float2x2;
                delete make_float2x3;
                delete make_float2x4;
                delete make_float3x2;
                delete make_float3x3;
                delete make_float3x4;
                delete make_float4x2;
                delete make_float4x3;
                delete make_float4x4;
                delete dot;
                delete cross;
                delete write;
                delete read;
                delete sample;
                delete sampleLOD;
                delete sampleBias;
                delete sampleGrad;
                delete gather;
                delete gatherRed;
                delete gatherGreen;
                delete gatherBlue;
                delete gatherAlpha;
                delete calculateLOD;
                delete getDimensions;
            }
        }

    }

    Scope *Scope::Create(OmegaCommon::StrRef name, Scope *parent) {
        return new Scope{name,parent};
    }

    bool LiteralExpr::isFloat() const {
        return f_num.has_value();
    }

    bool LiteralExpr::isDouble() const {
        return d_num.has_value();
    }

    bool LiteralExpr::isInt() const {
        return i_num.has_value();
    }

    bool LiteralExpr::isUint() const {
        return ui_num.has_value();
    }

    bool LiteralExpr::isLong() const {
        return i64_num.has_value();
    }

    bool LiteralExpr::isUlong() const {
        return ui64_num.has_value();
    }

    bool LiteralExpr::isBool() const {
        return b_val.has_value();
    }

    bool LiteralExpr::isString() const {
        return str.has_value();
    }

    bool Scope::isParentScope(ast::Scope *scope) const {
        ast::Scope *current = parent;
        bool success = false;
        while(current != nullptr){
            current = parent->parent;
            if(scope == current){
                success = true;
                break;
            }
        }
        return success;
    }

    TypeExpr *TypeExpr::Create(OmegaCommon::StrRef name, bool pointer,bool hasTypeArgs,OmegaCommon::Vector<TypeExpr *> * args) {
        if(hasTypeArgs){
            return new TypeExpr {name, pointer,hasTypeArgs,*args};
        }
        else {
            return new TypeExpr {name,pointer,hasTypeArgs};
        }

    }

    TypeExpr *TypeExpr::Create(Type *type, bool pointer) {
        return new TypeExpr{type->name, pointer};
    }

    bool TypeExpr::compare(TypeExpr *other) {
       return (pointer == other->pointer) && (name == other->name);
    }

    TypeExpr::~TypeExpr() {
        if(hasTypeArgs)
            for(auto el : args){
                delete el;
            }
    }

    OmegaCommon::StrRef canonicalBuiltinAlias(OmegaCommon::StrRef name) {
        if(name == BUILTIN_MOD) return BUILTIN_FMOD;
        if(name == BUILTIN_MAD) return BUILTIN_FMA;
        return name;
    }

    bool isReservedBuiltinName(OmegaCommon::StrRef name) {
        /// One set, spelled from the `BUILTIN_*` macros so it tracks the
        /// dispatch in `Sema::performSemForExpr` and the `builtinFunctionMap`
        /// constructor argument. `transpose` / `determinant` are matched by
        /// string literal in Sema (no macro), so they appear as literals here
        /// too. Aliases (`mod`/`mad`) are reserved alongside their canonical
        /// spellings — a user cannot define either form.
        static const std::set<std::string> reserved = {
            /// Vector / matrix constructors.
            BUILTIN_MAKE_FLOAT2, BUILTIN_MAKE_FLOAT3, BUILTIN_MAKE_FLOAT4,
            BUILTIN_MAKE_BOOL2, BUILTIN_MAKE_BOOL3, BUILTIN_MAKE_BOOL4,
            BUILTIN_MAKE_INT2, BUILTIN_MAKE_INT3, BUILTIN_MAKE_INT4,
            BUILTIN_MAKE_UINT2, BUILTIN_MAKE_UINT3, BUILTIN_MAKE_UINT4,
            BUILTIN_MAKE_HALF2, BUILTIN_MAKE_HALF3, BUILTIN_MAKE_HALF4,
            BUILTIN_MAKE_SHORT2, BUILTIN_MAKE_SHORT3, BUILTIN_MAKE_SHORT4,
            BUILTIN_MAKE_USHORT2, BUILTIN_MAKE_USHORT3, BUILTIN_MAKE_USHORT4,
            BUILTIN_MAKE_LONG2, BUILTIN_MAKE_LONG3, BUILTIN_MAKE_LONG4,
            BUILTIN_MAKE_ULONG2, BUILTIN_MAKE_ULONG3, BUILTIN_MAKE_ULONG4,
            BUILTIN_MAKE_FLOAT2X2, BUILTIN_MAKE_FLOAT3X3, BUILTIN_MAKE_FLOAT4X4,
            BUILTIN_MAKE_FLOAT2X3, BUILTIN_MAKE_FLOAT2X4, BUILTIN_MAKE_FLOAT3X2,
            BUILTIN_MAKE_FLOAT3X4, BUILTIN_MAKE_FLOAT4X2, BUILTIN_MAKE_FLOAT4X3,
            /// Geometric / texture builtins (the `builtinFunctionMap` set).
            BUILTIN_DOT, BUILTIN_CROSS,
            BUILTIN_SAMPLE, BUILTIN_SAMPLE_LOD, BUILTIN_SAMPLE_BIAS,
            BUILTIN_SAMPLE_GRAD, BUILTIN_GATHER, BUILTIN_GATHER_RED,
            BUILTIN_GATHER_GREEN, BUILTIN_GATHER_BLUE, BUILTIN_GATHER_ALPHA,
            BUILTIN_CALCULATE_LOD, BUILTIN_GET_DIMENSIONS,
            BUILTIN_WRITE, BUILTIN_READ,
            /// 1-arg math.
            BUILTIN_SIN, BUILTIN_COS, BUILTIN_TAN, BUILTIN_ASIN, BUILTIN_ACOS,
            BUILTIN_ATAN, BUILTIN_SQRT, BUILTIN_ABS, BUILTIN_FLOOR, BUILTIN_CEIL,
            BUILTIN_ROUND, BUILTIN_FRAC, BUILTIN_NORMALIZE, BUILTIN_LENGTH,
            BUILTIN_EXP, BUILTIN_EXP2, BUILTIN_LOG, BUILTIN_LOG2,
            BUILTIN_SIGN, BUILTIN_SATURATE, BUILTIN_TRUNC, BUILTIN_RSQRT,
            BUILTIN_DEGREES, BUILTIN_RADIANS, BUILTIN_SINH, BUILTIN_COSH,
            BUILTIN_TANH,
            /// 2-arg math.
            BUILTIN_ATAN2, BUILTIN_POW, BUILTIN_MIN, BUILTIN_MAX, BUILTIN_STEP,
            BUILTIN_REFLECT, BUILTIN_FMOD, BUILTIN_LDEXP,
            /// 3-arg math.
            BUILTIN_CLAMP, BUILTIN_LERP, BUILTIN_SMOOTHSTEP, BUILTIN_FMA,
            /// Aliases + out-param math.
            BUILTIN_MOD, BUILTIN_MAD, BUILTIN_MODF, BUILTIN_FREXP,
            /// §5.2 vector / matrix math.
            BUILTIN_DISTANCE, BUILTIN_FACEFORWARD, BUILTIN_REFRACT, BUILTIN_INVERSE,
            /// §5.2 bool reduce + component-wise compare.
            BUILTIN_ANY, BUILTIN_ALL,
            BUILTIN_LESSTHAN, BUILTIN_LESSTHANEQUAL,
            BUILTIN_GREATERTHAN, BUILTIN_GREATERTHANEQUAL,
            BUILTIN_EQUAL, BUILTIN_NOTEQUAL,
            /// §5.3 integer / bitfield ops.
            BUILTIN_COUNTBITS, BUILTIN_REVERSEBITS,
            /// Matrix intrinsics (string-matched in Sema).
            "transpose", "determinant",
            /// Compute barriers.
            BUILTIN_THREADGROUP_BARRIER, BUILTIN_DEVICE_BARRIER
        };
        return reserved.count(std::string(name)) > 0;
    }
}