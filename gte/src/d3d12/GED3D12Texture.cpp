#include "GED3D12Texture.h"
#include <cstring>
#include <algorithm>
#include "../common/GEResourceTracker.h"


_NAMESPACE_BEGIN_

GED3D12Texture::GED3D12Texture(GED3D12Engine *engine,
                               const TextureKind & kind,
                               const GETextureUsage & usage,
                               const TexturePixelFormat & pixelFormat,
                               ID3D12Resource *res,
                               ID3D12Resource *cpuSideRes,
                               const D3D12DescriptorHandle & srvHandleIn,
                               const D3D12DescriptorHandle & uavHandleIn,
                               ID3D12DescriptorHeap *rtvDescHeap,
                               ID3D12DescriptorHeap *dsvDescHeap,
                               D3D12_RESOURCE_STATES & currentState,
                               D3D12MA::Allocation *d3d12maAllocation,
                               D3D12MA::Allocation *d3d12maCpuSideAllocation):
                                GETexture(kind,usage,pixelFormat),
                                resource(res),
                                cpuSideresource(cpuSideRes),
                               srvHandle(srvHandleIn),
                               uavHandle(uavHandleIn),
                               rtvDescHeap(rtvDescHeap),
                               dsvDescHeap(dsvDescHeap),
                               owningEngine(engine),
                               d3d12maAllocation(d3d12maAllocation),
                               d3d12maCpuSideAllocation(d3d12maCpuSideAllocation),
                               currentState(currentState){
    if(usage == GPUAccessOnly || usage == RenderTarget){
        onGpu = true;
    }
    else {
        onGpu = false;
    }
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Create,
            ResourceTracking::Backend::D3D12,
            "Texture",
            traceResourceId,
            resource.Get(),
            static_cast<float>(resource->GetDesc().Width),
            static_cast<float>(resource->GetDesc().Height));
    DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Texture created: id=" << traceResourceId
               << " " << resource->GetDesc().Width << "x" << resource->GetDesc().Height);
}

void GED3D12Texture::updateAndValidateStatus(ID3D12GraphicsCommandList *commandList) {
    if(usage == ToGPU){
        if(!onGpu){
            uploadTextureFromUploadHeap(commandList);
        }
    }
    else if(usage == FromGPU) {
        if(onGpu){
            downloadTextureToReadbackHeap(commandList);
        }
    }
}

void GED3D12Texture::uploadTextureFromUploadHeap(ID3D12GraphicsCommandList *commandList) {
    assert(usage == ToGPU && "");

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),currentState,D3D12_RESOURCE_STATE_COPY_DEST);

    commandList->ResourceBarrier(1,&barrier);

    ID3D12Device *dev;
    auto desc = resource->GetDesc();
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    // §7.1: compute footprints for the whole resource so each subresource's
    // heap offset is correct, then copy only the subresources that were
    // written. The footprints' BaseOffset of 0 makes Footprint[s].Offset the
    // cumulative byte offset matching the copyBytes(region) write below.
    const UINT arraySz = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                         ? 1u : desc.DepthOrArraySize;
    const UINT numSubresources = UINT(desc.MipLevels) * arraySz; // planeCount 1 (color)
    OmegaCommon::Vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(numSubresources);
    dev->GetCopyableFootprints(&desc,0,numSubresources,0,footprints.data(),nullptr,nullptr,nullptr);

    // No region-targeted writes recorded ⇒ legacy whole-mip-0 path.
    OmegaCommon::Vector<UINT> subs = dirtySubresources;
    if(subs.empty()){
        subs.push_back(0);
    }
    for(UINT s : subs){
        if(s >= numSubresources){
            continue;
        }
        CD3DX12_TEXTURE_COPY_LOCATION destLoc(resource.Get(),s);
        CD3DX12_TEXTURE_COPY_LOCATION srcLoc(cpuSideresource.Get(),footprints[s]);
        commandList->CopyTextureRegion(&destLoc,0,0,0,&srcLoc,nullptr);
    }

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,currentState);

    commandList->ResourceBarrier(1,&barrier);
    dev->Release();
    onGpu = true;
    dirtySubresources.clear();
}

void GED3D12Texture::downloadTextureToReadbackHeap(ID3D12GraphicsCommandList *commandList) {
    assert(usage == FromGPU && "");

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),currentState,D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(1,&barrier);
    currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;

    // A texture→buffer copy must go through CopyTextureRegion with a placed
    // footprint — a texture's memory is tiled, so the old CopyBufferRegion
    // (a raw byte copy treating the texture as a buffer) could not linearize
    // it and left the readback companion full of zeros / garbage. The footprint
    // computed here matches exactly what getBytes reads back. Mirrors the
    // upload path (uploadTextureFromUploadHeap), reversed.
    ID3D12Device *dev;
    auto desc = resource->GetDesc();
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    dev->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,nullptr);

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(resource.Get(),0);
    CD3DX12_TEXTURE_COPY_LOCATION destLoc(cpuSideresource.Get(),footprint);
    commandList->CopyTextureRegion(&destLoc,0,0,0,&srcLoc,nullptr);

    dev->Release();
    onGpu = false;
}

