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
        OmegaCommon::String getCompositorShaderSourcePath() {
#if defined(TARGET_MACOS)
            char buf[2048];
            uint32_t bufSize = sizeof(buf);
            if(_NSGetExecutablePath(buf, &bufSize) == 0) {
                std::string path(buf);
                // exe: .../Contents/MacOS/AppName -> .../Contents/Resources/compositor.omegasl
                auto lastSlash = path.rfind('/');
                if(lastSlash != std::string::npos) {
                    std::string macosDir = path.substr(0, lastSlash);
                    auto parentSlash = macosDir.rfind('/');
                    if(parentSlash != std::string::npos) {
                        return macosDir.substr(0, parentSlash) + "/Resources/compositor.omegasl";
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
                return path.substr(0, pos + 1) + "compositor.omegasl";
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
                    return path.substr(0, pos + 1) + "compositor.omegasl";
                }
            }
            return {};
#endif
        }
    }

    void PipelineRegistry::resetState(){
        finalCopyByFormat_.clear();
        shaderLibrary_.reset();
        color_.reset();
        texture_.reset();
        finalCopy_.reset();
        linearGradient_.reset();
        gaussianBlurH_.reset();
        gaussianBlurV_.reset();
        directionalBlur_.reset();
        bufferWriter_.reset();
        fullscreenQuadBuffer_.reset();
    }

    bool PipelineRegistry::initialize(){
        resetState();
        bufferWriter_ = OmegaGTE::GEBufferWriter::Create();
        if(bufferWriter_ == nullptr){
            std::cout << "Failed to create compositor buffer writer." << std::endl;
            resetState();
            return false;
        }

        auto shaderSrcPath = getCompositorShaderSourcePath();
        if(shaderSrcPath.empty()){
            std::cout << "Failed to resolve compositor shader source path." << std::endl;
            resetState();
            return false;
        }
        try {
            auto compiledLib = gte.omegaSlCompiler->compile({
                OmegaSLCompiler::Source::fromFile(shaderSrcPath)
            });
            shaderLibrary_ = gte.graphicsEngine->loadShaderLibraryRuntime(compiledLib);
        }
        catch(const std::exception &ex){
            std::cout << "Failed to compile compositor shader source `" << shaderSrcPath
                      << "`: " << ex.what() << std::endl;
            resetState();
            return false;
        }
        catch(...){
            std::cout << "Failed to compile compositor shader source `" << shaderSrcPath
                      << "` due to an unknown exception." << std::endl;
            resetState();
            return false;
        }
        if(shaderLibrary_ == nullptr){
            std::cout << "Failed to load compositor shader library from `" << shaderSrcPath << "`." << std::endl;
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

        OMEGAWTK_DEBUG("Phase 1");

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

        OMEGAWTK_DEBUG("Phase 2");

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

        renderPipelineDescriptor.vertexFunc = getShader("copyVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("copyFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            finalCopy_ = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(finalCopy_ == nullptr){
                std::cout << "Final copy pipeline creation failed." << std::endl;
            }
        }
        else {
            finalCopy_.reset();
            std::cout << "Final copy pipeline is unavailable." << std::endl;
        }

        OMEGAWTK_DEBUG("Phase 3");

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

        auto struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT2,OMEGASL_FLOAT2});

        auto pos = OmegaGTE::FVec<4>::Create();
        auto texCoord = OmegaGTE::FVec<2>::Create();
        auto pad = OmegaGTE::FVec<2>::Create();
        pad[0][0] = 0.f; pad[1][0] = 0.f;
        pos[0][0] = -1.f;
        pos[1][0] = 1.f;
        pos[2][0] = 0.f;
        pos[3][0] = 1.f;

        texCoord[0][0] = 0.f;
        texCoord[1][0] = 0.f;
        fullscreenQuadBuffer_ = gte.graphicsEngine->makeBuffer({OmegaGTE::BufferDescriptor::Upload,struct_size * 6,struct_size});
        if(fullscreenQuadBuffer_ == nullptr){
            std::cout << "Failed to create compositor fullscreen draw buffer." << std::endl;
            resetState();
            return false;
        }
        bufferWriter_->setOutputBuffer(fullscreenQuadBuffer_);

        OMEGAWTK_DEBUG("Phase 4");
        /// Triangle 1
        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();

        texCoord[1][0] = 1.f;
        pos[1][0] = -1.f;

        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();

        texCoord[0][0] = 1.f;
        pos[0][0] = 1.f;

        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();


        /// Triangle 2

        texCoord[0][0] = texCoord[1][0] = 0.f;
        pos[1][0] = 1.f;
        pos[0][0] = -1.f;

        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();

        texCoord[0][0] = 1.f;
        pos[0][0] = 1.f;

        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();

        texCoord[1][0] = 1.f;
        pos[1][0] = -1.f;

        bufferWriter_->structBegin();
        bufferWriter_->writeFloat4(pos);
        bufferWriter_->writeFloat2(texCoord);
        bufferWriter_->writeFloat2(pad);
        bufferWriter_->structEnd();
        bufferWriter_->sendToBuffer();

        bufferWriter_->flush();
        return true;
    }

    void PipelineRegistry::shutdown(){
        resetState();
    }

    SharedHandle<OmegaGTE::GERenderPipelineState>
    PipelineRegistry::finalCopyForFormat(OmegaGTE::PixelFormat fmt){
        auto it = finalCopyByFormat_.find(fmt);
        if(it != finalCopyByFormat_.end() && it->second != nullptr){
            return it->second;
        }
        // The default pipeline was created with RGBA8Unorm. If that matches, reuse it.
        if(fmt == OmegaGTE::PixelFormat::RGBA8Unorm && finalCopy_ != nullptr){
            finalCopyByFormat_[fmt] = finalCopy_;
            return finalCopy_;
        }
        // Create a new pipeline for this format using the copy shaders.
        // Do NOT fall back to finalCopy_ when the requested format differs
        // from RGBA8Unorm — the Vulkan render pass formats would be
        // incompatible (e.g. BGRA8Unorm swapchain vs RGBA8Unorm pipeline),
        // resulting in a validation error and blank output.
        if(shaderLibrary_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] finalCopyForFormat: shaderLibrary is null, cannot create pipeline for format " << static_cast<int>(fmt) << std::endl;
#endif
            return nullptr;
        }
        auto copyVertex = shaderLibrary_->shaders.count("copyVertex") ? shaderLibrary_->shaders["copyVertex"] : nullptr;
        auto copyFragment = shaderLibrary_->shaders.count("copyFragment") ? shaderLibrary_->shaders["copyFragment"] : nullptr;
        if(copyVertex == nullptr || copyFragment == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] finalCopyForFormat: copy shaders missing, cannot create pipeline for format " << static_cast<int>(fmt) << std::endl;
#endif
            return nullptr;
        }
        OmegaGTE::RenderPipelineDescriptor desc {};
        desc.cullMode = OmegaGTE::RasterCullMode::None;
        desc.depthAndStencilDesc = {false,false};
        desc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
        desc.rasterSampleCount = 1;
        desc.vertexFunc = copyVertex;
        desc.fragmentFunc = copyFragment;
        desc.colorPixelFormats = { fmt };
        auto pipeline = gte.graphicsEngine->makeRenderPipelineState(desc);
        if(pipeline != nullptr){
            finalCopyByFormat_[fmt] = pipeline;
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] finalCopyForFormat: created pipeline for format " << static_cast<int>(fmt) << std::endl;
#endif
        } else {
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] finalCopyForFormat: pipeline creation FAILED for format " << static_cast<int>(fmt) << std::endl;
#endif
        }
        return pipeline;
    }

}
