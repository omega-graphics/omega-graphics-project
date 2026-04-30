#include "omegaGTE/GEMeshAsset.h"

#include <utility>

_NAMESPACE_BEGIN_

bool GEMeshAsset::load(const std::string & path) {
    return load(path, LoadOptions{});
}

std::future<bool> GEMeshAsset::loadAsync(const std::string & path,
                                         const LoadOptions & options) {
    std::string pathCopy = path;
    LoadOptions opts = options;
    return std::async(std::launch::async, [this, pathCopy = std::move(pathCopy), opts]() {
        return this->load(pathCopy, opts);
    });
}

std::future<bool> GEMeshAsset::loadAsync(const std::string & path) {
    return loadAsync(path, LoadOptions{});
}

_NAMESPACE_END_
