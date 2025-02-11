#include "omegaWTK/Media/Audio.h"
#include "omegaWTK/Media/Video.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"

#import <AVFoundation/AVFoundation.h>
#import <AVFAudio/AVFAudio.h>

#include <mutex>
#include <thread>


@interface OmegaWTKMediaAVFAudioCaptureSampleBufferDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate> {
    void *context;
    BOOL preview;
    BOOL videoDelegate;
}
-(instancetype)initWithCppContext:(void *)context videoCaptureDelegate:(BOOL)videoDelegate preview:(BOOL)preview;
@end

@interface OmegaWTKMediaAVFVideoCaptureSampleBufferDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>  {
    void *context;
    BOOL preview;
}
-(instancetype)initWithCppContext:(void *)context preview:(BOOL)preview;
@end

@interface OmegaWTKMediaAVFCaptureFileRecordingDelegate : NSObject <AVCaptureFileOutputRecordingDelegate>  {
    void *context;
}
-(instancetype)initWithCppContext:(void *)context;
@end




namespace OmegaWTK::Media {

    void CMTimeToTimePoint(CMTime & time,TimePoint & tp){
        Float64 secs = CMTimeGetSeconds(time);
        auto millisecs = (int64_t)(secs * 1000.f);
        tp = TimePoint(std::chrono::milliseconds(millisecs));
    }

    NSURL *createURLFromMediaInputStream(MediaInputStream & inputStream){
        NSURL *res;
        if(inputStream.bufferOrFile){
            res = [[NSURL alloc] initWithDataRepresentation:[NSData dataWithBytesNoCopy:inputStream.buffer.data length:inputStream.buffer.length] relativeToURL:nil];
        }
        else {
            res = [NSURL fileURLWithFileSystemRepresentation:inputStream.file.data() isDirectory:NO relativeToURL:nil];
        }
        return res;
    }

    NSURL *createURLFromMediaOutputStream(MediaOutputStream & outputStream){
        NSURL *res;
        if(outputStream.bufferOrFile){
            res = [[NSURL alloc] initWithDataRepresentation:[NSData dataWithBytesNoCopy:outputStream.buffer.data length:outputStream.buffer.length] relativeToURL:nil];
        }
        else {
            res = [NSURL fileURLWithFileSystemRepresentation:outputStream.file.data() isDirectory:NO relativeToURL:nil];
        }
        return res;
    }


    typedef UniqueHandle<AudioVideoProcessor> & AudioVideoProcessorRef;

    void AudioVideoProcessor_SetEncodeMode(AudioVideoProcessorRef processor,CMVideoCodecType codec);
    void AudioVideoProcessor_SetDecodeMode(AudioVideoProcessorRef processor,CMVideoFormatDescriptionRef decodeFormat);
    void AudioVideoProcessor_Encode(AudioVideoProcessorRef processor,CMSampleBufferRef input,CMSampleBufferRef *output);
    void AudioVideoProcessor_Decode(AudioVideoProcessorRef processor,CMSampleBufferRef input,CMSampleBufferRef *output);

