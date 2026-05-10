#include "omegaGTE/GETextureAsset.h"
#include "GED3D12.h"
#include "GED3D12Texture.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

_NAMESPACE_BEGIN_

namespace {

// ─── format helpers ─────────────────────────────────────────────────

std::wstring widen(const std::string &s) {
    return std::wstring(s.begin(), s.end());
}

std::string lowerExt(const std::string &path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

/// Map a DXGI format back to the engine's `TexturePixelFormat`. The
/// engine enum only models a handful of formats; BC / HDR / depth
/// formats fall through to RGBA8Unorm with a one-shot warning. The
/// `GETexture` itself still works because we own the SRV heap directly
/// — only the descriptor() return shape is lossy.
TexturePixelFormat mapDxgiToEngineFormat(DXGI_FORMAT fmt) {
    static bool warned = false;
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:        return TexturePixelFormat::RGBA8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return TexturePixelFormat::RGBA8Unorm_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:        return TexturePixelFormat::BGRA8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return TexturePixelFormat::BGRA8Unorm_SRGB;
        case DXGI_FORMAT_R16G16B16A16_UNORM:    return TexturePixelFormat::RGBA16Unorm;
        default:
            if (!warned) {
                std::cerr << "[GETextureAsset/D3D12] warning: DXGI format " << (int)fmt
                          << " is not modelled in TexturePixelFormat; "
                             "descriptor() reports RGBA8Unorm. The texture "
                             "still binds correctly via its native SRV heap."
                          << std::endl;
                warned = true;
            }
            return TexturePixelFormat::RGBA8Unorm;
    }
}

/// Encode an array+sampleCount-aware view dimension for the SRV
/// descriptor. Mirrors the dispatch in GED3D12Engine::makeTexture.
void fillSrvDesc(const DirectX::TexMetadata &md,
                 D3D12_SHADER_RESOURCE_VIEW_DESC &desc) {
    desc.Format = md.format;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const UINT mips = (UINT)md.mipLevels;

    if (md.IsCubemap()) {
        if (md.arraySize > 6) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            desc.TextureCubeArray.MipLevels = mips;
            desc.TextureCubeArray.NumCubes  = (UINT)(md.arraySize / 6);
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube.MipLevels = mips;
        }
        return;
    }
    switch (md.dimension) {
        case DirectX::TEX_DIMENSION_TEXTURE1D:
            if (md.arraySize > 1) {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                desc.Texture1DArray.MipLevels = mips;
                desc.Texture1DArray.ArraySize = (UINT)md.arraySize;
            } else {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                desc.Texture1D.MipLevels = mips;
            }
            break;
        case DirectX::TEX_DIMENSION_TEXTURE3D:
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipLevels = mips;
            break;
        case DirectX::TEX_DIMENSION_TEXTURE2D:
        default:
            if (md.arraySize > 1) {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.MipLevels = mips;
                desc.Texture2DArray.ArraySize = (UINT)md.arraySize;
            } else {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipLevels = mips;
            }
            break;
    }
}

GETexture::GETextureType engineTypeFor(const DirectX::TexMetadata &md) {
    switch (md.dimension) {
        case DirectX::TEX_DIMENSION_TEXTURE1D: return GETexture::Texture1D;
        case DirectX::TEX_DIMENSION_TEXTURE3D: return GETexture::Texture3D;
        default:                               return GETexture::Texture2D;
    }
}

TextureKind engineKindFor(const DirectX::TexMetadata &md) {
    if (md.IsCubemap()) {
        return md.arraySize > 6 ? TextureKind::TexCubeArray : TextureKind::TexCube;
    }
    switch (md.dimension) {
        case DirectX::TEX_DIMENSION_TEXTURE1D:
            return md.arraySize > 1 ? TextureKind::Tex1DArray : TextureKind::Tex1D;
        case DirectX::TEX_DIMENSION_TEXTURE3D:
            return TextureKind::Tex3D;
        default:
            return md.arraySize > 1 ? TextureKind::Tex2DArray : TextureKind::Tex2D;
    }
}

// ─── load + upload ──────────────────────────────────────────────────

