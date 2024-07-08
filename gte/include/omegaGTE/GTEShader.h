#include "GE.h"

#include "omegasl.h"

#ifndef OMEGAGTE_GTESHADER_H
#define OMEGAGTE_GTESHADER_H

_NAMESPACE_BEGIN_

struct GTEShader;

struct GTEShaderLibrary {
    std::map<std::string,SharedHandle<GTEShader>> shaders;
};

size_t OMEGAGTE_EXPORT omegaSLStructSize(OmegaCommon::Vector<omegasl_data_type> fields) noexcept;

struct OMEGAGTE_EXPORT GEBufferWriter {
    OMEGACOMMON_CLASS("OmegaGTE.GEBufferWriter")
    virtual void setOutputBuffer(SharedHandle<GEBuffer> & buffer) = 0;
    virtual void structBegin() = 0;
    virtual void writeFloat(float & v) = 0;
    virtual void writeFloat2(FVec<2> & v) = 0;
    virtual void writeFloat3(FVec<3> & v) = 0;
    virtual void writeFloat4(FVec<4> & v) = 0;
    virtual void structEnd() = 0;
    virtual void sendToBuffer() = 0;
    virtual void flush() = 0;
    static SharedHandle<GEBufferWriter> Create();
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
};


_NAMESPACE_END_

#endif