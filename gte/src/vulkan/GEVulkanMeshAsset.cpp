#include "omegaGTE/GEMeshAsset.h"

#include <iostream>
#include <stdexcept>

_NAMESPACE_BEGIN_

namespace {

class GEVulkanMeshAsset : public GEMeshAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
public:
    explicit GEVulkanMeshAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & /*options*/) override {
        std::cerr << "[GEMeshAsset/Vulkan] error: load('" << path
                  << "') is not implemented; lands in Phase 3.4 with cgltf + meshoptimizer."
                  << std::endl;
        throw std::runtime_error("GEMeshAsset Vulkan backend not implemented yet");
    }

    SharedHandle<GEMesh> mesh() const override { return nullptr; }
    std::vector<SharedHandle<GETextureAsset>> textureAssets() const override { return {}; }
    void release() override {}
};

}  // namespace

SharedHandle<GEMeshAsset> GEMeshAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GEMeshAsset>(new GEVulkanMeshAsset(engine));
}

_NAMESPACE_END_
