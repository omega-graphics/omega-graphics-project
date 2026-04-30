#include "omegaGTE/GETextureAsset.h"

#include <utility>

_NAMESPACE_BEGIN_

bool GETextureAsset::load(const std::string & path) {
    return load(path, LoadOptions{});
}

std::future<bool> GETextureAsset::loadAsync(const std::string & path,
                                            const LoadOptions & options) {
    std::string pathCopy = path;
    LoadOptions opts = options;
    return std::async(std::launch::async, [this, pathCopy = std::move(pathCopy), opts]() {
        return this->load(pathCopy, opts);
    });
}

std::future<bool> GETextureAsset::loadAsync(const std::string & path) {
    return loadAsync(path, LoadOptions{});
}

_NAMESPACE_END_
