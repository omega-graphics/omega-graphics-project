

#include "omegaGTE/GTEBase.h"
#include "omegaGTE/TE.h"

#include "GEVulkanRenderTarget.h"
#include "GEVulkanCommandQueue.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>


_NAMESPACE_BEGIN_

namespace {
    static inline GEViewport makeViewport(float width,float height){
        GEViewport viewport {};
        viewport.x = 0.f;
        viewport.y = 0.f;
        viewport.nearDepth = 0.f;
        viewport.farDepth = 1.f;
        viewport.width = std::max(1.f,width);
        viewport.height = std::max(1.f,height);
        return viewport;
    }

struct VulkanTessVertex { float pos[4]; float color[4]; };
struct VulkanTessParams { float rect[4]; float viewport[4]; float color[4]; float extra[4]; };
struct VulkanPathSeg { float se[4]; float sv[4]; float c[4]; float r[4]; };

TETessellationResult readbackVulkan(VmaAllocator allocator, VkBuffer buf, VmaAllocation alloc,
                                    unsigned vc,
                                    const std::optional<TETessellationResult::AttachmentData> &att) {
    TETessellationResult res;
    TETessellationResult::TEMesh mesh{TETessellationResult::TEMesh::TopologyTriangle};

    VulkanTessVertex *v = nullptr;
    vmaMapMemory(allocator, alloc, (void **)&v);
    if (!v) return res;

    for (unsigned i = 0; i + 2 < vc; i += 3) {
        TETessellationResult::TEMesh::Polygon p{};
        p.a.pt = {v[i].pos[0], v[i].pos[1], v[i].pos[2]};
        p.b.pt = {v[i+1].pos[0], v[i+1].pos[1], v[i+1].pos[2]};
        p.c.pt = {v[i+2].pos[0], v[i+2].pos[1], v[i+2].pos[2]};
        if (att) p.a.attachment = p.b.attachment = p.c.attachment = att;
        mesh.vertexPolygons.push_back(p);
    }
    vmaUnmapMemory(allocator, alloc);

    res.meshes.push_back(mesh);
    return res;
}

struct VulkanTessBufferPair {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
};

VulkanTessBufferPair createMappableBuffer(VmaAllocator allocator, VkDeviceSize size,
                                          VkBufferUsageFlags usage, VmaMemoryUsage memUsage) {
    VulkanTessBufferPair pair {};
    VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = memUsage;

    vmaCreateBuffer(allocator, &bufInfo, &allocInfo, &pair.buffer, &pair.alloc, nullptr);
    return pair;
}

#include "VulkanTessSpirv.inc"

VkShaderModule createTessModule(VkDevice device, const uint32_t *spirv, size_t sizeBytes) {
    VkShaderModuleCreateInfo info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = sizeBytes;
    info.pCode = spirv;
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &info, nullptr, &mod);
    return mod;
}

struct VulkanTessKernel {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
};

VulkanTessKernel createTessKernel(VkDevice device, VkShaderModule module) {
    VulkanTessKernel k {};

    VkDescriptorSetLayoutBinding bindings[2] {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &k.descLayout) != VK_SUCCESS) return k;

    VkPipelineLayoutCreateInfo plInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &k.descLayout;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &k.layout) != VK_SUCCESS) return k;

    VkComputePipelineCreateInfo cpInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpInfo.stage.module = module;
    cpInfo.stage.pName = "main";
    cpInfo.layout = k.layout;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &k.pipeline);

    return k;
}

struct VulkanTessPipelines {
    GEVulkanEngine *engine = nullptr;
    VulkanTessKernel rect, ellip, prism, path;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    bool ready = false;
    bool gpuReady = false;

