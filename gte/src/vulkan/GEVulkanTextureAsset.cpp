#include "omegaGTE/GETextureAsset.h"

#include <iostream>
#include <stdexcept>

_NAMESPACE_BEGIN_

namespace {

class GEVulkanTextureAsset : public GETextureAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
public:
    explicit GEVulkanTextureAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & /*options*/) override {
        std::cerr << "[GETextureAsset/Vulkan] error: load('" << path
                  << "') is not implemented; lands in Phase 3.4 with libktx + stb_image."
                  << std::endl;
        throw std::runtime_error("GETextureAsset Vulkan backend not implemented yet");
    }

    SharedHandle<GETexture> texture() const override { return nullptr; }
    TextureDescriptor descriptor() const override { return TextureDescriptor{}; }
    void release() override {}
};

}  // namespace

SharedHandle<GETextureAsset> GETextureAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GETextureAsset>(new GEVulkanTextureAsset(engine));
}

_NAMESPACE_END_
