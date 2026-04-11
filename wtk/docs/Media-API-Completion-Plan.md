# Media API Completion Plan

## Current State Assessment

### What exists and works
- **Image codecs** (PNG, JPEG, TIFF): Fully implemented cross-platform via libpng, turbojpeg, libtiff.
- **MediaIO.h**: Struct declarations for `MediaBuffer`, `MediaInputStream`, `MediaOutputStream`, plus Phase 0 type additions (`AudioSampleFormat`, `AudioStreamDesc`, `PixelFormat`, `MediaCodecID`, `VideoStreamDesc`, `ContainerFormat`, `MediaSourceDesc`). `AudioSample` added to `AudioVideoProcessorContext.h`.
- **Interface headers**: `Audio.h`, `Video.h`, `AudioVideoProcessorContext.h`, `MediaPlaybackSession.h` define the public API surface for capture, playback, and processing.

### What is stubbed (no implementation file)
- **`MediaInputStream` / `MediaOutputStream` factory methods** — `fromFile()`, `fromBuffer()`, `toFile()`, `toBuffer()` are declared in `MediaIO.h` but **no `MediaIO.cpp` exists** to define them. Any call site (e.g., `VideoViewPlaybackTest/main.cpp` line 46: `MediaInputStream::fromFile(filePath)`) will fail at link time. Every platform backend that consumes these structs (`AVFAudioVideoCapture.mm`, `WMFAudioVideoCapture.cpp`) reads their fields directly, assuming someone else constructed them — but there is currently no way to construct them correctly.

### What is incomplete

| Area | macOS (AVFoundation) | Windows (WMF) | Linux (FFmpeg) |
|---|---|---|---|
| **Device Enumeration** | Audio + Video done | Audio + Video done | 100% stubbed |
| **Audio Capture** | Session implemented | Session implemented | 100% stubbed |
| **Video Capture** | Session shell (start/stop/record empty) | Session implemented | 100% stubbed |
| **AudioVideoProcessor** | Empty class, no encode/decode | Constructor + codec selection partial, `encodeFrame`/`decodeFrame` bodies empty | Empty stub constructor |
| **Audio Playback** | Session implemented | Session implemented | Returns `nullptr` |
| **Video Playback** | Returns `nullptr` | Session setup done, `start`/`pause`/`reset` empty | Returns `nullptr` |
| **PlaybackDispatchQueue** | Threaded loop implemented | MF work queue wrapper done | Returns `nullptr` |

### Critical bugs
- **`MediaPlaybackSession.h:39`** — Missing semicolon after `setVideoFrameSink()` declaration. Must be fixed before any compilation.

---

## Phase 0: MediaIO.h Type Expansion

The existing `MediaIO.h` types cover raw byte-level I/O but lack the structured metadata that all three platform backends need for codec negotiation, sample format description, and pipeline configuration. These types are consumed by `AudioVideoProcessor`, `AudioPlaybackSession`, and `VideoCaptureSession` across all platforms.

### New types to add to `MediaIO.h`

```cpp
namespace OmegaWTK::Media {

    /// Audio sample format (PCM layout in memory)
    enum class AudioSampleFormat : OPT_PARAM {
        S16,        // signed 16-bit interleaved
        S32,        // signed 32-bit interleaved
        Float32,    // 32-bit float interleaved
        Float64,    // 64-bit float interleaved
        Planar_S16, // signed 16-bit planar (one buffer per channel)
        Planar_Float32
    };

    /// Describes the format of an audio stream
    struct AudioStreamDesc {
        unsigned int sampleRate;      // e.g. 44100, 48000
        unsigned int channels;        // e.g. 1 (mono), 2 (stereo), 6 (5.1)
        unsigned int bitsPerSample;   // e.g. 16, 24, 32
        AudioSampleFormat sampleFormat;
    };

    /// Identifies a codec for encode/decode operations
    enum class MediaCodecID : OPT_PARAM {
        // Video
        H264,
        HEVC,
        VP9,
        AV1,
        // Audio
        AAC,
        MP3,
        FLAC,
        Opus,
        PCM,
        // Raw / passthrough
        RawVideo,
        RawAudio
    };

    /// Describes the format of a video stream
    struct VideoStreamDesc {
        unsigned int width;
        unsigned int height;
        unsigned int frameRateNum;     // numerator   (e.g. 30000)
        unsigned int frameRateDen;     // denominator (e.g. 1001 for 29.97)
        MediaCodecID codec;
        BitmapImage::ColorFormat pixelFormat;
        unsigned int bitDepth;
    };

    /// Container/mux format for file-level I/O
    enum class ContainerFormat : OPT_PARAM {
        MP4,
        MKV,
        WebM,
        WAV,
        FLAC_Container,
        OGG,
        Raw
    };

    /// An audio sample buffer with timing
    struct AudioSample {
        void *data;
        size_t length;
        AudioStreamDesc format;
        TimePoint presentTime;
        TimePoint decodeTime;
    };

    /// Extended MediaInputStream with format hints
    struct MediaSourceDesc {
        MediaInputStream stream;
        ContainerFormat container;       // may be Unknown for auto-detect
        OmegaCommon::Vector<AudioStreamDesc> audioStreams;
        OmegaCommon::Vector<VideoStreamDesc> videoStreams;
    };
}
```