    void init(GEVulkanEngine *e) {
        if (ready) return;
        ready = true;
        engine = e;

        VkShaderModule rectMod = createTessModule(e->device, kTessRectSpirv, sizeof(kTessRectSpirv));
        VkShaderModule ellipMod = createTessModule(e->device, kTessEllipSpirv, sizeof(kTessEllipSpirv));
        VkShaderModule prismMod = createTessModule(e->device, kTessRPrismSpirv, sizeof(kTessRPrismSpirv));
        VkShaderModule pathMod = createTessModule(e->device, kTessPathSpirv, sizeof(kTessPathSpirv));

        if (rectMod && ellipMod && prismMod && pathMod) {
            rect = createTessKernel(e->device, rectMod);
            ellip = createTessKernel(e->device, ellipMod);
            prism = createTessKernel(e->device, prismMod);
            path = createTessKernel(e->device, pathMod);

            if (rect.pipeline && ellip.pipeline && prism.pipeline && path.pipeline) {
                VkDescriptorPoolSize poolSize {};
                poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                poolSize.descriptorCount = 8;
                VkDescriptorPoolCreateInfo dpInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                dpInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
                dpInfo.maxSets = 4;
                dpInfo.poolSizeCount = 1;
                dpInfo.pPoolSizes = &poolSize;
                vkCreateDescriptorPool(e->device, &dpInfo, nullptr, &descPool);

                VkCommandPoolCreateInfo cpInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                cpInfo.queueFamilyIndex = e->queueFamilyIndices.empty() ? 0 : e->queueFamilyIndices[0];
                vkCreateCommandPool(e->device, &cpInfo, nullptr, &cmdPool);

                gpuReady = (descPool != VK_NULL_HANDLE && cmdPool != VK_NULL_HANDLE);
            }
        }

        if (rectMod) vkDestroyShaderModule(e->device, rectMod, nullptr);
        if (ellipMod) vkDestroyShaderModule(e->device, ellipMod, nullptr);
        if (prismMod) vkDestroyShaderModule(e->device, prismMod, nullptr);
        if (pathMod) vkDestroyShaderModule(e->device, pathMod, nullptr);
    }

    ~VulkanTessPipelines() {
        if (!engine) return;
        auto dev = engine->device;
        auto destroyKernel = [dev](VulkanTessKernel &k) {
            if (k.pipeline) vkDestroyPipeline(dev, k.pipeline, nullptr);
            if (k.layout) vkDestroyPipelineLayout(dev, k.layout, nullptr);
            if (k.descLayout) vkDestroyDescriptorSetLayout(dev, k.descLayout, nullptr);
        };
        destroyKernel(rect); destroyKernel(ellip); destroyKernel(prism); destroyKernel(path);
        if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
        if (cmdPool) vkDestroyCommandPool(dev, cmdPool, nullptr);
    }
};

