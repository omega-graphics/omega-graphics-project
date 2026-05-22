#include "omegaVA/MediaPlaybackSession.h"

#include "omegaVA/Microsoft.h"

#include <omega-common/unicode.h>
#include <omega-common/img.h>
#include <thread>

#include <mfidl.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <mfapi.h>
#include <mmdeviceapi.h>
#include <mfcaptureengine.h>

#include <memory>

#include <d3d11_4.h>
#include <d3d12video.h>

#pragma comment(lib,"mfplat.lib")
#pragma comment(lib,"mfuuid.lib")
#pragma comment(lib,"shlwapi.lib")

namespace OmegaVA {

    void cpp_to_wstring(OmegaCommon::StrRef str,LPWSTR * output){
        auto wstr = OmegaCommon::UniString::fromUTF8(str.data());
        LPWSTR temp = new WCHAR[wstr.length()];
        memcpy(temp,wstr.getBuffer(),wstr.length());
        *output = (LPWSTR)temp;
    }

    IMFByteStream *createMFByteStreamMediaInputStream(MediaInputStream &inputStream){
        IMFByteStream *byteStream;
        LPWSTR str = nullptr;
        cpp_to_wstring(inputStream.file,&str);
        MFCreateFile(MF_ACCESSMODE_READ,MF_OPENMODE_FAIL_IF_NOT_EXIST,MF_FILEFLAGS_NOBUFFERING,str,&byteStream);
        return byteStream;
    }

    IMFByteStream *createMFByteStreamMediaOutputStream(MediaOutputStream &outputStream){
        IMFByteStream *byteStream;
        LPWSTR str = nullptr;
        cpp_to_wstring(outputStream.file,&str);
        MFCreateFile(MF_ACCESSMODE_WRITE,MF_OPENMODE_DELETE_IF_EXIST,MF_FILEFLAGS_NOBUFFERING,str,&byteStream);
        return byteStream;
    }


    /// AudioVideoProcessor Interface
    /// @{
    IMFActivate *AudioVideoProcessor_GetSoftwareEncodeTransform(SharedHandle<AudioVideoProcessor> & processor);

    IMFActivate *AudioVideoProcessor_GetSoftwareDecodeTransform(SharedHandle<AudioVideoProcessor> & processor);

    void AudioVideoProcessor_EncodeFrame(SharedHandle<AudioVideoProcessor> & processor,IMFSample *sample,IMFSample **output);

    void AudioVideoProcessor_DecodeFrame(SharedHandle<AudioVideoProcessor> & processor,IMFSample *sample,IMFSample **output);

    void AudioVideoProcessor_setDecodeCodec(SharedHandle<AudioVideoProcessor> & processor,const GUID &from,const GUID &to);

    void AudioVideoProcessor_setEncodeCodec(SharedHandle<AudioVideoProcessor> & processor,const GUID &from,const GUID &to);
    ///@}


    class WMFAudioVideoProcessorClient : public IMFTransform {
        unsigned refCount = 1;
        OmegaCommon::Map<DWORD,IMFStreamDescriptor *> outputStreams,inputStreams;
        OmegaCommon::Vector<IMFMediaType *> outputStreamMediaTypes,inputStreamMediaTypes;
        bool useHEVC;
    public:
        explicit WMFAudioVideoProcessorClient(bool encodeOrDecode,bool useHEVC):useHEVC(useHEVC){
            IMFMediaType *type;
            MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
            if(useHEVC) {
                type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
            }
            else {
                type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264);
            }
            if(encodeOrDecode){
                outputStreamMediaTypes.push_back(type);
            }
            else {
                inputStreamMediaTypes.push_back(type);
            }

            /// Encode Input / Decode Output

