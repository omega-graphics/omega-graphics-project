#include <stdlib.h>

#ifndef omegasl_h
#define  omegasl_h

/// @name Standard *.omegasllib structs
/// @{
typedef const char * CString;

struct omegasl_lib_header {
    unsigned name_length;
    CString name;
    unsigned entry_count;
};

typedef enum : int {
    OMEGASL_SHADER_DESC_IO_IN,
    OMEGASL_SHADER_DESC_IO_OUT,
    OMEGASL_SHADER_DESC_IO_INOUT
} omegasl_shader_layout_desc_io_mode;

typedef enum : int {
    OMEGASL_SHADER_CONSTANT_DESC,
    OMEGASL_SHADER_BUFFER_DESC,
    OMEGASL_SHADER_TEXTURE1D_DESC,
    OMEGASL_SHADER_TEXTURE2D_DESC,
    OMEGASL_SHADER_TEXTURE3D_DESC,

    OMEGASL_SHADER_SAMPLER1D_DESC,
    OMEGASL_SHADER_SAMPLER2D_DESC,
    OMEGASL_SHADER_SAMPLER3D_DESC,

    OMEGASL_SHADER_STATIC_SAMPLER1D_DESC,
    OMEGASL_SHADER_STATIC_SAMPLER2D_DESC,
    OMEGASL_SHADER_STATIC_SAMPLER3D_DESC
} omegasl_shader_layout_desc_type;

typedef enum : int {
    OMEGASL_SHADER_SAMPLER_LINEAR_FILTER,
    OMEGASL_SHADER_SAMPLER_POINT_FILTER,
    OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER,
    OMEGASL_SHADER_SAMPLER_MIN_ANISOTROPY_FILTER
} omegasl_shader_static_sampler_filter;

typedef enum : int {
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR,
    OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP
} omegasl_shader_static_sampler_address_mode;

struct omegasl_shader_static_sampler_desc {
    omegasl_shader_static_sampler_filter filter;
    omegasl_shader_static_sampler_address_mode u_address_mode,v_address_mode,w_address_mode;
    unsigned int max_anisotropy;
};

typedef enum : int {
    //Int Types
    OMEGASL_INT,
    OMEGASL_INT2,
    OMEGASL_INT3,
    OMEGASL_INT4,

    //Uint Types
    OMEGASL_UINT,
    OMEGASL_UINT2,
    OMEGASL_UINT3,
    OMEGASL_UINT4,

    //Float Types
    OMEGASL_FLOAT,
    OMEGASL_FLOAT2,
    OMEGASL_FLOAT3,
    OMEGASL_FLOAT4,
    OMEGASL_FLOAT2x1,
    OMEGASL_FLOAT2x2,
    OMEGASL_FLOAT2x3,
    OMEGASL_FLOAT2x4,
    OMEGASL_FLOAT3x1,
    OMEGASL_FLOAT3x2,
    OMEGASL_FLOAT3x3,
    OMEGASL_FLOAT3x4,
    OMEGASL_FLOAT4x1,
    OMEGASL_FLOAT4x2,
    OMEGASL_FLOAT4x3,
    OMEGASL_FLOAT4x4,

    OMEGASL_DOUBLE,
    OMEGASL_DOUBLE2,
    OMEGASL_DOUBLE3,
    OMEGASL_DOUBLE4

} omegasl_data_type;

struct omegasl_shader_constant_desc {
    omegasl_data_type type;
    union V {
        float f;
        double d;
        int i;
        unsigned int ui;
    } value;
    static omegasl_shader_constant_desc make_float(float f){
        omegasl_shader_constant_desc::V st{};
        st.f = f;
        return {OMEGASL_FLOAT,st};
    };
    static omegasl_shader_constant_desc make_double(double d){
        omegasl_shader_constant_desc::V st{};
        st.d = d;
        return {OMEGASL_DOUBLE,st};
    };
    static omegasl_shader_constant_desc make_int(int i){
        omegasl_shader_constant_desc::V st{};
        st.i = i;
        return {OMEGASL_INT,st};
    };
    static omegasl_shader_constant_desc make_uint(unsigned int ui){
        omegasl_shader_constant_desc::V st{};
        st.ui = ui;
        return {OMEGASL_UINT,st};
    };
};

struct omegasl_shader_layout_desc {
    omegasl_shader_layout_desc_type type;
    unsigned gpu_relative_loc;
    omegasl_shader_layout_desc_io_mode io_mode;
    size_t location;
    size_t offset;
    omegasl_shader_static_sampler_desc sampler_desc;
    omegasl_shader_constant_desc constant_desc;
};

typedef enum : int {
    OMEGASL_SHADER_VERTEX,
    OMEGASL_SHADER_FRAGMENT,
    OMEGASL_SHADER_COMPUTE
} omegasl_shader_type;


struct omegasl_vertex_shader_param_desc {
    CString name;
    omegasl_data_type type;
    size_t offset;
};

struct omegasl_vertex_shader_input_desc {
    bool useVertexID = false;
    omegasl_vertex_shader_param_desc *pParams = nullptr;
    unsigned nParam = 0;
};

struct omegasl_compute_shader_params_desc {
    bool useGlobalThreadID = false,useLocalThreadID = false,useThreadGroupID = false;
};

struct omegasl_compute_shader_threadgroup_desc {
    unsigned x = 0,y = 0,z = 0;
};


struct omegasl_shader {
    omegasl_shader_type type;
    CString name;
    omegasl_vertex_shader_input_desc vertexShaderInputDesc;
    omegasl_compute_shader_params_desc computeShaderParamsDesc;
    omegasl_compute_shader_threadgroup_desc threadgroupDesc;
    omegasl_shader_layout_desc *pLayout;
    unsigned nLayout;
    void *data;
    size_t dataSize;
};

/// @}


/// Build Library
#ifdef RUNTIME_SHADER_COMP_SUPPORT

#include <omega-common/common.h>

#include "omegaGTE/GTEBase.h"

_NAMESPACE_BEGIN_
struct GTEDevice;
_NAMESPACE_END_

#if _WIN32

#ifdef OMEGAGTE__BUILD__
#define OMEGASLC_EXPORT _declspec(dllexport)
#else
#define OMEGASLC_EXPORT __declspec(dllimport)
#endif

#else 

#define OMEGASLC_EXPORT

#endif


struct omegasl_shader_lib {
    omegasl_lib_header header;
    omegasl_shader *shaders;
};

/**
 *  @brief OmegaSL Compiler Frontend. (For Runtime Compilation Use Only)
 *  @paragraph
 *  The biggest difference between this and the `omegaslc` executable
 *  is that it invokes the target platform's runtime shader compiler
 *  (D3DCompileFromFile on Windows,
    \c \# id<MTLLibrary> [(id<MTLDevice>) newlibraryFromSource: withCompletionProvider:]
    on Apple Devices,
 *  and shaderc::CompilerGlslToSpv() on Android and Linux.)
 * */
class OMEGASLC_EXPORT OmegaSLCompiler {
public:
    struct OMEGASLC_EXPORT Source {
        static std::shared_ptr<Source> fromFile(OmegaCommon::FS::Path path);
        static std::shared_ptr<Source> fromString(OmegaCommon::String & buffer);
    };
    static std::shared_ptr<OmegaSLCompiler> Create(SharedHandle<OmegaGTE::GTEDevice> & device);
    virtual std::shared_ptr<omegasl_shader_lib> compile(std::initializer_list<std::shared_ptr<Source>> sources) = 0;
};

#endif

#endif