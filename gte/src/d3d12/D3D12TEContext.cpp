#include "GED3D12RenderTarget.h"
#include "GED3D12CommandQueue.h"
#include "omegaGTE/TE.h"

#include <d3dcompiler.h>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

_NAMESPACE_BEGIN_

namespace {

struct D3D12TessVertex { float pos[4]; float color[4]; };
struct D3D12TessParams { float rect[4]; float viewport[4]; float color[4]; float extra[4]; };
struct D3D12PathSeg { float se[4]; float sv[4]; float c[4]; float r[4]; };

const char *hlslRectSrc = R"(
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
StructuredBuffer<P> p:register(t0);RWStructuredBuffer<V> o:register(u0);
[numthreads(1,1,1)]void CSMain(uint3 t:SV_DispatchThreadID){
float x=p[0].r[0],y=p[0].r[1],w=p[0].r[2],h=p[0].r[3],vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;
float a=(2*x)/vw,b=(2*y)/vh,d=(2*(x+w))/vw,e=(2*(y+h))/vh;V z;z.c=c;
z.p=float4(a,b,0,1);o[0]=z;z.p=float4(a,e,0,1);o[1]=z;z.p=float4(d,b,0,1);o[2]=z;
z.p=float4(d,e,0,1);o[3]=z;z.p=float4(a,e,0,1);o[4]=z;z.p=float4(d,b,0,1);o[5]=z;})";

const char *hlslEllipSrc = R"(
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
StructuredBuffer<P> p:register(t0);RWStructuredBuffer<V> o:register(u0);
[numthreads(1,1,1)]void CSMain(uint3 t:SV_DispatchThreadID){
float cx=p[0].r[0],cy=p[0].r[1],rx=p[0].e[0],ry=p[0].e[1],ts=p[0].e[3];
float vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;float pi2=6.28318530718;
float a0=(pi2*float(t.x))/ts,a1=(pi2*float(t.x+1))/ts;uint b=t.x*3;V z;z.c=c;
z.p=float4((2*cx)/vw,(2*cy)/vh,0,1);o[b]=z;
z.p=float4((2*(cx+rx*cos(a0)))/vw,(2*(cy+ry*sin(a0)))/vh,0,1);o[b+1]=z;
z.p=float4((2*(cx+rx*cos(a1)))/vw,(2*(cy+ry*sin(a1)))/vh,0,1);o[b+2]=z;})";

const char *hlslRPrismSrc = R"(
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
StructuredBuffer<P> p:register(t0);RWStructuredBuffer<V> o:register(u0);
[numthreads(1,1,1)]void CSMain(uint3 t:SV_DispatchThreadID){
float x0=p[0].r[0],y0=p[0].r[1],z0=p[0].r[2],w=p[0].r[3],h=p[0].e[0],d=p[0].e[1];
float vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;
float a=(2*x0)/vw,b=(2*y0)/vh,e=(2*(x0+w))/vw,f=(2*(y0+h))/vh,g=z0,j=z0+d;
V z;z.c=c;uint i=0;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;
z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;
z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;
z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;
z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;
z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;
z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;})";

const char *hlslPathSrc = R"(
struct S{float4 se;float4 sv;float4 c;float4 r;};struct V{float4 p;float4 c;};
StructuredBuffer<S> s:register(t0);RWStructuredBuffer<V> v:register(u0);
[numthreads(1,1,1)]void CSMain(uint3 t:SV_DispatchThreadID){
float sx=s[t.x].se[0],sy=s[t.x].se[1],ex=s[t.x].se[2],ey=s[t.x].se[3];
float sw=s[t.x].sv[0],vw=s[t.x].sv[2],vh=s[t.x].sv[3];float4 c=s[t.x].c;
float dx=ex-sx,dy=ey-sy,l=sqrt(dx*dx+dy*dy),hw=sw*0.5;
float nx=0,ny=0;if(l>0.0001){nx=-dy/l*hw;ny=dx/l*hw;}
uint b=t.x*6;V o;o.c=c;
o.p=float4((2*(sx+nx))/vw,(2*(sy+ny))/vh,0,1);v[b]=o;
o.p=float4((2*(sx-nx))/vw,(2*(sy-ny))/vh,0,1);v[b+1]=o;
o.p=float4((2*(ex+nx))/vw,(2*(ey+ny))/vh,0,1);v[b+2]=o;
o.p=float4((2*(ex+nx))/vw,(2*(ey+ny))/vh,0,1);v[b+3]=o;
o.p=float4((2*(sx-nx))/vw,(2*(sy-ny))/vh,0,1);v[b+4]=o;
o.p=float4((2*(ex-nx))/vw,(2*(ey-ny))/vh,0,1);v[b+5]=o;})";

