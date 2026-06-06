#include "VulkanQueueFamilies.h"

_NAMESPACE_BEGIN_

namespace {

constexpr VkQueueFlags GfxComputeXferMask =
    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;

/// Find the first family whose flags satisfy `(flags & wantMask) == wantMask`
/// and `(flags & forbidMask) == 0`. Returns `count` on miss so callers can
/// compare against `count` rather than a sentinel index.
std::uint32_t findFamily(const VkQueueFamilyProperties * props,
                         std::uint32_t                   count,
                         VkQueueFlags                    wantMask,
                         VkQueueFlags                    forbidMask) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags f = props[i].queueFlags;
        if ((f & wantMask) == wantMask && (f & forbidMask) == 0) {
            return i;
        }
    }
    return count;
}

} // namespace

std::optional<VulkanFamilyChoice>
VulkanQueueFamilies::Pick(const VkQueueFamilyProperties * props,
                          std::uint32_t                   count,
                          GECommandQueueDesc::Type        requested,
                          bool                            requireDedicated) {
    if (count == 0 || props == nullptr) {
        return std::nullopt;
    }

    using Type = GECommandQueueDesc::Type;

    switch (requested) {
        case Type::Universal: {
            // Universal = a single family that can do everything. If no such
            // family exists, fail rather than silently dropping a capability
            // — the caller asked for the strongest guarantee, and quietly
            // weakening it to "graphics only" would let compute / transfer
            // submissions on this queue blow up later.
            const std::uint32_t idx =
                findFamily(props, count, GfxComputeXferMask, /*forbidMask=*/0);
            if (idx == count) {
                return std::nullopt;
            }
            return VulkanFamilyChoice{idx, Type::Universal, /*dedicated=*/true};
        }

        case Type::Graphics: {
            // Prefer a graphics family that also has compute (typical on every
            // modern desktop GPU) so a single graphics queue can still issue
            // compute work without an extra family hop. Fall back to
            // graphics-only if such a thing exists somehow.
            std::uint32_t idx = findFamily(props, count,
                                           VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT,
                                           /*forbidMask=*/0);
            if (idx == count) {
                idx = findFamily(props, count, VK_QUEUE_GRAPHICS_BIT, /*forbidMask=*/0);
            }
            if (idx == count) {
                return std::nullopt;
            }
            return VulkanFamilyChoice{idx, Type::Graphics, /*dedicated=*/true};
        }

        case Type::Compute: {
            // Async-compute: prefer a family with COMPUTE and NOT GRAPHICS.
            // That's the family that runs in parallel with the graphics queue
            // on hardware that exposes it (NVIDIA Pascal+, AMD GCN, Intel
            // Xe-HPG). Fall back to any compute-capable family.
            const std::uint32_t dedicated =
                findFamily(props, count, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
            if (dedicated != count) {
                return VulkanFamilyChoice{dedicated, Type::Compute, /*dedicated=*/true};
            }
            if (requireDedicated) {
                return std::nullopt;
            }
            const std::uint32_t any =
                findFamily(props, count, VK_QUEUE_COMPUTE_BIT, /*forbidMask=*/0);
            if (any == count) {
                return std::nullopt;
            }
            // Fell back to the graphics+compute family — report achieved as
            // Graphics so isDedicated() observes the downgrade.
            return VulkanFamilyChoice{any, Type::Graphics, /*dedicated=*/false};
        }

        case Type::Transfer: {
            // DMA / dedicated-copy: prefer a family with TRANSFER and neither
            // GRAPHICS nor COMPUTE — that's the discrete-GPU PCIe DMA engine,
            // which runs fully in parallel with graphics and compute and
            // shouldn't stall either. Then fall back to compute (async copy
            // still off the graphics path), then graphics (always
            // implicitly transfer-capable in Vulkan).
            const std::uint32_t dedicated =
                findFamily(props, count, VK_QUEUE_TRANSFER_BIT,
                           VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
            if (dedicated != count) {
                return VulkanFamilyChoice{dedicated, Type::Transfer, /*dedicated=*/true};
            }
            if (requireDedicated) {
                return std::nullopt;
            }
            const std::uint32_t compute =
                findFamily(props, count, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
            if (compute != count) {
                return VulkanFamilyChoice{compute, Type::Compute, /*dedicated=*/false};
            }
            // Any graphics queue implicitly supports transfer — wantMask
            // doesn't need to include TRANSFER_BIT.
            const std::uint32_t graphics =
                findFamily(props, count, VK_QUEUE_GRAPHICS_BIT, /*forbidMask=*/0);
            if (graphics == count) {
                return std::nullopt;
            }
            return VulkanFamilyChoice{graphics, Type::Graphics, /*dedicated=*/false};
        }
    }
    return std::nullopt;
}

_NAMESPACE_END_
