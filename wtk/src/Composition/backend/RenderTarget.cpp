

#include "RenderTarget.h"
#include "VisualTree.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "MainThreadDispatch.h"
#include "Pipeline.h"
#include "Texture.h"
#include "ResourceFactory.h"
#include "omegaWTK/Composition/Canvas.h"
#include "GeometryConvert.h"
#include "ResourceTrace.h"

#include "omegaWTK/Media/ImgCodec.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <memory>
#include <sstream>
#include <utility>

namespace OmegaWTK::Composition {
    #ifdef TARGET_MACOS
    void stopMTLCapture();
    #endif

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
        inline BufferPool * bufferPool(){
            return BackendResourceFactory::instance().bufferPool();
        }
        inline FencePool * fencePool(){
            return BackendResourceFactory::instance().fencePool();
        }
    }

    void InitializeEngine(){
        BackendResourceFactory::instance().pipelines().initialize();
        BackendResourceFactory::instance().initializePools();
    }

    void CleanupEngine(){
        BackendResourceFactory::instance().shutdownPools();
        BackendResourceFactory::instance().pipelines().shutdown();
    }

BackendRenderTargetContext::BackendRenderTargetContext(Composition::Rect & rect,
        SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTargetIn,
        float renderScaleValue):
        fence(fencePool() ? fencePool()->acquire() : gte.graphicsEngine->makeFence()),
        renderTarget(renderTargetIn),
        textures_(rect, renderScaleValue, renderTargetIn),
        frameRenderPass_(textures_, renderTarget)
        {
    traceResourceId = ResourceTrace::nextResourceId();
    ResourceTrace::emit("Create",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        textures_.renderTargetSize().w,
                        textures_.renderTargetSize().h,
                        textures_.renderScale());
    rebuildBackingTarget();
    imageProcessor = BackendResourceFactory::instance().createEffectProcessor(fence);
}

void BackendRenderTargetContext::rebuildBackingTarget(){
    textures_.recomputeBackingDimensions();
    ResourceTrace::emit("ResizeRebuild",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        static_cast<float>(textures_.backingWidth()),
                        static_cast<float>(textures_.backingHeight()),
                        textures_.renderScale());
    textures_.rebuild();
}

BackendRenderTargetContext::~BackendRenderTargetContext(){
    ResourceTrace::emit("Destroy",
                        "TextureTarget",
                        traceResourceId,
                        "BackendRenderTargetContext",
                        this,
                        textures_.renderTargetSize().w,
                        textures_.renderTargetSize().h,
                        textures_.renderScale());
    // textures_ destructor will release pool textures (with the Win32
    // waitForGPU dance) once this body returns.
    imageProcessor.reset();
    for(auto & entry : deferredBufferReleases){
        if(bufferPool() && entry.first){
            bufferPool()->release(std::move(entry.first), entry.second);
        }
    }
    deferredBufferReleases.clear();
    if(fencePool() && fence){
        fencePool()->release(std::move(fence));
    }
}

    void BackendRenderTargetContext::setRenderTargetSize(Composition::Rect &rect) {
        if(textures_.resizeLogical(rect)){
            rebuildBackingTarget();
        }
    }

    void BackendRenderTargetContext::setViewportOverride(float offsetX,float offsetY,float width,float height){
        frameRenderPass_.setViewportOverride(offsetX, offsetY, width, height);
        // Update logical size for tessellation; grow backing surface if the
        // requested viewport extent exceeds the current backing. Never
        // shrinks.
        if(textures_.applyViewportOverride(offsetX, offsetY, width, height)){
            rebuildBackingTarget();
        }
    }

    void BackendRenderTargetContext::clearViewportOverride(){
        frameRenderPass_.clearViewportOverride();
    }

#ifdef _WIN32
void BackendRenderTargetContext::resizeSwapChain(unsigned int newBackingWidth, unsigned int newBackingHeight) {
    if (renderTarget != nullptr)
        renderTarget->resizeSwapChain(newBackingWidth, newBackingHeight);
}
void BackendRenderTargetContext::waitForGPU() {
    if (renderTarget != nullptr)
        renderTarget->waitForGPU();
}
#endif

void BackendRenderTargetContext::clear(float r, float g, float b, float a) {
    frameRenderPass_.clearOnce(r, g, b, a);
}

void BackendRenderTargetContext::beginFrame(float clearR, float clearG, float clearB, float clearA) {
    frameRenderPass_.begin(clearR, clearG, clearB, clearA, !effectQueue.empty());
}