ComPtr<ID3D12PipelineState> compileD3D12Kernel(ID3D12Device *dev, const char *src,
                                                ID3D12RootSignature *rootSig) {
    ComPtr<ID3DBlob> csBlob, errBlob;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            "CSMain", "cs_5_0", 0, 0, &csBlob, &errBlob);
    if (FAILED(hr)) return nullptr;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = rootSig;
    desc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    desc.NodeMask = dev->GetNodeCount();

    ComPtr<ID3D12PipelineState> pso;
    hr = dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    return SUCCEEDED(hr) ? pso : nullptr;
}

TETessellationResult readbackD3D12(ID3D12Resource *buf, unsigned vc,
                                   const std::optional<TETessellationResult::AttachmentData> &att) {
    TETessellationResult res;
    TETessellationResult::TEMesh mesh{TETessellationResult::TEMesh::TopologyTriangle};
    D3D12TessVertex *v = nullptr;
    CD3DX12_RANGE readRange(0, vc * sizeof(D3D12TessVertex));
    buf->Map(0, &readRange, (void **)&v);
    if (!v) return res;
    for (unsigned i = 0; i + 2 < vc; i += 3) {
        TETessellationResult::TEMesh::Polygon p{};
        p.a.pt = {v[i].pos[0], v[i].pos[1], v[i].pos[2]};
        p.b.pt = {v[i+1].pos[0], v[i+1].pos[1], v[i+1].pos[2]};
        p.c.pt = {v[i+2].pos[0], v[i+2].pos[1], v[i+2].pos[2]};
        if (att) p.a.attachment = p.b.attachment = p.c.attachment = att;
        mesh.vertexPolygons.push_back(p);
    }
    buf->Unmap(0, nullptr);
    res.meshes.push_back(mesh);
    return res;
}

struct D3D12TessPipelines {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> rect, ellip, prism, path;
    bool ready = false;