    class PlaybackDispatchQueue {
    public:
        struct Client {
            bool audioOrVideo;
            bool skipPlease = true;
            AVSampleBufferRequest *sampleBufferRequest;
            AVSampleBufferGenerator *generator;
            AVSampleBufferAudioRenderer *audioRenderer;
            AVSampleCursor *cursor;
            bool useProcessor;
            AudioVideoProcessor *processor;
            VideoFrameSink *videoSink;
        };
    private:
        std::mutex mutex;
        OmegaCommon::Vector<Client> clients;
        bool finish = false;
        std::thread t;
    public:
        size_t addClient(const Client &client){
            std::lock_guard<std::mutex> lk(mutex);
            clients.push_back(client);
            return clients.size() - 1;
        }
        void startPlaybackForClient(size_t idx){
            std::lock_guard<std::mutex> lk(mutex);
            clients[idx].skipPlease = false;
        }
        void stopPlaybackForClient(size_t idx){
            std::lock_guard<std::mutex> lk(mutex);
            clients[idx].skipPlease = true;
        }
        void removeClient(size_t idx){
            clients.erase(clients.cbegin() + idx);
        }
        explicit PlaybackDispatchQueue():mutex(),t([&]{

            while(!finish){
                {
                    std::unique_lock<std::mutex> lk(mutex);
                    auto clients_size = clients.empty();
                    if(!clients_size){
                        for(auto & cl : clients){

                            if(cl.skipPlease){
                                continue;
                            }

                            CMSampleBufferRef sampleBuffer = [cl.generator createSampleBufferForRequest:cl.sampleBufferRequest];
                            if(cl.audioOrVideo){
                                /// Render Audio
                                [cl.audioRenderer enqueueSampleBuffer:sampleBuffer];
                            }
                            else {
                                /// Render Video
                                if(cl.useProcessor){
                                    
                                }

                                auto frame = std::make_shared<VideoFrame>();
                                CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
                                size_t offsetOut,length;
                                ImgByte *data;
                                CMBlockBufferGetDataPointer(blockBuffer,0,&offsetOut,&length,(char **)&data);
                                frame->videoFrame.data = data;
                                CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
                                auto size = CVImageBufferGetDisplaySize(imageBuffer);
                                frame->videoFrame.header.width = (unsigned)size.width;
                                frame->videoFrame.header.height = (unsigned)size.height;
                                cl.videoSink->pushFrame(frame);
                                CMTime currentTime = CMClockGetTime(CMClockGetHostTimeClock()),presentationTimeStamp = CMSampleBufferGetOutputPresentationTimeStamp(sampleBuffer);
                                if(CMTIME_COMPARE_INLINE(currentTime,>,presentationTimeStamp)){
                                    cl.videoSink->flush();
                                }
                                else {
                                    auto millis = std::chrono::duration<double,std::milli>((double)CMTimeGetSeconds(CMTimeSubtract(presentationTimeStamp,currentTime)) * 1000.f);
                                    std::this_thread::sleep_for(millis);
                                    cl.videoSink->presentCurrentFrame();
                                }
                            }
                            [cl.cursor stepInDecodeOrderByCount:1];
                        }
                    }
                    lk.unlock();
                }
            }
        }){

        }
        ~PlaybackDispatchQueue(){
            finish = true;
            t.join();
        }
    };

    SharedHandle<PlaybackDispatchQueue> createPlaybackDispatchQueue() {
        return (SharedHandle<PlaybackDispatchQueue>)new PlaybackDispatchQueue();
    };

    typedef SharedHandle<PlaybackDispatchQueue> & PlaybackDispatchQueueRef;


    struct AVFAudioCaptureDevice : public AudioCaptureDevice {
        __strong AVCaptureDevice *device;
        explicit AVFAudioCaptureDevice(AVCaptureDevice *dev):device(dev){

        };
        UniqueHandle<AudioCaptureSession> createCaptureSession() override {

        }
        ~AVFAudioCaptureDevice() = default;
    };



    OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> enumerateAudioCaptureDevices(){

        AVCaptureDeviceDiscoverySession *session = [AVCaptureDeviceDiscoverySession
                                                    discoverySessionWithDeviceTypes:
        @[AVCaptureDeviceTypeBuiltInMicrophone,AVCaptureDeviceTypeExternalUnknown] mediaType:AVMediaTypeAudio position:AVCaptureDevicePositionUnspecified];

        OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> devs;
        for(AVCaptureDevice *dev in session.devices){
            devs.emplace_back(new AVFAudioCaptureDevice(dev));
        }

        return devs;
    };

    struct  AudioPlaybackDevice {
        __strong AUAudioUnit *unit;
        explicit AudioPlaybackDevice(AUAudioUnit * dev):unit(dev){

        };
        ~AudioPlaybackDevice() = default;
    };

    typedef AudioPlaybackDevice AVFAudioPlaybackDevice;

     OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> enumerateAudioPlaybackDevices(){
         AudioComponent in = NULL;
         AudioComponentDescription desc {};

         OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> devices;

         while((in = AudioComponentFindNext(in,&desc)) != NULL){
             if(desc.componentType == kAudioUnitType_Generator) {
                 if(desc.componentSubType == kAudioUnitSubType_GenericOutput
                    || desc.componentSubType == kAudioUnitSubType_SystemOutput
                    || desc.componentSubType == kAudioUnitSubType_DefaultOutput) {
                     NSError *error;
                     AUAudioUnit *unit = [[AUAudioUnit alloc] initWithComponentDescription:desc error:&error];
                     devices.emplace_back(new AudioPlaybackDevice(unit));
                 }
             }
         };

         return devices;

     };


