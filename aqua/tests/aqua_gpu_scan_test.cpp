// AQUA Phase 6g — reusable-exclusive-scan stage-isolation test (§14.3 6g).
// The AQScan.omegasl reduce-then-scan chain (per-block shared-memory scans →
// recursively scanned block sums → uniform add) vs a sequential CPU exclusive
// prefix sum over randomized inputs. Integer adds make the GPU result
// BIT-exact — equality here is ==, not a band — and two runs must match
// byte-for-byte (within-path determinism).
//
// Sizes cross every structural boundary: sub-block, exact block, block+1,
// multi-block, and two/three recursion levels (block size 128 ⇒ level
// boundaries at 128 and 16384).
//
// INTERNAL test (src/ headers + AQUA_KERNELS_LIB_PATH), device-guarded.

#include "AQComputeBackend.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GE.h>
#include <omegaGTE/GECommandQueue.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Deterministic pseudo-random fill (SplitMix64, values kept small enough that
// a 300k-element sum stays far from uint32 wrap).
std::uint64_t mix(std::uint64_t& s) {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

OmegaCommon::Vector<std::uint32_t> randomInput(std::size_t n, std::uint64_t seed) {
    OmegaCommon::Vector<std::uint32_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint32_t>(mix(seed) % 1000u);
    }
    return v;
}

OmegaCommon::Vector<std::uint32_t> cpuExclusiveScan(const OmegaCommon::Vector<std::uint32_t>& in) {
    OmegaCommon::Vector<std::uint32_t> out(in.size());
    std::uint32_t acc = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = acc;
        acc += in[i];
    }
    return out;
}

} // namespace

int main() {
    std::printf("== AQUA Phase 6g: reusable GPU exclusive scan vs CPU prefix sum ==\n");

    OmegaCommon::Vector<SharedHandle<OmegaGTE::GTEDevice>> devices =
        OmegaGTE::enumerateDevices();
    if (devices.empty()) {
        std::printf("[SKIP] no GTE device on this host — GPU scan test skipped\n");
        return 0;
    }

    OmegaGTE::GTE gte = OmegaGTE::Init(devices[0]);
    check(static_cast<bool>(gte.graphicsEngine), "GTE initialized a graphics engine");

    OmegaGTE::GECommandQueueDesc queueDesc;
    queueDesc.maxBufferCount = 4;
    auto queue = gte.graphicsEngine->makeCommandQueue(queueDesc);

    auto backend = AQComputeBackend::TryCreate(gte.graphicsEngine, queue);
    check(backend != nullptr, "AQComputeBackend::TryCreate succeeds");
    if (!backend) { OmegaGTE::Close(gte); return 1; }

    check(backend->loadKernelLibrary(OmegaCommon::String(AQUA_KERNELS_LIB_PATH)),
          "loadKernelLibrary loads the merged AQKernels.omegasllib");

    // Structural boundary sizes for block = 128: sub-block, exact block,
    // block+1, several blocks, level-2 boundary (128^2), and a large 3-level
    // case shaped like the 6h compaction workload.
    const std::size_t sizes[] = {1, 2, 127, 128, 129, 1000, 4096,
                                 16384, 16385, 100000, 300000};
    for (std::size_t n : sizes) {
        const auto in = randomInput(n, 0xC0FFEEull + n);
        const auto expect = cpuExclusiveScan(in);
        OmegaCommon::Vector<std::uint32_t> got;
        const bool ran = backend->scanExclusive(in, got);
        bool exact = ran && got.size() == expect.size();
        std::size_t firstBad = n;
        for (std::size_t i = 0; exact && i < n; ++i) {
            if (got[i] != expect[i]) { exact = false; firstBad = i; }
        }
        if (!exact && ran && firstBad < n) {
            std::printf("  n=%zu first mismatch at %zu: gpu %u cpu %u\n",
                        n, firstBad, got[firstBad], expect[firstBad]);
        }
        check(exact, "exclusive scan bit-exact at n = " + std::to_string(n));
    }

    // A 0/1-flag input — the exact shape the 6h stable compaction scans.
    {
        const std::size_t n = 50000;
        auto in = randomInput(n, 0xF1A65ull);
        for (auto& v : in) v = v & 1u;
        const auto expect = cpuExclusiveScan(in);
        OmegaCommon::Vector<std::uint32_t> got;
        bool exact = backend->scanExclusive(in, got) && got.size() == n;
        for (std::size_t i = 0; exact && i < n; ++i) exact = (got[i] == expect[i]);
        check(exact, "0/1 alive-flag scan (the compaction shape) is bit-exact");
    }

    // Within-path determinism: same input twice, byte-identical output.
    {
        const auto in = randomInput(77777, 0xDE7E12ull);
        OmegaCommon::Vector<std::uint32_t> a, b;
        bool ran = backend->scanExclusive(in, a) && backend->scanExclusive(in, b);
        check(ran && a.size() == b.size() &&
              std::memcmp(a.data(), b.data(), a.size() * sizeof(std::uint32_t)) == 0,
              "two GPU scans are byte-identical (within-path determinism)");
    }

    // Empty input is a clean no-op.
    {
        OmegaCommon::Vector<std::uint32_t> in, out;
        check(backend->scanExclusive(in, out) && out.empty(), "empty input is a no-op");
    }

    OmegaGTE::Close(gte);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
    } else {
        std::printf("%d FAILURE(S)\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