class GED3D12TextureAsset : public GETextureAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GETexture> loadedTexture;
    TextureDescriptor loadedDescriptor{};

    /// Load the file at `path` into a DirectXTex ScratchImage. Selects
    /// the loader by extension. Returns S_OK on success.
    HRESULT loadScratchImage(const std::string &path,
                             DirectX::TexMetadata &md,
                             DirectX::ScratchImage &img) {
        const std::wstring wpath = widen(path);
        const std::string ext = lowerExt(path);
        if (ext == "dds") {
            return DirectX::LoadFromDDSFile(wpath.c_str(),
                                            DirectX::DDS_FLAGS_NONE, &md, img);
        }
        if (ext == "hdr") {
            return DirectX::LoadFromHDRFile(wpath.c_str(), &md, img);
        }
        if (ext == "tga") {
            return DirectX::LoadFromTGAFile(wpath.c_str(),
                                            DirectX::TGA_FLAGS_NONE, &md, img);
        }
        // PNG / JPEG / TIFF / BMP / GIF / ICO / WMP via WIC.
        return DirectX::LoadFromWICFile(wpath.c_str(),
                                        DirectX::WIC_FLAGS_NONE, &md, img);
    }

    /// Submit a one-shot copy command list on a transient direct queue
    /// and block until it completes. We don't borrow the engine's main
    /// queue because we don't want load() to interleave with rendering.
    bool submitUpload(ID3D12Device *device,
                      ID3D12Resource *target,
                      const std::vector<D3D12_SUBRESOURCE_DATA> &subs) {
        // Create the upload buffer sized for all subresources.
        const UINT64 uploadSize = GetRequiredIntermediateSize(target, 0,
                                                              (UINT)subs.size());
        auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        ComPtr<ID3D12Resource> upload;
        HRESULT hr = device->CreateCommittedResource(
            &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr)) {
            std::cerr << "[GETextureAsset/D3D12] upload heap creation failed." << std::endl;
            return false;
        }

        // Transient direct queue + allocator + fence. Direct queue (not
        // copy) so we can issue the resource barrier in the same list.
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)))) {
            std::cerr << "[GETextureAsset/D3D12] CreateCommandQueue failed." << std::endl;
            return false;
        }
        ComPtr<ID3D12CommandAllocator> alloc;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&alloc)))) {
            std::cerr << "[GETextureAsset/D3D12] CreateCommandAllocator failed." << std::endl;
            return false;
        }
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
            std::cerr << "[GETextureAsset/D3D12] CreateCommandList failed." << std::endl;
            return false;
        }

        UpdateSubresources(list.Get(), target, upload.Get(),
                           0, 0, (UINT)subs.size(),
                           const_cast<D3D12_SUBRESOURCE_DATA *>(subs.data()));

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            target,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &barrier);
        list->Close();

        ID3D12CommandList *raw = list.Get();
        queue->ExecuteCommandLists(1, &raw);

        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                       IID_PPV_ARGS(&fence)))) {
            std::cerr << "[GETextureAsset/D3D12] CreateFence failed." << std::endl;
            return false;
        }
        if (FAILED(queue->Signal(fence.Get(), 1))) return false;
        if (fence->GetCompletedValue() < 1) {
            HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!evt) return false;
            fence->SetEventOnCompletion(1, evt);
            WaitForSingleObject(evt, INFINITE);
            CloseHandle(evt);
        }
        return true;
    }