void GED3D12Texture::copyBytes(void *bytes,size_t bytesPerRow){
    assert(usage != GPUAccessOnly && "");
    // Defensive guards: a null cpuSideresource (texture created with
    // GPUAccessOnly, or allocation of the upload heap failed silently
    // in makeTexture's D3D12MA call) would access-violate at Map.
    // bytes/bytesPerRow at zero produce a no-op that wastes the Map.
    if(cpuSideresource.Get() == nullptr || bytes == nullptr || bytesPerRow == 0){
        DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE, "GED3D12Texture::copyBytes: null upload heap or bad input");
        return;
    }

    void *mem_ptr = nullptr;
    auto desc = resource->GetDesc();

    HRESULT hr = cpuSideresource->Map(0,nullptr,&mem_ptr);
    if(FAILED(hr) || mem_ptr == nullptr){
        // Previously exit(1). That terminated the process from the
        // compositor worker thread (visible as a DebugBreak inside
        // ucrtbased!_wassert during ExitProcess), which masked the
        // real failure mode. Returning early lets the caller see a
        // texture-without-pixels — a visible glitch the user can
        // iterate on — and logs the HRESULT so the surfacing
        // condition is identifiable.
        //
        // DXGI_ERROR_DEVICE_REMOVED (0x887A0005) is the common case
        // here: Map on an UPLOAD heap doesn't talk to the GPU, so a
        // device-removed result means the device was already killed
        // by earlier work. Query GetDeviceRemovedReason() so the log
        // surfaces the *upstream* cause (TDR, hung command, bad
        // resource state, driver bug) instead of just the
        // downstream symptom.
        HRESULT removedReason = S_OK;
        if(hr == DXGI_ERROR_DEVICE_REMOVED){
            ID3D12Device *dev = nullptr;
            if(SUCCEEDED(cpuSideresource->GetDevice(__uuidof(*dev), (void **)&dev))
               && dev != nullptr){
                removedReason = dev->GetDeviceRemovedReason();
                dev->Release();
            }
        }
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12Texture::copyBytes: Map failed hr=0x"
                     << std::hex << hr
                     << " removedReason=0x" << removedReason << std::dec
                     << " (cpuSideresource=" << cpuSideresource.Get()
                     << ", " << desc.Width << "x" << desc.Height
                     << " bytesPerRow=" << bytesPerRow << ")");
        return;
    }


    D3D12_SUBRESOURCE_DATA subresourceData {};
    subresourceData.pData = bytes;
    subresourceData.SlicePitch = (LONG_PTR)bytesPerRow * desc.Height;
    subresourceData.RowPitch = (LONG_PTR)bytesPerRow;

    ID3D12Device *dev;
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};

    UINT rows;
    UINT64 rowSize;

    dev->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,nullptr);

    D3D12_MEMCPY_DEST dest {((PBYTE)mem_ptr) + footprint.Offset,footprint.Footprint.RowPitch,footprint.Footprint.RowPitch * rows};
    MemcpySubresource(&dest,&subresourceData,rowSize,rows,1);

    cpuSideresource->Unmap(0,nullptr);
    dev->Release();

}

