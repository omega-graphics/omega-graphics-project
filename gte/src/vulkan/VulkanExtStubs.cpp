#include <vulkan/vulkan.h>

#if defined(TARGET_VULKAN)
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkSetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT *pNameInfo) {
    (void)device;
    (void)pNameInfo;
    return VK_SUCCESS;
}
#endif
