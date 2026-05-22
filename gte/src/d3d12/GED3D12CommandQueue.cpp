#include "GED3D12CommandQueue.h"

#include "../common/GEResourceTracker.h"
#include "GED3D12Pipeline.h"
#include "GED3D12RenderTarget.h"
#include "GED3D12Texture.h"

#include <memory>

#include <d3d12.h>
_NAMESPACE_BEGIN_

#ifndef D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE
#    define D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE                                                                   \
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
#endif
// GED3D12CommandBuffer::GED3D12CommandBuffer(){};
// void GED3D12CommandBuffer::commitToBuffer(){};
GED3D12CommandQueue::GED3D12CommandQueue(GED3D12Engine *engine, unsigned size)
    : GECommandQueue(size), engine(engine), currentCount(0) {
    HRESULT hr;

    hr = engine->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

    if (FAILED(hr)) {
        exit(1);
    };

    hr = engine->d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retentionFence));
    if (FAILED(hr)) {
        exit(1);
    };

    cpuEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    fence->SetEventOnCompletion(1, cpuEvent);

    D3D12_COMMAND_QUEUE_DESC desc;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = engine->d3d12_device->GetNodeCount();
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = engine->d3d12_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr)) {
        MessageBoxA(GetForegroundWindow(), "Failed to Create Command Queue.", "NOTE", MB_OK);
        exit(1);
    };
    traceResourceId = ResourceTracking::Tracker::instance().nextResourceId();
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Create, ResourceTracking::Backend::D3D12,
                                               "CommandQueue", traceResourceId, commandQueue.Get());
};

Retention::FenceGate GED3D12CommandQueue::gateForNextSubmit() {
    const std::uint64_t v = nextSubmitValue + 1;
    ComPtr<ID3D12Fence> f = retentionFence;
    return [f, v]() { return f->GetCompletedValue() >= v; };
}

void GED3D12CommandQueue::flushPendingRetentionUnder(const Retention::FenceGate &gate) {
    for (auto &buf : retainedCommandBuffers) {
        engine->retentionQueue.retainShared(std::move(buf), {gate});
    }
    retainedCommandBuffers.clear();
    for (auto &heap : retainedDescriptorHeaps) {
        engine->retentionQueue.enqueue({gate},
                                       [h = std::move(heap)]() mutable { h.Reset(); });
    }
    retainedDescriptorHeaps.clear();
}

GED3D12CommandBuffer::GED3D12CommandBuffer(ID3D12GraphicsCommandList6 *commandList,
                                           ID3D12CommandAllocator *commandAllocator, GED3D12CommandQueue *parentQueue)
    : commandList(commandList), commandAllocator(commandAllocator), parentQueue(parentQueue), inComputePass(false),
      inBlitPass(false), traceResourceId(ResourceTracking::Tracker::instance().nextResourceId()) {

    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Create, ResourceTracking::Backend::D3D12,
                                               "CommandBuffer", traceResourceId, this->commandList.Get());
};

unsigned int GED3D12CommandBuffer::getRootParameterIndexOfResource(unsigned int id, omegasl_shader &shader) {
    bool isSRV = false, isUAV = false, isCBV = false, isDescriptorTable = false, isSampler = false;
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};

    unsigned relative_index = 0;
    for (auto &l : layoutArr) {
        if (l.location == id) {
            relative_index = l.gpu_relative_loc;
            if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    isSRV = true;
                } else {
                    isUAV = true;
                }
            } else if (l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                // §2.4 constant buffer — bound as a root CBV.
                isCBV = true;
            } else if (l.type == OMEGASL_SHADER_SAMPLER1D_DESC || l.type == OMEGASL_SHADER_SAMPLER2D_DESC
                       || l.type == OMEGASL_SHADER_SAMPLER3D_DESC || l.type == OMEGASL_SHADER_SAMPLERCUBE_DESC) {
                // Extension 8 — runtime sampler. A sampler is a descriptor
                // table, but its range type is SAMPLER, so it must not match a
                // texture's SRV/UAV table that happens to share the same
                // register number (HLSL `t#` and `s#` are independent register
                // classes — `texture2d t0` + `sampler s0` both have
                // gpu_relative_loc 0).
                isDescriptorTable = true;
                isSampler = true;
            } else {
                isDescriptorTable = true;
            }
            break;
        }
    }

    unsigned regSpace;
    if (shader.type == OMEGASL_SHADER_FRAGMENT) {
        regSpace = 1;
    } else {
        regSpace = 0;
    }

    unsigned idx = 0;
    for (; idx < currentRootSignature->NumParameters; idx++) {
        auto &param = currentRootSignature->pParameters[idx];
        // std::cout << "PARAM_TYPE:" << (int)param.ParameterType << std::endl;
        if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV && isSRV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV && isUAV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && isCBV) {
            if (param.Descriptor.ShaderRegister == relative_index && param.Descriptor.RegisterSpace == regSpace) {
                break;
            }
        } else if (param.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && isDescriptorTable) {
            // Descriptor-table match must include the range *type*.
            // The mipmap-gen pipeline (srcMip = SRV at t0, dstMip = UAV
            // at u0) lays out two descriptor tables whose first range
            // shares BaseShaderRegister=0 — they differ only by range
            // type (SRV vs UAV). Without the type check, the UAV
            // lookup matched the SRV table at parameter 0 and the
            // dispatch loop double-bound the same root parameter with
            // mismatched-type handles — D3D12 debug-layer error
            // INVALID_DESCRIPTOR_HANDLE.
            auto &range = param.DescriptorTable.pDescriptorRanges[0];
            const bool typeMatches =
                (isSRV && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) ||
                (isUAV && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) ||
                // Extension 8 — a runtime sampler must match only a SAMPLER
                // range, never a texture's SRV/UAV table that shares the
                // register number (independent HLSL register classes).
                (isSampler && range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) ||
                // CBV / other descriptor-table lookups don't set
                // isSRV/isUAV/isSampler; fall through to a register-only match.
                (!isSRV && !isUAV && !isSampler);
            if (typeMatches &&
                range.BaseShaderRegister == relative_index &&
                range.RegisterSpace == regSpace) {
                break;
            }
        }
    }
    return idx;
}

// Pipeline-Completion-Extension-Plan §6.3 — locate the layout-desc that
// owns the given bind location and run the kind/sample-count check
// against the bound texture. Logs a diagnostic on mismatch and returns
// false; the caller may either skip the bind or carry on (we keep the
// command list valid but flag the user).
static bool checkTextureBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader,
                                          GETexture &tex) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateTextureBindKind((int)l.type, tex.getKind(),
                                           tex.getSampleCount(), shader.name, location);
        }
    }
    return true;
}

// Extension 8 §8.5 — sampler-bind validation. Rejects static-sampler and
// non-sampler slots via validateSamplerBindKind().
static bool checkSamplerBindAgainstShader(unsigned int location,
                                          const omegasl_shader &shader) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout,
                                                                shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == location) {
            return validateSamplerBindKind((int)l.type, shader.name, location);
        }
    }
    return true;
}

void GED3D12CommandBuffer::rebindDescriptorHeaps() {
    ID3D12DescriptorHeap *heaps[2];
    unsigned n = 0;
    if (currentResourceDescHeap) heaps[n++] = currentResourceDescHeap;
    if (currentSamplerDescHeap)  heaps[n++] = currentSamplerDescHeap;
    if (n) commandList->SetDescriptorHeaps(n, heaps);
}

