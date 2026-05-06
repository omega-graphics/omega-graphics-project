#include "Pipeline.h"
#include "omegaWTK/Core/Core.h"

#include <exception>
#include <iostream>
#include <string>

#if defined(TARGET_MACOS)
#include <mach-o/dyld.h>
#elif defined(TARGET_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace OmegaWTK::Composition {

    namespace {
        /// Resolve the path to the precompiled compositor shader library
        /// shipped alongside the executable. The library is produced at
        /// build time by `add_omegasl_lib(OmegaWTKCompositorShaderLib ...)`
        /// and copied into the bundle / output directory by
        /// `OmegaWTKApp.cmake`.
        OmegaCommon::String getCompositorShaderLibPath() {
#if defined(TARGET_MACOS)
            char buf[2048];
            uint32_t bufSize = sizeof(buf);
            if(_NSGetExecutablePath(buf, &bufSize) == 0) {
                std::string path(buf);
                // exe: .../Contents/MacOS/AppName -> .../Contents/Resources/compositor.omegasllib
                auto lastSlash = path.rfind('/');
                if(lastSlash != std::string::npos) {
                    std::string macosDir = path.substr(0, lastSlash);
                    auto parentSlash = macosDir.rfind('/');
                    if(parentSlash != std::string::npos) {
                        return macosDir.substr(0, parentSlash) + "/Resources/compositor.omegasllib";
                    }
                }
            }
            return {};
#elif defined(TARGET_WIN32)
            char buf[MAX_PATH];
            GetModuleFileNameA(NULL, buf, MAX_PATH);
            std::string path(buf);
            auto pos = path.rfind('\\');
            if(pos != std::string::npos) {
                return path.substr(0, pos + 1) + "compositor.omegasllib";
            }
            return {};
#else
            char buf[2048];
            ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if(len > 0) {
                buf[len] = '\0';
                std::string path(buf);
                auto pos = path.rfind('/');
                if(pos != std::string::npos) {
                    return path.substr(0, pos + 1) + "compositor.omegasllib";
                }
            }
            return {};
#endif
        }
    }

    void PipelineRegistry::resetState(){
        shaderLibrary_.reset();
        color_.reset();
        texture_.reset();
        sdf_.reset();
        path_.reset();
        linearGradient_.reset();
        gaussianBlurH_.reset();
        gaussianBlurV_.reset();
        directionalBlur_.reset();
        bufferWriter_.reset();
    }

    bool PipelineRegistry::initialize(){
        resetState();
        bufferWriter_ = OmegaGTE::GEBufferWriter::Create();
        if(bufferWriter_ == nullptr){
            std::cout << "Failed to create compositor buffer writer." << std::endl;
            resetState();
            return false;
        }

        auto shaderLibPath = getCompositorShaderLibPath();
        if(shaderLibPath.empty()){
            std::cout << "Failed to resolve compositor shader library path." << std::endl;
            resetState();
            return false;
        }
        try {
            shaderLibrary_ = gte.graphicsEngine->loadShaderLibrary(
                    OmegaCommon::FS::Path(shaderLibPath));
        }
        catch(const std::exception &ex){
            std::cout << "Failed to load compositor shader library `" << shaderLibPath
                      << "`: " << ex.what() << std::endl;
            resetState();
            return false;
        }
        catch(...){
            std::cout << "Failed to load compositor shader library `" << shaderLibPath
                      << "` due to an unknown exception." << std::endl;
            resetState();
            return false;
        }
        if(shaderLibrary_ == nullptr){
            std::cout << "Failed to load compositor shader library from `" << shaderLibPath << "`." << std::endl;
            resetState();
            return false;
        }
        auto getShader = [&](const char *name) -> SharedHandle<OmegaGTE::GTEShader> {
            auto it = shaderLibrary_->shaders.find(name);
            if(it == shaderLibrary_->shaders.end() || it->second == nullptr){
                std::cout << "Missing shader function " << name << std::endl;
                return nullptr;
            }
            return it->second;
        };

        OmegaGTE::RenderPipelineDescriptor renderPipelineDescriptor {};
        renderPipelineDescriptor.cullMode = OmegaGTE::RasterCullMode::None;
        renderPipelineDescriptor.depthAndStencilDesc = {false,false};
        renderPipelineDescriptor.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
        renderPipelineDescriptor.rasterSampleCount = 1;
        renderPipelineDescriptor.colorPixelFormats = { OmegaGTE::PixelFormat::BGRA8Unorm };
        renderPipelineDescriptor.vertexFunc = getShader("mainVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("mainFragment");

        if(renderPipelineDescriptor.vertexFunc == nullptr || renderPipelineDescriptor.fragmentFunc == nullptr){
            std::cout << "Failed to initialize mandatory color pipeline shaders." << std::endl;
            resetState();
            return false;
        }

        color_ = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
        if(color_ == nullptr){
            std::cout << "Failed to create mandatory color render pipeline state." << std::endl;
            resetState();
            return false;
        }

        renderPipelineDescriptor.vertexFunc = getShader("textureVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("textureFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            texture_ = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(texture_ == nullptr){
                std::cout << "Texture render pipeline creation failed." << std::endl;
            }
        }
        else {
            texture_.reset();
            std::cout << "Texture render pipeline is unavailable." << std::endl;
        }

        // SDF render pipeline (Phase 6). Drives Rect / RoundedRect /
        // Ellipse / Shadow primitives via the closed-form distance shader
        // in compositor.omegasl. The SDF rasterizes the primitive's full
        // bounding quad (including AA / stroke / blur padding) and the
        // fragment shader computes per-pixel coverage analytically; we
        // need alpha-over blending enabled so that pixels outside the
        // analytic silhouette (`a == 0`) preserve the destination instead
        // of overwriting it with the unweighted fill / stroke RGB. The
        // color and texture pipelines don't need blending because they
        // rely on tessellation for tight geometry coverage.
        OmegaGTE::BlendDescriptor sdfBlend {};
        sdfBlend.blendEnabled    = true;
        sdfBlend.srcColorFactor  = OmegaGTE::BlendFactor::SrcAlpha;
        sdfBlend.destColorFactor = OmegaGTE::BlendFactor::InvSrcAlpha;
        sdfBlend.colorOp         = OmegaGTE::BlendOperation::Add;
        sdfBlend.srcAlphaFactor  = OmegaGTE::BlendFactor::One;
        sdfBlend.destAlphaFactor = OmegaGTE::BlendFactor::InvSrcAlpha;
        sdfBlend.alphaOp         = OmegaGTE::BlendOperation::Add;
        sdfBlend.writeMask       = OmegaGTE::ColorWriteAll;
        renderPipelineDescriptor.colorBlendDescriptors = { sdfBlend };

        renderPipelineDescriptor.vertexFunc = getShader("sdfVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("sdfFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            sdf_ = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(sdf_ == nullptr){
                std::cout << "SDF render pipeline creation failed." << std::endl;
            }
        }
        else {
            sdf_.reset();
            std::cout << "SDF render pipeline is unavailable." << std::endl;
        }

        // Path render pipeline (Phase 6.4). Drives `VisualCommand::VectorPath`
        // draws via per-vertex (edgeDistance, attachmentTag) varyings — the
        // fragment shader derives a 1-pixel `smoothstep` AA band from
        // `fwidth(edgeDist)`. Same alpha-over blend setup as the SDF
        // pipeline because the path now produces fractional coverage at
        // the silhouette and pixels outside the silhouette must preserve
        // the destination.
        renderPipelineDescriptor.vertexFunc = getShader("pathVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("pathFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            path_ = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(path_ == nullptr){
                std::cout << "Path render pipeline creation failed." << std::endl;
            }
        }
        else {
            path_.reset();
            std::cout << "Path render pipeline is unavailable." << std::endl;
        }

        // Reset blend state for any subsequently-created pipelines in
        // this initialize() pass so they keep the existing opaque-write
        // contract.
        renderPipelineDescriptor.colorBlendDescriptors.clear();

        // Blur compute pipelines.
        auto blurHFunc = getShader("gaussianBlurH");
        auto blurVFunc = getShader("gaussianBlurV");
        auto dirBlurFunc = getShader("directionalBlur");
        if(blurHFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = blurHFunc;
            gaussianBlurH_ = gte.graphicsEngine->makeComputePipelineState(desc);
        }
        if(blurVFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = blurVFunc;
            gaussianBlurV_ = gte.graphicsEngine->makeComputePipelineState(desc);
        }
        if(dirBlurFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = dirBlurFunc;
            directionalBlur_ = gte.graphicsEngine->makeComputePipelineState(desc);
        }

        return true;
    }

    void PipelineRegistry::shutdown(){
        resetState();
    }

}
