#ifndef OMEGAWTK_MEDIA_WMF_WMFAUDIOVIDEOPROCESSOR_H
#define OMEGAWTK_MEDIA_WMF_WMFAUDIOVIDEOPROCESSOR_H

#include "omegaWTK/Media/AudioVideoProcessorContext.h"

// Forward declarations to avoid pulling in Windows SDK headers here (they redefine INTERFACE
// and break Video.h / MediaPlaybackSession.h). Full definitions are in WMFAudioVideoProcessor.cpp.
struct IMFActivate;
struct IMFVideoSampleAllocator;
struct IMFSample;
struct ID3D12CommandQueue;
struct ID3D11VideoContext;
struct ID3D11VideoDecoder;
typedef struct _GUID GUID;

namespace OmegaWTK::Media {

class AudioVideoProcessor {
public:
    IMFActivate *cpuEncodeTransform = nullptr;
    IMFActivate *cpuDecodeTransform = nullptr;
    IMFVideoSampleAllocator *sampleAllocator = nullptr;
    ID3D12CommandQueue *decodeCommandQueue = nullptr;
    ID3D12CommandQueue *encodeCommandQueue = nullptr;
    ID3D11VideoContext *d3d11_context = nullptr;
    ID3D11VideoDecoder *decoder = nullptr;
    bool useHardwareAccel = false;
    bool HEVCorH264 = false;

    explicit AudioVideoProcessor(bool useHardwareAccel, void *gte_device);
    void setEncodeCodec(const GUID &from, const GUID &to);
    void setDecodeCodec(const GUID &from, const GUID &to);
    void encodeFrame(IMFSample *sample, IMFSample **output);
    void decodeFrame(IMFSample *sample, IMFSample **output);
};

} // namespace OmegaWTK::Media

#endif
