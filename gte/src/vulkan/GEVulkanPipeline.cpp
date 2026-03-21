#include "GEVulkanPipeline.h"

_NAMESPACE_BEGIN_

GTEVulkanShader::GTEVulkanShader(GEVulkanEngine *parentEngine,omegasl_shader & shader,VkShaderModule & module): GTEShader({shader}),parentEngine(parentEngine),shaderModule(module){};

void GTEVulkanShader::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
    vkDestroyShaderModule(parentEngine->device,shaderModule,nullptr);
    shaderModule = VK_NULL_HANDLE;
}

GTEVulkanShader::~GTEVulkanShader(){
    if(!nativeReleased_){
        vkDestroyShaderModule(parentEngine->device,shaderModule,nullptr);
    }
};

GEVulkanRenderPipelineState::GEVulkanRenderPipelineState(SharedHandle<GTEShader> &vertexShader,
                                                         SharedHandle<GTEShader> &fragmentShader,GEVulkanEngine *parentEngine, VkPipeline &pipeline,
                                                         VkRenderPass & compatibilityRenderPass,
                                                         VkPipelineLayout &layout, VkDescriptorPool &descriptorPool,
                                                         OmegaCommon::Vector<VkDescriptorSet> & descs,
                                                         OmegaCommon::Vector<VkDescriptorSetLayout> & descLayouts) : __GERenderPipelineState(vertexShader,fragmentShader),
                                                         parentEngine(parentEngine),
                                                         pipeline(pipeline),
                                                         compatibilityRenderPass(compatibilityRenderPass),
                                                         layout(layout),
                                                         descriptorPool(descriptorPool),
                                                         descLayouts(descLayouts),
                                                         descs(descs){

}

void GEVulkanRenderPipelineState::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
    if(pipeline != VK_NULL_HANDLE){
        vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if(compatibilityRenderPass != VK_NULL_HANDLE){
        vkDestroyRenderPass(parentEngine->device,compatibilityRenderPass,nullptr);
        compatibilityRenderPass = VK_NULL_HANDLE;
    }
    if(layout != VK_NULL_HANDLE){
        vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
        layout = VK_NULL_HANDLE;
    }
    if(descriptorPool != VK_NULL_HANDLE && !descs.empty()){
        vkFreeDescriptorSets(parentEngine->device,descriptorPool,descs.size(),descs.data());
    }
    for(auto & d : descLayouts) {
        vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
    }
    descs.clear();
    descLayouts.clear();
    if(descriptorPool != VK_NULL_HANDLE){
        vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }
}

GEVulkanRenderPipelineState::~GEVulkanRenderPipelineState() {
    if(!nativeReleased_){
        if(pipeline != VK_NULL_HANDLE){
            vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
        }
        if(compatibilityRenderPass != VK_NULL_HANDLE){
            vkDestroyRenderPass(parentEngine->device,compatibilityRenderPass,nullptr);
        }
        if(layout != VK_NULL_HANDLE){
            vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
        }
        if(descriptorPool != VK_NULL_HANDLE && !descs.empty()){
            vkFreeDescriptorSets(parentEngine->device,descriptorPool,descs.size(),descs.data());
        }
        for(auto & d : descLayouts) {
            vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
        }
        descs.clear();
        if(descriptorPool != VK_NULL_HANDLE){
            vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
        }
    }
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

void GEVulkanComputePipelineState::releaseNative(){
    if(nativeReleased_) return;
    nativeReleased_ = true;
    if(pipeline != VK_NULL_HANDLE){
        vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if(layout != VK_NULL_HANDLE){
        vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
        layout = VK_NULL_HANDLE;
    }
    if(descriptorPool != VK_NULL_HANDLE && descSet != VK_NULL_HANDLE){
        vkFreeDescriptorSets(parentEngine->device,descriptorPool,1,&descSet);
        descSet = VK_NULL_HANDLE;
    }
    for(auto & d : descLayouts) {
        vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
    }
    descLayouts.clear();
    if(descriptorPool != VK_NULL_HANDLE){
        vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
        descriptorPool = VK_NULL_HANDLE;
    }
}

GEVulkanComputePipelineState::~GEVulkanComputePipelineState() {
    if(!nativeReleased_){
        if(pipeline != VK_NULL_HANDLE){
            vkDestroyPipeline(parentEngine->device,pipeline,nullptr);
        }
        if(layout != VK_NULL_HANDLE){
            vkDestroyPipelineLayout(parentEngine->device,layout,nullptr);
        }
        if(descriptorPool != VK_NULL_HANDLE && descSet != VK_NULL_HANDLE){
            vkFreeDescriptorSets(parentEngine->device,descriptorPool,1,&descSet);
        }
        for(auto & d : descLayouts) {
            vkDestroyDescriptorSetLayout(parentEngine->device,d,nullptr);
        }
        if(descriptorPool != VK_NULL_HANDLE){
            vkDestroyDescriptorPool(parentEngine->device,descriptorPool,nullptr);
        }
    }
}

_NAMESPACE_END_
