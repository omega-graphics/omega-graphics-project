# VideoView and SVGView Implementation Plan

## Goals
1. Provide a production API for `VideoView` that supports:
   - media file playback
   - live camera preview
   - camera capture recording
2. Provide a production API for `SVGView` that supports:
   - parsing SVG XML through `omegaWTK/Core/XML.h`
   - rendering SVG primitives through compositor `Canvas` commands
   - optional animation playback over the existing animation runtime
3. Keep both views synchronized with WidgetTree frame submission (no partial tree updates).

## Current Baseline
1. `VideoView` already implements `Media::VideoFrameSink` and can draw `VideoFrame` bitmaps.
2. `SVGView`/`SVGSession` exist but parsing/rendering is mostly stubbed.
3. Media backends already expose:
   - `Media::VideoPlaybackSession`
   - `Media::VideoCaptureSession`
   - `Media::enumerateVideoDevices()`
   - `Media::MediaInputStream` and `Media::MediaOutputStream`
4. XML parser already exists in `Core::XMLDocument`.

## Proposed Public API

### VideoView
```cpp
namespace OmegaWTK {

enum class VideoScaleMode : int {
    AspectFit,
    AspectFill,
    Stretch
};

enum class VideoSourceMode : int {
    None,
    Playback,
    CapturePreview,
    CaptureRecord
};

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
    virtual void onVideoError(const OmegaCommon::String &message) {}
    virtual ~VideoViewDelegate() = default;
};

class OMEGAWTK_EXPORT VideoView : public View, public Media::VideoFrameSink {
public:
    void setDelegate(VideoViewDelegate *delegate);

    void setScaleMode(VideoScaleMode mode);
    VideoScaleMode scaleMode() const;

    VideoSourceMode sourceMode() const;

    bool bindPlaybackSource(Media::MediaInputStream input,
                            const VideoViewPlaybackOptions &opts = {});
    bool bindCapturePreview(SharedHandle<Media::VideoDevice> videoDevice,
                            SharedHandle<Media::AudioCaptureDevice> audioDevice,
                            const VideoViewCaptureOptions &opts = {});
    bool bindCaptureRecord(SharedHandle<Media::VideoDevice> videoDevice,
                           SharedHandle<Media::AudioCaptureDevice> audioDevice,
                           Media::MediaOutputStream output,
                           const VideoViewCaptureOptions &opts = {});

    bool play();
    bool pause();
    bool stop();

    bool startPreview();
    bool stopPreview();
    bool startRecording();
    bool stopRecording();

    void clear();
};

}
```

### SVGView
```cpp
namespace OmegaWTK {

enum class SVGScaleMode : int {
    None,
    Meet,
    Slice
};

struct SVGViewRenderOptions {
    SVGScaleMode scaleMode = SVGScaleMode::Meet;
    bool antialias = true;
    bool enableAnimation = true;
};

class OMEGAWTK_EXPORT SVGViewDelegate {
public:
    virtual void onSVGLoaded() {}
    virtual void onSVGParseError(const OmegaCommon::String &message) {}
    virtual ~SVGViewDelegate() = default;
};

class OMEGAWTK_EXPORT SVGView : public View {
public:
    void setDelegate(SVGViewDelegate *delegate);

    bool setSourceDocument(Core::XMLDocument document);
    bool setSourceString(const OmegaCommon::String &svgText);
    bool setSourceStream(std::istream &stream);

    void setRenderOptions(const SVGViewRenderOptions &opts);
    SVGViewRenderOptions renderOptions() const;

    bool renderNow();
    bool play();
    bool pause();
    bool reset();
};

}
```

## Media Integration Design (VideoView)

### Playback path
1. Build processor via:
   - `Media::createAudioVideoProcessor(useHardwareAccel, gteDevice)`
2. Build queue via:
   - `Media::createPlaybackDispatchQueue()`
3. Build session via:
   - `Media::VideoPlaybackSession::Create(processor, queue)`
4. Wire session:
   - `setVideoSource(input)`
   - `setVideoFrameSink(*this)` (`VideoView` remains `VideoFrameSink`)
   - optional `setAudioPlaybackDevice(device)`
5. Control:
   - `start/pause/reset` mapped to `play/pause/stop`.

### Capture path
1. Device enumeration:
   - `Media::enumerateVideoDevices()`
2. Session creation:
   - `videoDevice->createCaptureSession(audioCaptureDevice)`
