

#include "RenderTarget.h"
#include "VisualTree.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "MainThreadDispatch.h"
#include "omegaWTK/Composition/Canvas.h"
#include "ResourceTrace.h"

#include "omegaWTK/Media/ImgCodec.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

#if defined(TARGET_MACOS)
#include <mach-o/dyld.h>
#elif defined(TARGET_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace OmegaWTK::Composition {
    #ifdef TARGET_MACOS
    void stopMTLCapture();
    #endif

    namespace {
        constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
        constexpr float kRenderScaleFloor = 2.f;
#else
        constexpr float kRenderScaleFloor = 1.f;
#endif

        static inline float sanitizeRenderScale(float scale){
            if(!std::isfinite(scale) || scale <= 0.f){
                return kRenderScaleFloor;
            }
            return std::clamp(scale,kRenderScaleFloor,kMaxTextureDimension);
        }

        static inline float sanitizeCoordinate(float value,float fallback){
            if(!std::isfinite(value)){
                return fallback;
            }
            return value;
        }

        static inline Core::Rect sanitizeRenderRect(const Core::Rect & candidate,
                                                    const Core::Rect & fallback,
                                                    float renderScale){
            Core::Rect sanitizedFallback = fallback;
            sanitizedFallback.pos.x = sanitizeCoordinate(sanitizedFallback.pos.x,0.f);
            sanitizedFallback.pos.y = sanitizeCoordinate(sanitizedFallback.pos.y,0.f);
            if(!std::isfinite(sanitizedFallback.w) || sanitizedFallback.w <= 0.f){
                sanitizedFallback.w = 1.f;
            }
            if(!std::isfinite(sanitizedFallback.h) || sanitizedFallback.h <= 0.f){
                sanitizedFallback.h = 1.f;
            }

            const float scale = sanitizeRenderScale(renderScale);
            const float maxLogicalDimension = std::max(1.f,kMaxTextureDimension / scale);
            sanitizedFallback.w = std::clamp(sanitizedFallback.w,1.f,maxLogicalDimension);
            sanitizedFallback.h = std::clamp(sanitizedFallback.h,1.f,maxLogicalDimension);

            Core::Rect sanitized = candidate;
            sanitized.pos.x = sanitizeCoordinate(sanitized.pos.x,sanitizedFallback.pos.x);
            sanitized.pos.y = sanitizeCoordinate(sanitized.pos.y,sanitizedFallback.pos.y);

            if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f){
                sanitized.w = sanitizedFallback.w;
            }
            if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f){
                sanitized.h = sanitizedFallback.h;
            }

            sanitized.w = std::clamp(sanitized.w,1.f,maxLogicalDimension);
            sanitized.h = std::clamp(sanitized.h,1.f,maxLogicalDimension);
            return sanitized;
        }

        static inline unsigned toBackingDimension(float logicalDimension,float renderScale){
            const float saneScale = sanitizeRenderScale(renderScale);
            float saneLogical = logicalDimension;
            if(!std::isfinite(saneLogical) || saneLogical <= 0.f){
                saneLogical = 1.f;
            }
            const auto scaled = static_cast<long>(std::lround(saneLogical * saneScale));
            const auto clamped = std::clamp<long>(scaled,1L,static_cast<long>(kMaxTextureDimension));
            return static_cast<unsigned>(clamped);
        }
    }

    static SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary;
    static SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;
    static SharedHandle<OmegaGTE::GERenderPipelineState> renderPipelineState;
    static SharedHandle<OmegaGTE::GERenderPipelineState> textureRenderPipelineState;
    static SharedHandle<OmegaGTE::GERenderPipelineState> finalCopyRenderPipelineState;
    static OmegaCommon::Map<OmegaGTE::PixelFormat,SharedHandle<OmegaGTE::GERenderPipelineState>> finalCopyPipelinesByFormat;

    static SharedHandle<OmegaGTE::GEComputePipelineState> linearGradientPipelineState;
    static SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurHPipelineState;
    static SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurVPipelineState;
    static SharedHandle<OmegaGTE::GEComputePipelineState> directionalBlurPipelineState;

    static constexpr std::size_t kTextureHeapSize = 64u * 1024u * 1024u;
    static constexpr std::size_t kBufferHeapSize = 8u * 1024u * 1024u;
    static SharedHandle<OmegaGTE::GEHeap> textureHeap;
    static SharedHandle<OmegaGTE::GEHeap> bufferHeap;
    static std::unique_ptr<TexturePool> texturePool;
    static std::unique_ptr<BufferPool> bufferPool;
    static std::unique_ptr<FencePool> fencePool;

    static OmegaCommon::String getCompositorShaderLibPath() {
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

    static SharedHandle<OmegaGTE::GEBuffer> finalTextureDrawBuffer;

    void loadGlobalRenderAssets(){
        bufferWriter = OmegaGTE::GEBufferWriter::Create();
        auto shaderLibPath = getCompositorShaderLibPath();
        shaderLibrary = gte.graphicsEngine->loadShaderLibrary(shaderLibPath);
        auto getShader = [&](const char *name) -> SharedHandle<OmegaGTE::GTEShader> {
            auto it = shaderLibrary->shaders.find(name);
            if(it == shaderLibrary->shaders.end() || it->second == nullptr){
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
        renderPipelineDescriptor.vertexFunc = getShader("mainVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("mainFragment");

        if(renderPipelineDescriptor.vertexFunc == nullptr || renderPipelineDescriptor.fragmentFunc == nullptr){
            std::cout << "Failed to initialize mandatory color pipeline shaders." << std::endl;
            return;
        }

        renderPipelineState = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
        if(renderPipelineState == nullptr){
            std::cout << "Failed to create mandatory color render pipeline state." << std::endl;
            return;
        }

        OMEGAWTK_DEBUG("Phase 2");

        renderPipelineDescriptor.vertexFunc = getShader("textureVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("textureFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            textureRenderPipelineState = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(textureRenderPipelineState == nullptr){
                std::cout << "Texture render pipeline creation failed." << std::endl;
            }
        }
        else {
            textureRenderPipelineState.reset();
            std::cout << "Texture render pipeline is unavailable." << std::endl;
        }

        renderPipelineDescriptor.vertexFunc = getShader("copyVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("copyFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            finalCopyRenderPipelineState = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
            if(finalCopyRenderPipelineState == nullptr){
                std::cout << "Final copy pipeline creation failed." << std::endl;
            }
        }
        else {
            finalCopyRenderPipelineState.reset();
            std::cout << "Final copy pipeline is unavailable." << std::endl;
        }

        OMEGAWTK_DEBUG("Phase 3");

//        OmegaGTE::ComputePipelineDescriptor linearGradientPipelineDesc {};
//        linearGradientPipelineDesc.computeFunc = shaderLibrary->shaders["linearGradient"];
//        linearGradientPipelineState = gte.graphicsEngine->makeComputePipelineState(linearGradientPipelineDesc);

        // Blur compute pipelines.
        auto blurHFunc = getShader("gaussianBlurH");
        auto blurVFunc = getShader("gaussianBlurV");
        auto dirBlurFunc = getShader("directionalBlur");
        if(blurHFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = blurHFunc;
            gaussianBlurHPipelineState = gte.graphicsEngine->makeComputePipelineState(desc);
        }
        if(blurVFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = blurVFunc;
            gaussianBlurVPipelineState = gte.graphicsEngine->makeComputePipelineState(desc);
        }
        if(dirBlurFunc != nullptr){
            OmegaGTE::ComputePipelineDescriptor desc {};
            desc.computeFunc = dirBlurFunc;
            directionalBlurPipelineState = gte.graphicsEngine->makeComputePipelineState(desc);
        }

        auto struct_size = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT2});

        auto pos = OmegaGTE::FVec<4>::Create();
        auto texCoord = OmegaGTE::FVec<2>::Create();
        pos[0][0] = -1.f;
        pos[1][0] = 1.f;
        pos[2][0] = 0.f;
        pos[3][0] = 1.f;

        texCoord[0][0] = 0.f;
        texCoord[1][0] = 0.f;
        finalTextureDrawBuffer = gte.graphicsEngine->makeBuffer({OmegaGTE::BufferDescriptor::Upload,struct_size * 6,struct_size});
        bufferWriter->setOutputBuffer(finalTextureDrawBuffer);

        OMEGAWTK_DEBUG("Phase 4");
        /// Triangle 1
        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();

        texCoord[1][0] = 1.f;
        pos[1][0] = -1.f;

        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();

        texCoord[0][0] = 1.f;
        pos[0][0] = 1.f;

        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();


        /// Triangle 2

        texCoord[0][0] = texCoord[1][0] = 0.f;
        pos[1][0] = 1.f;
        pos[0][0] = -1.f;

        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();

        texCoord[0][0] = 1.f;
        pos[0][0] = 1.f;

        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();

        texCoord[1][0] = 1.f;
        pos[1][0] = -1.f;

        bufferWriter->structBegin();
        bufferWriter->writeFloat4(pos);
        bufferWriter->writeFloat2(texCoord);
        bufferWriter->structEnd();
        bufferWriter->sendToBuffer();

        bufferWriter->flush();
    }

    void destroyGlobalRenderAssets(){
        finalCopyPipelinesByFormat.clear();
        shaderLibrary.reset();
        renderPipelineState.reset();
        textureRenderPipelineState.reset();
        finalCopyRenderPipelineState.reset();
        linearGradientPipelineState.reset();
        bufferWriter.reset();
        finalTextureDrawBuffer.reset();
    }

    static SharedHandle<OmegaGTE::GERenderPipelineState> getFinalCopyPipelineForFormat(OmegaGTE::PixelFormat fmt){
        auto it = finalCopyPipelinesByFormat.find(fmt);
        if(it != finalCopyPipelinesByFormat.end() && it->second != nullptr){
            return it->second;
        }
        // The default pipeline was created with RGBA8Unorm. If that matches, reuse it.
        if(fmt == OmegaGTE::PixelFormat::RGBA8Unorm && finalCopyRenderPipelineState != nullptr){
            finalCopyPipelinesByFormat[fmt] = finalCopyRenderPipelineState;
            return finalCopyRenderPipelineState;
        }
        // Create a new pipeline for this format using the copy shaders.
        if(shaderLibrary == nullptr){
            return finalCopyRenderPipelineState;
        }
        auto copyVertex = shaderLibrary->shaders.count("copyVertex") ? shaderLibrary->shaders["copyVertex"] : nullptr;
        auto copyFragment = shaderLibrary->shaders.count("copyFragment") ? shaderLibrary->shaders["copyFragment"] : nullptr;
        if(copyVertex == nullptr || copyFragment == nullptr){
            return finalCopyRenderPipelineState;
        }
        OmegaGTE::RenderPipelineDescriptor desc {};
        desc.cullMode = OmegaGTE::RasterCullMode::None;
        desc.depthAndStencilDesc = {false,false};
        desc.triangleFillMode = OmegaGTE::TriangleFillMode::Solid;
        desc.rasterSampleCount = 1;
        desc.vertexFunc = copyVertex;
        desc.fragmentFunc = copyFragment;
        desc.colorPixelFormat = fmt;
        auto pipeline = gte.graphicsEngine->makeRenderPipelineState(desc);
        if(pipeline != nullptr){
            finalCopyPipelinesByFormat[fmt] = pipeline;
        }
        return pipeline;
    }

    static void createResourcePools(){
        OmegaGTE::HeapDescriptor texHeapDesc{};
        texHeapDesc.len = kTextureHeapSize;
        OmegaGTE::HeapDescriptor bufHeapDesc{};
        bufHeapDesc.len = kBufferHeapSize;
        textureHeap = gte.graphicsEngine->makeHeap(texHeapDesc);
        bufferHeap = gte.graphicsEngine->makeHeap(bufHeapDesc);
        texturePool = std::make_unique<TexturePool>(textureHeap);
        bufferPool = std::make_unique<BufferPool>(bufferHeap);
        fencePool = std::make_unique<FencePool>();
    }

    static void destroyResourcePools(){
        if(fencePool) fencePool->drain();
        if(bufferPool) bufferPool->drain();
        if(texturePool) texturePool->drain();
        fencePool.reset();
        bufferPool.reset();
        texturePool.reset();
        bufferHeap.reset();
        textureHeap.reset();
    }

    void InitializeEngine(){
        loadGlobalRenderAssets();
        createResourcePools();
    }

    void CleanupEngine(){
        destroyResourcePools();
        destroyGlobalRenderAssets();
    }

BackendRenderTargetContext::BackendRenderTargetContext(Core::Rect & rect,
        SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTarget,
        float renderScaleValue):
        fence(fencePool ? fencePool->acquire() : gte.graphicsEngine->makeFence()),
        renderTarget(renderTarget),
        renderTargetSize(rect),
        renderScale(1.f)
        {
    renderScale = sanitizeRenderScale(renderScaleValue);
    renderTargetSize = sanitizeRenderRect(rect,
                                          Core::Rect{Core::Position{0.f,0.f},1.f,1.f},
                                          renderScale);
    traceResourceId = ResourceTrace::nextResourceId();
    ResourceTrace::emit("Create",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        renderTargetSize.w,
                        renderTargetSize.h,
                        renderScale);
    rebuildBackingTarget();
    imageProcessor = BackendCanvasEffectProcessor::Create(fence);
}

void BackendRenderTargetContext::rebuildBackingTarget(){
    renderTargetSize = sanitizeRenderRect(renderTargetSize,
                                          Core::Rect{Core::Position{0.f,0.f},1.f,1.f},
                                          renderScale);
    backingWidth = toBackingDimension(renderTargetSize.w,renderScale);
    backingHeight = toBackingDimension(renderTargetSize.h,renderScale);
    ResourceTrace::emit("ResizeRebuild",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        static_cast<float>(backingWidth),
                        static_cast<float>(backingHeight),
                        renderScale);

    TexturePoolKey poolKey {
        backingWidth,
        backingHeight,
        OmegaGTE::TexturePixelFormat::RGBA8Unorm,
        OmegaGTE::GETexture::RenderTarget
    };

    if(texturePool && (targetTexture || effectTexture)){
#ifdef _WIN32
        if(renderTarget != nullptr)
            renderTarget->waitForGPU();
#endif
        if(targetTexture)
            texturePool->release(std::move(targetTexture), poolKey);
        if(effectTexture)
            texturePool->release(std::move(effectTexture), poolKey);
    }
    targetTexture.reset();
    effectTexture.reset();

    // Texture and render-target allocation is thread-safe on Metal and
    // D3D12.  Previously this block dispatched synchronously to the main
    // thread, which deadlocked the compositor during live resize because
    // the main thread was busy in NSEventTrackingRunLoopMode.
    {
        if(texturePool){
            targetTexture = texturePool->acquire(poolKey);
            effectTexture = texturePool->acquire(poolKey);
        }
        else {
            OmegaGTE::TextureDescriptor textureDescriptor {};
            textureDescriptor.usage = OmegaGTE::GETexture::RenderTarget;
            textureDescriptor.storage_opts = OmegaGTE::Shared;
            textureDescriptor.width = backingWidth;
            textureDescriptor.height = backingHeight;
            textureDescriptor.type = OmegaGTE::GETexture::Texture2D;
            textureDescriptor.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
            targetTexture = gte.graphicsEngine->makeTexture(textureDescriptor);
            effectTexture = gte.graphicsEngine->makeTexture(textureDescriptor);
        }

        if(targetTexture == nullptr || effectTexture == nullptr){
            std::cout << "Failed to allocate backing textures." << std::endl;
            preEffectTarget.reset();
            effectTarget.reset();
            tessellationEngineContext.reset();
            return;
        }
        preEffectTarget = gte.graphicsEngine->makeTextureRenderTarget({true,targetTexture});
        effectTarget = gte.graphicsEngine->makeTextureRenderTarget({true,effectTexture});
        if(preEffectTarget == nullptr || effectTarget == nullptr){
            std::cout << "Failed to allocate Vulkan texture render targets." << std::endl;
            tessellationEngineContext.reset();
            return;
        }
        tessellationEngineContext = gte.triangulationEngine->createTEContextFromTextureRenderTarget(preEffectTarget);
        if(tessellationEngineContext == nullptr){
            std::cout << "Failed to create tessellation context for backing render target." << std::endl;
        }
    }
}

BackendRenderTargetContext::~BackendRenderTargetContext(){
    ResourceTrace::emit("Destroy",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        renderTargetSize.w,
                        renderTargetSize.h,
                        renderScale);
    TexturePoolKey poolKey {
        backingWidth,
        backingHeight,
        OmegaGTE::TexturePixelFormat::RGBA8Unorm,
        OmegaGTE::GETexture::RenderTarget
    };
    if(texturePool && (targetTexture || effectTexture)){
#ifdef _WIN32
        if(preEffectTarget != nullptr)
            preEffectTarget->waitForGPU();
        if(renderTarget != nullptr)
            renderTarget->waitForGPU();
#endif
        if(targetTexture)
            texturePool->release(std::move(targetTexture), poolKey);
        if(effectTexture)
            texturePool->release(std::move(effectTexture), poolKey);
    }
    imageProcessor.reset();
    preEffectTarget.reset();
    effectTarget.reset();
    tessellationEngineContext.reset();
    for(auto & entry : deferredBufferReleases){
        if(bufferPool && entry.first)
            bufferPool->release(std::move(entry.first), entry.second);
    }
    deferredBufferReleases.clear();
    if(fencePool && fence)
        fencePool->release(std::move(fence));
}

    void BackendRenderTargetContext::setRenderTargetSize(Core::Rect &rect) {
        const unsigned oldW = backingWidth;
        const unsigned oldH = backingHeight;

        const auto saneRect = sanitizeRenderRect(rect,renderTargetSize,renderScale);
        const unsigned newW = toBackingDimension(saneRect.w,renderScale);
        const unsigned newH = toBackingDimension(saneRect.h,renderScale);

        if(oldW == newW && oldH == newH){
            renderTargetSize = saneRect;
            return;
        }

        renderTargetSize = saneRect;
        rebuildBackingTarget();
    }

#ifdef _WIN32
void BackendRenderTargetContext::resizeSwapChain(unsigned int backingWidth, unsigned int backingHeight) {
    if (renderTarget != nullptr)
        renderTarget->resizeSwapChain(backingWidth, backingHeight);
}
void BackendRenderTargetContext::waitForGPU() {
    if (renderTarget != nullptr)
        renderTarget->waitForGPU();
}
#endif

void BackendRenderTargetContext::clear(float r, float g, float b, float a) {
    if(preEffectTarget == nullptr){
        return;
    }
    auto cb = preEffectTarget->commandBuffer();

    OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
    renderPassDesc.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
            OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(r,g,b,a),
            OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear);
    renderPassDesc.depthStencilAttachment.disabled = true;
    cb->startRenderPass(renderPassDesc);
    cb->endRenderPass();
    preEffectTarget->submitCommandBuffer(cb);
}

void BackendRenderTargetContext::applyEffectToTarget(const CanvasEffect & effect) {
    effectQueue.push_back(effect);
}


    void BackendRenderTargetContext::commit(){
        commit(0,0,std::chrono::steady_clock::now(),{});
    }

    void BackendRenderTargetContext::commit(std::uint64_t syncLaneId,
                                            std::uint64_t syncPacketId,
                                            std::chrono::steady_clock::time_point submitTimeCpu,
                                            BackendSubmissionCompletionHandler completionHandler){
        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:commit_start\",\"message\":\"commit start\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"D\"}\n";
            }
        }
        // #endregion
        if(preEffectTarget == nullptr){
            if(completionHandler){
                BackendSubmissionTelemetry telemetry {};
                telemetry.syncLaneId = syncLaneId;
                telemetry.syncPacketId = syncPacketId;
                telemetry.submitTimeCpu = submitTimeCpu;
                telemetry.completeTimeCpu = std::chrono::steady_clock::now();
                telemetry.presentTimeCpu = telemetry.completeTimeCpu;
                telemetry.status = BackendSubmissionStatus::Dropped;
                completionHandler(telemetry);
            }
            return;
        }
        auto _l_cb = preEffectTarget->commandBuffer();
        const bool canApplyEffects = !effectQueue.empty() &&
                                     imageProcessor != nullptr &&
                                     effectTexture != nullptr &&
                                     effectTarget != nullptr;
        std::cout << "[WTK Diag] commit: canApplyEffects=" << canApplyEffects << std::endl;
        if(canApplyEffects){
            preEffectTarget->submitCommandBuffer(_l_cb);
            std::cout << "[WTK Diag] commit: preEffectTarget->commit()" << std::endl;
            preEffectTarget->commit();
            std::cout << "[WTK Diag] commit: applyEffects" << std::endl;
            imageProcessor->applyEffects(effectTexture,preEffectTarget,effectQueue,backingWidth,backingHeight);
            preEffectTarget->waitForGPU();
            preEffectTarget->signalFence(fence);
        } else {
            preEffectTarget->submitCommandBuffer(_l_cb, fence);
            std::cout << "[WTK Diag] commit: preEffectTarget->commit()" << std::endl;
            preEffectTarget->commit();
            std::cout << "[WTK Diag] commit: preEffectTarget->commit() done" << std::endl;
        }
        committedTexture = preEffectTarget->underlyingTexture();
        effectQueue.clear();
        hasPendingContent = true;
    }

    void BackendRenderTargetContext::releaseDeferredBuffers(){
        if(bufferPool){
            for(auto & entry : deferredBufferReleases){
                if(entry.first)
                    bufferPool->release(std::move(entry.first), entry.second);
            }
            deferredBufferReleases.clear();
        }
    }

    void
    BackendRenderTargetContext::createGradientTexture(bool linearOrRadial, Gradient &gradient, OmegaGTE::GRect &rect,
                                                      SharedHandle<OmegaGTE::GETexture> &dest) {
        if(renderTarget == nullptr || dest == nullptr || bufferWriter == nullptr || linearGradientPipelineState == nullptr){
            return;
        }
        auto cb = renderTarget->commandBuffer();

        size_t structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT});

        OmegaGTE::BufferDescriptor bufferDescriptor {OmegaGTE::BufferDescriptor::Upload,structSize,structSize,OmegaGTE::Shared};

        auto constBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);


