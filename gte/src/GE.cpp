#include "omegaGTE/GE.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GTEShader.h"

#include "../omegasl/src/ShaderArchive.h"


#ifdef TARGET_DIRECTX
#include "d3d12/GED3D12.h"
#endif



#ifdef TARGET_METAL
#include "metal/GEMetal.h"
#endif



#ifdef TARGET_VULKAN
#include "vulkan/GEVulkan.h"
#endif

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

_NAMESPACE_BEGIN_


// The hand-rolled binary-read helpers and size-limit constants that used to
// live here moved into the shared `ShaderArchive` (de)serializer
// (gte/omegasl/src/ShaderArchive.{h,cpp}) — the single owner of the
// `.omegasllib` byte layout and its defensive bounds checks (Phase 1).

/// Names every @c OMEGASL_FEATURE_BIT_* in @p bits. Order matches the bit
/// indices declared in @c omegasl.h. Empty input yields @c "[]".
static std::string formatFeatureBitList(uint64_t bits) {
    static constexpr struct {
        unsigned long long bit;
        const char *name;
    } kFeatureNames[] = {
        {OMEGASL_FEATURE_BIT_RAYTRACING,         "RAYTRACING"},
        {OMEGASL_FEATURE_BIT_MESH_SHADERS,       "MESH_SHADERS"},
        {OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS,   "GEOMETRY_SHADERS"},
        {OMEGASL_FEATURE_BIT_TESSELLATION,       "TESSELLATION"},
        {OMEGASL_FEATURE_BIT_SUBGROUP_OPS,       "SUBGROUP_OPS"},
        {OMEGASL_FEATURE_BIT_BINDLESS,           "BINDLESS"},
        {OMEGASL_FEATURE_BIT_FLOAT16,            "FLOAT16"},
        {OMEGASL_FEATURE_BIT_INT64,              "INT64"},
        {OMEGASL_FEATURE_BIT_VARIABLE_RATE,      "VARIABLE_RATE"},
        {OMEGASL_FEATURE_BIT_SUBPASS_INPUTS,     "SUBPASS_INPUTS"},
        {OMEGASL_FEATURE_BIT_SPEC_CONSTANTS,     "SPEC_CONSTANTS"},
        {OMEGASL_FEATURE_BIT_TEXTURECUBE_RW,     "TEXTURECUBE_RW"},
        {OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE, "TEXTURE2D_MS_WRITE"},
        {OMEGASL_FEATURE_BIT_DOUBLE,             "DOUBLE"},
    };
    std::string out = "[";
    bool first = true;
    for(auto &entry : kFeatureNames){
        if(bits & entry.bit){
            if(!first) out += ", ";
            out += entry.name;
            first = false;
        }
    }
    out += "]";
    return out;
}

std::string OmegaGraphicsEngine::_formatMissingFeatures(uint64_t requiredFeatures,
                                                       uint64_t missingFeatures) {
    std::string msg = "requires features ";
    msg += formatFeatureBitList(requiredFeatures);
    msg += "; device lacks ";
    msg += formatFeatureBitList(missingFeatures);
    return msg;
}

SharedHandle<GTEShader> OmegaGraphicsEngine::_makeUnsupportedShaderSentinel(std::string diagnostic) {
    auto sentinel = std::make_shared<GTEShader>();
    sentinel->isUnsupported = true;
    sentinel->unsupportedDiagnostic = std::move(diagnostic);
    return sentinel;
}

bool OmegaGraphicsEngine::_checkPipelineShader(const SharedHandle<GTEShader> &shader,
                                               const char *role,
                                               const OmegaCommon::String &pipelineName) {
    if(shader == nullptr){
        std::cerr << "Pipeline '" << pipelineName << "' creation failed: "
                  << role << " shader handle is null." << std::endl;
        return false;
    }
    if(shader->isUnsupported){
        std::cerr << "Pipeline '" << pipelineName << "' creation failed: "
                  << role << " shader is unsupported on this device — "
                  << shader->unsupportedDiagnostic << "." << std::endl;
        return false;
    }
    return true;
}

GEBuffer::GEBuffer(const BufferDescriptor::Usage &usage):usage(usage) {

}

bool GEBuffer::checkCanRead() {
    assert((usage == BufferDescriptor::Readback || usage == BufferDescriptor::Universal) &&
           "Can only read from a `Readback` or `Universal` type of GEBuffer");
    return true;
}

