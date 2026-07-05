#include "GED3D12RenderTarget.h"
#include "GED3D12CommandQueue.h"
#include "omegaGTE/TE.h"
#include "../GTEBuiltinShaders.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

_NAMESPACE_BEGIN_

namespace {

struct D3D12TessVertex { float pos[4]; float color[4]; };
struct D3D12TessParams { float rect[4]; float viewport[4]; float color[4]; float extra[4]; };
struct D3D12PathSeg { float se[4]; float sv[4]; float c[4]; float r[4]; };

// Triangulation-Engine-Completion-Plan.md Phase 4 -- the four kernels below
// used to be hand-authored HLSL string literals compiled at first use via
// D3DCompile. They are now single-sourced in OmegaSL
// (gte/src/shaders/triangulate_*.omegasl), compiled offline into
// GTEBuiltinShaders.omegasllib as DXIL, and loaded here directly from the
// compiled bytecode -- no D3DCompile, no HLSL text.
ComPtr<ID3D12PipelineState> loadBuiltinKernel(ID3D12Device *dev, const char *name,
                                              ID3D12RootSignature *rootSig) {
    const omegasl_shader *shader = GTEBuiltinShaders::find(name);
    if (!shader || shader->data == nullptr || shader->dataSize == 0) {
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = rootSig;
    desc.CS = { shader->data, shader->dataSize };
    desc.NodeMask = dev->GetNodeCount();

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    return SUCCEEDED(hr) ? pso : nullptr;
}

TETriangulationResult readbackD3D12(ID3D12Resource *buf, unsigned vc,
                                   const std::optional<TETriangulationResult::AttachmentData> &att) {
    TETriangulationResult res;
    TETriangulationResult::TEMesh mesh{TETriangulationResult::TEMesh::TopologyTriangle};
    D3D12TessVertex *v = nullptr;
    CD3DX12_RANGE readRange(0, vc * sizeof(D3D12TessVertex));
    buf->Map(0, &readRange, (void **)&v);
    if (!v) return res;
    for (unsigned i = 0; i + 2 < vc; i += 3) {
        TETriangulationResult::TEMesh::Polygon p{};
        p.a.pt = {v[i].pos[0], v[i].pos[1], v[i].pos[2]};
        p.b.pt = {v[i+1].pos[0], v[i+1].pos[1], v[i+1].pos[2]};
        p.c.pt = {v[i+2].pos[0], v[i+2].pos[1], v[i+2].pos[2]};
        if (att) p.a.attachment = p.b.attachment = p.c.attachment = att;
        mesh.vertexPolygons.push_back(p);
    }
    buf->Unmap(0, nullptr);
    res.mesh = std::move(mesh);
    return res;
}

struct D3D12TessPipelines {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    // Phase 3 (Shared-Descriptor-Heap-Plan) — the GE-side queue wrapper
    // owns the per-queue transient descriptor ring; the bare D3D12
    // queue above is still used for ExecuteCommandLists / Signal.
    GED3D12CommandQueue *engineQueue = nullptr;
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> rect, ellip, prism, path;
    bool ready = false;

    void init(ID3D12Device *d, ID3D12CommandQueue *q, GED3D12CommandQueue *eq) {
        if (ready) return;
        ready = true;
        d->QueryInterface(IID_PPV_ARGS(&dev));
        q->QueryInterface(IID_PPV_ARGS(&queue));
        engineQueue = eq;

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

        rect  = loadBuiltinKernel(dev.Get(), GTEBuiltinShaders::TriangulateRect, rootSig.Get());
        ellip = loadBuiltinKernel(dev.Get(), GTEBuiltinShaders::TriangulateEllipsoid, rootSig.Get());
        prism = loadBuiltinKernel(dev.Get(), GTEBuiltinShaders::TriangulateRectPrism, rootSig.Get());
        path  = loadBuiltinKernel(dev.Get(), GTEBuiltinShaders::TriangulatePath2D, rootSig.Get());
    }
};

std::future<TETriangulationResult> d3d12GpuDispatch(
        OmegaTriangulationEngineContext::GPUTriangulationExtractedParams &ep,
        GEViewport &vp, float ctxArcStep, D3D12TessPipelines &pip,
        OmegaTriangulationEngineContext *ctx,
        const TETriangulationParams &origParams,
        GTEPolygonFrontFaceRotation ff, GEViewport *origVP) {

    std::optional<TETriangulationResult::AttachmentData> colorAtt;
    float cv[4] = {0,0,0,1};
    if (ep.hasColor) {
        colorAtt = TETriangulationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), FVec<3>::Create()};
        colorAtt->color[0][0] = ep.cr; colorAtt->color[1][0] = ep.cg;
        colorAtt->color[2][0] = ep.cb; colorAtt->color[3][0] = ep.ca;
        cv[0] = ep.cr; cv[1] = ep.cg; cv[2] = ep.cb; cv[3] = ep.ca;
    }