    void init(ID3D12Device *d, ID3D12CommandQueue *q) {
        if (ready) return;
        ready = true;
        d->QueryInterface(IID_PPV_ARGS(&dev));
        q->QueryInterface(IID_PPV_ARGS(&queue));

        CD3DX12_DESCRIPTOR_RANGE1 srvRange, uavRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

        CD3DX12_ROOT_PARAMETER1 params[2];
        params[0].InitAsDescriptorTable(1, &srvRange);
        params[1].InitAsDescriptorTable(1, &uavRange);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC sigDesc;
        sigDesc.Init_1_1(2, params);

        ComPtr<ID3DBlob> sigBlob;
        D3DX12SerializeVersionedRootSignature(&sigDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &sigBlob, nullptr);
        dev->CreateRootSignature(dev->GetNodeCount(), sigBlob->GetBufferPointer(),
                                 sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

        rect  = compileD3D12Kernel(dev.Get(), hlslRectSrc, rootSig.Get());
        ellip = compileD3D12Kernel(dev.Get(), hlslEllipSrc, rootSig.Get());
        prism = compileD3D12Kernel(dev.Get(), hlslRPrismSrc, rootSig.Get());
        path  = compileD3D12Kernel(dev.Get(), hlslPathSrc, rootSig.Get());
    }
};

std::future<TETessellationResult> d3d12GpuDispatch(
        OmegaTessellationEngineContext::GPUTessExtractedParams &ep,
        GEViewport &vp, float ctxArcStep, D3D12TessPipelines &pip,
        OmegaTessellationEngineContext *ctx,
        const TETessellationParams &origParams,
        GTEPolygonFrontFaceRotation ff, GEViewport *origVP) {

    std::optional<TETessellationResult::AttachmentData> colorAtt;
    float cv[4] = {0,0,0,1};
    if (ep.hasColor) {
        colorAtt = TETessellationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), FVec<3>::Create()};
        colorAtt->color[0][0] = ep.cr; colorAtt->color[1][0] = ep.cg;
        colorAtt->color[2][0] = ep.cb; colorAtt->color[3][0] = ep.ca;
        cv[0] = ep.cr; cv[1] = ep.cg; cv[2] = ep.cb; cv[3] = ep.ca;
    }

    auto fallback = [&]() {
        auto r = ctx->tessalateSync(origParams, ff, origVP);
        std::promise<TETessellationResult> p; p.set_value(std::move(r)); return p.get_future();
    };

    ID3D12PipelineState *pso = nullptr;
    unsigned vc = 0;
    unsigned tc = 1;
    size_t paramBufSize = 0;
    void *paramData = nullptr;

    using ET = OmegaTessellationEngineContext::GPUTessExtractedParams;
    D3D12TessParams tp {};
    D3D12PathSeg *pathSegs = nullptr;

    switch (ep.type) {
        case ET::Rect: {
            pso = pip.rect.Get(); vc = 6; tc = 1;
            tp = {{ep.rx,ep.ry,ep.rw,ep.rh},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{0,0,0,0}};
            paramBufSize = sizeof(D3D12TessParams);
            paramData = &tp;
            break;
        }
        case ET::Ellipsoid: {
            pso = pip.ellip.Get();
            float step = ctxArcStep > 0 ? ctxArcStep : 0.01f;
            unsigned segs = (unsigned)std::ceil(2.f * M_PI / step);
            vc = segs * 3; tc = segs;
            tp = {{ep.ex,ep.ey,0,0},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{ep.erad_x,ep.erad_y,step,(float)segs}};
            paramBufSize = sizeof(D3D12TessParams);
            paramData = &tp;
            break;
        }
        case ET::RectPrism: {
            pso = pip.prism.Get(); vc = 36; tc = 1;
            tp = {{ep.px,ep.py,ep.pz,ep.pw},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{ep.ph,ep.pd,0,0}};
            paramBufSize = sizeof(D3D12TessParams);
            paramData = &tp;
            break;
        }
        case ET::Path2D: {
            if (ep.pathSegments.empty()) return fallback();
            pso = pip.path.Get();
            unsigned sc = (unsigned)ep.pathSegments.size();
            vc = sc * 6; tc = sc;
            pathSegs = new D3D12PathSeg[sc];
            float sw = ep.strokeWidth > 0 ? ep.strokeWidth : 1.f;
            for (unsigned i = 0; i < sc; i++) {
                auto &s = ep.pathSegments[i];
                pathSegs[i] = {{s.sx,s.sy,s.ex,s.ey},{sw,0,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{0,0,0,0}};
            }
            paramBufSize = sc * sizeof(D3D12PathSeg);
            paramData = pathSegs;
            break;
        }
        default:
            return fallback();
    }

    if (!pso || !paramData) { delete[] pathSegs; return fallback(); }

    HRESULT hr;
    auto *dev = pip.dev.Get();

    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto paramDesc = CD3DX12_RESOURCE_DESC::Buffer(paramBufSize);

    ComPtr<ID3D12Resource> paramBuf;
    hr = dev->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE,
                                      &paramDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      nullptr, IID_PPV_ARGS(&paramBuf));
    if (FAILED(hr)) { delete[] pathSegs; return fallback(); }

    void *mapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    paramBuf->Map(0, &readRange, &mapped);
    memcpy(mapped, paramData, paramBufSize);
    paramBuf->Unmap(0, nullptr);
    delete[] pathSegs;

    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto outDesc = CD3DX12_RESOURCE_DESC::Buffer(vc * sizeof(D3D12TessVertex),
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> outBuf;
    hr = dev->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                                      &outDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      nullptr, IID_PPV_ARGS(&outBuf));
    if (FAILED(hr)) return fallback();

    auto readbackHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(vc * sizeof(D3D12TessVertex));
    ComPtr<ID3D12Resource> readbackBuf;
    hr = dev->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE,
                                      &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                      nullptr, IID_PPV_ARGS(&readbackBuf));
    if (FAILED(hr)) return fallback();

    D3D12_DESCRIPTOR_HEAP_DESC dhDesc {};
    dhDesc.NumDescriptors = 2;
    dhDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    dhDesc.NodeMask = dev->GetNodeCount();

    ComPtr<ID3D12DescriptorHeap> descHeap;
    hr = dev->CreateDescriptorHeap(&dhDesc, IID_PPV_ARGS(&descHeap));
    if (FAILED(hr)) return fallback();

    UINT increment = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(descHeap->GetCPUDescriptorHandleForHeapStart());

    size_t paramStride = (ep.type == ET::Path2D) ? sizeof(D3D12PathSeg) : sizeof(D3D12TessParams);
    UINT paramCount = (UINT)(paramBufSize / paramStride);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Buffer.StructureByteStride = (UINT)paramStride;
    srvDesc.Buffer.NumElements = paramCount;
    dev->CreateShaderResourceView(paramBuf.Get(), &srvDesc, cpuHandle);
    cpuHandle.Offset(1, increment);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.StructureByteStride = sizeof(D3D12TessVertex);
    uavDesc.Buffer.NumElements = vc;
    dev->CreateUnorderedAccessView(outBuf.Get(), nullptr, &uavDesc, cpuHandle);

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), pso, IID_PPV_ARGS(&cmdList));

    cmdList->SetComputeRootSignature(pip.rootSig.Get());
    ID3D12DescriptorHeap *heaps[] = { descHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(descHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetComputeRootDescriptorTable(0, gpuHandle);
    gpuHandle.Offset(1, increment);
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle);

    cmdList->Dispatch(tc, 1, 1);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(outBuf.Get());
    cmdList->ResourceBarrier(1, &uavBarrier);
    auto copyBarrier = CD3DX12_RESOURCE_BARRIER::Transition(outBuf.Get(),
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->ResourceBarrier(1, &copyBarrier);
    cmdList->CopyResource(readbackBuf.Get(), outBuf.Get());
    cmdList->Close();

    ID3D12CommandList *lists[] = { cmdList.Get() };
    pip.queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    pip.queue->Signal(fence.Get(), 1);
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    auto result = readbackD3D12(readbackBuf.Get(), vc, colorAtt);

    std::promise<TETessellationResult> prom;
    prom.set_value(std::move(result));
    return prom.get_future();
}

} // anon namespace


