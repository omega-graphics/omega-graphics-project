

#include "RenderTarget.h"
#include "omegaWTK/Composition/Canvas.h"

#include "omegaWTK/Media/ImgCodec.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace OmegaWTK::Composition {
    #ifdef TARGET_MACOS
    void stopMTLCapture();
    #endif

    static SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary;
    static SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter;
    static SharedHandle<OmegaGTE::GERenderPipelineState> renderPipelineState;
    static SharedHandle<OmegaGTE::GERenderPipelineState> textureRenderPipelineState;
    static SharedHandle<OmegaGTE::GERenderPipelineState> finalCopyRenderPipelineState;

    static SharedHandle<OmegaGTE::GEComputePipelineState> linearGradientPipelineState;

    OmegaCommon::String librarySource = R"(

struct GradientTextureConstParams {
    float arg;
};

struct LinearGradientStop {
    float pos;
    float4 color;
};

buffer<GradientTextureConstParams> input : 5;

buffer<LinearGradientStop> stops : 3;
texture2d outputTex : 4;

// [in input,in stops,out outputTex]
// compute(x=1,y=1,z=1)
// void linearGradient(uint3 thread_id : GlobalThreadID){
//
// }

 struct OmegaWTKColoredVertex {
   float4 pos;
   float4 color;
 };

 struct OmegaWTKColoredRasterData internal {
   float4 pos : Position;
   float4 color : Color;
 };

buffer<OmegaWTKColoredVertex> v_buffer : 0;

[in v_buffer]
vertex OmegaWTKColoredRasterData mainVertex(uint v_id : VertexID){
    OmegaWTKColoredVertex v = v_buffer[v_id];
    OmegaWTKColoredRasterData rasterData;
    rasterData.pos = v.pos;
    rasterData.color = v.color;
    return rasterData;
}

fragment float4 mainFragment(OmegaWTKColoredRasterData raster){
    return raster.color;
}

struct OmegaWTKTexturedVertex {
    float4 pos;
    float2 texCoord;
};

struct OmegaWTKTexturedRasterData internal {
    float4 pos : Position;
    float2 texCoord : TexCoord;
};

buffer<OmegaWTKTexturedVertex> v_buffer_1 : 1;

[in v_buffer_1]
vertex OmegaWTKTexturedRasterData textureVertex(uint v_id : VertexID){
    OmegaWTKTexturedVertex v = v_buffer_1[v_id];
    OmegaWTKTexturedRasterData rasterData;
    rasterData.pos = v.pos;
    rasterData.texCoord = v.texCoord;
    return rasterData;
}

texture2d tex : 2;
static sampler2d mainSampler(filter=linear);

[in tex,in mainSampler]
fragment float4 textureFragment(OmegaWTKTexturedRasterData raster){
    return sample(mainSampler,tex,raster.texCoord);
}

struct OmegaWTKCopyRasterData internal {
    float4 pos : Position;
    float2 texCoord : TexCoord;
};

[in v_buffer_1]
vertex OmegaWTKCopyRasterData copyVertex(uint v_id : VertexID){
    OmegaWTKTexturedVertex v = v_buffer_1[v_id];
    OmegaWTKCopyRasterData rasterData;
    rasterData.pos = v.pos;
    rasterData.texCoord = v.texCoord;
    return rasterData;
}

[in tex,in mainSampler]
fragment float4 copyFragment(OmegaWTKCopyRasterData raster){
    return sample(mainSampler,tex,raster.texCoord);
}

)";

    static SharedHandle<OmegaGTE::GEBuffer> finalTextureDrawBuffer;

    void loadGlobalRenderAssets(){
        bufferWriter = OmegaGTE::GEBufferWriter::Create();
        auto & compiler = gte.omegaSlCompiler;
        auto library = compiler->compile({OmegaSLCompiler::Source::fromString(librarySource)});
        shaderLibrary = gte.graphicsEngine->loadShaderLibraryRuntime(library);
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

        OMEGAWTK_DEBUG("Phase 2");

        renderPipelineDescriptor.vertexFunc = getShader("textureVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("textureFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            textureRenderPipelineState = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
        }
        else {
            textureRenderPipelineState.reset();
            std::cout << "Texture render pipeline is unavailable." << std::endl;
        }

        renderPipelineDescriptor.vertexFunc = getShader("copyVertex");
        renderPipelineDescriptor.fragmentFunc = getShader("copyFragment");
        if(renderPipelineDescriptor.vertexFunc != nullptr && renderPipelineDescriptor.fragmentFunc != nullptr){
            finalCopyRenderPipelineState = gte.graphicsEngine->makeRenderPipelineState(renderPipelineDescriptor);
        }
        else {
            finalCopyRenderPipelineState.reset();
            std::cout << "Final copy pipeline is unavailable." << std::endl;
        }

        OMEGAWTK_DEBUG("Phase 3");

//        OmegaGTE::ComputePipelineDescriptor linearGradientPipelineDesc {};
//        linearGradientPipelineDesc.computeFunc = shaderLibrary->shaders["linearGradient"];
//        linearGradientPipelineState = gte.graphicsEngine->makeComputePipelineState(linearGradientPipelineDesc);
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
        shaderLibrary.reset();
        renderPipelineState.reset();
        textureRenderPipelineState.reset();
        finalCopyRenderPipelineState.reset();
        linearGradientPipelineState.reset();
        bufferWriter.reset();
        finalTextureDrawBuffer.reset();
    }

    void InitializeEngine(){
        loadGlobalRenderAssets();
    }

    void CleanupEngine(){
        destroyGlobalRenderAssets();
    }

