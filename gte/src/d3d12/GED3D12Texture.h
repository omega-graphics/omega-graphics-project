#include "GED3D12.h"
#include "omegaGTE/GETexture.h"

#ifndef OMEGAGTE_D3D12_GED3D12TEXTURE_H
#define OMEGAGTE_D3D12_GED3D12TEXTURE_H

_NAMESPACE_BEGIN_

class GED3D12Texture : public GETexture {
public:
    void copyBytes(void *bytes, size_t len) override;
    size_t getBytes(void *bytes, size_t bytesPerRow) override;

    bool onGpu;

    ComPtr<ID3D12Resource> resource;

    ComPtr<ID3D12Resource> cpuSideresource;
    ComPtr<ID3D12DescriptorHeap> srvDescHeap;
    ComPtr<ID3D12DescriptorHeap> uavDescHeap;
    ComPtr<ID3D12DescriptorHeap> rtvDescHeap;
    ComPtr<ID3D12DescriptorHeap> dsvDescHeap;

    D3D12_RESOURCE_STATES currentState;

    void downloadTextureToReadbackHeap(ID3D12GraphicsCommandList *commandList);
    void uploadTextureFromUploadHeap(ID3D12GraphicsCommandList *commandList);

    void updateAndValidateStatus(ID3D12GraphicsCommandList *commandList);
    bool needsValidation();

    void setName(OmegaCommon::StrRef name) override{
        ATL::CStringW wstr(name.data());
        resource->SetName(wstr);
    }

    void *native() override {
        return (void *)resource.Get();
    }

    explicit GED3D12Texture(
            const GETextureType & type,
            const GETextureUsage & usage,
            const TexturePixelFormat & pixelFormat,
            ID3D12Resource *res,
            ID3D12Resource *cpuSideRes,
            ID3D12DescriptorHeap *descHeap,
            ID3D12DescriptorHeap *uavDescHeap,
            ID3D12DescriptorHeap *rtvDescHeap,
            ID3D12DescriptorHeap *dsvDescHeap,
            D3D12_RESOURCE_STATES & currentState);
};

_NAMESPACE_END_

#endif