D3D12_RESOURCE_STATES
GED3D12CommandBuffer::getRequiredResourceStateForResourceID(unsigned int &id, omegasl_shader &shader) {
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto &l : layoutArr) {
        if (l.location == id) {
            D3D12_RESOURCE_STATES state;
            if (l.type == OMEGASL_SHADER_TEXTURE1D_DESC || l.type == OMEGASL_SHADER_TEXTURE2D_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE3D_DESC ||
                /// OmegaSL §2.1 Phase A — cube/array/MS layout types are
                /// emitted by the compiler. Phase B will pick the correct
                /// SRV view-dimension when the texture is bound. Until then
                /// the resource-state transition logic is identical to a
                /// plain texture (SRV vs UAV depending on IO direction),
                /// so they fall through to the same branch.
                l.type == OMEGASL_SHADER_TEXTURE1D_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURECUBE_DESC ||
                l.type == OMEGASL_SHADER_TEXTURECUBE_ARRAY_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_MS_DESC ||
                l.type == OMEGASL_SHADER_TEXTURE2D_MS_ARRAY_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                } else {
                    state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            } else if (l.type == OMEGASL_SHADER_BUFFER_DESC) {
                if (l.io_mode == OMEGASL_SHADER_DESC_IO_IN) {
                    state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                } else {
                    state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                }
            } else if (l.type == OMEGASL_SHADER_UNIFORM_DESC) {
                // §2.4 constant buffer — read-only on the GPU.
                state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            } else {
                DEBUG_STREAM("This resource cannot be transitioned");
                exit(1);
            }
            return state;
        }
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

TextureSwizzle
GED3D12CommandBuffer::resolveEffectiveSwizzle(const TextureSwizzle & runtime,unsigned id,omegasl_shader &shader){
    if(!runtime.isIdentity()) return runtime;
    // Layout-desc encoding: 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
    auto decode = [](unsigned char b) -> TextureSwizzleChannel {
        switch(b){
            case 1: return TextureSwizzleChannel::Red;
            case 2: return TextureSwizzleChannel::Green;
            case 3: return TextureSwizzleChannel::Blue;
            case 4: return TextureSwizzleChannel::Alpha;
            case 5: return TextureSwizzleChannel::Zero;
            case 6: return TextureSwizzleChannel::One;
            default: return TextureSwizzleChannel::Identity;
        }
    };
    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutArr{shader.pLayout, shader.pLayout + shader.nLayout};
    for (auto & l : layoutArr) {
        if(l.location == id){
            if(l.swizzle_desc.r == 0 && l.swizzle_desc.g == 0
               && l.swizzle_desc.b == 0 && l.swizzle_desc.a == 0){
                return TextureSwizzle::identity();
            }
            return TextureSwizzle{
                decode(l.swizzle_desc.r),
                decode(l.swizzle_desc.g),
                decode(l.swizzle_desc.b),
                decode(l.swizzle_desc.a)
            };
        }
    }
    return TextureSwizzle::identity();
}

void GED3D12CommandBuffer::startBlitPass() {
    inBlitPass = true;
};

void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest) {
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *srcText = (GED3D12Texture *)src.get(), *destText = (GED3D12Texture *)dest.get();
    /// Resource Synchronization Checks
    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(), srcText->currentState,
                                                                        D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destText->resource.Get(), destText->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }
    commandList->CopyResource(destText->resource.Get(), srcText->resource.Get());
}

void GED3D12CommandBuffer::copyTextureToTexture(SharedHandle<GETexture> &src, SharedHandle<GETexture> &dest,
                                                const TextureRegion &region, const GPoint3D &destCoord) {
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *srcText = (GED3D12Texture *)src.get(), *destText = (GED3D12Texture *)dest.get();

    /// Resource Synchronization Checks
    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcText->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(srcText->resource.Get(), srcText->currentState,
                                                                        D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destText->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destText->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destText->resource.Get()));
        }

        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destText->resource.Get(), destText->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        srcText->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcText->resource.Get()), destLoc(destText->resource.Get());
    LONG top_pos = LONG(region.h) - LONG(region.y);
    CD3DX12_BOX _region((LONG)region.x, top_pos, LONG(region.x + region.w), LONG(top_pos + region.h));
    commandList->CopyTextureRegion(&destLoc, (UINT)destCoord.x, (UINT)destCoord.y, (UINT)destCoord.z, &srcLoc,
                                   &_region);
}

void GED3D12CommandBuffer::copyBufferToBuffer(SharedHandle<GEBuffer> &src, SharedHandle<GEBuffer> &dest,
                                              size_t size, size_t srcOffset, size_t destOffset) {
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *srcBuf = (GED3D12Buffer *)src.get();
    auto *destBuf = (GED3D12Buffer *)dest.get();

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcBuf->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE &&
        srcBuf->currentState != D3D12_RESOURCE_STATE_GENERIC_READ) {
        if (srcBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcBuf->buffer.Get(), srcBuf->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcBuf->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destBuf->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destBuf->buffer.Get(), destBuf->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destBuf->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    UINT64 bytes = size == 0 ? srcBuf->buffer->GetDesc().Width - srcOffset : size;
    commandList->CopyBufferRegion(destBuf->buffer.Get(), destOffset, srcBuf->buffer.Get(), srcOffset, bytes);
}

void GED3D12CommandBuffer::copyBufferToTexture(SharedHandle<GEBuffer> &src, SharedHandle<GETexture> &dest,
                                               size_t bytesPerRow, size_t bytesPerImage,
                                               const TextureRegion &destRegion, size_t srcBufferOffset) {
    (void)bytesPerImage;
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *srcBuf = (GED3D12Buffer *)src.get();
    auto *destTex = (GED3D12Texture *)dest.get();

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcBuf->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE &&
        srcBuf->currentState != D3D12_RESOURCE_STATE_GENERIC_READ) {
        if (srcBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcBuf->buffer.Get(), srcBuf->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcBuf->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destTex->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destTex->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destTex->resource.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destTex->resource.Get(), destTex->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destTex->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    footprint.Offset = srcBufferOffset;
    footprint.Footprint.Format = destTex->resource->GetDesc().Format;
    footprint.Footprint.Width = destRegion.w;
    footprint.Footprint.Height = destRegion.h;
    footprint.Footprint.Depth = destRegion.d == 0 ? 1 : destRegion.d;
    footprint.Footprint.RowPitch = static_cast<UINT>(bytesPerRow);

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcBuf->buffer.Get(), footprint);
    CD3DX12_TEXTURE_COPY_LOCATION destLoc(destTex->resource.Get(), 0);

    CD3DX12_BOX srcBox(0, 0, 0,
                       (LONG)destRegion.w,
                       (LONG)destRegion.h,
                       (LONG)(destRegion.d == 0 ? 1 : destRegion.d));
    commandList->CopyTextureRegion(&destLoc,
                                   destRegion.x, destRegion.y, destRegion.z,
                                   &srcLoc, &srcBox);
}

void GED3D12CommandBuffer::copyTextureToBuffer(SharedHandle<GETexture> &src, SharedHandle<GEBuffer> &dest,
                                               size_t bytesPerRow, size_t bytesPerImage,
                                               const TextureRegion &srcRegion, size_t destBufferOffset) {
    (void)bytesPerImage;
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *srcTex = (GED3D12Texture *)src.get();
    auto *destBuf = (GED3D12Buffer *)dest.get();

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (srcTex->currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        if (srcTex->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(srcTex->resource.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            srcTex->resource.Get(), srcTex->currentState, D3D12_RESOURCE_STATE_COPY_SOURCE));
        srcTex->currentState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destBuf->currentState != D3D12_RESOURCE_STATE_COPY_DEST) {
        if (destBuf->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(destBuf->buffer.Get()));
        }
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            destBuf->buffer.Get(), destBuf->currentState, D3D12_RESOURCE_STATE_COPY_DEST));
        destBuf->currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    footprint.Offset = destBufferOffset;
    footprint.Footprint.Format = srcTex->resource->GetDesc().Format;
    footprint.Footprint.Width = srcRegion.w;
    footprint.Footprint.Height = srcRegion.h;
    footprint.Footprint.Depth = srcRegion.d == 0 ? 1 : srcRegion.d;
    footprint.Footprint.RowPitch = static_cast<UINT>(bytesPerRow);

    CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcTex->resource.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION destLoc(destBuf->buffer.Get(), footprint);

    CD3DX12_BOX srcBox((LONG)srcRegion.x,
                       (LONG)srcRegion.y,
                       (LONG)srcRegion.z,
                       (LONG)(srcRegion.x + srcRegion.w),
                       (LONG)(srcRegion.y + srcRegion.h),
                       (LONG)(srcRegion.z + (srcRegion.d == 0 ? 1 : srcRegion.d)));
    commandList->CopyTextureRegion(&destLoc, 0, 0, 0, &srcLoc, &srcBox);
}

