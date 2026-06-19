// .omegasllib file round-trip test — OmegaSL linker Phase 0.
//
// Every other GTE shader test on this host uses the *runtime* compile path
// (omegaSlCompiler->compile → loadShaderLibraryRuntime), which builds the
// library in memory and never touches the on-disk container format. This test
// is the one that exercises the file path: omegaslc writes a real
// `.omegasllib` (with the Phase 0 magic / version / backend-id prefix) at build
// time, and this program loads it back through
// OmegaGraphicsEngine::loadShaderLibrary — the public reader
// (loadShaderLibraryFromInputStream). It is the regression guard for the
// writer↔reader byte-for-byte lockstep that Phase 0 introduces.
//
// Three checks:
//   1. the valid library loads and its shader resolves (happy round-trip);
//   2. a copy with a corrupted magic is rejected (returns null);
//   3. a copy with an unknown format version is rejected (returns null).
//
// ROUNDTRIP_LIB_PATH is the build-dir path to the omegaslc-produced fixture,
// injected by CMake.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GTEShader.h>
#include <omegaGTE/GEPipeline.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace OmegaGTE;

namespace {

std::vector<char> readAll(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

// Write `bytes` with a single byte overwritten, returning the new file path.
std::string writeMutated(const std::vector<char> &bytes, std::size_t pos,
                         char value, const std::string &suffix) {
    std::vector<char> copy = bytes;
    if (pos < copy.size()) {
        copy[pos] = value;
    }
    std::string path = std::string(ROUNDTRIP_LIB_PATH) + "." + suffix;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(copy.data(), static_cast<std::streamsize>(copy.size()));
    out.close();
    return path;
}

} // namespace

int main() {
    auto gte = OmegaGTE::InitWithDefaultDevice();
    int failures = 0;

    // 1. Valid library loads and the fixture shader resolves.
    auto lib = gte.graphicsEngine->loadShaderLibrary(ROUNDTRIP_LIB_PATH);
    if (!lib) {
        std::cerr << "FAIL: valid .omegasllib did not load\n";
        ++failures;
    } else {
        auto shader = lib->shaders["roundTripCompute"];
        if (!shader) {
            std::cerr << "FAIL: roundTripCompute did not resolve from a valid library\n";
            ++failures;
        } else if (shader->isUnsupported) {
            std::cerr << "FAIL: roundTripCompute loaded as an unsupported sentinel\n";
            ++failures;
        } else {
            std::cerr << "PASS: valid library round-tripped, roundTripCompute resolved\n";
            /// Build a real compute pipeline from the file-loaded shader.
            /// Pipeline creation dereferences the shader's `internal.pLayout`
            /// (the rt_in / rt_out buffer descriptors) *after* load — the exact
            /// post-load read the archive lifetime (`GTEShaderLibrary::
            /// _backingStore`) has to keep valid now that the reader no longer
            /// leaks the buffers. A dangling layout pointer would crash or fail
            /// here, so this is the lifetime regression guard.
            ComputePipelineDescriptor pd{};
            pd.computeFunc = shader;
            auto pipeline = gte.graphicsEngine->makeComputePipelineState(pd);
            if (!pipeline) {
                std::cerr << "FAIL: makeComputePipelineState returned null for the loaded shader\n";
                ++failures;
            } else {
                std::cerr << "PASS: built a compute pipeline from the file-loaded shader "
                             "(layout pointers valid post-load)\n";
            }
        }
    }

    const std::vector<char> bytes = readAll(ROUNDTRIP_LIB_PATH);
    if (bytes.size() < 12) {
        std::cerr << "FAIL: fixture library is smaller than the 12-byte prefix\n";
        OmegaGTE::Close(gte);
        return 1;
    }

    // 2. Corrupted magic (byte 0 'O' -> 'X') must be rejected.
    {
        std::string bad = writeMutated(bytes, 0, 'X', "badmagic");
        if (gte.graphicsEngine->loadShaderLibrary(bad.c_str())) {
            std::cerr << "FAIL: library with a bad magic loaded (should have been rejected)\n";
            ++failures;
        } else {
            std::cerr << "PASS: bad-magic library rejected\n";
        }
    }

    // 3. Unknown format version (low byte of the version u32 at offset 4
    //    bumped well past OMEGASLLIB_FORMAT_VERSION) must be rejected.
    {
        std::string bad = writeMutated(bytes, 4, static_cast<char>(0x7F), "badversion");
        if (gte.graphicsEngine->loadShaderLibrary(bad.c_str())) {
            std::cerr << "FAIL: library with an unknown format version loaded (should have been rejected)\n";
            ++failures;
        } else {
            std::cerr << "PASS: unknown-format-version library rejected\n";
        }
    }

    OmegaGTE::Close(gte);
    std::cerr << (failures == 0 ? "ROUNDTRIP OK\n" : "ROUNDTRIP FAILED\n");
    return failures == 0 ? 0 : 1;
}
