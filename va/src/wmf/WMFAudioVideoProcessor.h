#ifndef OMEGAVA_WMF_WMFAUDIOVIDEOPROCESSOR_H
#define OMEGAVA_WMF_WMFAUDIOVIDEOPROCESSOR_H

#include "omegaVA/AudioVideoProcessorContext.h"

// Forward declarations to avoid pulling in Windows SDK headers here (they redefine INTERFACE
// and break Video.h / MediaPlaybackSession.h). Full definitions are in WMFAudioVideoProcessor.cpp.
struct IMFActivate;
struct IMFTransform;
struct IMFSample;
typedef struct _GUID GUID;

namespace OmegaVA {

class AudioVideoProcessor {
public:
    // Activated MFTs (lazy: stored after setEncodeCodec/setDecodeCodec).
    IMFTransform *encodeMFT = nullptr;
    IMFTransform *decodeMFT = nullptr;
    // Kept for back-compat with the AudioVideoProcessor_GetSoftware*Transform accessors.
    IMFActivate *cpuEncodeTransform = nullptr;
    IMFActivate *cpuDecodeTransform = nullptr;
    bool useHardwareAccel = false;

    explicit AudioVideoProcessor(bool useHardwareAccel, void *gte_device);
    ~AudioVideoProcessor();
    void setEncodeCodec(const GUID &from, const GUID &to);
    void setDecodeCodec(const GUID &from, const GUID &to);
    void encodeFrame(IMFSample *sample, IMFSample **output);
    void decodeFrame(IMFSample *sample, IMFSample **output);
};

} // namespace OmegaVA

#endif
