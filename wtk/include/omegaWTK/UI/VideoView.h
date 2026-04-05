#ifndef OMEGAWTK_UI_VIDEOVIEW_H
#define OMEGAWTK_UI_VIDEOVIEW_H

#include "View.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"
#include "omegaWTK/Media/Video.h"

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
                                  public Media::VideoFrameSink {
    OmegaCommon::QueueHeap<SharedHandle<Media::VideoFrame>> framebuffer;
    SharedHandle<Composition::Canvas> videoCanvas;
    VideoViewDelegate *delegate_ = nullptr;
    VideoScaleMode scaleMode_ = VideoScaleMode::AspectFit;
    VideoSourceMode sourceMode_ = VideoSourceMode::None;
    SharedHandle<Media::VideoPlaybackSession> playbackSession_;
    UniqueHandle<Media::VideoCaptureSession> captureSession_;
    SharedHandle<Media::PlaybackDispatchQueue> dispatchQueue_;
    bool loop_ = false;

    void queueFrame(SharedHandle<Media::VideoFrame> &frame);

    bool framebuffered() const override {
        return true;
    }
    void flush() override;
    void pushFrame(SharedHandle<Media::VideoFrame> frame) override;
    void presentCurrentFrame() override;
public:
    OMEGACOMMON_CLASS("OmegaWTK.VideoView")
    friend class Widget;

    VideoView(const Core::Rect & rect,ViewPtr parent = nullptr);

    void setDelegate(VideoViewDelegate *delegate);
    void setScaleMode(VideoScaleMode mode);
    VideoScaleMode scaleMode() const;
    VideoSourceMode sourceMode() const;

    bool bindPlaybackSource(Media::MediaInputStream & input,
                            const VideoViewPlaybackOptions & opts = {});
    bool bindCapturePreview(SharedHandle<Media::VideoDevice> & videoDevice,
                            SharedHandle<Media::AudioCaptureDevice> audioDevice = nullptr,
                            const VideoViewCaptureOptions & opts = {});
    bool bindCaptureRecord(SharedHandle<Media::VideoDevice> & videoDevice,
                           Media::MediaOutputStream & output,
                           SharedHandle<Media::AudioCaptureDevice> audioDevice = nullptr,
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
