#ifndef OMEGAGTE_VULKAN_QUEUE_FAMILIES_H
#define OMEGAGTE_VULKAN_QUEUE_FAMILIES_H

#include "omegaGTE/GECommandQueue.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>

_NAMESPACE_BEGIN_

    /// Result of resolving a `GECommandQueueDesc::Type` against the physical
    /// device's `VkQueueFamilyProperties` list. Carries the Vulkan family
    /// index the engine should open queues against and the type the queue
    /// will actually advertise as (may differ from the request after
    /// fallback, in which case `dedicated` is false).
    struct VulkanFamilyChoice {
        std::uint32_t                familyIndex;
        GECommandQueueDesc::Type     achievedType;
        bool                         dedicated;
    };

    /// Pure helper: given a physical device's queue-family property table,
    /// pick the right family for each `GECommandQueueDesc::Type`. Encodes the
    /// per-type fallback rules from the CommandQueue-Typed-Pool plan:
    ///
    /// - **Universal** — first family with `GRAPHICS | COMPUTE | TRANSFER`
    ///   bits set. Cannot fall back; if no family has all three, returns
    ///   nullopt.
    /// - **Graphics** — first family with `GRAPHICS`. Prefers a family that
    ///   also has `COMPUTE` (so universal pipelines that compute-prepass
    ///   inside the same queue work without extra fences).
    /// - **Compute** — family with `COMPUTE` and NOT `GRAPHICS` (async
    ///   compute). Falls back to any compute-capable family (including the
    ///   graphics family); `dedicated` reflects whether the fallback fired.
    /// - **Transfer** — family with `TRANSFER` and neither `GRAPHICS` nor
    ///   `COMPUTE` (DMA / copy queue). Falls back first to a compute family,
    ///   then to the graphics family; `dedicated` reflects whether the
    ///   fallback fired.
    ///
    /// The helper is intentionally a free function (not a class) so it can
    /// be unit-tested against synthetic `VkQueueFamilyProperties` arrays
    /// without spinning up a real device.
    class VulkanQueueFamilies {
    public:
        /// Resolve a requested queue type against `props`. Returns nullopt if
        /// no family can satisfy the request and `requireDedicated` is true,
        /// or — for Universal — if no family carries all three capability
        /// bits.
        static std::optional<VulkanFamilyChoice>
        Pick(const VkQueueFamilyProperties * props,
             std::uint32_t                   count,
             GECommandQueueDesc::Type        requested,
             bool                            requireDedicated = false);
    };

_NAMESPACE_END_

#endif