    auto fallback = [&]() {
        auto r = ctx->triangulateSync(origParams, ff, origVP);
        std::promise<TETriangulationResult> p; p.set_value(std::move(r)); return p.get_future();
    };

    ID3D12PipelineState *pso = nullptr;
    unsigned vc = 0;
    unsigned tc = 1;
    size_t paramBufSize = 0;
    void *paramData = nullptr;

    using ET = OmegaTriangulationEngineContext::GPUTriangulationExtractedParams;
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

    // Phase 3 (Shared-Descriptor-Heap-Plan) — suballocate the SRV+UAV
    // pair from the engine queue's per-queue transient ring. Gate it on
    // a closure over our local `fence` (created below); after
    // WaitForSingleObject we call retire() so the slot is reclaimed.
    // If the engine queue isn't available (caller used a raw queue)
    // we'd have nowhere to allocate from — fall back to the CPU path.
    if (!pip.engineQueue) {
        DEBUG_STREAM("d3d12GpuDispatch: no engineQueue available; falling back to CPU");
        return fallback();
    }
    ComPtr<ID3D12Fence> dispatchFence;
    hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dispatchFence));
    if (FAILED(hr)) return fallback();

    D3D12DescriptorHandle ringSlot = pip.engineQueue->getTransientRing()->allocate(
        /*count=*/2u,
        [f = dispatchFence, target = UINT64(1)]() {
            return f && f->GetCompletedValue() >= target;
        });
    if (!ringSlot.valid()) {
        DEBUG_STREAM("d3d12GpuDispatch: transient ring exhausted; falling back to CPU");
        return fallback();
    }

    UINT increment = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(ringSlot.cpu);

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
    ID3D12DescriptorHeap *heaps[] = { pip.engineQueue->getTransientRing()->heap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(ringSlot.gpu);
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

    // Reuse the fence we created above for the ring stamp — saves an
    // extra CreateFence and means the gate captured inside the ring
    // stamp will signal exactly when this dispatch retires.
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    dispatchFence->SetEventOnCompletion(1, event);
    pip.queue->Signal(dispatchFence.Get(), 1);
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    // Our slot is now safe to recycle. The ring's retire() is FIFO — if
    // any earlier dispatch still has its gate pending, our slot has to
    // wait for it. That's fine; the ring has 4096 slots of headroom.
    pip.engineQueue->getTransientRing()->retire();

    auto result = readbackD3D12(readbackBuf.Get(), vc, colorAtt);

    std::promise<TETriangulationResult> prom;
    prom.set_value(std::move(result));
    return prom.get_future();
}

} // anon namespace


