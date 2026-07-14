

#include "vulkan/vulkan_core.h"

#include <stdint.h>
#define VMA_IMPLEMENTATION 1

#include "GEVulkan.h"
#include "GEVulkanCommandQueue.h"
#include "GEVulkanTexture.h"
#include "GEVulkanPipeline.h"
#include "GEVulkanRenderTarget.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <deque>
#include <limits>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <string>

#include "omegaGTE/GTEDevice.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEPipeline.h"
#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GTEMath.h"

#include "omega-common/utils.h"

#include <glm/glm.hpp>

#include "../BufferIO.h"


_NAMESPACE_BEGIN_

    /// PixelFormat-Completion-Plan — Vulkan translation table. Vulkan can name
    /// every family in the enum (BC, ASTC and ETC2 all have VK_FORMAT entries);
    /// whether a given *device* implements one is a separate question, answered by
    /// vkGetPhysicalDeviceFormatProperties rather than by this table.
    inline VkFormat pixelFormatToVkFormat(PixelFormat fmt){
        switch(fmt){
            // ── 8-bit color ──
            case PixelFormat::R8Unorm:            return VK_FORMAT_R8_UNORM;
            case PixelFormat::R8Snorm:            return VK_FORMAT_R8_SNORM;
            case PixelFormat::R8Uint:             return VK_FORMAT_R8_UINT;
            case PixelFormat::RG8Unorm:           return VK_FORMAT_R8G8_UNORM;
            case PixelFormat::RG8Snorm:           return VK_FORMAT_R8G8_SNORM;
            case PixelFormat::RGBA8Unorm:         return VK_FORMAT_R8G8B8A8_UNORM;
            case PixelFormat::RGBA8Unorm_SRGB:    return VK_FORMAT_R8G8B8A8_SRGB;
            case PixelFormat::RGBA8Snorm:         return VK_FORMAT_R8G8B8A8_SNORM;
            case PixelFormat::BGRA8Unorm:         return VK_FORMAT_B8G8R8A8_UNORM;
            case PixelFormat::BGRA8Unorm_SRGB:    return VK_FORMAT_B8G8R8A8_SRGB;

            // ── 16-bit color ──
            case PixelFormat::R16Unorm:           return VK_FORMAT_R16_UNORM;
            case PixelFormat::R16Float:           return VK_FORMAT_R16_SFLOAT;
            case PixelFormat::R16Uint:            return VK_FORMAT_R16_UINT;
            case PixelFormat::RG16Unorm:          return VK_FORMAT_R16G16_UNORM;
            case PixelFormat::RG16Float:          return VK_FORMAT_R16G16_SFLOAT;
            case PixelFormat::RGBA16Unorm:        return VK_FORMAT_R16G16B16A16_UNORM;
            case PixelFormat::RGBA16Float:        return VK_FORMAT_R16G16B16A16_SFLOAT;

            // ── 32-bit color ──
            case PixelFormat::R32Float:           return VK_FORMAT_R32_SFLOAT;
            case PixelFormat::R32Uint:            return VK_FORMAT_R32_UINT;
            case PixelFormat::RG32Float:          return VK_FORMAT_R32G32_SFLOAT;
            case PixelFormat::RGBA32Float:        return VK_FORMAT_R32G32B32A32_SFLOAT;

            // ── Packed ──
            /// Vulkan's packed formats are little-endian-component-order, so
            /// RGB10A2 is spelled A2B10G10R10 and R11G11B10 is B10G11R11.
            case PixelFormat::RGB10A2Unorm:       return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case PixelFormat::R11G11B10Float:     return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

            // ── Depth / stencil ──
            case PixelFormat::D16Unorm:           return VK_FORMAT_D16_UNORM;
            case PixelFormat::D32Float:           return VK_FORMAT_D32_SFLOAT;
            case PixelFormat::D24Unorm_S8Uint:    return VK_FORMAT_D24_UNORM_S8_UINT;
            case PixelFormat::D32Float_S8Uint:    return VK_FORMAT_D32_SFLOAT_S8_UINT;

            // ── BC ──
            case PixelFormat::BC1_RGBA_Unorm:     return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case PixelFormat::BC1_RGBA_Unorm_SRGB:return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case PixelFormat::BC3_RGBA_Unorm:     return VK_FORMAT_BC3_UNORM_BLOCK;
            case PixelFormat::BC3_RGBA_Unorm_SRGB:return VK_FORMAT_BC3_SRGB_BLOCK;
            case PixelFormat::BC5_RG_Unorm:       return VK_FORMAT_BC5_UNORM_BLOCK;
            case PixelFormat::BC7_RGBA_Unorm:     return VK_FORMAT_BC7_UNORM_BLOCK;
            case PixelFormat::BC7_RGBA_Unorm_SRGB:return VK_FORMAT_BC7_SRGB_BLOCK;

            // ── ASTC ──
            case PixelFormat::ASTC_4x4_Unorm:      return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
            case PixelFormat::ASTC_4x4_Unorm_SRGB: return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
            case PixelFormat::ASTC_6x6_Unorm:      return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
            case PixelFormat::ASTC_6x6_Unorm_SRGB: return VK_FORMAT_ASTC_6x6_SRGB_BLOCK;
            case PixelFormat::ASTC_8x8_Unorm:      return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
            case PixelFormat::ASTC_8x8_Unorm_SRGB: return VK_FORMAT_ASTC_8x8_SRGB_BLOCK;

            // ── ETC2 / EAC ──
            case PixelFormat::ETC2_RGB8_Unorm:       return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
            case PixelFormat::ETC2_RGB8_Unorm_SRGB:  return VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
            case PixelFormat::ETC2_RGBA8_Unorm:      return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
            case PixelFormat::ETC2_RGBA8_Unorm_SRGB: return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
            case PixelFormat::EAC_R11_Unorm:         return VK_FORMAT_EAC_R11_UNORM_BLOCK;
        }
        return VK_FORMAT_R8G8B8A8_UNORM;
    }

    inline const char *vkResultToStr(VkResult r){
        switch(r){
            case VK_SUCCESS:                          return "VK_SUCCESS";
            case VK_NOT_READY:                        return "VK_NOT_READY";
            case VK_TIMEOUT:                          return "VK_TIMEOUT";
            case VK_EVENT_SET:                        return "VK_EVENT_SET";
            case VK_EVENT_RESET:                      return "VK_EVENT_RESET";
            case VK_INCOMPLETE:                       return "VK_INCOMPLETE";
            case VK_ERROR_OUT_OF_HOST_MEMORY:         return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY:       return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED:      return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:                return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_MEMORY_MAP_FAILED:          return "VK_ERROR_MEMORY_MAP_FAILED";
            case VK_ERROR_LAYER_NOT_PRESENT:          return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT:      return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT:        return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER:        return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_TOO_MANY_OBJECTS:           return "VK_ERROR_TOO_MANY_OBJECTS";
            case VK_ERROR_FORMAT_NOT_SUPPORTED:       return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            case VK_ERROR_FRAGMENTED_POOL:            return "VK_ERROR_FRAGMENTED_POOL";
            case VK_ERROR_OUT_OF_POOL_MEMORY:         return "VK_ERROR_OUT_OF_POOL_MEMORY";
            case VK_ERROR_INVALID_EXTERNAL_HANDLE:    return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
            case VK_ERROR_FRAGMENTATION:              return "VK_ERROR_FRAGMENTATION";
            case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
            case VK_ERROR_SURFACE_LOST_KHR:           return "VK_ERROR_SURFACE_LOST_KHR";
            case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:   return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
            case VK_SUBOPTIMAL_KHR:                   return "VK_SUBOPTIMAL_KHR";
            case VK_ERROR_OUT_OF_DATE_KHR:            return "VK_ERROR_OUT_OF_DATE_KHR";
            case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:   return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
            case VK_ERROR_VALIDATION_FAILED_EXT:      return "VK_ERROR_VALIDATION_FAILED_EXT";
            case VK_ERROR_INVALID_SHADER_NV:          return "VK_ERROR_INVALID_SHADER_NV";
            default:                                  return "VK_ERROR_<unknown>";
        }
    }

    VkInstance GEVulkanEngine::instance = nullptr;

    static VkDebugUtilsMessengerEXT g_debugMessenger = VK_NULL_HANDLE;

    // Cached at `initVulkan()` time. Non-null only when the debug layer is
    // active AND `VK_EXT_debug_utils` was successfully enabled on the
    // instance — see §2.4 of `gte/docs/Debug-Layer-Plan.md`. The
    // dispatcher in `VulkanExtStubs.cpp` reads this and either calls
    // through or returns `VK_SUCCESS` (so naming becomes a no-op when the
    // layer is off, instead of crashing on a null proc address).
    PFN_vkSetDebugUtilsObjectNameEXT g_pfnSetDebugUtilsObjectNameEXT = nullptr;

    static VKAPI_ATTR VkBool32 VKAPI_CALL vulkanDebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *data,
        void *){
        const char *sev = "INFO";
        if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) sev = "ERROR";
        else if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) sev = "WARN";
        else if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) return VK_FALSE;
        // Funnel through `DEBUG_STREAM` so one toggle silences both
        // backend and engine output. The messenger is only created when
        // `isDebugLayerEnabled()` is already true, so the gate inside
        // `DEBUG_STREAM` never fires negatively here — keeping it for
        // consistency with the rest of the engine.
        DEBUG_STREAM("[VVL " << sev << "] " << (data && data->pMessage ? data->pMessage : "(null)"));
        return VK_FALSE;
    }

    bool vulkanInit = false;

    bool initVulkan(){
        if(vulkanInit && GEVulkanEngine::instance != VK_NULL_HANDLE){
            return true;
        }

        GEVulkanEngine::instance = VK_NULL_HANDLE;

        // Snapshot the debug-layer state once. The flag is meant to be
        // frozen for the process lifetime; reading it once here keeps the
        // instance configuration internally consistent even if a future
        // change makes the atomic mutable.
        const bool debugLayerEnabled = isDebugLayerEnabled();
        const bool wantGpuAssisted = debugLayerEnabled && isGpuBasedValidationEnabled();

        uint32_t extensionCount = 0;
        auto extRes = vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,nullptr);
        if(extRes != VK_SUCCESS){
            std::cerr << "Vulkan init failed: unable to enumerate instance extensions (" << extRes << ")" << std::endl;
            return false;
        }

        OmegaCommon::Vector<VkExtensionProperties> availableExtensions;
        availableExtensions.resize(extensionCount);
        extRes = vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,availableExtensions.data());
        if(extRes != VK_SUCCESS){
            std::cerr << "Vulkan init failed: unable to read instance extensions (" << extRes << ")" << std::endl;
            return false;
        }

        std::unordered_set<std::string> extensionSet;
        for(auto &ext : availableExtensions){
            extensionSet.emplace(ext.extensionName);
        }

        OmegaCommon::Vector<const char *> requiredInstanceExtensions;
        requiredInstanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
        requiredInstanceExtensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
        requiredInstanceExtensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
        requiredInstanceExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif

        // `VK_EXT_debug_utils` is only pushed when the debug layer is on.
        // Querying availability unconditionally lets us log the fact that
        // the runtime supports it but we declined to use it — useful when
        // diagnosing why no `[VVL …]` lines appear in a release build.
        const bool hasDebugUtils = extensionSet.find(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != extensionSet.end();
        const bool enableDebugUtils = debugLayerEnabled && hasDebugUtils;
        if(enableDebugUtils){
            requiredInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        OmegaCommon::Vector<const char *> enabledLayers;
        bool validationLayerEnabled = false;
        bool validationLayerHasValidationFeatures = false;
        if(debugLayerEnabled){
            uint32_t layerCount = 0;
            vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
            OmegaCommon::Vector<VkLayerProperties> layerProps;
            layerProps.resize(layerCount);
            if(layerCount > 0){
                vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());
            }
            for(auto & lp : layerProps){
                if(std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0){
                    enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
                    validationLayerEnabled = true;
                    break;
                }
            }

            // Only meaningful when GBV was requested — the cost of
            // enumerating the layer's extensions is negligible but skip
            // it if there's nothing to gate.
            if(validationLayerEnabled && wantGpuAssisted){
                uint32_t layerExtCount = 0;
                if(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                                          &layerExtCount, nullptr) == VK_SUCCESS && layerExtCount > 0){
                    OmegaCommon::Vector<VkExtensionProperties> layerExts;
                    layerExts.resize(layerExtCount);
                    if(vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                                              &layerExtCount, layerExts.data()) == VK_SUCCESS){
                        for(auto &le : layerExts){
                            if(std::strcmp(le.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0){
                                validationLayerHasValidationFeatures = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        for(const char *requiredExt : requiredInstanceExtensions){
            if(extensionSet.find(requiredExt) == extensionSet.end()){
                std::cerr << "Vulkan init failed: missing required instance extension `" << requiredExt << "`" << std::endl;
                return false;
            }
        }

        uint32_t apiVersion = VK_API_VERSION_1_0;
        if(vkEnumerateInstanceVersion != nullptr){
            auto versionRes = vkEnumerateInstanceVersion(&apiVersion);
            if(versionRes != VK_SUCCESS){
                apiVersion = VK_API_VERSION_1_0;
            }
        }

        VkApplicationInfo appInfo {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.apiVersion = apiVersion;
        appInfo.applicationVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.engineVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.pNext = nullptr;
        appInfo.pApplicationName = "OmegaGTE";
        appInfo.pEngineName = "OmegaGTE";

        VkInstanceCreateInfo instanceInfo {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pNext = nullptr;
        instanceInfo.flags = 0;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
        instanceInfo.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(requiredInstanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = requiredInstanceExtensions.data();

        // §2.3 GPU-assisted validation. Storage outside the if-block so
        // the address chained into `pNext` stays valid until
        // `vkCreateInstance` returns.
        VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
            VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        };
        VkValidationFeaturesEXT validationFeaturesInfo {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        if(wantGpuAssisted){
            if(validationLayerHasValidationFeatures){
                validationFeaturesInfo.pNext = nullptr;
                validationFeaturesInfo.enabledValidationFeatureCount =
                    static_cast<uint32_t>(sizeof(enabledValidationFeatures) / sizeof(enabledValidationFeatures[0]));
                validationFeaturesInfo.pEnabledValidationFeatures = enabledValidationFeatures;
                validationFeaturesInfo.disabledValidationFeatureCount = 0;
                validationFeaturesInfo.pDisabledValidationFeatures = nullptr;
                instanceInfo.pNext = &validationFeaturesInfo;
            } else {
                std::cerr << "[GEVulkanEngine_Internal] GPU-assisted validation requested but "
                             "`VK_EXT_validation_features` is not advertised by `VK_LAYER_KHRONOS_validation`; "
                             "downgrading to plain validation." << std::endl;
            }
        }

        auto createRes = vkCreateInstance(&instanceInfo,nullptr,&GEVulkanEngine::instance);
        if(createRes != VK_SUCCESS || GEVulkanEngine::instance == VK_NULL_HANDLE){
            std::cerr << "Vulkan init failed: vkCreateInstance returned " << createRes << std::endl;
            GEVulkanEngine::instance = VK_NULL_HANDLE;
            return false;
        }

        if(enableDebugUtils){
            auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                GEVulkanEngine::instance, "vkCreateDebugUtilsMessengerEXT");
            if(pfnCreate){
                VkDebugUtilsMessengerCreateInfoEXT dbgInfo {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
                dbgInfo.messageSeverity =
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
                dbgInfo.messageType =
                    VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                dbgInfo.pfnUserCallback = vulkanDebugCallback;
                pfnCreate(GEVulkanEngine::instance, &dbgInfo, nullptr, &g_debugMessenger);
            }

            // §2.4 — cache the device-level object-naming function pointer
            // so the dispatcher in `VulkanExtStubs.cpp` can call through
            // (debug layer on) or no-op (debug layer off / extension not
            // enabled). Loaded via the instance because the extension is
            // an instance-promoted utils extension; valid for any device
            // created from this instance.
            g_pfnSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(
                GEVulkanEngine::instance, "vkSetDebugUtilsObjectNameEXT");
        } else {
            g_pfnSetDebugUtilsObjectNameEXT = nullptr;
        }

        vulkanInit = true;
        return true;
    }



    void cleanupVulkan(){
        if(GEVulkanEngine::instance != VK_NULL_HANDLE){
            if(g_debugMessenger != VK_NULL_HANDLE){
                auto pfnDestroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                    GEVulkanEngine::instance, "vkDestroyDebugUtilsMessengerEXT");
                if(pfnDestroy){
                    pfnDestroy(GEVulkanEngine::instance, g_debugMessenger, nullptr);
                }
                g_debugMessenger = VK_NULL_HANDLE;
            }
            vkDestroyInstance(GEVulkanEngine::instance,nullptr);
            GEVulkanEngine::instance = VK_NULL_HANDLE;
        }
        vulkanInit = false;
    }

    struct GTEVulkanDevice : public GTEDevice {
        VkPhysicalDevice device;
        bool hasMemoryBudgetExt = false;
        GTEVulkanDevice(GTEDevice::Type type,const char *name,GTEDeviceFeatures & features,VkPhysicalDevice &device,bool hasMemoryBudgetExt_)
            : GTEDevice(type,name,features),device(device),hasMemoryBudgetExt(hasMemoryBudgetExt_) {

        };
        const void * native() override{
            return (void *)device;
        }
        GTEDeviceMemoryBudget queryMemoryBudget() override {
            GTEDeviceMemoryBudget out;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            VkPhysicalDeviceMemoryProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            if(hasMemoryBudgetExt){
                props2.pNext = &budget;
            }
            if(vkGetPhysicalDeviceMemoryProperties2 != nullptr){
                vkGetPhysicalDeviceMemoryProperties2(device, &props2);
            } else {
                vkGetPhysicalDeviceMemoryProperties(device, &props2.memoryProperties);
            }
            const auto &mp = props2.memoryProperties;
            uint64_t dedicated = 0;
            bool sawDeviceLocal = false;
            bool allDeviceLocalHostVisible = true;
            for(uint32_t i = 0; i < mp.memoryHeapCount; ++i){
                if(!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
                sawDeviceLocal = true;
                bool heapHostVisible = false;
                for(uint32_t j = 0; j < mp.memoryTypeCount; ++j){
                    if(mp.memoryTypes[j].heapIndex == i
                       && (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){
                        heapHostVisible = true;
                        break;
                    }
                }
                if(!heapHostVisible){
                    allDeviceLocalHostVisible = false;
                    dedicated += mp.memoryHeaps[i].size;
                }
            }
            out.unifiedMemory = sawDeviceLocal && allDeviceLocalHostVisible;
            out.dedicatedVideoMemory = out.unifiedMemory ? 0 : dedicated;
            if(hasMemoryBudgetExt){
                uint64_t avail = 0;
                for(uint32_t i = 0; i < mp.memoryHeapCount; ++i){
                    if(!(mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
                    uint64_t b = budget.heapBudget[i];
                    uint64_t u = budget.heapUsage[i];
                    avail += (b > u) ? (b - u) : 0;
                }
                out.availableVideoMemory = avail;
            }
            return out;
        }
        ~GTEVulkanDevice() override = default;
    };

    static GTEDeviceFeatures queryVulkanFeatures(VkPhysicalDevice dev,
                                                 const VkPhysicalDeviceProperties &props,
                                                 bool *outHasMemoryBudgetExt){
        GTEDeviceFeatures features{};

        std::unordered_set<std::string> extSet;
        std::uint32_t devExtCount = 0;
        if(vkEnumerateDeviceExtensionProperties(dev, nullptr, &devExtCount, nullptr) == VK_SUCCESS && devExtCount > 0){
            OmegaCommon::Vector<VkExtensionProperties> devExts(devExtCount);
            if(vkEnumerateDeviceExtensionProperties(dev, nullptr, &devExtCount, devExts.data()) == VK_SUCCESS){
                for(auto &e : devExts){
                    extSet.emplace(e.extensionName);
                }
            }
        }
        auto hasExt = [&](const char *name){ return extSet.find(name) != extSet.end(); };

        bool hasAS            = hasExt(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        bool hasRTP           = hasExt(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        bool hasConservative  = hasExt(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        bool hasMemBudget     = hasExt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if(outHasMemoryBudgetExt) *outHasMemoryBudgetExt = hasMemBudget;
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        bool hasMesh          = hasExt(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    #else
        bool hasMesh          = false;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
        bool hasVRS           = hasExt(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    #else
        bool hasVRS           = false;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME
        bool hasBary          = hasExt(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    #else
        bool hasBary          = false;
    #endif
        bool hasDescIdxExt    = hasExt(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

        bool has11 = props.apiVersion >= VK_API_VERSION_1_1;
    #ifdef VK_VERSION_1_2
        bool has12 = props.apiVersion >= VK_API_VERSION_1_2;
    #else
        bool has12 = false;
    #endif

        VkPhysicalDeviceFeatures baseFeats{};

        VkPhysicalDeviceDescriptorIndexingFeatures descIdxFeats{};
        descIdxFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeats{};
        meshFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrsFeats{};
        vrsFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeats{};
        baryFeats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR;
    #endif
    #ifdef VK_VERSION_1_2
        VkPhysicalDeviceVulkan12Features v12Feats{};
        v12Feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    #endif

        if(has11 && vkGetPhysicalDeviceFeatures2 != nullptr){
            VkPhysicalDeviceFeatures2 feats2{};
            feats2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            feats2.pNext = nullptr;
            auto chain = [&](void *s, void **pNextField){
                *pNextField = feats2.pNext;
                feats2.pNext = s;
            };
    #ifdef VK_VERSION_1_2
            if(has12) chain(&v12Feats, (void**)&v12Feats.pNext);
    #endif
            if(hasDescIdxExt || has12) chain(&descIdxFeats, (void**)&descIdxFeats.pNext);
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
            if(hasMesh) chain(&meshFeats, (void**)&meshFeats.pNext);
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
            if(hasVRS) chain(&vrsFeats, (void**)&vrsFeats.pNext);
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME
            if(hasBary) chain(&baryFeats, (void**)&baryFeats.pNext);
    #endif
            vkGetPhysicalDeviceFeatures2(dev, &feats2);
            baseFeats = feats2.features;
        } else {
            vkGetPhysicalDeviceFeatures(dev, &baseFeats);
        }

        // Capability bits — extension-gated features
        if(hasAS && hasRTP)        features.flags |= GTEDEVICE_FEATURE_RAYTRACING;
        if(hasConservative)        features.flags |= GTEDEVICE_FEATURE_CONSERVATIVE_RASTERIZATION;
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(hasMesh && meshFeats.meshShader)
            features.flags |= GTEDEVICE_FEATURE_MESH_SHADER;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
        if(hasVRS && vrsFeats.pipelineFragmentShadingRate)
            features.flags |= GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING;
    #endif
    #ifdef VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME
        if(hasBary && baryFeats.fragmentShaderBarycentric)
            features.flags |= GTEDEVICE_FEATURE_SHADER_BARYCENTRIC;
    #endif
        if(descIdxFeats.runtimeDescriptorArray
           && descIdxFeats.shaderSampledImageArrayNonUniformIndexing)
            features.flags |= GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING;

        // Capability bits — core VkPhysicalDeviceFeatures
        if(baseFeats.independentBlend)           features.flags |= GTEDEVICE_FEATURE_INDEPENDENT_BLEND;
        if(baseFeats.dualSrcBlend)               features.flags |= GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING;
        if(baseFeats.depthClamp)                 features.flags |= GTEDEVICE_FEATURE_DEPTH_CLAMP;
        if(baseFeats.depthBiasClamp)             features.flags |= GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP;
        if(baseFeats.fillModeNonSolid)           features.flags |= GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID;
        if(baseFeats.wideLines)                  features.flags |= GTEDEVICE_FEATURE_WIDE_LINES;
        if(baseFeats.samplerAnisotropy)          features.flags |= GTEDEVICE_FEATURE_SAMPLER_ANISOTROPY;
        if(baseFeats.multiDrawIndirect)          features.flags |= GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT;
        if(baseFeats.drawIndirectFirstInstance)  features.flags |= GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE;
        if(baseFeats.geometryShader)             features.flags |= GTEDEVICE_FEATURE_GEOMETRY_SHADER;
        if(baseFeats.tessellationShader)         features.flags |= GTEDEVICE_FEATURE_TESSELLATION_SHADER;
        if(baseFeats.shaderInt16)                features.flags |= GTEDEVICE_FEATURE_SHADER_INT16;
        if(baseFeats.shaderFloat64)              features.flags |= GTEDEVICE_FEATURE_SHADER_FLOAT64;
        if(baseFeats.shaderInt64)                features.flags |= GTEDEVICE_FEATURE_SHADER_INT64;
        if(baseFeats.textureCompressionBC)       features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_BC;
        if(baseFeats.textureCompressionETC2)     features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ETC2;
        if(baseFeats.textureCompressionASTC_LDR) features.flags |= GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ASTC;

    #ifdef VK_VERSION_1_2
        if(has12 && v12Feats.shaderFloat16)      features.flags |= GTEDEVICE_FEATURE_SHADER_FLOAT16;
    #endif
        if(props.limits.timestampComputeAndGraphics)
            features.flags |= GTEDEVICE_FEATURE_TIMESTAMP_QUERIES;

        // MSAA — highest bit set in framebufferColorSampleCounts.
        VkSampleCountFlags colorCounts = props.limits.framebufferColorSampleCounts;
        if(colorCounts & VK_SAMPLE_COUNT_64_BIT)      features.maxMSAASamples = 64;
        else if(colorCounts & VK_SAMPLE_COUNT_32_BIT) features.maxMSAASamples = 32;
        else if(colorCounts & VK_SAMPLE_COUNT_16_BIT) features.maxMSAASamples = 16;
        else if(colorCounts & VK_SAMPLE_COUNT_8_BIT)  features.maxMSAASamples = 8;
        else if(colorCounts & VK_SAMPLE_COUNT_4_BIT)  features.maxMSAASamples = 4;
        else if(colorCounts & VK_SAMPLE_COUNT_2_BIT)  features.maxMSAASamples = 2;
        else                                          features.maxMSAASamples = 1;

        // Numeric limits.
        features.maxTextureDimension2D         = props.limits.maxImageDimension2D;
        features.maxTextureDimension3D         = props.limits.maxImageDimension3D;
        features.maxTextureDimensionCube       = props.limits.maxImageDimensionCube;
        features.maxBufferSize                 = props.limits.maxStorageBufferRange;
        features.maxComputeWorkGroupSizeX      = props.limits.maxComputeWorkGroupSize[0];
        features.maxComputeWorkGroupSizeY      = props.limits.maxComputeWorkGroupSize[1];
        features.maxComputeWorkGroupSizeZ      = props.limits.maxComputeWorkGroupSize[2];
        features.maxComputeWorkGroupInvocations= props.limits.maxComputeWorkGroupInvocations;
        features.maxComputeSharedMemorySize    = props.limits.maxComputeSharedMemorySize;
        features.maxSamplerAnisotropy          = (uint32_t)props.limits.maxSamplerAnisotropy;
        features.timestampPeriod               = props.limits.timestampPeriod;

        // Shader model — coarse mapping from Vulkan API version + features.
        auto sm = GTEDeviceFeatures::ShaderModel::SM_5_0;
        if(has11) sm = GTEDeviceFeatures::ShaderModel::SM_6_0;
    #ifdef VK_VERSION_1_2
        if(has12 && v12Feats.shaderFloat16) sm = GTEDeviceFeatures::ShaderModel::SM_6_4;
    #endif
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(hasMesh && meshFeats.meshShader) sm = GTEDeviceFeatures::ShaderModel::SM_6_5;
    #endif
        features.shaderModel = sm;

        return features;
    }

    OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
        OmegaCommon::Vector<SharedHandle<GTEDevice>> devs;
        if(!vulkanInit && !initVulkan()){
            return devs;
        }
        if(GEVulkanEngine::instance == VK_NULL_HANDLE){
            std::cerr << "Vulkan enumerateDevices: invalid VkInstance." << std::endl;
            return devs;
        }
        OmegaCommon::Vector<VkPhysicalDevice> vk_devs;
        std::uint32_t device_count = 0;
        auto enumRes = vkEnumeratePhysicalDevices(GEVulkanEngine::instance,&device_count,nullptr);
        if(enumRes != VK_SUCCESS){
            std::cerr << "Vulkan enumerateDevices: vkEnumeratePhysicalDevices(count) failed (" << enumRes << ")" << std::endl;
            return devs;
        }
        if(device_count == 0){
            std::cerr << "Vulkan enumerateDevices: no physical devices found." << std::endl;
            return devs;
        }
        vk_devs.resize(device_count);
        enumRes = vkEnumeratePhysicalDevices(GEVulkanEngine::instance,&device_count,vk_devs.data());
        if(enumRes != VK_SUCCESS){
            std::cerr << "Vulkan enumerateDevices: vkEnumeratePhysicalDevices(list) failed (" << enumRes << ")" << std::endl;
            return devs;
        }
        for(auto dev : vk_devs){
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev,&props);

            bool hasMemBudgetExt = false;
            GTEDeviceFeatures features = queryVulkanFeatures(dev, props, &hasMemBudgetExt);

            GTEDevice::Type type = GTEDevice::Discrete;
            if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
                type = GTEDevice::Discrete;
            }
            else if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){
                type = GTEDevice::Integrated;
            }
            devs.emplace_back(SharedHandle<GTEDevice>(new GTEVulkanDevice(type,props.deviceName,features,dev,hasMemBudgetExt)));
        }
        return devs;
    }

    typedef unsigned char VulkanByte;

    /// §2.4 — sizes/alignments are parameterized by the layout standard.
    /// Scalars and vectors are identical between std430 and std140; only
    /// matrices diverge (std140 columns are 16-strided), so `std` only
    /// affects the matrix branch.
    static size_t sizeForType(omegasl_data_type type,
                              BufferLayoutStd std = BufferLayoutStd::Std430){
        if(isMatrixDataType(type)){
            auto [cols, rows] = matrixDims(type);
            return matrixSize(cols, rows, std);
        }
        switch(type){
            case OMEGASL_FLOAT:   return sizeof(float);
            case OMEGASL_FLOAT2:  return sizeof(glm::vec2);
            case OMEGASL_FLOAT3:  return sizeof(glm::vec3);
            case OMEGASL_FLOAT4:  return sizeof(glm::vec4);
            case OMEGASL_INT:
            case OMEGASL_UINT:    return sizeof(int);
            case OMEGASL_INT2:
            case OMEGASL_UINT2:   return sizeof(glm::ivec2);
            case OMEGASL_INT3:
            case OMEGASL_UINT3:   return sizeof(glm::ivec3);
            case OMEGASL_INT4:
            case OMEGASL_UINT4:   return sizeof(glm::ivec4);
            default:              return 0;
        }
    }

    static size_t alignmentForType(omegasl_data_type type,
                                   BufferLayoutStd std = BufferLayoutStd::Std430){
        if(isMatrixDataType(type)){
            return matrixAlignment(matrixDims(type).second, std);
        }
        switch(type){
            case OMEGASL_FLOAT: case OMEGASL_INT: case OMEGASL_UINT:
                return 4;
            case OMEGASL_FLOAT2: case OMEGASL_INT2: case OMEGASL_UINT2:
                return 8;
            default:
                return 16;
        }
    }


    class GEVulkanBufferWriter : public GEBufferWriter {
        GEVulkanBuffer *_buffer = nullptr;
        VulkanByte *mem_map = nullptr;
        size_t currentOffset = 0;

        bool inStruct = false;
        /// §2.4 — std140 layout when the bound buffer is a uniform/constant
        /// buffer (GLSL `uniform` block). Derived from the buffer's role.
        BufferLayoutStd layoutStd = BufferLayoutStd::Std430;

        OmegaCommon::Vector<DataBlock> blocks;
    public:
        void setOutputBuffer(SharedHandle<GEBuffer> &buffer) override {
            // G.5.3: unmap any prior binding first so re-pointing the
            // writer (the persistent-buffer reuse pattern) does not leak
            // a VMA mapping.
            clearOutputBuffer();
            _buffer = (GEVulkanBuffer *)buffer.get();
            layoutStd = (_buffer->role == BufferDescriptor::Uniform)
                            ? BufferLayoutStd::Std140 : BufferLayoutStd::Std430;
            vmaMapMemory(_buffer->engine->memAllocator,_buffer->alloc,(void **)&mem_map);
            currentOffset = 0;
        }
        void structBegin() override {
            if(!blocks.empty()){
                blocks.clear();
            }

            inStruct = true;
        }
        void structEnd() override {
            inStruct = false;
        }
        void writeFloat(float &v) override {
            blocks.push_back(DataBlock {OMEGASL_FLOAT,new float(v)});
        }
        void writeFloat2(FVec<2> &v) override {
            glm::vec2 vec {v[0][0],v[1][0]};
            blocks.push_back(DataBlock {OMEGASL_FLOAT2,new glm::vec2(vec)});
        }
        void writeFloat3(FVec<3> &v) override {
            glm::vec3 vec {v[0][0],v[1][0],v[2][0]};
            blocks.push_back(DataBlock {OMEGASL_FLOAT3,new glm::vec3(vec)});
        }
        void writeFloat4(FVec<4> &v) override {
            glm::vec4 vec {v[0][0],v[1][0],v[2][0],v[3][0]};
            blocks.push_back(DataBlock {OMEGASL_FLOAT4,new glm::vec4(vec)});
        }
        void writeInt(int &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT,new int(v)});
        }
        void writeInt2(IVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT2,new glm::ivec2(v[0][0],v[1][0])});
        }
        void writeInt3(IVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT3,new glm::ivec3(v[0][0],v[1][0],v[2][0])});
        }
        void writeInt4(IVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_INT4,new glm::ivec4(v[0][0],v[1][0],v[2][0],v[3][0])});
        }
        void writeUint(unsigned &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT,new unsigned(v)});
        }
        void writeUint2(UVec<2> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT2,new glm::uvec2(v[0][0],v[1][0])});
        }
        void writeUint3(UVec<3> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT3,new glm::uvec3(v[0][0],v[1][0],v[2][0])});
        }
        void writeUint4(UVec<4> &v) override {
            blocks.push_back(DataBlock {OMEGASL_UINT4,new glm::uvec4(v[0][0],v[1][0],v[2][0],v[3][0])});
        }

        /// Matrix writers: pack the host matrix for the active standard (Cx3
        /// columns padded to 16 under std430; every column 16-strided under
        /// std140) and stash the byte block. `sendToBuffer` recovers the size
        /// via `sizeForType(type, layoutStd)`.
        void writeFloat2x2(FMatrix<2,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,2>(), encodeFMatrix<2,2>(m, layoutStd)});
        }
        void writeFloat3x3(FMatrix<3,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,3>(), encodeFMatrix<3,3>(m, layoutStd)});
        }
        void writeFloat4x4(FMatrix<4,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,4>(), encodeFMatrix<4,4>(m, layoutStd)});
        }
        void writeFloat2x3(FMatrix<2,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,3>(), encodeFMatrix<2,3>(m, layoutStd)});
        }
        void writeFloat2x4(FMatrix<2,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<2,4>(), encodeFMatrix<2,4>(m, layoutStd)});
        }
        void writeFloat3x2(FMatrix<3,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,2>(), encodeFMatrix<3,2>(m, layoutStd)});
        }
        void writeFloat3x4(FMatrix<3,4> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<3,4>(), encodeFMatrix<3,4>(m, layoutStd)});
        }
        void writeFloat4x2(FMatrix<4,2> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,2>(), encodeFMatrix<4,2>(m, layoutStd)});
        }
        void writeFloat4x3(FMatrix<4,3> &m) override {
            blocks.push_back(DataBlock {matrixDataTypeFor<4,3>(), encodeFMatrix<4,3>(m, layoutStd)});
        }

        /// §12.2 follow-up — integer / unsigned matrix writers. Same
        /// column-padding path as the float writers (`encodeMatrix` is
        /// element-type-generic); the byte block is tagged with the int/uint
        /// matrix enum so `sendToBuffer` recovers the right stride.
        void writeInt2x2(IMatrix<2,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,2>(), encodeMatrix<int,2,2>(m, layoutStd)}); }
        void writeInt3x3(IMatrix<3,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,3>(), encodeMatrix<int,3,3>(m, layoutStd)}); }
        void writeInt4x4(IMatrix<4,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,4>(), encodeMatrix<int,4,4>(m, layoutStd)}); }
        void writeInt2x3(IMatrix<2,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,3>(), encodeMatrix<int,2,3>(m, layoutStd)}); }
        void writeInt2x4(IMatrix<2,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<2,4>(), encodeMatrix<int,2,4>(m, layoutStd)}); }
        void writeInt3x2(IMatrix<3,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,2>(), encodeMatrix<int,3,2>(m, layoutStd)}); }
        void writeInt3x4(IMatrix<3,4> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<3,4>(), encodeMatrix<int,3,4>(m, layoutStd)}); }
        void writeInt4x2(IMatrix<4,2> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,2>(), encodeMatrix<int,4,2>(m, layoutStd)}); }
        void writeInt4x3(IMatrix<4,3> &m) override { blocks.push_back(DataBlock {intMatrixDataTypeFor<4,3>(), encodeMatrix<int,4,3>(m, layoutStd)}); }
        void writeUint2x2(UMatrix<2,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,2>(), encodeMatrix<unsigned,2,2>(m, layoutStd)}); }
        void writeUint3x3(UMatrix<3,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,3>(), encodeMatrix<unsigned,3,3>(m, layoutStd)}); }
        void writeUint4x4(UMatrix<4,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,4>(), encodeMatrix<unsigned,4,4>(m, layoutStd)}); }
        void writeUint2x3(UMatrix<2,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,3>(), encodeMatrix<unsigned,2,3>(m, layoutStd)}); }
        void writeUint2x4(UMatrix<2,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<2,4>(), encodeMatrix<unsigned,2,4>(m, layoutStd)}); }
        void writeUint3x2(UMatrix<3,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,2>(), encodeMatrix<unsigned,3,2>(m, layoutStd)}); }
        void writeUint3x4(UMatrix<3,4> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<3,4>(), encodeMatrix<unsigned,3,4>(m, layoutStd)}); }
        void writeUint4x2(UMatrix<4,2> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,2>(), encodeMatrix<unsigned,4,2>(m, layoutStd)}); }
        void writeUint4x3(UMatrix<4,3> &m) override { blocks.push_back(DataBlock {uintMatrixDataTypeFor<4,3>(), encodeMatrix<unsigned,4,3>(m, layoutStd)}); }

        void sendToBuffer() override {
            assert(!inStruct && "Struct record must be finished before sending object to buffer");
            assert(mem_map != nullptr && "Output buffer must be mapped before sending data");

            /// std140 struct base alignment is always >= 16; std430 uses the
            /// largest member alignment.
            size_t structAlign = (layoutStd == BufferLayoutStd::Std140) ? 16 : 1;
            for(auto & b : blocks){
                size_t a = alignmentForType(b.type, layoutStd);
                if(a > structAlign) structAlign = a;
            }

            /// §2.4-1 — align-then-place: pad the cursor up to each member's
            /// base alignment (zero-filling the gap) before the copy, so a
            /// mixed field order like `{float, float4}` (or std140's
            /// 16-strided matrix columns) lands every member on the offset the
            /// shader reads it from, not packed tight.
            for(auto & b : blocks){
                size_t a = alignmentForType(b.type, layoutStd);
                size_t aligned = alignOffset(currentOffset, a);
                if(aligned > currentOffset){
                    memset(mem_map + currentOffset, 0, aligned - currentOffset);
                }
                currentOffset = aligned;
                size_t si = sizeForType(b.type, layoutStd);
                memcpy(mem_map + currentOffset,b.data,si);
                currentOffset += si;
            }

            size_t rem = currentOffset % structAlign;
            if(rem != 0){
                size_t padding = structAlign - rem;
                memset(mem_map + currentOffset, 0, padding);
                currentOffset += padding;
            }
        }

        void flush() override {
            vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            _buffer = nullptr;
        }
        void clearOutputBuffer() override {
            // G.5.3: release a prior VMA mapping so the writer can be
            // re-pointed safely (persistent-buffer reuse). Idempotent —
            // a no-op when nothing is bound, so `setOutputBuffer` can
            // call it unconditionally.
            if(_buffer == nullptr) return;
            if(mem_map != nullptr){
                vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            }
            _buffer = nullptr;
            mem_map = nullptr;
            currentOffset = 0;
        }
        void resetCursor() override {
            // G.5.3: rewind to byte 0 of the still-mapped buffer to
            // overwrite its contents in place — no Unmap, no re-Map.
            currentOffset = 0;
        }
        ~GEVulkanBufferWriter() override {
            // RAII safety net: a writer destroyed without an explicit
            // flush() (or clearOutputBuffer()) must still release its VMA
            // mapping, or the buffer's allocation asserts on destruction
            // (`m_MapCount == 0`). Idempotent — clearOutputBuffer() guards
            // on `_buffer == nullptr`, so this is a no-op when flush() ran.
            clearOutputBuffer();
        }
    };

    SharedHandle<GEBufferWriter> GEBufferWriter::Create() {
        return SharedHandle<GEBufferWriter>(new GEVulkanBufferWriter());
    }

    class GEVulkanBufferReader : public GEBufferReader {
        GEVulkanBuffer *_buffer = nullptr;
        VulkanByte *mem_map = nullptr;
        size_t currentOffset = 0;
        /// §2.4 — matches the writer: std140 for uniform buffers.
        BufferLayoutStd layoutStd = BufferLayoutStd::Std430;

        OmegaCommon::Vector<omegasl_data_type> readTypes;
        /// §2.4-1 — align-then-read: advance the cursor to the field's base
        /// alignment before each read, mirroring the writer's inter-member
        /// padding. Struct start is kept on the struct alignment by `structEnd`,
        /// so aligning the absolute cursor is equivalent to aligning within the
        /// struct.
        inline void alignRead(omegasl_data_type t){
            currentOffset = alignOffset(currentOffset, alignmentForType(t, layoutStd));
        }
    public:
        void setInputBuffer(SharedHandle<GEBuffer> &buffer) override {
            // G.5.3: unmap any prior binding first so re-pointing the
            // reader does not leak a VMA mapping.
            clearInputBuffer();
            _buffer = (GEVulkanBuffer *)buffer.get();
            layoutStd = (_buffer->role == BufferDescriptor::Uniform)
                            ? BufferLayoutStd::Std140 : BufferLayoutStd::Std430;
            vmaMapMemory(_buffer->engine->memAllocator,_buffer->alloc,(void **)&mem_map);
            currentOffset = 0;
        }
        void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) override {

        }
        void structBegin() override {
            readTypes.clear();
        }
        void structEnd() override {
            size_t structAlign = (layoutStd == BufferLayoutStd::Std140) ? 16 : 1;
            for(auto t : readTypes){
                size_t a = alignmentForType(t, layoutStd);
                if(a > structAlign) structAlign = a;
            }
            size_t rem = currentOffset % structAlign;
            if(rem != 0){
                currentOffset += structAlign - rem;
            }
        }
        void getFloat(float &v) override {
            alignRead(OMEGASL_FLOAT);
            memcpy(&v,mem_map + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
            readTypes.push_back(OMEGASL_FLOAT);
        }
        void getFloat2(FVec<2> &v) override {
            alignRead(OMEGASL_FLOAT2);
            glm::vec2 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_FLOAT2);
        }
        void getFloat3(FVec<3> &v) override {
            alignRead(OMEGASL_FLOAT3);
            glm::vec3 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_FLOAT3);
        }
        void getFloat4(FVec<4> &v) override {
            alignRead(OMEGASL_FLOAT4);
            glm::vec4 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            v[3][0] = vec.w;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_FLOAT4);
        }
        /// Integer / unsigned scalar + vector downloads — symmetric with
        /// `writeInt*` / `writeUint*`. int/uint share the byte layout; the
        /// unsigned readers use `glm::uvec*` but otherwise mirror the int path.
        void getInt(int &v) override {
            alignRead(OMEGASL_INT);
            memcpy(&v,mem_map + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
            readTypes.push_back(OMEGASL_INT);
        }
        void getInt2(IVec<2> &v) override {
            alignRead(OMEGASL_INT2);
            glm::ivec2 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_INT2);
        }
        void getInt3(IVec<3> &v) override {
            alignRead(OMEGASL_INT3);
            glm::ivec3 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_INT3);
        }
        void getInt4(IVec<4> &v) override {
            alignRead(OMEGASL_INT4);
            glm::ivec4 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            v[3][0] = vec.w;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_INT4);
        }
        void getUint(unsigned &v) override {
            alignRead(OMEGASL_UINT);
            memcpy(&v,mem_map + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
            readTypes.push_back(OMEGASL_UINT);
        }
        void getUint2(UVec<2> &v) override {
            alignRead(OMEGASL_UINT2);
            glm::uvec2 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_UINT2);
        }
        void getUint3(UVec<3> &v) override {
            alignRead(OMEGASL_UINT3);
            glm::uvec3 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_UINT3);
        }
        void getUint4(UVec<4> &v) override {
            alignRead(OMEGASL_UINT4);
            glm::uvec4 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            v[3][0] = vec.w;
            currentOffset += sizeof(vec);
            readTypes.push_back(OMEGASL_UINT4);
        }

        /// Matrix readers: read `cols * matrixColumnStride(rows, layoutStd)`
        /// bytes from the mapped buffer, dropping per-column padding when
        /// copying back into the host's tightly-packed `FMatrix`.
        template<class T, unsigned C, unsigned R>
        void getMatrixImpl(Matrix<T, C, R> &m, omegasl_data_type tag){
            alignRead(tag);
            decodeMatrix<T, C, R>(mem_map + currentOffset, m, layoutStd);
            currentOffset += matrixSize(C, R, layoutStd);
            readTypes.push_back(tag);
        }
        void getFloat2x2(FMatrix<2,2> &m) override { getMatrixImpl<float,2,2>(m, OMEGASL_FLOAT2x2); }
        void getFloat3x3(FMatrix<3,3> &m) override { getMatrixImpl<float,3,3>(m, OMEGASL_FLOAT3x3); }
        void getFloat4x4(FMatrix<4,4> &m) override { getMatrixImpl<float,4,4>(m, OMEGASL_FLOAT4x4); }
        void getFloat2x3(FMatrix<2,3> &m) override { getMatrixImpl<float,2,3>(m, OMEGASL_FLOAT2x3); }
        void getFloat2x4(FMatrix<2,4> &m) override { getMatrixImpl<float,2,4>(m, OMEGASL_FLOAT2x4); }
        void getFloat3x2(FMatrix<3,2> &m) override { getMatrixImpl<float,3,2>(m, OMEGASL_FLOAT3x2); }
        void getFloat3x4(FMatrix<3,4> &m) override { getMatrixImpl<float,3,4>(m, OMEGASL_FLOAT3x4); }
        void getFloat4x2(FMatrix<4,2> &m) override { getMatrixImpl<float,4,2>(m, OMEGASL_FLOAT4x2); }
        void getFloat4x3(FMatrix<4,3> &m) override { getMatrixImpl<float,4,3>(m, OMEGASL_FLOAT4x3); }
        /// §12.2 follow-up — integer / unsigned matrix readers.
        void getInt2x2(IMatrix<2,2> &m) override { getMatrixImpl<int,2,2>(m, OMEGASL_INT2x2); }
        void getInt3x3(IMatrix<3,3> &m) override { getMatrixImpl<int,3,3>(m, OMEGASL_INT3x3); }
        void getInt4x4(IMatrix<4,4> &m) override { getMatrixImpl<int,4,4>(m, OMEGASL_INT4x4); }
        void getInt2x3(IMatrix<2,3> &m) override { getMatrixImpl<int,2,3>(m, OMEGASL_INT2x3); }
        void getInt2x4(IMatrix<2,4> &m) override { getMatrixImpl<int,2,4>(m, OMEGASL_INT2x4); }
        void getInt3x2(IMatrix<3,2> &m) override { getMatrixImpl<int,3,2>(m, OMEGASL_INT3x2); }
        void getInt3x4(IMatrix<3,4> &m) override { getMatrixImpl<int,3,4>(m, OMEGASL_INT3x4); }
        void getInt4x2(IMatrix<4,2> &m) override { getMatrixImpl<int,4,2>(m, OMEGASL_INT4x2); }
        void getInt4x3(IMatrix<4,3> &m) override { getMatrixImpl<int,4,3>(m, OMEGASL_INT4x3); }
        void getUint2x2(UMatrix<2,2> &m) override { getMatrixImpl<unsigned,2,2>(m, OMEGASL_UINT2x2); }
        void getUint3x3(UMatrix<3,3> &m) override { getMatrixImpl<unsigned,3,3>(m, OMEGASL_UINT3x3); }
        void getUint4x4(UMatrix<4,4> &m) override { getMatrixImpl<unsigned,4,4>(m, OMEGASL_UINT4x4); }
        void getUint2x3(UMatrix<2,3> &m) override { getMatrixImpl<unsigned,2,3>(m, OMEGASL_UINT2x3); }
        void getUint2x4(UMatrix<2,4> &m) override { getMatrixImpl<unsigned,2,4>(m, OMEGASL_UINT2x4); }
        void getUint3x2(UMatrix<3,2> &m) override { getMatrixImpl<unsigned,3,2>(m, OMEGASL_UINT3x2); }
        void getUint3x4(UMatrix<3,4> &m) override { getMatrixImpl<unsigned,3,4>(m, OMEGASL_UINT3x4); }
        void getUint4x2(UMatrix<4,2> &m) override { getMatrixImpl<unsigned,4,2>(m, OMEGASL_UINT4x2); }
        void getUint4x3(UMatrix<4,3> &m) override { getMatrixImpl<unsigned,4,3>(m, OMEGASL_UINT4x3); }

        void reset() override {
            vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            _buffer = nullptr;
        }
        void clearInputBuffer() override {
            // G.5.3: release a prior VMA mapping so the reader can be
            // re-pointed safely. Idempotent (guards on the null binding,
            // unlike `reset()` which assumes a live buffer). `reset()`
            // remains the existing end-of-read teardown.
            if(_buffer == nullptr) return;
            if(mem_map != nullptr){
                vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            }
            _buffer = nullptr;
            mem_map = nullptr;
            currentOffset = 0;
        }
        ~GEVulkanBufferReader() override {
            // RAII safety net: a reader destroyed without an explicit
            // reset() (or clearInputBuffer()) must still release its VMA
            // mapping, or the buffer's allocation asserts on destruction
            // (`m_MapCount == 0`). Idempotent — clearInputBuffer() guards
            // on `_buffer == nullptr`, so this is a no-op when reset() ran.
            clearInputBuffer();
        }
    };

    SharedHandle<GEBufferReader> GEBufferReader::Create() {
        return SharedHandle<GEBufferReader>(new GEVulkanBufferReader());
    }


    SharedHandle<GTEShader> GEVulkanEngine::_loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime){
        if(shaderDesc == nullptr || shaderDesc->data == nullptr || shaderDesc->dataSize == 0){
            std::cerr << "Vulkan shader module creation skipped: invalid shader bytecode payload." << std::endl;
            return nullptr;
        }
        if((shaderDesc->dataSize % sizeof(std::uint32_t)) != 0){
            std::cerr << "Vulkan shader module creation skipped: SPIR-V bytecode size is not aligned to 4 bytes ("
                      << shaderDesc->dataSize << ")." << std::endl;
            return nullptr;
        }

        VkShaderModuleCreateInfo shaderModuleDesc {};

        shaderModuleDesc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleDesc.pNext = nullptr;
        shaderModuleDesc.pCode = (std::uint32_t *)shaderDesc->data;
        shaderModuleDesc.codeSize = shaderDesc->dataSize;
        shaderModuleDesc.flags = 0;

        VkShaderModule module = VK_NULL_HANDLE;
        auto moduleRes = vkCreateShaderModule(device,&shaderModuleDesc,nullptr,&module);
        if(moduleRes != VK_SUCCESS || module == VK_NULL_HANDLE){
            std::cerr << "Vulkan shader module creation failed (" << moduleRes << ") for shader `"
                      << (shaderDesc->name != nullptr ? shaderDesc->name : "<unnamed>") << "`." << std::endl;
            return nullptr;
        }
        auto result = std::shared_ptr<GTEVulkanShader>(new GTEVulkanShader(this,*shaderDesc,module));
        trackResource(result);
        return result;
    }


    GEVulkanEngine::GEVulkanEngine(SharedHandle<GTEVulkanDevice> device){
        if(!vulkanInit && !initVulkan()){
            std::cerr << "Failed to initialize Vulkan instance." << std::endl;
            std::exit(1);
        }
        if(GEVulkanEngine::instance == VK_NULL_HANDLE || device == nullptr){
            std::cerr << "Invalid Vulkan engine construction state." << std::endl;
            std::exit(1);
        }

        _deviceFeatures = device->features.featuresAsBitmask();

        physicalDevice = device->device;

        std::uint32_t count = 0;
        OmegaCommon::Vector<VkExtensionProperties> ext_props;
        auto extRes = vkEnumerateDeviceExtensionProperties(physicalDevice,nullptr,&count,nullptr);
        if(extRes != VK_SUCCESS){
            std::cerr << "Failed to enumerate Vulkan device extensions (" << extRes << ")" << std::endl;
            std::exit(1);
        }
        ext_props.resize(count);
        extRes = vkEnumerateDeviceExtensionProperties(physicalDevice,nullptr,&count,ext_props.data());
        if(extRes != VK_SUCCESS){
            std::cerr << "Failed to read Vulkan device extensions (" << extRes << ")" << std::endl;
            std::exit(1);
        }

        std::unordered_set<std::string> extensionSet;
        for(auto &ext : ext_props){
            extensionSet.emplace(ext.extensionName);
        }

        OmegaCommon::Vector<const char *> extensionNames;
        auto enableDeviceExtension = [&](const char *extName,bool required) -> bool {
            if(extensionSet.find(extName) == extensionSet.end()){
                if(required){
                    std::cerr << "Missing required Vulkan device extension `" << extName << "`" << std::endl;
                }
                return false;
            }
            extensionNames.push_back(extName);
            return true;
        };

        if(!enableDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME,true)){
            std::exit(1);
        }
        hasPushDescriptorExt = enableDeviceExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,false);
        hasSynchronization2Ext = enableDeviceExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,false);
        hasExtendedDynamicState = enableDeviceExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,false);

        hasBufferDeviceAddressExt = enableDeviceExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,false);
        hasDeferredHostOperationsExt = enableDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,false);
        hasAccelerationStructureExt = enableDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,false);
        hasRayTracingPipelineExt = enableDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,false);
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        /// Mesh-Shader-Plan Phase 4a — enable `VK_EXT_mesh_shader` so
        /// `vkCmdDrawMeshTasksEXT` + the mesh-stage pipeline path light
        /// up. The matching `meshShader` feature is chained into the
        /// device-feature `pNext` block below; without both, the driver
        /// rejects mesh-stage pipeline creation. Optional — devices
        /// without the extension still load (mesh PSO build then bails
        /// at the `GTEDEVICE_FEATURE_MESH_SHADER` gate).
        hasMeshShaderExt = enableDeviceExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME,false);
    #endif

        /// CommandQueue-Typed-Pool follow-up — opt into VK_KHR_global_priority
        /// so we can chain VkDeviceQueueGlobalPriorityCreateInfoKHR on
        /// each opened VkDeviceQueueCreateInfo and actually run high /
        /// low / medium-priority work on distinct VkQueues. Optional —
        /// devices without the extension still load, but every queue
        /// then shares a single MEDIUM (default) VkQueue per family and
        /// `desc.priority` becomes introspection-only.
        hasGlobalPriorityExt = enableDeviceExtension(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME, false);

        count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,nullptr);
        queueFamilyProps.resize(count);

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,queueFamilyProps.data());

        OmegaCommon::Vector<VkDeviceQueueCreateInfo> deviceQueues;
        // One global-priority chain per family — Vulkan VUID-02802 forbids
        // multiple VkDeviceQueueCreateInfo entries with the same
        // queueFamilyIndex, so we get ONE outer priority per family even
        // with the extension enabled. Storage is `std::deque` so growth
        // doesn't invalidate the pointers we hand to deviceQueues[i].pNext.
        std::deque<VkDeviceQueueGlobalPriorityCreateInfoKHR> queuePriorityChains;
        // Per-family float priority arrays, also kept stable via deque.
        // Each VkDeviceQueueCreateInfo points its pQueuePriorities at the
        // back of the relevant entry here. Values [1.0, 0.5, 0.0] map to
        // High / Normal / Low at lookup time; the values index into
        // `relativePriorityKinds` 1:1 so we can reconstruct which queue
        // got which logical priority after vkCreateDevice.
        std::deque<std::vector<float>> queueFloatPriorities;
        // Per-family table of the logical priorities we asked for at each
        // queueIndex. Populated in lockstep with queueFloatPriorities. After
        // vkCreateDevice succeeds, these get copied into
        // `openedQueuePriorities` so `lookupQueueOnFamily` can map
        // (family, KHR-priority) -> VkQueue.
        OmegaCommon::Vector<OmegaCommon::Vector<VkQueueGlobalPriorityKHR>> perFamilyKinds;

        // CommandQueue-Typed-Pool Phase 2 — open every family that exposes
        // any of the three capability bits we care about (GRAPHICS, COMPUTE,
        // TRANSFER) so the queue-family picker has a dedicated transfer
        // family available on hardware that exposes one. Skipping a family
        // here means `VulkanQueueFamilies::Pick` cannot return its index
        // and Transfer requests silently downgrade to the graphics family.
        //
        // The previous version of this loop only opened graphics/compute
        // families AND had an `else { continue; } ++id;` shape that skipped
        // the family-index increment on every non-matching family, which
        // meant any opened family after a skipped one got the wrong index.
        // The rewrite uses an indexed loop so the index is unconditional.
        //
        // CommandQueue-Typed-Pool follow-up — for each opened family, request
        // up to 3 VkQueues with relative float priorities [1.0, 0.5, 0.0]
        // so HIGH / NORMAL / LOW requests land on distinct queues that the
        // family-scheduler can rank. Falls back to fewer queues when the
        // family's queueCount can't support 3, and to a single MEDIUM queue
        // (queueCount=1) on families that only expose one queue (most
        // dedicated transfer families on integrated GPUs). The VK_KHR_global
        // priority chain — when the extension is enabled — additionally
        // tags the WHOLE family with a single outer priority (MEDIUM here,
        // since per-queue global priority isn't a thing in standard Vulkan
        // and chaining HIGH risks VK_ERROR_NOT_PERMITTED_KHR on hosts
        // without CAP_SYS_NICE / the GPU-priority entitlement).
        for(std::uint32_t id = 0; id < queueFamilyProps.size(); ++id){
            const auto & q = queueFamilyProps[id];
            const VkQueueFlags wanted =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
            if((q.queueFlags & wanted) == 0){
                continue;
            }
            queueFamilyIndices.push_back(id);

            const std::uint32_t maxQueues = q.queueCount > 0 ? q.queueCount : 1;
            const std::uint32_t openCount = std::min<std::uint32_t>(3, maxQueues);
            queueFloatPriorities.emplace_back();
            auto & floats = queueFloatPriorities.back();
            OmegaCommon::Vector<VkQueueGlobalPriorityKHR> kinds;
            // Float-priority ladder: 1.0 / 0.5 / 0.0 → HIGH / NORMAL / LOW.
            // openCount==1 picks NORMAL only (default semantics preserved);
            // openCount==2 picks HIGH + NORMAL (most useful pair when the
            // family can't host three queues); openCount==3 picks the full
            // ladder.
            constexpr float ladderFloats[3] = {1.0f, 0.5f, 0.0f};
            constexpr VkQueueGlobalPriorityKHR ladderKinds[3] = {
                VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR,
                VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR,
                VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR,
            };
            if (openCount >= 3) {
                floats = {ladderFloats[0], ladderFloats[1], ladderFloats[2]};
                kinds  = {ladderKinds[0],  ladderKinds[1],  ladderKinds[2]};
            } else if (openCount == 2) {
                floats = {ladderFloats[0], ladderFloats[1]};
                kinds  = {ladderKinds[0],  ladderKinds[1]};
            } else {
                floats = {ladderFloats[1]};
                kinds  = {ladderKinds[1]};
            }
            perFamilyKinds.push_back(kinds);

            VkDeviceQueueCreateInfo queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueInfo.pNext = nullptr;
            queueInfo.queueFamilyIndex = id;
            queueInfo.queueCount = openCount;
            queueInfo.pQueuePriorities = floats.data();
            queueInfo.flags = 0;
            if (hasGlobalPriorityExt) {
                // Chain MEDIUM as the per-family outer priority. The
                // per-queue differentiation lives in pQueuePriorities; the
                // chain is the "we know the extension exists, no surprises"
                // signal to the driver. Asking for HIGH here would risk
                // VK_ERROR_NOT_PERMITTED_KHR on dev hosts without the
                // privilege; the runtime retry below catches that anyway.
                VkDeviceQueueGlobalPriorityCreateInfoKHR chain {
                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
                chain.pNext = nullptr;
                chain.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
                queuePriorityChains.push_back(chain);
                queueInfo.pNext = &queuePriorityChains.back();
            }
            deviceQueues.push_back(queueInfo);
        }
        if(deviceQueues.empty()){
            std::cerr << "No Vulkan queue families support graphics/compute/transfer." << std::endl;
            std::exit(1);
        }

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(physicalDevice,&features);

        VkPhysicalDeviceFeatures2 features2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.features = features;
        features2.pNext = nullptr;

        VkPhysicalDeviceSynchronization2FeaturesKHR sync2Features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
        sync2Features.synchronization2 = VK_TRUE;
        sync2Features.pNext = nullptr;

        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extDynFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        extDynFeatures.extendedDynamicState = VK_TRUE;
        extDynFeatures.pNext = nullptr;

        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bdaFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR};
        bdaFeatures.bufferDeviceAddress = VK_TRUE;
        bdaFeatures.pNext = nullptr;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        accelFeatures.accelerationStructure = VK_TRUE;
        accelFeatures.pNext = nullptr;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
        rtPipelineFeatures.pNext = nullptr;

    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        /// Mesh-Shader-Plan Phase 4a/§5 — request the mesh-stage feature and,
        /// since §5, the task (amplification) stage alongside it. Phase 4a held
        /// `taskShader` at FALSE deliberately: enabling a stage the codegen could
        /// not emit would have advertised a capability we could not honor. §5
        /// lands that codegen, so the flag now follows the device.
        ///
        /// `taskShader` is queried, not assumed. `VK_EXT_mesh_shader` mandates
        /// `meshShader` but leaves `taskShader` optional, so a device can expose
        /// the extension with mesh-only support. Hard-coding VK_TRUE here would
        /// fail `vkCreateDevice` outright on such a device — taking the whole
        /// engine down, not just the amplification stage. `hasTaskShaderFeature`
        /// is what `makeMeshPipelineState` then checks before accepting an
        /// `amplificationFunc`.
        VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatureQuery {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 meshFeatureQuery2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        meshFeatureQuery2.pNext = &meshShaderFeatureQuery;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &meshFeatureQuery2);
        hasTaskShaderFeature = (meshShaderFeatureQuery.taskShader == VK_TRUE);

        VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        meshShaderFeatures.meshShader = VK_TRUE;
        meshShaderFeatures.taskShader = hasTaskShaderFeature ? VK_TRUE : VK_FALSE;
        meshShaderFeatures.pNext = nullptr;
    #endif

        // Timeline semaphores back the engine retention queue's per-queue
        // gate (FenceGate -> vkGetSemaphoreCounterValue >= V). Core in
        // Vulkan 1.2 — the VMA allocator already requests
        // VK_API_VERSION_1_2 below, so the device is required to support
        // this feature for the engine to function.
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timelineFeatures.timelineSemaphore = VK_TRUE;
        timelineFeatures.pNext = nullptr;

        void **pNextTail = &features2.pNext;
        *pNextTail = &timelineFeatures;
        pNextTail = &timelineFeatures.pNext;
        if(hasSynchronization2Ext){
            *pNextTail = &sync2Features;
            pNextTail = &sync2Features.pNext;
        }
        if(hasExtendedDynamicState){
            *pNextTail = &extDynFeatures;
            pNextTail = &extDynFeatures.pNext;
        }
        if(hasBufferDeviceAddressExt){
            *pNextTail = &bdaFeatures;
            pNextTail = &bdaFeatures.pNext;
        }
        if(hasAccelerationStructureExt){
            *pNextTail = &accelFeatures;
            pNextTail = &accelFeatures.pNext;
        }
        if(hasRayTracingPipelineExt){
            *pNextTail = &rtPipelineFeatures;
            pNextTail = &rtPipelineFeatures.pNext;
        }
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(hasMeshShaderExt){
            *pNextTail = &meshShaderFeatures;
            pNextTail = &meshShaderFeatures.pNext;
        }
    #endif

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pQueueCreateInfos = deviceQueues.data();
        info.queueCreateInfoCount = deviceQueues.size();
        info.enabledExtensionCount = extensionNames.size();
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        info.ppEnabledExtensionNames = extensionNames.data();

        // timelineFeatures is always chained, so we always go through
        // features2 (cannot mix pEnabledFeatures with VkPhysicalDeviceFeatures2
        // in pNext).
        info.pNext = &features2;
        info.pEnabledFeatures = nullptr;

        // CommandQueue-Typed-Pool follow-up — vkCreateDevice retry block.
        // The global-priority chain (MEDIUM) is normally granted to every
        // process, but some drivers / sandboxes still return
        // VK_ERROR_NOT_PERMITTED_KHR. When that fires, strip the chain
        // entirely and retry — the per-queue float priorities still give
        // the user HIGH/NORMAL/LOW differentiation within each family,
        // we just lose the (relatively minor) outer extension-tagged
        // priority on top of it.
        auto stripGlobalPriorityChain = [&]() {
            for (auto & dq : deviceQueues) {
                dq.pNext = nullptr;
            }
            hasGlobalPriorityExt = false;
        };

        VkResult deviceRes = vkCreateDevice(physicalDevice,&info,nullptr,&this->device);
        if (deviceRes == VK_ERROR_NOT_PERMITTED_KHR && hasGlobalPriorityExt) {
            DEBUG_STREAM("vkCreateDevice denied MEDIUM global-priority chain; retrying without the chain");
            stripGlobalPriorityChain();
            deviceRes = vkCreateDevice(physicalDevice,&info,nullptr,&this->device);
        }
        if(deviceRes != VK_SUCCESS || this->device == VK_NULL_HANDLE){
            std::cerr << "Failed to create Vulkan logical device (" << deviceRes << ")" << std::endl;
            std::exit(1);
        }

        // Walk deviceQueues in lockstep with perFamilyKinds — one
        // VkDeviceQueueCreateInfo per opened family, queueCount queues per
        // family, each queueIndex paired with perFamilyKinds[slot][i] so the
        // resulting openedQueuePriorities table tells lookupQueueOnFamily
        // which VkQueue corresponds to HIGH / NORMAL / LOW.
        for (std::size_t slot = 0; slot < deviceQueues.size(); ++slot) {
            auto & dq = deviceQueues[slot];
            std::vector<std::pair<VkSemaphore,VkQueue>> queues;
            OmegaCommon::Vector<std::int32_t> priorities;
            for(unsigned i = 0;i < dq.queueCount;i++){
                VkSemaphore sem = VK_NULL_HANDLE;
                VkQueue queue = VK_NULL_HANDLE;

                VkSemaphoreCreateInfo semaphoreInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                semaphoreInfo.flags = 0;
                semaphoreInfo.pNext = nullptr;
                auto semaphoreRes = vkCreateSemaphore(this->device,&semaphoreInfo,nullptr,&sem);
                if(semaphoreRes != VK_SUCCESS){
                    continue;
                }

                VkDeviceQueueInfo2 queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                queueInfo.pNext = nullptr;
                queueInfo.queueFamilyIndex = dq.queueFamilyIndex;
                queueInfo.queueIndex = i;
                queueInfo.flags = 0;
                vkGetDeviceQueue2(this->device,&queueInfo,&queue);
                if(queue == VK_NULL_HANDLE){
                    vkDestroySemaphore(this->device,sem,nullptr);
                    continue;
                }

                queues.emplace_back(std::make_pair(sem,queue));
                // Defensive bounds-check — perFamilyKinds[slot] was sized
                // to dq.queueCount in the build loop; only diverges if a
                // vkGetDeviceQueue2 call silently dropped one above (in
                // which case the bigger queueCount above won't match the
                // smaller priorities count here and lookupQueueOnFamily
                // notices via the `prios.size() == queues.size()` gate).
                if (slot < perFamilyKinds.size() && i < perFamilyKinds[slot].size()) {
                    priorities.push_back(static_cast<std::int32_t>(perFamilyKinds[slot][i]));
                }
            }
            if(!queues.empty()){
                deviceQueuefamilies.push_back(queues);
                openedQueuePriorities.push_back(priorities);
            }
        }
        if(deviceQueuefamilies.empty()){
            std::cerr << "Failed to acquire Vulkan device queues." << std::endl;
            std::exit(1);
        }

        if(hasPushDescriptorExt){
            vkCmdPushDescriptorSetKhr = (PFN_vkCmdPushDescriptorSetKHR) vkGetDeviceProcAddr(this->device,"vkCmdPushDescriptorSetKHR");
        }

        if(hasSynchronization2Ext){
            vkCmdPipelineBarrier2Khr = (PFN_vkCmdPipelineBarrier2KHR) vkGetDeviceProcAddr(this->device,"vkCmdPipelineBarrier2KHR");
        }

        if(hasExtendedDynamicState){
            vkCmdSetPrimitiveTopologyExt = (PFN_vkCmdSetPrimitiveTopologyEXT) vkGetDeviceProcAddr(this->device,"vkCmdSetPrimitiveTopologyEXT");
        }

        if(hasBufferDeviceAddressExt){
            vkGetBufferDeviceAddressKhr = (PFN_vkGetBufferDeviceAddressKHR) vkGetDeviceProcAddr(this->device,"vkGetBufferDeviceAddressKHR");
        }

        if(hasAccelerationStructureExt){
            vkCreateAccelerationStructureKhr = (PFN_vkCreateAccelerationStructureKHR) vkGetDeviceProcAddr(this->device,"vkCreateAccelerationStructureKHR");
            vkDestroyAccelerationStructureKhr = (PFN_vkDestroyAccelerationStructureKHR) vkGetDeviceProcAddr(this->device,"vkDestroyAccelerationStructureKHR");
            vkGetAccelerationStructureBuildSizesKhr = (PFN_vkGetAccelerationStructureBuildSizesKHR) vkGetDeviceProcAddr(this->device,"vkGetAccelerationStructureBuildSizesKHR");
            vkCmdBuildAccelerationStructuresKhr = (PFN_vkCmdBuildAccelerationStructuresKHR) vkGetDeviceProcAddr(this->device,"vkCmdBuildAccelerationStructuresKHR");
            vkCmdCopyAccelerationStructureKhr = (PFN_vkCmdCopyAccelerationStructureKHR) vkGetDeviceProcAddr(this->device,"vkCmdCopyAccelerationStructureKHR");
            vkGetAccelerationStructureDeviceAddressKhr = (PFN_vkGetAccelerationStructureDeviceAddressKHR) vkGetDeviceProcAddr(this->device,"vkGetAccelerationStructureDeviceAddressKHR");
        }

        if(hasRayTracingPipelineExt){
            vkCmdTraceRaysKhr = (PFN_vkCmdTraceRaysKHR) vkGetDeviceProcAddr(this->device,"vkCmdTraceRaysKHR");
        }
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(hasMeshShaderExt){
            /// Phase 4a — load the mesh-stage dispatch entry point.
            /// Static linkage isn't an option (`VK_EXT_mesh_shader`'s
            /// commands are extension-loader-only); resolve through
            /// `vkGetDeviceProcAddr` exactly like the RT dispatch above.
            vkCmdDrawMeshTasksExt = (PFN_vkCmdDrawMeshTasksEXT) vkGetDeviceProcAddr(this->device,"vkCmdDrawMeshTasksEXT");
        }
    #endif

        VmaAllocatorCreateInfo allocator_info {};
        allocator_info.instance = instance;
        allocator_info.device = this->device;
        allocator_info.physicalDevice = physicalDevice;
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
        if(hasBufferDeviceAddressExt){
            allocator_info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }
        auto _res = vmaCreateAllocator(&allocator_info,&memAllocator);

        if(_res != VK_SUCCESS){
            printf("Failed to Create Allocator");
            exit(1);
        };
      

        resource_count = 0;

        // Vulkan-Texture-Memory-Plan Phase 1. Persistent infrastructure
        // for the synchronous staging-buffer upload path used by
        // GEVulkanTexture::copyBytes / getBytes (ToGPU / FromGPU).
        // Pick the first graphics/compute queue as the upload queue —
        // matches what debugReadbackPixelRGBA8 already does. A dedicated
        // transfer-only family would be slightly faster on discrete
        // GPUs, but enumerating one requires fixing the queue-family
        // enumeration loop above (the `else { continue; }` arm skips
        // the `++id` so transfer-only family indices are wrong); that's
        // an independent change. Falling back to the graphics queue is
        // always safe because graphics-capable queues implicitly
        // support transfer.
        if(!queueFamilyIndices.empty() && !deviceQueuefamilies.empty()
           && !deviceQueuefamilies[0].empty()){
            uploadQueueFamily = queueFamilyIndices[0];
            // CommandQueue-Typed-Pool follow-up — pick the MEDIUM-priority
            // queue on family 0 explicitly. The naive `[0][0].second` would
            // now be the HIGH-float queue (since we open
            // pQueuePriorities=[1.0,0.5,0.0] in that order), and texture
            // staging shouldn't take priority over user-submitted frame
            // work. lookupQueueOnFamily falls through to the first opened
            // queue when openedQueuePriorities is empty (extension off /
            // single-queue family), so this stays correct in every config.
            uploadQueue       = lookupQueueOnFamily(uploadQueueFamily,
                                                    static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR));
            if (uploadQueue == VK_NULL_HANDLE) {
                uploadQueue = deviceQueuefamilies[0][0].second;
            }

            VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.queueFamilyIndex = uploadQueueFamily;
            // RESET_COMMAND_BUFFER so each upload can reset its
            // command buffer individually rather than tearing down the
            // pool. TRANSIENT advertises the one-shot lifetime hint.
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                           | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            auto poolRes = vkCreateCommandPool(this->device, &poolInfo, nullptr, &uploadCommandPool);
            if(poolRes != VK_SUCCESS){
                std::cerr << "GEVulkanEngine: failed to create upload command pool ("
                          << poolRes << "). copyBytes on ToGPU textures will fail." << std::endl;
                uploadCommandPool = VK_NULL_HANDLE;
            }

            VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            // Default unsignaled — the first wait will block until the
            // first submit completes.
            fenceInfo.flags = 0;
            auto fenceRes = vkCreateFence(this->device, &fenceInfo, nullptr, &uploadFence);
            if(fenceRes != VK_SUCCESS){
                std::cerr << "GEVulkanEngine: failed to create upload fence ("
                          << fenceRes << ")." << std::endl;
                uploadFence = VK_NULL_HANDLE;
            }
        }

        DEBUG_STREAM("Successfully Created GEVulkanEngine");
    };

    void * GEVulkanEngine::underlyingNativeDevice(){
        return (void *)device;
    }

    SharedHandle<OmegaGraphicsEngine> GEVulkanEngine::Create(SharedHandle<GTEDevice> & device){
        auto engine = SharedHandle<OmegaGraphicsEngine>(new GEVulkanEngine(std::dynamic_pointer_cast<GTEVulkanDevice>(device)));
        // Retain the GTEDevice handle so OmegaSL runtime compilation can be
        // initialized lazily (Extension 3 — blit fullscreen VS).
        static_cast<GEVulkanEngine *>(engine.get())->gteDevice = device;
        return engine;
    };

    VkQueue GEVulkanEngine::lookupQueueOnFamily(std::uint32_t familyIndex,
                                                std::int32_t wantedKhrPriority) const {
        // Locate the family slot. queueFamilyIndices is parallel to
        // deviceQueuefamilies / openedQueuePriorities; not present means the
        // family was never opened (e.g. requireDedicated landed us on a
        // family that wasn't qualifying at device-create).
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t i = 0; i < queueFamilyIndices.size(); ++i) {
            if (queueFamilyIndices[i] == familyIndex) { slot = i; break; }
        }
        if (slot == std::numeric_limits<std::uint32_t>::max() ||
            slot >= deviceQueuefamilies.size() || deviceQueuefamilies[slot].empty()) {
            return VK_NULL_HANDLE;
        }

        const auto & queues   = deviceQueuefamilies[slot];
        // When the extension was off, openedQueuePriorities stays empty.
        // Treat every entry as MEDIUM in that case and fall through to the
        // default-priority queue at index 0.
        const bool hasPriorities = slot < openedQueuePriorities.size()
                                && openedQueuePriorities[slot].size() == queues.size();
        if (!hasPriorities) {
            return queues.front().second;
        }
        const auto & prios = openedQueuePriorities[slot];

        // 1. Exact match.
        for (std::size_t i = 0; i < prios.size(); ++i) {
            if (prios[i] == wantedKhrPriority) {
                return queues[i].second;
            }
        }
        // 2. Fallback ladder. For REALTIME / HIGH requests we walk DOWN
        // (HIGH → MEDIUM → LOW) since elevation is the point of asking;
        // for LOW we walk UP (MEDIUM → HIGH) since dropping below LOW
        // doesn't make sense. MEDIUM at the centre is always present.
        const std::int32_t kRT     = static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR);
        const std::int32_t kHigh   = static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR);
        const std::int32_t kMedium = static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);
        const std::int32_t kLow    = static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);

        std::int32_t ladder[3] = {kMedium, kMedium, kMedium};
        if (wantedKhrPriority == kRT) {
            ladder[0] = kHigh;   ladder[1] = kMedium; ladder[2] = kLow;
        } else if (wantedKhrPriority == kHigh) {
            ladder[0] = kMedium; ladder[1] = kLow;    ladder[2] = kMedium;
        } else if (wantedKhrPriority == kLow) {
            ladder[0] = kMedium; ladder[1] = kHigh;   ladder[2] = kMedium;
        }
        for (std::int32_t want : ladder) {
            for (std::size_t i = 0; i < prios.size(); ++i) {
                if (prios[i] == want) {
                    return queues[i].second;
                }
            }
        }
        // Last resort: the first opened queue. Better than VK_NULL_HANDLE
        // even when every reasonable priority slot is somehow empty.
        return queues.front().second;
    }

    /// Phase-2 typed-pool path. Routes through `VulkanQueueFamilies::Pick`
    /// to resolve `desc.type` to a real Vulkan queue family (with the
    /// documented fallback ladder when no dedicated family exists), and
    /// plumbs `desc.label` through `VK_EXT_debug_utils` when available.
    ///
    /// `desc.priority` is recorded on the returned queue for introspection
    /// but is currently NOT plumbed into `VkDeviceQueueCreateInfo`. The
    /// `VK_KHR_global_priority` opt-in (and the matching
    /// `VkDeviceQueueGlobalPriorityCreateInfoKHR` chain at device-create
    /// time) is a Phase-2 follow-up tracked in the plan doc — it requires
    /// extension probing at device-create, not at queue-create, so wiring
    /// it deserves its own change.
    ///
    /// Returns `nullptr` when `desc.requireDedicated` is set and no family
    /// can satisfy the request (e.g. Transfer-only requested on a device
    /// that only exposes a unified graphics+compute family).
    SharedHandle<GECommandQueue> GEVulkanEngine::makeCommandQueue(const GECommandQueueDesc & desc){
        auto result = std::make_shared<GEVulkanCommandQueue>(this, desc);
        if (result->getNativeQueue() == VK_NULL_HANDLE) {
            // Construction failed to resolve a family (requireDedicated with
            // nothing dedicated, or the device exposes no families at all).
            // Return nullptr rather than handing back a half-constructed
            // queue — caller can fall back to a less-specific request.
            return nullptr;
        }
        trackResource(result);
        return result;
    }

    SharedHandle<GEBuffer> GEVulkanEngine::makeBuffer(const BufferDescriptor &desc){
        if(device == VK_NULL_HANDLE || memAllocator == nullptr){
            std::cerr << "Vulkan makeBuffer failed: invalid Vulkan device or allocator." << std::endl;
            return nullptr;
        }

        VkBufferCreateInfo buffer_desc {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_desc.pNext = nullptr;
        buffer_desc.flags = 0;
        buffer_desc.size = desc.len > 0 ? desc.len : 1;
        buffer_desc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if(hasBufferDeviceAddressExt){
            buffer_desc.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        if(hasAccelerationStructureExt){
            buffer_desc.usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        /// §2.4 — a buffer bound to a `uniform<T>` slot is written into a
        /// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER descriptor, which requires the
        /// VkBuffer to carry this usage bit. Added only for Uniform-role
        /// buffers; storage buffers keep the original mask.
        if(desc.role == BufferDescriptor::Uniform){
            buffer_desc.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        }
        buffer_desc.sharingMode = queueFamilyIndices.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        buffer_desc.queueFamilyIndexCount = buffer_desc.sharingMode == VK_SHARING_MODE_CONCURRENT
                                                ? static_cast<std::uint32_t>(queueFamilyIndices.size())
                                                : 0;
        buffer_desc.pQueueFamilyIndices = buffer_desc.sharingMode == VK_SHARING_MODE_CONCURRENT
                                              ? queueFamilyIndices.data()
                                              : nullptr;

        VmaAllocationCreateInfo alloc_info {};
        switch (desc.usage) {
            case BufferDescriptor::Upload : {
                alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                break;
            }
            case BufferDescriptor::Readback : {
                alloc_info.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                break;
            }
            case BufferDescriptor::GPUOnly : {
                alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
                break;
            }
            case BufferDescriptor::Universal : {
                // CPU-write + GPU-write + CPU-read. Vulkan host-visible
                // memory serves all three directions on one resource, so no
                // companions are needed — CPU_TO_GPU keeps the mapping
                // writable and the storage-usage bits above keep it
                // kernel-writable. (The D3D12 backend is where Universal
                // pays its companion + copy cost.)
                alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                break;
            }
        }

        alloc_info.priority = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo allocationInfo {};

        auto bufferRes = vmaCreateBuffer(memAllocator,&buffer_desc,&alloc_info,&buffer,&allocation,&allocationInfo);
        if(!VK_RESULT_SUCCEEDED(bufferRes) || buffer == VK_NULL_HANDLE || allocation == nullptr){
            std::cerr << "Vulkan makeBuffer failed: vmaCreateBuffer returned " << bufferRes << std::endl;
            return nullptr;
        }

        // Structured buffers are bound as storage buffers in current command encoding, so no texel view is required.
        VkBufferView bufferView = VK_NULL_HANDLE;

        auto result = std::shared_ptr<GEVulkanBuffer>(new GEVulkanBuffer(desc.usage,this,buffer,bufferView,allocation,allocationInfo));
        result->role = desc.role;
        trackResource(result);
        return result;
    };
    void GEVulkanHeap::releaseNative(){
        if(nativeReleased_) return;
        nativeReleased_ = true;
        if(pool != VK_NULL_HANDLE){
            vmaDestroyPool(engine->memAllocator, pool);
            pool = VK_NULL_HANDLE;
        }
    }
    GEVulkanHeap::~GEVulkanHeap(){
        if(!nativeReleased_){
            if(pool != VK_NULL_HANDLE){
                vmaDestroyPool(engine->memAllocator, pool);
            }
        }
    }

    SharedHandle<GEBuffer> GEVulkanHeap::makeBuffer(const BufferDescriptor &desc){
        VkBufferCreateInfo bufferInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = desc.len > 0 ? desc.len : 1;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        /// §2.4 — see GEVulkanEngine::makeBuffer; Uniform-role buffers need
        /// the uniform-buffer usage bit to be writable into a UBO descriptor.
        if(desc.role == BufferDescriptor::Uniform){
            bufferInfo.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        }
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo {};
        allocInfo.pool = pool;
        switch(desc.usage){
            case BufferDescriptor::Upload: allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU; break;
            case BufferDescriptor::Readback: allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU; break;
            case BufferDescriptor::GPUOnly: allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY; break;
            // Universal: host-visible serves CPU write + GPU write + CPU read
            // on one resource (see the engine makeBuffer note above).
            case BufferDescriptor::Universal: allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU; break;
        }

        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VmaAllocationInfo allocationInfo {};
        auto res = vmaCreateBuffer(engine->memAllocator, &bufferInfo, &allocInfo, &buffer, &alloc, &allocationInfo);
        if(!VK_RESULT_SUCCEEDED(res) || buffer == VK_NULL_HANDLE){
            return nullptr;
        }

        VkBufferView bufferView = VK_NULL_HANDLE;
        auto result = std::shared_ptr<GEVulkanBuffer>(new GEVulkanBuffer(desc.usage, engine, buffer, bufferView, alloc, allocationInfo));
        result->role = desc.role;
        engine->trackResource(result);
        return result;
    }

    SharedHandle<GETexture> GEVulkanHeap::makeTexture(const TextureDescriptor &desc){
        VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = 0;

        VkFormat imageFormat = pixelFormatToVkFormat(desc.pixelFormat);
        imageInfo.format = imageFormat;

        // §6.2 — heap path mirrors the engine path's kind dispatch.
        const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
        const unsigned descArrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
        const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);
        const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u) : 1u;

        VkImageType type = VK_IMAGE_TYPE_2D;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        unsigned vkArrayLayers = 1;
        switch(kind){
            case TextureKind::Tex1D:        type = VK_IMAGE_TYPE_1D; viewType = VK_IMAGE_VIEW_TYPE_1D; break;
            case TextureKind::Tex1DArray:
                type = VK_IMAGE_TYPE_1D; viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::Tex2D:        type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_2D; break;
            case TextureKind::Tex2DArray:
                type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::TexCube:
                type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_CUBE;
                vkArrayLayers = 6;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
            case TextureKind::TexCubeArray:
                type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                vkArrayLayers = descArrayLayers >= 6 ? descArrayLayers : 6;
                imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
            case TextureKind::Tex2DMS:      type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_2D; break;
            case TextureKind::Tex2DMSArray:
                type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::Tex3D:        type = VK_IMAGE_TYPE_3D; viewType = VK_IMAGE_VIEW_TYPE_3D; break;
            case TextureKind::Auto:         break;
        }
        imageInfo.imageType = type;
        imageInfo.extent = {desc.width, desc.height, desc.depth};
        imageInfo.mipLevels = isMS ? 1u : desc.mipLevels;
        imageInfo.arrayLayers = vkArrayLayers;
        switch(effectiveSampleCount){
            case 2:  imageInfo.samples = VK_SAMPLE_COUNT_2_BIT; break;
            case 4:  imageInfo.samples = VK_SAMPLE_COUNT_4_BIT; break;
            case 8:  imageInfo.samples = VK_SAMPLE_COUNT_8_BIT; break;
            case 16: imageInfo.samples = VK_SAMPLE_COUNT_16_BIT; break;
            case 32: imageInfo.samples = VK_SAMPLE_COUNT_32_BIT; break;
            case 64: imageInfo.samples = VK_SAMPLE_COUNT_64_BIT; break;
            default: imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; break;
        }
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if(desc.usage == GETexture::FromGPU || desc.usage == GETexture::GPUAccessOnly){
            imageInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        if(desc.usage == GETexture::RenderTarget || desc.usage == GETexture::RenderTargetAndDepthStencil){
            imageInfo.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo {};
        allocInfo.pool = pool;
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkImage img = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VmaAllocationInfo allocationInfo {};
        auto res = vmaCreateImage(engine->memAllocator, &imageInfo, &allocInfo, &img, &alloc, &allocationInfo);
        if(!VK_RESULT_SUCCEEDED(res) || img == VK_NULL_HANDLE){
            return nullptr;
        }

        VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = img;
        viewInfo.viewType = viewType;
        viewInfo.format = imageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = vkArrayLayers;

        VkImageView imgView = VK_NULL_HANDLE;
        vkCreateImageView(engine->device, &viewInfo, nullptr, &imgView);

        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY;

        auto result = std::shared_ptr<GEVulkanTexture>(new GEVulkanTexture(
            kind, desc.usage, desc.pixelFormat,
            engine, img, imgView, layout, allocationInfo, alloc, desc, memUsage));
        result->format = imageFormat;
        result->setShape(kind, vkArrayLayers, effectiveSampleCount);
        engine->trackResource(result);
        return result;
    }

    SharedHandle<GEHeap> GEVulkanEngine::makeHeap(const HeapDescriptor &desc){
        if(device == VK_NULL_HANDLE || memAllocator == nullptr){
            std::cerr << "Vulkan makeHeap failed: invalid device or allocator." << std::endl;
            return nullptr;
        }

        VmaPoolCreateInfo poolInfo {};
        poolInfo.blockSize = desc.len;
        poolInfo.maxBlockCount = 1;

        VkBufferCreateInfo sampleBufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sampleBufInfo.size = 64;
        sampleBufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo sampleAllocInfo {};
        sampleAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        uint32_t memTypeIdx = 0;
        vmaFindMemoryTypeIndexForBufferInfo(memAllocator, &sampleBufInfo, &sampleAllocInfo, &memTypeIdx);
        poolInfo.memoryTypeIndex = memTypeIdx;

        VmaPool pool = VK_NULL_HANDLE;
        auto res = vmaCreatePool(memAllocator, &poolInfo, &pool);
        if(!VK_RESULT_SUCCEEDED(res)){
            std::cerr << "Vulkan makeHeap failed: vmaCreatePool returned " << res << std::endl;
            return nullptr;
        }

        auto result = std::shared_ptr<GEVulkanHeap>(new GEVulkanHeap(this, pool, desc.len));
        trackResource(result);
        return result;
    };

    SharedHandle<GETexture>GEVulkanEngine::makeTexture(const TextureDescriptor &desc){
        if(device == VK_NULL_HANDLE || memAllocator == nullptr){
            std::cerr << "Vulkan makeTexture failed: invalid Vulkan device or allocator." << std::endl;
            return nullptr;
        }

        VkImageCreateInfo image_desc {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_desc.pNext = nullptr;
        image_desc.flags = 0;

        VkFormat image_format = pixelFormatToVkFormat(desc.pixelFormat);

        image_desc.format = image_format;

        // §6.2 — pick VkImageType / VkImageViewType / arrayLayers from
        // the descriptor's kind. Cube images need VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
        // so the view can use VIEW_TYPE_CUBE. `Auto` is treated as Tex2D for
        // back-compat with descriptors that never set kind explicitly.
        const TextureKind kind = desc.kind == TextureKind::Auto ? TextureKind::Tex2D : desc.kind;
        const unsigned descArrayLayers = desc.arrayLayers > 0 ? desc.arrayLayers : 1;
        const bool isMS = (kind == TextureKind::Tex2DMS || kind == TextureKind::Tex2DMSArray);

        VkImageType type = VK_IMAGE_TYPE_2D;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        unsigned vkArrayLayers = 1;
        switch (kind) {
            case TextureKind::Tex1D:
                type = VK_IMAGE_TYPE_1D;
                viewType = VK_IMAGE_VIEW_TYPE_1D;
                break;
            case TextureKind::Tex1DArray:
                type = VK_IMAGE_TYPE_1D;
                viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::Tex2D:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            case TextureKind::Tex2DArray:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::TexCube:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_CUBE;
                vkArrayLayers = 6;
                image_desc.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
            case TextureKind::TexCubeArray:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                vkArrayLayers = descArrayLayers >= 6 ? descArrayLayers : 6;
                image_desc.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                break;
            case TextureKind::Tex2DMS:
                // Vulkan: multisample-ness is on the underlying image's
                // `samples` field; the view type stays VIEW_TYPE_2D.
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            case TextureKind::Tex2DMSArray:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                vkArrayLayers = descArrayLayers;
                break;
            case TextureKind::Tex3D:
                type = VK_IMAGE_TYPE_3D;
                viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            case TextureKind::Auto:
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
        }

        const unsigned width = desc.width > 0 ? desc.width : 1;
        const unsigned height = desc.height > 0 ? desc.height : 1;
        const unsigned depth = desc.depth > 0 ? desc.depth : 1;
        const unsigned descMipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
        const unsigned mipLevels = isMS ? 1u : descMipLevels;
        const unsigned effectiveSampleCount = isMS ? (desc.sampleCount > 1 ? desc.sampleCount : 1u) : 1u;

        image_desc.imageType = type;
        image_desc.extent.width = width;
        image_desc.extent.height = height;
        image_desc.extent.depth = depth;
        image_desc.mipLevels = mipLevels;
        image_desc.arrayLayers = vkArrayLayers;
        // Default tiling is OPTIMAL; the per-usage switch below overrides
        // this to LINEAR for `ToGPU` / `FromGPU` so the image's allocation
        // can be HOST_VISIBLE for direct `copyBytes` / `getBytes`. Other
        // usages keep OPTIMAL — that's what device-local sampling /
        // attachment / compute paths need on a discrete GPU.
        image_desc.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        switch (effectiveSampleCount) {
            case 1:
                image_desc.samples = VK_SAMPLE_COUNT_1_BIT;
                break;
            case 2:
                image_desc.samples = VK_SAMPLE_COUNT_2_BIT;
                break;
            case 4:
                image_desc.samples = VK_SAMPLE_COUNT_4_BIT;
                break;
            case 8:
                image_desc.samples = VK_SAMPLE_COUNT_8_BIT;
                break;
            case 16:
                image_desc.samples = VK_SAMPLE_COUNT_16_BIT;
                break;
            case 32:
                image_desc.samples = VK_SAMPLE_COUNT_32_BIT;
                break;
            case 64:
                image_desc.samples = VK_SAMPLE_COUNT_64_BIT;
                break;
            default:
                image_desc.samples = VK_SAMPLE_COUNT_1_BIT;
                break;
        }

        VkImageUsageFlags usageFlags = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaMemoryUsage memoryUsage;


        switch (desc.usage) {
            case GETexture::GPUAccessOnly : {
                // GPU-only sampling + compute writes. D3D12 grants UAV
                // here (`isUAV=true`) and Metal grants `ShaderWrite`; the
                // missing STORAGE_BIT was a Vulkan parity bug — without it
                // any compute-shader write through this texture would
                // fail validation.
                usageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_STORAGE_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                break;
            }
            case GETexture::ToGPU : {
                // Vulkan-Texture-Memory-Plan §"Design". The image is
                // device-local OPTIMAL — a companion HOST_VISIBLE
                // staging VkBuffer (allocated further down once we
                // have the descriptor's mip chain in hand) carries
                // the CPU writes. The buffer→image copy is issued
                // synchronously from GEVulkanTexture::copyBytes via
                // GEVulkanEngine::submitImmediateUploadFromStaging.
                //
                // This mirrors D3D12 (`GED3D12.cpp:1986` ToGPU arm —
                // device-local resource + UPLOAD-heap cpuSideresource)
                // and unlocks every (format × mip-chain × MSAA)
                // combination that LINEAR tiling can't host. The
                // earlier LINEAR-direct-map path failed
                // `vkCreateImage` with `mipLevels > 1` because
                // virtually all desktop drivers report
                // `linearTilingFeatures.maxMipLevels == 1` for color
                // formats (the bug that surfaced via WTK's
                // ImageRenderTest at 900x987x10 mips).
                usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;

                break;
            }
            case GETexture::FromGPU : {
                // GPU renders / samples into the image, CPU reads back.
                // Mirror of ToGPU's reasoning: image stays
                // device-local OPTIMAL, a staging buffer in
                // HOST_VISIBLE memory receives the readback via
                // submitImmediateReadbackToStaging. STORAGE_BIT stays
                // off — no current FromGPU caller writes through
                // compute; render-targets that need readback already
                // include TRANSFER_SRC_BIT through their RenderTarget
                // arm.
                usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;

                break;
            }
            case GETexture::RenderTargetAndDepthStencil : {
                // TODO(depth-formats): when `TexturePixelFormat` grows
                // depth/stencil entries, branch the attachment usage on
                // whether `desc.pixelFormat` is a color or depth format —
                // a single VkImage cannot be both COLOR and
                // DEPTH_STENCIL attachment. Until then every format
                // resolved by `pixelFormatToVkFormat` is a color format,
                // so DEPTH_STENCIL_ATTACHMENT_BIT was dead code that
                // would fail validation the moment a depth format
                // landed. Treat this case as a color render target for
                // now; depth-target work will revisit.
                usageFlags =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                break;
            }
            case GETexture::RenderTarget : {
                usageFlags =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;

                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;

                break;
            }
            case GETexture::MSResolveSrc : {
                // A multisample-resolve source is a render target that
                // gets resolved into a single-sample destination. The
                // GPU rasterizes into it (COLOR_ATTACHMENT), the resolve
                // copies out (TRANSFER_SRC), and shaders may sample the
                // resolved image (SAMPLED). The previous flags
                // (STORAGE | TRANSFER_SRC) didn't include
                // COLOR_ATTACHMENT — there'd be nothing to resolve from.
                // STORAGE is also unsupported on multisampled images by
                // most drivers.
                usageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;

                break;
            }
            default: {
                usageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                break;
            }
        }

        // Format-support guard. After the staging-buffer move
        // (Vulkan-Texture-Memory-Plan Phase 3) the only tiling we
        // emit is OPTIMAL, so the check moved over to
        // `optimalTilingFeatures`. Color formats (RGBA8/16, BGRA8,
        // their SRGB siblings) all support SAMPLED + TRANSFER on
        // every desktop driver — the guard exists so future
        // additions (depth/stencil, BC/ASTC compressed) get a clean
        // diagnostic instead of a generic VMA failure when an
        // unsupported (format, usage) combination lands.
        {
            VkFormatProperties fmtProps{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, image_format, &fmtProps);
            const VkFormatFeatureFlags requiredFeatures =
                ((usageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0u)
                | ((usageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT : 0u)
                | ((usageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0u)
                | ((usageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0u)
                | ((usageFlags & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0u);
            if((fmtProps.optimalTilingFeatures & requiredFeatures) != requiredFeatures){
                std::cerr << "Vulkan makeTexture failed: format " << image_format
                          << " does not support OPTIMAL tiling for usage 0x"
                          << std::hex << usageFlags << std::dec
                          << " (optimalTilingFeatures=0x" << std::hex
                          << fmtProps.optimalTilingFeatures << ", required=0x"
                          << requiredFeatures << std::dec << ")."
                          << std::endl;
                return nullptr;
            }
        }

        // MSResolveSrc must actually be multi-sampled for a resolve to
        // make sense. The legacy `effectiveSampleCount` formula above
        // only goes >1 if `kind` is Tex2DMS / Tex2DMSArray — older
        // callers may set `usage=MSResolveSrc` without explicitly setting
        // `kind`, leaving samples=1 (which then fails to resolve). Force
        // samples=4 as a sane default in that case.
        if(desc.usage == GETexture::MSResolveSrc && image_desc.samples == VK_SAMPLE_COUNT_1_BIT){
            image_desc.samples = VK_SAMPLE_COUNT_4_BIT;
        }

        image_desc.usage = usageFlags;
        image_desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if(queueFamilyIndices.size() > 1){
            image_desc.sharingMode = VK_SHARING_MODE_CONCURRENT;
            image_desc.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
            image_desc.pQueueFamilyIndices = queueFamilyIndices.data();
        }
        else {
            image_desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_desc.queueFamilyIndexCount = 0;
            image_desc.pQueueFamilyIndices = nullptr;
        }

        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        

        VmaAllocationCreateInfo create_alloc_info {};
        create_alloc_info.usage = memoryUsage;
        VmaAllocation alloc = nullptr;
        VmaAllocationInfo alloc_info;

        auto imageRes = vmaCreateImage(memAllocator,&image_desc,&create_alloc_info,&image,&alloc,&alloc_info);
        if(imageRes != VK_SUCCESS || image == VK_NULL_HANDLE){
            std::cerr << "Vulkan makeTexture failed: vmaCreateImage returned "
                      << vkResultToStr(imageRes) << " (" << imageRes << ")"
                      << " for format=" << image_format
                      << " usage=0x" << std::hex << usageFlags << std::dec
                      << " " << width << "x" << height << "x" << depth
                      << std::endl;
            return nullptr;
        }
        

        VkImageViewCreateInfo image_view_desc {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        image_view_desc.pNext = nullptr;

        image_view_desc.viewType = viewType;
        image_view_desc.image = image;
        image_view_desc.flags = 0;
        image_view_desc.format = image_format;
        // Bake the descriptor's defaultSwizzle into the primary view so
        // every bind without a runtime override pays zero per-frame cost.
        // Texture-swizzle proposal §4 / Open Q1.
        auto bakedSwizzle = [](TextureSwizzleChannel ch, VkComponentSwizzle pos){
            switch(ch){
                case TextureSwizzleChannel::Identity: return pos;
                case TextureSwizzleChannel::Red:      return VK_COMPONENT_SWIZZLE_R;
                case TextureSwizzleChannel::Green:    return VK_COMPONENT_SWIZZLE_G;
                case TextureSwizzleChannel::Blue:     return VK_COMPONENT_SWIZZLE_B;
                case TextureSwizzleChannel::Alpha:    return VK_COMPONENT_SWIZZLE_A;
                case TextureSwizzleChannel::Zero:     return VK_COMPONENT_SWIZZLE_ZERO;
                case TextureSwizzleChannel::One:      return VK_COMPONENT_SWIZZLE_ONE;
            }
            return pos;
        };
        image_view_desc.components.r = bakedSwizzle(desc.defaultSwizzle.r, VK_COMPONENT_SWIZZLE_IDENTITY);
        image_view_desc.components.g = bakedSwizzle(desc.defaultSwizzle.g, VK_COMPONENT_SWIZZLE_IDENTITY);
        image_view_desc.components.b = bakedSwizzle(desc.defaultSwizzle.b, VK_COMPONENT_SWIZZLE_IDENTITY);
        image_view_desc.components.a = bakedSwizzle(desc.defaultSwizzle.a, VK_COMPONENT_SWIZZLE_IDENTITY);
        image_view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_view_desc.subresourceRange.baseMipLevel = 0;
        image_view_desc.subresourceRange.levelCount = mipLevels;
        image_view_desc.subresourceRange.baseArrayLayer = 0;
        image_view_desc.subresourceRange.layerCount = vkArrayLayers;

        auto viewRes = vkCreateImageView(device,&image_view_desc,nullptr,&imageView);
        if(viewRes != VK_SUCCESS || imageView == VK_NULL_HANDLE){
            std::cerr << "Vulkan makeTexture failed: vkCreateImageView returned " << viewRes << std::endl;
            vmaDestroyImage(memAllocator,image,alloc);
            return nullptr;
        }

        // Vulkan-Texture-Memory-Plan Phase 2/3 — allocate the staging
        // companion buffer for `ToGPU` / `FromGPU` and compute the
        // per-mip / per-layer copy regions. Other usages (GPU-only,
        // render targets, MSAA-resolve) don't need staging — they
        // never see CPU writes through `copyBytes`.
        VkBuffer       stagingBuffer    = VK_NULL_HANDLE;
        VmaAllocation  stagingAlloc     = nullptr;
        VkDeviceSize   stagingSize      = 0;
        OmegaCommon::Vector<VkBufferImageCopy> stagingRegions;
        if(desc.usage == GETexture::ToGPU || desc.usage == GETexture::FromGPU){
            // Bytes per texel for the formats `pixelFormatToVkFormat`
            // currently emits. Block-compressed formats land here
            // later (plan Open Q4) and will need a different
            // `bytesPerBlock` × `blocksPerRow` formula; flag in case
            // a new format slips into pixelFormatToVkFormat without
            // updating this switch.
            std::uint32_t bytesPerTexel = 0;
            switch(desc.pixelFormat){
                case PixelFormat::RGBA8Unorm:
                case PixelFormat::RGBA8Unorm_SRGB:
                case PixelFormat::BGRA8Unorm:
                case PixelFormat::BGRA8Unorm_SRGB:
                    bytesPerTexel = 4;
                    break;
                case PixelFormat::RGBA16Unorm:
                    bytesPerTexel = 8;
                    break;
            }
            if(bytesPerTexel == 0){
                std::cerr << "Vulkan makeTexture: ToGPU/FromGPU staging path has no "
                          << "bytes-per-texel mapping for pixelFormat=" << int(desc.pixelFormat)
                          << ". Add the format to the switch in GEVulkan.cpp." << std::endl;
                vkDestroyImageView(device, imageView, nullptr);
                vmaDestroyImage(memAllocator, image, alloc);
                return nullptr;
            }

            // Tightly-packed mip chain on the buffer side: each
            // mip's rows are contiguous, mips are concatenated,
            // layers are appended after the full chain. `extent`
            // halves on each level, floor-clamped to 1.
            stagingRegions.reserve(static_cast<std::size_t>(mipLevels) * vkArrayLayers);
            VkDeviceSize offset = 0;
            for(unsigned layer = 0; layer < vkArrayLayers; ++layer){
                unsigned mipW = width, mipH = height, mipD = depth;
                for(unsigned level = 0; level < mipLevels; ++level){
                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.bufferRowLength = 0;  // tightly packed → infer from imageExtent
                    region.bufferImageHeight = 0;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = level;
                    region.imageSubresource.baseArrayLayer = layer;
                    region.imageSubresource.layerCount = 1;
                    region.imageOffset = {0, 0, 0};
                    region.imageExtent = {mipW, mipH, mipD};
                    stagingRegions.push_back(region);

                    offset += static_cast<VkDeviceSize>(mipW) * mipH * mipD * bytesPerTexel;
                    mipW = mipW > 1 ? mipW >> 1u : 1;
                    mipH = mipH > 1 ? mipH >> 1u : 1;
                    mipD = mipD > 1 ? mipD >> 1u : 1;
                }
            }
            stagingSize = offset;

            VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufInfo.size  = stagingSize;
            // Both src (ToGPU upload) and dst (FromGPU readback) —
            // allocating both flags up-front lets the same buffer
            // serve either direction without re-creating, even
            // though a given texture only ever uses one.
            bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                          | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo bufAllocInfo {};
            bufAllocInfo.usage = (desc.usage == GETexture::ToGPU)
                ? VMA_MEMORY_USAGE_CPU_TO_GPU
                : VMA_MEMORY_USAGE_GPU_TO_CPU;
            bufAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
                               | ((desc.usage == GETexture::ToGPU)
                                    ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                    : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);

            VmaAllocationInfo bufAllocResult {};
            auto bufRes = vmaCreateBuffer(memAllocator, &bufInfo, &bufAllocInfo,
                                          &stagingBuffer, &stagingAlloc, &bufAllocResult);
            if(bufRes != VK_SUCCESS || stagingBuffer == VK_NULL_HANDLE){
                std::cerr << "Vulkan makeTexture: failed to allocate staging buffer ("
                          << vkResultToStr(bufRes) << ", size=" << stagingSize << ")." << std::endl;
                vkDestroyImageView(device, imageView, nullptr);
                vmaDestroyImage(memAllocator, image, alloc);
                return nullptr;
            }
        }

        TextureDescriptor sanitizedDesc = desc;
        sanitizedDesc.width = width;
        sanitizedDesc.height = height;
        sanitizedDesc.depth = depth;
        sanitizedDesc.mipLevels = mipLevels;
        sanitizedDesc.kind = kind;
        sanitizedDesc.arrayLayers = vkArrayLayers;
        sanitizedDesc.sampleCount = effectiveSampleCount;
        auto result = std::shared_ptr<GEVulkanTexture>(new GEVulkanTexture(
                kind,
                sanitizedDesc.usage,
                sanitizedDesc.pixelFormat,
                this,
                image,
                imageView,
                layout,
                alloc_info,
                alloc,
                sanitizedDesc,
                memoryUsage));
        result->format = image_format;
        result->stagingBuffer  = stagingBuffer;
        result->stagingAlloc   = stagingAlloc;
        result->stagingSize    = stagingSize;
        result->stagingRegions = std::move(stagingRegions);
        result->setShape(kind, vkArrayLayers, effectiveSampleCount);
        trackResource(result);
        return result;
    };

    // Vulkan-Texture-Memory-Plan Phase 1. Synchronous one-shot upload of
    // `tex.stagingBuffer` -> `tex.img`. Records a TRANSFER_DST barrier,
    // a vkCmdCopyBufferToImage covering every region in
    // `tex.stagingRegions` (mip0/layer0 in the current single-mip
    // copyBytes path; the regions vector is sized for the whole chain
    // so future per-mip overloads land here unchanged), transitions to
    // SHADER_READ_ONLY_OPTIMAL, and waits on the engine fence. Returns
    // false on any submission failure; the caller logs and leaves the
    // texture in an undefined-contents state (the same visible-glitch
    // contract D3D12's copyBytes failure path established in 5243ead).
    bool GEVulkanEngine::submitImmediateUploadFromStaging(GEVulkanTexture &tex){
        // Default: upload the whole pre-computed mip × layer chain.
        return submitImmediateUploadFromStaging(tex, tex.stagingRegions.data(),
            static_cast<std::uint32_t>(tex.stagingRegions.size()));
    }

    bool GEVulkanEngine::submitImmediateUploadFromStaging(GEVulkanTexture &tex,
                                                          const VkBufferImageCopy *regions,
                                                          std::uint32_t regionCount){
        if(uploadCommandPool == VK_NULL_HANDLE || uploadFence == VK_NULL_HANDLE
           || uploadQueue == VK_NULL_HANDLE){
            DEBUG_STREAM("submitImmediateUploadFromStaging: upload infra unavailable");
            return false;
        }
        if(tex.stagingBuffer == VK_NULL_HANDLE || regions == nullptr || regionCount == 0){
            DEBUG_STREAM("submitImmediateUploadFromStaging: no staging buffer / regions");
            return false;
        }

        std::lock_guard<std::mutex> guard(uploadMutex);

        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbAlloc {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAlloc.commandPool = uploadCommandPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        if(vkAllocateCommandBuffers(device, &cbAlloc, &cb) != VK_SUCCESS || cb == VK_NULL_HANDLE){
            DEBUG_STREAM("submitImmediateUploadFromStaging: vkAllocateCommandBuffers failed");
            return false;
        }

        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS){
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
            return false;
        }

        // `tex.layout` is the canonical tracker — `addResourceBarrierForTextureCopy`,
        // `insertResourceBarrierIfNeeded`, and `startRenderPass` keep it current as
        // the image moves through user-issued copies / draws / passes. Read it
        // directly so the barrier's oldLayout matches the image's actual state
        // even if the user mutated the image via `copyTextureToTexture` between
        // staging calls. (Previously read `tex.stagingCurrentLayout`, a shadow
        // tracker only the staging paths updated — it drifted for FromGPU
        // textures fed by `copyTextureToTexture`.)
        const VkImageLayout oldLayout = tex.layout;
        const std::uint32_t mipLevels  = tex.descriptor.mipLevels > 0 ? tex.descriptor.mipLevels : 1;
        const std::uint32_t layerCount = tex.descriptor.arrayLayers > 0 ? tex.descriptor.arrayLayers : 1;

        // oldLayout -> TRANSFER_DST_OPTIMAL. Broad src stage/access so the
        // barrier covers whichever prior op (shader sample, render-target
        // write, prior transfer) left the image in `oldLayout`.
        VkImageMemoryBarrier toDst {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            ? 0
            : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = oldLayout;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex.img;
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.baseMipLevel = 0;
        toDst.subresourceRange.levelCount = mipLevels;
        toDst.subresourceRange.baseArrayLayer = 0;
        toDst.subresourceRange.layerCount = layerCount;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toDst);

        vkCmdCopyBufferToImage(cb, tex.stagingBuffer, tex.img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            regionCount, regions);

        // TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL. That's the layout
        // the renderer's sampling-bind path expects (matches the
        // `layout` field GEVulkanTexture was constructed with on the
        // ToGPU path).
        VkImageMemoryBarrier toRead = toDst;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);

        if(vkEndCommandBuffer(cb) != VK_SUCCESS){
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
            return false;
        }

        // Fence is reused across calls; reset before submit.
        vkResetFences(device, 1, &uploadFence);

        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        if(vkQueueSubmit(uploadQueue, 1, &submit, uploadFence) != VK_SUCCESS){
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
            return false;
        }

        // Synchronous wait — matches D3D12's copyBytes contract and
        // the Non-Goals §1 explicit choice (async upload is a separate
        // plan). 5-second timeout is a safety net so a wedged GPU
        // doesn't hang the test runner forever; UINT64_MAX would be
        // strictly correct under VkResult semantics but in practice an
        // upload that takes >5s means something has gone fatally wrong
        // upstream and we want to surface it.
        constexpr std::uint64_t kTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;
        VkResult waitRes = vkWaitForFences(device, 1, &uploadFence, VK_TRUE, kTimeoutNs);
        vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);

        if(waitRes != VK_SUCCESS){
            DEBUG_STREAM("submitImmediateUploadFromStaging: vkWaitForFences failed " << waitRes);
            return false;
        }
        // `tex.layout` is the single layout tracker the engine consults
        // everywhere. Subsequent renderer binds and copies read this when
        // emitting barriers, so it must agree with the layout the staging
        // path left the image in.
        tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    // FromGPU mirror — transitions image -> TRANSFER_SRC_OPTIMAL, copies
    // into staging, restores prior layout, waits. Currently only
    // exercised by `getBytes`; `debugReadbackPixelRGBA8` keeps its own
    // self-contained one-shot for now (it predates this infra and
    // doesn't have a GEVulkanTexture-side staging buffer to point at).
    bool GEVulkanEngine::submitImmediateReadbackToStaging(GEVulkanTexture &tex){
        if(uploadCommandPool == VK_NULL_HANDLE || uploadFence == VK_NULL_HANDLE
           || uploadQueue == VK_NULL_HANDLE){
            return false;
        }
        if(tex.stagingBuffer == VK_NULL_HANDLE || tex.stagingRegions.empty()){
            return false;
        }

        std::lock_guard<std::mutex> guard(uploadMutex);

        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbAlloc {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAlloc.commandPool = uploadCommandPool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        if(vkAllocateCommandBuffers(device, &cbAlloc, &cb) != VK_SUCCESS || cb == VK_NULL_HANDLE){
            return false;
        }

        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);

        // Read the canonical `tex.layout` (kept current by every encoder
        // path that mutates this image: `copyTextureToTexture`, shader
        // binds, render-pass start). The previous code consulted
        // `stagingCurrentLayout`, a staging-only shadow tracker that
        // stayed UNDEFINED for FromGPU textures filled via
        // `copyTextureToTexture(rt, readback)` — the resulting
        // "SHADER_READ_ONLY_OPTIMAL best-guess" oldLayout disagreed with
        // the real TRANSFER_DST_OPTIMAL state and the validator caught
        // it (UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout).
        const VkImageLayout priorLayout = tex.layout;
        const std::uint32_t mipLevels  = tex.descriptor.mipLevels > 0 ? tex.descriptor.mipLevels : 1;
        const std::uint32_t layerCount = tex.descriptor.arrayLayers > 0 ? tex.descriptor.arrayLayers : 1;

        VkImageMemoryBarrier toSrc {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        // Broad src access — the prior op could be any of shader-sample,
        // render-target write, or a prior transfer. Stage is already
        // ALL_COMMANDS_BIT below; pair the access mask with the union of
        // memory-read / memory-write so the synchronization scope covers
        // every plausible producer of `priorLayout`.
        toSrc.srcAccessMask = (priorLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            ? 0
            : (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = priorLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = tex.img;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.baseMipLevel = 0;
        toSrc.subresourceRange.levelCount = mipLevels;
        toSrc.subresourceRange.baseArrayLayer = 0;
        toSrc.subresourceRange.layerCount = layerCount;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toSrc);

        vkCmdCopyImageToBuffer(cb, tex.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            tex.stagingBuffer,
            static_cast<std::uint32_t>(tex.stagingRegions.size()),
            tex.stagingRegions.data());

        VkBufferMemoryBarrier toHost {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = tex.stagingBuffer;
        toHost.offset = 0;
        toHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &toHost, 0, nullptr);

        // Restore the prior layout so subsequent draws / samples find
        // the image in the layout they expect.
        VkImageMemoryBarrier toPrior = toSrc;
        toPrior.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toPrior.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toPrior.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPrior.newLayout = priorLayout;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toPrior);

        if(vkEndCommandBuffer(cb) != VK_SUCCESS){
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
            return false;
        }

        vkResetFences(device, 1, &uploadFence);

        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        if(vkQueueSubmit(uploadQueue, 1, &submit, uploadFence) != VK_SUCCESS){
            vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
            return false;
        }

        constexpr std::uint64_t kTimeoutNs = 5ull * 1000ull * 1000ull * 1000ull;
        VkResult waitRes = vkWaitForFences(device, 1, &uploadFence, VK_TRUE, kTimeoutNs);
        vkFreeCommandBuffers(device, uploadCommandPool, 1, &cb);
        if(waitRes != VK_SUCCESS){
            return false;
        }
        // priorLayout was already `tex.layout` going in; the toPrior
        // barrier restored the image to it. No tracker update needed —
        // included as a no-op for symmetry / future-proofing if the
        // staging path is ever rewritten to leave the image in a
        // different post-readback layout.
        tex.layout = priorLayout;
        return true;
    }


    inline VkSamplerAddressMode convertAddressMode(const omegasl_shader_static_sampler_address_mode & addressMode){
        switch (addressMode) {
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP : {
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR : {
                return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            }
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP : {
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            }
            case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE : {
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
        }
    }

    inline VkSamplerAddressMode convertAddressMode(const SamplerDescriptor::AddressMode & addressMode){
        switch (addressMode) {
            case SamplerDescriptor::AddressMode::Wrap : {
                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
            case SamplerDescriptor::AddressMode::MirrorWrap : {
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            }
            case SamplerDescriptor::AddressMode::MirrorClampToEdge : {
                return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            }
            case SamplerDescriptor::AddressMode::ClampToEdge : {
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
        }
    }

    VkPipelineLayout GEVulkanEngine::createPipelineLayoutFromShaderDescs(unsigned shaderN,
                                                                         omegasl_shader *shaders,
                                                                         VkDescriptorPool * descriptorPool,
                                                                         OmegaCommon::Vector<VkDescriptorSet> & descs,
                                                                         OmegaCommon::Vector<VkDescriptorSetLayout> & descLayout,
                                                                         OmegaCommon::Vector<VkSampler> & outImmutableSamplers){
        if(descriptorPool != nullptr){
            *descriptorPool = VK_NULL_HANDLE;
        }
        descs.clear();
        descLayout.clear();
        outImmutableSamplers.clear();

        VkPipelineLayoutCreateInfo layout_info {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pNext = nullptr;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;

        OmegaCommon::ArrayRef<omegasl_shader> shadersArr {shaders,shaders + shaderN};

        OmegaCommon::Vector<VkDescriptorPoolSize> poolSizes;
        std::uint32_t setCount = 0;

        // Pre-count static samplers across all shaders so we can reserve
        // the vector and avoid pointer invalidation from reallocation.
        {
            std::size_t totalStaticSamplers = 0;
            for(auto & s : shadersArr){
                OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layouts {s.pLayout,s.pLayout + s.nLayout};
                for(auto & l : layouts){
                    if(l.type == OMEGASL_SHADER_STATIC_SAMPLER1D_DESC ||
                       l.type == OMEGASL_SHADER_STATIC_SAMPLER2D_DESC ||
                       l.type == OMEGASL_SHADER_STATIC_SAMPLER3D_DESC){
                        ++totalStaticSamplers;
                    }
                }
            }
            outImmutableSamplers.reserve(totalStaticSamplers);
        }

        // §2.2 push constant — a pipeline binds at most one push-constant
        // block, shared by whichever stages declared `[in pc]`. Push constants
        // are NOT descriptors (the switch below routes them to
        // `default: continue`, so they never enter a descriptor-set layout);
        // they become a single VkPushConstantRange on the pipeline layout with
        // the union of using stages. `pushRange` is function-scoped so it
        // outlives the vkCreatePipelineLayout call below.
        VkPushConstantRange pushRange {};
        {
            VkShaderStageFlags pushStages = 0;
            bool hasPushConstant = false;
            uint32_t pushSize = 0; // §2.4 — max sized push block across stages
            for(auto & s : shadersArr){
                OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layouts {s.pLayout,s.pLayout + s.nLayout};
                for(auto & l : layouts){
                    if(l.type == OMEGASL_SHADER_PUSH_CONSTANT_DESC){
                        VkShaderStageFlags stage = VK_SHADER_STAGE_COMPUTE_BIT;
                        if(s.type == OMEGASL_SHADER_VERTEX){ stage = VK_SHADER_STAGE_VERTEX_BIT; }
                        else if(s.type == OMEGASL_SHADER_FRAGMENT){ stage = VK_SHADER_STAGE_FRAGMENT_BIT; }
                        /// §16 Phase G — tessellation stages. A hull is the
                        /// tessellation-control stage; a domain is the
                        /// tessellation-evaluation stage. Both are core Vulkan.
                        else if(s.type == OMEGASL_SHADER_HULL){ stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; }
                        else if(s.type == OMEGASL_SHADER_DOMAIN){ stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; }
                    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
                        else if(s.type == OMEGASL_SHADER_MESH){ stage = VK_SHADER_STAGE_MESH_BIT_EXT; }
                        /// §5 — the amplification stage is Vulkan's TASK stage.
                        /// This is what lets a `constant<T>` declared `[in pc]`
                        /// on BOTH the amplification and the mesh shader reach
                        /// both: push constants are a single range whose
                        /// `stageFlags` is the union over every stage that
                        /// declared the block, so the task bit has to be in that
                        /// union or `vkCmdPushConstants` never reaches the amp.
                        else if(s.type == OMEGASL_SHADER_AMPLIFICATION){ stage = VK_SHADER_STAGE_TASK_BIT_EXT; }
                    #endif
                        pushStages |= stage;
                        hasPushConstant = true;
                        // §2.4 — the compiler carries the block's std140 byte
                        // size in `offset` (already a multiple of 16). Take the
                        // max across stages that declare the shared block.
                        if((uint32_t)l.offset > pushSize){ pushSize = (uint32_t)l.offset; }
                    }
                }
            }
            if(hasPushConstant){
                // §2.4 — size the range to the compiler-sized block; fall back
                // to the portable 128-byte cap for a size-0 (legacy / unsizable)
                // block. offset 0; partial updates use the vkCmdPushConstants
                // offset at command time.
                pushRange.stageFlags = pushStages;
                pushRange.offset = 0;
                pushRange.size = pushSize ? ((pushSize + 3u) & ~3u) : 128;
                layout_info.pushConstantRangeCount = 1;
                layout_info.pPushConstantRanges = &pushRange;
            }
        }

        for(auto & s : shadersArr){
            VkShaderStageFlags shaderStageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            if(s.type == OMEGASL_SHADER_VERTEX){
                shaderStageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            }
            else if(s.type == OMEGASL_SHADER_FRAGMENT){
                shaderStageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            /// §16 Phase G — a hull/domain descriptor binding must be visible
            /// to the tessellation-control / -evaluation stage that uses it.
            else if(s.type == OMEGASL_SHADER_HULL){
                shaderStageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            }
            else if(s.type == OMEGASL_SHADER_DOMAIN){
                shaderStageFlags = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            }
        #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
            /// Phase 4a — descriptor-set bindings used by the mesh stage
            /// need `VK_SHADER_STAGE_MESH_BIT_EXT` in their stage flags;
            /// without it the descriptor is invisible to the mesh
            /// shader at draw time and the validator complains. Mesh
            /// resources reuse the same set 0 the vertex stage owns
            /// (slot-doubling per 4c.1), so `bindResourceAtVertexShader`
            /// continues to write to the right place — only the layout
            /// needs to know which stage to expose it to.
            else if(s.type == OMEGASL_SHADER_MESH){
                shaderStageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
            }
            /// §5 — descriptor bindings used by the amplification stage need
            /// `VK_SHADER_STAGE_TASK_BIT_EXT`, or the descriptor is invisible to
            /// the task shader at draw time and the validator says so.
            ///
            /// The amplification shader's set index is NOT arbitrary: this loop
            /// creates one descriptor set per shader IN ARRAY ORDER, and
            /// `bindResourceAtVertexShader` / `bindResourceAtFragmentShader`
            /// hardcode sets 0 and 1. `makeMeshPipelineState` therefore passes
            /// `{mesh, fragment, amplification}` — appending the amp at the TAIL
            /// keeps mesh at set 0 and fragment at set 1 exactly where every
            /// existing bind expects them, and puts the amp at set 2, which is
            /// what `bindResourceAtAmplificationShader` writes.
            else if(s.type == OMEGASL_SHADER_AMPLIFICATION){
                shaderStageFlags = VK_SHADER_STAGE_TASK_BIT_EXT;
            }
        #endif

            OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layouts {s.pLayout,s.pLayout + s.nLayout};
            OmegaCommon::Vector<VkDescriptorSetLayoutBinding> bindings;
            bindings.reserve(layouts.size());

            for(auto & l : layouts){
                VkDescriptorSetLayoutBinding binding {};
                binding.binding = l.gpu_relative_loc;
                binding.descriptorCount = 1;
                binding.stageFlags = shaderStageFlags;
                binding.pImmutableSamplers = nullptr;

                switch (l.type) {
                    case OMEGASL_SHADER_BUFFER_DESC: {
                        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        break;
                    }
                    case OMEGASL_SHADER_UNIFORM_DESC: {
                        /// §2.4 constant buffer — `layout(std140) uniform`.
                        binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                        break;
                    }
                    case OMEGASL_SHADER_SAMPLER1D_DESC:
                    case OMEGASL_SHADER_SAMPLER2D_DESC:
                    case OMEGASL_SHADER_SAMPLER3D_DESC:
                    case OMEGASL_SHADER_SAMPLERCUBE_DESC: {
                        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                        break;
                    }
                    case OMEGASL_SHADER_STATIC_SAMPLER1D_DESC:
                    case OMEGASL_SHADER_STATIC_SAMPLER2D_DESC:
                    case OMEGASL_SHADER_STATIC_SAMPLER3D_DESC:
                    case OMEGASL_SHADER_STATIC_SAMPLERCUBE_DESC: {
                        // GLSL codegen emits `uniform sampler` (bare sampler),
                        // so the descriptor type must be VK_DESCRIPTOR_TYPE_SAMPLER.
                        // Create an immutable VkSampler from the static sampler
                        // description and bake it into the descriptor set layout.
                        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

                        VkSamplerCreateInfo sci {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                        sci.pNext = nullptr;
                        sci.addressModeU = convertAddressMode(l.sampler_desc.u_address_mode);
                        sci.addressModeV = convertAddressMode(l.sampler_desc.v_address_mode);
                        sci.addressModeW = convertAddressMode(l.sampler_desc.w_address_mode);
                        sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                        sci.unnormalizedCoordinates = VK_FALSE;
                        sci.compareEnable = VK_FALSE;
                        sci.mipLodBias = 0.f;
                        sci.minLod = 0.f;
                        sci.maxLod = VK_LOD_CLAMP_NONE;
                        sci.anisotropyEnable = VK_FALSE;
                        sci.maxAnisotropy = 1.f;
                        switch(l.sampler_desc.filter){
                            case OMEGASL_SHADER_SAMPLER_LINEAR_FILTER:
                                sci.magFilter = VK_FILTER_LINEAR;
                                sci.minFilter = VK_FILTER_LINEAR;
                                sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                                break;
                            case OMEGASL_SHADER_SAMPLER_POINT_FILTER:
                                sci.magFilter = VK_FILTER_NEAREST;
                                sci.minFilter = VK_FILTER_NEAREST;
                                sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                                break;
                            case OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER:
                            case OMEGASL_SHADER_SAMPLER_MIN_ANISOTROPY_FILTER:
                                sci.magFilter = VK_FILTER_LINEAR;
                                sci.minFilter = VK_FILTER_LINEAR;
                                sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                                sci.anisotropyEnable = VK_TRUE;
                                sci.maxAnisotropy = static_cast<float>(l.sampler_desc.max_anisotropy);
                                break;
                        }
                        VkSampler sampler = VK_NULL_HANDLE;
                        auto samplerRes = vkCreateSampler(device,&sci,nullptr,&sampler);
                        if(samplerRes == VK_SUCCESS && sampler != VK_NULL_HANDLE){
                            outImmutableSamplers.push_back(sampler);
                            // Safe: vector was pre-reserved, no reallocation.
                            binding.pImmutableSamplers = &outImmutableSamplers.back();
                        }
                        break;
                    }
                    case OMEGASL_SHADER_TEXTURE1D_DESC:
                    case OMEGASL_SHADER_TEXTURE2D_DESC:
                    case OMEGASL_SHADER_TEXTURE3D_DESC:
                    /// OmegaSL §2.1 Phase A — descriptor type bucket is the
                    /// same SAMPLED_IMAGE/STORAGE_IMAGE; Phase B will pick the
                    /// proper `VkImageViewType` (CUBE / 2D_ARRAY / …) when the
                    /// texture is created and bound.
                    case OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC:
                    case OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC:
                    case OMEGASL_SHADER_TEXTURECUBE_DESC:
                    case OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC:
                    case OMEGASL_SHADER_TEXTURE2D_MS_DESC:
                    case OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC: {
                        binding.descriptorType = l.io_mode == OMEGASL_SHADER_DESC_IO_IN
                                                 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        break;
                    }
                    default:
                        continue;
                }

                bindings.push_back(binding);

                bool merged = false;
                for(auto &poolSize : poolSizes){
                    if(poolSize.type == binding.descriptorType){
                        poolSize.descriptorCount += binding.descriptorCount;
                        merged = true;
                        break;
                    }
                }
                if(!merged){
                    VkDescriptorPoolSize poolSize {};
                    poolSize.type = binding.descriptorType;
                    poolSize.descriptorCount = binding.descriptorCount;
                    poolSizes.push_back(poolSize);
                }
            }

            VkDescriptorSetLayoutCreateInfo desc_layout_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            desc_layout_info.pNext = nullptr;
            desc_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            desc_layout_info.pBindings = bindings.empty() ? nullptr : bindings.data();
            // Vulkan permits at most one push-descriptor set per pipeline
            // layout (VUID-VkPipelineLayoutCreateInfo-pSetLayouts-00293),
            // so only set 0 (vertex) gets the push flag. Set 1+ (fragment)
            // is a regular descriptor set; the recorder allocates fresh
            // ones per draw from a FREE_DESCRIPTOR_SET-capable pool to
            // avoid the "descriptor set updated while bound" hazard.
            if(hasPushDescriptorExt && setCount == 0){
                desc_layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            }

            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            auto setLayoutRes = vkCreateDescriptorSetLayout(device,&desc_layout_info,nullptr,&setLayout);
            if(setLayoutRes != VK_SUCCESS || setLayout == VK_NULL_HANDLE){
                std::cerr << "Vulkan descriptor set layout creation failed (" << setLayoutRes << ")" << std::endl;
                continue;
            }

            descLayout.push_back(setLayout);
            setCount += 1;
        }

        layout_info.pSetLayouts = descLayout.empty() ? nullptr : descLayout.data();
        layout_info.setLayoutCount = static_cast<std::uint32_t>(descLayout.size());

        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        auto layoutRes = vkCreatePipelineLayout(device,&layout_info,nullptr,&pipeline_layout);
        if(layoutRes != VK_SUCCESS || pipeline_layout == VK_NULL_HANDLE){
            std::cerr << "Vulkan pipeline layout creation failed (" << layoutRes << ")" << std::endl;
            return VK_NULL_HANDLE;
        }

        if(descriptorPool == nullptr || descLayout.empty() || poolSizes.empty()){
            return pipeline_layout;
        }

        // Determine which layout slots need a regular descriptor pool. In
        // push-descriptor mode, set 0 is push and skipped. In fallback
        // mode, every set is regular. The pool is created with
        // FREE_DESCRIPTOR_SET_BIT and oversized so the command-buffer
        // recorder can allocate fresh sets per draw and return them when
        // the buffer is destroyed/reset, avoiding "descriptor set updated
        // while bound" violations on shared in-place sets.
        const std::size_t nonPushSetCount = hasPushDescriptorExt
            ? (descLayout.size() > 1 ? descLayout.size() - 1 : 0)
            : descLayout.size();
        if(nonPushSetCount == 0){
            return pipeline_layout;
        }

        constexpr std::uint32_t kFallbackPoolSlots = 256;
        VkDescriptorPoolCreateInfo poolCreateInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolCreateInfo.maxSets = kFallbackPoolSlots * static_cast<std::uint32_t>(nonPushSetCount);
        OmegaCommon::Vector<VkDescriptorPoolSize> scaledPoolSizes = poolSizes;
        for(auto & ps : scaledPoolSizes){
            ps.descriptorCount *= kFallbackPoolSlots;
        }
        poolCreateInfo.poolSizeCount = static_cast<std::uint32_t>(scaledPoolSizes.size());
        poolCreateInfo.pPoolSizes = scaledPoolSizes.data();

        auto poolRes = vkCreateDescriptorPool(device,&poolCreateInfo,nullptr,descriptorPool);
        if(poolRes != VK_SUCCESS || *descriptorPool == VK_NULL_HANDLE){
            std::cerr << "Vulkan descriptor pool creation failed (" << poolRes << ")" << std::endl;
            return pipeline_layout;
        }

        // Seed-allocate one descriptor set per non-push slot. These act
        // as a last-resort fallback if the recorder ever fails to allocate
        // a fresh ring slot (pool exhausted). `descs[i]` corresponds to
        // layout slot `descLayout[i + pushOffset]`.
        const std::size_t pushOffset = hasPushDescriptorExt ? 1u : 0u;
        VkDescriptorSetAllocateInfo descSetAllocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descSetAllocInfo.descriptorSetCount = static_cast<std::uint32_t>(nonPushSetCount);
        descSetAllocInfo.pSetLayouts = &descLayout[pushOffset];
        descSetAllocInfo.pNext = nullptr;
        descSetAllocInfo.descriptorPool = *descriptorPool;

        descs.resize(nonPushSetCount);
        auto allocRes = vkAllocateDescriptorSets(device,&descSetAllocInfo,descs.data());
        if(allocRes != VK_SUCCESS){
            std::cerr << "Vulkan descriptor set allocation failed (" << allocRes << ")" << std::endl;
            descs.clear();
        }

        return pipeline_layout;

    }

    inline VkCompareOp convertCompareFunc(CompareFunc & func){
        VkCompareOp res;
        switch (func) {
            case CompareFunc::Less : {
                res = VK_COMPARE_OP_LESS;
                break;
            }
            case CompareFunc::LessEqual : {
                res = VK_COMPARE_OP_LESS_OR_EQUAL;
                break;
            }
            case CompareFunc::Greater : {
                res = VK_COMPARE_OP_GREATER;
                break;
            }
            case CompareFunc::GreaterEqual : {
                res = VK_COMPARE_OP_GREATER_OR_EQUAL;
                break;
            }
        }
        return res;
    }

    inline VkFormat convertVertexFormatToVk(VertexFormat fmt){
        switch(fmt){
            case VertexFormat::Float:    return VK_FORMAT_R32_SFLOAT;
            case VertexFormat::Float2:   return VK_FORMAT_R32G32_SFLOAT;
            case VertexFormat::Float3:   return VK_FORMAT_R32G32B32_SFLOAT;
            case VertexFormat::Float4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
            case VertexFormat::Int:      return VK_FORMAT_R32_SINT;
            case VertexFormat::Int2:     return VK_FORMAT_R32G32_SINT;
            case VertexFormat::Int3:     return VK_FORMAT_R32G32B32_SINT;
            case VertexFormat::Int4:     return VK_FORMAT_R32G32B32A32_SINT;
            case VertexFormat::UInt:     return VK_FORMAT_R32_UINT;
            case VertexFormat::UInt2:    return VK_FORMAT_R32G32_UINT;
            case VertexFormat::UInt3:    return VK_FORMAT_R32G32B32_UINT;
            case VertexFormat::UInt4:    return VK_FORMAT_R32G32B32A32_UINT;
            case VertexFormat::UNorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::SNorm8x4: return VK_FORMAT_R8G8B8A8_SNORM;
            case VertexFormat::UShort2:  return VK_FORMAT_R16G16_UINT;
            case VertexFormat::UShort4:  return VK_FORMAT_R16G16B16A16_UINT;
            case VertexFormat::Half2:    return VK_FORMAT_R16G16_SFLOAT;
            case VertexFormat::Half4:    return VK_FORMAT_R16G16B16A16_SFLOAT;
        }
        return VK_FORMAT_UNDEFINED;
    }

    inline VkBlendFactor convertBlendFactorVk(BlendFactor f){
        switch(f){
            case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
            case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
            case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
            case BlendFactor::InvSrcColor:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
            case BlendFactor::InvSrcAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case BlendFactor::DestColor:        return VK_BLEND_FACTOR_DST_COLOR;
            case BlendFactor::InvDestColor:     return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case BlendFactor::DestAlpha:        return VK_BLEND_FACTOR_DST_ALPHA;
            case BlendFactor::InvDestAlpha:     return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case BlendFactor::SrcAlphaSaturated:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            case BlendFactor::Src1Color:        return VK_BLEND_FACTOR_SRC1_COLOR;
            case BlendFactor::InvSrc1Color:     return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
            case BlendFactor::Src1Alpha:        return VK_BLEND_FACTOR_SRC1_ALPHA;
            case BlendFactor::InvSrc1Alpha:     return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        }
        return VK_BLEND_FACTOR_ONE;
    }

    inline VkBlendOp convertBlendOperationVk(BlendOperation op){
        switch(op){
            case BlendOperation::Add:             return VK_BLEND_OP_ADD;
            case BlendOperation::Subtract:        return VK_BLEND_OP_SUBTRACT;
            case BlendOperation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case BlendOperation::Min:             return VK_BLEND_OP_MIN;
            case BlendOperation::Max:             return VK_BLEND_OP_MAX;
        }
        return VK_BLEND_OP_ADD;
    }

    inline VkColorComponentFlags convertColorWriteMaskVk(uint8_t mask){
        VkColorComponentFlags res = 0;
        if(mask & ColorWriteRed)   res |= VK_COLOR_COMPONENT_R_BIT;
        if(mask & ColorWriteGreen) res |= VK_COLOR_COMPONENT_G_BIT;
        if(mask & ColorWriteBlue)  res |= VK_COLOR_COMPONENT_B_BIT;
        if(mask & ColorWriteAlpha) res |= VK_COLOR_COMPONENT_A_BIT;
        return res;
    }

    inline VkStencilOp convertStencilOp(StencilOperation & op){
        VkStencilOp res;
        switch (op) {
            case StencilOperation::Retain : {
                res = VK_STENCIL_OP_KEEP;
                break;
            }
            case StencilOperation::Replace : {
                res = VK_STENCIL_OP_REPLACE;
                break;
            }
            case StencilOperation::Zero : {
                res = VK_STENCIL_OP_ZERO;
                break;
            }
        }
        return res;
    }


    SharedHandle<GERenderPipelineState> GEVulkanEngine::makeRenderPipelineState(RenderPipelineDescriptor &desc){
        if(!_checkPipelineShader(desc.vertexFunc,"vertex",desc.name) ||
           !_checkPipelineShader(desc.fragmentFunc,"fragment",desc.name)){
            return nullptr;
        }

        /// §16 Phase G — a tessellation pipeline is one that carries both a
        /// hull and a domain stage. On Vulkan the HS/DS run inside the one
        /// graphics pipeline (vertex → tess-control → tess-evaluation →
        /// fragment), so the vertex stage — authored as `vertex(tess=true)` —
        /// is required exactly as for a plain pipeline; the checks above cover
        /// it. Feature-gate on `GTEDEVICE_FEATURE_TESSELLATION_SHADER` (matching
        /// the mesh / raytracing pattern) so a device that cannot honor it
        /// returns nullptr with a diagnostic rather than tripping a driver
        /// assert later.
        const bool isTess = (bool)desc.hullFunc && (bool)desc.domainFunc;
        if(isTess){
            if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_TESSELLATION_SHADER)){
                DEBUG_STREAM("makeRenderPipelineState: device does not advertise "
                             "GTEDEVICE_FEATURE_TESSELLATION_SHADER ('" << desc.name << "')");
                return nullptr;
            }
            if(!_checkPipelineShader(desc.hullFunc,"hull",desc.name) ||
               !_checkPipelineShader(desc.domainFunc,"domain",desc.name)){
                return nullptr;
            }
        }

        /// Shader-descriptor set for the pipeline layout. A plain pipeline
        /// unions {vertex, fragment}; a tessellation pipeline additionally
        /// exposes {hull, domain} so their descriptor bindings land in the
        /// layout with the right stage flags (see the two stage-flag switches
        /// in `createPipelineLayoutFromShaderDescs`).
        omegasl_shader shaders[4] = {desc.vertexFunc->internal, desc.fragmentFunc->internal};
        unsigned shaderCount = 2;
        if(isTess){
            shaders[0] = desc.vertexFunc->internal;
            shaders[1] = desc.hullFunc->internal;
            shaders[2] = desc.domainFunc->internal;
            shaders[3] = desc.fragmentFunc->internal;
            shaderCount = 4;
        }

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;

        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        OmegaCommon::Vector<VkSampler> immutableSamplers;

        VkPipelineLayout layout = createPipelineLayoutFromShaderDescs(shaderCount,shaders,&descriptorPool,descs,descLayouts,immutableSamplers);
        if(layout == VK_NULL_HANDLE){
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        const unsigned colorFormatCount = desc.colorPixelFormats.empty()
                                              ? 1u
                                              : (unsigned)desc.colorPixelFormats.size();

        OmegaCommon::Vector<VkAttachmentDescription> colorAttachments;
        OmegaCommon::Vector<VkAttachmentReference> colorRefs;
        colorAttachments.resize(colorFormatCount);
        colorRefs.resize(colorFormatCount);
        for(unsigned i = 0; i < colorFormatCount; ++i){
            VkAttachmentDescription & a = colorAttachments[i];
            a = {};
            a.format = pixelFormatToVkFormat(desc.colorPixelFormats.empty()
                                                 ? PixelFormat::RGBA8Unorm
                                                 : desc.colorPixelFormats[i]);
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }

        VkSubpassDescription subpassDesc {};
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.colorAttachmentCount = colorFormatCount;
        subpassDesc.pColorAttachments = colorRefs.data();

        // Dependencies MUST be byte-identical to the command-queue render pass
        // (GEVulkanCommandBuffer::startRenderPass) — render-pass compatibility
        // for pipeline binding requires identical VkSubpassDependency entries
        // (VUID-vkCmdDraw-renderPass-02684).
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = colorFormatCount;
        renderPassInfo.pAttachments = colorAttachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpassDesc;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        VkRenderPass compatibilityRenderPass = VK_NULL_HANDLE;
        auto renderPassRes = vkCreateRenderPass(device,&renderPassInfo,nullptr,&compatibilityRenderPass);
        if(renderPassRes != VK_SUCCESS || compatibilityRenderPass == VK_NULL_HANDLE){
            std::cerr << "Vulkan render pass creation failed (" << renderPassRes << ")" << std::endl;
            vkDestroyPipelineLayout(device,layout,nullptr);
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        VkGraphicsPipelineCreateInfo createInfo {};
        createInfo.basePipelineHandle = VK_NULL_HANDLE;
        createInfo.basePipelineIndex = -1;
        createInfo.layout = layout;
        createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createInfo.renderPass = compatibilityRenderPass;
        createInfo.subpass = 0;

        OmegaCommon::Vector<VkVertexInputBindingDescription>   vkBindings;
        OmegaCommon::Vector<VkVertexInputAttributeDescription> vkAttributes;
        if(!desc.vertexInputDescriptor.attributes.empty()){
            vkBindings.reserve(desc.vertexInputDescriptor.bufferLayouts.size());
            for(unsigned i = 0; i < desc.vertexInputDescriptor.bufferLayouts.size(); ++i){
                const auto & bl = desc.vertexInputDescriptor.bufferLayouts[i];
                VkVertexInputBindingDescription bd {};
                bd.binding   = i;
                bd.stride    = bl.stride;
                bd.inputRate = (bl.stepFunction == VertexStepFunction::PerInstance)
                                 ? VK_VERTEX_INPUT_RATE_INSTANCE
                                 : VK_VERTEX_INPUT_RATE_VERTEX;
                vkBindings.push_back(bd);
            }
            vkAttributes.reserve(desc.vertexInputDescriptor.attributes.size());
            for(const auto & a : desc.vertexInputDescriptor.attributes){
                VkVertexInputAttributeDescription ad {};
                ad.binding  = a.bufferIndex;
                ad.location = a.shaderLocation;
                ad.format   = convertVertexFormatToVk(a.format);
                ad.offset   = a.offset;
                vkAttributes.push_back(ad);
            }
        }

        VkPipelineVertexInputStateCreateInfo vertexInputState {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInputState.vertexBindingDescriptionCount   = (uint32_t)vkBindings.size();
        vertexInputState.pVertexBindingDescriptions      = vkBindings.empty() ? nullptr : vkBindings.data();
        vertexInputState.vertexAttributeDescriptionCount = (uint32_t)vkAttributes.size();
        vertexInputState.pVertexAttributeDescriptions    = vkAttributes.empty() ? nullptr : vkAttributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssemblyState.primitiveRestartEnable = VK_FALSE;
        if(isTess){
            /// §16 Phase G — the tessellator consumes control-point patches;
            /// the input assembler must feed it a patch list. The per-patch
            /// control-point count is set on `pTessellationState` below.
            inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        }
        else switch(desc.primitiveTopologyCategory){
            case PrimitiveTopologyCategory::Line:
                inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                break;
            case PrimitiveTopologyCategory::Point:
                inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                break;
            case PrimitiveTopologyCategory::Triangle:
            default:
                inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                break;
        }

        /// Rasterizer State
        VkPipelineRasterizationStateCreateInfo rasterState {};
        rasterState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterState.depthClampEnable = VK_FALSE;
        rasterState.rasterizerDiscardEnable = VK_FALSE;
        rasterState.depthBiasEnable = VK_FALSE;
        rasterState.lineWidth = 1.0f;
        switch(desc.cullMode){
            case RasterCullMode::None : {
                rasterState.cullMode = VK_CULL_MODE_NONE;
                break;
            }
            case RasterCullMode::Front : {
                rasterState.cullMode = VK_CULL_MODE_FRONT_BIT;
                break;
            }
            case RasterCullMode::Back : {
                rasterState.cullMode = VK_CULL_MODE_BACK_BIT;
                break;
            }
        }

        if(desc.triangleFillMode == TriangleFillMode::Wireframe){
            rasterState.polygonMode = VK_POLYGON_MODE_LINE;
        }
        else {
            rasterState.polygonMode = VK_POLYGON_MODE_FILL;
        }

        /// Vulkan render-parity — front-face / winding normalization.
        /// `GEVulkanCommandBuffer::setViewports` (GEVulkanCommandQueue.cpp:1433)
        /// binds a NEGATIVE-height viewport to flip NDC-Y so `y=+1` maps to the
        /// framebuffer top, matching Metal/D3D12 (NDC coords themselves get no
        /// special treatment). That viewport flip REVERSES the apparent triangle
        /// winding the rasterizer sees, so applying `polygonFrontFaceRotation`
        /// straight through would classify the opposite faces as front on Vulkan
        /// vs Metal/D3D12. Compensate with the OPPOSITE VkFrontFace: the same
        /// `polygonFrontFaceRotation` then classifies the same PHYSICAL faces as
        /// front on every backend, keeping cull results AND `gl_FrontFacing` /
        /// two-sided stencil consistent cross-backend. Shared by the graphics
        /// and tessellation (isTess) pipelines; the mesh pipeline applies the
        /// same flip. Currently latent — every pipeline uses `cullMode=None` —
        /// but crucial the moment culling is enabled. (Alex, 2026-07-06.)
        if(desc.polygonFrontFaceRotation == GTEPolygonFrontFaceRotation::Clockwise){
            rasterState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
        else {
            rasterState.frontFace = VK_FRONT_FACE_CLOCKWISE;
        }

        VkViewport viewport {};
        viewport.x = 0.f;
        viewport.y = 0.f;
        viewport.width = 1.f;
        viewport.height = 1.f;
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;

        VkRect2D scissor {};
        scissor.offset = {0,0};
        scissor.extent = {1,1};

        VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        switch(desc.rasterSampleCount){
            case 2:
                sampleCount = VK_SAMPLE_COUNT_2_BIT;
                break;
            case 4:
                sampleCount = VK_SAMPLE_COUNT_4_BIT;
                break;
            case 8:
                sampleCount = VK_SAMPLE_COUNT_8_BIT;
                break;
            case 16:
                sampleCount = VK_SAMPLE_COUNT_16_BIT;
                break;
            case 32:
                sampleCount = VK_SAMPLE_COUNT_32_BIT;
                break;
            case 64:
                sampleCount = VK_SAMPLE_COUNT_64_BIT;
                break;
            default:
                sampleCount = VK_SAMPLE_COUNT_1_BIT;
                break;
        }

        VkPipelineMultisampleStateCreateInfo multisampleState {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampleState.rasterizationSamples = sampleCount;
        multisampleState.sampleShadingEnable = VK_FALSE;

        OmegaCommon::Vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        {
            // One blend attachment per color target. When the descriptor supplies
            // no blend state, the attachment is configured for opaque writes.
            size_t attachmentCount = colorFormatCount;
            colorBlendAttachments.resize(attachmentCount);
            for(size_t i = 0; i < attachmentCount; ++i){
                VkPipelineColorBlendAttachmentState & att = colorBlendAttachments[i];
                att = {};
                if(i < desc.colorBlendDescriptors.size()){
                    const auto & b = desc.colorBlendDescriptors[i];
                    att.blendEnable         = b.blendEnabled ? VK_TRUE : VK_FALSE;
                    att.srcColorBlendFactor = convertBlendFactorVk(b.srcColorFactor);
                    att.dstColorBlendFactor = convertBlendFactorVk(b.destColorFactor);
                    att.colorBlendOp        = convertBlendOperationVk(b.colorOp);
                    att.srcAlphaBlendFactor = convertBlendFactorVk(b.srcAlphaFactor);
                    att.dstAlphaBlendFactor = convertBlendFactorVk(b.destAlphaFactor);
                    att.alphaBlendOp        = convertBlendOperationVk(b.alphaOp);
                    att.colorWriteMask      = convertColorWriteMaskVk(b.writeMask);
                }
                else {
                    att.blendEnable = VK_FALSE;
                    att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                    att.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                    att.colorBlendOp = VK_BLEND_OP_ADD;
                    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                    att.alphaBlendOp = VK_BLEND_OP_ADD;
                    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                         VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT |
                                         VK_COLOR_COMPONENT_A_BIT;
                }
            }
        }

        VkPipelineColorBlendStateCreateInfo colorBlendState {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlendState.logicOpEnable = VK_FALSE;
        colorBlendState.attachmentCount = (uint32_t)colorBlendAttachments.size();
        colorBlendState.pAttachments = colorBlendAttachments.data();

        VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        OmegaCommon::Vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        /// §16 Phase G — a tessellation pipeline keeps its topology static at
        /// `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`; `drawPatches` never rebinds it
        /// (and dynamic topology would let a caller feed a non-patch topology
        /// into the tessellator, which is invalid). Only plain pipelines opt
        /// into dynamic topology.
        if(hasExtendedDynamicState && !isTess){
            dynamicStates.push_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT);
        }
        dynamicState.dynamicStateCount = dynamicStates.size();
        dynamicState.pNext = nullptr;
        dynamicState.pDynamicStates = dynamicStates.data();


        auto *vertexShader = (GTEVulkanShader *)desc.vertexFunc.get();
        VkPipelineShaderStageCreateInfo vertexStage {};
        vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertexStage.pNext = nullptr;
        vertexStage.flags = 0;
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexShader->shaderModule;
        vertexStage.pName = "main";
        vertexStage.pSpecializationInfo = nullptr;

        auto *fragmentShader = (GTEVulkanShader *)desc.fragmentFunc.get();
        VkPipelineShaderStageCreateInfo fragmentStage {};
        fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragmentStage.pNext = nullptr;
        fragmentStage.flags = 0;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentShader->shaderModule;
        fragmentStage.pName = "main";
        fragmentStage.pSpecializationInfo = nullptr;

        /// §16 Phase G — tessellation stages. When present, the pipeline runs
        /// vertex → tess-control (hull) → tess-evaluation (domain) → fragment;
        /// `pTessellationState.patchControlPoints` matches the hull's
        /// `outputcontrolpoints` (read from the serialized tessellation
        /// descriptor, falling back to the descriptor's convenience field).
        VkPipelineShaderStageCreateInfo tessControlStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        VkPipelineShaderStageCreateInfo tessEvalStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        VkPipelineTessellationStateCreateInfo tessState {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        uint32_t patchControlPoints = 0;
        VkPipelineShaderStageCreateInfo stages[4] = {vertexStage,fragmentStage};
        uint32_t stageCount = 2;
        if(isTess){
            auto *hullShader = (GTEVulkanShader *)desc.hullFunc.get();
            tessControlStage.stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            tessControlStage.module = hullShader->shaderModule;
            tessControlStage.pName  = "main";
            auto *domainShader = (GTEVulkanShader *)desc.domainFunc.get();
            tessEvalStage.stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            tessEvalStage.module = domainShader->shaderModule;
            tessEvalStage.pName  = "main";

            /// The hull's serialized descriptor is authoritative for the
            /// per-patch control-point count; the API-surface field is a
            /// fallback / validation value.
            patchControlPoints = desc.hullFunc->internal.tessellationDesc.output_control_points;
            if(patchControlPoints == 0){ patchControlPoints = desc.patchControlPoints; }
            if(patchControlPoints == 0){ patchControlPoints = 3; }
            tessState.patchControlPoints = patchControlPoints;

            stages[0] = vertexStage;
            stages[1] = tessControlStage;
            stages[2] = tessEvalStage;
            stages[3] = fragmentStage;
            stageCount = 4;
        }

        VkPipelineDepthStencilStateCreateInfo depthStencilStateDesc {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

        depthStencilStateDesc.minDepthBounds = 0.f;
        depthStencilStateDesc.maxDepthBounds = 1.f;
        depthStencilStateDesc.pNext = nullptr;
        
        depthStencilStateDesc.depthBoundsTestEnable = (VkBool32)desc.depthAndStencilDesc.enableDepth;
        depthStencilStateDesc.depthCompareOp = convertCompareFunc(desc.depthAndStencilDesc.depthOperation);
        depthStencilStateDesc.depthWriteEnable = desc.depthAndStencilDesc.writeAmount == DepthWriteAmount::All ? VK_TRUE : VK_FALSE;
        depthStencilStateDesc.depthTestEnable = (VkBool32)desc.depthAndStencilDesc.enableDepth;
        depthStencilStateDesc.stencilTestEnable = (VkBool32)desc.depthAndStencilDesc.enableStencil;

        depthStencilStateDesc.front.reference = 0;
        depthStencilStateDesc.front.compareMask = desc.depthAndStencilDesc.stencilReadMask;
        depthStencilStateDesc.front.compareOp = convertCompareFunc(desc.depthAndStencilDesc.frontFaceStencil.func);
        depthStencilStateDesc.front.writeMask = desc.depthAndStencilDesc.stencilWriteMask;
        depthStencilStateDesc.front.depthFailOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.depthFail);
        depthStencilStateDesc.front.failOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.stencilFail);
        depthStencilStateDesc.front.passOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.pass);

        depthStencilStateDesc.back.reference = 0;
        depthStencilStateDesc.back.compareMask = desc.depthAndStencilDesc.stencilReadMask;
        depthStencilStateDesc.back.compareOp = convertCompareFunc(desc.depthAndStencilDesc.backFaceStencil.func);
        depthStencilStateDesc.back.writeMask = desc.depthAndStencilDesc.stencilWriteMask;
        depthStencilStateDesc.back.depthFailOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.depthFail);
        depthStencilStateDesc.back.failOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.stencilFail);
        depthStencilStateDesc.back.passOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.pass);
        
        createInfo.pStages = stages;
        createInfo.stageCount = stageCount;
        if(isTess){
            createInfo.pTessellationState = &tessState;
        }
        createInfo.pDynamicState = &dynamicState;
        createInfo.pRasterizationState = &rasterState;
        createInfo.pVertexInputState = &vertexInputState;
        createInfo.pInputAssemblyState = &inputAssemblyState;
        createInfo.pViewportState = &viewportState;
        createInfo.pMultisampleState = &multisampleState;
        createInfo.pColorBlendState = &colorBlendState;
        createInfo.pDepthStencilState = &depthStencilStateDesc;

        VkPipeline pipeline = VK_NULL_HANDLE;
        auto pipelineRes = vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&createInfo,nullptr,&pipeline);
        if(!VK_RESULT_SUCCEEDED(pipelineRes) || pipeline == VK_NULL_HANDLE){
            std::cerr << "Vulkan graphics pipeline creation failed (" << pipelineRes << ")" << std::endl;
            vkDestroyRenderPass(device,compatibilityRenderPass,nullptr);
            vkDestroyPipelineLayout(device,layout,nullptr);
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_PIPELINE;
            nameInfoExt.objectHandle = (uint64_t)pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }
      
        auto result = std::shared_ptr<GEVulkanRenderPipelineState>(new GEVulkanRenderPipelineState(desc.vertexFunc,
                                                                                   desc.fragmentFunc,
                                                                                   this,
                                                                                   pipeline,
                                                                                   compatibilityRenderPass,
                                                                                   layout,
                                                                                   descriptorPool,
                                                                                   descs,
                                                                                   descLayouts,
                                                                                   immutableSamplers));
        /// §16 Phase G — record tessellation state so `drawPatches` can size
        /// its draw and `startRenderPass` can reject the pipeline.
        result->isTess = isTess;
        result->patchControlPoints = patchControlPoints;
        trackResource(result);
        return result;
    };
    SharedHandle<GEComputePipelineState> GEVulkanEngine::makeComputePipelineState(ComputePipelineDescriptor &desc){
        if(!_checkPipelineShader(desc.computeFunc,"compute",desc.name)){
            return nullptr;
        }

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;
        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool;
        OmegaCommon::Vector<VkSampler> immutableSamplers;

        VkPipelineLayout pipeline_layout = createPipelineLayoutFromShaderDescs(1,&desc.computeFunc->internal,&descriptorPool,descs,descLayouts,immutableSamplers);
        // Compute pipelines don't use static samplers currently, but clean up if any were created.
        for(auto & s : immutableSamplers){
            if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
        }

        

        VkComputePipelineCreateInfo pipeline_desc {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_desc.basePipelineIndex = -1;
        pipeline_desc.basePipelineHandle = VK_NULL_HANDLE;
        auto *computeShader = (GTEVulkanShader *)desc.computeFunc.get();
        VkPipelineShaderStageCreateInfo computeStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        computeStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeStage.module = computeShader->shaderModule;
        computeStage.pName = "main";
        
        pipeline_desc.stage = computeStage;
        pipeline_desc.layout = pipeline_layout;
         

         VkPipeline pipeline = VK_NULL_HANDLE;

         auto result = vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_desc,nullptr,&pipeline);
         if(!VK_RESULT_SUCCEEDED(result)){
            exit(1);
        };

        // Name the pipeline only AFTER it exists. Naming an uninitialized
        // VkPipeline handle hands garbage to the driver's
        // vkSetDebugUtilsObjectNameEXT, which segfaults inside the NVIDIA
        // driver when the debug layer is enabled.
        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_PIPELINE;
            nameInfoExt.objectHandle = (uint64_t)pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }

        VkDescriptorSet descSet = descs.empty() ? VK_NULL_HANDLE : descs.front();
        auto tracked = std::shared_ptr<GEVulkanComputePipelineState>(new GEVulkanComputePipelineState(desc.computeFunc,
                                                                                     this,
                                                                                     pipeline,
                                                                                     pipeline_layout,
                                                                                     descriptorPool,
                                                                                     descSet,
                                                                                     descLayouts));
        trackResource(tracked);
        return tracked;
    };

    // Extension 3: embedded OmegaSL source for the full-screen-triangle vertex
    // shader used by every blit pipeline. Mirrors the version embedded in the
    // D3D12 / Metal backends; the rasterizer-output struct is the contract the
    // user-supplied fragment shader must consume (see BlitPipelineDescriptor
    // doxygen in GEPipeline.h).
    static const char *kBlitFullscreenVsOmegaSL_Vulkan = R"(
struct OmegaGTEBlitVertexData internal {
    float4 pos : Position;
    float2 uv  : TexCoord;
};

vertex OmegaGTEBlitVertexData omega_gte_blit_fullscreen_vs(uint vid : VertexID){
    OmegaGTEBlitVertexData r;
    float u = (float)((vid << 1) & 2);
    float v = (float)(vid & 2);
    r.pos = make_float4(u * 2.0 - 1.0, 1.0 - v * 2.0, 0.0, 1.0);
    r.uv  = make_float2(u, v);
    return r;
}
)";

    bool GEVulkanEngine::ensureBlitFullscreenVs(){
        if(blitFullscreenVs) return true;
        try {
            auto compiler = OmegaSLCompiler::Create(gteDevice);
            if(!compiler){
                DEBUG_STREAM("ensureBlitFullscreenVs: OmegaSLCompiler::Create returned null");
                return false;
            }
            OmegaCommon::String src(kBlitFullscreenVsOmegaSL_Vulkan);
            auto source = OmegaSLCompiler::Source::fromString(src);
            blitFullscreenVsLib = compiler->compile({source});
            if(!blitFullscreenVsLib || blitFullscreenVsLib->header.entry_count == 0){
                DEBUG_STREAM("ensureBlitFullscreenVs: OmegaSL compile produced no shaders");
                blitFullscreenVsLib.reset();
                return false;
            }
            omegasl_shader *shaderDesc = &blitFullscreenVsLib->shaders[0];
            auto shader = _loadShaderFromDesc(shaderDesc, true);
            if(!shader){
                DEBUG_STREAM("ensureBlitFullscreenVs: _loadShaderFromDesc failed");
                blitFullscreenVsLib.reset();
                return false;
            }
            blitFullscreenVs = shader;
            return true;
        } catch(const std::exception &e) {
            DEBUG_STREAM("ensureBlitFullscreenVs: exception: " << e.what());
            blitFullscreenVs.reset();
            blitFullscreenVsLib.reset();
            return false;
        }
    }

    SharedHandle<GEBlitPipelineState> GEVulkanEngine::makeBlitPipelineState(BlitPipelineDescriptor &desc){
        if(!_checkPipelineShader(desc.fragmentFunc, "fragment", desc.name)){
            return nullptr;
        }
        if(!ensureBlitFullscreenVs()){
            DEBUG_STREAM("makeBlitPipelineState: ensureBlitFullscreenVs failed");
            return nullptr;
        }
        RenderPipelineDescriptor rpDesc{};
        rpDesc.name = desc.name.empty() ? OmegaCommon::String("OmegaGTE.Internal.BlitPipeline") : desc.name;
        rpDesc.vertexFunc = blitFullscreenVs;
        rpDesc.fragmentFunc = desc.fragmentFunc;
        rpDesc.colorPixelFormats = { desc.destPixelFormat };
        rpDesc.primitiveTopologyCategory = PrimitiveTopologyCategory::Triangle;
        rpDesc.rasterSampleCount = 1;
        rpDesc.cullMode = RasterCullMode::None;
        rpDesc.triangleFillMode = TriangleFillMode::Solid;
        auto rp = makeRenderPipelineState(rpDesc);
        if(!rp){
            DEBUG_STREAM("makeBlitPipelineState: underlying makeRenderPipelineState failed");
            return nullptr;
        }
        auto result = std::shared_ptr<GEVulkanBlitPipelineState>(new GEVulkanBlitPipelineState(rp));
        return result;
    };

    SharedHandle<GERenderPipelineState> GEVulkanEngine::makeMeshPipelineState(MeshPipelineDescriptor &desc){
        /// Mesh-Shader-Plan Phase 4a — Vulkan mesh PSO via
        /// `VkGraphicsPipeline` with `VK_SHADER_STAGE_MESH_BIT_EXT`.
        /// Mirrors the graphics `makeRenderPipelineState` above for
        /// render-pass / blend / depth-stencil / raster / multisample
        /// state — the only divergence is the stages array (mesh
        /// replaces vertex; no vertex-input or input-assembly state
        /// because the mesh stage emits primitives directly) and the
        /// PSO carries the new `isMesh` flag the command-buffer side
        /// asserts on at draw time.
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_MESH_SHADER)){
            DEBUG_STREAM("makeMeshPipelineState: device does not advertise "
                         "GTEDEVICE_FEATURE_MESH_SHADER ('" << desc.name << "')");
            return nullptr;
        }
    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        if(!hasMeshShaderExt){
            /// Belt-and-suspenders: the GTEDEVICE flag should have
            /// already failed above, but if a device claimed the
            /// capability without us enabling the extension we
            /// wouldn't have a valid `VK_SHADER_STAGE_MESH_BIT_EXT`
            /// to put in the stages array. Fail loud rather than
            /// hand the driver a malformed pipeline.
            DEBUG_STREAM("makeMeshPipelineState: VK_EXT_mesh_shader was not enabled at device init ('"
                         << desc.name << "')");
            return nullptr;
        }
    #else
        DEBUG_STREAM("makeMeshPipelineState: Vulkan headers built without VK_EXT_mesh_shader ('"
                     << desc.name << "')");
        return nullptr;
    #endif
        if(!_checkPipelineShader(desc.meshFunc, "mesh", desc.name)){
            return nullptr;
        }
        if(!_checkPipelineShader(desc.fragmentFunc, "fragment", desc.name)){
            return nullptr;
        }
        /// §5 — amplification (Task) stage. Optional: a mesh pipeline runs
        /// perfectly well without one, so a null `amplificationFunc` is the
        /// normal case and everything below degrades to the Phase-4a shape.
        const bool hasAmplification = (desc.amplificationFunc != nullptr);
        if(hasAmplification){
            if(!_checkPipelineShader(desc.amplificationFunc, "amplification", desc.name)){
                return nullptr;
            }
            if(!hasTaskShaderFeature){
                /// The device exposed VK_EXT_mesh_shader but not its optional
                /// `taskShader` feature — mesh-only hardware. Fail loud at
                /// pipeline build rather than hand the driver a TASK stage it
                /// never agreed to run.
                DEBUG_STREAM("makeMeshPipelineState: device does not support the "
                             "VK_EXT_mesh_shader `taskShader` feature, so an amplification "
                             "stage cannot be bound ('" << desc.name << "')");
                return nullptr;
            }
            /// The amp and the mesh stage must agree on the payload struct. They
            /// are compiled independently — nothing but this check stops a caller
            /// pairing an amplification shader with a mesh shader built against a
            /// DIFFERENT payload type, in which case the mesh side would read the
            /// amp's bytes through the wrong layout and silently render garbage.
            /// Comparing the serialized size is the cheap, sufficient test: a
            /// mismatch always means different structs.
            const unsigned ampPayload  = desc.amplificationFunc->internal.payloadDesc.size;
            const unsigned meshPayload = desc.meshFunc->internal.payloadDesc.size;
            if(ampPayload == 0){
                DEBUG_STREAM("makeMeshPipelineState: amplification shader declares no `out payload` ('"
                             << desc.name << "')");
                return nullptr;
            }
            if(ampPayload != meshPayload){
                DEBUG_STREAM("makeMeshPipelineState: payload mismatch between the amplification stage ("
                             << ampPayload << " bytes) and the mesh stage (" << meshPayload
                             << " bytes) — the two shaders were built against different payload structs ('"
                             << desc.name << "')");
                return nullptr;
            }
        }

    #ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
        /// Shader-array ORDER is load-bearing, not cosmetic:
        /// `createPipelineLayoutFromShaderDescs` creates one descriptor set per
        /// shader in this order, and `bindResourceAtVertexShader` /
        /// `bindResourceAtFragmentShader` write sets 0 and 1 by hard-coded index.
        /// Appending the amplification shader at the TAIL keeps mesh at set 0 and
        /// fragment at set 1 — putting it first would silently redirect every
        /// existing mesh-stage bind into the amp's set.
        omegasl_shader shaders[] = {desc.meshFunc->internal,
                                    desc.fragmentFunc->internal,
                                    hasAmplification ? desc.amplificationFunc->internal
                                                     : omegasl_shader{}};
        const unsigned shaderCount = hasAmplification ? 3u : 2u;

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;
        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        OmegaCommon::Vector<VkSampler> immutableSamplers;

        VkPipelineLayout layout = createPipelineLayoutFromShaderDescs(shaderCount,shaders,&descriptorPool,descs,descLayouts,immutableSamplers);
        if(layout == VK_NULL_HANDLE){
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        /// Render pass — identical shape to the graphics path: one
        /// subpass, per-color-attachment description + ref, external
        /// dependency on COLOR_ATTACHMENT_OUTPUT. Reused so the mesh
        /// PSO is render-pass-compatible with the same fragment-side
        /// state.
        const unsigned colorFormatCount = desc.colorPixelFormats.empty()
                                              ? 1u
                                              : (unsigned)desc.colorPixelFormats.size();

        OmegaCommon::Vector<VkAttachmentDescription> colorAttachments;
        OmegaCommon::Vector<VkAttachmentReference> colorRefs;
        colorAttachments.resize(colorFormatCount);
        colorRefs.resize(colorFormatCount);
        for(unsigned i = 0; i < colorFormatCount; ++i){
            VkAttachmentDescription & a = colorAttachments[i];
            a = {};
            a.format = pixelFormatToVkFormat(desc.colorPixelFormats.empty()
                                                 ? PixelFormat::RGBA8Unorm
                                                 : desc.colorPixelFormats[i]);
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }

        VkSubpassDescription subpassDesc {};
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.colorAttachmentCount = colorFormatCount;
        subpassDesc.pColorAttachments = colorRefs.data();

        // Dependencies MUST be byte-identical to the command-queue render pass
        // (GEVulkanCommandBuffer::startRenderPass) — see comment in the
        // graphics pipeline path (VUID-vkCmdDraw-renderPass-02684).
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo renderPassInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = colorFormatCount;
        renderPassInfo.pAttachments = colorAttachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpassDesc;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        VkRenderPass compatibilityRenderPass = VK_NULL_HANDLE;
        auto renderPassRes = vkCreateRenderPass(device,&renderPassInfo,nullptr,&compatibilityRenderPass);
        if(renderPassRes != VK_SUCCESS || compatibilityRenderPass == VK_NULL_HANDLE){
            std::cerr << "Vulkan mesh render pass creation failed (" << renderPassRes << ")" << std::endl;
            vkDestroyPipelineLayout(device,layout,nullptr);
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        /// Stages — mesh + fragment. NO vertex stage; vertex-input +
        /// input-assembly states are spec-ignored when a mesh stage is
        /// present but we pass well-formed zeroed structs anyway so
        /// validation layers don't complain about a NULL pointer.
        auto *meshShader = (GTEVulkanShader *)desc.meshFunc.get();
        VkPipelineShaderStageCreateInfo meshStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        meshStage.pNext = nullptr;
        meshStage.flags = 0;
        meshStage.stage = VK_SHADER_STAGE_MESH_BIT_EXT;
        meshStage.module = meshShader->shaderModule;
        meshStage.pName = "main";
        meshStage.pSpecializationInfo = nullptr;

        auto *fragmentShader = (GTEVulkanShader *)desc.fragmentFunc.get();
        VkPipelineShaderStageCreateInfo fragmentStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        fragmentStage.pNext = nullptr;
        fragmentStage.flags = 0;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentShader->shaderModule;
        fragmentStage.pName = "main";
        fragmentStage.pSpecializationInfo = nullptr;

        /// §5 — the task stage runs BEFORE the mesh stage, so it goes first in
        /// the stages array. (Vulkan does not actually require a particular
        /// order here — it keys off `stage` — but writing them in execution order
        /// is what the next reader will expect.)
        VkPipelineShaderStageCreateInfo taskStage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        if(hasAmplification){
            auto *ampShader = (GTEVulkanShader *)desc.amplificationFunc.get();
            taskStage.pNext = nullptr;
            taskStage.flags = 0;
            taskStage.stage = VK_SHADER_STAGE_TASK_BIT_EXT;
            taskStage.module = ampShader->shaderModule;
            taskStage.pName = "main";
            taskStage.pSpecializationInfo = nullptr;
        }

        VkPipelineShaderStageCreateInfo stages[3] = {};
        uint32_t stageCount = 0;
        if(hasAmplification){
            stages[stageCount++] = taskStage;
        }
        stages[stageCount++] = meshStage;
        stages[stageCount++] = fragmentStage;

        VkPipelineVertexInputStateCreateInfo vertexInputState {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInputState.vertexBindingDescriptionCount = 0;
        vertexInputState.pVertexBindingDescriptions = nullptr;
        vertexInputState.vertexAttributeDescriptionCount = 0;
        vertexInputState.pVertexAttributeDescriptions = nullptr;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssemblyState.primitiveRestartEnable = VK_FALSE;

        /// Raster state — identical mapping to the graphics path.
        VkPipelineRasterizationStateCreateInfo rasterState {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterState.depthClampEnable = VK_FALSE;
        rasterState.rasterizerDiscardEnable = VK_FALSE;
        rasterState.depthBiasEnable = VK_FALSE;
        rasterState.lineWidth = 1.0f;
        switch(desc.cullMode){
            case RasterCullMode::None:  rasterState.cullMode = VK_CULL_MODE_NONE;       break;
            case RasterCullMode::Front: rasterState.cullMode = VK_CULL_MODE_FRONT_BIT;  break;
            case RasterCullMode::Back:  rasterState.cullMode = VK_CULL_MODE_BACK_BIT;   break;
        }
        rasterState.polygonMode = (desc.triangleFillMode == TriangleFillMode::Wireframe)
                                      ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        /// Vulkan render-parity: OPPOSITE VkFrontFace to compensate for the
        /// negative-height viewport's winding reversal (see the graphics-pipeline
        /// note in makeRenderPipelineState — GEVulkanCommandQueue.cpp:1433).
        rasterState.frontFace = (desc.polygonFrontFaceRotation == GTEPolygonFrontFaceRotation::Clockwise)
                                    ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;

        /// Viewport/scissor placeholders — actual values come at draw
        /// time via VK_DYNAMIC_STATE_{VIEWPORT,SCISSOR}, same as the
        /// graphics path.
        VkViewport viewport {0.f,0.f,1.f,1.f,0.f,1.f};
        VkRect2D scissor {{0,0},{1,1}};
        VkPipelineViewportStateCreateInfo viewportState {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;

        VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT;
        switch(desc.rasterSampleCount){
            case 2:  sampleCount = VK_SAMPLE_COUNT_2_BIT;  break;
            case 4:  sampleCount = VK_SAMPLE_COUNT_4_BIT;  break;
            case 8:  sampleCount = VK_SAMPLE_COUNT_8_BIT;  break;
            case 16: sampleCount = VK_SAMPLE_COUNT_16_BIT; break;
            case 32: sampleCount = VK_SAMPLE_COUNT_32_BIT; break;
            case 64: sampleCount = VK_SAMPLE_COUNT_64_BIT; break;
            default: sampleCount = VK_SAMPLE_COUNT_1_BIT;  break;
        }
        VkPipelineMultisampleStateCreateInfo multisampleState {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampleState.rasterizationSamples = sampleCount;
        multisampleState.sampleShadingEnable = VK_FALSE;

        OmegaCommon::Vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        colorBlendAttachments.resize(colorFormatCount);
        for(size_t i = 0; i < colorFormatCount; ++i){
            VkPipelineColorBlendAttachmentState & att = colorBlendAttachments[i];
            att = {};
            if(i < desc.colorBlendDescriptors.size()){
                const auto & b = desc.colorBlendDescriptors[i];
                att.blendEnable         = b.blendEnabled ? VK_TRUE : VK_FALSE;
                att.srcColorBlendFactor = convertBlendFactorVk(b.srcColorFactor);
                att.dstColorBlendFactor = convertBlendFactorVk(b.destColorFactor);
                att.colorBlendOp        = convertBlendOperationVk(b.colorOp);
                att.srcAlphaBlendFactor = convertBlendFactorVk(b.srcAlphaFactor);
                att.dstAlphaBlendFactor = convertBlendFactorVk(b.destAlphaFactor);
                att.alphaBlendOp        = convertBlendOperationVk(b.alphaOp);
                att.colorWriteMask      = convertColorWriteMaskVk(b.writeMask);
            }
            else {
                att.blendEnable = VK_FALSE;
                att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                att.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                att.colorBlendOp = VK_BLEND_OP_ADD;
                att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                att.alphaBlendOp = VK_BLEND_OP_ADD;
                att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            }
        }
        VkPipelineColorBlendStateCreateInfo colorBlendState {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlendState.logicOpEnable = VK_FALSE;
        colorBlendState.attachmentCount = (uint32_t)colorBlendAttachments.size();
        colorBlendState.pAttachments = colorBlendAttachments.data();

        OmegaCommon::Vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE
        };
        VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = (uint32_t)dynamicStates.size();
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineDepthStencilStateCreateInfo depthStencilStateDesc {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencilStateDesc.minDepthBounds = 0.f;
        depthStencilStateDesc.maxDepthBounds = 1.f;
        depthStencilStateDesc.depthBoundsTestEnable = (VkBool32)desc.depthAndStencilDesc.enableDepth;
        depthStencilStateDesc.depthCompareOp = convertCompareFunc(desc.depthAndStencilDesc.depthOperation);
        depthStencilStateDesc.depthWriteEnable = desc.depthAndStencilDesc.writeAmount == DepthWriteAmount::All ? VK_TRUE : VK_FALSE;
        depthStencilStateDesc.depthTestEnable = (VkBool32)desc.depthAndStencilDesc.enableDepth;
        depthStencilStateDesc.stencilTestEnable = (VkBool32)desc.depthAndStencilDesc.enableStencil;
        depthStencilStateDesc.front.reference = 0;
        depthStencilStateDesc.front.compareMask = desc.depthAndStencilDesc.stencilReadMask;
        depthStencilStateDesc.front.compareOp = convertCompareFunc(desc.depthAndStencilDesc.frontFaceStencil.func);
        depthStencilStateDesc.front.writeMask = desc.depthAndStencilDesc.stencilWriteMask;
        depthStencilStateDesc.front.depthFailOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.depthFail);
        depthStencilStateDesc.front.failOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.stencilFail);
        depthStencilStateDesc.front.passOp = convertStencilOp(desc.depthAndStencilDesc.frontFaceStencil.pass);
        depthStencilStateDesc.back.reference = 0;
        depthStencilStateDesc.back.compareMask = desc.depthAndStencilDesc.stencilReadMask;
        depthStencilStateDesc.back.compareOp = convertCompareFunc(desc.depthAndStencilDesc.backFaceStencil.func);
        depthStencilStateDesc.back.writeMask = desc.depthAndStencilDesc.stencilWriteMask;
        depthStencilStateDesc.back.depthFailOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.depthFail);
        depthStencilStateDesc.back.failOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.stencilFail);
        depthStencilStateDesc.back.passOp = convertStencilOp(desc.depthAndStencilDesc.backFaceStencil.pass);

        VkGraphicsPipelineCreateInfo createInfo {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        createInfo.basePipelineHandle = VK_NULL_HANDLE;
        createInfo.basePipelineIndex = -1;
        createInfo.layout = layout;
        createInfo.renderPass = compatibilityRenderPass;
        createInfo.subpass = 0;
        createInfo.pStages = stages;
        createInfo.stageCount = stageCount;
        createInfo.pDynamicState = &dynamicState;
        createInfo.pRasterizationState = &rasterState;
        createInfo.pVertexInputState = &vertexInputState;     // ignored when mesh stage present
        createInfo.pInputAssemblyState = &inputAssemblyState; // ignored when mesh stage present
        createInfo.pViewportState = &viewportState;
        createInfo.pMultisampleState = &multisampleState;
        createInfo.pColorBlendState = &colorBlendState;
        createInfo.pDepthStencilState = &depthStencilStateDesc;

        VkPipeline pipeline = VK_NULL_HANDLE;
        auto pipelineRes = vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&createInfo,nullptr,&pipeline);
        if(!VK_RESULT_SUCCEEDED(pipelineRes) || pipeline == VK_NULL_HANDLE){
            std::cerr << "Vulkan mesh pipeline creation failed (" << pipelineRes << ")" << std::endl;
            vkDestroyRenderPass(device,compatibilityRenderPass,nullptr);
            vkDestroyPipelineLayout(device,layout,nullptr);
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            for(auto & s : immutableSamplers){
                if(s != VK_NULL_HANDLE) vkDestroySampler(device,s,nullptr);
            }
            return nullptr;
        }

        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.objectType = VK_OBJECT_TYPE_PIPELINE;
            nameInfoExt.objectHandle = (uint64_t)pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }

        auto result = std::shared_ptr<GEVulkanRenderPipelineState>(
            new GEVulkanRenderPipelineState(desc.meshFunc,
                                            desc.fragmentFunc,
                                            this,
                                            pipeline,
                                            compatibilityRenderPass,
                                            layout,
                                            descriptorPool,
                                            descs,
                                            descLayouts,
                                            immutableSamplers,
                                            /*meshVariant=*/true));
        /// §5 — stamped after construction (rather than threaded through the
        /// already-11-argument constructor) because it is optional and every
        /// other mesh-PSO field is not.
        if(hasAmplification){
            result->amplificationShader = desc.amplificationFunc;
        }
        trackResource(result);
        return result;
    #endif
    }

    SharedHandle<GEFence> GEVulkanEngine::makeFence(){
        
        VkFenceCreateInfo fenceCreateInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        fenceCreateInfo.pNext = nullptr;

        VkFence fence = VK_NULL_HANDLE;
        auto fenceRes = vkCreateFence(device,&fenceCreateInfo,nullptr,&fence);
        if(fenceRes != VK_SUCCESS || fence == VK_NULL_HANDLE){
            std::cerr << "Vulkan fence creation failed (" << fenceRes << ")" << std::endl;
            return nullptr;
        }

        VkEventCreateInfo eventCreateInfo {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        eventCreateInfo.pNext = nullptr;
        eventCreateInfo.flags = 0;

        VkEvent event = VK_NULL_HANDLE;
        auto eventRes = vkCreateEvent(device,&eventCreateInfo,nullptr,&event);
        if(eventRes != VK_SUCCESS){
            std::cerr << "Vulkan event creation failed (" << eventRes << ")" << std::endl;
            event = VK_NULL_HANDLE;
        }

        auto result = std::shared_ptr<GEVulkanFence>(new GEVulkanFence(this,fence,event));
        trackResource(result);
        return result;
    };

    SharedHandle<GESamplerState> GEVulkanEngine::makeSamplerState(const SamplerDescriptor &desc) {
        VkSamplerCreateInfo samplerCreateInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerCreateInfo.pNext = nullptr;
        samplerCreateInfo.addressModeU = convertAddressMode(desc.uAddressMode);
        samplerCreateInfo.addressModeV = convertAddressMode(desc.vAddressMode);
        samplerCreateInfo.addressModeW = convertAddressMode(desc.wAddressMode);
        samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

        VkFilter magFilter,minFilter;
        VkSamplerMipmapMode mipFilter;
        switch (desc.filter) {
            case SamplerDescriptor::Filter::Linear : {
                magFilter = minFilter = VK_FILTER_LINEAR;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            }
            case SamplerDescriptor::Filter::Point : {
                magFilter = minFilter = VK_FILTER_NEAREST;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            }
            case SamplerDescriptor::Filter::MagLinearMinPointMipLinear : {
                magFilter = VK_FILTER_LINEAR;
                minFilter = VK_FILTER_NEAREST;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            }
            case SamplerDescriptor::Filter::MagLinearMinLinearMipPoint : {
                magFilter = minFilter = VK_FILTER_LINEAR;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            }
            case SamplerDescriptor::Filter::MagLinearMinPointMipPoint : {
                magFilter = VK_FILTER_LINEAR;
                minFilter = VK_FILTER_NEAREST;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            }
            case SamplerDescriptor::Filter::MagPointMinLinearMipLinear : {
                magFilter = VK_FILTER_NEAREST;
                minFilter = VK_FILTER_LINEAR;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            }
            case SamplerDescriptor::Filter::MagPointMinLinearMipPoint : {
                magFilter = VK_FILTER_NEAREST;
                minFilter = VK_FILTER_LINEAR;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                break;
            }
            case SamplerDescriptor::Filter::MagPointMinPointMipLinear : {
                minFilter = magFilter = VK_FILTER_NEAREST;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                break;
            }
            case SamplerDescriptor::Filter::MaxAnisotropic : {
                magFilter = minFilter = VK_FILTER_LINEAR;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                samplerCreateInfo.anisotropyEnable = VK_TRUE;
                samplerCreateInfo.maxAnisotropy = (float)desc.maxAnisotropy;
            }
            case SamplerDescriptor::Filter::MinAnisotropic : {
                magFilter = minFilter = VK_FILTER_NEAREST;
                mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                samplerCreateInfo.anisotropyEnable = VK_TRUE;
                samplerCreateInfo.maxAnisotropy = (float)desc.maxAnisotropy;
            }
        }
        samplerCreateInfo.magFilter = magFilter;
        samplerCreateInfo.minFilter = minFilter;
        samplerCreateInfo.mipmapMode = mipFilter;

        VkSampler sampler;

        vkCreateSampler(device,&samplerCreateInfo,nullptr,&sampler);

        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_SAMPLER;
            nameInfoExt.objectHandle = (uint64_t)sampler;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }

        auto result = std::shared_ptr<GEVulkanSamplerState>(new GEVulkanSamplerState(this,sampler));
        trackResource(result);
        return result;
    }

    SharedHandle<GENativeRenderTarget> GEVulkanEngine::makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc,
                                                                               SharedHandle<GECommandQueue> presentQueue){
        if(instance == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queueFamilyIndices.empty()){
            std::cerr << "Vulkan native render target creation failed: Vulkan engine is not initialized." << std::endl;
            return nullptr;
        }
        // Gate the requested color format against the portable
        // swap-chain/drawable intersection so a backend mismatch is
        // surfaced rather than silently substituted.
        if(!isPortableNativeRenderTargetFormat(desc.pixelFormat)){
            std::cerr << "Vulkan native render target creation failed: requested pixelFormat is not in the portable swap-chain set." << std::endl;
            return nullptr;
        }

        // Phase 2 of the CommandQueue-Typed-Pool plan: a Transfer-typed
        // queue is not allowed to be the presentation queue. Vulkan
        // requires a graphics-capable family for swap-chain ownership
        // (vkAcquireNextImageKHR / vkQueuePresentKHR) and even when a
        // transfer-only family is present-capable on a given driver, the
        // user said "this queue is for DMA" — present must go through a
        // queue that matches that intent. Reject up front so the
        // descriptor parse fails loudly rather than later in swap-chain
        // creation with a generic VK_ERROR_INITIALIZATION_FAILED.
        if(presentQueue != nullptr && presentQueue->type() == GECommandQueueDesc::Type::Transfer){
            std::cerr << "Vulkan native render target creation failed: presentQueue is a Transfer queue; swap-chain presents require a graphics-capable queue." << std::endl;
            return nullptr;
        }

        VkSurfaceKHR surfaceKhr = VK_NULL_HANDLE;

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
        if(desc.wl_display != nullptr && desc.wl_surface != nullptr){
            VkWaylandSurfaceCreateInfoKHR infoKhr{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
            infoKhr.pNext = nullptr;
            infoKhr.surface = desc.wl_surface;
            infoKhr.display = desc.wl_display;
            infoKhr.flags = 0;
            auto surfaceRes = vkCreateWaylandSurfaceKHR(instance,&infoKhr,nullptr,&surfaceKhr);
            if(surfaceRes != VK_SUCCESS){
                std::cerr << "vkCreateWaylandSurfaceKHR failed (" << surfaceRes << ")" << std::endl;
                surfaceKhr = VK_NULL_HANDLE;
            } else {
                VkBool32 waylandSupported = VK_FALSE;
                auto supportRes = vkGetPhysicalDeviceSurfaceSupportKHR(
                        physicalDevice,queueFamilyIndices[0],surfaceKhr,&waylandSupported);
                if(supportRes != VK_SUCCESS || waylandSupported == VK_FALSE){
                    vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
                    surfaceKhr = VK_NULL_HANDLE;
                }
            }
        }
#endif

#ifdef VK_USE_PLATFORM_XLIB_KHR
        if(surfaceKhr == VK_NULL_HANDLE && desc.x_display != nullptr && desc.x_window != 0){
            VkXlibSurfaceCreateInfoKHR xlibSurfaceCreateInfoKhr{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
            xlibSurfaceCreateInfoKhr.pNext = nullptr;
            xlibSurfaceCreateInfoKhr.flags = 0;
            xlibSurfaceCreateInfoKhr.window = desc.x_window;
            xlibSurfaceCreateInfoKhr.dpy = desc.x_display;
            auto surfaceRes = vkCreateXlibSurfaceKHR(instance,&xlibSurfaceCreateInfoKhr,nullptr,&surfaceKhr);
            if(surfaceRes != VK_SUCCESS){
                std::cerr << "vkCreateXlibSurfaceKHR failed (" << surfaceRes << ")" << std::endl;
                surfaceKhr = VK_NULL_HANDLE;
            }
        }
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
        if(surfaceKhr == VK_NULL_HANDLE && desc.window != nullptr){
            VkAndroidSurfaceCreateInfoKHR androidSurfaceCreateInfoKhr {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            androidSurfaceCreateInfoKhr.window = desc.window;
            androidSurfaceCreateInfoKhr.pNext = nullptr;
            androidSurfaceCreateInfoKhr.flags = 0;
            auto surfaceRes = vkCreateAndroidSurfaceKHR(instance,&androidSurfaceCreateInfoKhr,nullptr,&surfaceKhr);
            if(surfaceRes != VK_SUCCESS){
                std::cerr << "vkCreateAndroidSurfaceKHR failed (" << surfaceRes << ")" << std::endl;
                surfaceKhr = VK_NULL_HANDLE;
            }
        }
#endif

        if(surfaceKhr == VK_NULL_HANDLE){
            std::cerr << "Vulkan native render target creation failed: no compatible native surface handle was provided." << std::endl;
            return nullptr;
        }

        auto capsRes = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice,surfaceKhr,&capabilitiesKhr);
        if(capsRes != VK_SUCCESS){
            std::cerr << "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (" << capsRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        OmegaCommon::Vector<VkSurfaceFormatKHR> surfaceFormats;
        std::uint32_t count = 0;
        auto formatRes = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surfaceKhr,&count,nullptr);
        if(formatRes != VK_SUCCESS || count == 0){
            std::cerr << "vkGetPhysicalDeviceSurfaceFormatsKHR(count) failed (" << formatRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        surfaceFormats.resize(count);
        formatRes = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surfaceKhr,&count,surfaceFormats.data());
        if(formatRes != VK_SUCCESS){
            std::cerr << "vkGetPhysicalDeviceSurfaceFormatsKHR(list) failed (" << formatRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        // Honor `desc.pixelFormat`: pick the surface entry whose VkFormat
        // matches the requested portable format. If the surface does not
        // advertise the requested format, fail loudly rather than silently
        // substituting one — the caller asked for a specific format and
        // accepting any other would break shaders/blit assumptions.
        const VkFormat requestedVkFormat = pixelFormatToVkFormat(desc.pixelFormat);
        VkSurfaceFormatKHR selectedSurfaceFormat{};
        bool foundRequestedFormat = false;
        for(auto &formatCandidate : surfaceFormats){
            if(formatCandidate.format == requestedVkFormat){
                selectedSurfaceFormat = formatCandidate;
                foundRequestedFormat = true;
                break;
            }
        }
        if(!foundRequestedFormat){
            std::cerr << "Vulkan native render target creation failed: surface does not advertise requested VkFormat "
                      << requestedVkFormat << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }
        DEBUG_STREAM("Selected swapchain format: " << selectedSurfaceFormat.format);

        OmegaCommon::Vector<VkPresentModeKHR> presentModes;
        auto presentRes = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,surfaceKhr,&count,nullptr);
        if(presentRes != VK_SUCCESS || count == 0){
            std::cerr << "vkGetPhysicalDeviceSurfacePresentModesKHR(count) failed (" << presentRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        presentModes.resize(count);
        presentRes = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,surfaceKhr,&count,presentModes.data());
        if(presentRes != VK_SUCCESS){
            std::cerr << "vkGetPhysicalDeviceSurfacePresentModesKHR(list) failed (" << presentRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        VkPresentModeKHR presentModeKhr = VK_PRESENT_MODE_FIFO_KHR;
        for(auto mode : presentModes){
            if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR){
                presentModeKhr = mode;
                break;
            }
            if(mode == VK_PRESENT_MODE_FIFO_KHR){
                presentModeKhr = mode;
            }
        }

        VkExtent2D swapExtent = capabilitiesKhr.currentExtent;
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        if(swapExtent.width == UINT32_MAX || swapExtent.height == UINT32_MAX){
            swapExtent.width = desc.width > 0 ? desc.width : 1;
            swapExtent.height = desc.height > 0 ? desc.height : 1;
        }
#endif
        if(swapExtent.width == 0){
            swapExtent.width = 1;
        }
        if(swapExtent.height == 0){
            swapExtent.height = 1;
        }

        std::uint32_t imageCount = capabilitiesKhr.minImageCount > 2 ? capabilitiesKhr.minImageCount : 2;
        if(capabilitiesKhr.maxImageCount > 0 && imageCount > capabilitiesKhr.maxImageCount){
            imageCount = capabilitiesKhr.maxImageCount;
        }

        VkSwapchainKHR swapchainKhr = VK_NULL_HANDLE;
        VkSwapchainCreateInfoKHR swapchainInfo {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surfaceKhr;
        swapchainInfo.pNext = nullptr;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.clipped = VK_FALSE;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainInfo.imageFormat = selectedSurfaceFormat.format;
        swapchainInfo.imageColorSpace = selectedSurfaceFormat.colorSpace;
        swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.presentMode = presentModeKhr;
        swapchainInfo.imageExtent = swapExtent;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.preTransform = capabilitiesKhr.currentTransform;
        swapchainInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
        swapchainInfo.pQueueFamilyIndices = queueFamilyIndices.data();

        auto swapchainRes = vkCreateSwapchainKHR(device,&swapchainInfo,nullptr,&swapchainKhr);
        if(swapchainRes != VK_SUCCESS || swapchainKhr == VK_NULL_HANDLE){
            std::cerr << "vkCreateSwapchainKHR failed (" << swapchainRes << ")" << std::endl;
            vkDestroySurfaceKHR(instance,surfaceKhr,nullptr);
            return nullptr;
        }

        unsigned mipLevels = 1;

        auto result = std::shared_ptr<GEVulkanNativeRenderTarget>(new GEVulkanNativeRenderTarget(
                this,
                std::move(presentQueue),
                surfaceKhr,
                swapchainKhr,
                selectedSurfaceFormat.format,
                mipLevels,
                swapExtent));
        trackResource(result);
        return result;
    };

    SharedHandle<GETextureRenderTarget> GEVulkanEngine::makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc){
        SharedHandle<GETexture> tex;
        if(!desc.renderToExistingTexture){
            TextureDescriptor texDesc{};
            texDesc.storage_opts = OmegaGTE::Shared;
            texDesc.usage = GETexture::GPUAccessOnly;
            texDesc.kind = TextureKind::Tex2D;
            tex = makeTexture(texDesc);
        }
        else {
            tex = desc.texture;
        }

        auto vk_tex = std::dynamic_pointer_cast<GEVulkanTexture>(tex);

        VkFramebuffer fb = VK_NULL_HANDLE;

        auto result = std::shared_ptr<GEVulkanTextureRenderTarget>(new GEVulkanTextureRenderTarget(this,vk_tex,fb));
        trackResource(result);
        return result;
    };

    SharedHandle<GEBuffer> GEVulkanEngine::createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes){
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
            DEBUG_STREAM("Raytracing not supported on this device");
            return nullptr;
        }
        struct VkAABB { float minX,minY,minZ,maxX,maxY,maxZ; };
        size_t totalSize = sizeof(VkAABB) * boxes.size();
        auto buffer = std::dynamic_pointer_cast<GEVulkanBuffer>(
            makeBuffer({BufferDescriptor::Upload, totalSize, sizeof(VkAABB)}));
        if(!buffer) return nullptr;

        void *mapped = nullptr;
        vmaMapMemory(memAllocator, buffer->alloc, &mapped);
        auto *dst = reinterpret_cast<VkAABB *>(mapped);
        for(size_t i = 0; i < boxes.size(); ++i){
            dst[i].minX = boxes[i].minX;
            dst[i].minY = boxes[i].minY;
            dst[i].minZ = boxes[i].minZ;
            dst[i].maxX = boxes[i].maxX;
            dst[i].maxY = boxes[i].maxY;
            dst[i].maxZ = boxes[i].maxZ;
        }
        vmaUnmapMemory(memAllocator, buffer->alloc);
        return std::static_pointer_cast<GEBuffer>(buffer);
    }

    SharedHandle<GEAccelerationStruct> GEVulkanEngine::allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc){
        if(!gteDevice->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING)){
            DEBUG_STREAM("Raytracing not supported on this device");
            return nullptr;
        }
        if(!hasAccelerationStructureExt || vkCreateAccelerationStructureKhr == nullptr ||
           vkGetAccelerationStructureBuildSizesKhr == nullptr){
            std::cerr << "Vulkan acceleration structure extension not available." << std::endl;
            return nullptr;
        }

        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<uint32_t> maxPrimitiveCounts;

        for(auto & g : desc.data){
            VkAccelerationStructureGeometryKHR geom {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            if(g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES){
                geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                geom.geometry.triangles.pNext = nullptr;
                geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                geom.geometry.triangles.vertexStride = sizeof(float) * 3;
                geom.geometry.triangles.maxVertex = 0;
                geom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getTriangleList().buffer);
                if(vkBuf && hasBufferDeviceAddressExt && vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.triangles.vertexData.deviceAddress = vkGetBufferDeviceAddressKhr(device, &addrInfo);
                    uint32_t vertexCount = static_cast<uint32_t>(vkBuf->size() / (sizeof(float) * 3));
                    geom.geometry.triangles.maxVertex = vertexCount > 0 ? vertexCount - 1 : 0;
                    maxPrimitiveCounts.push_back(vertexCount / 3);
                } else {
                    maxPrimitiveCounts.push_back(0);
                }
            } else {
                geom.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
                geom.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
                geom.geometry.aabbs.pNext = nullptr;
                geom.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
                auto vkBuf = std::dynamic_pointer_cast<GEVulkanBuffer>(g.getAabb().buffer);
                if(vkBuf && hasBufferDeviceAddressExt && vkGetBufferDeviceAddressKhr){
                    VkBufferDeviceAddressInfoKHR addrInfo {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR};
                    addrInfo.buffer = vkBuf->buffer;
                    geom.geometry.aabbs.data.deviceAddress = vkGetBufferDeviceAddressKhr(device, &addrInfo);
                    maxPrimitiveCounts.push_back(static_cast<uint32_t>(vkBuf->size() / sizeof(VkAabbPositionsKHR)));
                } else {
                    maxPrimitiveCounts.push_back(0);
                }
            }
            geometries.push_back(geom);
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        if(geometries.empty()){
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            buildInfo.geometryCount = 0;
            buildInfo.pGeometries = nullptr;
        } else {
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
            buildInfo.pGeometries = geometries.data();
        }

        VkAccelerationStructureBuildSizesInfoKHR sizeInfo {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKhr(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo,
            maxPrimitiveCounts.empty() ? nullptr : maxPrimitiveCounts.data(),
            &sizeInfo);

        size_t structSize = sizeInfo.accelerationStructureSize > 0 ? sizeInfo.accelerationStructureSize : 256;
        size_t scratchSize = sizeInfo.buildScratchSize > 0 ? sizeInfo.buildScratchSize : 256;

        VkBufferCreateInfo structBufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        structBufInfo.size = structSize;
        structBufInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        structBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo structAllocInfo {};
        structAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkBuffer structBuf = VK_NULL_HANDLE;
        VmaAllocation structAlloc = nullptr;
        VmaAllocationInfo structAllocResult {};
        auto res = vmaCreateBuffer(memAllocator, &structBufInfo, &structAllocInfo, &structBuf, &structAlloc, &structAllocResult);
        if(res != VK_SUCCESS || structBuf == VK_NULL_HANDLE){
            std::cerr << "Vulkan accel struct buffer creation failed (" << res << ")" << std::endl;
            return nullptr;
        }
        VkBufferView nullView = VK_NULL_HANDLE;
        auto structBuffer = std::shared_ptr<GEVulkanBuffer>(
            new GEVulkanBuffer(BufferDescriptor::GPUOnly, this, structBuf, nullView, structAlloc, structAllocResult));
        trackResource(structBuffer);

        VkBufferCreateInfo scratchBufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        scratchBufInfo.size = scratchSize;
        scratchBufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratchBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo scratchAllocInfo {};
        scratchAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkBuffer scratchBuf = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = nullptr;
        VmaAllocationInfo scratchAllocResult {};
        res = vmaCreateBuffer(memAllocator, &scratchBufInfo, &scratchAllocInfo, &scratchBuf, &scratchAlloc, &scratchAllocResult);
        if(res != VK_SUCCESS || scratchBuf == VK_NULL_HANDLE){
            std::cerr << "Vulkan accel struct scratch buffer creation failed (" << res << ")" << std::endl;
            return nullptr;
        }
        VkBufferView nullView2 = VK_NULL_HANDLE;
        auto scratchBuffer = std::shared_ptr<GEVulkanBuffer>(
            new GEVulkanBuffer(BufferDescriptor::GPUOnly, this, scratchBuf, nullView2, scratchAlloc, scratchAllocResult));
        trackResource(scratchBuffer);

        VkAccelerationStructureCreateInfoKHR asCreateInfo {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        asCreateInfo.buffer = structBuf;
        asCreateInfo.size = structSize;
        asCreateInfo.type = buildInfo.type;

        VkAccelerationStructureKHR accelStruct = VK_NULL_HANDLE;
        res = vkCreateAccelerationStructureKhr(device, &asCreateInfo, nullptr, &accelStruct);
        if(res != VK_SUCCESS || accelStruct == VK_NULL_HANDLE){
            std::cerr << "Vulkan acceleration structure creation failed (" << res << ")" << std::endl;
            return nullptr;
        }

        auto result = std::shared_ptr<GEVulkanAccelerationStruct>(
            new GEVulkanAccelerationStruct(this, accelStruct, structBuffer, scratchBuffer));
        trackResource(result);
        return result;
    }

    void GEVulkanEngine::releaseAllTrackedResources(){
        for(auto & tracked : trackedResources){
            if(tracked.ref.lock()){
                tracked.releaseFn(tracked.rawPtr);
            }
        }
        trackedResources.clear();
    }

    void GEVulkanEngine::waitForGPUIdle(){
        if(device != VK_NULL_HANDLE){
            vkDeviceWaitIdle(device);
        }
    }

    bool GEVulkanEngine::debugReadbackPixelRGBA8(SharedHandle<GETexture> tex, unsigned x, unsigned y, std::uint8_t out[4]){
        if(device == VK_NULL_HANDLE || !tex){
            std::cerr << "[debugReadback] no device/tex" << std::endl;
            return false;
        }
        auto vkTex = (GEVulkanTexture *)tex.get();
        if(vkTex->img == VK_NULL_HANDLE){
            std::cerr << "[debugReadback] no image" << std::endl;
            return false;
        }

        if(deviceQueuefamilies.empty() || deviceQueuefamilies[0].empty()){
            std::cerr << "[debugReadback] no queue" << std::endl;
            return false;
        }
        std::uint32_t qfi = queueFamilyIndices.empty() ? 0 : queueFamilyIndices[0];
        // CommandQueue-Typed-Pool follow-up — debug readback runs as
        // MEDIUM background work; same reasoning as uploadQueue. The
        // pre-Phase-2 code grabbed `[0][0].second` directly, which was
        // MEDIUM in that world but is HIGH in this one.
        VkQueue queue = lookupQueueOnFamily(qfi,
                                            static_cast<std::int32_t>(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR));
        if (queue == VK_NULL_HANDLE) {
            queue = deviceQueuefamilies[0][0].second;
        }

        VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo {};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

        VkBuffer stageBuf = VK_NULL_HANDLE;
        VmaAllocation stageAlloc = nullptr;
        VmaAllocationInfo stageAllocInfo {};
        if(vmaCreateBuffer(memAllocator, &bufInfo, &allocInfo, &stageBuf, &stageAlloc, &stageAllocInfo) != VK_SUCCESS){
            std::cerr << "[debugReadback] vmaCreateBuffer failed" << std::endl;
            return false;
        }

        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = qfi;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if(vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS){
            vmaDestroyBuffer(memAllocator, stageBuf, stageAlloc);
            return false;
        }

        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cbAlloc {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAlloc.commandPool = pool;
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cbAlloc, &cb);

        VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &begin);

        VkImageLayout origLayout = vkTex->layout;
        VkImageMemoryBarrier toSrc {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = origLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = vkTex->img;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy region {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
        region.imageExtent = { 1, 1, 1 };
        vkCmdCopyImageToBuffer(cb, vkTex->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stageBuf, 1, &region);

        VkImageMemoryBarrier toOrig = toSrc;
        toOrig.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toOrig.dstAccessMask = 0;
        toOrig.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toOrig.newLayout = origLayout;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toOrig);

        VkBufferMemoryBarrier toHost {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = stageBuf;
        toHost.offset = 0;
        toHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, nullptr, 1, &toHost, 0, nullptr);

        vkEndCommandBuffer(cb);

        VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        VkResult subRes = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if(subRes != VK_SUCCESS){
            std::cerr << "[debugReadback] submit failed: " << subRes << std::endl;
        }
        vkQueueWaitIdle(queue);

        std::uint8_t *mapped = (std::uint8_t *)stageAllocInfo.pMappedData;
        if(mapped != nullptr){
            out[0] = mapped[0];
            out[1] = mapped[1];
            out[2] = mapped[2];
            out[3] = mapped[3];
        }

        vkFreeCommandBuffers(device, pool, 1, &cb);
        vkDestroyCommandPool(device, pool, nullptr);
        vmaDestroyBuffer(memAllocator, stageBuf, stageAlloc);
        return mapped != nullptr;
    }

    GEVulkanEngine::~GEVulkanEngine(){
        if(device != VK_NULL_HANDLE){
            // Wait until every queue on the device is idle. With Vulkan we
            // get a single device-wide call instead of D3D12's per-queue
            // Signal+Wait. After this returns every retention gate (which
            // queries vkGetSemaphoreCounterValue on a per-queue timeline)
            // would report signaled.
            vkDeviceWaitIdle(device);

            // Run resource destructors. After slice 3 these enqueue VMA
            // destroys onto retentionQueue rather than calling them inline.
            releaseAllTrackedResources();

            // Drain unconditionally. The vkDeviceWaitIdle above is the
            // promise that every gate is signaled; drainAll skips the
            // gate queries that drainCompleted would do.
            retentionQueue.drainAll();

            // Vulkan-Texture-Memory-Plan Phase 1 teardown. Must run
            // after releaseAllTrackedResources — texture destructors
            // can still invoke the staging upload path during release
            // (though current callers only upload at creation, defensive
            // ordering matches D3D12).
            if(uploadFence != VK_NULL_HANDLE){
                vkDestroyFence(device, uploadFence, nullptr);
                uploadFence = VK_NULL_HANDLE;
            }
            if(uploadCommandPool != VK_NULL_HANDLE){
                vkDestroyCommandPool(device, uploadCommandPool, nullptr);
                uploadCommandPool = VK_NULL_HANDLE;
            }

            for(auto & qf : deviceQueuefamilies){
                for(auto & q : qf){
                    if(q.first != VK_NULL_HANDLE){
                        vkDestroySemaphore(device,q.first,nullptr);
                    }
                }
            }
            // Allocator-Lifetime-Hardening Phase 2 ordering contract. By here
            // every VMA allocation is already freed: releaseAllTrackedResources()
            // above freed the still-live caller-held resources inline, and
            // retentionQueue.drainAll() ran the deferred destroys of resources
            // dropped before Close(). So the allocator is torn down with no live
            // allocations (no VMA "allocations not freed" assert). This MUST run
            // before vkDestroyDevice below — vmaDestroyAllocator operates on the
            // device, and (unlike D3D12MA) VMA does not hold the device alive.
            if(memAllocator != nullptr){
                vmaDestroyAllocator(memAllocator);
                memAllocator = nullptr;
            }
            vkDestroyDevice(device,nullptr);
            device = VK_NULL_HANDLE;
        } else if(memAllocator != nullptr){
            vmaDestroyAllocator(memAllocator);
            memAllocator = nullptr;
        }
    }



_NAMESPACE_END_