            type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
            if(encodeOrDecode){
                inputStreamMediaTypes.push_back(type);
            }
            else {
                outputStreamMediaTypes.push_back(type);
            }
        }
        ULONG STDMETHODCALLTYPE AddRef() override{
            return InterlockedIncrement(&refCount);
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(const IID &iid,void **ppData) override{

        }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG newCount = InterlockedDecrement(&refCount);
            if(newCount == 0){
                delete this;
            }
            return newCount;
        }
        HRESULT AddInputStreams(DWORD cStreams, DWORD *adwStreamIDs) override {
            for(;cStreams > 0;cStreams--){
                inputStreams.insert(std::make_pair(*adwStreamIDs,nullptr));
                ++adwStreamIDs;
            }
            return S_OK;
        }
        HRESULT GetInputAvailableType(DWORD dwInputStreamID, DWORD dwTypeIndex, IMFMediaType **ppType) override {
            *ppType = inputStreamMediaTypes[dwTypeIndex];
            return S_OK;
        }
        HRESULT GetOutputAvailableType(DWORD dwOutputStreamID, DWORD dwTypeIndex, IMFMediaType **ppType) override {
            *ppType = outputStreamMediaTypes[dwTypeIndex];
            return S_OK;
        }
        HRESULT SetOutputType(DWORD dwOutputStreamID, IMFMediaType *pType, DWORD dwFlags) override {
            auto & stream = outputStreams[dwOutputStreamID];
            if(stream != nullptr){
                stream->Release();
            }
            MFCreateStreamDescriptor(dwOutputStreamID,1,&pType,&stream);
            return S_OK;
        }
        HRESULT SetInputType(DWORD dwInputStreamID, IMFMediaType *pType, DWORD dwFlags) override {
            auto & stream = inputStreams[dwInputStreamID];
            if(stream != nullptr){
                stream->Release();
            }
            MFCreateStreamDescriptor(dwInputStreamID,1,&pType,&stream);
            return S_OK;
        }
        HRESULT ProcessInput(DWORD dwInputStreamID, IMFSample *pSample, DWORD dwFlags) override {

        }
        HRESULT ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount, MFT_OUTPUT_DATA_BUFFER *pOutputSamples, DWORD *pdwStatus) override {
            return S_OK;
        }
        ~WMFAudioVideoProcessorClient() = default;
    };





    class WMFVideoSampleGrabber : public IMFSampleGrabberSinkCallback {
        VideoFrameSink *visualSink;
        unsigned refCount = 1;
        FrameSize frameSize;
        IMFPresentationClock *clock;
    public:
        explicit WMFVideoSampleGrabber(VideoFrameSink * sink,const FrameSize & frameSize):
        visualSink(sink),
        frameSize(frameSize){

        }
        ULONG STDMETHODCALLTYPE AddRef() override{
            return InterlockedIncrement(&refCount);
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(const IID &iid,void **ppData) override{
            if(iid == IID_IUnknown || iid == IID_IMFSampleGrabberSinkCallback){
                *ppData = this;
                return S_OK;
            }
            else {
                *ppData = nullptr;
                return E_NOINTERFACE;
            }
        }
        ULONG STDMETHODCALLTYPE Release() override {
            ULONG newCount = InterlockedDecrement(&refCount);
            if(newCount == 0){
                delete this;
            }
            return newCount;
        }
        HRESULT STDMETHODCALLTYPE  OnClockStart(
                /* [in] */ MFTIME hnsSystemTime,
                /* [in] */ LONGLONG llClockStartOffset) override {
            return S_OK;
        };
        HRESULT STDMETHODCALLTYPE OnClockStop(
                /* [in] */ MFTIME hnsSystemTime) override {
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnClockPause(
                /* [in] */ MFTIME hnsSystemTime) override {
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnClockRestart(
                /* [in] */ MFTIME hnsSystemTime) override {
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnClockSetRate(
                /* [in] */ MFTIME hnsSystemTime,
                /* [in] */ float flRate) override {
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnProcessSample(const GUID &guidMajorMediaType, DWORD dwSampleFlags, LONGLONG llSampleTime, LONGLONG llSampleDuration, const BYTE *pSampleBuffer, DWORD dwSampleSize) override {
            auto frame = std::make_shared<VideoFrame>();

            MFTIME time;
            clock->GetTime(&time);

            frame->decodeFinishTime = TimePoint(std::chrono::nanoseconds(time * 100));
            frame->presentTime = TimePoint(std::chrono::nanoseconds(llSampleTime * 100));
            // Borrow the Media Foundation sample buffer as a non-owning
            // view — the WMF callback owns the lifetime, and PixelStorage
            // with a null deleter (`view`) will not try to free it on
            // BitmapImage destruction.
            frame->videoFrame.pixels = OmegaCommon::Img::PixelStorage::view(
                const_cast<OmegaCommon::Img::Byte *>(
                    reinterpret_cast<const OmegaCommon::Img::Byte *>(pSampleBuffer)),
                static_cast<std::size_t>(dwSampleSize));
            frame->videoFrame.header.height = frameSize.height;
            frame->videoFrame.header.width = frameSize.width;
            frame->videoFrame.header.color_format = OmegaCommon::Img::ColorFormat::RGBA;
            frame->videoFrame.header.bitDepth = 8;
            frame->videoFrame.header.channels = 4;
            frame->videoFrame.header.alpha_format = OmegaCommon::Img::AlphaFormat::Straight;
            visualSink->pushFrame(frame);

            if(time > llSampleTime){
                visualSink->flush();
            }
            else {
                auto millis = std::chrono::milliseconds((llSampleTime - time)/10);
                std::this_thread::sleep_for(millis);
                visualSink->presentCurrentFrame();
            }
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnSetPresentationClock(IMFPresentationClock *pPresentationClock) override {
            pPresentationClock->AddRef();
            clock = pPresentationClock;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnShutdown() override {
            return S_OK;
        }
    };


    class PlaybackDispatchQueue {
        DWORD workQueue;
    public:
        explicit PlaybackDispatchQueue():workQueue(0){
            MFAllocateWorkQueue(&workQueue);
        }
        ~PlaybackDispatchQueue(){
            MFUnlockWorkQueue(workQueue);
        }
    };

    SharedHandle<PlaybackDispatchQueue> createPlaybackDispatchQueue(){
        return std::make_shared<PlaybackDispatchQueue>();
    }

    struct AudioPlaybackDevice {
        IMFMediaSink *sink;
        IMMDevice *device;
    };

   struct WMFAudioCaptureDevice : public AudioCaptureDevice {
        IMFActivate *activate;
   public:
       explicit WMFAudioCaptureDevice(IMFActivate *activate):activate(activate){

       }
       UniqueHandle<AudioCaptureSession> createCaptureSession() override;
    };

    OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> enumerateAudioCaptureDevices(){
        OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> devs;
        IMFAttributes *attrs;
        MFCreateAttributes(&attrs,0);
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID);
        IMFActivate ** activates;
        UINT32 len;
        MFEnumDeviceSources(attrs,&activates,&len);
        for(;len > 0;len--){
            IMFActivate *activate = *activates;
            devs.emplace_back(new WMFAudioCaptureDevice(activate));
            ++activates;
        }
        Core::SafeRelease(&attrs);
        return devs;
    }

    struct WMFVideoDevice : public VideoDevice {
        IMFActivate *activate;
    public:
        explicit WMFVideoDevice(IMFActivate *activate):activate(activate){

        }
        UniqueHandle<VideoCaptureSession> createCaptureSession(SharedHandle<AudioCaptureDevice> &audioCaptureDevice) override;
    };

    OmegaCommon::Vector<SharedHandle<VideoDevice>> enumerateVideoDevices(){
        OmegaCommon::Vector<SharedHandle<VideoDevice>> devs;
        IMFAttributes *attrs;
        MFCreateAttributes(&attrs,0);
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate ** activates;
        UINT32 len;
        MFEnumDeviceSources(attrs,&activates,&len);
        for(;len > 0;len--){
            IMFActivate *activate = *activates;
            devs.emplace_back(new WMFVideoDevice(activate));
            ++activates;
        }
        Core::SafeRelease(&attrs);
        return devs;
    }

    Microsoft::WRL::ComPtr<IMFCaptureEngineClassFactory> captureEngineFactory = nullptr;

    class WMFAudioCaptureSession : public AudioCaptureSession {
        IMFCaptureEngine *engine;
    public:
        explicit WMFAudioCaptureSession(WMFAudioCaptureDevice * device){
            if(captureEngineFactory.Get() == nullptr){
                CoCreateInstance(CLSID_MFCaptureEngineClassFactory,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&captureEngineFactory));
            }
            captureEngineFactory->CreateInstance(CLSID_MFCaptureEngine,IID_PPV_ARGS(&engine));
            IMFAttributes *attrs;
            MFCreateAttributes(&attrs,0);
            attrs->SetUINT32(MF_CAPTURE_ENGINE_USE_AUDIO_DEVICE_ONLY,TRUE);
            engine->Initialize(nullptr,attrs,device->activate,nullptr);
        }
        void setAudioOutputStream(MediaOutputStream &outputStream) override {
            IMFCaptureRecordSink *sink;
            engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_RECORD, reinterpret_cast<IMFCaptureSink **>(&sink));
            LPWSTR wstr;
            cpp_to_wstring(outputStream.file,&wstr);
            sink->SetOutputFileName(wstr);
        }
        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> &device) override {
            IMFCapturePreviewSink *sink;
            engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW, reinterpret_cast<IMFCaptureSink **>(&sink));
            sink->SetCustomSink(device->sink);
        }
        void startPreview() override {
            engine->StartPreview();
        }
        void startRecord() override {
            engine->StartRecord();
        }
        void stopRecord() override {
            engine->StopRecord(TRUE,TRUE);
        }
        void stopPreview() override {
            engine->StopPreview();
        }
    };

    UniqueHandle<AudioCaptureSession> WMFAudioCaptureDevice::createCaptureSession() {
        return (UniqueHandle<AudioCaptureSession>)new WMFAudioCaptureSession(this);
    }

    class WMFVideoCaptureSampleSink : public IMFCaptureEngineOnSampleCallback {
        unsigned refCount = 1;
        VideoFrameSink *sink;
        UINT32 frameWidth = 0;
        UINT32 frameHeight = 0;
    public:
        explicit WMFVideoCaptureSampleSink(VideoFrameSink *sink, UINT32 width, UINT32 height)
            : sink(sink), frameWidth(width), frameHeight(height) {}
        ULONG AddRef() override{
            return InterlockedIncrement(&refCount);
        }
        HRESULT QueryInterface(const GUID &iid,void **pData) override{
            if (iid == IID_IUnknown || iid == __uuidof(IMFCaptureEngineOnSampleCallback)) {
                *pData = static_cast<IMFCaptureEngineOnSampleCallback *>(this);
                AddRef();
                return S_OK;
            }
            *pData = nullptr;
            return E_NOINTERFACE;
        }
        HRESULT OnSample(IMFSample *pSample) override {
            if (!pSample || !sink) return S_OK;

            IMFMediaBuffer *buffer = nullptr;
            if (FAILED(pSample->ConvertToContiguousBuffer(&buffer)) || !buffer) return S_OK;

            BYTE *bytes = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&bytes, nullptr, &length))) {
                buffer->Release();
                return S_OK;
            }

            LONGLONG sampleTime = 0;
            pSample->GetSampleTime(&sampleTime);

            auto frame = std::make_shared<VideoFrame>();
            frame->presentTime       = TimePoint(std::chrono::nanoseconds(sampleTime * 100));
            frame->decodeFinishTime  = std::chrono::high_resolution_clock::now();
            // Borrow the locked MF buffer as a non-owning view. We unlock + release the
            // buffer below, so the borrow is only valid until pushFrame returns; the sink
            // must copy or consume synchronously. This matches WMFVideoSampleGrabber.
            frame->videoFrame.pixels = OmegaCommon::Img::PixelStorage::view(
                reinterpret_cast<OmegaCommon::Img::Byte *>(bytes),
                static_cast<std::size_t>(length));
            frame->videoFrame.header.width        = frameWidth;
            frame->videoFrame.header.height       = frameHeight;
            // NOTE: actual subtype may be NV12/YUY2/RGB32 depending on the capture
            // device — see Phase 4.3 in Media-API-Completion-Plan.md for the open
            // color-format negotiation gap.
            frame->videoFrame.header.color_format = OmegaCommon::Img::ColorFormat::RGBA;
            frame->videoFrame.header.bitDepth     = 8;
            frame->videoFrame.header.channels     = 4;
            frame->videoFrame.header.alpha_format = OmegaCommon::Img::AlphaFormat::Straight;
            sink->pushFrame(frame);

            buffer->Unlock();
            buffer->Release();
            return S_OK;
        }
        ULONG Release() override{
            ULONG newCount = InterlockedDecrement(&refCount);
            if(newCount == 0){
                delete this;
            }
            return newCount;
        }
    };

    class WMFVideoCaptureSession : public VideoCaptureSession {
        IMFCaptureEngine *engine;
        Core::UniqueComPtr<WMFVideoCaptureSampleSink> videoPreviewSink;
    public:
        explicit WMFVideoCaptureSession(WMFVideoDevice * videoDevice,WMFAudioCaptureDevice * audioDevice){
            if(captureEngineFactory.Get() == nullptr){
                CoCreateInstance(CLSID_MFCaptureEngineClassFactory,NULL,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&captureEngineFactory));
            }
            captureEngineFactory->CreateInstance(CLSID_MFCaptureEngine,IID_PPV_ARGS(&engine));
            IMFAttributes *attrs;
            MFCreateAttributes(&attrs,0);
            engine->Initialize(nullptr,attrs,audioDevice->activate,videoDevice->activate);
        }
        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> &device) override {
            IMFCapturePreviewSink *sink;
            engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW,reinterpret_cast<IMFCaptureSink **>(&sink));
            sink->SetCustomSink(device->sink);
        }
        void setVideoFrameSinkForPreview(VideoFrameSink &frameSink) override {
            // Discover the source's stream 0 native media type so we can configure
            // the preview sink stream and tell the sample sink the frame dimensions.
            IMFCaptureSource *source = nullptr;
            engine->GetSource(&source);
            IMFMediaType *srcType = nullptr;
            UINT32 width = 0, height = 0;
            if (source) {
                if (SUCCEEDED(source->GetCurrentDeviceMediaType(0, &srcType)) && srcType) {
                    MFGetAttributeSize(srcType, MF_MT_FRAME_SIZE, &width, &height);
                }
            }

            auto ptr = new WMFVideoCaptureSampleSink(&frameSink, width, height);
            if(videoPreviewSink.get() != nullptr){
                videoPreviewSink->Release();
            }
            videoPreviewSink = ptr;

            IMFCapturePreviewSink *sink = nullptr;
            engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW,reinterpret_cast<IMFCaptureSink **>(&sink));
            // AddStream is what actually wires the sink to the source stream; without
            // it SetSampleCallback's callback never fires.
            DWORD sinkStreamIdx = 0;
            if (sink && srcType) {
                sink->AddStream(0, srcType, nullptr, &sinkStreamIdx);
            }
            if (sink) {
                sink->SetSampleCallback(sinkStreamIdx, videoPreviewSink.get());
            }
            if (srcType) srcType->Release();
            if (source)  source->Release();
        }
        void setVideoOutputStream(MediaOutputStream &outputStream) override {
            IMFCaptureRecordSink *sink;
            engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_RECORD,reinterpret_cast<IMFCaptureSink **>(&sink));
            LPWSTR wstr;
            cpp_to_wstring(outputStream.file,&wstr);
            sink->SetOutputFileName(wstr);
        }
        void startPreview() override {
            engine->StartPreview();
        }
        void startRecord() override {
            engine->StartRecord();
        }
        void stopRecord() override {
            engine->StopRecord(TRUE,TRUE);
        }
        void stopPreview() override {
            engine->StopPreview();
        }
    };

    UniqueHandle<VideoCaptureSession>
    WMFVideoDevice::createCaptureSession(SharedHandle<AudioCaptureDevice> &audioCaptureDevice) {
        return (UniqueHandle<VideoCaptureSession>)new WMFVideoCaptureSession(this,(WMFAudioCaptureDevice *)audioCaptureDevice.get());
    }




    OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> enumerateAudioPlaybackDevices(){
        IMFAttributes *attrs;
        MFCreateAttributes(&attrs,0);
        const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
        const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
        IMMDeviceEnumerator *pEnumerator;
        HRESULT hr = CoCreateInstance(
                CLSID_MMDeviceEnumerator, NULL,
                CLSCTX_INPROC_SERVER, IID_IMMDeviceEnumerator,
                (void**)&pEnumerator);

        OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> res;

        IMMDeviceCollection *collection;
        pEnumerator->EnumAudioEndpoints(EDataFlow::eRender,DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED,&collection);
        UINT count;
        collection->GetCount(&count);
        for(unsigned i = 0;i < count;i++){
            IMMDevice *device;
            IMFMediaSink *renderer;
            collection->Item(i,&device);
            LPWSTR id;
            device->GetId(&id);
            attrs->SetString(MF_AUDIO_RENDERER_ATTRIBUTE_ENDPOINT_ID,id);
            MFCreateAudioRenderer(attrs,&renderer);
            auto pb_dev = std::make_shared<AudioPlaybackDevice>();
            pb_dev->device = device;
            pb_dev->sink = renderer;
            res.push_back(pb_dev);
        }
        Core::SafeRelease(&attrs);
        return res;
    }

    class WMFAudioPlaybackSession : public AudioPlaybackSession {
        IMFMediaSession *session;
        IMFTopology *topology;
        IMFTopologyNode *sourceNode,*outputNode;
        IMFSourceResolver *sourceResolver;
        PROPVARIANT p;
        IMFMediaSource *mediaSource;
    public:
        explicit WMFAudioPlaybackSession(SharedHandle<AudioVideoProcessor> & processor) : AudioPlaybackSession(processor){
            PropVariantInit(&p);
            IMFAttributes *attrs;
            MFCreateAttributes(&attrs,0);
            MFCreateMediaSession(attrs,&session);
            MFCreateTopology(&topology);
            MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE,&outputNode);
            topology->AddNode(outputNode);
            MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE,&sourceNode);
            topology->AddNode(sourceNode);
            MFCreateSourceResolver(&sourceResolver);
            sourceNode->ConnectOutput(0,outputNode,0);
            session->SetTopology(0,topology);
            Core::SafeRelease(&attrs);
        }
        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> &device) override {
            outputNode->SetObject(device->sink);
        }
        void setAudioSource(MediaInputStream &inputStream) override {
            MF_OBJECT_TYPE t;
            IUnknown *obj;
            sourceResolver->CreateObjectFromByteStream(createMFByteStreamMediaInputStream(inputStream),NULL,MF_RESOLUTION_MEDIASOURCE,NULL,&t,&obj);
            obj->QueryInterface(IID_PPV_ARGS(&mediaSource));
            sourceNode->SetUnknown(MF_TOPONODE_SOURCE,mediaSource);
            IMFPresentationDescriptor *desc;
            mediaSource->CreatePresentationDescriptor(&desc);
            sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR,desc);
            IMFStreamDescriptor *streamDesc;
            BOOL selected;
            desc->GetStreamDescriptorByIndex(0,&selected,&streamDesc);
            sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR,streamDesc);
        }
        void start() override {
            session->Start(nullptr,&p);
        }
        void pause() override {
            session->Stop();
        }
        void reset() override {
            PropVariantClear(&p);
            session->Shutdown();
        }
        ~WMFAudioPlaybackSession() override {
            session->Shutdown();
            Core::SafeRelease(&session);
        }
    };

    SharedHandle<AudioPlaybackSession> AudioPlaybackSession::Create(SharedHandle<AudioVideoProcessor> & processor,SharedHandle<PlaybackDispatchQueue> & queue) {
        return (SharedHandle<AudioPlaybackSession>)new WMFAudioPlaybackSession(processor);
    }

    class WMFVideoPlaybackSession : public VideoPlaybackSession {
        IMFMediaSession *session = nullptr;
        IMFTopology *topology = nullptr;
        IMFTopologyNode *sourceNode = nullptr;
        IMFTopologyNode *videoOutputNode = nullptr;
        IMFTopologyNode *audioSourceNode = nullptr;
        IMFTopologyNode *audioOutputNode = nullptr;
        SharedHandle<WMFVideoSampleGrabber> videoSink;
        IMFActivate *videoSampleGrabber = nullptr;
        IMFSourceResolver *sourceResolver = nullptr;
        PROPVARIANT p {};
        IMFMediaSource *mediaSource = nullptr;
        IMFPresentationDescriptor *presentDesc = nullptr;
        FrameSize frameSize {};
        bool topologyDirty = true;
    public:
        explicit WMFVideoPlaybackSession(SharedHandle<AudioVideoProcessor> & processor) : VideoPlaybackSession(processor){
            IMFAttributes *attrs;
            MFCreateAttributes(&attrs,0);
            MFCreateMediaSession(attrs,&session);
            MFCreateTopology(&topology);
            MFCreateSourceResolver(&sourceResolver);
            PropVariantInit(&p);

            // Always-present nodes: video source + video output, connected source[0]->video[0].
            MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode);
            MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE,       &videoOutputNode);
            topology->AddNode(sourceNode);
            topology->AddNode(videoOutputNode);
            sourceNode->ConnectOutput(0, videoOutputNode, 0);

            Core::SafeRelease(&attrs);
        }
        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> &device) override {
            // Lazily build the audio output branch the first time an audio device is
            // supplied. Requires mediaSource to have been set already (setVideoSource).
            if (!audioOutputNode) {
                MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &audioSourceNode);
                MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE,       &audioOutputNode);
                topology->AddNode(audioSourceNode);
                topology->AddNode(audioOutputNode);
                audioSourceNode->ConnectOutput(0, audioOutputNode, 0);

                // Wire stream descriptor for the audio stream if we already know the
                // presentation descriptor. We pick the first non-video stream as audio.
                if (mediaSource && presentDesc) {
                    audioSourceNode->SetUnknown(MF_TOPONODE_SOURCE, mediaSource);
                    audioSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, presentDesc);
                    DWORD streamCount = 0;
                    presentDesc->GetStreamDescriptorCount(&streamCount);
                    for (DWORD i = 0; i < streamCount; ++i) {
                        IMFStreamDescriptor *sd = nullptr;
                        BOOL selected = FALSE;
                        if (FAILED(presentDesc->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;
                        IMFMediaTypeHandler *h = nullptr;
                        GUID major = {};
                        if (SUCCEEDED(sd->GetMediaTypeHandler(&h)) && h) {
                            h->GetMajorType(&major);
                            h->Release();
                        }
                        if (major == MFMediaType_Audio) {
                            audioSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, sd);
                            sd->Release();
                            break;
                        }
                        sd->Release();
                    }
                }
            }
            audioOutputNode->SetObject(device->sink);
            topologyDirty = true;
        }
        void setVideoFrameSink(VideoFrameSink &sink) override {
            videoSink = std::make_shared<WMFVideoSampleGrabber>(&sink,frameSize);
            IMFMediaType *type;
            MFCreateMediaType(&type);
            type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_v410);
            MFCreateSampleGrabberSinkActivate(type,videoSink.get(),&videoSampleGrabber);
            videoOutputNode->SetObject(videoSampleGrabber);
            type->Release();
            topologyDirty = true;
        }
        void setVideoSource(MediaInputStream &inputStream) override {
            MF_OBJECT_TYPE t;
            IUnknown *obj;
            sourceResolver->CreateObjectFromByteStream(createMFByteStreamMediaInputStream(inputStream),NULL,0,NULL,&t,&obj);
            obj->QueryInterface(IID_PPV_ARGS(&mediaSource));
            sourceNode->SetUnknown(MF_TOPONODE_SOURCE,mediaSource);
            if (presentDesc) { presentDesc->Release(); presentDesc = nullptr; }
            mediaSource->CreatePresentationDescriptor(&presentDesc);
            sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR,presentDesc);

            // Pick the first video stream as the source-node feed; fall back to stream 0.
            IMFStreamDescriptor *streamDesc = nullptr;
            BOOL selected = FALSE;
            DWORD streamCount = 0;
            presentDesc->GetStreamDescriptorCount(&streamCount);
            for (DWORD i = 0; i < streamCount; ++i) {
                IMFStreamDescriptor *sd = nullptr;
                if (FAILED(presentDesc->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;
                IMFMediaTypeHandler *h = nullptr;
                GUID major = {};
                if (SUCCEEDED(sd->GetMediaTypeHandler(&h)) && h) {
                    h->GetMajorType(&major);
                    h->Release();
                }
                if (major == MFMediaType_Video) { streamDesc = sd; break; }
                sd->Release();
            }
            if (!streamDesc) {
                presentDesc->GetStreamDescriptorByIndex(0,&selected,&streamDesc);
            }
            if (streamDesc) {
                sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR,streamDesc);
                IMFMediaTypeHandler *handler = nullptr;
                if (SUCCEEDED(streamDesc->GetMediaTypeHandler(&handler)) && handler) {
                    IMFMediaType *type = nullptr;
                    if (SUCCEEDED(handler->GetCurrentMediaType(&type)) && type) {
                        UINT32 w = 0, h = 0;
                        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
                        frameSize = FrameSize {w, h};
                        type->Release();
                    }
                    handler->Release();
                }
                streamDesc->Release();
            }
            topologyDirty = true;
        }
        void start() override {
            if (topologyDirty) {
                session->SetTopology(0, topology);
                topologyDirty = false;
            }
            session->Start(nullptr, &p);
        }
        void pause() override {
            session->Pause();
        }
        void reset() override {
            PropVariantClear(&p);
            PropVariantInit(&p);
            session->Stop();
            session->ClearTopologies();
            topologyDirty = true;
        }
        ~WMFVideoPlaybackSession() override {
            if (session) {
                session->Shutdown();
                Core::SafeRelease(&session);
            }
            if (presentDesc)     presentDesc->Release();
            if (mediaSource)     mediaSource->Release();
            if (videoSampleGrabber) videoSampleGrabber->Release();
            if (sourceResolver)  sourceResolver->Release();
            if (topology)        topology->Release();
        }
    };

    SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(SharedHandle<AudioVideoProcessor> & processor, SharedHandle<PlaybackDispatchQueue> & dispatchQueue) {
        (void)dispatchQueue;
        return std::shared_ptr<VideoPlaybackSession>(new WMFVideoPlaybackSession(processor));
    }
}