class D3D12NativeRenderTargetTEContext : public OmegaTriangulationEngineContext {
    SharedHandle<GED3D12NativeRenderTarget> target;
    D3D12TessPipelines pip;
public:
    GEViewport getEffectiveViewport() override {
        auto desc = target->renderTargets[target->frameIndex]->GetDesc();
        // Phase F-G: tessellation NDC must be authored against the LIVE source
        // region (what IDXGISwapChain2::SetSourceSize presents), NOT the
        // (possibly larger, bucketed) back-buffer — otherwise tessellated
        // geometry scales against the bucket and misaligns with the rest of
        // the frame. sourceWidth_/Height_ track the exact buffer size in the
        // legacy (non-bucketed) path, so this is a no-op there.
        const float w = (float)(target->sourceWidth_  > 0
                                    ? target->sourceWidth_  : (unsigned)desc.Width);
        const float h = (float)(target->sourceHeight_ > 0
                                    ? target->sourceHeight_ : (unsigned)desc.Height);
        return GEViewport{0, 0, w, h, 0.f, 1.f};
    }

    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *x_result, float *y_result, float *z_result) override {
        if (viewport != nullptr) {
            translateCoordsDefaultImpl(x, y, z, viewport, x_result, y_result, z_result);
        } else {
            auto vp = getEffectiveViewport();
            translateCoordsDefaultImpl(x, y, z, &vp, x_result, y_result, z_result);
        }
    }

    std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready && target) {
            auto presentQueue = target->presentQueue();
            if(presentQueue){
                auto *queue = (ID3D12CommandQueue *)presentQueue->native();
                auto *engineQueue = dynamic_cast<GED3D12CommandQueue *>(presentQueue.get());
                ComPtr<ID3D12Device> dev;
                queue->GetDevice(IID_PPV_ARGS(&dev));
                pip.init(dev.Get(), queue, engineQueue);
            }
        }
        GPUTriangulationExtractedParams ep;
        extractGPUTriangulationParams(params, ep);
        GEViewport vp = viewport ? *viewport : getEffectiveViewport();
        return d3d12GpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit D3D12NativeRenderTargetTEContext(const SharedHandle<GED3D12NativeRenderTarget> &target)
        : target(target) {}
    ~D3D12NativeRenderTargetTEContext() override = default;
};

class D3D12TextureRenderTargetTEContext : public OmegaTriangulationEngineContext {
    SharedHandle<GED3D12TextureRenderTarget> target;
    SharedHandle<GECommandQueue> queue;
    D3D12TessPipelines pip;
public:
    GEViewport getEffectiveViewport() override {
        auto desc = target->texture->resource->GetDesc();
        return GEViewport{0, 0, (float)desc.Width, (float)desc.Height, 0.f, 1.f};
    }

    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *x_result, float *y_result, float *z_result) override {
        if (viewport != nullptr) {
            translateCoordsDefaultImpl(x, y, z, viewport, x_result, y_result, z_result);
        } else {
            auto vp = getEffectiveViewport();
            translateCoordsDefaultImpl(x, y, z, &vp, x_result, y_result, z_result);
        }
    }

    std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready && queue) {
            auto *q = (ID3D12CommandQueue *)queue->native();
            auto *engineQueue = dynamic_cast<GED3D12CommandQueue *>(queue.get());
            ComPtr<ID3D12Device> dev;
            q->GetDevice(IID_PPV_ARGS(&dev));
            pip.init(dev.Get(), q, engineQueue);
        }
        GPUTriangulationExtractedParams ep;
        extractGPUTriangulationParams(params, ep);
        GEViewport vp = viewport ? *viewport : getEffectiveViewport();
        return d3d12GpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit D3D12TextureRenderTargetTEContext(const SharedHandle<GED3D12TextureRenderTarget> &target,
                                                SharedHandle<GECommandQueue> queue)
        : target(target), queue(std::move(queue)) {}
    ~D3D12TextureRenderTargetTEContext() override = default;
};

SharedHandle<OmegaTriangulationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget) {
    return std::make_shared<D3D12NativeRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12NativeRenderTarget>(renderTarget));
}

SharedHandle<OmegaTriangulationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget,
                                                                                  SharedHandle<GECommandQueue> &queue) {
    return std::make_shared<D3D12TextureRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12TextureRenderTarget>(renderTarget), queue);
}

_NAMESPACE_END_
