
#include "AVFAudioVideoProcessor.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"

#import <AVFoundation/AVFoundation.h>
#import <AVFAudio/AVFAudio.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

namespace OmegaWTK::Media {

    UniqueHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel,void *gteDevice){
        (void)useHardwareAccel;
        (void)gteDevice;
        return std::make_unique<AudioVideoProcessor>();
    }

    SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(
            UniqueHandle<AudioVideoProcessor> & processor,
            SharedHandle<PlaybackDispatchQueue> & dispatchQueue){
        (void)processor;
        (void)dispatchQueue;
        return nullptr;
    }

};