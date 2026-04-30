#include "omegaGTE/GETextureAsset.h"

#include <iostream>
#include <stdexcept>

_NAMESPACE_BEGIN_

namespace {

class GED3D12TextureAsset : public GETextureAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
public:
    explicit GED3D12TextureAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & /*options*/) override {
        std::cerr << "[GETextureAsset/D3D12] error: load('" << path
                  << "') is not implemented; lands in Phase 3.4 with DirectXTex."
                  << std::endl;
        throw std::runtime_error("GETextureAsset D3D12 backend not implemented yet");
    }

    SharedHandle<GETexture> texture() const override { return nullptr; }
    TextureDescriptor descriptor() const override { return TextureDescriptor{}; }
    void release() override {}
};

}  // namespace

SharedHandle<GETextureAsset> GETextureAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GETextureAsset>(new GED3D12TextureAsset(engine));
}

_NAMESPACE_END_
