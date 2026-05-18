#include "omegaVA/Core.h"
#include "omega-common/img.h"
#include "omegaVA/MediaIO.h"

#include <chrono>

#ifndef OMEGAVA_AUDIOVIDEOPROCESSORCONTEXT_H
#define OMEGAVA_AUDIOVIDEOPROCESSORCONTEXT_H

namespace OmegaVA {

    typedef std::chrono::high_resolution_clock::time_point TimePoint;

    /// @brief Frame dimensions in pixels.
    struct FrameSize {
        uint32_t width;
        uint32_t height;
    };

    /// @brief A decoded video frame with timing metadata.
    struct VideoFrame {
        OmegaCommon::Img::BitmapImage videoFrame;
        TimePoint decodeFinishTime;
        TimePoint presentTime;
    };

    /// @brief A decoded audio sample buffer with format and timing metadata.
    struct AudioSample {
        void *data = nullptr;
        size_t length = 0;
        AudioStreamDesc format {};
        TimePoint presentTime;
        TimePoint decodeTime;
    };

    INTERFACE VideoFrameSink {
    public:
        INTERFACE_METHOD bool framebuffered() const ABSTRACT;
        INTERFACE_METHOD void pushFrame(SharedHandle<VideoFrame> frame) ABSTRACT;
        INTERFACE_METHOD void presentCurrentFrame() ABSTRACT;
        INTERFACE_METHOD void flush() ABSTRACT;
    };

    class AudioVideoProcessor;

    OMEGAVA_EXPORT SharedHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel,void *gteDevice);

    class AudioVideoProcessorContext {
    protected:
        SharedHandle<AudioVideoProcessor> processor;
        explicit AudioVideoProcessorContext(SharedHandle<AudioVideoProcessor> & processor):processor(processor){
            
        };
    };

};

#endif
