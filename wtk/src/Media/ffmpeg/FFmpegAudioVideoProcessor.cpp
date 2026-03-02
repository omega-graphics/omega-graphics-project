#include "omegaWTK/Media/AudioVideoProcessorContext.h"

namespace OmegaWTK::Media {
    class AudioVideoProcessor {
    public:
        explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice) {
            (void)useHardwareAccel;
            (void)gteDevice;
        }
    };

    UniqueHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel, void *gteDevice) {
        return std::make_unique<AudioVideoProcessor>(useHardwareAccel, gteDevice);
    }
};