     struct AVFVideoDevice : public VideoDevice {
         __strong AVCaptureDevice *device;
         explicit AVFVideoDevice(AVCaptureDevice *dev):device(dev){

         };
         UniqueHandle<VideoCaptureSession> createCaptureSession(SharedHandle<AudioCaptureDevice> &audioCaptureDevice) override {

         }
         ~AVFVideoDevice() = default;
     };

    OmegaCommon::Vector<SharedHandle<VideoDevice>> enumerateVideoDevices(){
        AVCaptureDeviceDiscoverySession *session = [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:
                        @[AVCaptureDeviceTypeBuiltInWideAngleCamera,AVCaptureDeviceTypeExternalUnknown] mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];

        OmegaCommon::Vector<SharedHandle<VideoDevice>> devs;
        for(AVCaptureDevice *dev in session.devices){
            devs.emplace_back(new AVFVideoDevice(dev));
        }

        return devs;
    }

    struct AVFAudioCaptureSession : public AudioCaptureSession {
        AVCaptureSession *captureSession;
        AVCaptureOutput *audioOut;
        AVCaptureAudioDataOutput *previewOut;
        AVCaptureDeviceInput *audioInput;
        AVSampleBufferRenderSynchronizer *renderSynchronizer;
        AVSampleBufferAudioRenderer *audioRenderer;
        OmegaWTKMediaAVFAudioCaptureSampleBufferDelegate *audioCaptureDelegate,*audioPreviewDelegate;

        AVAssetWriter *bufferWriter;
        AVAssetWriterInput *bufferWriterInput;

        bool previewOn = false;
        bool isBufferOutput = false;