void GED3D12CommandBuffer::generateMipmaps(SharedHandle<GETexture> &texture) {
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *tex = (GED3D12Texture *)texture.get();
    const auto texDesc = tex->resource->GetDesc();
    if (texDesc.MipLevels <= 1) {
        return;
    }
    if (texDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        texDesc.DepthOrArraySize != 1) {
        DEBUG_STREAM("GED3D12CommandBuffer::generateMipmaps: only 2D, single-slice "
                     "textures are supported by the box-filter compute kernel. tex="
                     << tex);
        return;
    }
    if ((texDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        DEBUG_STREAM("GED3D12CommandBuffer::generateMipmaps: texture was not created "
                     "with ALLOW_UNORDERED_ACCESS; cannot bind mip levels as UAVs. tex="
                     << tex);
        return;
    }

    auto *engine = parentQueue->engine;
    if (!engine->ensureMipmapGenPipeline()) {
        DEBUG_STREAM("GED3D12CommandBuffer::generateMipmaps: pipeline init failed.");
        return;
    }

    auto *device = engine->d3d12_device.Get();
    auto *pipeline = (GED3D12ComputePipelineState *)engine->mipmapGenPipeline.get();
    auto &shaderInternal = pipeline->computeShader->internal;

    // Point `currentRootSignature` at the mipmap-gen pipeline's root
    // signature *before* asking `getRootParameterIndexOfResource` to
    // walk it. `currentRootSignature` is normally assigned by
    // `setComputePipelineState`, but this function bypasses that path
    // (the per-mip loop drives the dispatch directly). Without this
    // assignment the lookup walks a null/stale root-signature pointer
    // and access-violates on `currentRootSignature->NumParameters`.
    // We assign here rather than calling `setComputePipelineState`
    // because the dispatch loop below issues its own
    // `SetPipelineState` / `SetComputeRootSignature` interleaved with
    // per-mip barriers, and routing through `setComputePipelineState`
    // would also flip the `inComputePass` guard which doesn't apply
    // inside a blit pass.
    currentRootSignature = &pipeline->rootSignatureDesc;

    // OmegaSL location 0 = srcMip (in / SRV), 1 = dstMip (out / UAV).
    unsigned srvId = 0, uavId = 1;
    const unsigned srvRoot = getRootParameterIndexOfResource(srvId, shaderInternal);
    const unsigned uavRoot = getRootParameterIndexOfResource(uavId, shaderInternal);

    const UINT mipCount = texDesc.MipLevels;
    const DXGI_FORMAT format = texDesc.Format;

    // One SRV (mip N) + one UAV (mip N+1) per pair, contiguous in a single shader-visible heap.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = (mipCount - 1) * 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = device->GetNodeCount();

    ComPtr<ID3D12DescriptorHeap> descHeap;
    HRESULT hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descHeap));
    if (FAILED(hr)) {
        DEBUG_STREAM("GED3D12CommandBuffer::generateMipmaps: CreateDescriptorHeap failed hr="
                     << std::hex << hr);
        return;
    }

    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuStart(descHeap->GetCPUDescriptorHandleForHeapStart());

    for (UINT i = 0; i + 1 < mipCount; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = i;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex->resource.Get(), &srvDesc, cpuStart);
        cpuStart.Offset(1, incr);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = i + 1;
        device->CreateUnorderedAccessView(tex->resource.Get(), nullptr, &uavDesc, cpuStart);
        cpuStart.Offset(1, incr);
    }

    // Move the whole resource into UAV state so per-mip subresource transitions below are valid.
    if (tex->currentState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(), tex->currentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &barrier);
        tex->currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ID3D12DescriptorHeap *heaps[] = { descHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(pipeline->rootSignature.Get());
    commandList->SetPipelineState(pipeline->pipelineState.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gpuStart(descHeap->GetGPUDescriptorHandleForHeapStart());

    for (UINT i = 0; i + 1 < mipCount; ++i) {
        const UINT64 dstW = std::max<UINT64>(1, texDesc.Width  >> (i + 1));
        const UINT   dstH = std::max<UINT>  (1, texDesc.Height >> (i + 1));

        // Transition src mip i → NON_PIXEL_SHADER_RESOURCE; dst mip i+1 stays in UAV.
        auto preBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            i);
        commandList->ResourceBarrier(1, &preBarrier);

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(gpuStart, i * 2,     incr);
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(gpuStart, i * 2 + 1, incr);
        commandList->SetComputeRootDescriptorTable(srvRoot, srvHandle);
        commandList->SetComputeRootDescriptorTable(uavRoot, uavHandle);

        const UINT groupsX = static_cast<UINT>((dstW + 7) / 8);
        const UINT groupsY = static_cast<UINT>((dstH + 7) / 8);
        commandList->Dispatch(groupsX, groupsY, 1);

        // UAV barrier on dst mip + transition src mip i back to UAV so we end in a uniform state.
        D3D12_RESOURCE_BARRIER postBarriers[2];
        postBarriers[0] = CD3DX12_RESOURCE_BARRIER::UAV(tex->resource.Get());
        postBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->resource.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            i);
        commandList->ResourceBarrier(2, postBarriers);
    }

    // Keep the descriptor heap alive until the GPU is done with the command list.
    parentQueue->retainedDescriptorHeaps.push_back(descHeap);
}