bool GEBuffer::checkCanWrite() {
    assert((usage == BufferDescriptor::Upload || usage == BufferDescriptor::Universal) &&
           "Can only write to an `Upload` or `Universal` type of GEBuffer");
    return true;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibraryFromInputStream(std::istream &in) {
    if(!in.good()){
        std::cerr << "OmegaSL shader library load failed: input stream is not readable." << std::endl;
        return nullptr;
    }

    /// Parse the whole archive — prefix validation (magic + format version),
    /// the library name, and every shader record — through the one shared
    /// deserializer (`ReadShaderArchive`, in `ShaderArchive.cpp`). It performs
    /// no GTE-device work and no feature gating; it stops at raw records. This
    /// is the same core the link tool uses to read a `.omegasllib` with no GPU
    /// present, and it removes the second hand-rolled copy of the byte layout
    /// (Phase 1).
    auto archive = std::make_shared<omegasl::OmegaSLShaderArchive>();
    std::string archiveErr;
    if(!omegasl::ReadShaderArchive(in,*archive,archiveErr)){
        std::cerr << "OmegaSL shader library load failed: " << archiveErr << "." << std::endl;
        return nullptr;
    }

    auto lib = std::make_shared<GTEShaderLibrary>();
    /// The archive owns the backing storage that each record's pointer fields
    /// (name, pLayout, vertex params) — copied into `GTEShader::internal` at
    /// load — alias. Pipeline creation dereferences `internal.pLayout` well
    /// after load, so the archive must outlive the shaders: hand its ownership
    /// to the library (the documented owner of its shaders). This replaces the
    /// old per-buffer `.release()` leak with a clean, library-scoped owner.
    lib->_backingStore = archive;

    /// Per record: device feature-gating (a shader whose `#requires(...)` bits
    /// the active device can't satisfy becomes a rejection sentinel with a
    /// diagnostic) then `_loadShaderFromDesc`. Uniform across stages now that
    /// all parsing — including the per-stage decoration — was done up front by
    /// `ReadShaderArchive`. The records' pointers stay valid via `lib`'s
    /// ownership of the archive (`_backingStore`).
    for(auto & shaderEntry : archive->shaders){
        SharedHandle<GTEShader> shader = nullptr;
        const uint64_t missing = shaderEntry.requiredFeatures & ~_deviceFeatures;
        if(missing != 0){
            auto diag = _formatMissingFeatures(shaderEntry.requiredFeatures,missing);
            std::cerr << "OmegaSL shader '" << shaderEntry.name << "' rejected at load: "
                      << diag << "." << std::endl;
            lib->unsupportedDiagnostics.emplace(std::string(shaderEntry.name),diag);
            shader = _makeUnsupportedShaderSentinel(std::move(diag));
        }
        else {
            shader = _loadShaderFromDesc(&shaderEntry);
        }
        lib->shaders.insert(std::make_pair(std::string(shaderEntry.name),shader));
    }
    return lib;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibrary(FS::Path path) {
    auto absPath = path.absPath();
    if(absPath.empty()){
        std::cerr << "OmegaSL shader library load failed: resolved path is empty." << std::endl;
        return nullptr;
    }
    if(!path.exists()){
        std::cerr << "OmegaSL shader library load failed: file does not exist at `" << absPath << "`." << std::endl;
        return nullptr;
    }

    std::ifstream in(absPath,std::ios::in | std::ios::binary);
    if(!in.is_open()){
        std::cerr << "OmegaSL shader library load failed: could not open `" << absPath << "`." << std::endl;
        return nullptr;
    }

    auto res = loadShaderLibraryFromInputStream(in);
    in.close();
    return res;
}

SharedHandle<GTEShaderLibrary> OmegaGraphicsEngine::loadShaderLibraryRuntime(std::shared_ptr<omegasl_shader_lib> &lib) {
    OmegaCommon::ArrayRef<omegasl_shader> shaders {lib->shaders,lib->shaders + lib->header.entry_count};
    auto shaderLib = std::make_shared<GTEShaderLibrary>();
    for(const auto & s : shaders){
        SharedHandle<GTEShader> shader = nullptr;
        const uint64_t missing = s.requiredFeatures & ~_deviceFeatures;
        if(missing != 0){
            auto diag = _formatMissingFeatures(s.requiredFeatures,missing);
            std::cerr << "OmegaSL shader '" << s.name << "' rejected at runtime load: "
                      << diag << "." << std::endl;
            shaderLib->unsupportedDiagnostics.emplace(std::string(s.name),diag);
            shader = _makeUnsupportedShaderSentinel(std::move(diag));
        }
        else {
            shader = _loadShaderFromDesc((omegasl_shader *)&s,true);
        }
        shaderLib->shaders.insert(std::make_pair(std::string(s.name), shader));
    }
    return shaderLib;
}

bool isPortableNativeRenderTargetFormat(PixelFormat fmt){
    // Intersection of swap-chain / drawable formats supported by D3D12 FLIP
    // model, CAMetalLayer, and Vulkan WSI. See the declaration in GE.h for
    // why RGBA8 and RGBA16 are excluded.
    switch(fmt){
        case PixelFormat::BGRA8Unorm:
        case PixelFormat::BGRA8Unorm_SRGB:
            return true;
        default:
            return false;
    }
}

SharedHandle<OmegaGraphicsEngine> OmegaGraphicsEngine::Create(SharedHandle<GTEDevice> & device){
    #ifdef TARGET_METAL
        return CreateMetalEngine(device);
    #endif
    #ifdef TARGET_DIRECTX
        return GED3D12Engine::Create(device);
    #endif
    #ifdef TARGET_VULKAN
        return GEVulkanEngine::Create(device);
    #endif
};

_NAMESPACE_END_