        explicit AVFAudioCaptureSession(SharedHandle<AVFAudioCaptureDevice> & device){
            captureSession = [[AVCaptureSession alloc] init];
            NSError *error;
            renderSynchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
            audioRenderer = [[AVSampleBufferAudioRenderer alloc]init];
            [renderSynchronizer addRenderer:audioRenderer];
            audioInput = [AVCaptureDeviceInput deviceInputWithDevice:device->device error:&error];
            previewOut = [[AVCaptureAudioDataOutput alloc] init];
            [previewOut setSampleBufferDelegate:audioPreviewDelegate queue:dispatch_get_main_queue()];
        }
        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> &device) override {
            CFStringRef uid;
            UInt32 uid_size = sizeof(CFStringRef);
            AudioDeviceGetProperty(device->unit.deviceID,0,0,kAudioDevicePropertyDeviceUID,&uid_size,(void *)uid);
            [audioRenderer setAudioOutputDeviceUniqueID:(__bridge id)uid];
        }
        void setAudioOutputStream(MediaOutputStream &outputStream) override {
            if(outputStream.bufferOrFile){
                AVCaptureAudioDataOutput *dataOutput = [[AVCaptureAudioDataOutput alloc] init];
                [dataOutput setSampleBufferDelegate:audioCaptureDelegate queue:dispatch_get_main_queue()];
                [captureSession addOutput:dataOutput];
                isBufferOutput = true;
                audioOut = dataOutput;
                NSError *error;
                bufferWriter = [AVAssetWriter assetWriterWithURL:createURLFromMediaOutputStream(outputStream) fileType:(AVFileTypeAppleM4A) error:&error];
                bufferWriterInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{}];
                [bufferWriter addInput:bufferWriterInput];
            }
            else {
                AVCaptureAudioFileOutput *fileOutput = [[AVCaptureAudioFileOutput alloc] init];
                [captureSession addOutput:fileOutput];
                NSURL *fileUrl = createURLFromMediaOutputStream(outputStream);
                [fileOutput startRecordingToOutputFileURL:fileUrl recordingDelegate:nil];
                isBufferOutput = false;
                audioOut = fileOutput;
            }
        }
        void startPreview() override {
            [captureSession startRunning];
            previewOn = true;
        }
        void startRecord() override {
            if(![captureSession isRunning]) {
                [captureSession startRunning];
            }
        }
        void stopPreview() override {
            if([captureSession isRunning]) {
                [captureSession startRunning];
            }
            previewOn = false;
        }
        void stopRecord() override {
            if([captureSession isRunning]) {
                if(!isBufferOutput) {
                    auto * output = (AVCaptureAudioFileOutput *)audioOut;
                    [output stopRecording];
                }
                [captureSession stopRunning];

                [captureSession removeOutput:audioOut];
                if(previewOn){
                    [captureSession startRunning];
                }
            }
        }
    };


    struct AVFVideoCaptureSession : public VideoCaptureSession {
        AVCaptureSession *captureSession;

        AVCaptureOutput *mainOut;

        AVCaptureVideoDataOutput *videoPreviewOut;

        AVCaptureDeviceInput *inputVideo,*inputAudio;
        AVSampleBufferRenderSynchronizer *renderSynchronizer;

        AVAssetWriter *bufferWriter;
        AVAssetWriterInputGroup *bufferWriterInputGroup;
        AVAssetWriterInput *bufferWriterAudioInput;
        AVAssetWriterInput *bufferWriterVideoInput;

        OmegaWTKMediaAVFVideoCaptureSampleBufferDelegate *videoCaptureDelegate;
        OmegaWTKMediaAVFVideoCaptureSampleBufferDelegate *videoPreviewDelegate;
        OmegaWTKMediaAVFAudioCaptureSampleBufferDelegate *audioCaptureDelegate;

        VideoFrameSink *sink;

        explicit AVFVideoCaptureSession(SharedHandle<AVFVideoDevice> & videoDevice,SharedHandle<AVFAudioCaptureDevice> & audioDevice){
            captureSession = [[AVCaptureSession alloc] init];
            NSError *error;
            inputVideo = [AVCaptureDeviceInput deviceInputWithDevice:videoDevice->device error:&error];
            inputAudio = [AVCaptureDeviceInput deviceInputWithDevice:audioDevice->device error:&error];
            [captureSession addInput:inputVideo];
            [captureSession addInput:inputAudio];
            videoPreviewOut = [[AVCaptureVideoDataOutput alloc] init];
            [videoPreviewOut setSampleBufferDelegate:videoPreviewDelegate queue:dispatch_get_main_queue()];
            OSType e = kCVPixelFormatType_Lossless_32BGRA;
            [videoPreviewOut setVideoSettings:@{(__bridge id)kCVPixelBufferPixelFormatTypeKey:(__bridge id)&e}];
            [captureSession addOutput:videoPreviewOut];
        }
        void setVideoFrameSinkForPreview(VideoFrameSink &frameSink) override {
            sink = &frameSink;
        }
        void setVideoOutputStream(MediaOutputStream &outputStream) override {
            if(outputStream.bufferOrFile){
                AVCaptureVideoDataOutput *dataOutput = [[AVCaptureVideoDataOutput alloc] init];
                [dataOutput setSampleBufferDelegate:videoCaptureDelegate queue:dispatch_get_main_queue()];
                [captureSession addOutput:dataOutput];
                NSError* error;
                bufferWriter = [AVAssetWriter assetWriterWithURL:createURLFromMediaOutputStream(outputStream) fileType:AVFileTypeMPEG4 error:&error];
                bufferWriterVideoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{}];
                bufferWriterAudioInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{}];

                bufferWriterInputGroup = [AVAssetWriterInputGroup assetWriterInputGroupWithInputs:@[bufferWriterAudioInput,bufferWriterVideoInput] defaultInput:nil];
                [bufferWriter addInputGroup:bufferWriterInputGroup];
            }
            else {
                AVCaptureMovieFileOutput *fileOutput = [[AVCaptureMovieFileOutput alloc] init];
                NSURL *url = createURLFromMediaOutputStream(outputStream);
                [captureSession addOutput:fileOutput];
                [fileOutput startRecordingToOutputFileURL:url recordingDelegate:nil];
                mainOut = fileOutput;
            }

        }
        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> &device) override {

        }
        void startPreview() override {

        }
        void startRecord() override {

        }
        void stopRecord() override {

        }
        void stopPreview() override {

        }
    };


    struct AVFAudioPlaybackSession :
            public AudioPlaybackSession{
        AVSampleBufferAudioRenderer *renderer;
        AVSampleBufferRequest *request = nil;
        AVSampleBufferGenerator *generator = nil;
        AVSampleCursor *cursor;
        AVSampleBufferRenderSynchronizer *synchronizer;
        PlaybackDispatchQueueRef  playbackDispatchQueue;
        size_t playbackClientIndex;
    public:
        explicit AVFAudioPlaybackSession(AudioVideoProcessorRef processor,PlaybackDispatchQueueRef dispatchQueue) : AudioPlaybackSession(processor),playbackDispatchQueue(dispatchQueue){
            synchronizer = [[AVSampleBufferRenderSynchronizer alloc] init];
            renderer = [[AVSampleBufferAudioRenderer alloc] init];
            [synchronizer addRenderer:renderer];
        }
        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> &device) override {
            CFStringRef uid;
            UInt32 uid_size = sizeof(CFStringRef);
            AudioDeviceGetProperty(device->unit.deviceID,0,0,kAudioDevicePropertyDeviceUID,&uid_size,(void *)uid);
            [renderer setAudioOutputDeviceUniqueID:(__bridge id)uid];
        }
        void setAudioSource(MediaInputStream &inputStream) override {
            AVAsset *asset = [AVAsset assetWithURL:createURLFromMediaInputStream(inputStream)];
            NSError *error;
            if(generator != nil){
                [generator release];
            }
            generator = [[AVSampleBufferGenerator alloc] initWithAsset:asset timebase:NULL];
            cursor = [[asset tracks].firstObject makeSampleCursorAtFirstSampleInDecodeOrder];
            request = [[AVSampleBufferRequest alloc] initWithStartCursor:cursor];
            request.preferredMinSampleCount = 1;
            request.maxSampleCount = 1;
            request.direction = AVSampleBufferRequestDirectionForward;
            playbackClientIndex = playbackDispatchQueue->addClient({true,true});
        }
        void start() override {
            playbackDispatchQueue->startPlaybackForClient(playbackClientIndex);
        }
        void pause() override {
            playbackDispatchQueue->stopPlaybackForClient(playbackClientIndex);
        }
        void reset() override {
            playbackDispatchQueue->removeClient(playbackClientIndex);
        }
        ~AVFAudioPlaybackSession() override {

        };
    };

    SharedHandle<AudioPlaybackSession> AudioPlaybackSession::Create(AudioVideoProcessorRef processor,PlaybackDispatchQueueRef dispatchQueue) {
        return SharedHandle<AudioPlaybackSession>(new AVFAudioPlaybackSession(processor,dispatchQueue));
    }

    struct AVFVideoPlaybackSession :
            public VideoPlaybackSession{
        AVSampleBufferAudioRenderer *renderer;
        AVSampleBufferRenderSynchronizer *synchronizer;
        AVSampleBufferGenerator *sampleBufferGen = nil;
        AVSampleBufferRequest *videoSampleRequest,*audioSampleRequest;
        AVSampleCursor *videoCursor,*audioCursor;
        PlaybackDispatchQueueRef dispatchQueue;
        size_t playbackClientIndex;

        std::thread *playbackLoop;
        std::mutex m;
        bool playing = false;

        explicit AVFVideoPlaybackSession(AudioVideoProcessorRef processor,PlaybackDispatchQueueRef dispatchQueue) : VideoPlaybackSession(processor),dispatchQueue(dispatchQueue){

        }
        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> &device) override {

        }
        void setVideoFrameSink(VideoFrameSink &sink) override {
            PlaybackDispatchQueue::Client c;
            c.audioOrVideo = false;
            c.useProcessor = true;
            c.cursor = videoCursor;
            c.videoSink = &sink;
            c.generator = sampleBufferGen;
            c.sampleBufferRequest = videoSampleRequest;
            playbackClientIndex = dispatchQueue->addClient({true,true});
        }
        void setVideoSource(MediaInputStream &inputStream) override {
            AVAsset *asset = [AVAsset assetWithURL:[NSURL fileURLWithPath:@""]];

            AVAssetTrack *videoTrack = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
            AVAssetTrack *audioTrack = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;

            videoCursor = [videoTrack makeSampleCursorAtFirstSampleInDecodeOrder];
            audioCursor = [audioTrack makeSampleCursorAtFirstSampleInDecodeOrder];

            videoSampleRequest = [[AVSampleBufferRequest alloc] initWithStartCursor:videoCursor];
            videoSampleRequest.direction = AVSampleBufferRequestDirectionForward;
            videoSampleRequest.maxSampleCount = 1;
            videoSampleRequest.preferredMinSampleCount = 1;

            audioSampleRequest = [[AVSampleBufferRequest alloc] initWithStartCursor:videoCursor];

            sampleBufferGen = [[AVSampleBufferGenerator alloc] initWithAsset:asset timebase:nil];
        }
        void start() override {
            /// THE VIDEO CYCLE
            /// Send image buffer and audio sample buffer in cycle.
            dispatchQueue->startPlaybackForClient(playbackClientIndex);
            
        }
        void pause() override {
            dispatchQueue->stopPlaybackForClient(playbackClientIndex);
        }
        void reset() override {

        }
    };

}

