#include "GED3D12Texture.h"
#include <cstring>


_NAMESPACE_BEGIN_

GED3D12Texture::GED3D12Texture(const GETextureType & type,
                               const GETextureUsage & usage,
                               const TexturePixelFormat & pixelFormat,
                               ID3D12Resource *res,
                               ID3D12Resource *cpuSideRes,
                               ID3D12DescriptorHeap *descHeap,
                               ID3D12DescriptorHeap *uavDescHeap,
                               ID3D12DescriptorHeap *rtvDescHeap,
                               ID3D12DescriptorHeap *dsvDescHeap,
                               D3D12_RESOURCE_STATES & currentState):
                                GETexture(type,usage,pixelFormat),
                                resource(res),
                                cpuSideresource(cpuSideRes),
                               srvDescHeap(descHeap),
                               uavDescHeap(uavDescHeap),
                               rtvDescHeap(rtvDescHeap),
                               dsvDescHeap(dsvDescHeap),
                               currentState(currentState){
    if(usage == GPUAccessOnly || usage == RenderTarget){
        onGpu = true;
    }
    else {
        onGpu = false;
    }

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

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;

    ID3D12Device *dev;
    auto desc = resource->GetDesc();
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);
    dev->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,nullptr);


    CD3DX12_TEXTURE_COPY_LOCATION destLoc(resource.Get(),0),srcLoc(cpuSideresource.Get(),footprint);

    commandList->CopyTextureRegion(&destLoc,0,0,0,&srcLoc,nullptr);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,currentState);

    commandList->ResourceBarrier(1,&barrier);
    dev->Release();
    onGpu = true;
}

void GED3D12Texture::downloadTextureToReadbackHeap(ID3D12GraphicsCommandList *commandList) {
    assert(usage == FromGPU && "");

    UINT64 bytes = GetRequiredIntermediateSize(resource.Get(),0,1);

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),currentState,D3D12_RESOURCE_STATE_COPY_SOURCE);

    commandList->ResourceBarrier(1,&barrier);

    commandList->CopyBufferRegion(cpuSideresource.Get(),0,resource.Get(),0,bytes);
    onGpu = false;
}

void GED3D12Texture::copyBytes(void *bytes,size_t bytesPerRow){
    assert(usage != GPUAccessOnly && "");
    void *mem_ptr;

    auto desc = resource->GetDesc();

    HRESULT hr = cpuSideresource->Map(0,nullptr,&mem_ptr);
    if(FAILED(hr)){
        DEBUG_STREAM("Failed to Map Memory Ptr to Texture");
        exit(1);
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

size_t GED3D12Texture::getBytes(void *bytes, size_t bytesPerRow) {
    assert(usage == FromGPU && "");
    void *mem_ptr = nullptr;

    auto desc = resource->GetDesc();

    if(bytes != nullptr) {

        HRESULT hr = cpuSideresource->Map(0, nullptr, &mem_ptr);
        if (FAILED(hr)) {
            DEBUG_STREAM("Failed to Map Memory Ptr to Texture");
            exit(1);
        }
    }


    D3D12_SUBRESOURCE_DATA subresourceData {};
    subresourceData.pData = mem_ptr;
    subresourceData.SlicePitch = (LONG_PTR)bytesPerRow * desc.Height;
    subresourceData.RowPitch = (LONG_PTR)bytesPerRow;

    ID3D12Device *dev;
    cpuSideresource->GetDevice(__uuidof(*dev),(void **)&dev);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};

    UINT rows;
    UINT64 rowSize;

    UINT64 totalBytes;

    dev->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&totalBytes);

    if(bytes != nullptr) {

        D3D12_MEMCPY_DEST dest{((PBYTE) bytes) + footprint.Offset, footprint.Footprint.RowPitch,
                               footprint.Footprint.RowPitch * rows};
        MemcpySubresource(&dest, &subresourceData, rowSize, rows, 1);
    }

    if(bytes != nullptr) {
        cpuSideresource->Unmap(0, nullptr);
    }
    dev->Release();
    return totalBytes;
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

_NAMESPACE_END_