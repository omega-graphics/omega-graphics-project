#include "GTEBase.h"
#include "GTEShader.h"
#include <initializer_list>
#include <map>

#ifndef OMEGAGTE_GEPIPELINE_H
#define OMEGAGTE_GEPIPELINE_H

#ifdef None 
#undef None
#endif

#if defined(TARGET_METAL) && defined(__OBJC__)
@protocol MTLLibrary;
#endif

_NAMESPACE_BEGIN_

    enum class RasterCullMode : int {
        None = 0x00,
        Front,
        Back
    };

    enum class TriangleFillMode : int {
        Wireframe = 0x00,
        Solid
    };

    enum class CompareFunc : int {
        Less,
        LessEqual,
        Greater,
        GreaterEqual
    };

    enum class StencilOperation : int {
        Retain,
        Zero,
        Replace,
        IncrementWrap,
        DecrementWrap
    };

    enum class DepthWriteAmount : int {
        Zero,
        All
    };

    struct  OMEGAGTE_EXPORT RenderPipelineDescriptor {
        OmegaCommon::StrRef name;
        SharedHandle<GTEShader> vertexFunc;
        SharedHandle<GTEShader> fragmentFunc;
        unsigned rasterSampleCount = 0;
        RasterCullMode cullMode = RasterCullMode::None;
        TriangleFillMode triangleFillMode = TriangleFillMode::Solid;
        GTEPolygonFrontFaceRotation polygonFrontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise;
        struct DepthStencilDesc {
            bool enableDepth = false;
            bool enableStencil = false;
            DepthWriteAmount writeAmount = DepthWriteAmount::All;
            CompareFunc depthOperation = CompareFunc::Less;
            float depthBias = 0.f,slopeScale = 0.f,depthClamp = 0.f;
            unsigned stencilReadMask = 0,stencilWriteMask = 0;
            struct StencilDesc {
                StencilOperation
                        stencilFail = StencilOperation::Replace,
                        depthFail = StencilOperation::Replace,
                        pass = StencilOperation::Retain;
                CompareFunc func = CompareFunc::Less;
            } frontFaceStencil,backFaceStencil;
        } depthAndStencilDesc;
    };

    struct  OMEGAGTE_EXPORT ComputePipelineDescriptor {
        OmegaCommon::StrRef name;
        SharedHandle<GTEShader> computeFunc;

    };
    typedef struct __GERenderPipelineState  GERenderPipelineState;
    typedef struct __GEComputePipelineState GEComputePipelineState;


_NAMESPACE_END_

#endif
