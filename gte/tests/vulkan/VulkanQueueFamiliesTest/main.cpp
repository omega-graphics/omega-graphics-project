/// CommandQueue-Typed-Pool Phase 2 — unit test for
/// `OmegaGTE::VulkanQueueFamilies::Pick`. Pure CPU, no device required —
/// drives the picker against synthetic `VkQueueFamilyProperties` arrays
/// that mirror the queue-family shapes the user might encounter in the
/// wild:
///
///   1. NVIDIA-style discrete GPU — three families (graphics+compute+xfer,
///      compute-only, transfer-only). Every Type request should resolve to
///      a dedicated family.
///   2. Intel-style integrated GPU — one family with graphics+compute+xfer.
///      Universal/Graphics request as dedicated; Compute and Transfer
///      requests fall back to the graphics family (isDedicated = false on
///      the picker's `dedicated` flag).
///   3. Hypothetical compute-only device (no graphics family). Graphics
///      and Universal must return nullopt; Compute must succeed as
///      dedicated; Transfer must fall back to the compute family.
///   4. Edge case — empty family list. Every request returns nullopt.
///   5. `requireDedicated=true` enforcement — on a graphics-only device,
///      a Compute or Transfer request returns nullopt instead of falling
///      back.
///
/// Vulkan SDK headers are required for `VkQueueFamilyProperties`, so this
/// test lives under `tests/vulkan/`. The picker logic itself is
/// platform-agnostic; this is the deterministic, fast home for the
/// "exhaustive fallback ladder" coverage. The companion CommandQueueDesc
/// integration test runs through the real backend in
/// `tests/vulkan/CommandQueueDescTest/`.

#include "../../../src/vulkan/VulkanQueueFamilies.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace OmegaGTE;
using Type = GECommandQueueDesc::Type;

namespace {

VkQueueFamilyProperties makeFamily(VkQueueFlags flags, std::uint32_t count = 1) {
    VkQueueFamilyProperties p{};
    p.queueFlags = flags;
    p.queueCount = count;
    p.timestampValidBits = 64;
    p.minImageTransferGranularity = {1, 1, 1};
    return p;
}

bool expectMiss(Type type, const std::vector<VkQueueFamilyProperties> & props,
                bool requireDedicated, const char * label) {
    const auto choice = VulkanQueueFamilies::Pick(
        props.data(), static_cast<std::uint32_t>(props.size()), type, requireDedicated);
    if (choice.has_value()) {
        std::printf("FAIL[%s]: expected no match, got family=%u achieved=%d dedicated=%d\n",
                    label, choice->familyIndex, static_cast<int>(choice->achievedType),
                    choice->dedicated);
        return false;
    }
    return true;
}

bool expectHit(Type type, const std::vector<VkQueueFamilyProperties> & props,
               bool requireDedicated, std::uint32_t wantFamily,
               Type wantAchieved, bool wantDedicated, const char * label) {
    const auto choice = VulkanQueueFamilies::Pick(
        props.data(), static_cast<std::uint32_t>(props.size()), type, requireDedicated);
    if (!choice.has_value()) {
        std::printf("FAIL[%s]: expected family=%u, got no match\n", label, wantFamily);
        return false;
    }
    bool ok = true;
    if (choice->familyIndex != wantFamily) {
        std::printf("FAIL[%s]: familyIndex = %u, want %u\n", label, choice->familyIndex, wantFamily);
        ok = false;
    }
    if (choice->achievedType != wantAchieved) {
        std::printf("FAIL[%s]: achievedType = %d, want %d\n", label,
                    static_cast<int>(choice->achievedType), static_cast<int>(wantAchieved));
        ok = false;
    }
    if (choice->dedicated != wantDedicated) {
        std::printf("FAIL[%s]: dedicated = %d, want %d\n", label,
                    choice->dedicated, wantDedicated);
        ok = false;
    }
    return ok;
}

bool caseDiscreteGpu() {
    // Family layout typical of a modern discrete NVIDIA/AMD card:
    //   0: graphics + compute + transfer (the universal queue)
    //   1: compute + transfer (async-compute)
    //   2: transfer-only (DMA / PCIe copy engine)
    std::vector<VkQueueFamilyProperties> props = {
        makeFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 16),
        makeFamily(VK_QUEUE_COMPUTE_BIT  | VK_QUEUE_TRANSFER_BIT, 8),
        makeFamily(VK_QUEUE_TRANSFER_BIT, 2),
    };
    bool ok = true;
    ok &= expectHit(Type::Universal, props, false, /*family=*/0, Type::Universal, true,
                    "discrete/universal");
    ok &= expectHit(Type::Graphics,  props, false, /*family=*/0, Type::Graphics,  true,
                    "discrete/graphics");
    ok &= expectHit(Type::Compute,   props, false, /*family=*/1, Type::Compute,   true,
                    "discrete/compute-dedicated");
    ok &= expectHit(Type::Transfer,  props, false, /*family=*/2, Type::Transfer,  true,
                    "discrete/transfer-dedicated");
    return ok;
}

