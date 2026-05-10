#include "omegaGTE/GEMeshAsset.h"
#include "omegaGTE/GEMesh.h"
#include "omegaGTE/GETextureAsset.h"
#include "GED3D12.h"
#include "../common/MeshParser.h"

#include <cstring>
#include <iostream>
#include <memory>

_NAMESPACE_BEGIN_

namespace {

class GED3D12MeshAsset : public GEMeshAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GEMesh> loadedMesh;
    std::vector<SharedHandle<GETextureAsset>> loadedTextures;

public:
    explicit GED3D12MeshAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            std::cerr << "[GEMeshAsset/D3D12] error: no engine bound." << std::endl;
            return false;
        }
        auto *d3dEngine = dynamic_cast<GED3D12Engine *>(engine.get());
        if (!d3dEngine) {
            std::cerr << "[GEMeshAsset/D3D12] error: engine is not a D3D12 engine." << std::endl;
            return false;
        }

        MeshParser::ParsedMesh parsed;
        if (!MeshParser::parseMesh(path, options.desiredDescriptor, parsed)) {
            return false;
        }

        // Upload-heap GEBuffer matches the Metal path: persistently
        // mappable, suitable for shader read on D3D12. A future pass can
        // promote to a default-heap copy via the engine's command queue
        // when we want VRAM-resident vertex buffers.
        BufferDescriptor bdesc;
        bdesc.usage        = BufferDescriptor::Upload;
        bdesc.len          = (size_t)parsed.vertexCount * parsed.stride;
        bdesc.objectStride = parsed.stride;
        bdesc.opts         = Shared;
        SharedHandle<GEBuffer> vbuf = engine->makeBuffer(bdesc);
        if (!vbuf) {
            std::cerr << "[GEMeshAsset/D3D12] error: makeBuffer failed." << std::endl;
            return false;
        }

        // Map the underlying ID3D12Resource and memcpy. Mirrors the
        // Metal `mtlBuf.contents` direct-write path.
        auto *d3dBuf = static_cast<GED3D12Buffer *>(vbuf.get());
        if (!d3dBuf || !d3dBuf->buffer) {
            std::cerr << "[GEMeshAsset/D3D12] error: native buffer is null." << std::endl;
            return false;
        }
        void *mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (FAILED(d3dBuf->buffer->Map(0, &noRead, &mapped)) || !mapped) {
            std::cerr << "[GEMeshAsset/D3D12] error: buffer Map failed." << std::endl;
            return false;
        }
        std::memcpy(mapped, parsed.packed.data(),
                    parsed.packed.size() * sizeof(float));
        d3dBuf->buffer->Unmap(0, nullptr);

        auto m = std::make_shared<GEMesh>();
        m->vertexBuffer = vbuf;
        m->vertexCount  = parsed.vertexCount;
        m->vertexStride = parsed.stride;
        m->descriptor   = options.desiredDescriptor;

        // Resolve material textures via TextureAsset. Same contract as
        // the Metal backend: first base-color path wins, attached at
        // `options.baseColorSlot`.
        if (options.loadMaterialTextures && !parsed.baseColorTexturePath.empty()) {
            auto texAsset = GETextureAsset::Create(engine);
            GETextureAsset::LoadOptions topts;
            topts.generateMipmaps = true;
            topts.sRGB = true;
            if (texAsset->load(parsed.baseColorTexturePath, topts)) {
                if (auto tex = texAsset->texture()) {
                    m->textureBindings[options.baseColorSlot] = tex;
                }
                loadedTextures.push_back(texAsset);
            } else {
                std::cerr << "[GEMeshAsset/D3D12] warning: base-color texture '"
                          << parsed.baseColorTexturePath << "' failed to load." << std::endl;
            }
        }

        loadedMesh = m;
        return true;
    }

    SharedHandle<GEMesh> mesh() const override { return loadedMesh; }

    std::vector<SharedHandle<GETextureAsset>> textureAssets() const override {
        return loadedTextures;
    }

    void release() override {
        loadedMesh.reset();
        loadedTextures.clear();
    }
};

}  // namespace

SharedHandle<GEMeshAsset> GEMeshAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GEMeshAsset>(new GED3D12MeshAsset(engine));
}

_NAMESPACE_END_