//        bufferWriter->setOutputBuffer(constBuffer);
//        bufferWriter->structBegin();
//        bufferWriter->writeFloat(gradient);
//        bufferWriter->structEnd();
//        bufferWriter->sendToBuffer();
//        bufferWriter->flush();

        structSize =  OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT,OMEGASL_FLOAT4});

        bufferDescriptor.len = structSize * gradient.stops.size();
        bufferDescriptor.objectStride = structSize;

        auto stopsBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

        bufferWriter->setOutputBuffer(stopsBuffer);

        for(auto & stop : gradient.stops){
            bufferWriter->structBegin();
            bufferWriter->writeFloat(stop.pos);
            auto vec_color = OmegaGTE::makeColor(stop.color.r,stop.color.g,stop.color.b,stop.color.a);
            bufferWriter->writeFloat4(vec_color);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        }

        bufferWriter->flush();

        cb->startComputePass(linearGradientPipelineState);
        cb->bindResourceAtComputeShader(dest,4);
        cb->bindResourceAtComputeShader(constBuffer,5);
        cb->bindResourceAtComputeShader(stopsBuffer,3);
        cb->dispatchThreads((unsigned)rect.w,(unsigned)rect.h,1);
        cb->endComputePass();
        renderTarget->submitCommandBuffer(cb);
    }

    typedef decltype(VisualCommand::params) VisualCommandParams;

    void BackendRenderTargetContext::renderToTarget(VisualCommand::Type type, void *params) {
        if(bufferWriter == nullptr || preEffectTarget == nullptr || tessellationEngineContext == nullptr){
            return;
        }
        OmegaGTE::TETriangulationResult result;

        OmegaGTE::GEViewport viewPort {};
        viewPort.x = viewPort.y = viewPort.nearDepth = 0.f;
        viewPort.farDepth = 1.f;
        viewPort.width = renderTargetSize.w;
        viewPort.height = renderTargetSize.h;

        std::cout << "W:" << renderTargetSize.w
                  << " H:" << renderTargetSize.h
                  << " BW:" << backingWidth
                  << " BH:" << backingHeight
                  << " S:" << renderScale
                  << std::endl;

        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:renderToTarget\",\"message\":\"entry\",\"data\":{\"type\":"
                  << (int)type << "},\"timestamp\":" << ts << ",\"hypothesisId\":\"A\"}\n";
            }
        }
        // #endregion

        if (params == nullptr) {
            return;
        }

        size_t struct_size;
        bool useTextureRenderPipeline = false;
        float textureCoordDenomW = 1.f;
        float textureCoordDenomH = 1.f;

        SharedHandle<OmegaGTE::GETexture> texturePaint;

        SharedHandle<OmegaGTE::GEFence> textureFence;

        switch (type) {
            case VisualCommand::Rect : {
                auto & _params = ((VisualCommandParams*)params)->rectParams;
                if (_params.brush == nullptr) return;
                auto te_params = OmegaGTE::TETriangulationParams::Rect(_params.rect);

                useTextureRenderPipeline = !_params.brush->isColor;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }

                result = tessellationEngineContext->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Bitmap : {
                auto & _params = ((VisualCommandParams*)params)->bitmapParams;
                auto te_params = OmegaGTE::TETriangulationParams::Rect(_params.rect);

                useTextureRenderPipeline = true;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                if(_params.texture){
                    texturePaint = _params.texture;
                    textureFence = _params.textureFence;
                }
                else {
                    OmegaGTE::TextureDescriptor texDesc {OmegaGTE::GETexture::Texture2D};
                    texDesc.usage = OmegaGTE::GETexture::ToGPU;
                    texDesc.width = _params.img->header.width;
                    texDesc.height = _params.img->header.height;
                    std::cout << "TEX W:" << texDesc.width << "TEX H:" << texDesc.height << std::endl;
                    texturePaint = gte.graphicsEngine->makeTexture(texDesc);
                    texturePaint->copyBytes((void *)_params.img->data,_params.img->header.stride);
                }

                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeTexture2D(_params.rect.w,_params.rect.h));

                result = tessellationEngineContext->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::RoundedRect : {
                auto & _params = ((VisualCommandParams*)params)->roundedRectParams;
                if (_params.brush == nullptr) return;
                auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(_params.rect);

                useTextureRenderPipeline = !_params.brush->isColor;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);

                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }
                result = tessellationEngineContext->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Ellipse : {
                auto & _params = ((VisualCommandParams*)params)->ellipseParams;
                const float cx = _params.ellipse.x;
                const float cy = _params.ellipse.y;
                const float rx = std::max(0.0f,_params.ellipse.rad_x);
                const float ry = std::max(0.0f,_params.ellipse.rad_y);
                if(rx <= 0.0f || ry <= 0.0f){
                    return;
                }

                auto color = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->isColor){
                    color = OmegaGTE::makeColor(_params.brush->color.r,
                                                _params.brush->color.g,
                                                _params.brush->color.b,
                                                _params.brush->color.a);
                }
                if(color[0][0] == 0.f && color[1][0] == 0.f && color[2][0] == 0.f && color[3][0] == 0.f)
                    color = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);

                auto toNdcPoint = [&](float px,float py){
                    return OmegaGTE::GPoint3D{
                            ((2.0f * px) / viewPort.width) - 1.0f,
                            ((2.0f * py) / viewPort.height) - 1.0f,
                            0.0f};
                };

                OmegaGTE::TETriangulationResult::TEMesh mesh {OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangle};
                const auto center = toNdcPoint(cx,cy);

                const float twoPi = static_cast<float>(2.0 * OmegaGTE::PI);
                const unsigned segmentCount = std::min(4096u, std::max(
                        96u,
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * renderScale))));
                auto prev = toNdcPoint(cx + rx,cy);

                for(unsigned i = 1; i <= segmentCount; i++){
                    const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segmentCount);
                    const float px = cx + (std::cos(angle) * rx);
                    const float py = cy + (std::sin(angle) * ry);
                    auto next = toNdcPoint(px,py);

                    OmegaGTE::TETriangulationResult::TEMesh::Polygon tri {};
                    tri.a.pt = center;
                    tri.b.pt = prev;
                    tri.c.pt = next;
                    tri.a.attachment = tri.b.attachment = tri.c.attachment =
                            std::make_optional<OmegaGTE::TETriangulationResult::AttachmentData>(
                                    OmegaGTE::TETriangulationResult::AttachmentData{
                                            color,
                                            OmegaGTE::FVec<2>::Create(),
                                            OmegaGTE::FVec<3>::Create()});

                    mesh.vertexPolygons.push_back(tri);
                    prev = next;
                }

                result.meshes.push_back(mesh);

                break;
            }
            case VisualCommand::VectorPath : {
                auto & _params = ((VisualCommandParams*)params)->pathParams;
                if(_params.path == nullptr || _params.path->size() < 2){
                    return;
                }
                auto te_params = OmegaGTE::TETriangulationParams::GraphicsPath2D(*_params.path,
                                                                                 _params.strokeWidth,
                                                                                 _params.contour,
                                                                                 _params.fill);
                // First attachment: stroke color.
                auto strokeColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->isColor){
                    strokeColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                      _params.brush->color.g,
                                                      _params.brush->color.b,
                                                      _params.brush->color.a);
                }
                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(strokeColor));

                // Second attachment: fill color.
                if(_params.fill && _params.fillBrush != nullptr && _params.fillBrush->isColor){
                    auto fillColor = OmegaGTE::makeColor(_params.fillBrush->color.r,
                                                         _params.fillBrush->color.g,
                                                         _params.fillBrush->color.b,
                                                         _params.fillBrush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(fillColor));
                }

                result = tessellationEngineContext->triangulateSync(te_params,
                                                                  OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                                  &viewPort);
                break;
            }
            case VisualCommand::Shadow: {
                auto & _params = ((VisualCommandParams*)params)->shadowParams;
                const auto & shadow = _params.shadow;

                // Offset and expand the shape rect by blurAmount.
                float expand = std::max(0.f,shadow.blurAmount);
                Core::Rect shadowRect {
                    Core::Position{
                        _params.shapeRect.pos.x + shadow.x_offset - expand,
                        _params.shapeRect.pos.y + shadow.y_offset - expand
                    },
                    std::max(1.f,_params.shapeRect.w + expand * 2.f),
                    std::max(1.f,_params.shapeRect.h + expand * 2.f)
                };

                auto shadowColor = OmegaGTE::makeColor(shadow.color.r,
                                                         shadow.color.g,
                                                         shadow.color.b,
                                                         shadow.color.a * shadow.opacity);

                if(_params.isEllipse){
                    // Tessellate as ellipse.
                    float cx = shadowRect.pos.x + shadowRect.w * 0.5f;
                    float cy = shadowRect.pos.y + shadowRect.h * 0.5f;
                    float rx = shadowRect.w * 0.5f;
                    float ry = shadowRect.h * 0.5f;

                    auto toNdcPoint = [&](float px,float py){
                        return OmegaGTE::GPoint3D{
                                ((2.0f * px) / viewPort.width) - 1.0f,
                                ((2.0f * py) / viewPort.height) - 1.0f,
                                0.0f};
                    };

                    OmegaGTE::TETriangulationResult::TEMesh mesh {OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangle};
                    const auto center = toNdcPoint(cx,cy);
                    const unsigned segCount = std::min(4096u,std::max(96u,
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * renderScale))));
                    auto prev = toNdcPoint(cx + rx,cy);
                    const float twoPi = static_cast<float>(2.0 * OmegaGTE::PI);

                    for(unsigned i = 1; i <= segCount; i++){
                        const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segCount);
                        auto next = toNdcPoint(cx + std::cos(angle) * rx,cy + std::sin(angle) * ry);

                        OmegaGTE::TETriangulationResult::TEMesh::Polygon tri {};
                        tri.a.pt = center; tri.b.pt = prev; tri.c.pt = next;
                        tri.a.attachment = tri.b.attachment = tri.c.attachment =
                            std::make_optional<OmegaGTE::TETriangulationResult::AttachmentData>(
                                OmegaGTE::TETriangulationResult::AttachmentData{
                                    shadowColor,OmegaGTE::FVec<2>::Create(),OmegaGTE::FVec<3>::Create()});
                        mesh.vertexPolygons.push_back(tri);
                        prev = next;
                    }
                    result.meshes.push_back(mesh);
                }
                else if(_params.cornerRadius > 0.f){
                    Core::RoundedRect rr {};
                    rr.pos = shadowRect.pos;
                    rr.w = shadowRect.w;
                    rr.h = shadowRect.h;
                    rr.rad_x = std::min(_params.cornerRadius + expand,shadowRect.w * 0.5f);
                    rr.rad_y = rr.rad_x;
                    auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(rr);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = tessellationEngineContext->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                else {
                    auto te_params = OmegaGTE::TETriangulationParams::Rect(shadowRect);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = tessellationEngineContext->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                break;
            }
            case VisualCommand::SetTransform: {
                auto & _params = ((VisualCommandParams*)params)->transformMatrix;
                currentTransform = _params;
                return;
            }
            case VisualCommand::SetOpacity: {
                currentOpacity = ((VisualCommandParams*)params)->opacityValue;
                return;
            }
            case VisualCommand::Text:
            default:
                return;
        }

        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:after_switch\",\"message\":\"after switch\",\"data\":{\"vertexCount\":"
                  << result.totalVertexCount() << "},\"timestamp\":" << ts << ",\"hypothesisId\":\"B\"}\n";
            }
        }
        // #endregion

        if(result.totalVertexCount() == 0){
            return;
        }
        if(useTextureRenderPipeline){
            if(textureRenderPipelineState == nullptr){
                std::cout << "Texture render pipeline unavailable. Skipping textured draw command." << std::endl;
                return;
            }
            struct_size = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT2});
        }
        else {
            if(renderPipelineState == nullptr){
                std::cout << "Color render pipeline unavailable. Skipping draw command." << std::endl;
                return;
            }
            struct_size = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT4});
        }

        std::size_t requiredBytes = result.totalVertexCount() * struct_size;
        SharedHandle<OmegaGTE::GEBuffer> buffer;
        if(bufferPool){
            buffer = bufferPool->acquire(requiredBytes, struct_size);
        }
        else {
            OmegaGTE::BufferDescriptor bufferDesc {OmegaGTE::BufferDescriptor::Upload,requiredBytes,struct_size};
            buffer = gte.graphicsEngine->makeBuffer(bufferDesc);
        }

        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:buffer_ready\",\"message\":\"buffer ready\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"C\"}\n";
            }
        }
        // #endregion

        bufferWriter->setOutputBuffer(buffer);

        // #region agent log
        { std::ofstream f("../../../debug-85f774.log", std::ios::app); if (f) { auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:after_setOutputBuffer\",\"message\":\"after setOutputBuffer\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"C1\"}\n"; } }
        // #endregion

        auto cb = preEffectTarget->commandBuffer();

        if(textureFence != nullptr){
            preEffectTarget->notifyCommandBuffer(cb,textureFence);
        }
        
        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};

        OmegaGTE::GEViewport viewport {};
        viewport.x = 0;
        viewport.y = 0;
        viewport.farDepth = 1.f;
        viewport.nearDepth = 0.f;
        viewport.width = static_cast<float>(backingWidth);
        viewport.height = static_cast<float>(backingHeight);
        OmegaGTE::GEScissorRect scissorRect {
                0,
                0,
                static_cast<float>(backingWidth),
                static_cast<float>(backingHeight)};

        renderPassDesc.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(1.f,1.f,1.f,1.f),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadPreserve);

        // #region agent log
        { std::ofstream f("../../../debug-85f774.log", std::ios::app); if (f) { auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:before_vertex_loop\",\"message\":\"before vertex loop\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"C2\"}\n"; } }
        // #endregion

        unsigned startVertexIndex = 0;

        const bool hasTransform = !(currentTransform == OmegaGTE::FMatrix<4,4>::Identity());
        const float opacityMul = currentOpacity;

        auto applyTransform = [&](OmegaGTE::FVec<4> & pos){
            if(hasTransform){
                pos = currentTransform * pos;
            }
        };

        auto writeColorVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<4> color){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
            applyTransform(pos);
            if(opacityMul < 1.f){
                color[3][0] *= opacityMul;
            }
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat4(color);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };

         auto writeTexVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<2> coord){
            auto normalizedCoord = OmegaGTE::FVec<2>::Create();
            float u = coord[0][0];
            float v = coord[1][0];
            if((u < 0.f || u > 1.f || v < 0.f || v > 1.f) &&
               textureCoordDenomW > 0.f &&
               textureCoordDenomH > 0.f){
                u /= textureCoordDenomW;
                v /= textureCoordDenomH;
            }
            u = std::clamp(u,0.f,1.f);
            v = std::clamp(v,0.f,1.f);
            normalizedCoord[0][0] = u;
            normalizedCoord[1][0] = v;
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
            applyTransform(pos);
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat2(normalizedCoord);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        };


        const auto fallbackColor = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
        auto fallbackTexCoord = OmegaGTE::FVec<2>::Create();
        fallbackTexCoord[0][0] = 0.f;
        fallbackTexCoord[1][0] = 0.f;

        for(auto & m : result.meshes) {
            for(auto & v : m.vertexPolygons){
                if(useTextureRenderPipeline){
                    auto & aCoord = v.a.attachment ? v.a.attachment->texture2Dcoord : fallbackTexCoord;
                    auto & bCoord = v.b.attachment ? v.b.attachment->texture2Dcoord : fallbackTexCoord;
                    auto & cCoord = v.c.attachment ? v.c.attachment->texture2Dcoord : fallbackTexCoord;
                    writeTexVertexToBuffer(v.a.pt,aCoord);
                    writeTexVertexToBuffer(v.b.pt,bCoord);
                    writeTexVertexToBuffer(v.c.pt,cCoord);
                }
                else {
                    auto useColor = [&fallbackColor](const std::optional<OmegaGTE::TETriangulationResult::AttachmentData> &att) -> OmegaGTE::FVec<4> {
                        if (!att) return fallbackColor;
                        const auto &c = att->color;
                        if (c[0][0] == 0.f && c[1][0] == 0.f && c[2][0] == 0.f && c[3][0] == 0.f)
                            return fallbackColor;
                        return c;
                    };
                    OmegaGTE::FVec<4> aColor = useColor(v.a.attachment);
                    OmegaGTE::FVec<4> bColor = useColor(v.b.attachment);
                    OmegaGTE::FVec<4> cColor = useColor(v.c.attachment);
                    writeColorVertexToBuffer(v.a.pt, aColor);
                    writeColorVertexToBuffer(v.b.pt, bColor);
                    writeColorVertexToBuffer(v.c.pt, cColor);
                }
            }
        }

        // #region agent log
        {
            std::ofstream f("../../../debug-85f774.log", std::ios::app);
            if (f) {
                auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"85f774\",\"location\":\"RenderTarget.cpp:before_startRenderPass\",\"message\":\"before startRenderPass\",\"data\":{},\"timestamp\":" << ts << ",\"hypothesisId\":\"E\"}\n";
            }
        }
        // #endregion

        std::cout << "[WTK Diag] startRenderPass" << std::endl;
        cb->startRenderPass(renderPassDesc);
        if(useTextureRenderPipeline){
            cb->setRenderPipelineState(textureRenderPipelineState);
            cb->bindResourceAtVertexShader(buffer,1);
            cb->bindResourceAtFragmentShader(texturePaint,2);
        }
        else {
             cb->setRenderPipelineState(renderPipelineState);
             cb->bindResourceAtVertexShader(buffer,0);
        }
        cb->setViewports({viewport});
        cb->setScissorRects({scissorRect});
        std::cout << "[WTK Diag] drawPolygons" << std::endl;


        for(auto & m : result.meshes){
            OmegaGTE::GERenderTarget::CommandBuffer::PolygonType topology;
            if(m.topology == OmegaGTE::TETriangulationResult::TEMesh::TopologyTriangleStrip){
                topology = OmegaGTE::GERenderTarget::CommandBuffer::TriangleStrip;
            }
            else {
                topology = OmegaGTE::GERenderTarget::CommandBuffer::Triangle;
            }
            cb->drawPolygons(topology, m.vertexCount(), startVertexIndex);
            startVertexIndex += m.vertexCount();
        }
        std::cout << "[WTK Diag] flush+endRenderPass" << std::endl;
        bufferWriter->flush();
        cb->endRenderPass();
        std::cout << "[WTK Diag] submitCommandBuffer" << std::endl;
        preEffectTarget->submitCommandBuffer(cb);
        if(bufferPool && buffer){
            deferredBufferReleases.push_back({std::move(buffer), requiredBytes});
        }
        std::cout << "[WTK Diag] renderToTarget done" << std::endl;
    }

    void RenderTargetStore::cleanTargets(LayerTree *tree){
        if(tree == nullptr)
            return;
        OmegaCommon::Vector<Layer *> liveLayers {};
        tree->collectAllLayers(liveLayers);

        for(auto & storeEntry : store){
            auto & compTarget = storeEntry.second;
            auto surfIt = compTarget.surfaceTargets.begin();
            while(surfIt != compTarget.surfaceTargets.end()){
                bool isLive = false;
                for(auto *liveLayer : liveLayers){
                    if(liveLayer == surfIt->first){
                        isLive = true;
                        break;
                    }
                }
                if(!isLive){
                    surfIt = compTarget.surfaceTargets.erase(surfIt);
                }
                else {
                    ++surfIt;
                }
            }
        }
    }

    void RenderTargetStore::cleanTreeTargets(LayerTree *tree){
        if(tree == nullptr)
            return;
        cleanTargets(tree);
    }

    void RenderTargetStore::removeRenderTarget(const SharedHandle<CompositionRenderTarget> & target){
        auto it = store.find(target);
        if(it != store.end()){
            store.erase(it);
        }
    }

    /// Blits one texture onto the native drawable and presents.
    static void blitAndPresent(SharedHandle<OmegaGTE::GENativeRenderTarget> & nativeTarget,
                               SharedHandle<OmegaGTE::GETexture> & tex,
                               unsigned w, unsigned h){
        auto nativeFormat = nativeTarget->pixelFormat();
        auto finalPipeline = getFinalCopyPipelineForFormat(nativeFormat);
        if(finalPipeline == nullptr){
            auto cb = nativeTarget->commandBuffer();
            OmegaGTE::GERenderTarget::RenderPassDesc rp {};
            cb->startRenderPass(rp);
            cb->endRenderPass();
            nativeTarget->submitCommandBuffer(cb);
            nativeTarget->commitAndPresent();
            return;
        }

        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.depthStencilAttachment.disabled = true;
        renderPassDesc.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment{
                {0.f,0.f,0.f,0.f},
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction::Clear};

        auto cb = nativeTarget->commandBuffer();
        cb->startRenderPass(renderPassDesc);
        cb->setRenderPipelineState(finalPipeline);

        OmegaGTE::GEViewport vp {};
        vp.x = 0.f; vp.y = 0.f;
        vp.nearDepth = 0.f; vp.farDepth = 1.f;
        vp.width = static_cast<float>(w);
        vp.height = static_cast<float>(h);
        OmegaGTE::GEScissorRect sr {0.f, 0.f, static_cast<float>(w), static_cast<float>(h)};
        cb->setViewports({vp});
        cb->setScissorRects({sr});

        cb->bindResourceAtVertexShader(finalTextureDrawBuffer, 1);
        cb->bindResourceAtFragmentShader(tex, 2);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);

        cb->endRenderPass();
        nativeTarget->submitCommandBuffer(cb);
        nativeTarget->commitAndPresent();
    }

    void compositeAndPresentTarget(BackendCompRenderTarget & compTarget){
        compTarget.needsPresent = false;

        auto & nativeTarget = compTarget.viewPresentTarget.nativeTarget;

        // --- Fast path: single-canvas View (no body visuals) ---
        // UIView and SVGView after Phase 1 have exactly one root visual and no children.
        if(compTarget.visualTree->body.empty() && compTarget.visualTree->root != nullptr){
            auto *ctx = &compTarget.visualTree->root->renderTarget;
            auto tex = ctx->getCommittedTexture();
            if(tex == nullptr || nativeTarget == nullptr){
                if(ctx->hasPendingContent){
                    ctx->hasPendingContent = false;
                    ctx->releaseDeferredBuffers();
                }
                return;
            }

            if(ctx->hasPendingContent){
                auto cb = nativeTarget->commandBuffer();
                nativeTarget->notifyCommandBuffer(cb, ctx->getFence());
                nativeTarget->submitCommandBuffer(cb);
            }

            blitAndPresent(nativeTarget, tex, ctx->getBackingWidth(), ctx->getBackingHeight());

            if(ctx->hasPendingContent){
                ctx->hasPendingContent = false;
                ctx->releaseDeferredBuffers();
            }
            return;
        }

        // --- General path: multi-layer compositing ---
        // Collect ALL visuals that have ever been rendered to (committedTexture != null).
        OmegaCommon::Vector<BackendRenderTargetContext *> allContexts;
        OmegaCommon::Vector<BackendRenderTargetContext *> freshlyPending;
        if(compTarget.visualTree->root != nullptr){
            auto *ctx = &compTarget.visualTree->root->renderTarget;
            if(ctx->getCommittedTexture() != nullptr){
                allContexts.push_back(ctx);
            }
            if(ctx->hasPendingContent){
                freshlyPending.push_back(ctx);
            }
        }
        for(auto & visual : compTarget.visualTree->body){
            if(visual != nullptr){
                auto *ctx = &visual->renderTarget;
                if(ctx->getCommittedTexture() != nullptr){
                    allContexts.push_back(ctx);
                }
                if(ctx->hasPendingContent){
                    freshlyPending.push_back(ctx);
                }
            }
        }
        if(allContexts.empty()){
            return;
        }

        if(nativeTarget == nullptr){
            for(auto *ctx : freshlyPending){
                ctx->hasPendingContent = false;
                ctx->releaseDeferredBuffers();
            }
            return;
        }

        auto cb = nativeTarget->commandBuffer();
        for(auto *ctx : freshlyPending){
            nativeTarget->notifyCommandBuffer(cb, ctx->getFence());
        }

        unsigned maxW = 1;
        unsigned maxH = 1;
        for(auto *ctx : allContexts){
            maxW = std::max(maxW, ctx->getBackingWidth());
            maxH = std::max(maxH, ctx->getBackingHeight());
        }

        auto nativeFormat = nativeTarget->pixelFormat();
        auto finalPipeline = getFinalCopyPipelineForFormat(nativeFormat);
        if(finalPipeline == nullptr){
            OmegaGTE::GERenderTarget::RenderPassDesc rp {};
            cb->startRenderPass(rp);
            cb->endRenderPass();
            nativeTarget->submitCommandBuffer(cb);
            nativeTarget->commitAndPresent();
            for(auto *ctx : freshlyPending){
                ctx->hasPendingContent = false;
                ctx->releaseDeferredBuffers();
            }
            return;
        }

        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.depthStencilAttachment.disabled = true;
        renderPassDesc.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment{
                {0.f,0.f,0.f,0.f},
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction::Clear};

        cb->startRenderPass(renderPassDesc);
        cb->setRenderPipelineState(finalPipeline);

        OmegaGTE::GEViewport finalViewport {};
        finalViewport.x = 0.f;
        finalViewport.y = 0.f;
        finalViewport.nearDepth = 0.f;
        finalViewport.farDepth = 1.f;
        finalViewport.width = static_cast<float>(maxW);
        finalViewport.height = static_cast<float>(maxH);
        OmegaGTE::GEScissorRect finalScissorRect {
                0.f, 0.f,
                static_cast<float>(maxW),
                static_cast<float>(maxH)};
        cb->setViewports({finalViewport});
        cb->setScissorRects({finalScissorRect});

        for(auto *ctx : allContexts){
            auto tex = ctx->getCommittedTexture();
            if(tex == nullptr){ continue; }
            cb->bindResourceAtVertexShader(finalTextureDrawBuffer, 1);
            cb->bindResourceAtFragmentShader(tex, 2);
            cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
        }

        cb->endRenderPass();
        nativeTarget->submitCommandBuffer(cb);
        nativeTarget->commitAndPresent();

        for(auto *ctx : freshlyPending){
            ctx->hasPendingContent = false;
            ctx->releaseDeferredBuffers();
        }
    }

    void RenderTargetStore::presentAllPending(){
        for(auto & entry : store){
            auto & compTarget = entry.second;
            if(!compTarget.needsPresent || compTarget.visualTree == nullptr){
                continue;
            }
            compositeAndPresentTarget(compTarget);
        }
    }

    /// Unified cross-platform effect processor using OmegaSL compute shaders.
    class GPUCanvasEffectProcessor : public BackendCanvasEffectProcessor {
    public:
        explicit GPUCanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence):
            BackendCanvasEffectProcessor(fence){}

        void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                          SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                          OmegaCommon::Vector<CanvasEffect> & effects,
                          unsigned width,
                          unsigned height) override {
            if(effects.empty()){
                return;
            }
            auto src = textureTarget->underlyingTexture();
            if(src == nullptr || dest == nullptr){
                return;
            }
            if(width == 0 || height == 0){
                return;
            }

            for(auto & effect : effects){
                switch(effect.type){
                    case CanvasEffect::GaussianBlur: {
                        auto blurH = gaussianBlurHPipelineState;
                        auto blurV = gaussianBlurVPipelineState;
                        if(blurH == nullptr || blurV == nullptr){ break; }
                        float radius = std::max(0.f, effect.gaussianBlur.radius);
                        if(radius <= 0.f){ break; }

                        // BlurParams: float radius, uint texWidth, uint texHeight, float angle
                        auto structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT,OMEGASL_UINT,OMEGASL_UINT,OMEGASL_FLOAT});
                        OmegaGTE::BufferDescriptor bd {OmegaGTE::BufferDescriptor::Upload,structSize,structSize};
                        auto pb = gte.graphicsEngine->makeBuffer(bd);
                        if(pb == nullptr){ break; }
                        bufferWriter->setOutputBuffer(pb);
                        float angle = 0.f;
                        bufferWriter->structBegin();
                        bufferWriter->writeFloat(radius);
                        bufferWriter->writeUint(width);
                        bufferWriter->writeUint(height);
                        bufferWriter->writeFloat(angle);
                        bufferWriter->structEnd();
                        bufferWriter->sendToBuffer();
                        bufferWriter->flush();

                        unsigned gx = (width + 7) / 8;
                        unsigned gy = (height + 7) / 8;

                        // H pass: src → dest
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(blurH);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(src, 3);
                            cb->bindResourceAtComputeShader(dest, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // V pass: dest → src (ping-pong)
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(blurV);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(dest, 3);
                            cb->bindResourceAtComputeShader(src, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // After H→dest, V→src ping-pong, result is back in src
                        // (the preEffectTarget's underlying texture), which commit()
                        // uses as committedTexture.
                        break;
                    }
                    case CanvasEffect::DirectionalBlur: {
                        auto dirPipe = directionalBlurPipelineState;
                        if(dirPipe == nullptr){ break; }
                        float radius = std::max(0.f, effect.directionalBlur.radius);
                        if(radius <= 0.f){ break; }

                        float dirAngle = effect.directionalBlur.angle;

                        auto structSize = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT,OMEGASL_UINT,OMEGASL_UINT,OMEGASL_FLOAT});
                        OmegaGTE::BufferDescriptor bd {OmegaGTE::BufferDescriptor::Upload,structSize,structSize};
                        auto pb = gte.graphicsEngine->makeBuffer(bd);
                        if(pb == nullptr){ break; }
                        bufferWriter->setOutputBuffer(pb);
                        bufferWriter->structBegin();
                        bufferWriter->writeFloat(radius);
                        bufferWriter->writeUint(width);
                        bufferWriter->writeUint(height);
                        bufferWriter->writeFloat(dirAngle);
                        bufferWriter->structEnd();
                        bufferWriter->sendToBuffer();
                        bufferWriter->flush();

                        unsigned gx = (width + 7) / 8;
                        unsigned gy = (height + 7) / 8;

                        // Pass 1: directional blur src→dest
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(dirPipe);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(src, 3);
                            cb->bindResourceAtComputeShader(dest, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // Pass 2: copy dest→src using H blur with zero radius (identity)
                        {
                            auto pb2 = gte.graphicsEngine->makeBuffer(bd);
                            if(pb2 != nullptr){
                                float zeroRadius = 0.f;
                                float zeroAngle = 0.f;
                                bufferWriter->setOutputBuffer(pb2);
                                bufferWriter->structBegin();
                                bufferWriter->writeFloat(zeroRadius);
                                bufferWriter->writeUint(width);
                                bufferWriter->writeUint(height);
                                bufferWriter->writeFloat(zeroAngle);
                                bufferWriter->structEnd();
                                bufferWriter->sendToBuffer();
                                bufferWriter->flush();

                                auto blurH = gaussianBlurHPipelineState;
                                if(blurH != nullptr){
                                    auto cb2 = textureTarget->commandBuffer();
                                    cb2->startComputePass(blurH);
                                    cb2->bindResourceAtComputeShader(pb2, 5);
                                    cb2->bindResourceAtComputeShader(dest, 3);
                                    cb2->bindResourceAtComputeShader(src, 4);
                                    cb2->dispatchThreadgroups(gx, gy, 1);
                                    cb2->endComputePass();
                                    textureTarget->submitCommandBuffer(cb2);
                                }
                            }
                        }
                        break;
                    }
                }
            }

            // Fence synchronization for the composite pass.
            auto cb = textureTarget->commandBuffer();
            textureTarget->submitCommandBuffer(cb, fence);
            textureTarget->commit();
        }
    };

    SharedHandle<BackendCanvasEffectProcessor>
    BackendCanvasEffectProcessor::Create(SharedHandle<OmegaGTE::GEFence> & fence){
        return SharedHandle<BackendCanvasEffectProcessor>(new GPUCanvasEffectProcessor(fence));
    }

}