void GED3D12Texture::copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion &destRegion){
    assert(usage != GPUAccessOnly && "");
    if(bytes == nullptr || bytesPerRow == 0 || destRegion.w == 0 || destRegion.h == 0){
        return;
    }

    // Pipeline-Completion-Extension-Plan §4.5. Writes the supplied bytes into
    // the sub-region of `cpuSideresource` (the upload heap) that corresponds
    // to `destRegion`. The deferred-upload path (`uploadTextureFromUploadHeap`,
    // triggered by `updateAndValidateStatus` at first bind) then copies the
    // whole subresource to the GPU. Consumers calling the region overload
    // must populate every pixel they care about before the first bind —
    // bytes outside the written region come from the upload heap's current
    // contents (uninitialised on a fresh texture).
    UINT bytesPerTexel = 4;
    switch(pixelFormat){
        case PixelFormat::RGBA16Unorm: bytesPerTexel = 8; break;
        default: bytesPerTexel = 4; break;
    }

    void *mem_ptr = nullptr;
    HRESULT hr = cpuSideresource->Map(0,nullptr,&mem_ptr);
    if(FAILED(hr)){
        DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "Failed to Map Memory Ptr to Texture");
        return;
    }

    auto desc = resource->GetDesc();
    ID3D12Device *dev = nullptr;
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    // §7.1: resolve the (mipLevel, arrayLayer) subresource and grab the
    // footprint table for the whole resource so we write at the right
    // cumulative heap offset (matching uploadTextureFromUploadHeap).
    const UINT arraySz = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                         ? 1u : desc.DepthOrArraySize;
    const UINT numSubresources = UINT(desc.MipLevels) * arraySz; // planeCount 1 (color)
    const UINT subresource = D3D12CalcSubresource(destRegion.mipLevel, destRegion.arrayLayer,
                                                  0, desc.MipLevels, arraySz);
    if(subresource >= numSubresources){
        DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE, "GED3D12Texture::copyBytes(region): subresource out of range");
        cpuSideresource->Unmap(0,nullptr);
        dev->Release();
        return;
    }

    OmegaCommon::Vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(numSubresources);
    OmegaCommon::Vector<UINT> rowsArr(numSubresources);
    dev->GetCopyableFootprints(&desc,0,numSubresources,0,footprints.data(),rowsArr.data(),nullptr,nullptr);

    const auto &footprint = footprints[subresource];
    const UINT rows = rowsArr[subresource];
    const UINT depth = destRegion.d == 0 ? 1u : destRegion.d;
    const UINT slicePitch = footprint.Footprint.RowPitch * rows;
    const size_t rowBytes = static_cast<size_t>(destRegion.w) * bytesPerTexel;
    auto *base = static_cast<PBYTE>(mem_ptr) + footprint.Offset;
    const auto *src = static_cast<const BYTE *>(bytes);

    for(UINT z = 0; z < depth; ++z){
        for(UINT y = 0; y < destRegion.h; ++y){
            PBYTE dst = base
                + static_cast<size_t>(destRegion.z + z) * slicePitch
                + static_cast<size_t>(destRegion.y + y) * footprint.Footprint.RowPitch
                + static_cast<size_t>(destRegion.x) * bytesPerTexel;
            const BYTE *srow = src
                + static_cast<size_t>(z) * bytesPerRow * destRegion.h
                + static_cast<size_t>(y) * bytesPerRow;
            std::memcpy(dst, srow, rowBytes);
        }
    }

    cpuSideresource->Unmap(0,nullptr);
    dev->Release();

    // Record the subresource so uploadTextureFromUploadHeap re-uploads it
    // (and only it). Dedup so repeated writes to the same subresource don't
    // queue duplicate copies.
    if(std::find(dirtySubresources.begin(), dirtySubresources.end(), subresource)
       == dirtySubresources.end()){
        dirtySubresources.push_back(subresource);
    }

    // Force the next bind through updateAndValidateStatus to re-upload the
    // (now partially-modified) upload heap. Matches the existing behaviour
    // for first-upload textures; for textures already on GPU this triggers
    // a full subresource re-upload that includes the modified region.
    onGpu = false;
}

size_t GED3D12Texture::getBytes(void *bytes, size_t bytesPerRow) {
    assert(usage == FromGPU && "");
    void *mem_ptr = nullptr;

    auto desc = resource->GetDesc();

    if(bytes != nullptr) {
        if(cpuSideresource.Get() == nullptr){
            DEBUG_CRITICAL(DEBUG_DOMAIN_RESOURCE, "GED3D12Texture::getBytes: null readback heap");
            return 0;
        }
        HRESULT hr = cpuSideresource->Map(0, nullptr, &mem_ptr);
        if (FAILED(hr) || mem_ptr == nullptr) {
            // Previously exit(1) — same fatal-from-worker-thread issue
            // as copyBytes. Return 0 (no bytes read) and log the HR so
            // the caller can detect the failure.
            DEBUG_ERROR(DEBUG_DOMAIN_RESOURCE, "GED3D12Texture::getBytes: Map failed hr=0x"
                         << std::hex << hr << std::dec);
            return 0;
        }
    }


    ID3D12Device *dev;
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};

    UINT rows;
    UINT64 rowSize;

    UINT64 totalBytes;

    dev->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&totalBytes);

    if(bytes != nullptr) {
        // The mapped readback heap is laid out at the D3D12-aligned footprint
        // row pitch (a 256-byte multiple per D3D12_TEXTURE_DATA_PITCH_ALIGNMENT),
        // NOT the caller's packed `bytesPerRow`. Read from that aligned source
        // stride into the caller's tightly-packed destination, copying `rowSize`
        // (the tight row width) per row.
        //
        // The source/destination row pitches were previously swapped: the
        // aligned footprint pitch was used as the *destination* stride into the
        // caller's packed buffer, so the trailing rows wrote past its end (an
        // 8x8 RGBA8 readback strode 256*7+32 bytes into a 256-byte buffer —
        // CRT heap corruption). footprint.Offset likewise belongs on the source
        // (mapped heap), not the destination. This mirrors the upload path in
        // copyBytes, inverted.
        D3D12_MEMCPY_DEST dest{(PBYTE)bytes, (SIZE_T)bytesPerRow,
                               (SIZE_T)bytesPerRow * rows};
        D3D12_SUBRESOURCE_DATA src{};
        src.pData      = (const BYTE *)mem_ptr + footprint.Offset;
        src.RowPitch   = (LONG_PTR)footprint.Footprint.RowPitch;
        src.SlicePitch = (LONG_PTR)footprint.Footprint.RowPitch * rows;
        MemcpySubresource(&dest, &src, (SIZE_T)rowSize, rows, 1);

        cpuSideresource->Unmap(0, nullptr);
    }
    dev->Release();
    // Bytes written into the caller's packed buffer — not the alignment-padded
    // GPU footprint total (`totalBytes`), which would over-report by the
    // per-row 256-byte padding.
    return (size_t)bytesPerRow * rows;
}

    bool GED3D12Texture::needsValidation() {
        if(usage == ToGPU){
            return !onGpu;
        }
        else if(usage == FromGPU) {
            return onGpu;
        }
        return false;
    }

