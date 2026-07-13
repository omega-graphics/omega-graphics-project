#include "GTEBase.h"
#include "GEMesh.h"
#include "GETextureAsset.h"

#include <future>
#include <string>
#include <vector>

#ifndef OMEGAGTE_GEMESHASSET_H
#define OMEGAGTE_GEMESHASSET_H

_NAMESPACE_BEGIN_

    /// @brief Loads mesh data from standard 3D model formats and produces
    /// a `GEMesh`. Backend-agnostic public API; the concrete subclass is
    /// selected by `Create()` based on the linked-in graphics backend.
    ///
    /// Supported formats are backend-specific. The Metal backend
    /// (Model I/O / `MTKMesh`) handles **OBJ**, **glTF 2.0**, USD, and
    /// Alembic out of the box. D3D12 and Vulkan implementations land in
    /// Phase 3.4.
    ///
    /// MeshAsset returns a fully-populated `GEMesh` whose
    /// `textureBindings` map already includes any base-color texture
    /// referenced by the source material; those textures are owned by
    /// `TextureAsset` instances exposed through `textureAssets()`.
    class OMEGAGTE_EXPORT GEMeshAsset {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEMeshAsset")

        /// @brief Options used during load.
        struct OMEGAGTE_EXPORT LoadOptions {
            /// Vertex layout the caller wants on the resulting `GEMesh`.
            /// Attributes the source format does not provide are written
            /// as zeros (with a one-shot warning); attributes the source
            /// has but the caller did not request are dropped.
            GEMeshDescriptor desiredDescriptor = {};
            /// Resolve and load referenced material textures (e.g. glTF
            /// base color) into `TextureAsset` instances and into the
            /// resulting `GEMesh::textureBindings`.
            bool loadMaterialTextures = true;
            /// Slot to bind the base-color texture into, when present.
            /// Matches the OmegaSL resource register the user's shader
            /// expects for the base-color sampler.
            unsigned baseColorSlot = 0;
        };

        /// @brief Synchronously load a mesh file from disk and build a
        /// `GEMesh`. Returns `false` and logs on failure.
        virtual bool load(const std::string & path,
                          const LoadOptions & options) = 0;

        /// @brief Convenience overload using default `LoadOptions`.
        bool load(const std::string & path);

        /// @brief Asynchronous variant. Default implementation runs
        /// `load` on `std::async(std::launch::async, ...)`.
        virtual std::future<bool> loadAsync(const std::string & path,
                                            const LoadOptions & options);

        /// @brief Convenience async overload using default `LoadOptions`.
        std::future<bool> loadAsync(const std::string & path);

        /// @brief The loaded mesh, or null if load was not called or
        /// failed.
        OMEGA_NODISCARD virtual SharedHandle<GEMesh> mesh() const = 0;

        /// @brief All `TextureAsset`s the loader created for material
        /// textures referenced by the source mesh. Empty if
        /// `loadMaterialTextures` was false or the source has no
        /// material textures.
        OMEGA_NODISCARD virtual std::vector<SharedHandle<GETextureAsset>> textureAssets() const = 0;

        /// @brief Free GPU and CPU resources held by this asset early.
        /// Optional — destruction handles release automatically.
        virtual void release() = 0;

        /// @brief Construct an instance bound to the given graphics
        /// engine.
        static SharedHandle<GEMeshAsset> Create(SharedHandle<OmegaGraphicsEngine> & engine);

        virtual ~GEMeshAsset() = default;
    };

_NAMESPACE_END_

#endif
