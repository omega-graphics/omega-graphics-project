#include "Audio.h"
#include "Video.h"
#include "AudioVideoProcessorContext.h"

#ifndef OMEGAVA_MEDIAPLAYBACKSESSION_H
#define OMEGAVA_MEDIAPLAYBACKSESSION_H

namespace OmegaVA {

    /// @brief A threaded queue for scheduling and dispatching playback of media from playback sessions.
    class PlaybackDispatchQueue;

    /** @brief Creates A PlaybackDispatchQueue
       @returns A new PlaybackDispatchQueue.
     */
    OMEGAVA_EXPORT SharedHandle<PlaybackDispatchQueue> createPlaybackDispatchQueue();

    INTERFACE OMEGAVA_EXPORT AudioPlaybackSession : public AudioVideoProcessorContext{
    protected:
        explicit AudioPlaybackSession(SharedHandle<AudioVideoProcessor> & processor) : AudioVideoProcessorContext(processor){}
    public:
        static SharedHandle<AudioPlaybackSession> Create(SharedHandle<AudioVideoProcessor> & processor,
                                                         SharedHandle<PlaybackDispatchQueue> & dispatchQueue);
        INTERFACE_METHOD void setAudioSource(MediaInputStream &inputStream) ABSTRACT;
        INTERFACE_METHOD void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> & device) ABSTRACT;
        INTERFACE_METHOD void start() ABSTRACT;
        INTERFACE_METHOD void pause() ABSTRACT;
        INTERFACE_METHOD void reset() ABSTRACT;
        INTERFACE_METHOD ~AudioPlaybackSession() = default;
    };

    INTERFACE OMEGAVA_EXPORT VideoPlaybackSession  : public AudioVideoProcessorContext{
    protected:
        explicit VideoPlaybackSession(SharedHandle<AudioVideoProcessor> & processor) : AudioVideoProcessorContext(processor){}
    public:
        static SharedHandle<VideoPlaybackSession> Create(SharedHandle<AudioVideoProcessor> & processor,
                                                         SharedHandle<PlaybackDispatchQueue> & dispatchQueue);
        INTERFACE_METHOD void setVideoSource(MediaInputStream & inputStream) ABSTRACT;
        INTERFACE_METHOD void setVideoFrameSink(VideoFrameSink & sink) ABSTRACT;
        INTERFACE_METHOD void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> & device) ABSTRACT;
        INTERFACE_METHOD void start() ABSTRACT;
        INTERFACE_METHOD void pause() ABSTRACT;
        INTERFACE_METHOD void reset() ABSTRACT;
        INTERFACE_METHOD ~VideoPlaybackSession() = default;
    };

}

#endif