D3D12DescriptorHandle GED3D12Texture::getOrCreateSwizzledSrvHandle(GED3D12Engine *engine,
                                                                    const TextureSwizzle & swizzle){
    if(swizzle.isIdentity() || !hasPrimarySrvDesc) return srvHandle;
    for(auto & entry : swizzledSrvCache){
        if(entry.first == swizzle) return entry.second;
    }

    auto encodeSwizzle = [](TextureSwizzleChannel ch, unsigned positional) -> unsigned {
        switch(ch){
            case TextureSwizzleChannel::Identity: return positional;
            case TextureSwizzleChannel::Red:      return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0;
            case TextureSwizzleChannel::Green:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1;
            case TextureSwizzleChannel::Blue:     return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2;
            case TextureSwizzleChannel::Alpha:    return D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3;
            case TextureSwizzleChannel::Zero:     return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
            case TextureSwizzleChannel::One:      return D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
        }
        return positional;
    };

    // Phase 2: allocate a slot from the engine's shared CBV/SRV/UAV heap.
    // On exhaustion fall back to the primary handle silently — same shape
    // as the old per-heap creation-fail branch, but without producing an
    // orphan heap that would never be freed.
    D3D12DescriptorHandle h = engine->resourceDescriptorAllocator->allocate(1);
    if(!h.valid()){
        return srvHandle;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC swizzledDesc = primarySrvDesc;
    swizzledDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        encodeSwizzle(swizzle.r, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0),
        encodeSwizzle(swizzle.g, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1),
        encodeSwizzle(swizzle.b, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2),
        encodeSwizzle(swizzle.a, D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3));

    engine->d3d12_device->CreateShaderResourceView(resource.Get(), &swizzledDesc, h.cpu);
    swizzledSrvCache.push_back({swizzle, h});
    return h;
}

GED3D12Texture::~GED3D12Texture(){
    ResourceTracking::Tracker::instance().emit(
            ResourceTracking::EventType::Destroy,
            ResourceTracking::Backend::D3D12,
            "Texture",
            traceResourceId,
            resource.Get(),
            static_cast<float>(resource->GetDesc().Width),
            static_cast<float>(resource->GetDesc().Height));
    DEBUG_INFO(DEBUG_DOMAIN_RESOURCE, "Texture destroyed: id=" << traceResourceId);

    // Phase 2: return shared-heap descriptor slots to the engine allocator
    // once every command list currently in flight on every live queue has
    // retired. `freeDescriptorAfterQueueDrain` is a no-op for invalid
    // handles, so textures that never got an SRV / UAV are fine.
    if(owningEngine){
        owningEngine->freeDescriptorAfterQueueDrain(
            owningEngine->resourceDescriptorAllocator.get(), srvHandle);
        owningEngine->freeDescriptorAfterQueueDrain(
            owningEngine->resourceDescriptorAllocator.get(), uavHandle);
        for(auto & entry : swizzledSrvCache){
            owningEngine->freeDescriptorAfterQueueDrain(
                owningEngine->resourceDescriptorAllocator.get(), entry.second);
        }
    }
    swizzledSrvCache.clear();

    // Drop the COM refs before releasing the D3D12MA allocations so the
    // allocator's leak validator sees the underlying resources already
    // destroyed when the allocations go away.
    resource.Reset();
    cpuSideresource.Reset();
    if (d3d12maAllocation) {
        d3d12maAllocation->Release();
        d3d12maAllocation = nullptr;
    }
    if (d3d12maCpuSideAllocation) {
        d3d12maCpuSideAllocation->Release();
        d3d12maCpuSideAllocation = nullptr;
    }
}

_NAMESPACE_END_