public:
    explicit GED3D12TextureAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            std::cerr << "[GETextureAsset/D3D12] error: no engine bound." << std::endl;
            return false;
        }
        auto *d3dEngine = dynamic_cast<GED3D12Engine *>(engine.get());
        if (!d3dEngine || !d3dEngine->d3d12_device) {
            std::cerr << "[GETextureAsset/D3D12] error: engine is not a D3D12 engine." << std::endl;
            return false;
        }
        ID3D12Device *device = d3dEngine->d3d12_device.Get();

        DirectX::TexMetadata md{};
        DirectX::ScratchImage scratch;
        HRESULT hr = loadScratchImage(path, md, scratch);
        if (FAILED(hr)) {
            std::cerr << "[GETextureAsset/D3D12] failed to decode '" << path
                      << "' (hr=0x" << std::hex << hr << std::dec << ")" << std::endl;
            return false;
        }

        // Optional sRGB promotion for 8-bit unorm sources.
        if (options.sRGB && !DirectX::IsSRGB(md.format) && DirectX::HasAlpha(md.format)) {
            DXGI_FORMAT srgb = DirectX::MakeSRGB(md.format);
            if (srgb != md.format) {
                md.format = srgb;
                // Override per-image format too.
                for (size_t i = 0; i < scratch.GetImageCount(); ++i) {
                    const_cast<DirectX::Image *>(scratch.GetImages())[i].format = srgb;
                }
            }
        }

        // Optional mipmap generation. Only meaningful for uncompressed
        // single-mip 2D inputs; everything else passes through.
        DirectX::ScratchImage withMips;
        DirectX::ScratchImage *upload = &scratch;
        if (options.generateMipmaps && md.mipLevels == 1
            && md.dimension == DirectX::TEX_DIMENSION_TEXTURE2D
            && !DirectX::IsCompressed(md.format)) {
            hr = DirectX::GenerateMipMaps(scratch.GetImages(),
                                          scratch.GetImageCount(),
                                          scratch.GetMetadata(),
                                          DirectX::TEX_FILTER_DEFAULT,
                                          0, withMips);
            if (SUCCEEDED(hr)) {
                upload = &withMips;
                md = withMips.GetMetadata();
            } else {
                std::cerr << "[GETextureAsset/D3D12] mipgen failed (hr=0x"
                          << std::hex << hr << std::dec << "); using base mip only." << std::endl;
            }
        }

        // Create the GPU resource (default heap), initial state COPY_DEST
        // so UpdateSubresources can write into it.
        ComPtr<ID3D12Resource> resource;
        hr = DirectX::CreateTextureEx(device, md,
                                      D3D12_RESOURCE_FLAG_NONE,
                                      DirectX::CREATETEX_DEFAULT,
                                      &resource);
        if (FAILED(hr)) {
            std::cerr << "[GETextureAsset/D3D12] CreateTextureEx failed (hr=0x"
                      << std::hex << hr << std::dec << ")" << std::endl;
            return false;
        }
        // Transition target to COPY_DEST inside the upload list. Its
        // creation state from CreateTextureEx is COMMON; UpdateSubresources
        // expects the target to already be in COPY_DEST. Fold that into
        // submitUpload via a leading barrier.
        // (We simplified: the helper only emits the trailing barrier and
        // assumes COPY_DEST. So issue the leading barrier here via a
        // tiny inline command list.)
        // Simpler: tell submitUpload to bracket both transitions.

        std::vector<D3D12_SUBRESOURCE_DATA> subs;
        hr = DirectX::PrepareUpload(device, upload->GetImages(),
                                    upload->GetImageCount(), md, subs);
        if (FAILED(hr)) {
            std::cerr << "[GETextureAsset/D3D12] PrepareUpload failed." << std::endl;
            return false;
        }

        // Run upload: this helper transitions target into COPY_DEST,
        // copies, then transitions to PIXEL_SHADER_RESOURCE. Since
        // CreateTextureEx leaves the resource in COMMON, UpdateSubresources
        // can write directly without an explicit pre-barrier on most
        // hardware (COMMON → copy is a promoted state). The trailing
        // barrier in submitUpload moves it to PIXEL_SHADER_RESOURCE.
        if (!submitUpload(device, resource.Get(), subs)) return false;

        // Build a single-descriptor SRV heap.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap *srvHeap = nullptr;
        hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap));
        if (FAILED(hr)) {
            std::cerr << "[GETextureAsset/D3D12] SRV heap creation failed." << std::endl;
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        fillSrvDesc(md, srvDesc);
        device->CreateShaderResourceView(resource.Get(), &srvDesc,
                                         srvHeap->GetCPUDescriptorHandleForHeapStart());

        // Wrap as a GED3D12Texture. The resource is already on the GPU
        // and in PIXEL_SHADER_RESOURCE state; pass that as currentState.
        const TexturePixelFormat enginePF = mapDxgiToEngineFormat(md.format);
        const GETexture::GETextureType etype = engineTypeFor(md);

        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ID3D12Resource *raw = resource.Get();
        raw->AddRef();  // GED3D12Texture holds via ComPtr; ScratchImage path
                        // didn't go through D3D12MA so allocations are null.

        auto *tex = new GED3D12Texture(etype, GETexture::ToGPU, enginePF,
                                       raw, /*cpuSideRes*/ nullptr,
                                       srvHeap, /*uavDescHeap*/ nullptr,
                                       /*rtvDescHeap*/ nullptr, /*dsvDescHeap*/ nullptr,
                                       state,
                                       /*d3d12maAllocation*/ nullptr,
                                       /*d3d12maCpuSideAllocation*/ nullptr);
        // Resource is already uploaded; mark so updateAndValidateStatus
        // skips the upload-heap path on first bind.
        tex->onGpu = true;
        tex->primarySrvDesc = srvDesc;
        tex->hasPrimarySrvDesc = true;
        tex->setShape(engineKindFor(md),
                      (unsigned)md.arraySize,
                      /*samples=*/1u);

        loadedTexture = SharedHandle<GETexture>(tex);

        loadedDescriptor = TextureDescriptor{};
        loadedDescriptor.type        = etype;
        loadedDescriptor.usage       = GETexture::ToGPU;
        loadedDescriptor.pixelFormat = enginePF;
        loadedDescriptor.width       = (unsigned)md.width;
        loadedDescriptor.height      = (unsigned)md.height;
        loadedDescriptor.depth       = (unsigned)md.depth;
        loadedDescriptor.mipLevels   = (unsigned)md.mipLevels;
        loadedDescriptor.sampleCount = 1;
        loadedDescriptor.kind        = engineKindFor(md);
        loadedDescriptor.arrayLayers = (unsigned)md.arraySize;

        return true;
    }

    SharedHandle<GETexture> texture() const override { return loadedTexture; }
    TextureDescriptor descriptor() const override { return loadedDescriptor; }

    void release() override {
        loadedTexture.reset();
        loadedDescriptor = TextureDescriptor{};
    }
};

}  // namespace

SharedHandle<GETextureAsset> GETextureAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GETextureAsset>(new GED3D12TextureAsset(engine));
}

_NAMESPACE_END_
