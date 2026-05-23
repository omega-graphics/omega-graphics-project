#include <kreate/Pipeline.h>
#include "PipelineFactory.h"
#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GEPipeline.h>
#include <omegasl.h>
#include <fstream>
#include <sstream>
#include <iostream>

namespace Kreate {

struct Pipeline::Impl {
    SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLib;
    SharedHandle<OmegaGTE::GERenderPipelineState> state;
};

Pipeline::Pipeline() : impl(std::make_unique<Impl>()) {}
Pipeline::~Pipeline() = default;

namespace {

OmegaGTE::RasterCullMode mapCull(CullMode m) {
    switch (m) {
        case CullMode::None:  return OmegaGTE::RasterCullMode::None;
        case CullMode::Front: return OmegaGTE::RasterCullMode::Front;
        case CullMode::Back:  return OmegaGTE::RasterCullMode::Back;
    }
    return OmegaGTE::RasterCullMode::None;
}

OmegaGTE::TriangleFillMode mapFill(FillMode m) {
    switch (m) {
        case FillMode::Solid:     return OmegaGTE::TriangleFillMode::Solid;
        case FillMode::Wireframe: return OmegaGTE::TriangleFillMode::Wireframe;
    }
    return OmegaGTE::TriangleFillMode::Solid;
}

} // namespace

std::shared_ptr<Pipeline> PipelineFactory::buildFromLibrary(
    OmegaGTE::GTE &gte,
    SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLib,
    const PipelineDesc &desc) {
    if (!shaderLib) {
        std::cerr << "Kreate::Pipeline: shader library load failed\n";
        return nullptr;
    }

    auto vIt = shaderLib->shaders.find(desc.vertexFunction);
    auto fIt = shaderLib->shaders.find(desc.fragmentFunction);
    if (vIt == shaderLib->shaders.end()) {
        std::cerr << "Kreate::Pipeline: vertex function '"
                  << desc.vertexFunction << "' not in shader library\n";
        return nullptr;
    }
    if (fIt == shaderLib->shaders.end()) {
        std::cerr << "Kreate::Pipeline: fragment function '"
                  << desc.fragmentFunction << "' not in shader library\n";
        return nullptr;
    }

    OmegaGTE::RenderPipelineDescriptor pdesc{};
    pdesc.vertexFunc = vIt->second;
    pdesc.fragmentFunc = fIt->second;
    pdesc.cullMode = mapCull(desc.cullMode);
    pdesc.triangleFillMode = mapFill(desc.fillMode);
    pdesc.depthAndStencilDesc.enableDepth = desc.enableDepth;

    auto state = gte.graphicsEngine->makeRenderPipelineState(pdesc);
    if (!state) {
        std::cerr << "Kreate::Pipeline: makeRenderPipelineState returned null\n";
        return nullptr;
    }

    auto p = std::shared_ptr<Pipeline>(new Pipeline());
    p->impl->shaderLib = std::move(shaderLib);
    p->impl->state = std::move(state);
    return p;
}

std::shared_ptr<Pipeline> PipelineFactory::create(OmegaGTE::GTE &gte,
                                                  const std::string &omegaslPath,
                                                  const PipelineDesc &desc) {
    std::ifstream in(omegaslPath);
    if (!in) {
        std::cerr << "Kreate::Pipeline: cannot open " << omegaslPath << "\n";
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    OmegaCommon::String src = ss.str();

    auto compiled = gte.omegaSlCompiler->compile({
        OmegaSLCompiler::Source::fromString(src)
    });
    if (!compiled) {
        std::cerr << "Kreate::Pipeline: shader compile failed\n";
        return nullptr;
    }
    auto lib = gte.graphicsEngine->loadShaderLibraryRuntime(compiled);
    return buildFromLibrary(gte, lib, desc);
}

std::shared_ptr<Pipeline> PipelineFactory::createFromLibrary(
    OmegaGTE::GTE &gte,
    const std::string &libPath,
    const PipelineDesc &desc) {
    auto lib = gte.graphicsEngine->loadShaderLibrary(OmegaCommon::FS::Path(libPath));
    return buildFromLibrary(gte, lib, desc);
}

} // namespace Kreate