std::future<TETessellationResult> vulkanGpuDispatch(
        OmegaTessellationEngineContext::GPUTessExtractedParams &ep,
        GEViewport &vp, float ctxArcStep, VulkanTessPipelines &pip,
        OmegaTessellationEngineContext *ctx,
        const TETessellationParams &origParams,
        GTEPolygonFrontFaceRotation ff, GEViewport *origVP) {

    std::optional<TETessellationResult::AttachmentData> colorAtt;
    float cv[4] = {0,0,0,1};
    if (ep.hasColor) {
        colorAtt = TETessellationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), FVec<3>::Create()};
        colorAtt->color[0][0] = ep.cr; colorAtt->color[1][0] = ep.cg;
        colorAtt->color[2][0] = ep.cb; colorAtt->color[3][0] = ep.ca;
        cv[0] = ep.cr; cv[1] = ep.cg; cv[2] = ep.cb; cv[3] = ep.ca;
    }

    auto fallback = [&]() {
        auto r = ctx->tessalateSync(origParams, ff, origVP);
        std::promise<TETessellationResult> p; p.set_value(std::move(r)); return p.get_future();
    };

    if (!pip.gpuReady) return fallback();

    VulkanTessKernel *kernel = nullptr;
    unsigned vc = 0, tc = 1;
    size_t paramSize = 0;
    void *paramData = nullptr;

    using ET = OmegaTessellationEngineContext::GPUTessExtractedParams;
    VulkanTessParams tp {};
    VulkanPathSeg *pathSegs = nullptr;

    switch (ep.type) {
        case ET::Rect: {
            kernel = &pip.rect; vc = 6; tc = 1;
            tp = {{ep.rx,ep.ry,ep.rw,ep.rh},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{0,0,0,0}};
            paramSize = sizeof(VulkanTessParams); paramData = &tp;
            break;
        }
        case ET::Ellipsoid: {
            kernel = &pip.ellip;
            float step = ctxArcStep > 0 ? ctxArcStep : 0.01f;
            unsigned segs = (unsigned)std::ceil(2.f * M_PI / step);
            vc = segs * 3; tc = segs;
            tp = {{ep.ex,ep.ey,0,0},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{ep.erad_x,ep.erad_y,step,(float)segs}};
            paramSize = sizeof(VulkanTessParams); paramData = &tp;
            break;
        }
        case ET::RectPrism: {
            kernel = &pip.prism; vc = 36; tc = 1;
            tp = {{ep.px,ep.py,ep.pz,ep.pw},{vp.x,vp.y,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{ep.ph,ep.pd,0,0}};
            paramSize = sizeof(VulkanTessParams); paramData = &tp;
            break;
        }
        case ET::Path2D: {
            if (ep.pathSegments.empty()) return fallback();
            kernel = &pip.path;
            unsigned sc = (unsigned)ep.pathSegments.size();
            vc = sc * 6; tc = sc;
            pathSegs = new VulkanPathSeg[sc];
            float sw = ep.strokeWidth > 0 ? ep.strokeWidth : 1.f;
            for (unsigned i = 0; i < sc; i++) {
                auto &s = ep.pathSegments[i];
                pathSegs[i] = {{s.sx,s.sy,s.ex,s.ey},{sw,0,vp.width,vp.height},{cv[0],cv[1],cv[2],cv[3]},{0,0,0,0}};
            }
            paramSize = sc * sizeof(VulkanPathSeg); paramData = pathSegs;
            break;
        }
        default:
            return fallback();
    }

    if (!kernel || !kernel->pipeline || !paramData) { delete[] pathSegs; return fallback(); }

    auto *e = pip.engine;

    auto paramBuf = createMappableBuffer(e->memAllocator, paramSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    auto outBuf = createMappableBuffer(e->memAllocator, vc * sizeof(VulkanTessVertex),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);

    if (paramBuf.buffer == VK_NULL_HANDLE || outBuf.buffer == VK_NULL_HANDLE) {
        delete[] pathSegs;
        if (paramBuf.buffer) vmaDestroyBuffer(e->memAllocator, paramBuf.buffer, paramBuf.alloc);
        if (outBuf.buffer) vmaDestroyBuffer(e->memAllocator, outBuf.buffer, outBuf.alloc);
        return fallback();
    }

    void *mapped = nullptr;
    vmaMapMemory(e->memAllocator, paramBuf.alloc, &mapped);
    memcpy(mapped, paramData, paramSize);
    vmaUnmapMemory(e->memAllocator, paramBuf.alloc);
    delete[] pathSegs;

    VkDescriptorSetAllocateInfo dsAllocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsAllocInfo.descriptorPool = pip.descPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &kernel->descLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    auto dsRes = vkAllocateDescriptorSets(e->device, &dsAllocInfo, &descSet);
    if (dsRes != VK_SUCCESS) {
        vmaDestroyBuffer(e->memAllocator, paramBuf.buffer, paramBuf.alloc);
        vmaDestroyBuffer(e->memAllocator, outBuf.buffer, outBuf.alloc);
        return fallback();
    }

    VkDescriptorBufferInfo paramBufInfo {paramBuf.buffer, 0, paramSize};
    VkDescriptorBufferInfo outBufInfo {outBuf.buffer, 0, vc * sizeof(VulkanTessVertex)};
    VkWriteDescriptorSet writes[2] {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &paramBufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &outBufInfo;
    vkUpdateDescriptorSets(e->device, 2, writes, 0, nullptr);

    VkCommandBufferAllocateInfo cbAllocInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAllocInfo.commandPool = pip.cmdPool;
    cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAllocInfo.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(e->device, &cbAllocInfo, &cmdBuf);

    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);
    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->pipeline);
    vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, kernel->layout, 0, 1, &descSet, 0, nullptr);
    vkCmdDispatch(cmdBuf, tc, 1, 1);
    vkEndCommandBuffer(cmdBuf);

    VkQueue gpuQueue = VK_NULL_HANDLE;
    if (!e->deviceQueuefamilies.empty() && !e->deviceQueuefamilies[0].empty()) {
        gpuQueue = e->deviceQueuefamilies[0][0].second;
    }
    if (gpuQueue == VK_NULL_HANDLE) {
        vkFreeCommandBuffers(e->device, pip.cmdPool, 1, &cmdBuf);
        vkFreeDescriptorSets(e->device, pip.descPool, 1, &descSet);
        vmaDestroyBuffer(e->memAllocator, paramBuf.buffer, paramBuf.alloc);
        vmaDestroyBuffer(e->memAllocator, outBuf.buffer, outBuf.alloc);
        return fallback();
    }

    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(e->device, &fenceInfo, nullptr, &fence);

    VkSubmitInfo submitInfo {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;
    vkQueueSubmit(gpuQueue, 1, &submitInfo, fence);
    vkWaitForFences(e->device, 1, &fence, VK_TRUE, UINT64_MAX);

    auto result = readbackVulkan(e->memAllocator, outBuf.buffer, outBuf.alloc, vc, colorAtt);

    vkDestroyFence(e->device, fence, nullptr);
    vkFreeCommandBuffers(e->device, pip.cmdPool, 1, &cmdBuf);
    vkFreeDescriptorSets(e->device, pip.descPool, 1, &descSet);
    vmaDestroyBuffer(e->memAllocator, paramBuf.buffer, paramBuf.alloc);
    vmaDestroyBuffer(e->memAllocator, outBuf.buffer, outBuf.alloc);

    std::promise<TETessellationResult> prom;
    prom.set_value(std::move(result));
    return prom.get_future();
}

} // anon namespace