### Rationale
- **`AudioStreamDesc` / `VideoStreamDesc`**: Every platform backend currently either hardcodes format info inline or ignores it entirely. AVFoundation needs `AudioStreamBasicDescription`-compatible values. WMF needs them to build `IMFMediaType`. FFmpeg needs them for `AVCodecContext` setup.
- **`MediaCodecID`**: WMF currently uses raw GUIDs (`MFVideoFormat_HEVC`), AVFoundation uses `CMVideoCodecType`, FFmpeg uses `AVCodecID`. A shared enum lets the public API stay platform-agnostic while each backend maps to its native identifiers.
- **`AudioSample`**: Currently audio data flows through platform-specific opaque types (`CMSampleBufferRef`, `IMFSample`). An `AudioSample` provides a common currency for the `VideoFrameSink`-equivalent on the audio side.
- **`ContainerFormat`**: Needed by the processor and playback sessions to select demuxers/muxers. Currently the WMF backend hardcodes `MFTranscodeContainerType_MP3`/`MPEG4` inline.

---

## Phase 1: Fix the Build Break

**File**: `wtk/include/omegaWTK/Media/MediaPlaybackSession.h`

Line 39 — add the missing semicolon:
```cpp
// Before:
INTERFACE_METHOD void setVideoFrameSink(VideoFrameSink & sink) ABSTRACT

// After:
INTERFACE_METHOD void setVideoFrameSink(VideoFrameSink & sink) ABSTRACT;
```

---

## Phase 1b: MediaInputStream / MediaOutputStream — Implement Factory Methods

### Problem

`MediaInputStream` and `MediaOutputStream` declare four static factory methods in `MediaIO.h`, but **no implementation file exists**. Every call site (e.g., `VideoViewPlaybackTest/main.cpp` line 46: `MediaInputStream::fromFile(filePath)`) will produce an unresolved-symbol link error. The structs themselves are used pervasively — every platform backend has helpers that read their `bufferOrFile`, `file`, and `buffer` fields:

| Platform | Input helper | Output helper |
|---|---|---|
| macOS | `createURLFromMediaInputStream()` in `AVFAudioVideoCapture.mm` | `createURLFromMediaOutputStream()` in `AVFAudioVideoCapture.mm` |
| Windows | `createMFByteStreamMediaInputStream()` in `WMFAudioVideoCapture.cpp` | `createMFByteStreamMediaOutputStream()` in `WMFAudioVideoCapture.cpp` |
| Linux | Not yet implemented | Not yet implemented |

All of these helpers assume the struct fields are already populated — they branch on `bufferOrFile` and read either `file` or `buffer.data`/`buffer.length`. The missing piece is the factory methods that populate those fields.

### Implementation

**New file**: `wtk/src/Media/MediaIO.cpp`

This file is platform-agnostic (no platform-specific code). The CMake `file(GLOB MEDIA_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/*.cpp")` will pick it up automatically on all platforms.

