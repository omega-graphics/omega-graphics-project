

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
#include <vector>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <string>

#include "OmegaGTE.h"

#include "omega-common/common.h"

#include <glm/glm.hpp>

#include "../BufferIO.h"


_NAMESPACE_BEGIN_

    VkInstance GEVulkanEngine::instance = nullptr;

    bool vulkanInit = false;

    bool initVulkan(){
        if(vulkanInit && GEVulkanEngine::instance != VK_NULL_HANDLE){
            return true;
        }

        GEVulkanEngine::instance = VK_NULL_HANDLE;

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
        instanceInfo.enabledLayerCount = 0;
        instanceInfo.ppEnabledLayerNames = nullptr;
        instanceInfo.enabledExtensionCount = static_cast<uint32_t>(requiredInstanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = requiredInstanceExtensions.data();

        auto createRes = vkCreateInstance(&instanceInfo,nullptr,&GEVulkanEngine::instance);
        if(createRes != VK_SUCCESS || GEVulkanEngine::instance == VK_NULL_HANDLE){
            std::cerr << "Vulkan init failed: vkCreateInstance returned " << createRes << std::endl;
            GEVulkanEngine::instance = VK_NULL_HANDLE;
            return false;
        }

        vulkanInit = true;
        return true;
    }



    void cleanupVulkan(){
        if(GEVulkanEngine::instance != VK_NULL_HANDLE){
            vkDestroyInstance(GEVulkanEngine::instance,nullptr);
            GEVulkanEngine::instance = VK_NULL_HANDLE;
        }
        vulkanInit = false;
    }

    struct GTEVulkanDevice : public GTEDevice {
        VkPhysicalDevice device;
        GTEVulkanDevice(GTEDevice::Type type,const char *name,GTEDeviceFeatures & features,VkPhysicalDevice &device) : GTEDevice(type,name,features),device(device) {

        };
        const void * native() override{
            return (void *)device;
        }
        ~GTEVulkanDevice() override = default;
    };

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
            GTEDeviceFeatures features {false};
            GTEDevice::Type type = GTEDevice::Discrete;
            if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
                type = GTEDevice::Discrete;
            }
            else if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){
                type = GTEDevice::Integrated;
            }
            devs.emplace_back(SharedHandle<GTEDevice>(new GTEVulkanDevice(type,props.deviceName,features,dev)));
        }
        return devs;
    }

    typedef unsigned char VulkanByte;

    class GEVulkanBufferWriter : public GEBufferWriter {
        GEVulkanBuffer *_buffer = nullptr;
        VulkanByte *mem_map = nullptr;
        size_t currentOffset = 0;

        bool inStruct = false;

        OmegaCommon::Vector<DataBlock> blocks;
    public:
        void setOutputBuffer(SharedHandle<GEBuffer> &buffer) override {
            _buffer = (GEVulkanBuffer *)buffer.get();
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

        void sendToBuffer() override {
            assert(!inStruct && "Struct record must be finished before sending object to buffer");
            assert(mem_map != nullptr && "Output buffer must be mapped before sending data");
            size_t biggestWord = 1;
            bool afterBiggest = false;
            for(auto & b : blocks){
                size_t si = 0;
                switch (b.type) {
                    case OMEGASL_FLOAT : {
                        si = sizeof(float);
                        break;
                    }
                    case OMEGASL_FLOAT2 : {
                        si = sizeof(glm::vec2);
                        break;
                    }
                    case OMEGASL_FLOAT3 : {
                        si = sizeof(glm::vec3);
                        break;
                    }
                    case OMEGASL_FLOAT4 : {
                        si = sizeof(glm::vec4);
                        break;
                    }
                }
                memcpy(mem_map + currentOffset,b.data,si);
                currentOffset += si;
            }
        }

        void flush() override {
            vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            _buffer = nullptr;
        }
    };

    SharedHandle<GEBufferWriter> GEBufferWriter::Create() {
        return SharedHandle<GEBufferWriter>(new GEVulkanBufferWriter());
    }

    class GEVulkanBufferReader : public GEBufferReader {
        GEVulkanBuffer *_buffer = nullptr;
        VulkanByte *mem_map = nullptr;
        size_t currentOffset = 0;
    public:
        void setInputBuffer(SharedHandle<GEBuffer> &buffer) override {
            _buffer = (GEVulkanBuffer *)buffer.get();
            vmaMapMemory(_buffer->engine->memAllocator,_buffer->alloc,(void **)&mem_map);
            currentOffset = 0;
        }
        void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields) override {

        }
        void structBegin() override {

        }
        void structEnd() override {

        }
        void getFloat(float &v) override {
            memcpy(&v,mem_map + currentOffset,sizeof(v));
            currentOffset += sizeof(v);
        }
        void getFloat2(FVec<2> &v) override {
            glm::vec2 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            currentOffset += sizeof(vec);
        }
        void getFloat3(FVec<3> &v) override {
            glm::vec3 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            currentOffset += sizeof(vec);
        }
        void getFloat4(FVec<4> &v) override {
            glm::vec4 vec;
            memcpy(&vec,mem_map + currentOffset,sizeof(vec));
            v[0][0] = vec.x;
            v[1][0] = vec.y;
            v[2][0] = vec.z;
            v[3][0] = vec.w;
            currentOffset += sizeof(vec);
        }
        void reset() override {
            vmaUnmapMemory(_buffer->engine->memAllocator,_buffer->alloc);
            _buffer = nullptr;
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
        return SharedHandle<GTEShader>(new GTEVulkanShader(this,*shaderDesc,module));
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
        // Keep push descriptors disabled until layouts are created with PUSH_DESCRIPTOR flags.
        enableDeviceExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,false);
        hasPushDescriptorExt = false;
        // Keep sync2 disabled until VkPhysicalDeviceSynchronization2Features is explicitly enabled.
        enableDeviceExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,false);
        hasSynchronization2Ext = false;
        // Keep topology dynamic state disabled until feature negotiation is wired.
        enableDeviceExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,false);
        hasExtendedDynamicState = false;

        count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,nullptr);
        queueFamilyProps.resize(count);

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,queueFamilyProps.data());

        OmegaCommon::Vector<VkDeviceQueueCreateInfo> deviceQueues;
        const float queuePriority = 1.f;
        unsigned id = 0;
        for(auto & q : queueFamilyProps){
            if(q.queueFlags & VK_QUEUE_GRAPHICS_BIT || q.queueFlags & VK_QUEUE_COMPUTE_BIT){
                queueFamilyIndices.push_back(id);
                VkDeviceQueueCreateInfo queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queueInfo.pNext = nullptr;
                queueInfo.queueFamilyIndex = id;
                queueInfo.queueCount = 1;
                queueInfo.pQueuePriorities = &queuePriority;
                queueInfo.flags = 0;
                deviceQueues.push_back(queueInfo);
            }
            else {
                continue;
            }
             ++id;
        }
        if(deviceQueues.empty()){
            std::cerr << "No Vulkan queue families support graphics/compute." << std::endl;
            std::exit(1);
        }

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(physicalDevice,&features);

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext = nullptr;
        info.pQueueCreateInfos = deviceQueues.data();
        info.queueCreateInfoCount = deviceQueues.size();
        info.enabledExtensionCount = extensionNames.size();
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        info.ppEnabledExtensionNames = extensionNames.data();
        info.pEnabledFeatures = &features;

        auto deviceRes = vkCreateDevice(physicalDevice,&info,nullptr,&this->device);
        if(deviceRes != VK_SUCCESS || this->device == VK_NULL_HANDLE){
            std::cerr << "Failed to create Vulkan logical device (" << deviceRes << ")" << std::endl;
            std::exit(1);
        }

        for(auto & dq : deviceQueues){
            std::vector<std::pair<VkSemaphore,VkQueue>> queues;
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
            }
            if(!queues.empty()){
                deviceQueuefamilies.push_back(queues);
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


        VmaAllocatorCreateInfo allocator_info {};
        allocator_info.instance = instance;
        allocator_info.device = this->device;
        allocator_info.physicalDevice = physicalDevice;
        allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
        auto _res = vmaCreateAllocator(&allocator_info,&memAllocator);

        if(_res != VK_SUCCESS){
            printf("Failed to Create Allocator");
            exit(1);
        };
      

        resource_count = 0;
       
        DEBUG_STREAM("Successfully Created GEVulkanEngine");
    };

    void * GEVulkanEngine::underlyingNativeDevice(){
        return (void *)device;
    }

    SharedHandle<OmegaGraphicsEngine> GEVulkanEngine::Create(SharedHandle<GTEDevice> & device){
        return SharedHandle<OmegaGraphicsEngine>(new GEVulkanEngine(std::dynamic_pointer_cast<GTEVulkanDevice>(device)));
    };

    SharedHandle<GECommandQueue> GEVulkanEngine::makeCommandQueue(unsigned int maxBufferCount){
        return std::make_shared<GEVulkanCommandQueue>(this,maxBufferCount);
    };

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
                            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
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

        return SharedHandle<GEBuffer>(new GEVulkanBuffer(desc.usage,this,buffer,bufferView,allocation,allocationInfo));
    };
    GEVulkanHeap::~GEVulkanHeap(){
        if(pool != VK_NULL_HANDLE){
            vmaDestroyPool(engine->memAllocator, pool);
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
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo {};
        allocInfo.pool = pool;
        switch(desc.usage){
            case BufferDescriptor::Upload: allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU; break;
            case BufferDescriptor::Readback: allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU; break;
            case BufferDescriptor::GPUOnly: allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY; break;
        }

        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VmaAllocationInfo allocationInfo {};
        auto res = vmaCreateBuffer(engine->memAllocator, &bufferInfo, &allocInfo, &buffer, &alloc, &allocationInfo);
        if(!VK_RESULT_SUCCEEDED(res) || buffer == VK_NULL_HANDLE){
            return nullptr;
        }

        VkBufferView bufferView = VK_NULL_HANDLE;
        return SharedHandle<GEBuffer>(new GEVulkanBuffer(desc.usage, engine, buffer, bufferView, alloc, allocationInfo));
    }

    SharedHandle<GETexture> GEVulkanHeap::makeTexture(const TextureDescriptor &desc){
        VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.flags = 0;

        VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        switch(desc.pixelFormat){
            case TexturePixelFormat::RGBA8Unorm: imageFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
            case TexturePixelFormat::RGBA16Unorm: imageFormat = VK_FORMAT_R16G16B16A16_UNORM; break;
            case TexturePixelFormat::RGBA8Unorm_SRGB: imageFormat = VK_FORMAT_R8G8B8A8_SRGB; break;
        }
        imageInfo.format = imageFormat;

        VkImageType type = VK_IMAGE_TYPE_2D;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
        switch(desc.type){
            case GETexture::Texture1D: type = VK_IMAGE_TYPE_1D; viewType = VK_IMAGE_VIEW_TYPE_1D; break;
            case GETexture::Texture2D: type = VK_IMAGE_TYPE_2D; viewType = VK_IMAGE_VIEW_TYPE_2D; break;
            case GETexture::Texture3D: type = VK_IMAGE_TYPE_3D; viewType = VK_IMAGE_VIEW_TYPE_3D; break;
        }
        imageInfo.imageType = type;
        imageInfo.extent = {desc.width, desc.height, desc.depth};
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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
        viewInfo.subresourceRange.levelCount = desc.mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imgView = VK_NULL_HANDLE;
        vkCreateImageView(engine->device, &viewInfo, nullptr, &imgView);

        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY;

        return SharedHandle<GETexture>(new GEVulkanTexture(
            desc.type, desc.usage, desc.pixelFormat,
            engine, img, imgView, layout, allocationInfo, alloc, desc, memUsage));
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

        return SharedHandle<GEHeap>(new GEVulkanHeap(this, pool, desc.len));
    };

    SharedHandle<GETexture>GEVulkanEngine::makeTexture(const TextureDescriptor &desc){
        if(device == VK_NULL_HANDLE || memAllocator == nullptr){
            std::cerr << "Vulkan makeTexture failed: invalid Vulkan device or allocator." << std::endl;
            return nullptr;
        }

        VkImageCreateInfo image_desc {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_desc.pNext = nullptr;
        image_desc.flags = 0;

        VkFormat image_format;

        switch (desc.pixelFormat) {
            case TexturePixelFormat::RGBA8Unorm : {
                image_format = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            }
            case TexturePixelFormat::RGBA16Unorm : {
                image_format = VK_FORMAT_R16G16B16A16_UNORM;
                break;
            }
            case TexturePixelFormat::RGBA8Unorm_SRGB : {
                image_format = VK_FORMAT_R8G8B8A8_SRGB;
                break;
            }
            default: {
                image_format = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            }
        }

        image_desc.format = image_format;

        VkImageType type;
        VkImageViewType viewType;
        
        switch (desc.type) {
            case GETexture::Texture1D : {
                type = VK_IMAGE_TYPE_1D;
                viewType = VK_IMAGE_VIEW_TYPE_1D;
                break;
            }
            case GETexture::Texture2D : {
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            }
            case GETexture::Texture3D : {
                type = VK_IMAGE_TYPE_3D;
                viewType = VK_IMAGE_VIEW_TYPE_3D;
                break;
            }
            default: {
                type = VK_IMAGE_TYPE_2D;
                viewType = VK_IMAGE_VIEW_TYPE_2D;
                break;
            }
        }

        const unsigned width = desc.width > 0 ? desc.width : 1;
        const unsigned height = desc.height > 0 ? desc.height : 1;
        const unsigned depth = desc.depth > 0 ? desc.depth : 1;
        const unsigned mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;

        image_desc.imageType = type;
        image_desc.extent.width = width;
        image_desc.extent.height = height;
        image_desc.extent.depth = depth;
        image_desc.mipLevels = mipLevels;
        image_desc.arrayLayers = 1;
        image_desc.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        switch (desc.sampleCount) {
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
                usageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT 
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT 
                | VK_IMAGE_USAGE_SAMPLED_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                break;
            }
            case GETexture::ToGPU : {
                usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                memoryUsage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        
                break;
            }
            case GETexture::FromGPU : {
                usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_TO_CPU;
         
                break;
            }
            case GETexture::RenderTargetAndDepthStencil : {
                usageFlags =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
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
                usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
            std::cerr << "Vulkan makeTexture failed: vmaCreateImage returned " << imageRes << std::endl;
            return nullptr;
        }
        

        VkImageViewCreateInfo image_view_desc {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        image_view_desc.pNext = nullptr;

        image_view_desc.viewType = viewType;
        image_view_desc.image = image;
        image_view_desc.flags = 0;
        image_view_desc.format = image_format;
        image_view_desc.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        image_view_desc.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        image_view_desc.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        image_view_desc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        image_view_desc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        image_view_desc.subresourceRange.baseMipLevel = 0;
        image_view_desc.subresourceRange.levelCount = mipLevels;
        image_view_desc.subresourceRange.baseArrayLayer = 0;
        image_view_desc.subresourceRange.layerCount = 1;

        auto viewRes = vkCreateImageView(device,&image_view_desc,nullptr,&imageView);
        if(viewRes != VK_SUCCESS || imageView == VK_NULL_HANDLE){
            std::cerr << "Vulkan makeTexture failed: vkCreateImageView returned " << viewRes << std::endl;
            vmaDestroyImage(memAllocator,image,alloc);
            return nullptr;
        }

        

        TextureDescriptor sanitizedDesc = desc;
        sanitizedDesc.width = width;
        sanitizedDesc.height = height;
        sanitizedDesc.depth = depth;
        sanitizedDesc.mipLevels = mipLevels;
        return SharedHandle<GETexture>(new GEVulkanTexture(
                sanitizedDesc.type,
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
    };


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
                                                                         OmegaCommon::Vector<VkDescriptorSetLayout> & descLayout){
        if(descriptorPool != nullptr){
            *descriptorPool = VK_NULL_HANDLE;
        }
        descs.clear();
        descLayout.clear();

        VkPipelineLayoutCreateInfo layout_info {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pNext = nullptr;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;

        OmegaCommon::ArrayRef<omegasl_shader> shadersArr {shaders,shaders + shaderN};

        OmegaCommon::Vector<VkDescriptorPoolSize> poolSizes;
        std::uint32_t setCount = 0;

        for(auto & s : shadersArr){
            VkShaderStageFlags shaderStageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            if(s.type == OMEGASL_SHADER_VERTEX){
                shaderStageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            }
            else if(s.type == OMEGASL_SHADER_FRAGMENT){
                shaderStageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }

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
                    case OMEGASL_SHADER_SAMPLER1D_DESC:
                    case OMEGASL_SHADER_SAMPLER2D_DESC:
                    case OMEGASL_SHADER_SAMPLER3D_DESC: {
                        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                        break;
                    }
                    case OMEGASL_SHADER_TEXTURE1D_DESC:
                    case OMEGASL_SHADER_TEXTURE2D_DESC:
                    case OMEGASL_SHADER_TEXTURE3D_DESC: {
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

        VkDescriptorPoolCreateInfo poolCreateInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCreateInfo.maxSets = setCount > 0 ? setCount : 1;
        poolCreateInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        poolCreateInfo.pPoolSizes = poolSizes.data();

        auto poolRes = vkCreateDescriptorPool(device,&poolCreateInfo,nullptr,descriptorPool);
        if(poolRes != VK_SUCCESS || *descriptorPool == VK_NULL_HANDLE){
            std::cerr << "Vulkan descriptor pool creation failed (" << poolRes << ")" << std::endl;
            return pipeline_layout;
        }

        VkDescriptorSetAllocateInfo descSetAllocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descSetAllocInfo.descriptorSetCount = static_cast<std::uint32_t>(descLayout.size());
        descSetAllocInfo.pSetLayouts = descLayout.data();
        descSetAllocInfo.pNext = nullptr;
        descSetAllocInfo.descriptorPool = *descriptorPool;

        descs.resize(descLayout.size());
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
        if(desc.vertexFunc == nullptr || desc.fragmentFunc == nullptr){
            std::cerr << "Vulkan render pipeline creation failed: missing shader functions." << std::endl;
            return nullptr;
        }

        omegasl_shader shaders[] = {desc.vertexFunc->internal,desc.fragmentFunc->internal};

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;
        
        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

        VkPipelineLayout layout = createPipelineLayoutFromShaderDescs(2,shaders,&descriptorPool,descs,descLayouts);
        if(layout == VK_NULL_HANDLE){
            for(auto & descLayout : descLayouts){
                if(descLayout != VK_NULL_HANDLE){
                    vkDestroyDescriptorSetLayout(device,descLayout,nullptr);
                }
            }
            if(descriptorPool != VK_NULL_HANDLE){
                vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            }
            return nullptr;
        }

        VkAttachmentDescription colorAttachment {};
        colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpassDesc {};
        subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassDesc.colorAttachmentCount = 1;
        subpassDesc.pColorAttachments = &colorRef;

        VkSubpassDependency dependency {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpassDesc;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

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
            return nullptr;
        }

        VkGraphicsPipelineCreateInfo createInfo {};
        createInfo.basePipelineHandle = VK_NULL_HANDLE;
        createInfo.basePipelineIndex = -1;
        createInfo.layout = layout;
        createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createInfo.renderPass = compatibilityRenderPass;
        createInfo.subpass = 0;

        VkPipelineVertexInputStateCreateInfo vertexInputState {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInputState.vertexBindingDescriptionCount = 0;
        vertexInputState.pVertexBindingDescriptions = nullptr;
        vertexInputState.vertexAttributeDescriptionCount = 0;
        vertexInputState.pVertexAttributeDescriptions = nullptr;

        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssemblyState.primitiveRestartEnable = VK_FALSE;
        inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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

        if(desc.polygonFrontFaceRotation == GTEPolygonFrontFaceRotation::Clockwise){
            rasterState.frontFace = VK_FRONT_FACE_CLOCKWISE;
        }
        else {
            rasterState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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

        VkPipelineColorBlendAttachmentState colorBlendAttachment {};
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                              VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT |
                                              VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlendState {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlendState.logicOpEnable = VK_FALSE;
        colorBlendState.attachmentCount = 1;
        colorBlendState.pAttachments = &colorBlendAttachment;

        VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        OmegaCommon::Vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        if(hasExtendedDynamicState){
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
        
        VkPipelineShaderStageCreateInfo stages[] = {vertexStage,fragmentStage};

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
        createInfo.stageCount = 2;
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
      
        return SharedHandle<GERenderPipelineState>(new GEVulkanRenderPipelineState(desc.vertexFunc,
                                                                                   desc.fragmentFunc,
                                                                                   this,
                                                                                   pipeline,
                                                                                   compatibilityRenderPass,
                                                                                   layout,
                                                                                   descriptorPool,
                                                                                   descs,
                                                                                   descLayouts));
    };
    SharedHandle<GEComputePipelineState> GEVulkanEngine::makeComputePipelineState(ComputePipelineDescriptor &desc){

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;
        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool;

        VkPipelineLayout pipeline_layout = createPipelineLayoutFromShaderDescs(1,&desc.computeFunc->internal,&descriptorPool,descs,descLayouts);

        

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
         

         VkPipeline pipeline;

        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_PIPELINE;
            nameInfoExt.objectHandle = (uint64_t)pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }
         auto result = vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_desc,nullptr,&pipeline);
         if(!VK_RESULT_SUCCEEDED(result)){
            exit(1);
        };

        VkDescriptorSet descSet = descs.empty() ? VK_NULL_HANDLE : descs.front();
        return SharedHandle<GEComputePipelineState>(new GEVulkanComputePipelineState(desc.computeFunc,
                                                                                     this,
                                                                                     pipeline,
                                                                                     pipeline_layout,
                                                                                     descriptorPool,
                                                                                     descSet,
                                                                                     descLayouts));
    };

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

        return SharedHandle<GEFence>(new GEVulkanFence(this,fence,event));
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

        return SharedHandle<GESamplerState>(new GEVulkanSamplerState(this,sampler));
    }

    SharedHandle<GENativeRenderTarget> GEVulkanEngine::makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc){
        if(instance == VK_NULL_HANDLE || device == VK_NULL_HANDLE || queueFamilyIndices.empty()){
            std::cerr << "Vulkan native render target creation failed: Vulkan engine is not initialized." << std::endl;
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

        VkSurfaceFormatKHR selectedSurfaceFormat = surfaceFormats[0];
        for(auto &formatCandidate : surfaceFormats){
            if(formatCandidate.format == VK_FORMAT_R8G8B8A8_UNORM){
                selectedSurfaceFormat = formatCandidate;
                break;
            }
            if(formatCandidate.format == VK_FORMAT_R8G8B8A8_SRGB){
                selectedSurfaceFormat = formatCandidate;
            }
        }

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

        return SharedHandle<GENativeRenderTarget>(new GEVulkanNativeRenderTarget(
                this,
                surfaceKhr,
                swapchainKhr,
                selectedSurfaceFormat.format,
                mipLevels,
                swapExtent));
    };

    SharedHandle<GETextureRenderTarget> GEVulkanEngine::makeTextureRenderTarget(const TextureRenderTargetDescriptor &desc){
        SharedHandle<GETexture> tex;
        if(!desc.renderToExistingTexture){
            TextureDescriptor texDesc {GETexture::Texture2D,OmegaGTE::Shared,GETexture::GPUAccessOnly};
            tex = makeTexture(texDesc);
        }
        else {
            tex = desc.texture;
        }

        auto vk_tex = std::dynamic_pointer_cast<GEVulkanTexture>(tex);

        VkFramebuffer fb = VK_NULL_HANDLE;

        return SharedHandle<GETextureRenderTarget>(new GEVulkanTextureRenderTarget(this,vk_tex,fb));
    };

    GEVulkanEngine::~GEVulkanEngine(){
        if(device != VK_NULL_HANDLE){
            for(auto & qf : deviceQueuefamilies){
                for(auto & q : qf){
                    if(q.first != VK_NULL_HANDLE){
                        vkDestroySemaphore(device,q.first,nullptr);
                    }
                }
            }
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