class VulkanNativeRenderTargetTEContext : public OmegaTessellationEngineContext {
public:

    SharedHandle<GEVulkanNativeRenderTarget> renderTarget;
    VulkanTessPipelines pip;

    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result) override {
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            const float width = renderTarget != nullptr
                                ? static_cast<float>(renderTarget->extent.width)
                                : 1.f;
            const float height = renderTarget != nullptr
                                 ? static_cast<float>(renderTarget->extent.height)
                                 : 1.f;
            auto vp = makeViewport(width,height);
            translateCoordsDefaultImpl(x,y,z,&vp,x_result,y_result,z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready && renderTarget && renderTarget->commandQueue) {
            auto *engine = renderTarget->commandQueue->getEngine();
            if (engine) pip.init(engine);
        }
        if (!pip.ready || !pip.gpuReady) {
            GPUTessExtractedParams ep;
            extractGPUTessParams(params, ep);
            auto result = tessalateSync(params, direction, viewport);
            std::promise<TETessellationResult> p;
            p.set_value(std::move(result));
            return p.get_future();
        }
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0, 0, 1, 1, 0, 1};
        return vulkanGpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit VulkanNativeRenderTargetTEContext(SharedHandle<GEVulkanNativeRenderTarget> renderTarget):renderTarget(renderTarget){};
};

class VulkanTextureRenderTargetTEContext : public OmegaTessellationEngineContext {
public:
    SharedHandle<GEVulkanTextureRenderTarget> renderTarget;
    VulkanTessPipelines pip;

    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result) override {
        if (viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            float width = 1.f;
            float height = 1.f;
            if(renderTarget != nullptr && renderTarget->texture != nullptr){
                width = static_cast<float>(renderTarget->texture->descriptor.width);
                height = static_cast<float>(renderTarget->texture->descriptor.height);
            }
            auto defaultViewport = makeViewport(width,height);
            translateCoordsDefaultImpl(x,y,z,&defaultViewport,x_result,y_result,z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        if (!pip.ready && renderTarget && renderTarget->commandQueue) {
            auto *engine = renderTarget->commandQueue->getEngine();
            if (engine) pip.init(engine);
        }
        if (!pip.ready || !pip.gpuReady) {
            GPUTessExtractedParams ep;
            extractGPUTessParams(params, ep);
            auto result = tessalateSync(params, direction, viewport);
            std::promise<TETessellationResult> p;
            p.set_value(std::move(result));
            return p.get_future();
        }
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0, 0, 1, 1, 0, 1};
        return vulkanGpuDispatch(ep, vp, arcStep, pip, this, params, direction, viewport);
    }

    explicit VulkanTextureRenderTargetTEContext(SharedHandle<GEVulkanTextureRenderTarget> renderTarget):
    renderTarget(renderTarget){};
};


SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> & renderTarget){
    auto vulkanRenderTarget = std::dynamic_pointer_cast<GEVulkanNativeRenderTarget>(renderTarget);
    if(vulkanRenderTarget == nullptr){
        return pnullptr;
    }
    return SharedHandle<OmegaTessellationEngineContext>(new VulkanNativeRenderTargetTEContext(vulkanRenderTarget));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget){
    auto vulkanRenderTarget = std::dynamic_pointer_cast<GEVulkanTextureRenderTarget>(renderTarget);
    if(vulkanRenderTarget == nullptr){
        return nullptr;
    }
    return SharedHandle<OmegaTessellationEngineContext>(new VulkanTextureRenderTargetTEContext(vulkanRenderTarget));
};

_NAMESPACE_END_