```cpp
#include "omegaWTK/Media/MediaIO.h"

namespace OmegaWTK::Media {

    MediaInputStream MediaInputStream::fromFile(const OmegaCommon::FS::Path & path) {
        MediaInputStream s;
        s.bufferOrFile = false;            // false = file mode
        s.file = path.string();
        s.buffer = {nullptr, 0};
        return s;
    }

    MediaInputStream MediaInputStream::fromBuffer(void *data, size_t length) {
        MediaInputStream s;
        s.bufferOrFile = true;             // true = buffer mode
        s.file = {};
        s.buffer = {data, length};
        return s;
    }

    MediaOutputStream MediaOutputStream::toFile(const OmegaCommon::FS::Path & path) {
        MediaOutputStream s;
        s.bufferOrFile = false;
        s.file = path.string();
        s.buffer = {nullptr, 0};
        return s;
    }

    MediaOutputStream MediaOutputStream::toBuffer(void *data, size_t length) {
        MediaOutputStream s;
        s.bufferOrFile = true;
        s.file = {};
        s.buffer = {data, length};
        return s;
    }

}
```

### Semantics

| Factory | `bufferOrFile` | `file` | `buffer` |
|---|---|---|---|
| `fromFile(path)` | `false` | `path.string()` | `{nullptr, 0}` |
| `fromBuffer(data, len)` | `true` | empty | `{data, len}` |
| `toFile(path)` | `false` | `path.string()` | `{nullptr, 0}` |
| `toBuffer(data, len)` | `true` | empty | `{data, len}` |

The `bufferOrFile` discriminant matches the convention already used by every platform backend:
- macOS: `if(inputStream.bufferOrFile)` → `NSData` memory URL; `else` → `fileURLWithFileSystemRepresentation`
- Windows: `if(inputStream.bufferOrFile)` → `SHCreateMemStream`; `else` → `MFCreateFile`

### Call sites that will link once implemented

| File | Line | Call |
|---|---|---|
| `wtk/tests/VideoViewPlaybackTest/main.cpp` | 46 | `MediaInputStream::fromFile(filePath)` |
| `wtk/src/UI/VideoView.cpp` | 110+ | Receives `MediaInputStream &` (constructed by caller) |
| `wtk/src/UI/VideoView.cpp` | 168+ | Receives `MediaOutputStream &` (constructed by caller) |

### Notes

- **No ownership transfer** for buffer mode: the factories store the raw pointer and length as-is. The caller must ensure the buffer outlives the stream. This matches how WMF's `SHCreateMemStream` and AVFoundation's `dataWithBytesNoCopy:` consume the data — neither copies.
- **`OmegaCommon::FS::Path`**: The `path.string()` call converts to `OmegaCommon::String` (which is `std::string`). The macOS backend then converts via `file.data()` → `fileURLWithFileSystemRepresentation`, and WMF converts via `cpp_to_wstring`.
- **Future**: If `MediaSourceDesc` gains its own factory/builder methods, they should construct a `MediaInputStream` internally via these factories and attach format metadata.

---

## Phase 2: Linux (FFmpeg + VA-API) — Full Implementation

This is the largest body of work. Every function currently returns `nullptr` or is an empty stub.

### 2.1 Architecture

```
FFmpegAudioVideoProcessor
  ├─ libavcodec   (encode/decode via AVCodecContext)
  ├─ libavformat  (demux/mux containers)
  ├─ libavutil    (pixel format conversion, frame allocation)
  ├─ libswscale   (color space conversion to RGBA for VideoFrame)
  ├─ libswresample (audio resampling)
  └─ VA-API       (hardware-accelerated decode/encode when useHardwareAccel=true)

FFmpegAudioVideoCapture
  ├─ libavdevice  (v4l2 for video, alsa/pulseaudio for audio)
  └─ libavformat  (input device as format context)

PlaybackDispatchQueue
  └─ std::thread + std::condition_variable (matches macOS model)
```

### 2.2 `AudioVideoProcessor` Implementation

**File**: `wtk/src/Media/ffmpeg/FFmpegAudioVideoProcessor.h`

