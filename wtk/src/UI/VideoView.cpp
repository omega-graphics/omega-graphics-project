#include "omegaWTK/UI/VideoView.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"

// Backend AudioVideoProcessor headers live in OmegaVA's private src tree
// (see va/CMakeLists.txt — va/src is exposed as a PRIVATE include dir on
// OmegaWTK_UI so VideoView's destruction of SharedHandle<AudioVideoProcessor>
// has the full type at hand).


namespace OmegaWTK {

static Composition::Rect computeScaledRect(const Composition::Rect &viewRect,
                                    uint32_t frameW, uint32_t frameH,
                                    VideoScaleMode mode) {
    if (mode == VideoScaleMode::Stretch || frameW == 0 || frameH == 0)
        return viewRect;

    float vw = viewRect.w;
    float vh = viewRect.h;
    float frameAspect = static_cast<float>(frameW) / static_cast<float>(frameH);
    float viewAspect  = vw / vh;

    float destW, destH;
    if (mode == VideoScaleMode::AspectFit) {
        if (frameAspect > viewAspect) {
            destW = vw;
            destH = vw / frameAspect;
        } else {
            destH = vh;
            destW = vh * frameAspect;
        }
    } else {
        if (frameAspect > viewAspect) {
            destH = vh;
            destW = vh * frameAspect;
        } else {
            destW = vw;
            destH = vw / frameAspect;
        }
    }

    float x = viewRect.pos.x + (vw - destW) / 2.f;
    float y = viewRect.pos.y + (vh - destH) / 2.f;
    return Composition::Rect{Composition::Point2D{x, y}, destW, destH};
}

VideoView::VideoView(const Composition::Rect & rect, ViewPtr parent)
    : View(rect, parent),
      framebuffer(2) {
    videoCanvas = makeCanvas(getLayerTree()->getRootLayer());
}

void VideoView::queueFrame(SharedHandle<OmegaVA::VideoFrame> &frame) {
    auto &hdr = frame->videoFrame.header;
    Composition::Rect destRect = computeScaledRect(getRect(), hdr.width, hdr.height, scaleMode_);
    // Aliasing-constructor share: `f` carries the same refcount as `frame`
    // but exposes `&frame->videoFrame`. The inner BitmapImage member is
    // not freed independently — the outer VideoFrame is the heap owner —
    // and the frame stays alive for as long as `f` does. The previous
    // `SharedHandle<BitmapImage> f(&frame->videoFrame)` was UB: it tried
    // to `delete` a struct-member pointer on reset, which became
    // dangerous once BitmapImage acquired a real destructor in Phase 2.
    SharedHandle<OmegaCommon::Img::BitmapImage> f(frame, &frame->videoFrame);
    videoCanvas->drawImage(f, destRect);
    videoCanvas->sendFrame();
}

void VideoView::pushFrame(SharedHandle<OmegaVA::VideoFrame> frame) {
    if (!framebuffer.full())
        framebuffer.push(frame);
}

void VideoView::presentCurrentFrame() {
    startCompositionSession();
    auto f = framebuffer.first();
    queueFrame(f);
    framebuffer.pop();
    endCompositionSession();
}

void VideoView::flush() {
    startCompositionSession();
    while (!framebuffer.empty()) {
        auto f = framebuffer.first();
        queueFrame(f);
        framebuffer.pop();
    }
    endCompositionSession();
}

// -- Delegate & accessors --

void VideoView::setDelegate(VideoViewDelegate *delegate) {
    delegate_ = delegate;
}

void VideoView::setScaleMode(VideoScaleMode mode) {
    scaleMode_ = mode;
}

VideoScaleMode VideoView::scaleMode() const {
    return scaleMode_;
}

VideoSourceMode VideoView::sourceMode() const {
    return sourceMode_;
}

// -- Playback binding --

bool VideoView::bindPlaybackSource(OmegaVA::MediaInputStream & input,
                                   const VideoViewPlaybackOptions & opts) {
    clear();

    auto processor = OmegaVA::createAudioVideoProcessor(opts.useHardwareAccel, nullptr);
    if (!processor) {
        if (delegate_)
            delegate_->onVideoError("Failed to create audio/video processor");
        return false;
    }

    dispatchQueue_ = OmegaVA::createPlaybackDispatchQueue();
    playbackSession_ = OmegaVA::VideoPlaybackSession::Create(processor, dispatchQueue_);
    if (!playbackSession_) {
        if (delegate_)
            delegate_->onVideoError("Failed to create playback session");
        return false;
    }

    playbackSession_->setVideoSource(input);
    playbackSession_->setVideoFrameSink(*this);

    sourceMode_ = VideoSourceMode::Playback;
    loop_ = opts.loop;

    if (delegate_)
        delegate_->onVideoReady();

    if (opts.autoplay)
        play();

    return true;
}

// -- Capture bindings --

bool VideoView::bindCapturePreview(SharedHandle<OmegaVA::VideoDevice> & videoDevice,
                                   SharedHandle<OmegaVA::AudioCaptureDevice> audioDevice,
                                   const VideoViewCaptureOptions & opts) {
    clear();

    captureSession_ = videoDevice->createCaptureSession(audioDevice);
    if (!captureSession_) {
        if (delegate_)
            delegate_->onVideoError("Failed to create capture session");
        return false;
    }

    captureSession_->setVideoFrameSinkForPreview(*this);
    sourceMode_ = VideoSourceMode::CapturePreview;

    if (delegate_)
        delegate_->onVideoReady();

    return true;
}

bool VideoView::bindCaptureRecord(SharedHandle<OmegaVA::VideoDevice> & videoDevice,
                                  OmegaVA::MediaOutputStream & output,
                                  SharedHandle<OmegaVA::AudioCaptureDevice> audioDevice,
                                  const VideoViewCaptureOptions & opts) {
    clear();

    captureSession_ = videoDevice->createCaptureSession(audioDevice);
    if (!captureSession_) {
        if (delegate_)
            delegate_->onVideoError("Failed to create capture session");
        return false;
    }

    captureSession_->setVideoFrameSinkForPreview(*this);
    captureSession_->setVideoOutputStream(output);
    sourceMode_ = VideoSourceMode::CaptureRecord;

    if (delegate_)
        delegate_->onVideoReady();

    return true;
}

// -- Playback controls --

void VideoView::play() {
    if (sourceMode_ == VideoSourceMode::Playback && playbackSession_)
        playbackSession_->start();
}

void VideoView::pause() {
    if (sourceMode_ == VideoSourceMode::Playback && playbackSession_)
        playbackSession_->pause();
}

void VideoView::stop() {
    if (sourceMode_ != VideoSourceMode::Playback || !playbackSession_)
        return;

    playbackSession_->reset();

    startCompositionSession();
    while (!framebuffer.empty())
        framebuffer.pop();
    videoCanvas->sendFrame();
    endCompositionSession();
}

// -- Capture controls --

void VideoView::startPreview() {
    if ((sourceMode_ == VideoSourceMode::CapturePreview ||
         sourceMode_ == VideoSourceMode::CaptureRecord) && captureSession_)
        captureSession_->startPreview();
}

void VideoView::stopPreview() {
    if ((sourceMode_ == VideoSourceMode::CapturePreview ||
         sourceMode_ == VideoSourceMode::CaptureRecord) && captureSession_)
        captureSession_->stopPreview();
}

void VideoView::startRecording() {
    if (sourceMode_ == VideoSourceMode::CaptureRecord && captureSession_)
        captureSession_->startRecord();
}

void VideoView::stopRecording() {
    if (sourceMode_ == VideoSourceMode::CaptureRecord && captureSession_)
        captureSession_->stopRecord();
}

// -- Teardown --

void VideoView::clear() {
    if (sourceMode_ == VideoSourceMode::Playback && playbackSession_) {
        playbackSession_->reset();
        playbackSession_.reset();
    }

    if ((sourceMode_ == VideoSourceMode::CapturePreview ||
         sourceMode_ == VideoSourceMode::CaptureRecord) && captureSession_) {
        captureSession_->stopPreview();
        captureSession_.reset();
    }

    dispatchQueue_.reset();

    startCompositionSession();
    while (!framebuffer.empty())
        framebuffer.pop();
    videoCanvas->sendFrame();
    endCompositionSession();

    sourceMode_ = VideoSourceMode::None;
    loop_ = false;
}

}
