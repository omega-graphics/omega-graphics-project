
#include <cstdint>
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
#include "../common/GEResourceTracker.h"

#include <functional>

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

        bool hasAccelerationStructureExt = false;
        bool hasRayTracingPipelineExt = false;
        bool hasBufferDeviceAddressExt = false;
        bool hasDeferredHostOperationsExt = false;

        PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKhr;
        PFN_vkCmdSetPrimitiveTopologyEXT vkCmdSetPrimitiveTopologyExt;
        PFN_vkCmdPipelineBarrier2KHR vkCmdPipelineBarrier2Khr;

        PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKhr = nullptr;
        PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKhr = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKhr = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKhr = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKhr = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKhr = nullptr;
        PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKhr = nullptr;
        PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKhr = nullptr;

        VmaAllocator memAllocator;
        unsigned resource_count;
    
        VkDevice device;
        VkPhysicalDevice physicalDevice;

        OmegaCommon::Vector<OmegaCommon::Vector<std::pair<VkSemaphore,VkQueue>>> deviceQueuefamilies;

        VkSurfaceCapabilitiesKHR capabilitiesKhr;

        OmegaCommon::Vector<VkQueueFamilyProperties> queueFamilyProps;

        OmegaCommon::Vector<std::uint32_t> queueFamilyIndices;

        struct TrackedResource {
            std::weak_ptr<void> ref;
            void *rawPtr;
            void (*releaseFn)(void *ptr);
        };
        OmegaCommon::Vector<TrackedResource> trackedResources;

        template<typename T>
        void trackResource(const std::shared_ptr<T> &res) {
            trackedResources.push_back({res, res.get(),
                [](void *ptr){ static_cast<T*>(ptr)->releaseNative(); }});
        }
        void releaseAllTrackedResources();

        explicit GEVulkanEngine(SharedHandle<GTEVulkanDevice> device);

        void * underlyingNativeDevice() override;

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

        #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override;
        SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) override;
        #endif

        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);

        ~GEVulkanEngine();
    };

    class GEVulkanBuffer : public GEBuffer {
        bool nativeReleased_ = false;
    public:
        GEVulkanEngine *engine;
        std::uint64_t traceResourceId = 0;

        VkBuffer buffer;
        VkBufferView bufferView;

        VmaAllocation alloc;
        VmaAllocationInfo alloc_info;

        void setName(OmegaCommon::StrRef name) override {
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_BUFFER;
            nameInfoExt.objectHandle = (uint64_t)buffer;
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
            traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
            ResourceTracking::Tracker::instance().emit(
                    ResourceTracking::EventType::Create,
                    ResourceTracking::Backend::Vulkan,
                    "Buffer",
                    traceResourceId,
                    reinterpret_cast<const void *>(buffer),
                    static_cast<float>(alloc_info.size));
        };
        void releaseNative(){
            if(nativeReleased_) return;
            nativeReleased_ = true;
            vmaDestroyBuffer(engine->memAllocator,buffer,alloc);
            vkDestroyBufferView(engine->device,bufferView,nullptr);
            buffer = VK_NULL_HANDLE;
            bufferView = VK_NULL_HANDLE;
            alloc = nullptr;
        }
        ~GEVulkanBuffer() override{
            ResourceTracking::Tracker::instance().emit(
                    ResourceTracking::EventType::Destroy,
                    ResourceTracking::Backend::Vulkan,
                    "Buffer",
                    traceResourceId,
                    reinterpret_cast<const void *>(buffer),
                    static_cast<float>(alloc_info.size));
            if(!nativeReleased_){
                vmaDestroyBuffer(engine->memAllocator,buffer,alloc);
                vkDestroyBufferView(engine->device,bufferView,nullptr);
            }
        };
    };

    class GEVulkanFence : public GEFence {
        bool nativeReleased_ = false;
    public:
        GEVulkanEngine *engine;

        VkFence fence;

        VkEvent event = VK_NULL_HANDLE;

        void setName(OmegaCommon::StrRef name) override {
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_FENCE;
            nameInfoExt.objectHandle = (uint64_t)fence;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        }

        void *native() override {
            return (void *)fence;
        }

        GEVulkanFence(GEVulkanEngine *engine,VkFence fence,VkEvent event = VK_NULL_HANDLE):engine(engine),fence(fence),event(event){

        }
        void releaseNative(){
            if(nativeReleased_) return;
            nativeReleased_ = true;
            if(event != VK_NULL_HANDLE){
                vkDestroyEvent(engine->device,event,nullptr);
                event = VK_NULL_HANDLE;
            }
            if(fence != VK_NULL_HANDLE){
                vkDestroyFence(engine->device,fence,nullptr);
                fence = VK_NULL_HANDLE;
            }
        }
        ~GEVulkanFence() {
            if(!nativeReleased_){
                if(event != VK_NULL_HANDLE){
                    vkDestroyEvent(engine->device,event,nullptr);
                }
                if(fence != VK_NULL_HANDLE){
                    vkDestroyFence(engine->device,fence,nullptr);
                }
            }
        }
    };

    class GEVulkanHeap : public GEHeap {
        GEVulkanEngine *engine;
        VmaPool pool;
        size_t heapSize;
        bool nativeReleased_ = false;
    public:
        GEVulkanHeap(GEVulkanEngine *engine, VmaPool pool, size_t heapSize)
            : engine(engine), pool(pool), heapSize(heapSize) {}

        size_t currentSize() override { return heapSize; }

        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;
        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override;

        void releaseNative();
        ~GEVulkanHeap();
    };

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED
    class GEVulkanAccelerationStruct : public GEAccelerationStruct {
        bool nativeReleased_ = false;
    public:
        GEVulkanEngine *engine;
        VkAccelerationStructureKHR accelStruct;
        SharedHandle<GEVulkanBuffer> structBuffer;
        SharedHandle<GEVulkanBuffer> scratchBuffer;

        void setName(OmegaCommon::StrRef name){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
            nameInfoExt.objectHandle = (uint64_t)accelStruct;
            nameInfoExt.pObjectName = name.data();
            vkSetDebugUtilsObjectNameEXT(engine->device,&nameInfoExt);
        }

        explicit GEVulkanAccelerationStruct(
            GEVulkanEngine *engine,
            VkAccelerationStructureKHR accelStruct,
            SharedHandle<GEVulkanBuffer> & structBuffer,
            SharedHandle<GEVulkanBuffer> & scratchBuffer)
            :engine(engine),accelStruct(accelStruct),
             structBuffer(structBuffer),scratchBuffer(scratchBuffer){}

        void releaseNative(){
            if(nativeReleased_) return;
            nativeReleased_ = true;
            if(accelStruct != VK_NULL_HANDLE && engine != nullptr && engine->vkDestroyAccelerationStructureKhr != nullptr){
                engine->vkDestroyAccelerationStructureKhr(engine->device, accelStruct, nullptr);
                accelStruct = VK_NULL_HANDLE;
            }
            structBuffer.reset();
            scratchBuffer.reset();
        }
        ~GEVulkanAccelerationStruct() override {
            if(!nativeReleased_){
                if(accelStruct != VK_NULL_HANDLE && engine != nullptr && engine->vkDestroyAccelerationStructureKhr != nullptr){
                    engine->vkDestroyAccelerationStructureKhr(engine->device, accelStruct, nullptr);
                }
            }
        }
    };
    #endif

    class GEVulkanSamplerState : public GESamplerState {
        bool nativeReleased_ = false;
    public:
        GEVulkanEngine *engine;

        VkSampler sampler;

        GEVulkanSamplerState(GEVulkanEngine *engine,VkSampler sampler):engine(engine),sampler(sampler){

        }
        void releaseNative(){
            if(nativeReleased_) return;
            nativeReleased_ = true;
            vkDestroySampler(engine->device,sampler,nullptr);
            sampler = VK_NULL_HANDLE;
        }
        ~GEVulkanSamplerState(){
            if(!nativeReleased_){
                vkDestroySampler(engine->device,sampler,nullptr);
            }
        };
    };
    
_NAMESPACE_END_

#endif