3. Preview wiring:
   - `setVideoFrameSinkForPreview(*this)`
   - optional `setAudioPlaybackDeviceForPreview(device)`
4. Record wiring:
   - `setVideoOutputStream(output)`
   - use `startRecord/stopRecord`
5. Preview controls:
   - use `startPreview/stopPreview`.

### Frame presentation contract
1. `pushFrame` only enqueues decoded frames.
2. `presentCurrentFrame` consumes one frame and submits one compositor frame.
3. `flush` drains all pending frames and submits in-order.
4. `VideoView` uses `startCompositionSession/endCompositionSession` per present cycle.
5. Compositor sync lane from WidgetTree remains authoritative for atomic tree updates.

## XML Integration Design (SVGView)

### Parsing entrypoints
1. `setSourceString`:
   - `Core::XMLDocument::parseFromString(svgText)`
2. `setSourceStream`:
   - `Core::XMLDocument::parseFromStream(stream)`
3. `setSourceDocument`:
   - directly stores parsed document

### DOM traversal model
1. Start from `doc.root()`.
2. Read tags through:
   - `Tag::name()`
   - `Tag::attribute(name)`
   - `Tag::children()`
3. Build internal display list of normalized draw ops:
   - `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, `path`, `g`
4. Convert style attributes:
   - `fill`, `fill-opacity`, `stroke`, `stroke-opacity`, `stroke-width`
5. Map path data (`d`) into `Composition::Path`.

### Rendering model
1. Each normalized SVG node emits one or more `Canvas` operations:
   - `drawRect`
   - `drawRoundedRect` when `rx/ry` present
   - `drawEllipse`
   - `drawPath`
2. Group transforms (`<g transform=...>`) are flattened into transformed geometry in Slice A.
3. Later slices may move transforms to layer effects for better animation throughput.

## Internal Architecture

### VideoView internals
1. Session state:
   - `SharedHandle<Media::VideoPlaybackSession> playbackSession`
   - `UniqueHandle<Media::VideoCaptureSession> captureSession`
2. Source mode lock:
   - only one active source mode at a time
3. Render resources:
   - root video layer + `Canvas`
   - frame queue bounded (drop oldest on overflow)
4. Lifecycle:
   - stop/reset and clear resources when switching mode

### SVGView internals
1. Parse state:
   - `Core::Optional<Core::XMLDocument> sourceDoc`
   - `OmegaCommon::Vector<SVGDrawOp> drawOps`
2. Render resources:
   - root svg layer + `Canvas`
3. Session/timeline:
   - optional `Composition::AnimationSchedulerClient` integration for animated SVG
4. Invalidation:
   - re-render on source change and `Resize`

## Implementation Slices

### Slice A: Functional core
1. Finalize `VideoView` API and implement playback source binding.
2. Implement capture preview and capture record binding.
3. Implement source-mode switching safety.
4. Implement `SVGView` source parsing from string/stream/document via `XMLDocument`.
5. Implement SVG primitives:
   - rect/circle/ellipse/line/polyline/polygon/path
6. Implement style parsing for fill and stroke.
7. Add tests:
   - `VideoPlaybackViewTest`
   - `VideoCapturePreviewTest`
   - `SVGViewParseRenderTest`

### Slice B: Resize and quality
1. Implement scale modes (`AspectFit/Fill/Stretch`, `None/Meet/Slice`).
2. Add viewport/viewBox mapping for SVG.
3. Improve frame pacing for VideoView during resize bursts.
4. Cache parsed SVG display list and only rebuild on source change.

### Slice C: Animation and advanced features
1. SVG animation time sampling tied to animation scheduler.
2. Optional layer-level transform mapping for grouped SVG nodes.
3. Optional video post-processing via compositor effects.

## Error Handling and Diagnostics
1. All parse failures return `false` and call delegate error callback.
2. Media session creation failures return `false` and call delegate error callback.
3. Debug logging should include:
   - source mode transitions
   - frame queue depth
   - SVG parse node counts and unsupported tag counts

## Validation Matrix
1. Playback:
   - MP4 file playback with audio device selected
2. Capture:
   - preview start/stop
   - record start/stop to file stream
3. SVG:
   - simple shape file
   - path-heavy file
   - resize behavior with viewBox
4. Compositor sync:
   - mixed WidgetTree updates do not interleave partial frames
