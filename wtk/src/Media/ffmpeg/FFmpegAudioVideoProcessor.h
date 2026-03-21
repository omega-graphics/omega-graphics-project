#ifndef OMEGAWTK_MEDIA_FFMPEG_FFMPEGAUDIOVIDEOPROCESSOR_H
#define OMEGAWTK_MEDIA_FFMPEG_FFMPEGAUDIOVIDEOPROCESSOR_H

#include "omegaWTK/Media/AudioVideoProcessorContext.h"

namespace OmegaWTK::Media {

class AudioVideoProcessor {
public:
    explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice) {
        (void)useHardwareAccel;
        (void)gteDevice;
    }
};

} // namespace OmegaWTK::Media

#endif // OMEGAWTK_MEDIA_FFMPEG_FFMPEGAUDIOVIDEOPROCESSOR_H