void GED3D12CommandBuffer::fillBuffer(SharedHandle<GEBuffer> &buffer, uint32_t value,
                                      size_t offset, size_t size) {
    assert(inBlitPass && "Not in Blit Pass! Exiting...");
    auto *buf = (GED3D12Buffer *)buffer.get();
    const auto bufDesc = buf->buffer->GetDesc();
    const UINT64 totalSize = bufDesc.Width;
    const UINT64 fillOffset = static_cast<UINT64>(offset);
    const UINT64 fillSize =
        size == 0 ? (totalSize - fillOffset) : static_cast<UINT64>(size);

    if ((bufDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        // ClearUnorderedAccessViewUint requires the buffer to have been created
        // as a UAV. Buffers without UAV access need either a staging upload path
        // or a compute shader fill; neither is currently wired up.
        DEBUG_STREAM("GED3D12CommandBuffer::fillBuffer: buffer was not created "
                     "with ALLOW_UNORDERED_ACCESS; fill skipped. Requires UAV-"
                     "capable buffer or compute-shader path. buffer="
                     << buf);
        return;
    }

    OmegaCommon::Vector<D3D12_RESOURCE_BARRIER> resourceBarriers;
    if (buf->currentState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        resourceBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            buf->buffer.Get(), buf->currentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        buf->currentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (!resourceBarriers.empty()) {
        commandList->ResourceBarrier(resourceBarriers.size(), resourceBarriers.data());
    }

    ID3D12DescriptorHeap *heap = buf->bufferDescHeap.Get();
    if (heap == nullptr) {
        DEBUG_STREAM("GED3D12CommandBuffer::fillBuffer: buffer has no descriptor "
                     "heap; cannot resolve UAV handle.");
        return;
    }
    const UINT values[4] = {value, value, value, value};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = heap->GetCPUDescriptorHandleForHeapStart();
    commandList->SetDescriptorHeaps(1, &heap);
    commandList->ClearUnorderedAccessViewUint(gpuHandle, cpuHandle,
                                              buf->buffer.Get(), values, 0, nullptr);
    (void)fillOffset;
    (void)fillSize;
}

void GED3D12CommandBuffer::finishBlitPass() {
    inBlitPass = false;
};

void GED3D12CommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                            SharedHandle<GETexture> &src,
                                            SharedHandle<GETexture> &dest) {
    auto *dst = (GED3D12Texture *)dest.get();
    auto descD = dst->resource->GetDesc();
    TextureRegion srcRegion{0, 0, 0, (unsigned)descD.Width, descD.Height, 1};
    TextureRegion destRegion{0, 0, 0, (unsigned)descD.Width, descD.Height, 1};
    blitWithPipeline(pipelineState, src, dest, srcRegion, destRegion);
}

void GED3D12CommandBuffer::blitWithPipeline(SharedHandle<GEBlitPipelineState> &pipelineState,
                                            SharedHandle<GETexture> &src,
                                            SharedHandle<GETexture> &dest,
                                            const TextureRegion &srcRegion,
                                            const TextureRegion &destRegion) {
    (void)srcRegion;
    assert(!inRenderPass && !inBlitPass && !inComputePass &&
           "blitWithPipeline must not be called inside an existing pass scope");
    if (!pipelineState) {
        DEBUG_STREAM("blitWithPipeline: pipelineState is null");
        return;
    }
    auto *blitPipe = (GED3D12BlitPipelineState *)pipelineState.get();
    if (!blitPipe->renderPipeline) {
        DEBUG_STREAM("blitWithPipeline: underlying render pipeline is null");
        return;
    }

    // One-shot texture render target wrapping `dest`. The SharedHandle is
    // kept on the stack so the underlying object outlives the pass.
    TextureRenderTargetDescriptor trtDesc{};
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture = dest;
    auto trtSh = parentQueue->engine->makeTextureRenderTarget(trtDesc);
    if (!trtSh) {
        DEBUG_STREAM("blitWithPipeline: makeTextureRenderTarget failed");
        return;
    }

    GERenderPassDescriptor rpDesc{};
    rpDesc.tRenderTarget = trtSh.get();
    rpDesc.colorAttachments.emplace_back(
        GERenderPassDescriptor::ColorAttachment::ClearColor(0.f, 0.f, 0.f, 0.f),
        GERenderPassDescriptor::ColorAttachment::Discard);
    rpDesc.depthStencilAttachment.disabled = true;

    startRenderPass(rpDesc);
    setRenderPipelineState(blitPipe->renderPipeline);
    bindResourceAtFragmentShader(src, 0, TextureSwizzle::identity());
    GEViewport vp{(float)destRegion.x, (float)destRegion.y,
                  (float)destRegion.w, (float)destRegion.h,
                  0.f, 1.f};
    setViewports({vp});
    GEScissorRect sr{(float)destRegion.x, (float)destRegion.y,
                     (float)destRegion.w, (float)destRegion.h};
    setScissorRects({sr});
    drawPolygons(GECommandBuffer::Triangle, 3, 0);
    finishRenderPass();
}

void GED3D12CommandBuffer::beginAccelStructPass() {}

static void fillGeometryDescsFromGE(const GEAccelerationStructDescriptor &desc,
                                    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> &out) {
    for (auto &g : desc.data) {
        D3D12_RAYTRACING_GEOMETRY_DESC gd{};
        gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        if (g.type == GEAccelerationStructDescriptor::Geometry::TRIANGLES) {
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(g.getTriangleList().buffer);
            if (d3dBuf) {
                gd.Triangles.VertexBuffer.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                gd.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
                gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                gd.Triangles.VertexCount = static_cast<UINT>(d3dBuf->size() / (sizeof(float) * 3));
            }
        } else {
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            auto d3dBuf = std::dynamic_pointer_cast<GED3D12Buffer>(g.getAabb().buffer);
            if (d3dBuf) {
                gd.AABBs.AABBs.StartAddress = d3dBuf->buffer->GetGPUVirtualAddress();
                gd.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
                gd.AABBs.AABBCount = d3dBuf->size() / sizeof(D3D12_RAYTRACING_AABB);
            }
        }
        out.push_back(gd);
    }
}

void GED3D12CommandBuffer::buildAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                      const GEAccelerationStructDescriptor &desc) {
    auto accel_struct = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    fillGeometryDescsFromGE(desc, geometryDescs);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.SourceAccelerationStructureData = NULL;
    d.DestAccelerationStructureData = accel_struct->structBuffer->buffer->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = accel_struct->scratchBuffer->buffer->GetGPUVirtualAddress();
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    if (geometryDescs.empty()) {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    } else {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        d.Inputs.pGeometryDescs = geometryDescs.data();
    }

    commandList->BuildRaytracingAccelerationStructure(&d, 0, nullptr);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(accel_struct->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::copyAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                     SharedHandle<GEAccelerationStruct> &dest) {
    auto srcAS = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
    auto destAS = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(dest);
    commandList->CopyRaytracingAccelerationStructure(destAS->structBuffer->buffer->GetGPUVirtualAddress(),
                                                     srcAS->structBuffer->buffer->GetGPUVirtualAddress(),
                                                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(destAS->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::refitAccelerationStructure(SharedHandle<GEAccelerationStruct> &src,
                                                      SharedHandle<GEAccelerationStruct> &dest,
                                                      const GEAccelerationStructDescriptor &desc) {
    auto accel_struct_src = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(src);
    auto accel_struct_dest = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(dest);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs;
    fillGeometryDescsFromGE(desc, geometryDescs);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.SourceAccelerationStructureData = accel_struct_src->structBuffer->buffer->GetGPUVirtualAddress();
    d.DestAccelerationStructureData = accel_struct_dest->structBuffer->buffer->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = accel_struct_dest->scratchBuffer->buffer->GetGPUVirtualAddress();
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    if (geometryDescs.empty()) {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    } else {
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.NumDescs = static_cast<UINT>(geometryDescs.size());
        d.Inputs.pGeometryDescs = geometryDescs.data();
    }

    commandList->BuildRaytracingAccelerationStructure(&d, 0, nullptr);

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(accel_struct_dest->structBuffer->buffer.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
}

void GED3D12CommandBuffer::finishAccelStructPass() {
    
}

void GED3D12CommandBuffer::startRenderPass(const GERenderPassDescriptor &desc) {
    inRenderPass = true;
    assert(!inComputePass && "Cannot start a Render Pass while in a compute pass.");
    static constexpr unsigned kMaxRT = 8;
    D3D12_RENDER_PASS_RENDER_TARGET_DESC rt_descs[kMaxRT] = {};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC ds_desc;

    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS resolveParams;

    CD3DX12_CPU_DESCRIPTOR_HANDLE cpu_handle;
    CD3DX12_CPU_DESCRIPTOR_HANDLE ds_cpu_handle;

    const auto rtvDescSize =
        parentQueue->engine->d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto dsvDescSize =
        parentQueue->engine->d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    if (desc.nRenderTarget) {
        auto *nativeRenderTarget = (GED3D12NativeRenderTarget *)desc.nRenderTarget;
        if (desc.multisampleResolve) {
            multisampleResolvePass = true;
            auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());

            if (!desc.depthStencilAttachment.disabled) {
                ds_cpu_handle =
                    CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }

            D3D12_RESOURCE_STATES resource_state;
            if (firstRenderPass) {
                resource_state = D3D12_RESOURCE_STATE_PRESENT;
            } else {
                resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            auto barrier =
                CD3DX12_RESOURCE_BARRIER::Transition(nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex],
                                                     resource_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            commandList->ResourceBarrier(1, &barrier);

            auto dxgi_format = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex]->GetDesc().Format;
            RECT rc;
            GetClientRect(nativeRenderTarget->hwnd, &rc);
            resolveParams.pSrcResource = resolveTexture->resource.Get();
            resolveParams.SubresourceCount = 1;
            resolveParams.PreserveResolveSource = TRUE;
            resolveParams.pDstResource = nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex];
            D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS params;
            params.DstSubresource = 0;
            params.SrcSubresource = 0;
            params.DstX = 0;
            params.DstY = 0;
            params.SrcRect = CD3DX12_RECT(rc.left, rc.top, rc.right, rc.bottom);
            resolveParams.pSubresourceParameters = &params;
            resolveParams.Format = dxgi_format;
            resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
        } else {
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(nativeRenderTarget->rtvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                                              nativeRenderTarget->frameIndex, rtvDescSize);
            if (!desc.depthStencilAttachment.disabled) {
                ds_cpu_handle =
                    CD3DX12_CPU_DESCRIPTOR_HANDLE(nativeRenderTarget->dsvDescHeap->GetCPUDescriptorHandleForHeapStart(),
                                                  nativeRenderTarget->frameIndex, dsvDescSize);
            }
            if (firstRenderPass) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    nativeRenderTarget->renderTargets[nativeRenderTarget->frameIndex], D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);

                commandList->ResourceBarrier(1, &barrier);
            }
        }
        currentTarget.native = nativeRenderTarget;
    } else if (desc.tRenderTarget) {
        auto *textureRenderTarget = (GED3D12TextureRenderTarget *)desc.tRenderTarget;
        if (desc.multisampleResolve) {
            auto resolveTexture = (GED3D12Texture *)desc.resolveDesc.multiSampleTextureSrc.get();
            cpu_handle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
            if (!desc.depthStencilAttachment.disabled) {
                ds_cpu_handle =
                    CD3DX12_CPU_DESCRIPTOR_HANDLE(resolveTexture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }

            auto *targetTexture = textureRenderTarget->texture.get();
            if (targetTexture != nullptr) {
                auto currentState = targetTexture->currentState;
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(targetTexture->resource.Get(), currentState,
                                                                    D3D12_RESOURCE_STATE_RESOLVE_DEST);
                commandList->ResourceBarrier(1, &barrier);
                targetTexture->currentState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            }

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
            params.SrcRect = CD3DX12_RECT(0, 0, desc.Width, desc.Height);
            resolveParams.pSubresourceParameters = &params;
            resolveParams.Format = dxgi_format;
            resolveParams.ResolveMode = D3D12_RESOLVE_MODE_MAX;
        } else {
            auto *targetTexture = textureRenderTarget->texture.get();
            if (targetTexture != nullptr) {
                if (targetTexture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                    auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(targetTexture->resource.Get());
                    commandList->ResourceBarrier(1, &barrier);
                }
                if (!(targetTexture->currentState & D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                        targetTexture->resource.Get(), targetTexture->currentState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    commandList->ResourceBarrier(1, &barrier);
                    targetTexture->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
            }
            cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                textureRenderTarget->texture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
            if (!desc.depthStencilAttachment.disabled) {
                ds_cpu_handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(
                    textureRenderTarget->texture->dsvDescHeap->GetCPUDescriptorHandleForHeapStart());
            }
        }
        currentTarget.texture = textureRenderTarget;
    };

    const unsigned attachmentCount =
        desc.colorAttachments.empty() ? 1u : (unsigned)std::min<size_t>(desc.colorAttachments.size(), kMaxRT);

    for (unsigned i = 0; i < attachmentCount; ++i) {
        D3D12_RENDER_PASS_RENDER_TARGET_DESC &rt_desc = rt_descs[i];
        CD3DX12_CPU_DESCRIPTOR_HANDLE attachmentHandle;
        const GERenderPassDescriptor::ColorAttachment *attachment =
            desc.colorAttachments.empty() ? nullptr : &desc.colorAttachments[i];

        if (i == 0 && (attachment == nullptr || attachment->texture == nullptr)) {
            attachmentHandle = cpu_handle;
        } else {
            assert(attachment != nullptr && attachment->texture != nullptr &&
                   "Color attachments beyond index 0 must supply an explicit texture.");
            auto *attachTexture = (GED3D12Texture *)attachment->texture.get();
            if (attachTexture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(attachTexture->resource.Get());
                commandList->ResourceBarrier(1, &barrier);
            }
            if (!(attachTexture->currentState & D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    attachTexture->resource.Get(), attachTexture->currentState,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1, &barrier);
                attachTexture->currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            attachmentHandle =
                CD3DX12_CPU_DESCRIPTOR_HANDLE(attachTexture->rtvDescHeap->GetCPUDescriptorHandleForHeapStart());
        }

        rt_desc.cpuDescriptor = attachmentHandle;

        if (i == 0 && desc.multisampleResolve) {
            rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            rt_desc.EndingAccess.Resolve = resolveParams;
        }

        const auto loadAction = (attachment != nullptr)
                                    ? attachment->loadAction
                                    : GERenderPassDescriptor::ColorAttachment::Discard;
        const bool useResolveEnd = (i == 0 && desc.multisampleResolve);

        switch (loadAction) {
            case GERenderPassDescriptor::ColorAttachment::Load: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::LoadPreserve: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Discard: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::ColorAttachment::Clear: {
                rt_desc.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                const FLOAT colors[] = {
                    attachment ? attachment->clearColor.r : 0.f,
                    attachment ? attachment->clearColor.g : 0.f,
                    attachment ? attachment->clearColor.b : 0.f,
                    attachment ? attachment->clearColor.a : 0.f,
                };
                rt_desc.BeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_R8G8B8A8_UNORM, colors);
                if (!useResolveEnd)
                    rt_desc.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }
    }

    if (!desc.depthStencilAttachment.disabled) {
        ds_desc.cpuDescriptor = ds_cpu_handle;
    }

    if (desc.depthStencilAttachment.disabled) {
        commandList->BeginRenderPass(attachmentCount, rt_descs, nullptr, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    } else {

        if (desc.multisampleResolve) {
            ds_desc.DepthEndingAccess.Type = ds_desc.StencilEndingAccess.Type =
                D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
            resolveParams.Format = DXGI_FORMAT_UNKNOWN;
            ds_desc.DepthEndingAccess.Resolve = ds_desc.StencilEndingAccess.Resolve = resolveParams;
        }

        switch (desc.depthStencilAttachment.depthloadAction) {
            case GERenderPassDescriptor::DepthStencilAttachment::Discard : {
                   ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve: {
                ds_desc.DepthBeginningAccess.Type = ds_desc.StencilBeginningAccess.Type =
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                ds_desc.DepthBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                ds_desc.DepthBeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN, desc.depthStencilAttachment.clearDepth,
                                        desc.depthStencilAttachment.clearStencil);
                if (!desc.multisampleResolve)
                    ds_desc.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }

        switch (desc.depthStencilAttachment.stencilLoadAction) {
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Load: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::LoadPreserve: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
            case GERenderPassDescriptor::DepthStencilAttachment::LoadAction::Clear: {
                ds_desc.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
                ds_desc.StencilBeginningAccess.Clear.ClearValue =
                    CD3DX12_CLEAR_VALUE(DXGI_FORMAT_UNKNOWN, desc.depthStencilAttachment.clearDepth,
                                        desc.depthStencilAttachment.clearStencil);
                if (!desc.multisampleResolve)
                    ds_desc.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                break;
            }
        }

        commandList->BeginRenderPass(attachmentCount, rt_descs, &ds_desc, D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES);
    }

    if (firstRenderPass) {
        firstRenderPass = false;
    }
};

void GED3D12CommandBuffer::setRenderPipelineState(SharedHandle<GERenderPipelineState> &pipelineState) {
    assert(!inComputePass && "Cannot set Render Pipeline State while in Compute Pass");
    auto *d3d12_pipeline_state = (GED3D12RenderPipelineState *)pipelineState.get();
    commandList->SetPipelineState(d3d12_pipeline_state->pipelineState.Get());
    currentRenderPipeline = d3d12_pipeline_state;
    commandList->SetGraphicsRootSignature(d3d12_pipeline_state->rootSignature.Get());
    currentRootSignature = &d3d12_pipeline_state->rootSignatureDesc;
};

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GEBuffer> &buffer, unsigned int index) {
    assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const at a Vertex Func when not in render pass");
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_buffer->currentState & required_state)) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            d3d12_buffer->buffer.Get(), d3d12_buffer->currentState, required_state);
        commandList->ResourceBarrier(1, &barrier);
        d3d12_buffer->currentState = required_state;
    }

    commandList->SetDescriptorHeaps(1, d3d12_buffer->bufferDescHeap.GetAddressOf());

    const auto rootParam = getRootParameterIndexOfResource(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        // §2.4 constant buffer — root CBV (the root-param lookup already
        // resolved the matching CBV parameter from the shader layout).
        commandList->SetGraphicsRootConstantBufferView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (d3d12_buffer->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        commandList->SetGraphicsRootShaderResourceView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else {
        commandList->SetGraphicsRootUnorderedAccessView(rootParam, d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
};

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GETexture> &texture, unsigned int index,
                                                       const TextureSwizzle & swizzle) {
    assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const at a Vertex Func when not in render pass");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(index, currentRenderPipeline->vertexShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

        d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
        buffer->commandList->Close();
        parentQueue->commandQueue->ExecuteCommandLists(1,
                                                       (ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
    }

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->vertexShader->internal);

    if (d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_texture->currentState & required_state)) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            d3d12_texture->resource.Get(), d3d12_texture->currentState, required_state);
        commandList->ResourceBarrier(1, &barrier);
        d3d12_texture->currentState = required_state;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE cpuDescHandle;

    if (d3d12_texture->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        /// Use Shader Resource View.
        cpuDescHandle = d3d12_texture->srvDescHeap->GetGPUDescriptorHandleForHeapStart();
    } else {
        /// Use Unordered Access View
        cpuDescHandle = d3d12_texture->uavDescHeap->GetGPUDescriptorHandleForHeapStart();
    }

    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, index, currentRenderPipeline->vertexShader->internal);
    ID3D12DescriptorHeap *heapToBind = d3d12_texture->srvDescHeap.Get();
    if((d3d12_texture->currentState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) && !effective.isIdentity()){
        heapToBind = d3d12_texture->getOrCreateSwizzledSrvHeap(parentQueue->engine->d3d12_device.Get(), effective);
        cpuDescHandle = heapToBind->GetGPUDescriptorHandleForHeapStart();
    }
    currentResourceDescHeap = heapToBind;
    rebindDescriptorHeaps();
    unsigned idx = getRootParameterIndexOfResource(index, currentRenderPipeline->vertexShader->internal);
    commandList->SetGraphicsRootDescriptorTable(idx, cpuDescHandle);
};

void GED3D12CommandBuffer::bindResourceAtVertexShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    assert((!inComputePass && !inBlitPass) && "Cannot bind sampler at a Vertex Func when not in render pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentRenderPipeline->vertexShader->internal);
    assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
    if (!ok) return;
    // Each GED3D12SamplerState owns a shader-visible single-entry SAMPLER
    // heap. Bind it alongside the current CBV/SRV/UAV heap (Option A: one
    // runtime sampler per draw, matching the texture path's heap model).
    currentSamplerDescHeap = d3d12_sampler->descHeap.Get();
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(id, currentRenderPipeline->vertexShader->internal);
    commandList->SetGraphicsRootDescriptorTable(rootParam, d3d12_sampler->descHeap->GetGPUDescriptorHandleForHeapStart());
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GEBuffer> &buffer, unsigned int index) {
    assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const a Fragment Func when not in render pass");
    auto *d3d12_buffer = (GED3D12Buffer *)buffer.get();

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->fragmentShader->internal);

    if (d3d12_buffer->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_buffer->buffer.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_buffer->currentState & required_state)) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            d3d12_buffer->buffer.Get(), d3d12_buffer->currentState, required_state);
        commandList->ResourceBarrier(1, &barrier);
        d3d12_buffer->currentState = required_state;
    }

    commandList->SetDescriptorHeaps(1, d3d12_buffer->bufferDescHeap.GetAddressOf());

    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        commandList->SetGraphicsRootConstantBufferView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (d3d12_buffer->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        commandList->SetGraphicsRootShaderResourceView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else {
        commandList->SetGraphicsRootUnorderedAccessView(
            getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GETexture> &texture, unsigned int index,
                                                         const TextureSwizzle & swizzle) {
    assert((!inComputePass && !inBlitPass) && "Cannot set Resource Const a Fragment Func when not in render pass");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(index, currentRenderPipeline->fragmentShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        auto buffer = std::dynamic_pointer_cast<GED3D12CommandBuffer>(parentQueue->getAvailableBuffer());

        d3d12_texture->updateAndValidateStatus(buffer->commandList.Get());
        buffer->commandList->Close();
        parentQueue->commandQueue->ExecuteCommandLists(1,
                                                       (ID3D12CommandList *const *)buffer->commandList.GetAddressOf());
    }

    auto required_state = getRequiredResourceStateForResourceID(index, currentRenderPipeline->fragmentShader->internal);

    if (d3d12_texture->currentState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(d3d12_texture->resource.Get());
        commandList->ResourceBarrier(1, &barrier);
    }

    if (!(d3d12_texture->currentState & required_state)) {
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            d3d12_texture->resource.Get(), d3d12_texture->currentState, required_state);
        commandList->ResourceBarrier(1, &barrier);
        d3d12_texture->currentState = required_state;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE cpuDescHandle{};

    if (d3d12_texture->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        /// Use Shader Resource View.
        cpuDescHandle = d3d12_texture->srvDescHeap->GetGPUDescriptorHandleForHeapStart();
    } else {
        /// Use Unordered Access View
        cpuDescHandle = d3d12_texture->uavDescHeap->GetGPUDescriptorHandleForHeapStart();
    }

    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, index, currentRenderPipeline->fragmentShader->internal);
    ID3D12DescriptorHeap *heapToBind = d3d12_texture->srvDescHeap.Get();
    if((d3d12_texture->currentState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) && !effective.isIdentity()){
        heapToBind = d3d12_texture->getOrCreateSwizzledSrvHeap(parentQueue->engine->d3d12_device.Get(), effective);
        cpuDescHandle = heapToBind->GetGPUDescriptorHandleForHeapStart();
    }
    currentResourceDescHeap = heapToBind;
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(index, currentRenderPipeline->fragmentShader->internal);
    DEBUG_STREAM("Root Param With Texture:" << rootParam);
    commandList->SetGraphicsRootDescriptorTable(rootParam, cpuDescHandle);
};

void GED3D12CommandBuffer::bindResourceAtFragmentShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    assert((!inComputePass && !inBlitPass) && "Cannot bind sampler at a Fragment Func when not in render pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentRenderPipeline->fragmentShader->internal);
    assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
    if (!ok) return;
    currentSamplerDescHeap = d3d12_sampler->descHeap.Get();
    rebindDescriptorHeaps();
    unsigned rootParam = getRootParameterIndexOfResource(id, currentRenderPipeline->fragmentShader->internal);
    commandList->SetGraphicsRootDescriptorTable(rootParam, d3d12_sampler->descHeap->GetGPUDescriptorHandleForHeapStart());
};

void GED3D12CommandBuffer::setStencilRef(unsigned int ref) {
    commandList->OMSetStencilRef(ref);
}

void GED3D12CommandBuffer::setViewports(std::vector<GEViewport> viewports) {
    std::vector<D3D12_VIEWPORT> d3d12_viewports;
    auto viewports_it = viewports.begin();
    while (viewports_it != viewports.end()) {
        GEViewport &viewport = *viewports_it;
        GRect rect{};
        if (currentTarget.native != nullptr) {
            auto res_desc = currentTarget.native->renderTargets[currentTarget.native->frameIndex]->GetDesc();
            rect.pos.x = 0;
            rect.pos.y = 0;
            rect.w = (float)res_desc.Width;
            rect.h = (float)res_desc.Height;
        } else {
            rect.pos.x = 0;
            rect.pos.y = 0;
            auto res_desc = currentTarget.texture->texture->resource->GetDesc();
            rect.w = (float)res_desc.Width;
            rect.h = (float)res_desc.Height;
        }

        CD3DX12_VIEWPORT v(viewport.x, rect.h - (viewport.y + viewport.height), viewport.width, viewport.height,
                           viewport.nearDepth, viewport.farDepth);
        d3d12_viewports.push_back(v);
        ++viewports_it;
    };
    commandList->RSSetViewports(d3d12_viewports.size(), d3d12_viewports.data());
};

void GED3D12CommandBuffer::setScissorRects(std::vector<GEScissorRect> scissorRects) {
    std::vector<D3D12_RECT> d3d12_rects;
    auto rects_it = scissorRects.begin();
    while (rects_it != scissorRects.end()) {
        GEScissorRect &_rect = *rects_it;

        GRect rect{};
        if (currentTarget.native != nullptr) {

            auto res_desc = currentTarget.native->renderTargets[currentTarget.native->frameIndex]->GetDesc();
            rect.pos.x = 0;
            rect.pos.y = 0;
            rect.w = (float)res_desc.Width;
            rect.h = (float)res_desc.Height;
        } else {
            rect.pos.x = 0;
            rect.pos.y = 0;
            auto res_desc = currentTarget.texture->texture->resource->GetDesc();
            rect.w = (float)res_desc.Width;
            rect.h = (float)res_desc.Height;
        }

        float top_coord = rect.h - (_rect.height + _rect.y);

        CD3DX12_RECT r((LONG)_rect.x, (LONG)top_coord, LONG(_rect.width + _rect.x), LONG(top_coord + _rect.height));
        d3d12_rects.push_back(r);
        ++rects_it;
    };
    commandList->RSSetScissorRects(d3d12_rects.size(), d3d12_rects.data());
};

void GED3D12CommandBuffer::setVertexBuffer(SharedHandle<GEBuffer> &buffer) {
    auto *b = (GED3D12Buffer *)buffer.get();
    D3D12_VERTEX_BUFFER_VIEW view;
    view.BufferLocation = b->buffer->GetGPUVirtualAddress();
    view.SizeInBytes = UINT(b->size());
    view.StrideInBytes = 1;
    commandList->IASetVertexBuffers(0, 1, &view);
};

static D3D12_PRIMITIVE_TOPOLOGY d3d12TopologyForPolygonType(GECommandBuffer::PolygonType polygonType) {
    switch (polygonType) {
        case GECommandBuffer::Triangle:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case GECommandBuffer::TriangleStrip:
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case GECommandBuffer::Line:
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case GECommandBuffer::LineStrip:
            return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case GECommandBuffer::Point:
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void GED3D12CommandBuffer::drawPolygons(RenderPassDrawPolygonType polygonType, unsigned int vertexCount,
                                        size_t startIdx) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawInstanced(vertexCount, 1, startIdx, 0);
};

void GED3D12CommandBuffer::setIndexBuffer(SharedHandle<GEBuffer> & buffer, RenderPassIndexType indexType) {
    auto *b = (GED3D12Buffer *)buffer.get();
    D3D12_INDEX_BUFFER_VIEW view;
    view.BufferLocation = b->buffer->GetGPUVirtualAddress();
    view.SizeInBytes = UINT(b->size());
    view.Format = (indexType == RenderPassIndexType::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    commandList->IASetIndexBuffer(&view);
}

void GED3D12CommandBuffer::drawIndexedPolygons(RenderPassDrawPolygonType polygonType,
                                               unsigned indexCount, size_t startIndex,
                                               int baseVertex) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawIndexedInstanced(indexCount, 1, UINT(startIndex), baseVertex, 0);
}

void GED3D12CommandBuffer::drawPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                 unsigned vertexCount, size_t startIdx,
                                                 unsigned instanceCount, unsigned firstInstance) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawInstanced(vertexCount, instanceCount, UINT(startIdx), firstInstance);
}

