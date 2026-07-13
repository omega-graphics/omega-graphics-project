#include "GEVulkan.h"
#include "../GEPipeline.cpp"

#ifndef OMEGAGTE_VULKAN_GEVULKANPIPELINE_H
#define OMEGAGTE_VULKAN_GEVULKANPIPELINE_H

_NAMESPACE_BEGIN_

struct GTEVulkanShader : public GTEShader {
    GEVulkanEngine *parentEngine;
    VkShaderModule shaderModule;
    bool nativeReleased_ = false;
    GTEVulkanShader(GEVulkanEngine *parentEngine,omegasl_shader & shader,VkShaderModule & shaderModule);
    void releaseNative();
    ~GTEVulkanShader();
};

class GEVulkanRenderPipelineState : public __GERenderPipelineState {
    GEVulkanEngine *parentEngine;
    bool nativeReleased_ = false;
public:

    VkPipeline pipeline;
    VkRenderPass compatibilityRenderPass;
    VkPipelineLayout layout;

    VkDescriptorPool descriptorPool;

    OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;

    OmegaCommon::Vector<VkDescriptorSet> descs;

    /// Immutable samplers baked into descriptor set layouts (static samplers).
    OmegaCommon::Vector<VkSampler> immutableSamplers;

    /// Mesh-Shader-Plan Phase 4a — variant flag. When true, the
    /// `vertexShader` slot inherited from `__GERenderPipelineState`
    /// holds the mesh shader (slot-doubling pattern shared with
    /// D3D12 4b.1 and Metal 4c.1). `GEVulkanCommandBuffer::drawMeshTasks`
    /// asserts on this flag before issuing `vkCmdDrawMeshTasksEXT`.
    bool isMesh = false;

    /// §5 — the optional amplification (task) stage of a mesh pipeline. Null on
    /// every other pipeline kind, and on a mesh pipeline built without one.
    ///
    /// This gets its OWN slot rather than doubling onto an existing one (the way
    /// the mesh shader doubles onto `vertexShader`) because amplification does
    /// not replace any stage — it is an additional stage that coexists with the
    /// mesh stage, with its own resource table and its own descriptor set. There
    /// is nothing for it to double onto. `bindResourceAtAmplificationShader` and
    /// `setRenderConstants` read `internal` off this handle to resolve the amp's
    /// bindings.
    SharedHandle<GTEShader> amplificationShader;

    /// §16 Phase G — tessellation-pipeline flag + per-patch control-point
    /// count. Set by `makeRenderPipelineState` after construction when the
    /// descriptor carried `hullFunc`/`domainFunc`. `GEVulkanCommandBuffer::
    /// drawPatches` reads `patchControlPoints` to compute the draw's vertex
    /// count (`patchCount * patchControlPoints`), and `startRenderPass`
    /// rejects a pipeline whose `isTess` is true (a tessellated draw must go
    /// through `startTessRenderPass`).
    bool isTess = false;
    uint32_t patchControlPoints = 0;

    GEVulkanRenderPipelineState(SharedHandle<GTEShader> & vertexShader,
                                SharedHandle<GTEShader> & fragmentShader,
                                GEVulkanEngine *parentEngine,
                                VkPipeline & pipeline,
                                VkRenderPass & compatibilityRenderPass,
                                VkPipelineLayout & layout,
                                VkDescriptorPool & descriptorPool,
                                OmegaCommon::Vector<VkDescriptorSet> & descs,
                                OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts,
                                OmegaCommon::Vector<VkSampler> & immutableSamplers);
    /// Mesh-Shader-Plan Phase 4a — mesh-variant constructor. Same
    /// shape as the graphics constructor; `meshShader` lands in the
    /// `vertexShader` base slot and `isMesh` is stamped true.
    GEVulkanRenderPipelineState(SharedHandle<GTEShader> & meshShader,
                                SharedHandle<GTEShader> & fragmentShader,
                                GEVulkanEngine *parentEngine,
                                VkPipeline & pipeline,
                                VkRenderPass & compatibilityRenderPass,
                                VkPipelineLayout & layout,
                                VkDescriptorPool & descriptorPool,
                                OmegaCommon::Vector<VkDescriptorSet> & descs,
                                OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts,
                                OmegaCommon::Vector<VkSampler> & immutableSamplers,
                                bool meshVariant);
    void releaseNative();
    ~GEVulkanRenderPipelineState();
};

// Extension 3: wraps a regular Vulkan render pipeline whose vertex stage is
// the engine-supplied full-screen-triangle shader.
class GEVulkanBlitPipelineState : public __GEBlitPipelineState {
public:
    SharedHandle<GERenderPipelineState> renderPipeline;
    explicit GEVulkanBlitPipelineState(SharedHandle<GERenderPipelineState> & rp)
        : renderPipeline(rp) {}
};

class GEVulkanComputePipelineState : public __GEComputePipelineState {
    GEVulkanEngine *parentEngine;
    bool nativeReleased_ = false;
public:
    VkPipeline pipeline;
    VkPipelineLayout layout;

    VkDescriptorPool descriptorPool;

    OmegaCommon::Vector<VkDescriptorSetLayout> descLayouts;

    VkDescriptorSet descSet;
    
    GEVulkanComputePipelineState(SharedHandle<GTEShader> & computeShader,
                                 GEVulkanEngine *parentEngine,
                                 VkPipeline & pipeline,
                                 VkPipelineLayout & layout,
                                 VkDescriptorPool & descriptorPool,
                                 VkDescriptorSet & descSet,
                                 OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts);
    void releaseNative();
    ~GEVulkanComputePipelineState();
};

_NAMESPACE_END_

#endif