bool caseIntegratedGpu() {
    // Family layout typical of Intel/most mobile parts:
    //   0: graphics + compute + transfer (everything in one family)
    std::vector<VkQueueFamilyProperties> props = {
        makeFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1),
    };
    bool ok = true;
    ok &= expectHit(Type::Universal, props, false, 0, Type::Universal, true,
                    "integrated/universal");
    ok &= expectHit(Type::Graphics,  props, false, 0, Type::Graphics,  true,
                    "integrated/graphics");
    // Compute / Transfer must fall back to the graphics family because no
    // dedicated family exists; achieved reports as Graphics, dedicated=false.
    ok &= expectHit(Type::Compute,   props, false, 0, Type::Graphics,  false,
                    "integrated/compute-fallback");
    ok &= expectHit(Type::Transfer,  props, false, 0, Type::Graphics,  false,
                    "integrated/transfer-fallback");
    return ok;
}

bool caseComputeOnlyDevice() {
    // Hypothetical no-graphics device. Universal / Graphics must fail;
    // Compute hits family 0 as dedicated; Transfer falls back to compute.
    std::vector<VkQueueFamilyProperties> props = {
        makeFamily(VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 4),
    };
    bool ok = true;
    ok &= expectMiss(Type::Universal, props, false, "compute-only/universal");
    ok &= expectMiss(Type::Graphics,  props, false, "compute-only/graphics");
    ok &= expectHit(Type::Compute,    props, false, 0, Type::Compute, true,
                    "compute-only/compute");
    ok &= expectHit(Type::Transfer,   props, false, 0, Type::Compute, false,
                    "compute-only/transfer-fallback");
    return ok;
}

bool caseEmpty() {
    std::vector<VkQueueFamilyProperties> props;
    bool ok = true;
    ok &= expectMiss(Type::Universal, props, false, "empty/universal");
    ok &= expectMiss(Type::Graphics,  props, false, "empty/graphics");
    ok &= expectMiss(Type::Compute,   props, false, "empty/compute");
    ok &= expectMiss(Type::Transfer,  props, false, "empty/transfer");
    return ok;
}

bool caseRequireDedicated() {
    // Graphics-only device with no dedicated compute/transfer family.
    // requireDedicated=true must turn the otherwise-allowed fallback into a
    // miss; requireDedicated=false reproduces the integrated-GPU fallback.
    std::vector<VkQueueFamilyProperties> props = {
        makeFamily(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1),
    };
    bool ok = true;
    ok &= expectMiss(Type::Compute,  props, /*requireDedicated=*/true, "dedicated/compute");
    ok &= expectMiss(Type::Transfer, props, /*requireDedicated=*/true, "dedicated/transfer");
    // Universal and Graphics are never "fallback" so requireDedicated has no
    // effect on them — they still succeed against the graphics family.
    ok &= expectHit(Type::Universal, props, true, 0, Type::Universal, true,
                    "dedicated/universal-ok");
    ok &= expectHit(Type::Graphics,  props, true, 0, Type::Graphics,  true,
                    "dedicated/graphics-ok");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= caseDiscreteGpu();
    ok &= caseIntegratedGpu();
    ok &= caseComputeOnlyDevice();
    ok &= caseEmpty();
    ok &= caseRequireDedicated();
    std::printf("%s: VulkanQueueFamilies::Pick\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