void GED3D12CommandBuffer::drawIndexedPolygonsInstanced(RenderPassDrawPolygonType polygonType,
                                                        unsigned indexCount, size_t startIndex,
                                                        int baseVertex, unsigned instanceCount,
                                                        unsigned firstInstance) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->DrawIndexedInstanced(indexCount, instanceCount, UINT(startIndex), baseVertex, firstInstance);
}

static void transitionBufferForIndirectArgs(ID3D12GraphicsCommandList6 *commandList, GED3D12Buffer *argBuf) {
    if (argBuf->currentState == D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) {
        return;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(argBuf->buffer.Get(),
                                                        argBuf->currentState,
                                                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    commandList->ResourceBarrier(1, &barrier);
    argBuf->currentState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
}

void GED3D12CommandBuffer::drawPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                SharedHandle<GEBuffer> & argumentBuffer,
                                                size_t argumentBufferOffset) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDrawIndirectSignature();
    if (sig == nullptr) {
        DEBUG_STREAM("drawPolygonsIndirect: draw indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::drawIndexedPolygonsIndirect(RenderPassDrawPolygonType polygonType,
                                                       SharedHandle<GEBuffer> & argumentBuffer,
                                                       size_t argumentBufferOffset) {
    assert(!inComputePass && "Cannot Draw Polygons while in Compute Pass");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDrawIndexedIndirectSignature();
    if (sig == nullptr) {
        DEBUG_STREAM("drawIndexedPolygonsIndirect: indexed draw indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->IASetPrimitiveTopology(d3d12TopologyForPolygonType(polygonType));
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::finishRenderPass() {
    assert(inRenderPass && "");
    commandList->EndRenderPass();
    commandList->ClearState(nullptr);

    if (multisampleResolvePass) {
        ID3D12Resource *target;
        if (currentTarget.native != nullptr) {
            target = currentTarget.native->renderTargets[currentTarget.native->frameIndex];
        } else {
            target = currentTarget.texture->texture->resource.Get();
        }
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(target, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ResourceBarrier(1, &barrier);
    }

    currentTarget.texture = nullptr;
    currentTarget.native = nullptr;
    currentRenderPipeline = nullptr;
    currentRootSignature = nullptr;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
};

void GED3D12CommandBuffer::startComputePass(const GEComputePassDescriptor &desc) {
    inComputePass = true;
};

void GED3D12CommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> &pipelineState) {
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
    d3d12_buffer->buffer->GetHeapProperties(&heap_props, &heapFlags);
    commandList->SetDescriptorHeaps(1, d3d12_buffer->bufferDescHeap.GetAddressOf());
    if (d3d12_buffer->role == BufferDescriptor::Uniform) {
        // §2.4 constant buffer — root CBV.
        commandList->SetComputeRootConstantBufferView(
            getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (heap_props.Type == D3D12_HEAP_TYPE_UPLOAD) {
        commandList->SetComputeRootShaderResourceView(
            getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    } else if (heap_props.Type == D3D12_HEAP_TYPE_READBACK) {
        auto resource_barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_buffer->buffer.Get(),
                                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &resource_barrier);
        commandList->SetComputeRootUnorderedAccessView(
            getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
            d3d12_buffer->buffer->GetGPUVirtualAddress());
    }
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> &texture, unsigned int id,
                                                        const TextureSwizzle & swizzle) {
    assert(inComputePass && "");
    auto *d3d12_texture = (GED3D12Texture *)texture.get();

    checkTextureBindAgainstShader(id, currentComputePipeline->computeShader->internal, *d3d12_texture);

    if (d3d12_texture->needsValidation()) {
        d3d12_texture->updateAndValidateStatus(commandList.Get());
    }

    D3D12_HEAP_PROPERTIES heap_props;
    D3D12_HEAP_FLAGS heapFlags;
    d3d12_texture->resource->GetHeapProperties(&heap_props, &heapFlags);
    TextureSwizzle effective = resolveEffectiveSwizzle(swizzle, id, currentComputePipeline->computeShader->internal);
    ID3D12DescriptorHeap *heapToBind = d3d12_texture->srvDescHeap.Get();
    if(!effective.isIdentity()){
        heapToBind = d3d12_texture->getOrCreateSwizzledSrvHeap(parentQueue->engine->d3d12_device.Get(), effective);
    }
    currentResourceDescHeap = heapToBind;
    rebindDescriptorHeaps();
    if (heap_props.Type == D3D12_HEAP_TYPE_READBACK) {
        auto resource_barrier = CD3DX12_RESOURCE_BARRIER::Transition(d3d12_texture->resource.Get(),
                                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &resource_barrier);
    }
    commandList->SetComputeRootDescriptorTable(
        getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
        heapToBind->GetGPUDescriptorHandleForHeapStart());
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GESamplerState> &sampler, unsigned int id) {
    assert(inComputePass && "Cannot bind sampler at a Compute Func when not in compute pass");
    auto *d3d12_sampler = (GED3D12SamplerState *)sampler.get();
    bool ok = checkSamplerBindAgainstShader(id, currentComputePipeline->computeShader->internal);
    assert(ok && "Extension 8: sampler bound to a static or non-sampler slot");
    if (!ok) return;
    currentSamplerDescHeap = d3d12_sampler->descHeap.Get();
    rebindDescriptorHeaps();
    commandList->SetComputeRootDescriptorTable(
        getRootParameterIndexOfResource(id, currentComputePipeline->computeShader->internal),
        d3d12_sampler->descHeap->GetGPUDescriptorHandleForHeapStart());
}

void GED3D12CommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> &accelStruct,
                                                       unsigned int idx) {
    auto d3d12_buffer = std::dynamic_pointer_cast<GED3D12AccelerationStruct>(accelStruct);
    commandList->SetComputeRootShaderResourceView(
        getRootParameterIndexOfResource(idx, currentComputePipeline->computeShader->internal),
        d3d12_buffer->structBuffer->buffer->GetGPUVirtualAddress());
}

void GED3D12CommandBuffer::dispatchRays(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "Must be in a compute pass to dispatch rays");
    D3D12_DISPATCH_RAYS_DESC rays{};
    rays.Width = x;
    rays.Height = y;
    rays.Depth = z;

    rays.RayGenerationShaderRecord.StartAddress = 0;
    rays.RayGenerationShaderRecord.SizeInBytes = 0;
    rays.MissShaderTable.StartAddress = 0;
    rays.MissShaderTable.SizeInBytes = 0;
    rays.MissShaderTable.StrideInBytes = 0;
    rays.HitGroupTable.StartAddress = 0;
    rays.HitGroupTable.SizeInBytes = 0;
    rays.HitGroupTable.StrideInBytes = 0;
    rays.CallableShaderTable.StartAddress = 0;
    rays.CallableShaderTable.SizeInBytes = 0;
    rays.CallableShaderTable.StrideInBytes = 0;

    commandList->DispatchRays(&rays);
}

void GED3D12CommandBuffer::dispatchThreadgroups(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "");
    commandList->Dispatch(x, y, z);
}

void GED3D12CommandBuffer::dispatchThreads(unsigned int x, unsigned int y, unsigned int z) {
    assert(inComputePass && "");
    auto &tg = currentComputePipeline->computeShader->internal.threadgroupDesc;
    unsigned gx = (x + tg.x - 1) / tg.x;
    unsigned gy = (y + tg.y - 1) / tg.y;
    unsigned gz = (z + tg.z - 1) / tg.z;
    commandList->Dispatch(gx, gy, gz);
}

void GED3D12CommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer,
                                                        size_t argumentBufferOffset) {
    assert(inComputePass && "Must be in a compute pass to dispatch threadgroups");
    auto *argBuf = (GED3D12Buffer *)argumentBuffer.get();
    auto *sig = parentQueue->engine->getDispatchIndirectSignature();
    if (sig == nullptr) {
        DEBUG_STREAM("dispatchThreadgroupsIndirect: dispatch indirect signature unavailable");
        return;
    }
    transitionBufferForIndirectArgs(commandList.Get(), argBuf);
    commandList->ExecuteIndirect(sig, 1, argBuf->buffer.Get(),
                                 UINT64(argumentBufferOffset),
                                 nullptr, 0);
}

void GED3D12CommandBuffer::finishComputePass() {
    commandList->ClearState(nullptr);
    inComputePass = false;
    currentComputePipeline = nullptr;
    currentRootSignature = nullptr;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
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

GED3D12CommandBuffer::~GED3D12CommandBuffer() {
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Destroy, ResourceTracking::Backend::D3D12,
                                               "CommandBuffer", traceResourceId, commandList.Get());
}

void GED3D12CommandQueue::notifyCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                              SharedHandle<GEFence> &waitFence) {
    multiQueueSync = true;
    auto fence = (GED3D12Fence *)waitFence.get();
    if (fence->lastSignaledValue > 0) {
        commandQueue->Wait(fence->fence.Get(), fence->lastSignaledValue);
    }
    multiQueueSync = false;
};

void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer) {
    HRESULT hr;
    auto d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
    d3d12_buffer->closed = true;
    submittedTraceCommandBufferIds.push_back(d3d12_buffer->traceResourceId);
    ResourceTracking::Event submitEvent{};
    submitEvent.backend = ResourceTracking::Backend::D3D12;
    submitEvent.eventType = ResourceTracking::EventType::Submit;
    submitEvent.resourceType = "CommandBuffer";
    submitEvent.resourceId = d3d12_buffer->traceResourceId;
    submitEvent.queueId = traceResourceId;
    submitEvent.commandBufferId = d3d12_buffer->traceResourceId;
    submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(d3d12_buffer->commandList.Get());
    ResourceTracking::Tracker::instance().emit(submitEvent);
    retainedCommandBuffers.push_back(commandBuffer);
    commandLists.push_back(d3d12_buffer->commandList.Get());
};

void GED3D12CommandQueue::submitCommandBuffer(SharedHandle<GECommandBuffer> &commandBuffer,
                                              SharedHandle<GEFence> &signalFence) {
    multiQueueSync = true;
    auto *d3d12_buffer = (GED3D12CommandBuffer *)commandBuffer.get();
    auto *fence = dynamic_cast<GED3D12Fence *>(signalFence.get());
    d3d12_buffer->closed = true;
    submittedTraceCommandBufferIds.push_back(d3d12_buffer->traceResourceId);
    ResourceTracking::Event submitEvent{};
    submitEvent.backend = ResourceTracking::Backend::D3D12;
    submitEvent.eventType = ResourceTracking::EventType::Submit;
    submitEvent.resourceType = "CommandBuffer";
    submitEvent.resourceId = d3d12_buffer->traceResourceId;
    submitEvent.queueId = traceResourceId;
    submitEvent.commandBufferId = d3d12_buffer->traceResourceId;
    submitEvent.nativeHandle = reinterpret_cast<std::uint64_t>(d3d12_buffer->commandList.Get());
    ResourceTracking::Tracker::instance().emit(submitEvent);

    // Preserve submission order: queued command lists must execute before the
    // fence signal command list so cross-queue waits observe rendered data.
    if (!commandLists.empty()) {
        for (auto &cl : commandLists) {
            if (cl != nullptr) {
                cl->Close();
            }
        }
        commandQueue->ExecuteCommandLists(commandLists.size(), (ID3D12CommandList *const *)commandLists.data());
        commandLists.clear();
        const auto pendingGate = gateForNextSubmit();
        ++nextSubmitValue;
        commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
        flushPendingRetentionUnder(pendingGate);
    }

    d3d12_buffer->commandList->Close();
    commandQueue->ExecuteCommandLists(1, (ID3D12CommandList *const *)d3d12_buffer->commandList.GetAddressOf());
    {
        const auto bufGate = gateForNextSubmit();
        ++nextSubmitValue;
        commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
        engine->retentionQueue.retainShared(SharedHandle<GECommandBuffer>(commandBuffer), {bufGate});
    }
    const auto signalValue = fence->nextSignalValue++;
    fence->lastSignaledValue = signalValue;
    commandQueue->Signal(fence->fence.Get(), signalValue);
    multiQueueSync = false;
    engine->retentionQueue.drainCompleted();
}

void GED3D12CommandQueue::signalFence(SharedHandle<GEFence> &fence) {
    auto d3d12Fence = static_cast<GED3D12Fence *>(fence.get());
    const auto signalValue = d3d12Fence->nextSignalValue++;
    d3d12Fence->lastSignaledValue = signalValue;
    commandQueue->Signal(d3d12Fence->fence.Get(), signalValue);
}

void GED3D12CommandQueue::waitForFence(SharedHandle<GEFence> &fence, std::uint64_t value) {
    if (value == 0)
        return;
    auto d3d12Fence = static_cast<GED3D12Fence *>(fence.get());
    const UINT64 completed = d3d12Fence->fence->GetCompletedValue();
    if (completed >= value)
        return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev == nullptr)
        return;
    HRESULT hr = d3d12Fence->fence->SetEventOnCompletion(value, ev);
    if (SUCCEEDED(hr))
        WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

void GED3D12CommandBuffer::reset() {
    closed = false;
    firstRenderPass = true;
    currentResourceDescHeap = nullptr;
    currentSamplerDescHeap = nullptr;
    commandList->Reset(commandAllocator.Get(), nullptr);
    commandAllocator->Reset();
};

void GED3D12CommandQueue::commitToGPU() {
    if (!multiQueueSync) {
        for (auto &cl : commandLists) {
            if (cl != nullptr) {
                cl->Close();
            }
        }
        if (!commandLists.empty()) {
            commandQueue->ExecuteCommandLists(commandLists.size(), (ID3D12CommandList *const *)commandLists.data());
            const auto gate = gateForNextSubmit();
            ++nextSubmitValue;
            commandQueue->Signal(retentionFence.Get(), nextSubmitValue);
            commandLists.clear();
            flushPendingRetentionUnder(gate);
        } else {
            // Nothing to execute; any items pushed during this batch (e.g.
            // descriptor heaps from a generateMipmaps that was followed by no
            // submit) have no GPU work to gate against. Clearing them here
            // matches the historical behavior — the GPU was never going to
            // touch them anyway.
            retainedCommandBuffers.clear();
            retainedDescriptorHeaps.clear();
        }
    }
    engine->retentionQueue.drainCompleted();
};

void GED3D12CommandQueue::commitToGPUAndWait() {
    commitToGPU();
    fence->SetEventOnCompletion(1, cpuEvent);
    commandQueue->Signal(fence.Get(), 1);
    WaitForSingleObject(cpuEvent, INFINITE);
    commandQueue->Signal(fence.Get(), 0);
    for (const auto traceId : submittedTraceCommandBufferIds) {
        ResourceTracking::Event completeEvent{};
        completeEvent.backend = ResourceTracking::Backend::D3D12;
        completeEvent.eventType = ResourceTracking::EventType::Complete;
        completeEvent.resourceType = "CommandBuffer";
        completeEvent.resourceId = traceId;
        completeEvent.queueId = traceResourceId;
        completeEvent.commandBufferId = traceId;
        completeEvent.nativeHandle = reinterpret_cast<std::uint64_t>(commandQueue.Get());
        ResourceTracking::Tracker::instance().emit(completeEvent);
    }
    submittedTraceCommandBufferIds.clear();
    // Wait above guarantees every prior submit's retention-fence value has
    // been reached, so any retention entries gated on this queue are
    // releasable now.
    engine->retentionQueue.drainCompleted();
}

SharedHandle<GECommandBuffer> GED3D12CommandQueue::getAvailableBuffer() {
    HRESULT hr;
    ID3D12GraphicsCommandList6 *commandList;

    ID3D12CommandAllocator *commandAllocator;

    hr = engine->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));

    if (FAILED(hr)) {
        exit(1);
    };

    hr = engine->d3d12_device->CreateCommandList(engine->d3d12_device->GetNodeCount(), D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 commandAllocator, NULL, IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) {
        MessageBoxA(GetForegroundWindow(), "Failed to Create Command List", "NOTE", MB_OK);
        std::cout << "ERROR:" << std::hex << hr << std::endl;
        exit(1);
    };
    return SharedHandle<GECommandBuffer>(new GED3D12CommandBuffer(commandList, commandAllocator, this));
};

ID3D12GraphicsCommandList6 *GED3D12CommandQueue::getLastCommandList() {
    // Returns nullptr when no command list is pending submission.
    // Callers (notably `GED3D12NativeRenderTarget::present`) treat null
    // as "queue already committed" and allocate a fresh barrier CB.
    // Pre queue-decoupling this never returned null because the render
    // target itself owned a queue and kept lists alive; post-decoupling
    // a caller can commitToGPU before present and leave the queue empty.
    if(commandLists.empty()) return nullptr;
    return commandLists.back();
}

GED3D12CommandQueue::~GED3D12CommandQueue() {
    ResourceTracking::Tracker::instance().emit(ResourceTracking::EventType::Destroy, ResourceTracking::Backend::D3D12,
                                               "CommandQueue", traceResourceId, commandQueue.Get());
    CloseHandle(cpuEvent);
}

_NAMESPACE_END_