@implementation OmegaWTKMediaAVFAudioCaptureSampleBufferDelegate
- (instancetype)initWithCppContext:(void *)context videoCaptureDelegate:(BOOL)videoDelegate preview:(BOOL)preview {
    if(self = [super init]){
        self->context = context;
        self->videoDelegate = videoDelegate;
        self->preview = preview;
    }
    return self;
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection *)connection {
    if(videoDelegate){

    }
    else {
        auto session = (OmegaWTK::Media::AVFAudioCaptureSession *)context;
        if(preview){
            [session->audioRenderer enqueueSampleBuffer:sampleBuffer];
        }
        else {
            [session->bufferWriterInput appendSampleBuffer:sampleBuffer];
        }
    }
}
@end

using namespace std::chrono_literals;


@implementation OmegaWTKMediaAVFVideoCaptureSampleBufferDelegate
- (instancetype)initWithCppContext:(void *)context preview:(BOOL)preview {
    if(self = [super init]){
        self->context = context;
        self->preview = preview;
    }
    return self;
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection *)connection {
    auto session = (OmegaWTK::Media::AVFVideoCaptureSession *)context;
    if(preview){
        CVImageBufferRef imgBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        auto frame = std::make_shared<OmegaWTK::Media::VideoFrame>();
        OmegaWTK::Media::TimePoint timePoint;
        CMTime t = CMSampleBufferGetOutputDecodeTimeStamp(sampleBuffer);
        OmegaWTK::Media::CMTimeToTimePoint(t,timePoint);
        frame->decodeFinishTime = timePoint;
        t = CMSampleBufferGetOutputPresentationTimeStamp(sampleBuffer);
        OmegaWTK::Media::CMTimeToTimePoint(t,timePoint);
        frame->presentTime = timePoint;
        session->sink->pushFrame(frame);
        auto currentTime = CMClockGetTime(CMClockGetHostTimeClock());
        if(CMTIME_COMPARE_INLINE(currentTime,>,t)) {
            session->sink->flush();
        }
        else {
            auto millis = std::chrono::duration<double,std::milli>(CMTimeGetSeconds(CMTimeSubtract(t,currentTime)) * 1000.f);
            std::this_thread::sleep_for(millis);
            session->sink->presentCurrentFrame();
        }
    }
    else {
        [session->bufferWriterVideoInput appendSampleBuffer:sampleBuffer];
    }
}
@end

@implementation OmegaWTKMediaAVFCaptureFileRecordingDelegate
- (instancetype)initWithCppContext:(void *)context {
    if(self = [super init]){
        self->context = context;
    }
    return self;
}

- (void)captureOutput:(AVCaptureOutput *)output didFinishRecordingToOutputFileAtURL:(NSURL *)outputFileURL fromConnections:(NSArray<AVCaptureConnection *> *)connections error:(NSError *)error {

}
@end