```cpp
class AudioVideoProcessor {
    AVCodecContext *encodeCtx = nullptr;
    AVCodecContext *decodeCtx = nullptr;
    AVBufferRef *hwDeviceCtx = nullptr;   // VA-API device ref
    SwsContext *swsCtx = nullptr;
    SwrContext *swrCtx = nullptr;

    bool useHardwareAccel;
    MediaCodecID currentEncodeCodec = MediaCodecID::H264;
    MediaCodecID currentDecodeCodec = MediaCodecID::H264;

public:
    explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice);
    ~AudioVideoProcessor();

    void setEncodeCodec(MediaCodecID codec, const VideoStreamDesc &desc);
    void setDecodeCodec(MediaCodecID codec, const VideoStreamDesc &desc);
    void encodeFrame(const VideoFrame &input, MediaBuffer &output);
    void decodeFrame(const MediaBuffer &input, VideoFrame &output);

    // Audio codec support
    void setAudioEncodeCodec(MediaCodecID codec, const AudioStreamDesc &desc);
    void setAudioDecodeCodec(MediaCodecID codec, const AudioStreamDesc &desc);
    void encodeAudio(const AudioSample &input, MediaBuffer &output);
    void decodeAudio(const MediaBuffer &input, AudioSample &output);
};
```

**Constructor logic**:
1. Call `av_log_set_level(AV_LOG_WARNING)`.
2. If `useHardwareAccel`:
   - `av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0)`.
   - Store the `AVHWDeviceContext` for later use by encode/decode contexts.
3. Store the GTE device pointer for potential interop (e.g., DMA-BUF export to Vulkan/OpenGL textures).

**`setDecodeCodec` logic**:
1. Map `MediaCodecID` to `AVCodecID` (e.g., `H264` -> `AV_CODEC_ID_H264`).
2. If hardware-accelerated:
   - Find decoder with `avcodec_find_decoder(codecId)`.
   - Allocate `AVCodecContext`, set `hw_device_ctx = av_buffer_ref(hwDeviceCtx)`.
   - For VA-API, set `get_format` callback to select `AV_PIX_FMT_VAAPI`.
3. If software:
   - Standard `avcodec_find_decoder` + `avcodec_open2`.
4. Allocate `SwsContext` for converting decoded frames to RGBA (`sws_getContext`).

**`decodeFrame` logic**:
1. Wrap input `MediaBuffer` in `AVPacket` via `av_packet_from_data`.
2. `avcodec_send_packet(decodeCtx, packet)`.
3. `avcodec_receive_frame(decodeCtx, frame)`.
4. If VA-API: `av_hwframe_transfer_data(sw_frame, frame, 0)` to download from GPU.
5. `sws_scale` to convert to RGBA.
6. Copy to `VideoFrame.videoFrame.data`, populate header fields.
7. Set `decodeFinishTime` to `std::chrono::high_resolution_clock::now()`.

**`encodeFrame`** follows the inverse: RGBA -> `sws_scale` to encoder's pixel format -> `avcodec_send_frame` -> `avcodec_receive_packet` -> copy to output `MediaBuffer`.

### 2.3 Device Enumeration

**File**: `wtk/src/Media/ffmpeg/FFmpegAudioVideoCapture.cpp`

**`enumerateVideoDevices()`**:
1. `avdevice_register_all()` (once).
2. Open `/dev/video*` via `avformat_open_input` with `v4l2` input format.
3. Query device capabilities via `v4l2_ioctl(VIDIOC_QUERYCAP)` or by probing the format context.
4. Wrap each discovered device in an `FFmpegVideoDevice` that stores the device path.

**`enumerateAudioCaptureDevices()`**:
1. Use PulseAudio's `pa_context_get_source_info_list` to enumerate audio sources.
2. Alternatively, enumerate ALSA devices via `snd_device_name_hint(-1, "pcm", &hints)`.
3. Wrap each in an `FFmpegAudioCaptureDevice`.

**`enumerateAudioPlaybackDevices()`**:
1. PulseAudio: `pa_context_get_sink_info_list` for playback sinks.
2. ALSA fallback: `snd_device_name_hint` filtered for output devices.

### 2.4 Capture Sessions

**`FFmpegAudioCaptureSession`**:
```
                        ┌──────────────┐
  ALSA/Pulse device ──> │ AVFormatCtx  │ ──> AVPacket ──> MediaOutputStream
                        │ (input fmt)  │         │
                        └──────────────┘    (optional)
                                              AVCodecCtx (encode to AAC/FLAC)
```

- Open input device as `AVFormatContext` with `avformat_open_input(ctx, devicePath, alsa_fmt, NULL)`.
- Read packets in a dedicated thread via `av_read_frame`.
- For preview: decode packets to PCM, feed to playback device via `AudioPlaybackDevice`.
- For record: optionally encode (if output format requires it) and write to `MediaOutputStream`.

