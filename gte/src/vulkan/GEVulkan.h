
#ifdef VULKAN_TARGET_X11
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif

#ifdef VULKAN_TARGET_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#endif

#ifdef VULKAN_TARGET_ANDROID
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif



#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "omegaGTE/GE.h"

#ifndef OMEGAGTE_VULKAN_GEVULKAN_H
#define OMEGAGTE_VULKAN_GEVULKAN_H



_NAMESPACE_BEGIN_
    struct GTEVulkanDevice;
    #define VK_RESULT_SUCCEEDED(val) (val == VK_SUCCESS)
    class GEVulkanEngine : public OmegaGraphicsEngine {

        SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) override;

        VkPipelineLayout createPipelineLayoutFromShaderDescs(unsigned shaderN,
                                                             omegasl_shader *shaders,
                                                             VkDescriptorPool * descriptorPool,
                                                             OmegaCommon::Vector<VkDescriptorSet> & descs,
                                                             OmegaCommon::Vector<VkDescriptorSetLayout> & descLayout);
    public:
        static VkInstance instance;

        bool hasPushDescriptorExt = false;
        bool hasSynchronization2Ext = false;
        bool hasExtendedDynamicState = false;

        PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKhr;
        PFN_vkCmdSetPrimitiveTopologyEXT vkCmdSetPrimitiveTopologyExt;
        PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2Khr;

        VmaAllocator memAllocator;
        unsigned resource_count;
    
        VkDevice device;
        VkPhysicalDevice physicalDevice;

        OmegaCommon::Vector<OmegaCommon::Vector<std::pair<VkSemaphore,VkQueue>>> deviceQueuefamilies;

        VkSurfaceCapabilitiesKHR capabilitiesKhr;

        OmegaCommon::Vector<VkQueueFamilyProperties> queueFamilyProps;

        OmegaCommon::Vector<std::uint32_t> queueFamilyIndices;

        explicit GEVulkanEngine(SharedHandle<GTEVulkanDevice> device);

        SharedHandle<GECommandQueue> makeCommandQueue(unsigned int maxBufferCount) override;

        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;

        SharedHandle<GEFence> makeFence() override;

        SharedHandle<GEHeap> makeHeap(const HeapDescriptor &desc) override;

        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override;

        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc) override;

        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc) override;

        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc) override;

        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc) override;

        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override;

        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);

        ~GEVulkanEngine();
    };

    class GEVulkanBuffer : public GEBuffer {
    public:
        GEVulkanEngine *engine;

        VkBuffer buffer;
        VkBufferView bufferView;

        VmaAllocation alloc;
        VmaAllocationInfo alloc_info;

        void setName(OmegaCommon::StrRef name) override {
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_BUFFER;
            nameInfoExt.objectHandle = buffer;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        }

        void *native() override {
            return (void *)buffer;
        }

        /// Sync 2.0 Extension

        VkAccessFlags2KHR priorAccess2;
        VkPipelineStageFlags2KHR priorPipelineAccess2;

        // Standard Sync


        VkAccessFlags priorAccess;
        VkPipelineStageFlags priorPipelineAccess;

        size_t size() override {
            return alloc_info.size;
        };
        explicit GEVulkanBuffer(
            const BufferDescriptor::Usage & usage,
            GEVulkanEngine *engine,
            VkBuffer & buffer,
            VkBufferView &view,
            VmaAllocation alloc, 
            VmaAllocationInfo alloc_info):GEBuffer(usage),engine(engine),buffer(buffer),
            bufferView(view),alloc(alloc),alloc_info(alloc_info){

        };
        ~GEVulkanBuffer() override{
            vmaDestroyBuffer(engine->memAllocator,buffer,alloc);
            vkDestroyBufferView(engine->device,bufferView,nullptr);
        };
    };

    class GEVulkanFence : public GEFence {
    public:
        GEVulkanEngine *engine;

        VkFence fence;

        VkEvent event;

        void setName(OmegaCommon::StrRef name) override {
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_FENCE;
            nameInfoExt.objectHandle = fence;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        }

        void *native() override {
            return (void *)fence;
        }

        GEVulkanFence(GEVulkanEngine *engine,VkFence fence):engine(engine),fence(fence){

        }
        ~GEVulkanFence() {
            vkDestroyFence(engine->device,fence,nullptr);
        }
    };

    class GEVulkanSamplerState : public GESamplerState {
    public:
        GEVulkanEngine *engine;

        VkSampler sampler;

        GEVulkanSamplerState(GEVulkanEngine *engine,VkSampler sampler):engine(engine),sampler(sampler){

        }
        ~GEVulkanSamplerState(){
            vkDestroySampler(engine->device,sampler,nullptr);
        };
    };
    
_NAMESPACE_END_

#endif