#include "GED3D12RenderTarget.h"
#include "omegaGTE/TE.h"
// #include "omegaGTE/GTEShaderTypes.h"

_NAMESPACE_BEGIN_

// OmegaGTETexturedVertex *convertVertex(OmegaGTE::GETexturedVertex & vertex){
//     auto rc = new OmegaGTETexturedVertex();
//     rc->pos = DirectX::XMFLOAT3(vertex.pos.getI(),vertex.pos.getJ(),vertex.pos.getK());
//     rc->texturePos = DirectX::XMFLOAT2(vertex.textureCoord.getI(),vertex.textureCoord.getJ());
//     return rc;
// };
// OmegaGTEColorVertex *convertVertex(OmegaGTE::GEColoredVertex & vertex){
//     auto rc = new OmegaGTEColorVertex();
//     rc->pos = DirectX::XMFLOAT3(vertex.pos.getI(),vertex.pos.getJ(),vertex.pos.getK());
//     rc->color = DirectX::XMFLOAT4(vertex.color.valueAt(1,1),vertex.color.valueAt(1,2),vertex.color.valueAt(1,3),vertex.color.valueAt(1,4));
//     return rc;
// };

class D3D12NativeRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12NativeRenderTarget> target;
    public:
    void translateCoords(float x, float y, float z,GEViewport *viewport, float *x_result, float *y_result, float *z_result){
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            /// Use Entire Render Target as Viewport.
            auto desc = target->renderTargets[target->frameIndex]->GetDesc();
            
            GEViewport geViewport {0,0,(float)desc.Width,(float)desc.Height};
            translateCoordsDefaultImpl(x,y,z,&geViewport,x_result,y_result,z_result);
        };
    };
    // std::future<TETessellationResult> tessalateAsync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){};
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,GTEPolygonFrontFaceRotation direction, GEViewport * viewport){
        return {};
    };
    // TETessellationResult tessalateSync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){};
    explicit D3D12NativeRenderTargetTEContext(const SharedHandle<GED3D12NativeRenderTarget> & target):target(target){

    };
};

class D3D12TextureRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12TextureRenderTarget> target;
    public:
    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result){
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            auto desc = target->texture->resource->GetDesc();
            GEViewport geViewport {0,0,(float)desc.Width,(float)desc.Height};
            translateCoordsDefaultImpl(x,y,z,&geViewport,x_result,y_result,z_result);
        };
    };
    // std::future<TETessellationResult> tessalateAsync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){};
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params, GTEPolygonFrontFaceRotation direction,GEViewport * viewport){
        return {};
    };
    // TETessellationResult tessalateSync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){};
    explicit D3D12TextureRenderTargetTEContext(const SharedHandle<GED3D12TextureRenderTarget> & target):target(target){};
};

SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget){
    return std::make_shared<D3D12NativeRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12NativeRenderTarget>(renderTarget));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget){
    return std::make_shared<D3D12TextureRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12TextureRenderTarget>(renderTarget));
};



_NAMESPACE_END_