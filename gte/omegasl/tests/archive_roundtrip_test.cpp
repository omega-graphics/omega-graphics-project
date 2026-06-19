// ShaderArchive test helper (OmegaSL linker Phase 1 + 3). Two modes:
//
//   archive_roundtrip_test <lib.omegasllib> [expected_shader_count]
//       Read->Write byte-identical round-trip: parse the archive, serialize it
//       straight back, and assert the bytes match the original file exactly —
//       proving the shared (de)serializer is its own inverse. With an optional
//       expected count, also assert the archive holds that many shaders (used
//       to check a `--link` merge has Sigma(inputs) entries). Pure host code,
//       no GPU device — the reusable core the link tool depends on.
//
//   archive_roundtrip_test backend-variant <in.omegasllib> <out.omegasllib>
//       Write a copy of <in> with its backend id bumped to a *different*
//       backend (id+1 mod 3). Lets a cross-platform CTest manufacture a
//       backend-mismatched input for the linker's mismatch-rejection test
//       without depending on a second graphics platform.

#include "../src/ShaderArchive.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::string &path, bool &ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    if (!ok) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int backendVariant(const std::string &inPath, const std::string &outPath) {
    bool ok = false;
    const std::string original = readFile(inPath, ok);
    if (!ok) {
        std::cerr << "cannot open " << inPath << "\n";
        return 2;
    }
    std::istringstream parse(original);
    omegasl::OmegaSLShaderArchive arc;
    std::string err;
    if (!omegasl::ReadShaderArchive(parse, arc, err)) {
        std::cerr << "ReadShaderArchive(" << inPath << "): " << err << "\n";
        return 1;
    }
    arc.backendId = static_cast<std::uint8_t>((arc.backendId + 1) % 3);
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!omegasl::WriteShaderArchive(out, arc, err)) {
        std::cerr << "WriteShaderArchive(" << outPath << "): " << err << "\n";
        return 1;
    }
    std::cerr << "wrote backend-variant (backend id " << unsigned(arc.backendId) << ") to "
              << outPath << "\n";
    return 0;
}

int verify(const std::string &path, long expectedCount) {
    bool ok = false;
    const std::string original = readFile(path, ok);
    if (!ok) {
        std::cerr << "cannot open " << path << "\n";
        return 2;
    }
    std::istringstream parse(original);
    omegasl::OmegaSLShaderArchive arc;
    std::string err;
    if (!omegasl::ReadShaderArchive(parse, arc, err)) {
        std::cerr << "FAIL: ReadShaderArchive(" << path << "): " << err << "\n";
        return 1;
    }
    std::ostringstream out;
    if (!omegasl::WriteShaderArchive(out, arc, err)) {
        std::cerr << "FAIL: WriteShaderArchive: " << err << "\n";
        return 1;
    }
    const std::string roundtripped = out.str();
    if (roundtripped != original) {
        std::cerr << "FAIL: Read->Write not byte-identical (original " << original.size()
                  << " bytes, round-trip " << roundtripped.size() << " bytes)\n";
        const std::size_t n = std::min(original.size(), roundtripped.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (original[i] != roundtripped[i]) {
                std::cerr << "  first differing byte at offset " << i << "\n";
                break;
            }
        }
        return 1;
    }
    if (expectedCount >= 0 && static_cast<long>(arc.shaders.size()) != expectedCount) {
        std::cerr << "FAIL: expected " << expectedCount << " shaders but archive holds "
                  << arc.shaders.size() << "\n";
        return 1;
    }
    std::cerr << "PASS: ShaderArchive Read->Write byte-identical (" << original.size()
              << " bytes, " << arc.shaders.size() << " shaders)\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::string(argv[1]) == "backend-variant") {
        if (argc < 4) {
            std::cerr << "usage: " << argv[0] << " backend-variant <in> <out>\n";
            return 2;
        }
        return backendVariant(argv[2], argv[3]);
    }

    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <lib.omegasllib> [expected_shader_count]\n"
                  << "       " << argv[0] << " backend-variant <in> <out>\n";
        return 2;
    }
    const long expectedCount = (argc >= 3) ? std::stol(argv[2]) : -1;
    return verify(argv[1], expectedCount);
}
