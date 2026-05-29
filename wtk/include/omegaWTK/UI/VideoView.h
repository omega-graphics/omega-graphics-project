#ifndef OMEGAWTK_UI_VIDEOVIEW_H
#define OMEGAWTK_UI_VIDEOVIEW_H

#include "View.h"
#include "omegaVA/MediaPlaybackSession.h"
#include "omegaVA/Video.h"

namespace OmegaWTK {

enum class VideoScaleMode : int { AspectFit, AspectFill, Stretch };
enum class VideoSourceMode : int { None, Playback, CapturePreview, CaptureRecord };

struct VideoViewPlaybackOptions {
    bool useHardwareAccel = true;
    bool autoplay = false;
    bool loop = false;
};

struct VideoViewCaptureOptions {
    bool previewAudio = false;
    bool recordAudio = true;
};

class OMEGAWTK_EXPORT VideoViewDelegate {
public:
    virtual void onVideoReady() {}
    virtual void onVideoEndOfStream() {}
    virtual void onVideoError(const OmegaCommon::String & message) {}
    virtual ~VideoViewDelegate() = default;
};

/**
 @brief The visual display output of a VideoPlaybackSession or a capture preview output of a VideoCaptureSession.
*/
class OMEGAWTK_EXPORT VideoView : public View,
                                  public OmegaVA::VideoFrameSink {
    OmegaCommon::QueueHeap<SharedHandle<OmegaVA::VideoFrame>> framebuffer;
    VideoViewDelegate *delegate_ = nullptr;
    VideoScaleMode scaleMode_ = VideoScaleMode::AspectFit;
    VideoSourceMode sourceMode_ = VideoSourceMode::None;
    SharedHandle<OmegaVA::VideoPlaybackSession> playbackSession_;
    UniqueHandle<OmegaVA::VideoCaptureSession> captureSession_;
    SharedHandle<OmegaVA::PlaybackDispatchQueue> dispatchQueue_;
    bool loop_ = false;

    void queueFrame(SharedHandle<OmegaVA::VideoFrame> &frame);

    bool framebuffered() const override {
        return true;
    }
    void flush() override;
    void pushFrame(SharedHandle<OmegaVA::VideoFrame> frame) override;
    void presentCurrentFrame() override;
public:
    OMEGACOMMON_CLASS("OmegaWTK.VideoView")
    friend class Widget;

    VideoView(const Composition::Rect & rect,ViewPtr parent = nullptr);

    void setDelegate(VideoViewDelegate *delegate);
    void setScaleMode(VideoScaleMode mode);
    VideoScaleMode scaleMode() const;
    VideoSourceMode sourceMode() const;

    bool bindPlaybackSource(OmegaVA::MediaInputStream & input,
                            const VideoViewPlaybackOptions & opts = {});
    bool bindCapturePreview(SharedHandle<OmegaVA::VideoDevice> & videoDevice,
                            SharedHandle<OmegaVA::AudioCaptureDevice> audioDevice = nullptr,
                            const VideoViewCaptureOptions & opts = {});
    bool bindCaptureRecord(SharedHandle<OmegaVA::VideoDevice> & videoDevice,
                           OmegaVA::MediaOutputStream & output,
                           SharedHandle<OmegaVA::AudioCaptureDevice> audioDevice = nullptr,
                           const VideoViewCaptureOptions & opts = {});

    void play();
    void pause();
    void stop();

    void startPreview();
    void stopPreview();

    void startRecording();
    void stopRecording();

    void clear();
};

}

#endif
