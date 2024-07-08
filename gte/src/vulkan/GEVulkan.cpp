

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

#include "OmegaGTE.h"

#include <glm/glm.hpp>

#include "../BufferIO.h"


_NAMESPACE_BEGIN_

    VkInstance GEVulkanEngine::instance = nullptr;

    bool vulkanInit = false;

    void initVulkan(){
        OmegaCommon::Vector<VkLayerProperties> layers;
        uint32_t count;
        vkEnumerateInstanceLayerProperties(&count,nullptr);
        layers.resize(count);
        vkEnumerateInstanceLayerProperties(&count,layers.data());
        OmegaCommon::Vector<VkExtensionProperties> extensions;
         OmegaCommon::Vector<char *> layerNames,extensionNames; 
        for(auto & layer : layers){
            layerNames.push_back(layer.layerName);
            vkEnumerateInstanceExtensionProperties(layer.layerName,&count,nullptr);
            auto oldSize = extensions.size();
            extensions.resize(oldSize + count);
            vkEnumerateInstanceExtensionProperties(layer.layerName,&count,extensions.data() + oldSize);
        }

        for(auto & ext : extensions){
            extensionNames.push_back(ext.extensionName);
        }

        uint32_t apiVersion;

        vkEnumerateInstanceVersion(&apiVersion);

        VkApplicationInfo appInfo {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        appInfo.apiVersion = apiVersion;
        appInfo.applicationVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.engineVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.pNext = nullptr;
        appInfo.pEngineName = "OmegaGTE";


        VkInstanceCreateInfo instanceInfo {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pNext = nullptr;
        instanceInfo.flags = 0;
        instanceInfo.pApplicationInfo = &appInfo;
        instanceInfo.enabledLayerCount = layerNames.size();
        instanceInfo.ppEnabledLayerNames = layerNames.data();
        instanceInfo.enabledExtensionCount = extensionNames.size();
        instanceInfo.ppEnabledExtensionNames = extensionNames.data();

        vkCreateInstance(&instanceInfo,nullptr,&GEVulkanEngine::instance);
        vulkanInit = true;
    }



    void cleanupVulkan(){
        vkDestroyInstance(GEVulkanEngine::instance,nullptr);
        vulkanInit = false;
    }

    struct GTEVulkanDevice : public GTEDevice {
        VkPhysicalDevice device;
        GTEVulkanDevice(GTEDevice::Type type,const char *name,GTEDeviceFeatures & features,VkPhysicalDevice &device) : GTEDevice(type,name,features),device(device) {

        };
        ~GTEVulkanDevice() override = default;
    };

    OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
        OmegaCommon::Vector<SharedHandle<GTEDevice>> devs;
        if(!vulkanInit){
            initVulkan();
        }
        OmegaCommon::Vector<VkPhysicalDevice> vk_devs;
        std::uint32_t device_count;
        vkEnumeratePhysicalDevices(GEVulkanEngine::instance,&device_count,nullptr);
        vk_devs.resize(device_count);
        vkEnumeratePhysicalDevices(GEVulkanEngine::instance,&device_count,vk_devs.data());
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
            assert(inStruct && "Struct Record must be finished before sending object to buffer");
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
        VkShaderModuleCreateInfo shaderModuleDesc {};

        shaderModuleDesc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleDesc.pNext = nullptr;
        shaderModuleDesc.pCode = (std::uint32_t *)shaderDesc->data;
        shaderModuleDesc.codeSize = shaderDesc->dataSize;
        shaderModuleDesc.flags = 0;

        VkShaderModule module;
        vkCreateShaderModule(device,&shaderModuleDesc,nullptr,&module);
        return SharedHandle<GTEShader>(new GTEVulkanShader(this,*shaderDesc,module));
    }


    GEVulkanEngine::GEVulkanEngine(SharedHandle<GTEVulkanDevice> device){

        physicalDevice = device->device;

        std::uint32_t count;

        std::vector<char *> extensionNames,layerNames;

        OmegaCommon::Vector<VkLayerProperties> layer_props;
        count = 0;
        vkEnumerateDeviceLayerProperties(physicalDevice,&count,nullptr);
        layer_props.resize(count);
        vkEnumerateDeviceLayerProperties(physicalDevice,&count,layer_props.data());

        VkResult res;
        OmegaCommon::Vector<VkExtensionProperties> ext_props;

        for(auto layer : layer_props) {

            layerNames.push_back(layer.layerName);

            vkEnumerateDeviceExtensionProperties(physicalDevice, layer.layerName, &count, nullptr);
            auto prevSize = ext_props.size();
            ext_props.resize(prevSize + count);
            vkEnumerateDeviceExtensionProperties(physicalDevice, layer.layerName, &count, ext_props.data() + prevSize);

        }


        count = 0;

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,nullptr);
        queueFamilyProps.resize(count);

        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice,&count,queueFamilyProps.data());

        OmegaCommon::Vector<VkDeviceQueueCreateInfo> deviceQueues;
        unsigned id = 0;
        for(auto & q : queueFamilyProps){
            if(q.queueFlags & VK_QUEUE_GRAPHICS_BIT || q.queueFlags & VK_QUEUE_COMPUTE_BIT){
                queueFamilyIndices.push_back(id);
                VkDeviceQueueCreateInfo queueInfo {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                queueInfo.pNext = nullptr;
                queueInfo.queueFamilyIndex = id;
                queueInfo.queueCount = q.queueCount;
                deviceQueues.push_back(queueInfo);
            }
            else {
                continue;
            }
             ++id;
        }

        for(auto & ext : ext_props){
            extensionNames.push_back(ext.extensionName);
            auto str_ref = StrRef(ext.extensionName);
            if(str_ref == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME){
                hasPushDescriptorExt = true;
            }
            else if(str_ref == VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME){
                hasSynchronization2Ext = true;
            }
            else if(str_ref == VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME){
                hasExtendedDynamicState = true;
            }
        }

        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(physicalDevice,&features);

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext = nullptr;
        info.pQueueCreateInfos = deviceQueues.data();
        info.queueCreateInfoCount = deviceQueues.size();
        info.enabledExtensionCount = ext_props.size();
        info.enabledLayerCount = layer_props.size();
        info.ppEnabledLayerNames = layerNames.data();
        info.ppEnabledExtensionNames = extensionNames.data();
        info.pEnabledFeatures = &features;

        vkCreateDevice(physicalDevice,&info,nullptr,&this->device);

        unsigned queueFamilyIndex = 0;
        for(auto & dq : deviceQueues){
            std::vector<std::pair<VkSemaphore,VkQueue>> queues;
            for(unsigned i = 0;i < dq.queueCount;i++){
                VkSemaphore sem;
                VkQueue queue;

                VkSemaphoreCreateInfo semaphoreInfo {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
                semaphoreInfo.flags = VK_SEMAPHORE_TYPE_TIMELINE;
                semaphoreInfo.pNext = nullptr;
                vkCreateSemaphore(this->device,&semaphoreInfo,nullptr,&sem);

                VkDeviceQueueInfo2 queueInfo {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                queueInfo.pNext = nullptr;
                queueInfo.queueFamilyIndex = queueFamilyIndex;
                queueInfo.queueIndex = i;
                queueInfo.flags = 0;
                vkGetDeviceQueue2(this->device,&queueInfo,&queue);

                queues.emplace_back(std::make_pair(sem,queue));
            }
            deviceQueuefamilies.push_back(queues);
            queueFamilyIndex += 1;

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

    SharedHandle<OmegaGraphicsEngine> GEVulkanEngine::Create(SharedHandle<GTEDevice> & device){
        return SharedHandle<OmegaGraphicsEngine>(new GEVulkanEngine(std::dynamic_pointer_cast<GTEVulkanDevice>(device)));
    };

    SharedHandle<GECommandQueue> GEVulkanEngine::makeCommandQueue(unsigned int maxBufferCount){
        return std::make_shared<GEVulkanCommandQueue>(this,maxBufferCount);
    };

    SharedHandle<GEBuffer> GEVulkanEngine::makeBuffer(const BufferDescriptor &desc){
        VkBufferCreateInfo buffer_desc;
       
        buffer_desc.flags = 0;
        buffer_desc.size = desc.len;
        buffer_desc.sharingMode = VK_SHARING_MODE_CONCURRENT;

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
        VmaAllocation allocation;
        VmaAllocationInfo allocationInfo;

        VkBuffer buffer;
        
        vmaCreateBuffer(memAllocator,&buffer_desc,&alloc_info,&buffer,&allocation,&allocationInfo);

        VkBufferViewCreateInfo bufferViewInfo {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
        bufferViewInfo.format = VK_FORMAT_UNDEFINED;
        bufferViewInfo.buffer = buffer;
        bufferViewInfo.pNext = nullptr;
        bufferViewInfo.offset = 0;
        bufferViewInfo.range = VK_WHOLE_SIZE;

        VkBufferView bufferView;

        vkCreateBufferView(device,&bufferViewInfo,nullptr,&bufferView);

       
        return SharedHandle<GEBuffer>(new GEVulkanBuffer(desc.usage,this,buffer,bufferView,allocation,allocationInfo));
    };
    SharedHandle<GEHeap> GEVulkanEngine::makeHeap(const HeapDescriptor &desc){
        return nullptr;
    };

    SharedHandle<GETexture>GEVulkanEngine::makeTexture(const TextureDescriptor &desc){
        VkImageCreateInfo image_desc;
        image_desc.queueFamilyIndexCount = queueFamilyIndices.size();
        image_desc.pQueueFamilyIndices = queueFamilyIndices.data();

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
        }

        image_desc.imageType = type;
        image_desc.extent.width = desc.width;
        image_desc.extent.height = desc.height;
        image_desc.extent.depth = desc.depth;
        image_desc.mipLevels = desc.mipLevels;
        image_desc.arrayLayers = 1;

        VkImageUsageFlags usageFlags = 0;
        VkImageLayout layout;
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
                layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        
                break;
            }
            case GETexture::FromGPU : {
                usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         
                break;
            }
            case GETexture::RenderTargetAndDepthStencil :
                usageFlags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            case GETexture::RenderTarget : {
                usageFlags = 
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | 
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;

                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                layout = VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR;
                
                break;
            }
            case GETexture::MSResolveSrc : {
                usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY;
                layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                
                break;
            }
        }

        image_desc.usage = usageFlags;
        image_desc.initialLayout = layout;

        image_desc.sharingMode = VK_SHARING_MODE_CONCURRENT;
        
    
        VkImage image;
        VkImageView imageView;
        

        VmaAllocationCreateInfo create_alloc_info {};
        create_alloc_info.usage = memoryUsage;
        VmaAllocation alloc;
        VmaAllocationInfo alloc_info;

        vmaCreateImage(memAllocator,&image_desc,&create_alloc_info,&image,&alloc,&alloc_info);
        

        VkImageViewCreateInfo image_view_desc;

        image_view_desc.viewType = viewType;
        image_view_desc.image = image;
        image_view_desc.flags = 0;
        image_view_desc.format = image_format;

        vkCreateImageView(device,&image_view_desc,nullptr,&imageView);

        

        return SharedHandle<GETexture>(new GEVulkanTexture(desc.type,desc.usage,desc.pixelFormat,this,image,imageView,layout,alloc_info,alloc,desc,memoryUsage));
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
        VkPipelineLayoutCreateInfo layout_info {};

        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pNext = nullptr;

        VkDescriptorSetLayoutCreateInfo desc_layout_info{};
        VkDescriptorSetLayoutBinding b;
        b.descriptorCount = 1;

        OmegaCommon::ArrayRef<omegasl_shader> shadersArr {shaders,shaders + shaderN};

        VkDescriptorPoolCreateInfo poolCreateInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};

        std::uint32_t setCount = 0;

        OmegaCommon::Vector<VkDescriptorPoolSize> poolSizes;

        OmegaCommon::Vector<unsigned> resourceIDs;

        for(auto & s : shadersArr){
            VkShaderStageFlags shaderStageFlags;

            if(s.type == OMEGASL_SHADER_VERTEX){
                shaderStageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            }
            else if(s.type == OMEGASL_SHADER_FRAGMENT){
                shaderStageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            else {
                shaderStageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }

            OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layouts {s.pLayout,s.pLayout + s.nLayout};
            VkDescriptorSetLayout set_layout;
            OmegaCommon::Vector<VkDescriptorSetLayoutBinding> bindings;
            for(auto & l : layouts){
                b.pImmutableSamplers = nullptr;
                switch (l.type) {
                    case OMEGASL_SHADER_BUFFER_DESC : {
                        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        break;
                    }
                    case OMEGASL_SHADER_SAMPLER1D_DESC : 
                    case OMEGASL_SHADER_SAMPLER2D_DESC :
                    case OMEGASL_SHADER_SAMPLER3D_DESC : {
                        b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                        break;
                    }
                    case OMEGASL_SHADER_TEXTURE1D_DESC :
                    case OMEGASL_SHADER_TEXTURE2D_DESC :
                    case OMEGASL_SHADER_TEXTURE3D_DESC : {
                        if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN){
                            b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        }
                        else {
                            b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        }
                        break;
                    }
                }
                b.binding = l.gpu_relative_loc;
                resourceIDs.push_back(l.location);
                b.stageFlags = shaderStageFlags;
                setCount += 1;

                VkDescriptorPoolSize poolSize {};
                poolSize.descriptorCount = bindings.size();
                poolSize.type = b.descriptorType;
                poolSizes.push_back(poolSize);
            }
            desc_layout_info.pNext = nullptr;
            desc_layout_info.bindingCount = bindings.size();
            desc_layout_info.pBindings = bindings.data();
            desc_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            vkCreateDescriptorSetLayout(device,&desc_layout_info,nullptr,&set_layout);

        }

        layout_info.pSetLayouts = descLayout.data();
        layout_info.setLayoutCount = descLayout.size();
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;

        VkPipelineLayout pipeline_layout;

        vkCreatePipelineLayout(device,&layout_info,nullptr,&pipeline_layout);

        poolCreateInfo.maxSets = setCount;
        poolCreateInfo.pPoolSizes = poolSizes.data();
        poolCreateInfo.poolSizeCount = poolSizes.size();

        vkCreateDescriptorPool(device,&poolCreateInfo,nullptr,descriptorPool);

        VkDescriptorSetAllocateInfo descSetAllocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descSetAllocInfo.descriptorSetCount = descLayout.size();
        descSetAllocInfo.pSetLayouts = descLayout.data();
        descSetAllocInfo.pNext = nullptr;
        descSetAllocInfo.descriptorPool = *descriptorPool;

        descs.resize(descLayout.size());

        vkAllocateDescriptorSets(device,&descSetAllocInfo,descs.data());

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
        
        omegasl_shader shaders[] = {desc.vertexFunc->internal,desc.fragmentFunc->internal};

        OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;
        
        OmegaCommon::Vector<VkDescriptorSet> descs;
        VkDescriptorPool descriptorPool;

        VkPipelineLayout layout = createPipelineLayoutFromShaderDescs(2,shaders,&descriptorPool,descs,descLayouts);

        VkGraphicsPipelineCreateInfo createInfo {};
        createInfo.basePipelineHandle = VK_NULL_HANDLE;
        createInfo.basePipelineIndex = -1;
        createInfo.layout = layout;
        createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

        /// Rasterizer State
        VkPipelineRasterizationStateCreateInfo rasterState {};
        rasterState.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
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




        VkPipelineDynamicStateCreateInfo dynamicState {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        OmegaCommon::Vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        dynamicState.dynamicStateCount = dynamicStates.size();
        dynamicState.pNext = nullptr;
        dynamicState.pDynamicStates = dynamicStates.data();


        auto *vertexShader = (GTEVulkanShader *)desc.vertexFunc.get();
        VkPipelineShaderStageCreateInfo vertexStage {};
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertexShader->shaderModule;
        vertexStage.pName = "main";

        auto *fragmentShader = (GTEVulkanShader *)desc.fragmentFunc.get();
        VkPipelineShaderStageCreateInfo fragmentStage {};
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragmentShader->shaderModule;
        fragmentStage.pName = "main";
        
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
        createInfo.pDepthStencilState = &depthStencilStateDesc;

        VkPipeline pipeline;

        vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&createInfo,nullptr,&pipeline);

        if(desc.name.size() > 0){
            VkDebugUtilsObjectNameInfoEXT nameInfoExt {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
            nameInfoExt.pNext = nullptr;
            nameInfoExt.objectType = VK_OBJECT_TYPE_PIPELINE;
            nameInfoExt.objectHandle = pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }
      
        return SharedHandle<GERenderPipelineState>(new GEVulkanRenderPipelineState(desc.vertexFunc,
                                                                                   desc.fragmentFunc,
                                                                                   this,
                                                                                   pipeline,
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
            nameInfoExt.objectHandle = pipeline;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }
         auto result = vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_desc,nullptr,&pipeline);
         if(!VK_RESULT_SUCCEEDED(result)){
            exit(1);
        };

        return SharedHandle<GEComputePipelineState>(new GEVulkanComputePipelineState(desc.computeFunc,
                                                                                     this,
                                                                                     pipeline,
                                                                                     pipeline_layout,
                                                                                     descriptorPool,
                                                                                     descs.front(),
                                                                                     descLayouts));
    };

    SharedHandle<GEFence> GEVulkanEngine::makeFence(){
        
        VkFenceCreateInfo fenceCreateInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        fenceCreateInfo.pNext = nullptr;

        VkFence fence;
        vkCreateFence(device,&fenceCreateInfo,nullptr,&fence);

        return SharedHandle<GEFence>(new GEVulkanFence(this,fence));
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
            nameInfoExt.objectHandle = sampler;
            nameInfoExt.pObjectName = desc.name.data();
            vkSetDebugUtilsObjectNameEXT(device,&nameInfoExt);
        }

        return SharedHandle<GESamplerState>(new GEVulkanSamplerState(this,sampler));
    }

    SharedHandle<GENativeRenderTarget> GEVulkanEngine::makeNativeRenderTarget(const NativeRenderTargetDescriptor &desc){
        VkSurfaceKHR surfaceKhr;

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
        bool wantsWayland = desc.wl_display != nullptr && desc.wl_surface != nullptr;
        VkBool32 waylandSupported = VK_FALSE;
        if(wantsWayland) {
            VkWaylandSurfaceCreateInfoKHR infoKhr{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
            infoKhr.pNext = nullptr;
            infoKhr.surface = desc.wl_surface;
            infoKhr.display = desc.wl_display;
            infoKhr.flags = 0;
            vkCreateWaylandSurfaceKHR(instance, &infoKhr, nullptr, &surfaceKhr);


            vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndices[0], surfaceKhr, &waylandSupported);
        }
        if(waylandSupported == VK_FALSE || !wantsWayland) {
            /// If Wayland is not supported by the Physical Device, use X11 window instead
#endif

#ifdef VK_USE_PLATFORM_XLIB_KHR
            VkXlibSurfaceCreateInfoKHR xlibSurfaceCreateInfoKhr{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
            xlibSurfaceCreateInfoKhr.pNext = nullptr;
            xlibSurfaceCreateInfoKhr.flags = 0;
            xlibSurfaceCreateInfoKhr.window = desc.x_window;
            xlibSurfaceCreateInfoKhr.dpy = desc.x_display;
            vkCreateXlibSurfaceKHR(instance, &xlibSurfaceCreateInfoKhr, nullptr, &surfaceKhr);
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
            VkAndroidSurfaceCreateInfoKHR androidSurfaceCreateInfoKhr {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            androidSurfaceCreateInfoKhr.window = desc.window;
            androidSurfaceCreateInfoKhr.pNext = nullptr;
            androidSurfaceCreateInfoKhr.flags = 0;
#endif

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice,surfaceKhr,&capabilitiesKhr);

        OmegaCommon::Vector<VkSurfaceFormatKHR> surfaceFormats;
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surfaceKhr,&count,nullptr);

        surfaceFormats.resize(count);

        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice,surfaceKhr,&count,surfaceFormats.data());

        OmegaCommon::Vector<VkPresentModeKHR> presentModes;

        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,surfaceKhr,&count,nullptr);

        presentModes.resize(count);

        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,surfaceKhr,&count,presentModes.data());

        VkPresentModeKHR presentModeKhr;

        for(auto mode : presentModes){
            if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR){
                presentModeKhr = mode;
                break;
            }
            else if(mode == VK_PRESENT_MODE_FIFO_KHR){
                presentModeKhr = mode;
                break;
            }
        }

        VkSwapchainKHR swapchainKhr;

        VkSwapchainCreateInfoKHR swapchainInfo {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surfaceKhr;
        swapchainInfo.pNext = nullptr;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.clipped = VK_FALSE;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainInfo.imageFormat = surfaceFormats[0].format;
        swapchainInfo.imageColorSpace = surfaceFormats[0].colorSpace;
        swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchainInfo.presentMode = presentModeKhr;
        swapchainInfo.imageExtent = capabilitiesKhr.currentExtent;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainInfo.minImageCount = 2;
        swapchainInfo.preTransform = capabilitiesKhr.currentTransform;
        swapchainInfo.queueFamilyIndexCount = queueFamilyIndices.size();
        swapchainInfo.pQueueFamilyIndices = queueFamilyIndices.data();


        vkCreateSwapchainKHR(device,&swapchainInfo,nullptr,&swapchainKhr);

        unsigned mipLevels = 0;

        return SharedHandle<GENativeRenderTarget>(new GEVulkanNativeRenderTarget(this,surfaceKhr,swapchainKhr,surfaceFormats[0].format,mipLevels,capabilitiesKhr.currentExtent));
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

        VkFramebuffer fb;

        VkFramebufferCreateInfo fbInfo {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = VK_NULL_HANDLE;
        fbInfo.pNext = nullptr;
        fbInfo.width = desc.rect.w;
        fbInfo.height = desc.rect.h;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &vk_tex->img_view;
        fbInfo.layers = 1;
        fbInfo.flags = 0;

        vkCreateFramebuffer(device,&fbInfo,nullptr, &fb);

        return SharedHandle<GETextureRenderTarget>(new GEVulkanTextureRenderTarget(this,vk_tex,fb));
    };

    GEVulkanEngine::~GEVulkanEngine(){
        for(auto & qf : deviceQueuefamilies){
            for(auto & q : qf){
                vkDestroySemaphore(device,q.first,nullptr);
            }
        }
        vkDestroyDevice(device,nullptr);
        vmaDestroyAllocator(memAllocator);
    }



_NAMESPACE_END_