#include <vulkan/vulkan.h>

#if defined(TARGET_VULKAN)

// `vkSetDebugUtilsObjectNameEXT` is an instance-extension entry point
// (`VK_EXT_debug_utils`). The OmegaGTE call sites invoke it unconditionally
// at resource-creation time — gating each site behind an
// `isDebugLayerEnabled()` check would scatter the same conditional across
// every backend object.
//
// Centralize it here instead: `g_pfnSetDebugUtilsObjectNameEXT` is
// populated by `initVulkan()` only when the debug layer is on AND the
// extension was actually enabled on the instance. When the pointer is
// null (release build, debug layer off, or extension absent) the call
// becomes a `VK_SUCCESS` no-op. See `gte/docs/Debug-Layer-Plan.md` §2.4.
namespace OmegaGTE {
    extern PFN_vkSetDebugUtilsObjectNameEXT g_pfnSetDebugUtilsObjectNameEXT;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkSetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT *pNameInfo) {
    auto fn = OmegaGTE::g_pfnSetDebugUtilsObjectNameEXT;
    if(fn == nullptr){
        return VK_SUCCESS;
    }
    return fn(device, pNameInfo);
}
#endif