**`FFmpegVideoCaptureSession`**:
- Open V4L2 device as `AVFormatContext`.
- Read raw video frames, convert via `sws_scale` to RGBA.
- Push to `VideoFrameSink` for preview.
- For record: encode via `AudioVideoProcessor` and mux audio+video to `MediaOutputStream` using `avformat_write_header` / `av_interleaved_write_frame`.

### 2.5 Playback Sessions

**`FFmpegPlaybackDispatchQueue`**:
- A `std::thread` with a `std::condition_variable` that processes a vector of `Client` entries (same model as the macOS implementation).
- Each client has a demux context (`AVFormatContext`), a decode context, and a sink.
- The loop reads packets, decodes, and dispatches to the appropriate sink with presentation-time synchronization.

**`MediaInputStream` / `MediaOutputStream` consumption** (depends on Phase 1b):

The FFmpeg backend needs helpers analogous to the macOS/WMF ones. These bridge from the Phase 1b-populated struct fields to FFmpeg I/O contexts:

```cpp
// File mode:  avformat_open_input(ctx, stream.file.c_str(), nullptr, nullptr)
// Buffer mode: create a custom AVIOContext via avio_alloc_context() backed by stream.buffer.data/length
AVFormatContext *openMediaInputStream(MediaInputStream &stream);

// File mode:  avformat_alloc_output_context2(ctx, nullptr, nullptr, stream.file.c_str())
// Buffer mode: custom AVIOContext for writing to stream.buffer
AVFormatContext *openMediaOutputStream(MediaOutputStream &stream, const char *formatName);
```

**`FFmpegAudioPlaybackSession`**:
1. `setAudioSource`: Open `MediaInputStream` via `openMediaInputStream()`. Find the audio stream via `avformat_find_stream_info`.
2. `setAudioPlaybackDevice`: Open a PulseAudio/ALSA output stream matching the audio format.
3. `start`: Register with the `PlaybackDispatchQueue`. The queue thread reads audio packets, decodes to PCM, and writes to the output device via `pa_simple_write` (PulseAudio) or `snd_pcm_writei` (ALSA).
4. `pause` / `reset`: Signal the queue to stop/remove the client.

**`FFmpegVideoPlaybackSession`**:
1. `setVideoSource`: Open `MediaInputStream` as `AVFormatContext`. Find both audio and video streams.
2. `setVideoFrameSink`: Store the sink pointer for frame delivery.
3. `start`: Register with the `PlaybackDispatchQueue`. The queue thread:
   - Demuxes packets and routes audio vs. video by stream index.
   - Decodes video frames via `AudioVideoProcessor` (with VA-API if enabled).
   - Converts to RGBA, wraps in `VideoFrame`, pushes to `VideoFrameSink`.
   - Decodes audio and sends to the playback device.
   - Synchronizes to presentation timestamps (sleep until present time, flush if behind).

### 2.6 VA-API Hardware Acceleration Path

When `useHardwareAccel = true`:

1. **Init**: `av_hwdevice_ctx_create` with `AV_HWDEVICE_TYPE_VAAPI`. This opens `/dev/dri/renderD128` (or the DRM node specified by the GTE device).
2. **Decode**: The `AVCodecContext` is configured with `hw_device_ctx`. FFmpeg's VA-API acceleration handles surface allocation internally. Decoded frames arrive as `AV_PIX_FMT_VAAPI` surfaces.
3. **GPU->CPU transfer**: `av_hwframe_transfer_data` copies the VA surface to a CPU-side `AVFrame` in `NV12` or `YUV420P`. Then `sws_scale` converts to RGBA.
4. **Zero-copy path (future)**: If the GTE device is a Vulkan device, VA-API surfaces can be exported as DMA-BUF fds and imported into Vulkan via `VK_EXT_external_memory_dma_buf`. This avoids the GPU->CPU->GPU round-trip. This is an optimization to defer to a later phase.
5. **Encode**: Similar — allocate VA-API encode surfaces, upload CPU frames, encode.

### 2.7 CMake Integration

