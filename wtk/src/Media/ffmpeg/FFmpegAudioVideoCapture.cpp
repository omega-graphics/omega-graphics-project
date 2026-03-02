#include "omegaWTK/Media/Audio.h"
#include "omegaWTK/Media/Video.h"

namespace OmegaWTK::Media {
    class FFmpegVideoDevice : public VideoDevice {
    public:
        UniqueHandle<VideoCaptureSession> createCaptureSession(SharedHandle<AudioCaptureDevice> &audioCaptureDevice) override {
            (void)audioCaptureDevice;
            return nullptr;
        }
    };
}
