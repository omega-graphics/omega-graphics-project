#include "GED3D12CommandQueue.h"
#include "GED3D12RenderTarget.h"
#include "GED3D12Pipeline.h"
#include "GED3D12Texture.h"
#include <d3d12.h>
#include <memory>
_NAMESPACE_BEGIN_

#ifndef D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
#define D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
#endif
    // GED3D12CommandBuffer::GED3D12CommandBuffer(){};
    // void GED3D12CommandBuffer::commitToBuffer(){};
    GED3D12CommandQueue::GED3D12CommandQueue(GED3D12Engine *engine,unsigned size):GECommandQueue(size),engine(engine),currentCount(0){
        HRESULT hr;

        hr = engine->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));

        if(FAILED(hr)){
            exit(1);
        };

        cpuEvent = CreateEvent(NULL,FALSE,FALSE,NULL);

        fence->SetEventOnCompletion(1,cpuEvent);

        D3D12_COMMAND_QUEUE_DESC desc;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = engine->d3d12_device->GetNodeCount();
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = engine->d3d12_device->CreateCommandQueue(&desc,IID_PPV_ARGS(&commandQueue));
        if(FAILED(hr)){
            MessageBoxA(GetForegroundWindow(),"Failed to Create Command Queue.","NOTE",MB_OK);
            exit(1);
        };

    };

    GED3D12CommandBuffer::GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,ID3D12CommandAllocator *commandAllocator,GED3D12CommandQueue *parentQueue):commandList(commandList),commandAllocator(commandAllocator),parentQueue(parentQueue),inComputePass(false),inBlitPass(false){
        
    };

    unsigned int GED3D12CommandBuffer::getRootParameterIndexOfResource(unsigned int id, omegasl_shader &shader){
        bool isSRV = false,isUAV = false,isDescriptorTable = false;
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr {shader.pLayout,shader.pLayout + shader.nLayout};

        unsigned relative_index = 0;
        for(auto & l : layoutArr){
            if(l.location == id) {
                relative_index = l.gpu_relative_loc;
                if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                    if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN){
                        isSRV = true;
                    }
                    else {
                        isUAV = true;
                    }
                }
                else {
                    isDescriptorTable = true;
                }
                break;
            }
        }

        unsigned regSpace;
        if(shader.type == OMEGASL_SHADER_FRAGMENT){
            regSpace = 1;
        }
        else {
            regSpace = 0;
        }

        unsigned idx = 0;
        for(;idx < currentRootSignature->NumParameters;idx++){
            auto & param = currentRootSignature->pParameters[idx];
            // std::cout << "PARAM_TYPE:" << (int)param.ParameterType << std::endl;
            if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV && isSRV){
                if(param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace){
                    break;
                }
            }
            else if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV && isUAV){
                if(param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace){
                    break;
                }
            }
            else if(param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && isDescriptorTable) {
                auto & range = param.DescriptorTable.pDescriptorRanges[0];
                if(range.BaseShaderRegister == relative_index && range.RegisterSpace == regSpace){
                    break;
                }
            }
        }
        return idx;
    }

    D3D12_RESOURCE_STATES
    GED3D12CommandBuffer::getRequiredResourceStateForResourceID(unsigned int &id, omegasl_shader &shader) {
        OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr {shader.pLayout,shader.pLayout + shader.nLayout};
        for(auto & l : layoutArr){
            if(l.location == id){
                D3D12_RESOURCE_STATES state;
                if(l.type == OMEGASL_SHADER_TEXTURE1D_DESC || l.type == OMEGASL_SHADER_TEXTURE2D_DESC || l.type == OMEGASL_SHADER_TEXTURE3D_DESC){
                    if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN){
                        if(shader.type == OMEGASL_SHADER_FRAGMENT){
                            state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                        }
                        else {
                            state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        }
                    }
                    else {
                        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    }
                }
                else if(l.type == OMEGASL_SHADER_BUFFER_DESC){
                    if(l.io_mode == OMEGASL_SHADER_DESC_IO_IN){
                        state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                    }
                    else {
                        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    }
                }
                else {
                    DEBUG_STREAM("This resource cannot be transitioned");
                    exit(1);
                }
                return state;
            }
        }
        return D3D12_RESOURCE_STATE_COMMON;
    }

    void GED3D12CommandBuffer::startBlitPass(){
        inBlitPass = true;
    };

    void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
        assert(inBlitPass && "Not in Blit Pass! Exiting...");
        auto *srcText = (GED3D12Texture *)src.get(),*destText = (GED3D12Texture *)dest.get();
        /// Resource Synchronization Checks
        OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
        if(srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE){
            if(srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
                resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
            }

            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(),srcText->currentState,D3D12_RESOURCE_STATE_COPY_SOURCE));
            srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        if(destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST){
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(destText->resource.Get(),destText->currentState,D3D12_RESOURCE_STATE_COPY_DEST));
            srcText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        if(!resourceBarriers.empty()){
            commandList->ResourceBarrier(resourceBarriers.size(),resourceBarriers.data());
        }
        commandList->CopyResource(destText->resource.Get(),srcText->resource.Get());
    }

    void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest,const TextureRegion & region,const GPoint3D & destCoord) {
        assert(inBlitPass && "Not in Blit Pass! Exiting...");
        auto *srcText = (GED3D12Texture *)src.get(),*destText = (GED3D12Texture *)dest.get();

        /// Resource Synchronization Checks
        OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
        if(srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE){
            if(srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
                resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
            }

            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(),srcText->currentState,D3D12_RESOURCE_STATE_COPY_SOURCE));
            srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }

        if(destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST){
            if(destText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
                resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destText->resource.Get()));
            }

            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(destText->resource.Get(),destText->currentState,D3D12_RESOURCE_STATE_COPY_DEST));
            srcText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        if(!resourceBarriers.empty()){
            commandList->ResourceBarrier(resourceBarriers.size(),resourceBarriers.data());
        }


        CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcText->resource.Get()),
                                        destLoc(destText->resource.Get());
        LONG top_pos = LONG(region.h) - LONG(region.y);
        CD3DX12_BOX _region ((LONG)region.x,top_pos,LONG(region.x + region.w),LONG(top_pos + region.h));
        commandList->CopyTextureRegion(&destLoc,(UINT)destCoord.x,(UINT)destCoord.y,(UINT)destCoord.z,&srcLoc,&_region);
    }

    void GED3D12CommandBuffer::finishBlitPass(){
        inBlitPass = false;
    };

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    void GED3D12CommandBuffer::beginAccelStructPass(){

    }

    void GED3D12CommandBuffer::buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, const GEAccelerationStructDescriptor &desc){
        auto accel_struct = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d;
        d.SourceAccelerationStructureData = NULL;
        d.DestAccelerationStructureData = accel_struct->structBuffer->buffer->GetGPUVirtualAddress();
        d.ScratchAccelerationStructureData = accel_struct->scratchBuffer->buffer->GetGPUVirtualAddress();
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postBuild;
        
        commandList->BuildRaytracingAccelerationStructure(&d,1,&postBuild);

        
    }

    void GED3D12CommandBuffer::refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src, SharedHandle<GEAccelerationStruct> &dest, const GEAccelerationStructDescriptor &desc){
         auto accel_struct_src = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
         auto accel_struct_dest = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(dest);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d;
        d.SourceAccelerationStructureData = accel_struct_src->structBuffer->buffer->GetGPUVirtualAddress();;
        d.DestAccelerationStructureData = accel_struct_dest->structBuffer->buffer->GetGPUVirtualAddress();
        d.ScratchAccelerationStructureData = accel_struct_dest->scratchBuffer->buffer->GetGPUVirtualAddress();
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postBuild;
        
        commandList->BuildRaytracingAccelerationStructure(&d,1,&postBuild);
    }

    void GED3D12CommandBuffer::finishAccelStructPass(){

    }

    #endif

    void GED3D12CommandBuffer::startRenderPass(const GERenderPassDescriptor &desc){
        inRenderPass = true;
        assert(!inComputePass && "Cannot start a Render Pass while in a compute pass.");
        D3D12_RENDER_PASS_RENDER_TARGET_DESC rt_desc;
        D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds_desc;

        D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS resolveParams;

        CD3DX12_CPU_DESCRIPTOR_HANDLE cpu_handle;
        CD3DX12_CPU_DESCRIPTOR_HANDLE ds_cpu_handle;

        if(desc.nRenderTarget) {
            auto *nativeRenderTarget = (GED3D12NativeRenderTarget *)desc.nRenderTarget;
            if(desc.multisampleResolve){
                multisampleResolvePass = true;
                auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
                cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());

                if(!desc.depthStencilAttachment.disabled){
                    ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
                }

                D3D12_RESOURCE_STATES resource_state;
                if(firstRenderPass) {
                    resource_state = D3D12_RESOURCE_STATE_PRESENT;
                }
                else {
                    resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }

                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex], resource_state,
                        D3D12_RESOURCE_STATE_RESOLVE_DEST);
                commandList->ResourceBarrier(1, &barrier);

                auto dxgi_format = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex]->GetDesc().Format;
                RECT rc;
                GetClientRect(nativeRenderTarget->hwnd,&rc);
                resolveParams.pSrcResource = resolveTexture->resource.Get();
                resolveParams.SubresourceCount = 1;
                resolveParams.PreserveResolveSource = TRUE;
                resolveParams.pDstResource = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex];
                D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS params;
                params.DstSubresource = 0;
                params.SrcSubresource = 0;
                params.DstX = 0;
                params.DstY = 0;
                params.SrcRect = CD3DX12_RECT(rc.left,rc.top,rc.right,rc.bottom);
                resolveParams.pSubresourceParameters = &params;
                resolveParams.Format = dxgi_format;
                resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
            }
            else {
                cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                        nativeRenderTarget->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
                if(!desc.depthStencilAttachment.disabled){
                    ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(nativeRenderTarget->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
                }
                if(firstRenderPass) {
                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                            nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex],
                            D3D12_RESOURCE_STATE_PRESENT,
                            D3D12_RESOURCE_STATE_RENDER_TARGET);

                    commandList->ResourceBarrier(1, &barrier);
                }
            }
            currentTarget.native = nativeRenderTarget;
        }
        else if(desc.tRenderTarget){
            auto *textureRenderTarget = (GED3D12TextureRenderTarget *)desc.tRenderTarget;
            if(desc.multisampleResolve){
                auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
                cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                        resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
                if(!desc.depthStencilAttachment.disabled){
                    ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
                }

                auto currentState = resolveTexture->currentState;

                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        textureRenderTarget->texture->resource.Get(),currentState,
                        D3D12_RESOURCE_STATE_RESOLVE_DEST);
                commandList->ResourceBarrier(1, &barrier);
                resolveTexture->currentState = D3D12_RESOURCE_STATE_RESOLVE_DEST;

                auto desc = textureRenderTarget->texture->resource->GetDesc();
                auto dxgi_format = desc.Format;

                resolveParams.pSrcResource = resolveTexture->resource.Get();
                resolveParams.SubresourceCount = 1;
                resolveParams.PreserveResolveSource = TRUE;
                resolveParams.pDstResource = textureRenderTarget->texture->resource.Get();
                D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS params;
                params.DstSubresource = 0;
                params.SrcSubresource = 0;
                params.DstX = 0;
                params.DstY = 0;
                params.SrcRect = CD3DX12_RECT(0,0,desc.Width,desc.Height);
                resolveParams.pSubresourceParameters = &params;
                resolveParams.Format = dxgi_format;
                resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
            }
            else {
                cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                        textureRenderTarget->texture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
                if(!desc.depthStencilAttachment.disabled){
                    ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(textureRenderTarget->texture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
                }
            }
            currentTarget.texture = textureRenderTarget;
        };
        rt_desc.cpuDescriptor = cpu_handle;
        
        if(!desc.depthStencilAttachment.disabled){
            ds_desc.cpuDescriptor = ds_cpu_handle;
        }

        if(desc.multisampleResolve){
            rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            rt_desc.EndingAccess.Resolve = resolveParams;
        }

        switch (desc.colorAttachment->loadAction) {
            case GERenderPassDescriptor::ColorAttachment::Load : {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if(!desc.multisampleResolve)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::LoadPreserve : {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if(!desc.multisampleResolve)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Discard : {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if(!desc.multisampleResolve)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Clear : {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                const FLOAT colors[] = {desc.colorAttachment->clearColor.r,desc.colorAttachment->clearColor.g,desc.colorAttachment->clearColor.b,desc.colorAttachment->clearColor.a};
                rt_desc.BeginningAccess.Clear.ClearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM,colors);
                /// Same as StoreAction in Metal
                if(!desc.multisampleResolve)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }


        if(desc.depthStencilAttachment.disabled){
            commandList->BeginRenderPass(1,&rt_desc,nullptr,D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
        }
        else {

            if(desc.multisampleResolve){
                ds_desc.DepthEndingAccess.Type = ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
                resolveParams.Format = DXGI_FORMAT_UNKNOWN;
                ds_desc.DepthEndingAccess.Resolve = ds_desc.StencilEndingAccess.Resolve = resolveParams;
            }

            switch (desc.depthStencilAttachment.depthloadAction) {
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                    ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                    if(!desc.multisampleResolve)
                        ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve : {
                    ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                    if(!desc.multisampleResolve)
                        ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                    ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                    ds_desc.DepthBeginningAccess.Clear.ClearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN,desc.depthStencilAttachment.clearDepth,desc.depthStencilAttachment.clearStencil);
                    if(!desc.multisampleResolve)
                        ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                    break;
                }
            }

            switch (desc.depthStencilAttachment.stencilLoadAction) {
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                    ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                    if(!desc.multisampleResolve)
                        ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve : {
                    ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                    if(!desc.multisampleResolve)
                        ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                    break;
                }
                case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                    ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                    ds_desc.StencilBeginningAccess.Clear.ClearValue = CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN,desc.depthStencilAttachment.clearDepth,desc.depthStencilAttachment.clearStencil);
                    if(!desc.multisampleResolve)
                        ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                    break;
                }
            }

            commandList->BeginRenderPass(1,&rt_desc,&ds_desc,D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
        }

        if(firstRenderPass){
            firstRenderPass = false;
        }

    };

    void GED3D12CommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState){
         assert(!inComputePass && "Cannot set Render Pipeline State while in Compute Pass");
        auto *d3d12_pipeline_state = (GED3D12RenderPipelineState *)pipelineState.get();
        commandList->SetPipelineState(d3d12_pipeline_state->pipelineState.Get());
        currentRenderPipeline = d3d12_pipeline_state;
        commandList->SetGraphicsRootSignature(d3d12_pipeline_state->rootSignature.Get());
        currentRootSignature = &d3d12_pipeline_state->rootSignatureDesc;
    };

    void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int index){
        assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const at a Vertex Func when not in render pass");
        auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

        auto required_state = getRequiredResourceStateForResourceID(index,currentRenderPipeline->vertexShader->internal);

        if(d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
            commandList->ResourceBarrier(1,&barrier);
        }

        if(!(d3d12_buffer->currentState & required_state)){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_buffer->buffer.Get(),d3d12_buffer->currentState,required_state);
            commandList->ResourceBarrier(1,&barrier);
            d3d12_buffer->currentState = required_state;
        }

        commandList->SetDescriptorHeaps(1,d3d12_buffer->bufferDescHeap.GetAddressOf());

        if(d3d12_buffer->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE){
            commandList->SetGraphicsRootShaderResourceView(getRootParameterIndexOfResource(index,currentRenderPipeline->vertexShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }
        else {
            commandList->SetGraphicsRootUnorderedAccessView(getRootParameterIndexOfResource(index,currentRenderPipeline->vertexShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }

    };

    void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int index){
         assert((!inComputePass && !inBlitPass) &&"Cannot set Resource Const at a Vertex Func when not in render pass");
        auto *d3d12_texture = (GED3D12Texture *)texture.get();

        if(d3d12_texture->needsValidation()) {
            auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

            d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
            buffer->commandList->Close();
            parentQueue->commandQueue->ExecuteCommandLists(1,(ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
        }

        auto required_state = getRequiredResourceStateForResourceID(index,currentRenderPipeline->vertexShader->internal);

        if(d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
            commandList->ResourceBarrier(1,&barrier);
        }

        if(!(d3d12_texture->currentState & required_state)){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_texture->resource.Get(),d3d12_texture->currentState,required_state);
            commandList->ResourceBarrier(1,&barrier);
            d3d12_texture->currentState = required_state;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE cpuDescHandle;

        if(d3d12_texture->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE){
            /// Use Shader Resource View.
            cpuDescHandle = d3d12_texture->srvDescHeap->GetGPUDescriptorHandleForHeapStart();
        }
        else {
            /// Use Unordered Access View
            cpuDescHandle = d3d12_texture->uavDescHeap->GetGPUDescriptorHandleForHeapStart();
        }
        
        commandList->SetDescriptorHeaps(1,d3d12_texture->srvDescHeap.GetAddressOf());
        unsigned idx = getRootParameterIndexOfResource(index,currentRenderPipeline->vertexShader->internal);
        commandList->SetGraphicsRootDescriptorTable(idx,cpuDescHandle);

    };

    void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int index){
         assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const a Fragment Func when not in render pass");
        auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

        auto required_state = getRequiredResourceStateForResourceID(index,currentRenderPipeline->fragmentShader->internal);

        if(d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
            commandList->ResourceBarrier(1,&barrier);
        }

        if(!(d3d12_buffer->currentState & required_state)){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_buffer->buffer.Get(),d3d12_buffer->currentState,required_state);
            commandList->ResourceBarrier(1,&barrier);
            d3d12_buffer->currentState = required_state;
        }

        commandList->SetDescriptorHeaps(1,d3d12_buffer->bufferDescHeap.GetAddressOf());

        if(d3d12_buffer->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE){
            commandList->SetGraphicsRootShaderResourceView(getRootParameterIndexOfResource(index,currentRenderPipeline->fragmentShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }
        else {
            commandList->SetGraphicsRootUnorderedAccessView(getRootParameterIndexOfResource(index,currentRenderPipeline->fragmentShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }

    };

    void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int index){
         assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const a Fragment Func when not in render pass");
        auto *d3d12_texture = (GED3D12Texture *)texture.get();
        if(d3d12_texture->needsValidation()) {
            auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

            d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
            buffer->commandList->Close();
            parentQueue->commandQueue->ExecuteCommandLists(1,(ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
        }

        auto required_state = getRequiredResourceStateForResourceID(index,currentRenderPipeline->fragmentShader->internal);

        if(d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
            commandList->ResourceBarrier(1,&barrier);
        }

        if(!(d3d12_texture->currentState & required_state)){
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_texture->resource.Get(),d3d12_texture->currentState,required_state);
            commandList->ResourceBarrier(1,&barrier);
            d3d12_texture->currentState = required_state;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE cpuDescHandle {};

        if(d3d12_texture->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE){
            /// Use Shader Resource View.
            cpuDescHandle = d3d12_texture->srvDescHeap->GetGPUDescriptorHandleForHeapStart();
        }
        else {
            /// Use Unordered Access View
            cpuDescHandle = d3d12_texture->uavDescHeap->GetGPUDescriptorHandleForHeapStart();
        }

        commandList->SetDescriptorHeaps(1,d3d12_texture->srvDescHeap.GetAddressOf());
        unsigned rootParam = getRootParameterIndexOfResource(index,currentRenderPipeline->fragmentShader->internal);
        DEBUG_STREAM("Root Param With Texture:" << rootParam);
        commandList->SetGraphicsRootDescriptorTable(rootParam,cpuDescHandle);

    };

    void GED3D12CommandBuffer::setStencilRef(unsigned int ref) {
        commandList->OMSetStencilRef(ref);
    }

    void GED3D12CommandBuffer::setViewports(std::vector<GEViewport> viewports){
        std::vector<D3D12_VIEWPORT> d3d12_viewports;
        auto viewports_it = viewports.begin();
        while(viewports_it != viewports.end()){
            GEViewport & viewport = *viewports_it;
            GRect rect {};
            if(currentTarget.native != nullptr) {
                auto res_desc = currentTarget.native->renderTargets[currentTarget.native->frameIndex]->GetDesc();
                rect.pos.x = 0;
                rect.pos.y = 0;
                rect.w = (float)res_desc.Width;
                rect.h = (float)res_desc.Height;
            }
            else {
                rect.pos.x = 0;
                rect.pos.y = 0;
                auto res_desc = currentTarget.texture->texture->resource->GetDesc();
                rect.w = (float)res_desc.Width;
                rect.h = (float)res_desc.Height;
            }

            CD3DX12_VIEWPORT v(viewport.x,rect.h - (viewport.y + viewport.height),viewport.width,viewport.height,viewport.nearDepth,viewport.farDepth);
            d3d12_viewports.push_back(v);
            ++viewports_it;
        };
        commandList->RSSetViewports(d3d12_viewports.size(),d3d12_viewports.data());
    };

    void GED3D12CommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects){
        std::vector<D3D12_RECT> d3d12_rects;
        auto rects_it = scissorRects.begin();
        while(rects_it != scissorRects.end()){
            GEScissorRect & _rect = *rects_it;

            GRect rect {};
            if(currentTarget.native != nullptr) {

                auto res_desc = currentTarget.native->renderTargets[currentTarget.native->frameIndex]->GetDesc();
                rect.pos.x = 0;
                rect.pos.y = 0;
                rect.w = (float)res_desc.Width;
                rect.h = (float)res_desc.Height;
            }
            else {
                rect.pos.x = 0;
                rect.pos.y = 0;
                auto res_desc = currentTarget.texture->texture->resource->GetDesc();
                rect.w = (float)res_desc.Width;
                rect.h = (float)res_desc.Height;
            }

            float top_coord = rect.h - (_rect.height + _rect.y);

            CD3DX12_RECT r((LONG)_rect.x,(LONG)top_coord,LONG(_rect.width + _rect.x),LONG(top_coord + _rect.height));
            d3d12_rects.push_back(r);
            ++rects_it;
        };
        commandList->RSSetScissorRects(d3d12_rects.size(),d3d12_rects.data());
    };

    void GED3D12CommandBuffer::setVertexBuffer(SharedHandle<GEBuffer> &buffer)
    {
        auto *b = (GED3D12Buffer *)buffer.get();
        D3D12_VERTEX_BUFFER_VIEW view;
        view.BufferLocation = b->buffer->GetGPUVirtualAddress();
        view.SizeInBytes = UINT(b->size());
        view.StrideInBytes = 1;
        commandList->IASetVertexBuffers(0,1,&view);
    };

    void GED3D12CommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount, size_t startIdx){
        assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if(polygonType == GECommandBuffer::RenderPassDrawPolygonType::Triangle){
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
        else if(polygonType == GECommandBuffer::RenderPassDrawPolygonType::TriangleStrip){
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        };
        commandList->IASetPrimitiveTopology(topology);
        commandList->DrawInstanced(vertexCount,1,startIdx,0);
    };

    void GED3D12CommandBuffer::finishRenderPass(){
        assert(inRenderPass && "");
        commandList->EndRenderPass();
        commandList->ClearState(nullptr);

        if(multisampleResolvePass){
            ID3D12Resource *target;
            if(currentTarget.native != nullptr){
                target = currentTarget.native->renderTargets[currentTarget.native->frameIndex];
            }
            else {
                target = currentTarget.texture->texture->resource.Get();
            }
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(target,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1,&barrier);
        }

        currentTarget.texture = nullptr;
        currentTarget.native = nullptr;
        currentRenderPipeline = nullptr;
        currentRootSignature = nullptr;
    };

    void GED3D12CommandBuffer::startComputePass(const GEComputePassDescriptor &desc){
        inComputePass = true;
    };

    void GED3D12CommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState){
        auto *d3d12_pipeline_state = (GED3D12ComputePipelineState *)pipelineState.get();
        commandList->SetPipelineState(d3d12_pipeline_state->pipelineState.Get());
        commandList->SetComputeRootSignature(d3d12_pipeline_state->rootSignature.Get());
        currentComputePipeline = d3d12_pipeline_state;
        currentRootSignature = &d3d12_pipeline_state->rootSignatureDesc;
    };

    void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> &buffer, unsigned int id) {
        assert(inComputePass && "");
        auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();
        D3D12_HEAP_PROPERTIES heap_props;
        D3D12_HEAP_FLAGS heapFlags;
        d3d12_buffer->buffer->GetHeapProperties(&heap_props,&heapFlags);
        commandList->SetDescriptorHeaps(1,d3d12_buffer->bufferDescHeap.GetAddressOf());
        if(heap_props.Type == D3D12_HEAP_TYPE_UPLOAD){
            commandList->SetComputeRootShaderResourceView(getRootParameterIndexOfResource(id,currentComputePipeline->computeShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }
        else if(heap_props.Type == D3D12_HEAP_TYPE_READBACK){
            auto resource_barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_buffer->buffer.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1,&resource_barrier);
            commandList->SetComputeRootUnorderedAccessView(getRootParameterIndexOfResource(id,currentComputePipeline->computeShader->internal),d3d12_buffer->buffer->GetGPUVirtualAddress());
        }


    }

    void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id) {
        assert(inComputePass && "");
        auto *d3d12_texture = (GED3D12Texture *)texture.get();
        if(d3d12_texture->needsValidation()) {
            d3d12_texture->updateAndValidateStatus(commandList.Get());
        }

        D3D12_HEAP_PROPERTIES heap_props;
        D3D12_HEAP_FLAGS heapFlags;
        d3d12_texture->resource->GetHeapProperties(&heap_props,&heapFlags);
        commandList->SetDescriptorHeaps(1,d3d12_texture->srvDescHeap.GetAddressOf());
        if(heap_props.Type == D3D12_HEAP_TYPE_READBACK){
            auto resource_barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_texture->resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->ResourceBarrier(1,&resource_barrier);
        }
        commandList->SetComputeRootDescriptorTable(getRootParameterIndexOfResource(id,currentComputePipeline->computeShader->internal),d3d12_texture->srvDescHeap->GetGPUDescriptorHandleForHeapStart());

    }

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct,unsigned int idx){
        auto d3d12_buffer = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(accelStruct);
        commandList->SetComputeRootShaderResourceView(getRootParameterIndexOfResource(idx,currentComputePipeline->computeShader->internal),d3d12_buffer->structBuffer->buffer->GetGPUVirtualAddress());
    }

    void GED3D12CommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z){
         assert(inComputePass && "");
         D3D12_DISPATCH_RAYS_DESC rays {};
         rays.Width = x;
         rays.Height = y;
         rays.Depth = z;
         
         commandList->DispatchRays(&rays);
    }

    #endif

    void GED3D12CommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
        assert(inComputePass && "");
        commandList->Dispatch(x,y,z);
    }

   

    void GED3D12CommandBuffer::finishComputePass(){
        commandList->ClearState(nullptr);
        inComputePass = false;
        currentComputePipeline = nullptr;
        currentRootSignature = nullptr;
    };

//    void GED3D12CommandBuffer::waitForFence(SharedHandle<GEFence> &fence,unsigned val) {
////        auto _fence = (GED3D12Fence *)fence.get();
////
////        parentQueue->commandQueue->Wait(_fence->fence.Get(),val);
//
//    }
//
//    void GED3D12CommandBuffer::signalFence(SharedHandle<GEFence> &fence,unsigned val) {
////        auto _fence = (GED3D12Fence *)fence.get();
////        parentQueue->commandQueue->Signal(_fence->fence.Get(),val);
//    }

    GED3D12CommandBuffer::~GED3D12CommandBuffer() = default;

    void GED3D12CommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &waitFence) {
        multiQueueSync = true;
        auto fence = (GED3D12Fence *)waitFence.get();
        commandQueue->Wait(fence->fence.Get(),1);
        commandQueue->Signal(fence->fence.Get(),0);
         multiQueueSync = false;
    };

    void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer){
        HRESULT hr;
        auto d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
        d3d12_buffer->closed = true;
        
        commandLists.push_back(d3d12_buffer->commandList.Get());
    };

    void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                                  SharedHandle<GEFence> &signalFence) {
        multiQueueSync = true;
        auto d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
        auto fence = (GED3D12Fence *)signalFence.get();
        d3d12_buffer->closed = true;
        d3d12_buffer->commandList->Close();
        commandQueue->ExecuteCommandLists(1,(ID3D12CommandList *const *)d3d12_buffer->commandList.GetAddressOf());
        commandQueue->Signal(fence->fence.Get(),1);
        multiQueueSync = false;
    }

    void GED3D12CommandBuffer::reset(){
        closed = false;
        firstRenderPass = true;
        commandList->Reset(commandAllocator.Get(),nullptr);
        commandAllocator->Reset();
    };

    void GED3D12CommandQueue::commitToGPU(){
        if(!multiQueueSync) {
            for (auto &cl: commandLists) {
                cl->Close();
            }
            commandQueue->ExecuteCommandLists(commandLists.size(), (ID3D12CommandList *const *) commandLists.data());
        }
    };

    void GED3D12CommandQueue::commitToGPUAndWait() {
        commitToGPU();
        commandQueue->Signal(fence.Get(),1);
        WaitForSingleObject(cpuEvent,INFINITE);
        commandQueue->Signal(fence.Get(),0);
    }

    SharedHandle<GECommandBuffer> GED3D12CommandQueue::getAvailableBuffer(){
        HRESULT hr;
        ID3D12GraphicsCommandList6 *commandList;

        ID3D12CommandAllocator * commandAllocator;

        hr = engine->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&commandAllocator));

        if(FAILED(hr)){
            exit(1);
        };

        hr = engine->d3d12_device->CreateCommandList(engine->d3d12_device->GetNodeCount(),D3D12_COMMAND_LIST_TYPE_DIRECT,commandAllocator,NULL,IID_PPV_ARGS(&commandList));
        if(FAILED(hr)){
             MessageBoxA(GetForegroundWindow(),"Failed to Create Command List","NOTE",MB_OK);
             std::cout << "ERROR:" << std::hex << hr << std::endl;
            exit(1);
        };
        return SharedHandle<GECommandBuffer>(new GED3D12CommandBuffer(commandList,commandAllocator,this));

    };

    ID3D12GraphicsCommandList6 *GED3D12CommandQueue::getLastCommandList() {
        return commandLists.back();
    }

    GED3D12CommandQueue::~GED3D12CommandQueue(){
        CloseHandle(cpuEvent);
    }


_NAMESPACE_END_