class D3D12NativeRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12NativeRenderTarget> target;
    D3D12TessPipelines pip;
public:
    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *x_result, float *y_result, float *z_result) {
        if (viewport != nullptr) {
            translateCoordsDefaultImpl(x, y, z, viewport, x_result, y_result, z_result);
        } else {
            auto desc = target->renderTargets[target->frameIndex]->GetDesc();
            GEViewport geViewport{0, 0, (float)desc.Width, (float)desc.Height};
            translateCoordsDefaultImpl(x, y, z, &geViewport, x_result, y_result, z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready) {
            auto *queue = (ID3D12CommandQueue *)target->nativeCommandQueue();
            ComPtr<ID3D12Device> dev;
            queue->GetDevice(IID_PPV_ARGS(&dev));
            pip.init(dev.Get(), queue);
        }
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0, 0, 1, 1, 0, 1};
        return d3d12GpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit D3D12NativeRenderTargetTEContext(const SharedHandle<GED3D12NativeRenderTarget> &target)
        : target(target) {}
};

class D3D12TextureRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12TextureRenderTarget> target;
    D3D12TessPipelines pip;
public:
    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *x_result, float *y_result, float *z_result) {
        if (viewport != nullptr) {
            translateCoordsDefaultImpl(x, y, z, viewport, x_result, y_result, z_result);
        } else {
            auto desc = target->texture->resource->GetDesc();
            GEViewport geViewport{0, 0, (float)desc.Width, (float)desc.Height};
            translateCoordsDefaultImpl(x, y, z, &geViewport, x_result, y_result, z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready) {
            auto *queue = (ID3D12CommandQueue *)target->nativeCommandQueue();
            ComPtr<ID3D12Device> dev;
            queue->GetDevice(IID_PPV_ARGS(&dev));
            pip.init(dev.Get(), queue);
        }
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0, 0, 1, 1, 0, 1};
        return d3d12GpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit D3D12TextureRenderTargetTEContext(const SharedHandle<GED3D12TextureRenderTarget> &target)
        : target(target) {}
};

SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget) {
    return std::make_shared<D3D12NativeRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12NativeRenderTarget>(renderTarget));
}

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget) {
    return std::make_shared<D3D12TextureRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12TextureRenderTarget>(renderTarget));
}

_NAMESPACE_END_
