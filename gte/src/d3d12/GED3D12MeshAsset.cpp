#include "omegaGTE/GEMeshAsset.h"

#include <iostream>
#include <stdexcept>

_NAMESPACE_BEGIN_

namespace {

class GED3D12MeshAsset : public GEMeshAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
public:
    explicit GED3D12MeshAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & /*options*/) override {
        std::cerr << "[GEMeshAsset/D3D12] error: load('" << path
                  << "') is not implemented; lands in Phase 3.4 with DirectXMesh + cgltf."
                  << std::endl;
        throw std::runtime_error("GEMeshAsset D3D12 backend not implemented yet");
    }

    SharedHandle<GEMesh> mesh() const override { return nullptr; }
    std::vector<SharedHandle<GETextureAsset>> textureAssets() const override { return {}; }
    void release() override {}
};

}  // namespace

SharedHandle<GEMeshAsset> GEMeshAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GEMeshAsset>(new GED3D12MeshAsset(engine));
}

_NAMESPACE_END_