BackendRenderTargetContext::BackendRenderTargetContext(Core::Rect & rect,
        SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTarget,
        float renderScale):
        fence(gte.graphicsEngine->makeFence()),
        renderTarget(renderTarget),
        renderTargetSize(rect),
        renderScale(std::max(1.0f,renderScale))
        {
    rebuildBackingTarget();
    imageProcessor = BackendCanvasEffectProcessor::Create(fence);
}

void BackendRenderTargetContext::rebuildBackingTarget(){
    const auto logicalW = std::max(1.0f,renderTargetSize.w);
    const auto logicalH = std::max(1.0f,renderTargetSize.h);
    backingWidth = std::max(1u,static_cast<unsigned>(std::lround(logicalW * renderScale)));
    backingHeight = std::max(1u,static_cast<unsigned>(std::lround(logicalH * renderScale)));

    OmegaGTE::TextureDescriptor textureDescriptor {};
    textureDescriptor.usage = OmegaGTE::GETexture::RenderTarget;
    textureDescriptor.storage_opts = OmegaGTE::Shared;
    textureDescriptor.width = backingWidth;
    textureDescriptor.height = backingHeight;
    textureDescriptor.type = OmegaGTE::GETexture::Texture2D;
    textureDescriptor.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;

    targetTexture = gte.graphicsEngine->makeTexture(textureDescriptor);
    effectTexture = gte.graphicsEngine->makeTexture(textureDescriptor);
    preEffectTarget = gte.graphicsEngine->makeTextureRenderTarget({true,targetTexture});
    effectTarget = gte.graphicsEngine->makeTextureRenderTarget({true,effectTexture});
    tessellationEngineContext = gte.tessalationEngine->createTEContextFromTextureRenderTarget(preEffectTarget);
}

    void BackendRenderTargetContext::setRenderTargetSize(Core::Rect &rect) {
        const unsigned oldW = backingWidth;
        const unsigned oldH = backingHeight;

        renderTargetSize = rect;

        const auto newLogicalW = std::max(1.0f,renderTargetSize.w);
        const auto newLogicalH = std::max(1.0f,renderTargetSize.h);
        const unsigned newW = std::max(1u,static_cast<unsigned>(std::lround(newLogicalW * renderScale)));
        const unsigned newH = std::max(1u,static_cast<unsigned>(std::lround(newLogicalH * renderScale)));

        if(oldW == newW && oldH == newH){
            renderTargetSize = rect;
            return;
        }

        rebuildBackingTarget();
    }

