
#include "AVFAudioVideoProcessor.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"

#import <AVFoundation/AVFoundation.h>
#import <AVFAudio/AVFAudio.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

namespace OmegaWTK::Media {

    // ── VTCompressionSession output callback ─────────────────

    static void avfCompressionOutputCallback(
            void *outputCallbackRefCon,
            void *sourceFrameRefCon,
            OSStatus status,
            VTEncodeInfoFlags infoFlags,
            CMSampleBufferRef sampleBuffer) {

        auto *p = (AudioVideoProcessor *)outputCallbackRefCon;
        if (status == noErr && sampleBuffer) {
            CFRetain(sampleBuffer);
            if (p->lastEncodedBuffer) {
                CFRelease(p->lastEncodedBuffer);
            }
            p->lastEncodedBuffer = sampleBuffer;
        }
    }

    // ── Constructor / Destructor ─────────────────────────────

    AudioVideoProcessor::AudioVideoProcessor(bool useHardwareAccel, void *gteDevice)
        : compressionSession(nullptr)
        , decompressionSession(nullptr)
        , lastEncodedBuffer(nullptr)
        , useHardwareAccel(useHardwareAccel)
        , gteDevice(gteDevice)
        , encodeCodecType(kCMVideoCodecType_H264)
        , encodeWidth(0)
        , encodeHeight(0) {
    }

    AudioVideoProcessor::~AudioVideoProcessor() {
        if (compressionSession) {
            VTCompressionSessionInvalidate((VTCompressionSessionRef)compressionSession);
            CFRelease(compressionSession);
        }
        if (decompressionSession) {
            VTDecompressionSessionInvalidate((VTDecompressionSessionRef)decompressionSession);
            CFRelease(decompressionSession);
        }
        if (lastEncodedBuffer) {
            CFRelease(lastEncodedBuffer);
        }
    }

    // ── Factory ──────────────────────────────────────────────

    UniqueHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel, void *gteDevice) {
        return std::make_unique<AudioVideoProcessor>(useHardwareAccel, gteDevice);
    }

    // ── Bridge functions ─────────────────────────────────────
    // Declared in AVFAudioVideoCapture.mm, defined here.

    typedef UniqueHandle<AudioVideoProcessor> & AudioVideoProcessorRef;

    void AudioVideoProcessor_SetEncodeMode(AudioVideoProcessorRef processor, CMVideoCodecType codec) {
        auto *p = processor.get();

        // Tear down existing compression session
        if (p->compressionSession) {
            VTCompressionSessionInvalidate((VTCompressionSessionRef)p->compressionSession);
            CFRelease(p->compressionSession);
            p->compressionSession = nullptr;
        }

        p->encodeCodecType = (unsigned int)codec;
        // Compression session is created lazily in Encode once frame dimensions are known.
    }

    void AudioVideoProcessor_SetDecodeMode(AudioVideoProcessorRef processor, CMVideoFormatDescriptionRef decodeFormat) {
        auto *p = processor.get();

        // Tear down existing decompression session
        if (p->decompressionSession) {
            VTDecompressionSessionInvalidate((VTDecompressionSessionRef)p->decompressionSession);
            CFRelease(p->decompressionSession);
            p->decompressionSession = nullptr;
        }

        if (!decodeFormat) return;

        // Output pixel buffer attributes -- BGRA for VideoFrame compatibility
        NSDictionary *destAttrs = @{
            (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @(p->useHardwareAccel && p->gteDevice != nullptr)
        };

        NSDictionary *decoderSpec = nil;
        if (p->useHardwareAccel) {
            decoderSpec = @{
                (__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @YES
            };
        }

        VTDecompressionSessionRef session = nullptr;
        OSStatus err = VTDecompressionSessionCreate(
            kCFAllocatorDefault,
            decodeFormat,
            (__bridge CFDictionaryRef)decoderSpec,
            (__bridge CFDictionaryRef)destAttrs,
            nullptr,    // Use block-based decode via DecodeFrameWithOutputHandler
            &session);

        if (err == noErr) {
            p->decompressionSession = session;
        }
    }

    void AudioVideoProcessor_Encode(AudioVideoProcessorRef processor, CMSampleBufferRef input, CMSampleBufferRef *output) {
        auto *p = processor.get();
        *output = nullptr;
        if (!input) return;

        CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(input);
        if (!imageBuffer) return;

        // Lazy-create compression session on first frame (now we know the dimensions)
        if (!p->compressionSession) {
            size_t width  = CVPixelBufferGetWidth(imageBuffer);
            size_t height = CVPixelBufferGetHeight(imageBuffer);
            p->encodeWidth  = (unsigned int)width;
            p->encodeHeight = (unsigned int)height;

            NSDictionary *encoderSpec = nil;
            if (p->useHardwareAccel) {
                encoderSpec = @{
                    (__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES
                };
            }

            VTCompressionSessionRef session = nullptr;
            OSStatus err = VTCompressionSessionCreate(
                kCFAllocatorDefault,
                (int32_t)width,
                (int32_t)height,
                (CMVideoCodecType)p->encodeCodecType,
                (__bridge CFDictionaryRef)encoderSpec,
                nullptr,            // sourceImageBufferAttributes
                kCFAllocatorDefault,
                avfCompressionOutputCallback,
                p,                  // outputCallbackRefCon -- the processor itself
                &session);

            if (err != noErr) return;

            VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTCompressionSessionPrepareToEncodeFrames(session);
            p->compressionSession = session;
        }

        CMTime pts      = CMSampleBufferGetOutputPresentationTimeStamp(input);
        CMTime duration = CMSampleBufferGetOutputDuration(input);

        // Clear stale output
        if (p->lastEncodedBuffer) {
            CFRelease(p->lastEncodedBuffer);
            p->lastEncodedBuffer = nullptr;
        }

        VTCompressionSessionEncodeFrame(
            (VTCompressionSessionRef)p->compressionSession,
            imageBuffer,
            pts,
            duration,
            nullptr,    // frameProperties
            nullptr,    // sourceFrameRefCon
            nullptr);   // infoFlagsOut

        // Flush synchronously so the callback fires before we return
        VTCompressionSessionCompleteFrames(
            (VTCompressionSessionRef)p->compressionSession,
            CMTimeAdd(pts, duration));

        // Callback stored the encoded buffer in p->lastEncodedBuffer
        *output = (CMSampleBufferRef)p->lastEncodedBuffer;
        p->lastEncodedBuffer = nullptr; // caller owns it now
    }

    void AudioVideoProcessor_Decode(AudioVideoProcessorRef processor, CMSampleBufferRef input, CMSampleBufferRef *output) {
        auto *p = processor.get();
        *output = nullptr;
        if (!input || !p->decompressionSession) return;

        __block CVPixelBufferRef decodedPixelBuffer = nullptr;

        VTDecompressionSessionDecodeFrameWithOutputHandler(
            (VTDecompressionSessionRef)p->decompressionSession,
            input,
            kVTDecodeFrame_1xRealTimePlayback,
            nullptr,    // infoFlagsOut
            ^(OSStatus status, VTDecodeInfoFlags infoFlags,
              CVImageBufferRef imageBuffer,
              CMTime presentationTimeStamp,
              CMTime presentationDuration) {
                if (status == noErr && imageBuffer) {
                    CVPixelBufferRetain(imageBuffer);
                    decodedPixelBuffer = imageBuffer;
                }
            });

        // Wait for the asynchronous decode to finish
        VTDecompressionSessionWaitForAsynchronousFrames(
            (VTDecompressionSessionRef)p->decompressionSession);

        if (!decodedPixelBuffer) return;

        // Wrap the decoded pixel buffer back into a CMSampleBuffer
        CMVideoFormatDescriptionRef formatDesc = nullptr;
        CMVideoFormatDescriptionCreateForImageBuffer(
            kCFAllocatorDefault, decodedPixelBuffer, &formatDesc);

        CMSampleTimingInfo timingInfo;
        timingInfo.duration                = CMSampleBufferGetOutputDuration(input);
        timingInfo.presentationTimeStamp   = CMSampleBufferGetOutputPresentationTimeStamp(input);
        timingInfo.decodeTimeStamp         = CMSampleBufferGetDecodeTimeStamp(input);

        CMSampleBufferRef outputBuffer = nullptr;
        CMSampleBufferCreateReadyWithImageBuffer(
            kCFAllocatorDefault,
            decodedPixelBuffer,
            formatDesc,
            &timingInfo,
            &outputBuffer);

        *output = outputBuffer;

        if (formatDesc) CFRelease(formatDesc);
        CVPixelBufferRelease(decodedPixelBuffer);
    }

};
