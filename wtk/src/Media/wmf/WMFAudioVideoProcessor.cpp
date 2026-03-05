#include "WMFAudioVideoProcessor.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"

#include <d3d11_4.h>
#include <d3d11on12.h>
#include <d3d12video.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <ShlObj.h>

#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mf.lib")
#pragma comment(lib,"mfuuid.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"d3d11.lib")

namespace OmegaWTK::Media {

AudioVideoProcessor::AudioVideoProcessor(bool useHardwareAccel, void *gte_device)
    : useHardwareAccel(useHardwareAccel) {
    if (useHardwareAccel) {
        auto *device = (ID3D12Device *)gte_device;
        D3D12_COMMAND_QUEUE_DESC commandQueueDesc {};
        commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        commandQueueDesc.NodeMask = device->GetNodeCount();
        commandQueueDesc.Priority = 1;
        commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
        device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&decodeCommandQueue));
        commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
        device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&encodeCommandQueue));
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        IUnknown *const queues[] = {decodeCommandQueue, encodeCommandQueue};
        ID3D11DeviceContext *context;
        ID3D11Device *dev;
        D3D11On12CreateDevice((IUnknown *)gte_device, 0, levels, 1, queues, 2, device->GetNodeCount(), &dev, &context, nullptr);
        context->QueryInterface(IID_PPV_ARGS(&d3d11_context));
        ID3D11VideoDevice *video_dev;
        dev->QueryInterface(IID_PPV_ARGS(&video_dev));
        D3D11_VIDEO_DECODER_DESC desc {};
        desc.Guid = D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        desc.OutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

void AudioVideoProcessor::setEncodeCodec(const GUID &from, const GUID &to) {
    if (useHardwareAccel) {
        HEVCorH264 = (to == MFVideoFormat_HEVC);
    } else {
        MFT_REGISTER_TYPE_INFO startFormat{MFMediaType_Video, from}, endFormat{MFMediaType_Video, to};
        IMFActivate **activates;
        UINT32 count;
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, 0, &startFormat, &endFormat, &activates, &count);
    }
}

void AudioVideoProcessor::setDecodeCodec(const GUID &from, const GUID &to) {
    if (useHardwareAccel) {
        HEVCorH264 = (from == MFVideoFormat_HEVC);
    } else {
        MFT_REGISTER_TYPE_INFO startFormat{MFMediaType_Video, from}, endFormat{MFMediaType_Video, to};
        IMFActivate **activates;
        UINT32 count;
        MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, 0, &startFormat, &endFormat, &activates, &count);
    }
}

void AudioVideoProcessor::encodeFrame(IMFSample *sample, IMFSample **output) {
    (void)sample;
    (void)output;
}

void AudioVideoProcessor::decodeFrame(IMFSample *sample, IMFSample **output) {
    (void)sample;
    (void)output;
}

UniqueHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel, void *gte_device) {
        return std::make_unique<AudioVideoProcessor>(useHardwareAccel,gte_device);
    }

    IMFActivate *AudioVideoProcessor_GetSoftwareEncodeTransform(UniqueHandle<AudioVideoProcessor> & processor){
        return processor->cpuEncodeTransform;
    }

    IMFActivate *AudioVideoProcessor_GetSoftwareDecodeTransform(UniqueHandle<AudioVideoProcessor> & processor){
        return processor->cpuDecodeTransform;
    }

    void AudioVideoProcessor_EncodeFrame(UniqueHandle<AudioVideoProcessor> & processor,IMFSample *sample,IMFSample **output){
        processor->encodeFrame(sample,output);
    }

    void AudioVideoProcessor_DecodeFrame(UniqueHandle<AudioVideoProcessor> & processor,IMFSample *sample,IMFSample **output){
        processor->decodeFrame(sample,output);
    }

    void AudioVideoProcessor_setDecodeCodec(UniqueHandle<AudioVideoProcessor> & processor,const GUID &from,const GUID &to){
        processor->setDecodeCodec(from,to);
    }

    void AudioVideoProcessor_setEncodeCodec(UniqueHandle<AudioVideoProcessor> & processor,const GUID &from,const GUID &to){
        processor->setEncodeCodec(from,to);
    }


};