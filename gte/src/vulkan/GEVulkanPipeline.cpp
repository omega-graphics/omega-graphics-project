#include "GEVulkanPipeline.h"

_NAMESPACE_BEGIN_

GTEVulkanShader::GTEVulkanShader(GEVulkanEngine *parentEngine,omegasl_shader & shader,VkShaderModule & module): GTEShader({shader}),parentEngine(parentEngine),shaderModule(module){};

GTEVulkanShader::~GTEVulkanShader(){
    vkDestroyShaderModule(parentEngine->device,shaderModule,nullptr);
};

GEVulkanRenderPipelineState::GEVulkanRenderPipelineState(SharedHandle<GTEShader> &vertexShader,
                                                         SharedHandle<GTEShader> &fragmentShader,GEVulkanEngine *parentEngine, VkPipeline &pipeline,
                                                         VkPipelineLayout &layout, VkDescriptorPool &descriptorPool,
                                                         OmegaCommon::Vector<VkDescriptorSet> & descs,
                                                         OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts) : __GERenderPipelineState(vertexShader,fragmentShader),
                                                         parentEngine(parentEngine),
                                                         pipeline(pipeline),
                                                         layout(layout),
                                                         descriptorPool(descriptorPool),
                                                         descLayouts(descLayouts),
                                                         descs(descs){

}

GEVulkanRenderPipelineState::~GEVulkanRenderPipelineState() {
    vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
    vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
    vkFreeDescriptorSets(parentEngine->device,descriptorPool,descs.size(),descs.data());
    for(auto & d : descLayouts) {
        vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
    }
    descs.clear();
    vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
}

GEVulkanComputePipelineState::GEVulkanComputePipelineState(SharedHandle<GTEShader> &computeShader,
                                                           GEVulkanEngine *parentEngine, VkPipeline &pipeline,
                                                           VkPipelineLayout &layout, VkDescriptorPool &descriptorPool,
                                                           VkDescriptorSet & descSet,
                                                           OmegaCommon::Vector<VkDescriptorSetLayout> &descLayouts): __GEComputePipelineState(computeShader),
                                                           parentEngine(parentEngine),
                                                           pipeline(pipeline),
                                                           layout(layout),
                                                           descriptorPool(descriptorPool),
                                                           descLayouts(descLayouts),
                                                           descSet(descSet){

}

GEVulkanComputePipelineState::~GEVulkanComputePipelineState() {
    vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
    vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
    vkFreeDescriptorSets(parentEngine->device,descriptorPool,1,&descSet);
    for(auto & d : descLayouts) {
        vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
    }
    vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
}

_NAMESPACE_END_