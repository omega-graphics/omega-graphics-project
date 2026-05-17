#include "WMFAudioVideoProcessor.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mf.lib")
#pragma comment(lib,"mfuuid.lib")

namespace OmegaWTK::Media {

namespace {

// Activate the first MFT returned by MFTEnumEx for the requested codec pair,
// then set its input/output media types so it's ready for ProcessInput/ProcessOutput.
IMFTransform *activateMFTForCodecPair(GUID category,
                                      const GUID &fromSubtype,
                                      const GUID &toSubtype,
                                      bool preferHardware,
                                      IMFActivate **outActivate) {
    MFT_REGISTER_TYPE_INFO inputType  {MFMediaType_Video, fromSubtype};
    MFT_REGISTER_TYPE_INFO outputType {MFMediaType_Video, toSubtype};

    UINT32 flags = preferHardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
                                  : (MFT_ENUM_FLAG_SYNCMFT  | MFT_ENUM_FLAG_SORTANDFILTER);

    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category, flags, &inputType, &outputType, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return nullptr;
    }

    IMFTransform *transform = nullptr;
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform));
    if (FAILED(hr)) {
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        return nullptr;
    }
    if (outActivate) {
        activates[0]->AddRef();
        *outActivate = activates[0];
    }
    // Release the rest; we keep transform via its own reference.
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);

    // Configure media types. The codec-pair API only carries subtypes — width/height/
    // framerate negotiation happens elsewhere (the source provides them).
    IMFMediaType *inType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&inType))) {
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE,    fromSubtype);
        transform->SetInputType(0, inType, 0);
        inType->Release();
    }
    IMFMediaType *outType = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&outType))) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE,    toSubtype);
        transform->SetOutputType(0, outType, 0);
        outType->Release();
    }
    return transform;
}

// Drive one ProcessInput/ProcessOutput cycle. Returns nullptr in *output if the MFT
// needs more input before it can produce a sample.
void driveMFT(IMFTransform *transform, IMFSample *sample, IMFSample **output) {
    *output = nullptr;
    if (!transform || !sample) return;

    HRESULT hr = transform->ProcessInput(0, sample, 0);
    if (FAILED(hr)) return;

    MFT_OUTPUT_DATA_BUFFER outBuf {};
    outBuf.dwStreamID = 0;
    outBuf.pSample    = nullptr;     // Let the MFT allocate the output sample.
    outBuf.dwStatus   = 0;
    outBuf.pEvents    = nullptr;

    DWORD status = 0;
    hr = transform->ProcessOutput(0, 1, &outBuf, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;        // Caller should feed another input.
    if (FAILED(hr)) return;

    *output = outBuf.pSample;
    if (outBuf.pEvents) outBuf.pEvents->Release();
}

} // namespace

AudioVideoProcessor::AudioVideoProcessor(bool useHardwareAccel, void * /*gte_device*/)
    : useHardwareAccel(useHardwareAccel) {
    // Codec selection (and the actual MFT activation) is deferred to setEncodeCodec /
    // setDecodeCodec, since the codec pair is unknown until then.
}

AudioVideoProcessor::~AudioVideoProcessor() {
    if (encodeMFT) encodeMFT->Release();
    if (decodeMFT) decodeMFT->Release();
    if (cpuEncodeTransform) cpuEncodeTransform->Release();
    if (cpuDecodeTransform) cpuDecodeTransform->Release();
}

void AudioVideoProcessor::setEncodeCodec(const GUID &from, const GUID &to) {
    if (encodeMFT) { encodeMFT->Release(); encodeMFT = nullptr; }
    if (cpuEncodeTransform) { cpuEncodeTransform->Release(); cpuEncodeTransform = nullptr; }
    encodeMFT = activateMFTForCodecPair(MFT_CATEGORY_VIDEO_ENCODER, from, to,
                                        useHardwareAccel, &cpuEncodeTransform);
}

void AudioVideoProcessor::setDecodeCodec(const GUID &from, const GUID &to) {
    if (decodeMFT) { decodeMFT->Release(); decodeMFT = nullptr; }
    if (cpuDecodeTransform) { cpuDecodeTransform->Release(); cpuDecodeTransform = nullptr; }
    decodeMFT = activateMFTForCodecPair(MFT_CATEGORY_VIDEO_DECODER, from, to,
                                        useHardwareAccel, &cpuDecodeTransform);
}

void AudioVideoProcessor::encodeFrame(IMFSample *sample, IMFSample **output) {
    driveMFT(encodeMFT, sample, output);
}

void AudioVideoProcessor::decodeFrame(IMFSample *sample, IMFSample **output) {
    driveMFT(decodeMFT, sample, output);
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

} // namespace OmegaWTK::Media
