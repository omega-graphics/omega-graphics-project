
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
#include "../common/GERetentionQueue.h"

#include <functional>
#include <mutex>

#ifndef OMEGAGTE_VULKAN_GEVULKAN_H
#define OMEGAGTE_VULKAN_GEVULKAN_H



_NAMESPACE_BEGIN_
    struct GTEVulkanDevice;
    class GEVulkanTexture;
    #define VK_RESULT_SUCCEEDED(val) (val == VK_SUCCESS)
    class GEVulkanEngine : public OmegaGraphicsEngine {

        SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime) override;

        VkPipelineLayout createPipelineLayoutFromShaderDescs(unsigned shaderN,
                                                             omegasl_shader *shaders,
                                                             VkDescriptorPool * descriptorPool,
                                                             OmegaCommon::Vector<VkDescriptorSet> & descs,
                                                             OmegaCommon::Vector<VkDescriptorSetLayout> & descLayout,
                                                             OmegaCommon::Vector<VkSampler> & outImmutableSamplers);
    public:
        static VkInstance instance;

        bool hasPushDescriptorExt = false;
        bool hasSynchronization2Ext = false;
        bool hasExtendedDynamicState = false;

        bool hasAccelerationStructureExt = false;
        bool hasRayTracingPipelineExt = false;
        bool hasBufferDeviceAddressExt = false;
        bool hasDeferredHostOperationsExt = false;
        /// Mesh-Shader-Plan Phase 4a — `VK_EXT_mesh_shader`. Enabled
        /// at device creation when present; gates whether
        /// `vkCmdDrawMeshTasksExt` is loaded and whether the
        /// `VkPhysicalDeviceMeshShaderFeaturesEXT { meshShader = TRUE }`
        /// chain is added to `vkCreateDevice`. Separate from the
        /// detection-time `GTEDEVICE_FEATURE_MESH_SHADER` capability
        /// bit — that one reports "device supports it"; this one
        /// records "we asked the driver to turn it on".
        bool hasMeshShaderExt = false;

        /// CommandQueue-Typed-Pool follow-up — `VK_KHR_global_priority` is
        /// enabled on the device. Drives whether the device-create loop
        /// chained `VkDeviceQueueGlobalPriorityCreateInfoKHR` per
        /// `VkDeviceQueueCreateInfo` and whether `openedQueuePriorities`
        /// has meaningful contents. False on drivers that don't expose the
        /// extension; queues fall back to a single MEDIUM VkQueue per
        /// family.
        bool hasGlobalPriorityExt = false;

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
        /// Mesh-Shader-Plan Phase 4a — `vkCmdDrawMeshTasksEXT` is an
        /// extension entry point so it must be loaded via
        /// `vkGetDeviceProcAddr` (not statically linked). Loaded after
        /// device creation when `hasMeshShaderExt` is true; nullptr
        /// otherwise. `GEVulkanCommandBuffer::drawMeshTasks` reaches
        /// this through `parentQueue->engine->vkCmdDrawMeshTasksExt`.
        PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksExt = nullptr;

        VmaAllocator memAllocator;
        unsigned resource_count;

        // GPU-safe deferred-release queue. Encoders / submit paths hand
        // resources here gated on per-queue timeline-semaphore values; entries
        // are released at drainCompleted() time, after the GPU is provably
        // done with them. See gte/docs/GPU-Safe-Resource-Deletion-Plan.md.
        Retention::Queue retentionQueue;
    
        VkDevice device;
        VkPhysicalDevice physicalDevice;

        OmegaCommon::Vector<OmegaCommon::Vector<std::pair<VkSemaphore,VkQueue>>> deviceQueuefamilies;

        /// CommandQueue-Typed-Pool follow-up — global-priority lookup table.
        /// `openedQueuePriorities[i][j]` is the `VkQueueGlobalPriorityKHR`
        /// that `deviceQueuefamilies[i][j]` was created with. When
        /// `hasGlobalPriorityExt` is false this stays empty and every
        /// priority lookup falls through to the single MEDIUM queue at
        /// `deviceQueuefamilies[i][0]`. Storage type uses `int32_t` to
        /// keep the header free of the global-priority enum (it lives in
        /// `vulkan_core.h`, already included by every consumer of this
        /// header but kept opaque here so the type stays self-describing).
        OmegaCommon::Vector<OmegaCommon::Vector<std::int32_t>> openedQueuePriorities;

        VkSurfaceCapabilitiesKHR capabilitiesKhr;

        OmegaCommon::Vector<VkQueueFamilyProperties> queueFamilyProps;

        OmegaCommon::Vector<std::uint32_t> queueFamilyIndices;

        /// CommandQueue-Typed-Pool follow-up — find the VkQueue on
        /// `familyIndex` whose creation global priority best matches
        /// `wantedKhrPriority` (a `VkQueueGlobalPriorityKHR` cast to
        /// `int32_t` so the helper can stay declared in the header). The
        /// fallback ladder is REALTIME→HIGH→MEDIUM→LOW for ascending
        /// requests and the symmetric descending ladder for LOW (since LOW
        /// without an opened LOW queue should not silently promote). When
        /// `hasGlobalPriorityExt` is false, returns the single MEDIUM
        /// queue at `deviceQueuefamilies[slot][0]`. Returns
        /// `VK_NULL_HANDLE` if the family was not opened at all.
        VkQueue lookupQueueOnFamily(std::uint32_t familyIndex,
                                    std::int32_t wantedKhrPriority) const;

        /// CommandQueue-Typed-Pool Phase 2 — resolve a raw Vulkan
        /// queue-family index to the corresponding slot inside
        /// `deviceQueuefamilies` (and parallel index inside
        /// `queueFamilyIndices`). Returns `(VK_NULL_HANDLE, 0)` when the
        /// family was not opened at device-create time, which lets the
        /// caller treat the resolution as a soft miss instead of crashing.
        /// The first element is `deviceQueuefamilies[slot].front().second`
        /// — the VkQueue itself — for convenience.
        std::pair<VkQueue, std::uint32_t> queueForFamily(std::uint32_t familyIndex) const {
            for (std::uint32_t i = 0; i < queueFamilyIndices.size(); ++i) {
                if (queueFamilyIndices[i] == familyIndex) {
                    if (i < deviceQueuefamilies.size() && !deviceQueuefamilies[i].empty()) {
                        return {deviceQueuefamilies[i].front().second, i};
                    }
                    return {VK_NULL_HANDLE, i};
                }
            }
            return {VK_NULL_HANDLE, 0};
        }

        // Vulkan-Texture-Memory-Plan Phase 1. Persistent command pool +
        // fence used by GEVulkanTexture::copyBytes / getBytes to drive
        // the synchronous staging-buffer upload/readback path. The pool
        // is TRANSIENT (every command buffer is one-shot) and is bound
        // to the queue family of `uploadQueue` so vkBeginCommandBuffer
        // doesn't have to look up a fresh pool per call (as
        // debugReadbackPixelRGBA8 still does — that path predates this
        // infra and could be migrated in a follow-up).
        //
        // The plan called for a ring of pools/buffers; since the API
        // contract is synchronous (Non-Goals §1), there is never more
        // than one upload in flight, so a single pool + single fence
        // are sufficient. A mutex serializes concurrent copyBytes
        // calls from different threads against the shared pool/fence —
        // expected contention is near-zero for current callers (texture
        // cache populates synchronously during recording).
        VkCommandPool uploadCommandPool = VK_NULL_HANDLE;
        VkFence       uploadFence       = VK_NULL_HANDLE;
        VkQueue       uploadQueue       = VK_NULL_HANDLE;
        std::uint32_t uploadQueueFamily = 0;
        std::mutex    uploadMutex;

        /// Submit a one-shot transfer that copies `tex.stagingBuffer` ->
        /// `tex.img`, transitions the image to `SHADER_READ_ONLY_OPTIMAL`,
        /// and waits for the submission to complete. Used by
        /// GEVulkanTexture::copyBytes for `ToGPU` textures. Returns
        /// false on submission failure (caller falls back to reporting
        /// the failure via DEBUG_STREAM; the texture is left
        /// undefined-contents but allocated).
        bool submitImmediateUploadFromStaging(GEVulkanTexture &tex);

        /// §7.1 variant: upload only the supplied subresource copy regions
        /// instead of the whole pre-computed chain. The region copyBytes
        /// overload uses this so a single-subresource upload doesn't clobber
        /// the other (still-unwritten) subresources sitting in the staging
        /// buffer. The image-layout barrier still spans the whole image,
        /// which is safe — untouched subresources transition SHADER_READ ->
        /// TRANSFER_DST -> SHADER_READ without losing their contents.
        bool submitImmediateUploadFromStaging(GEVulkanTexture &tex,
                                              const VkBufferImageCopy *regions,
                                              std::uint32_t regionCount);

        /// Mirror of submitImmediateUploadFromStaging for `FromGPU`:
        /// transitions `tex.img` -> `TRANSFER_SRC_OPTIMAL`, copies into
        /// `tex.stagingBuffer`, restores the prior layout, and waits.
        /// Used by GEVulkanTexture::getBytes.
        bool submitImmediateReadbackToStaging(GEVulkanTexture &tex);

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

        SharedHandle<GECommandQueue> makeCommandQueue(const GECommandQueueDesc & desc) override;

        SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor &desc) override;

        SharedHandle<GEFence> makeFence() override;

        SharedHandle<GEHeap> makeHeap(const HeapDescriptor &desc) override;

        SharedHandle<GETexture> makeTexture(const TextureDescriptor &desc) override;

        SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor &desc) override;

        SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor &desc) override;

        SharedHandle<GEBlitPipelineState> makeBlitPipelineState(BlitPipelineDescriptor &desc) override;

        /// Mesh-Shader-Plan Phase 3 — public API stub. Feature-gates +
        /// validates shaders + logs + returns nullptr. Phase 4a lands
        /// the real `VkGraphicsPipeline` build with
        /// `VK_SHADER_STAGE_MESH_BIT_EXT` (and optional
        /// `VK_SHADER_STAGE_TASK_BIT_EXT`).
        SharedHandle<GERenderPipelineState> makeMeshPipelineState(MeshPipelineDescriptor &desc) override;

        // Extension 3: cached built-in full-screen-triangle vertex shader,
        // shared by every blit pipeline created on this engine. Lazily
        // compiled on the first makeBlitPipelineState call.
        SharedHandle<GTEDevice> gteDevice;
        SharedHandle<GTEShader> blitFullscreenVs;
        std::shared_ptr<omegasl_shader_lib> blitFullscreenVsLib;
        bool ensureBlitFullscreenVs();

        SharedHandle<GENativeRenderTarget> makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc,
                                                                   SharedHandle<GECommandQueue> presentQueue) override;

        SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc) override;

        bool debugReadbackPixelRGBA8(SharedHandle<GETexture> tex, unsigned x, unsigned y, std::uint8_t out[4]) override;

        SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) override;

        SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) override;
        SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor &desc) override;

        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);

        void waitForGPUIdle() override;
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

        // Gates accumulated by encoders that bound this buffer. The buffer
        // must outlive every gate before vmaDestroyBuffer can run.
        OmegaCommon::Vector<Retention::FenceGate> pendingGates;

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
            if(nativeReleased_) return;
            // Hand the VMA destroy + buffer-view destroy to the engine
            // retention queue gated on every prior submit that touched this
            // buffer. Encoders accumulate these gates via pendingGates. If no
            // submission ever used the buffer, gates is empty and the release
            // runs on the next drainCompleted (i.e. effectively immediate),
            // matching the previous inline-destroy behavior for unused
            // resources.
            if(engine != nullptr){
                VkBuffer     b   = buffer;
                VkBufferView bv  = bufferView;
                VmaAllocation a  = alloc;
                VmaAllocator alc = engine->memAllocator;
                VkDevice     dev = engine->device;
                std::vector<Retention::FenceGate> gates(pendingGates.begin(), pendingGates.end());
                engine->retentionQueue.enqueue(std::move(gates),
                    [alc, dev, b, bv, a]() {
                        vmaDestroyBuffer(alc, b, a);
                        if (bv != VK_NULL_HANDLE) vkDestroyBufferView(dev, bv, nullptr);
                    });
                buffer = VK_NULL_HANDLE;
                bufferView = VK_NULL_HANDLE;
                alloc = nullptr;
            }
        };
    };

    class GEVulkanFence : public GEFence {
        bool nativeReleased_ = false;
    public:
        GEVulkanEngine *engine;

        VkFence fence;

        VkEvent event = VK_NULL_HANDLE;

        /// Texture-fence ordering guard. CPU-side mirror of the producer
        /// having recorded a `vkCmdSetEvent` (the "signal") that a later
        /// `vkCmdWaitEvents` ("wait") has not yet consumed. The two-arg
        /// `submitCommandBuffer(cb, fence)` sets it true; `notifyCommandBuffer`
        /// only records the wait when it is true, then clears it. This mirrors
        /// the `lastSignaledValue > 0` guard in the D3D12 backend and the
        /// `waitValue > 0` guard in Metal: when the producing render is skipped
        /// (e.g. a content-cache hit reuses an already-rendered texture), the
        /// event is never set, so the wait must NOT be recorded — waiting on a
        /// never-set VkEvent is the spec violation that crashes Vulkan.
        bool eventSignalRecorded = false;

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
