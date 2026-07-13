#include "omegaGTE/GEMeshAsset.h"
#include "omegaGTE/GEMesh.h"
#include "omegaGTE/GETextureAsset.h"
#include "GEVulkan.h"
#include "../common/MeshParser.h"

#include <cstring>
#include <iostream>
#include <memory>

_NAMESPACE_BEGIN_

namespace {

// Phase 3.4 Vulkan implementation. Mirrors GED3D12MeshAsset.cpp: format
// dispatch + CPU packing live in the shared MeshParser; the backend
// implementation only owns GPU upload. glTF (.gltf / .glb) and OBJ are
// supported via cgltf and the inline OBJ parser respectively; FBX is
// deferred along with the D3D12 backend.
class GEVulkanMeshAsset : public GEMeshAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GEMesh> loadedMesh;
    std::vector<SharedHandle<GETextureAsset>> loadedTextures;

public:
    explicit GEVulkanMeshAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            std::cerr << "[GEMeshAsset/Vulkan] error: no engine bound." << std::endl;
            return false;
        }
        auto *vkEngine = dynamic_cast<GEVulkanEngine *>(engine.get());
        if (!vkEngine) {
            std::cerr << "[GEMeshAsset/Vulkan] error: engine is not a Vulkan engine." << std::endl;
            return false;
        }

        MeshParser::ParsedMesh parsed;
        if (!MeshParser::parseMesh(path, options.desiredDescriptor, parsed)) {
            return false;
        }

        // HOST_VISIBLE upload buffer. Matches the D3D12 path: single
        // mapping, memcpy, no staging copy. A future pass can promote to
        // a GPU_ONLY buffer via the engine's transfer queue once we have
        // a shared transfer-list helper across backends.
        BufferDescriptor bdesc;
        bdesc.usage        = BufferDescriptor::Upload;
        bdesc.len          = (size_t)parsed.vertexCount * parsed.stride;
        bdesc.objectStride = parsed.stride;
        bdesc.opts         = Shared;
        SharedHandle<GEBuffer> vbuf = engine->makeBuffer(bdesc);
        if (!vbuf) {
            std::cerr << "[GEMeshAsset/Vulkan] error: makeBuffer failed." << std::endl;
            return false;
        }

        auto *vkBuf = static_cast<GEVulkanBuffer *>(vbuf.get());
        if (!vkBuf || vkBuf->buffer == VK_NULL_HANDLE || vkBuf->alloc == nullptr) {
            std::cerr << "[GEMeshAsset/Vulkan] error: native buffer is null." << std::endl;
            return false;
        }
        void *mapped = nullptr;
        if (vmaMapMemory(vkEngine->memAllocator, vkBuf->alloc, &mapped) != VK_SUCCESS
            || mapped == nullptr) {
            std::cerr << "[GEMeshAsset/Vulkan] error: vmaMapMemory failed." << std::endl;
            return false;
        }
        std::memcpy(mapped, parsed.packed.data(),
                    parsed.packed.size() * sizeof(float));
        vmaUnmapMemory(vkEngine->memAllocator, vkBuf->alloc);

        auto m = std::make_shared<GEMesh>();
        m->vertexBuffer = vbuf;
        m->vertexCount  = parsed.vertexCount;
        m->vertexStride = parsed.stride;
        m->descriptor   = options.desiredDescriptor;
        // Bound the mesh while the CPU stream is still in hand — it is dropped
        // the moment this function returns, and a caller placing the mesh in a
        // scene has no other way to learn its size.
        m->bounds       = geMeshComputeBounds(parsed.packed.data(),
                                              parsed.vertexCount, parsed.stride);

        // Resolve material textures via TextureAsset. Same contract as
        // the Metal / D3D12 backends: first base-color path wins,
        // attached at `options.baseColorSlot`.
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
                std::cerr << "[GEMeshAsset/Vulkan] warning: base-color texture '"
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
    return SharedHandle<GEMeshAsset>(new GEVulkanMeshAsset(engine));
}

_NAMESPACE_END_