Add to `wtk/CMakeLists.txt` under the Linux/FFmpeg section:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavcodec
    libavformat
    libavutil
    libswscale
    libswresample
    libavdevice
)
pkg_check_modules(VAAPI REQUIRED libva libva-drm)
pkg_check_modules(PULSE libpulse-simple)  # optional, fallback to ALSA

target_include_directories(omegaWTK PRIVATE ${FFMPEG_INCLUDE_DIRS} ${VAAPI_INCLUDE_DIRS})
target_link_libraries(omegaWTK PRIVATE ${FFMPEG_LIBRARIES} ${VAAPI_LIBRARIES})

if(PULSE_FOUND)
    target_compile_definitions(omegaWTK PRIVATE OMEGA_AUDIO_PULSE=1)
    target_link_libraries(omegaWTK PRIVATE ${PULSE_LIBRARIES})
else()
    find_package(ALSA REQUIRED)
    target_link_libraries(omegaWTK PRIVATE ALSA::ALSA)
endif()
```

---

## Phase 3: macOS (AVFoundation) — Complete the Gaps

### 3.1 `AudioVideoProcessor`

The class is currently empty. Implement using VideoToolbox for hardware encode/decode:

**`AVFAudioVideoProcessor.h`**:
```cpp
class AudioVideoProcessor {
    VTCompressionSessionRef compressionSession = nullptr;
    VTDecompressionSessionRef decompressionSession = nullptr;
    bool useHardwareAccel;
    void *gteDevice;   // MTLDevice* for Metal texture interop

public:
    explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice);
    ~AudioVideoProcessor();

    void setEncodeCodec(CMVideoCodecType codec);
    void setDecodeCodec(CMVideoFormatDescriptionRef format);
    void encode(CMSampleBufferRef input, CMSampleBufferRef *output);
    void decode(CMSampleBufferRef input, CVPixelBufferRef *output);
};
```

**Constructor**:
- Store `useHardwareAccel` and `gteDevice` (cast to `id<MTLDevice>`).
- If hardware accel: set `kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder` in session properties.

**`setDecodeCodec`**:
- `VTDecompressionSessionCreate` with the format description.
- Configure output to `kCVPixelFormatType_32BGRA` for `VideoFrame` compatibility.
- If Metal interop desired: set `kCVPixelBufferMetalCompatibilityKey` on the pixel buffer pool.

**`encode`**:
- `VTCompressionSessionCreate` with target codec (H.264 / HEVC).
- `VTCompressionSessionEncodeFrame` -> callback delivers encoded `CMSampleBufferRef`.

**`decode`**:
- `VTDecompressionSessionDecodeFrame` -> callback delivers `CVPixelBufferRef`.
- Lock the pixel buffer, copy to `VideoFrame.videoFrame.data`, set header fields.

### 3.2 `VideoPlaybackSession::Create`

Currently returns `nullptr`. Wire it to `AVFVideoPlaybackSession` (which already exists but is disconnected):

```objc
SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(
        AudioVideoProcessorRef processor, PlaybackDispatchQueueRef dispatchQueue) {
    return SharedHandle<VideoPlaybackSession>(
        new AVFVideoPlaybackSession(processor, dispatchQueue));
}
```

### 3.3 `AVFVideoCaptureSession` — Fill Empty Methods

The `start/stop/record/preview` methods are empty shells. Implement:
- `startPreview`: `[captureSession startRunning]`, set preview flag.
- `startRecord`: Start the `AVAssetWriter` if buffer output, or the file output already configured.
- `stopRecord` / `stopPreview`: Mirror the pattern from `AVFAudioCaptureSession`.

### 3.4 `AVFAudioCaptureDevice::createCaptureSession`

Currently returns nothing (missing return statement). Fix:
```objc
UniqueHandle<AudioCaptureSession> createCaptureSession() override {
    auto selfHandle = std::dynamic_pointer_cast<AVFAudioCaptureDevice>(shared_from_this());
    // or simpler: pass raw ptr
    return std::make_unique<AVFAudioCaptureSession>(/* this device */);
}
```
This will require `AVFAudioCaptureDevice` to be made compatible with `shared_from_this` or to restructure the factory to accept a raw pointer.

---

## Phase 4: Windows (WMF) — Finish the Stubs

### 4.1 `AudioVideoProcessor::encodeFrame` / `decodeFrame`

The bodies are empty. Implement:

**`encodeFrame`**:
1. If hardware-accelerated:
   - Submit to the D3D11 video encoder via `ID3D11VideoContext::SubmitDecoderBuffers` (the decoder profile is already partially set up in the constructor).
   - Read back encoded bitstream from output buffer.
2. If software:
   - Activate the MFT via `cpuEncodeTransform->ActivateObject(IID_PPV_ARGS(&transform))`.
   - `transform->ProcessInput(0, sample, 0)`.
   - `transform->ProcessOutput(0, 1, &outputBuffer, &status)`.
   - Return the output sample.

**`decodeFrame`**: Same pattern, using `cpuDecodeTransform` / decode command queue.

### 4.2 `WMFVideoPlaybackSession::start` / `pause` / `reset`

```cpp
void start() override {
    session->Start(nullptr, &p);
}
void pause() override {
    session->Pause();  // Note: currently calls Stop(), should be Pause()
}
void reset() override {
    PropVariantClear(&p);
    session->Stop();
    session->ClearTopologies();
}
```

### 4.3 `WMFVideoCaptureSampleSink::OnSample`

Currently returns `S_OK` without processing. Needs to:
1. Get the buffer from the sample via `pSample->GetBufferByIndex`.
2. Lock, extract pixel data.
3. Wrap in `VideoFrame` with dimensions from the capture format.
4. Push to the `VideoFrameSink`.

### 4.4 Complete the D3D11 decoder setup

The constructor creates the D3D11On12 device and queries the video device, but never finishes creating the decoder. After the `D3D11_VIDEO_DECODER_DESC`:
```cpp
D3D11_VIDEO_DECODER_CONFIG config {};
UINT configCount;
video_dev->GetVideoDecoderConfigCount(&desc, &configCount);
video_dev->GetVideoDecoderConfig(&desc, 0, &config);
video_dev->CreateVideoDecoder(&desc, &config, &decoder);
```

---

## Phase 5: AudioVideoProcessor Public API Alignment

Currently each platform defines `AudioVideoProcessor` differently (WMF uses GUIDs, AVF uses `CMVideoCodecType`, FFmpeg will use `AVCodecID`). Unify the public factory:

### Update `AudioVideoProcessorContext.h`

Add codec-agnostic methods that each backend implements by mapping to native types:

```cpp
class AudioVideoProcessor {
public:
    virtual ~AudioVideoProcessor() = default;
    virtual void setVideoEncodeCodec(MediaCodecID codec, const VideoStreamDesc &desc) = 0;
    virtual void setVideoDecodeCodec(MediaCodecID codec, const VideoStreamDesc &desc) = 0;
    virtual void setAudioEncodeCodec(MediaCodecID codec, const AudioStreamDesc &desc) = 0;
    virtual void setAudioDecodeCodec(MediaCodecID codec, const AudioStreamDesc &desc) = 0;
};
```

Each platform's `AudioVideoProcessor` becomes a concrete subclass. The existing `createAudioVideoProcessor` factory already returns `UniqueHandle<AudioVideoProcessor>`, so consumer code stays unchanged.

---

## Implementation Order

1. **Phase 0**: Add types to `MediaIO.h`. No functional change, just new declarations.
2. **Phase 1**: Fix the semicolon. One character.
3. **Phase 1b**: Create `MediaIO.cpp` with factory method bodies. Unblocks linking for any code that constructs streams.
4. **Phase 2**: Linux/FFmpeg — the largest piece, no existing code to break.
5. **Phase 3**: macOS — filling gaps in existing code. Higher risk of regressions.
6. **Phase 4**: Windows — finishing partially-written code.
7. **Phase 5**: API unification — refactor after all three backends work.

Phases 0, 1, and 1b are prerequisites — they must land first. Phases 2, 3, and 4 are independent and can be developed in parallel on separate platform branches.

---

## Dependencies

| Platform | External Libraries |
|---|---|
| Linux | libavcodec, libavformat, libavutil, libswscale, libswresample, libavdevice, libva, libva-drm, libpulse-simple (or ALSA) |
| macOS | AVFoundation, AVFAudio, CoreVideo, VideoToolbox, CoreMedia (all system frameworks, already imported) |
| Windows | Media Foundation, D3D11, D3D12 (already linked via pragma comments) |
