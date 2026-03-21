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

    GEVulkanRenderPipelineState(SharedHandle<GTEShader> & vertexShader,
                                SharedHandle<GTEShader> & fragmentShader,
                                GEVulkanEngine *parentEngine,
                                VkPipeline & pipeline,
                                VkRenderPass & compatibilityRenderPass,
                                VkPipelineLayout & layout,
                                VkDescriptorPool & descriptorPool,
                                OmegaCommon::Vector<VkDescriptorSet> & descs,
                                OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts);
    void releaseNative();
    ~GEVulkanRenderPipelineState();
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