void BackendRenderTargetContext::clear(float r, float g, float b, float a) {
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
        auto _l_cb = preEffectTarget->commandBuffer();
        const bool canApplyEffects = !effectQueue.empty() &&
                                     imageProcessor != nullptr &&
                                     effectTexture != nullptr &&
                                     effectTarget != nullptr;
        if(canApplyEffects){
            preEffectTarget->submitCommandBuffer(_l_cb);
        }
        else {
            preEffectTarget->submitCommandBuffer(_l_cb,fence);
        }
        preEffectTarget->commit();

        SharedHandle<OmegaGTE::GETexture> finalTexture = preEffectTarget->underlyingTexture();
        if(canApplyEffects){
            imageProcessor->applyEffects(effectTexture,preEffectTarget,effectQueue);
            finalTexture = effectTexture;
        }
        effectQueue.clear();

        auto cb = renderTarget->commandBuffer();

        renderTarget->notifyCommandBuffer(cb, fence);
        OmegaGTE::GERenderTarget::RenderPassDesc renderPassDesc {};
        renderPassDesc.depthStencilAttachment.disabled = true;

        renderPassDesc.colorAttachment = new OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment{
                {0.f,0.f,0.f,0.f},
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction::Clear};

        if(completionHandler){
            cb->setCompletionHandler(
                    [completionHandler = std::move(completionHandler),
                            syncLaneId,
                            syncPacketId,
                            submitTimeCpu](const OmegaGTE::GECommandBufferCompletionInfo & info){
                        BackendSubmissionTelemetry telemetry {};
                        telemetry.syncLaneId = syncLaneId;
                        telemetry.syncPacketId = syncPacketId;
                        telemetry.submitTimeCpu = submitTimeCpu;
                        telemetry.completeTimeCpu = std::chrono::steady_clock::now();
                        telemetry.presentTimeCpu = telemetry.completeTimeCpu;
                        telemetry.gpuStartTimeSec = info.gpuStartTimeSec;
                        telemetry.gpuEndTimeSec = info.gpuEndTimeSec;
                        telemetry.status = info.status == OmegaGTE::GECommandBufferCompletionInfo::Status::Completed
                                           ? BackendSubmissionStatus::Completed
                                           : BackendSubmissionStatus::Error;
                        completionHandler(telemetry);
                    });
        }
        cb->startRenderPass(renderPassDesc);
        auto finalPipeline = finalCopyRenderPipelineState ? finalCopyRenderPipelineState : textureRenderPipelineState;
        if(finalPipeline == nullptr){
            std::cout << "No final compositing pipeline available." << std::endl;
            cb->endRenderPass();
            renderTarget->submitCommandBuffer(cb);
            renderTarget->commitAndPresent();
            return;
        }
        cb->setRenderPipelineState(finalPipeline);
        cb->bindResourceAtVertexShader(finalTextureDrawBuffer,1);
        cb->bindResourceAtFragmentShader(finalTexture,2);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle,6,0);
        cb->endRenderPass();
        renderTarget->submitCommandBuffer(cb);
        renderTarget->commitAndPresent();

        #ifdef TARGET_MACOS

        stopMTLCapture();
        
        #endif
    }

    void
    BackendRenderTargetContext::createGradientTexture(bool linearOrRadial, Gradient &gradient, OmegaGTE::GRect &rect,
                                                      SharedHandle<OmegaGTE::GETexture> &dest) {
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
        OmegaGTE::TETessellationResult result;

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

        size_t struct_size;
        bool useTextureRenderPipeline = false;
        float textureCoordDenomW = 1.f;
        float textureCoordDenomH = 1.f;

        SharedHandle<OmegaGTE::GETexture> texturePaint;

        SharedHandle<OmegaGTE::GEFence> textureFence;

        switch (type) {
            case VisualCommand::Rect : {
                auto & _params = ((VisualCommandParams*)params)->rectParams;
                OmegaGTE::GRect r{OmegaGTE::GPoint2D {0,0},_params.rect.w,_params.rect.h};
                auto te_params = OmegaGTE::TETessellationParams::Rect(r);

                useTextureRenderPipeline = !_params.brush->isColor;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);

                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETessellationParams::Attachment::makeColor(color));
                }

                result = tessellationEngineContext->tessalateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                result.translate(-((viewPort.width/2) - _params.rect.pos.x),
                                 -((viewPort.height/2) - _params.rect.pos.y),
                                 0,
                                 viewPort);

                break;
            }
            case VisualCommand::Bitmap : {
                auto & _params = ((VisualCommandParams*)params)->bitmapParams;
                OmegaGTE::GRect r{OmegaGTE::GPoint2D {0,0},_params.rect.w,_params.rect.h};
                auto te_params = OmegaGTE::TETessellationParams::Rect(r);

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

                te_params.addAttachment(OmegaGTE::TETessellationParams::Attachment::makeTexture2D(r.w,r.h));

                result = tessellationEngineContext->tessalateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                result.translate(-((viewPort.width/2) - _params.rect.pos.x),
                                 -((viewPort.height/2) - _params.rect.pos.y),
                                 0,
                                 viewPort);

                break;
            }
            case VisualCommand::RoundedRect : {
                auto & _params = ((VisualCommandParams*)params)->roundedRectParams;
                auto te_params = OmegaGTE::TETessellationParams::RoundedRect(_params.rect);

                useTextureRenderPipeline = !_params.brush->isColor;
                textureCoordDenomW = std::max(1.f,_params.rect.w);
                textureCoordDenomH = std::max(1.f,_params.rect.h);

                if(!useTextureRenderPipeline){
                    auto color = OmegaGTE::makeColor(_params.brush->color.r,
                                                     _params.brush->color.g,
                                                     _params.brush->color.b,
                                                     _params.brush->color.a);
                    te_params.addAttachment(OmegaGTE::TETessellationParams::Attachment::makeColor(color));
                }
                result = tessellationEngineContext->tessalateSync(te_params,OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,&viewPort);
                result.translate(-((viewPort.width/2) - _params.rect.pos.x),
                                 -((viewPort.height/2) - _params.rect.pos.y),
                                 0,
                                 viewPort);

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

                auto toNdcPoint = [&](float px,float py){
                    return OmegaGTE::GPoint3D{
                            ((2.0f * px) / viewPort.width) - 1.0f,
                            ((2.0f * py) / viewPort.height) - 1.0f,
                            0.0f};
                };

                OmegaGTE::TETessellationResult::TEMesh mesh {OmegaGTE::TETessellationResult::TEMesh::TopologyTriangle};
                const auto center = toNdcPoint(cx,cy);

                const float twoPi = static_cast<float>(2.0 * OmegaGTE::PI);
                const unsigned segmentCount = std::max(
                        96u,
                        static_cast<unsigned>(std::ceil(std::max(rx,ry) * renderScale)));
                auto prev = toNdcPoint(cx + rx,cy);

                for(unsigned i = 1; i <= segmentCount; i++){
                    const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segmentCount);
                    const float px = cx + (std::cos(angle) * rx);
                    const float py = cy + (std::sin(angle) * ry);
                    auto next = toNdcPoint(px,py);

                    OmegaGTE::TETessellationResult::TEMesh::Polygon tri {};
                    tri.a.pt = center;
                    tri.b.pt = prev;
                    tri.c.pt = next;
                    tri.a.attachment = tri.b.attachment = tri.c.attachment =
                            std::make_optional<OmegaGTE::TETessellationResult::AttachmentData>(
                                    OmegaGTE::TETessellationResult::AttachmentData{
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
                auto te_params = OmegaGTE::TETessellationParams::GraphicsPath2D(*_params.path,
                                                                                 _params.strokeWidth,
                                                                                 _params.contour,
                                                                                 _params.fill);
                auto color = OmegaGTE::makeColor(1.f,1.f,1.f,1.f);
                if(_params.brush != nullptr && _params.brush->isColor){
                    color = OmegaGTE::makeColor(_params.brush->color.r,
                                                _params.brush->color.g,
                                                _params.brush->color.b,
                                                _params.brush->color.a);
                }
                te_params.addAttachment(OmegaGTE::TETessellationParams::Attachment::makeColor(color));
                result = tessellationEngineContext->tessalateSync(te_params,
                                                                  OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise,
                                                                  &viewPort);
                break;
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
                std::cout << "Texture render pipeline unavailable. Skipping textured draw command." << std::endl;
                return;
            }
            struct_size = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT2});
        }
        else {
            struct_size = OmegaGTE::omegaSLStructSize({OMEGASL_FLOAT4,OMEGASL_FLOAT4});
        }

        OmegaGTE::BufferDescriptor bufferDesc {OmegaGTE::BufferDescriptor::Upload,result.totalVertexCount() *struct_size,struct_size};
        auto buffer = gte.graphicsEngine->makeBuffer(bufferDesc);

        bufferWriter->setOutputBuffer(buffer);

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

        unsigned startVertexIndex = 0;

        auto writeColorVertexToBuffer = [&](OmegaGTE::GPoint3D & pt,OmegaGTE::FVec<4> color){
            auto pos = OmegaGTE::FVec<4>::Create();
            pos[0][0] = pt.x;
            pos[1][0] = pt.y;
            pos[2][0] = pt.z;
            pos[3][0] = 1.f;
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
                    auto & aColor = v.a.attachment ? v.a.attachment->color : fallbackColor;
                    auto & bColor = v.b.attachment ? v.b.attachment->color : fallbackColor;
                    auto & cColor = v.c.attachment ? v.c.attachment->color : fallbackColor;
                    writeColorVertexToBuffer(v.a.pt,aColor);
                    writeColorVertexToBuffer(v.b.pt,bColor);
                    writeColorVertexToBuffer(v.c.pt,cColor);
                }
            }
        }

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


        for(auto & m : result.meshes){
            OmegaGTE::GERenderTarget::CommandBuffer::PolygonType topology;
            if(m.topology == OmegaGTE::TETessellationResult::TEMesh::TopologyTriangleStrip){
                topology = OmegaGTE::GERenderTarget::CommandBuffer::TriangleStrip;
            }
            else {
                topology = OmegaGTE::GERenderTarget::CommandBuffer::Triangle;
            }
            cb->drawPolygons(topology, m.vertexCount(), startVertexIndex);
            startVertexIndex += m.vertexCount();
        }
        bufferWriter->flush();
        cb->endRenderPass();
        preEffectTarget->submitCommandBuffer(cb);
    }



}