void BackendRenderTargetContext::endFrame() {
    frameRenderPass_.end();
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
        // Direct-to-native path: rendering already went to the native
        // drawable in beginFrame/endFrame.  Just present.
        if(frameRenderPass_.renderingToNative()){
            renderTarget->commitAndPresent();
            frameRenderPass_.clearRenderingToNative();
            if(completionHandler){
                BackendSubmissionTelemetry telemetry {};
                telemetry.syncLaneId = syncLaneId;
                telemetry.syncPacketId = syncPacketId;
                telemetry.submitTimeCpu = submitTimeCpu;
                telemetry.completeTimeCpu = std::chrono::steady_clock::now();
                telemetry.presentTimeCpu = telemetry.completeTimeCpu;
                telemetry.status = BackendSubmissionStatus::Completed;
                completionHandler(telemetry);
            }
            return;
        }

        // Texture (effect) path — unchanged from before.
        if(textures_.preEffectTarget() == nullptr){
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
        auto _l_cb = textures_.preEffectTarget()->commandBuffer();
        const bool canApplyEffects = !effectQueue.empty() &&
                                     imageProcessor != nullptr &&
                                     textures_.effectTexture() != nullptr &&
                                     textures_.effectTarget() != nullptr;
#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "[WTK Diag] commit: canApplyEffects=" << canApplyEffects << std::endl;
#endif
        if(canApplyEffects){
            textures_.preEffectTarget()->submitCommandBuffer(_l_cb);
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] commit: textures_.preEffectTarget()->commit()" << std::endl;
#endif
            textures_.preEffectTarget()->commit();
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] commit: applyEffects" << std::endl;
#endif
            imageProcessor->applyEffects(textures_.effectTexture(),textures_.preEffectTarget(),effectQueue,textures_.backingWidth(),textures_.backingHeight());
            textures_.preEffectTarget()->waitForGPU();
            textures_.preEffectTarget()->signalFence(fence);
        } else {
            textures_.preEffectTarget()->submitCommandBuffer(_l_cb, fence);
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] commit: textures_.preEffectTarget()->commit()" << std::endl;
#endif
            textures_.preEffectTarget()->commit();
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "[WTK Diag] commit: textures_.preEffectTarget()->commit() done" << std::endl;
#endif
        }
        effectQueue.clear();

        // Effect path: blit the result to the native drawable and present.
        textures_.presentBlit(fence);
    }

    void BackendRenderTargetContext::releaseDeferredBuffers(){
        if(bufferPool()){
            for(auto & entry : deferredBufferReleases){
                if(entry.first)
                    bufferPool()->release(std::move(entry.first), entry.second);
            }
            deferredBufferReleases.clear();
        }
    }

    typedef decltype(VisualCommand::params) VisualCommandParams;

    void BackendRenderTargetContext::renderToTarget(VisualCommand::Type type, void *params) {
        auto & pipelines = pipelineRegistry();
        auto bufferWriter = pipelines.bufferWriter();
        auto renderPipelineState = pipelines.color();
        auto textureRenderPipelineState = pipelines.texture();
        if(bufferWriter == nullptr || textures_.preEffectTarget() == nullptr || textures_.tessellationContext() == nullptr){
            return;
        }
        OmegaGTE::TETriangulationResult result;

        OmegaGTE::GEViewport viewPort {};
        viewPort.x = viewPort.y = viewPort.nearDepth = 0.f;
        viewPort.farDepth = 1.f;
        viewPort.width = textures_.renderTargetSize().w;
        viewPort.height = textures_.renderTargetSize().h;

#ifdef OMEGAWTK_TRACE_RENDER
        std::cout << "W:" << textures_.renderTargetSize().w
                  << " H:" << textures_.renderTargetSize().h
                  << " BW:" << textures_.backingWidth()
                  << " BH:" << textures_.backingHeight()
                  << " S:" << textures_.renderScale()
                  << std::endl;
#endif

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
                auto gteRect = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::Rect(gteRect);

                switch (_params.brush->type) {
                    case Brush::Type::Color:    useTextureRenderPipeline = false; break;
                    case Brush::Type::Gradient: useTextureRenderPipeline = true;  break;
                    case Brush::Type::None:     return;
                }
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);
                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }

                result = textures_.tessellationContext()->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::Bitmap : {
                auto & _params = ((VisualCommandParams*)params)->bitmapParams;
                auto gteBmpRect = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::Rect(gteBmpRect);

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
#ifdef OMEGAWTK_TRACE_RENDER
                    std::cout << "TEX W:" << texDesc.width << "TEX H:" << texDesc.height << std::endl;
#endif
                    texturePaint = gte.graphicsEngine->makeTexture(texDesc);
                    texturePaint->copyBytes((void *)_params.img->data,_params.img->header.stride);
                }

                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeTexture2D(uint32_t(_params.rect.w),uint32_t(_params.rect.h)));

                result = textures_.tessellationContext()->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

                break;
            }
            case VisualCommand::RoundedRect : {
                auto & _params = ((VisualCommandParams*)params)->roundedRectParams;
                if (_params.brush == nullptr) return;
                auto gteRR = toGTE(_params.rect);
                auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(gteRR);

                switch (_params.brush->type) {
                    case Brush::Type::Color:    useTextureRenderPipeline = false; break;
                    case Brush::Type::Gradient: useTextureRenderPipeline = true;  break;
                    case Brush::Type::None:     return;
                }
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);

                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(color));
                }
                result = textures_.tessellationContext()->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);

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
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
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
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * textures_.renderScale()))));
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
                if(_params.brush != nullptr && _params.brush->type == Brush::Type::Color){
                    strokeColor = OmegaGTE::makeColor(_params.brush->color.r,
                                                      _params.brush->color.g,
                                                      _params.brush->color.b,
                                                      _params.brush->color.a);
                }
                te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(strokeColor));

                // Second attachment: fill color.
                if(_params.fill && _params.fillBrush != nullptr && _params.fillBrush->type == Brush::Type::Color){
                    auto fillColor = OmegaGTE::makeColor(_params.fillBrush->color.r,
                                                         _params.fillBrush->color.g,
                                                         _params.fillBrush->color.b,
                                                         _params.fillBrush->color.a);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(fillColor));
                }

                result = textures_.tessellationContext()->triangulateSync(te_params,
                                                                  OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                                  &viewPort);
                break;
            }
            case VisualCommand::Shadow: {
                auto & _params = ((VisualCommandParams*)params)->shadowParams;
                const auto & shadow = _params.shadow;

                // Offset and expand the shape rect by blurAmount.
                float expand = std::max(0.f,shadow.blurAmount);
                Composition::Rect shadowRect {
                    Composition::Point2D{
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
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * textures_.renderScale()))));
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
                    Composition::RoundedRect rr {};
                    rr.pos = shadowRect.pos;
                    rr.w = shadowRect.w;
                    rr.h = shadowRect.h;
                    rr.rad_x = std::min(_params.cornerRadius + expand,shadowRect.w * 0.5f);
                    rr.rad_y = rr.rad_x;
                    auto gteRR_s = toGTE(rr);
                    auto te_params = OmegaGTE::TETriangulationParams::RoundedRect(gteRR_s);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = textures_.tessellationContext()->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                else {
                    auto gteShadowRect = toGTE(shadowRect);
                    auto te_params = OmegaGTE::TETriangulationParams::Rect(gteShadowRect);
                    te_params.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(shadowColor));
                    result = textures_.tessellationContext()->triangulateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                }
                break;
            }
            case VisualCommand::SetTransform: {
                auto & _params = ((VisualCommandParams*)params)->transformMatrix;
                currentTransform = toGTEMatrix(_params);
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

        if(result.totalVertexCount() == 0){
            return;
        }
        if(useTextureRenderPipeline){
            if(textureRenderPipelineState == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
                std::cout << "Texture render pipeline unavailable. Skipping textured draw command." << std::endl;
#endif
                return;
            }
            struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT2,OMEGASL_FLOAT2});
        }
        else {
            if(renderPipelineState == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
                std::cout << "Color render pipeline unavailable. Skipping draw command." << std::endl;
#endif
                return;
            }
            struct_size = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT4,OMEGASL_FLOAT4});
        }

        std::size_t requiredBytes = result.totalVertexCount() * struct_size;
        SharedHandle<OmegaGTE::GEBuffer> buffer;
        if(bufferPool()){
            buffer = bufferPool()->acquire(requiredBytes, struct_size);
        }
        else {
            OmegaGTE::BufferDescriptor bufferDesc {OmegaGTE::BufferDescriptor::Upload,requiredBytes,struct_size};
            buffer = gte.graphicsEngine->makeBuffer(bufferDesc);
        }

        bufferWriter->setOutputBuffer(buffer);

        // Acquire the command buffer for this draw. When a frame is open the
        // scope wraps the frame's CB; otherwise the scope opens a standalone
        // render pass on the offscreen target. Texture-fence handling (mid-
        // frame restart vs standalone notify) is bundled into beginDraw().
        auto scope = frameRenderPass_.beginDraw(textureFence);
        auto & cb = scope.cb;

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
            auto texPad = OmegaGTE::FVec<2>::Create();
            texPad[0][0] = 0.f; texPad[1][0] = 0.f;
            bufferWriter->structBegin();
            bufferWriter->writeFloat4(pos);
            bufferWriter->writeFloat2(normalizedCoord);
            bufferWriter->writeFloat2(texPad);
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

        // Flush vertex data before draw calls
        bufferWriter->flush();

        // Bind pipeline and resources. The frame render pass suppresses
        // redundant pipeline binds within an open frame and forces a rebind
        // for standalone scopes.
        if(useTextureRenderPipeline){
            frameRenderPass_.bindTexturePipeline(scope);
            cb->bindResourceAtVertexShader(buffer,1);
            cb->bindResourceAtFragmentShader(texturePaint,2);
        }
        else {
            frameRenderPass_.bindColorPipeline(scope);
            cb->bindResourceAtVertexShader(buffer,0);
        }

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

        frameRenderPass_.endDraw(scope);

        if(bufferPool() && buffer){
            deferredBufferReleases.push_back({std::move(buffer), requiredBytes});
        }
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

}
