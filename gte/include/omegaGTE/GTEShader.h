#include "GE.h"

#include "omegasl.h"

#ifndef OMEGAGTE_GTESHADER_H
#define OMEGAGTE_GTESHADER_H

_NAMESPACE_BEGIN_

/**
  @brief Backend-agnostic shader handle.
  @paragraph Each backend subclasses this struct (see @c GED3D12Shader,
  @c GEMetalShader, @c GTEVulkanShader). The base struct also doubles as the
  sentinel for shaders rejected at library-load time: when the device cannot
  satisfy a shader's @c requiredFeatures, the loader inserts a base-only
  @c GTEShader instance with @c isUnsupported set and a populated
  @c unsupportedDiagnostic. Pipeline builders detect the sentinel and surface
  the diagnostic instead of attempting to build with bytecode that was never
  emitted. See OmegaSL-Feature-Gap-Survey §14.3 / §14.7.1 task C.
 */
struct GTEShader {
    omegasl_shader internal;
    /// Set to true when this handle is a Layer-3 rejection sentinel
    /// produced by @c loadShaderLibraryFromInputStream / @c loadShaderLibraryRuntime.
    bool isUnsupported = false;
    /// "requires features [...]; device lacks [...]" — populated only when
    /// @c isUnsupported is true.
    std::string unsupportedDiagnostic;
};

struct GTEShaderLibrary {
    std::map<std::string,SharedHandle<GTEShader>> shaders;
    /**
      @brief Per-shader load-time rejection diagnostics keyed by shader name.
      @paragraph Populated by @c OmegaGraphicsEngine::loadShaderLibraryFromInputStream
      (and the runtime-load equivalent) when a shader's @c requiredFeatures bits
      are not satisfied by the active device. The shader's slot in @c shaders is
      an unsupported-sentinel @c GTEShader (see above); pipeline builders
      consult this map to surface a precise diagnostic at pipeline-creation
      time. See OmegaSL-Feature-Gap-Survey §14.3 / §14.7.1 task B.
     */
    std::map<std::string,std::string> unsupportedDiagnostics;
};

size_t OMEGAGTE_EXPORT omegaSLStructStride(OmegaCommon::Vector<omegasl_data_type> fields) noexcept;

struct OMEGAGTE_EXPORT GEBufferWriter {
    OMEGACOMMON_CLASS("OmegaGTE.GEBufferWriter")
    virtual void setOutputBuffer(SharedHandle<GEBuffer> & buffer) = 0;
    virtual void structBegin() = 0;
    virtual void writeFloat(float & v) = 0;
    virtual void writeFloat2(FVec<2> & v) = 0;
    virtual void writeFloat3(FVec<3> & v) = 0;
    virtual void writeFloat4(FVec<4> & v) = 0;
    virtual void writeInt(int & v) = 0;
    virtual void writeInt2(IVec<2> & v) = 0;
    virtual void writeInt3(IVec<3> & v) = 0;
    virtual void writeInt4(IVec<4> & v) = 0;
    virtual void writeUint(unsigned & v) = 0;
    virtual void writeUint2(UVec<2> & v) = 0;
    virtual void writeUint3(UVec<3> & v) = 0;
    virtual void writeUint4(UVec<4> & v) = 0;
    virtual void structEnd() = 0;
    virtual void sendToBuffer() = 0;
    virtual void flush() = 0;
    static SharedHandle<GEBufferWriter> Create();
    virtual ~GEBufferWriter() = default;
};

struct OMEGAGTE_EXPORT GEBufferReader {
    OMEGACOMMON_CLASS("OmegaGTE.GEBufferReader")
    virtual void setInputBuffer(SharedHandle<GEBuffer> & buffer) = 0;
    virtual void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) = 0;
    virtual void structBegin() = 0;
    virtual void getFloat(float & v) = 0;
    virtual void getFloat2(FVec<2> & v) = 0;
    virtual void getFloat3(FVec<3> & v) = 0;
    virtual void getFloat4(FVec<4> & v) = 0;
    virtual void structEnd() = 0;
    virtual void reset() = 0;
    static SharedHandle<GEBufferReader> Create();
    virtual ~GEBufferReader() = default;
};


_NAMESPACE_END_

#endif