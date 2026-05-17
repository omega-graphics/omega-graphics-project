# Media API Completion Plan

> **Status snapshot (2026-05-17)** — macOS backend essentially complete. WMF backend now: audio playback complete; video playback (start/pause/reset + topology), processor (MFT-driven encode/decode), and capture sample sink (OnSample → VideoFrame) all wired today. Half-built D3D11 decoder removed in favor of an MFT-only path. FFmpeg backend untouched. Phase 1b also landed today.

## Current State Assessment

### What exists and works
- **Image codecs** (PNG, JPEG, TIFF): Fully implemented cross-platform via libpng, turbojpeg, libtiff.
- **MediaIO.h**: Struct declarations for `MediaBuffer`, `MediaInputStream`, `MediaOutputStream`, plus Phase 0 type additions (`AudioSampleFormat`, `AudioStreamDesc`, `PixelFormat`, `MediaCodecID`, `VideoStreamDesc`, `ContainerFormat`, `MediaSourceDesc`). `AudioSample` lives in `AudioVideoProcessorContext.h`. ✅ **Phase 0 complete** (note: enum members landed as `PlanarS16` / `PlanarFloat32` / `FlacContainer` rather than the underscored forms originally drafted).
- **Interface headers**: `Audio.h`, `Video.h`, `AudioVideoProcessorContext.h`, `MediaPlaybackSession.h` define the public API surface for capture, playback, and processing.
- **macOS (AVFoundation)**: Device enumeration, audio + video capture sessions (incl. start/stop/preview/record), audio + video playback sessions, `AudioVideoProcessor` (video encode/decode via VideoToolbox), and `PlaybackDispatchQueue` are all implemented.
- **Windows (WMF)**: Device enumeration, audio + video capture sessions, and **audio** playback session (full IMFMediaSession topology with start/pause/reset) are implemented.

### What is stubbed (no implementation file)
- ~~`MediaInputStream` / `MediaOutputStream` factory methods~~ — ✅ Resolved. `wtk/src/Media/MediaIO.cpp` now defines `fromFile` / `toFile`; the buffer-path fields and factories were removed from `MediaIO.h`; AVF + WMF backends collapsed to file-only.

### What is incomplete

| Area | macOS (AVFoundation) | Windows (WMF) | Linux (FFmpeg) |
|---|---|---|---|
| **Device Enumeration** | ✅ Done | ✅ Done | ❌ 100% stubbed |
| **Audio Capture** | ✅ Session implemented | ✅ Session implemented | ❌ 100% stubbed |
| **Video Capture** | ✅ Session implemented (start/stop/preview/record filled) | ✅ Session implemented; `WMFVideoCaptureSampleSink::OnSample` extracts buffer → `VideoFrame` → sink; `setVideoFrameSinkForPreview` now calls `AddStream` so the callback actually fires | ❌ 100% stubbed |
| **AudioVideoProcessor** | ✅ Video encode/decode via VideoToolbox (audio path still TBD) | ✅ MFT-driven encode/decode (HW hint via `MFT_ENUM_FLAG_HARDWARE` when `useHardwareAccel`); lazy activation on `setEncodeCodec` / `setDecodeCodec`; D3D11/D3D12 cruft removed | ❌ Empty stub constructor; no encode/decode methods exist |
| **Audio Playback** | ✅ Session implemented | ✅ Session implemented (full topology, start/pause/reset) | ❌ Returns `nullptr` |
| **Video Playback** | ✅ `Create` returns real session; start/pause/reset implemented | ✅ Constructor builds source + video-output nodes connected; `setVideoSource` picks first video stream; `setAudioPlaybackDevice` lazily builds audio branch; `start`/`pause`/`reset` wired to `IMFMediaSession` | ❌ Returns `nullptr` |
| **PlaybackDispatchQueue** | ✅ Threaded loop implemented (note: `useProcessor` branch is an empty block) | ⚠ MF work queue wrapper allocated but never consumed | ❌ Returns `nullptr` |
| **MediaIO factories** | ✅ `MediaIO.cpp` defines `fromFile` / `toFile`; AVF backend file-only | ✅ Same; WMF backend file-only | ❌ No helpers yet (Phase 2.5) |

### Critical bugs
- ~~**`MediaPlaybackSession.h:39`** — Missing semicolon after `setVideoFrameSink()` declaration.~~ ✅ **Fixed.**
- ~~**`MediaIO.cpp` missing** — `fromFile` / `toFile` declared but undefined.~~ ✅ **Fixed 2026-05-17.**

---

## Phase 0: MediaIO.h Type Expansion — ✅ COMPLETE

> Landed. `MediaIO.h` now defines `AudioSampleFormat`, `AudioStreamDesc`, `PixelFormat`, `MediaCodecID`, `VideoStreamDesc`, `ContainerFormat`, and `MediaSourceDesc`. `AudioSample` lives in `AudioVideoProcessorContext.h` next to `VideoFrame`. Naming differences from the original draft: `Planar_S16` → `PlanarS16`, `Planar_Float32` → `PlanarFloat32`, `FLAC_Container` → `FlacContainer`, and an `Unknown` member was added to each enum. The `VideoStreamDesc::pixelFormat` field uses the new `PixelFormat` enum (not `BitmapImage::ColorFormat`).

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

## Phase 1: Fix the Build Break — ✅ COMPLETE

> Semicolon added at `MediaPlaybackSession.h:39`. Header now compiles.

**File**: `wtk/include/omegaWTK/Media/MediaPlaybackSession.h`

Line 39 — add the missing semicolon:
```cpp
// Before:
INTERFACE_METHOD void setVideoFrameSink(VideoFrameSink & sink) ABSTRACT

// After:
INTERFACE_METHOD void setVideoFrameSink(VideoFrameSink & sink) ABSTRACT;
```

---

## Phase 1b: MediaInputStream / MediaOutputStream — Simplify to File-Only & Implement — ✅ COMPLETE

> Landed 2026-05-17. `MediaIO.h` structs now only carry `file` + the static factory. `wtk/src/Media/MediaIO.cpp` (auto-picked-up by the existing `file(GLOB MEDIA_SRCS …)`) defines `MediaInputStream::fromFile` and `MediaOutputStream::toFile`. AVF helpers `createURLFromMediaInputStream` / `createURLFromMediaOutputStream` collapsed to a single `fileURLWithFileSystemRepresentation` call. WMF helpers `createMFByteStreamMediaInputStream` / `createMFByteStreamMediaOutputStream` collapsed to a single `MFCreateFile` call; `setAudioOutputStream` / `setVideoOutputStream` on both AVF and WMF now drop the buffer branch entirely.

### Problem

`MediaInputStream` and `MediaOutputStream` currently declare `bufferOrFile`, `file`, and `buffer` fields plus four factory methods (`fromFile`, `fromBuffer`, `toFile`, `toBuffer`), but **no implementation file exists** — every call site fails at link time. The buffer option is a waste of memory: media I/O is inherently file-based (or URL-based on macOS), and holding raw byte buffers in these structs duplicates data that should live in the codec/processor layer (`MediaBuffer` already serves that role).

### Struct changes in `MediaIO.h`

Remove `bufferOrFile`, `buffer`, `fromBuffer()`, and `toBuffer()`. The structs become file-path wrappers:

```cpp
struct MediaInputStream {
    OmegaCommon::String file;
    static MediaInputStream fromFile(const OmegaCommon::FS::Path & path);
};

struct MediaOutputStream {
    OmegaCommon::String file;
    static MediaOutputStream toFile(const OmegaCommon::FS::Path & path);
};
```

### Backend helper cleanup

Every platform backend currently branches on `bufferOrFile`. With the buffer path removed, these helpers simplify to file-only:

| Platform | Helper | Change |
|---|---|---|
| macOS | `createURLFromMediaInputStream()` | Remove `if(bufferOrFile)` branch. Always use `[NSURL fileURLWithFileSystemRepresentation:stream.file.data() ...]` |
| macOS | `createURLFromMediaOutputStream()` | Same — remove buffer branch |
| Windows | `createMFByteStreamMediaInputStream()` | Remove `SHCreateMemStream` branch. Always use `MFCreateFile` |
| Windows | `createMFByteStreamMediaOutputStream()` | Same — remove `SHCreateMemStream` branch |
| Linux | `openMediaInputStream()` (new) | File-only: `avformat_open_input(ctx, stream.file.c_str(), ...)` |
| Linux | `openMediaOutputStream()` (new) | File-only: `avformat_alloc_output_context2(ctx, ..., stream.file.c_str())` |

### Implementation

**New file**: `wtk/src/Media/MediaIO.cpp`

Platform-agnostic. CMake `file(GLOB MEDIA_SRCS "${OMEGAWTK_SOURCE_DIR}/src/Media/*.cpp")` auto-picks it up.

```cpp
#include "omegaWTK/Media/MediaIO.h"

namespace OmegaWTK::Media {

    MediaInputStream MediaInputStream::fromFile(const OmegaCommon::FS::Path & path) {
        MediaInputStream s;
        s.file = path.string();
        return s;
    }

    MediaOutputStream MediaOutputStream::toFile(const OmegaCommon::FS::Path & path) {
        MediaOutputStream s;
        s.file = path.string();
        return s;
    }

}
```

### Call sites that will link once implemented

| File | Line | Call |
|---|---|---|
| `wtk/tests/VideoViewPlaybackTest/main.cpp` | 46 | `MediaInputStream::fromFile(filePath)` |
| `wtk/src/UI/VideoView.cpp` | 110+ | Receives `MediaInputStream &` (constructed by caller) |
| `wtk/src/UI/VideoView.cpp` | 168+ | Receives `MediaOutputStream &` (constructed by caller) |

### Notes

- **`MediaBuffer` is not removed** — it remains in `MediaIO.h` for the `AudioVideoProcessor` encode/decode API, where it represents compressed codec data. It is no longer used by the stream structs.
- **`OmegaCommon::FS::Path`**: `path.string()` converts to `OmegaCommon::String` (`std::string`). macOS converts via `file.data()` → `fileURLWithFileSystemRepresentation`; WMF converts via `cpp_to_wstring`.
- **Future**: If in-memory source support is ever needed (e.g., streaming from a network buffer), it should be handled at the platform backend level (custom `AVIOContext` for FFmpeg, `SHCreateMemStream` for WMF) rather than baked into the stream struct.

---

## Phase 2: Linux (FFmpeg + VA-API) — Full Implementation — ❌ NOT STARTED

> No progress. `FFmpegAudioVideoProcessor` is still an empty class with no encode/decode methods. `FFmpegAudioVideoCapture.cpp` has no device-enumeration implementations. `FFmpegMediaPlaybackStubs.cpp` still returns `nullptr` from `AudioPlaybackSession::Create`, `VideoPlaybackSession::Create`, and `createPlaybackDispatchQueue`. Entire phase below remains the to-do list.

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

The FFmpeg backend needs helpers analogous to the macOS/WMF ones. These bridge from the stream's `file` field to FFmpeg I/O contexts:

```cpp
// Opens the file path stored in stream.file as an AVFormatContext for reading
AVFormatContext *openMediaInputStream(MediaInputStream &stream);
// avformat_open_input(ctx, stream.file.c_str(), nullptr, nullptr)

// Opens the file path stored in stream.file as an AVFormatContext for writing
AVFormatContext *openMediaOutputStream(MediaOutputStream &stream, const char *formatName);
// avformat_alloc_output_context2(ctx, nullptr, formatName, stream.file.c_str())
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

## Phase 3: macOS (AVFoundation) — Complete the Gaps — ✅ MOSTLY COMPLETE

> All four sub-phases plus AVF audio playback wiring have landed. `AVFAudioPlaybackSession::setAudioSource` now picks the asset's first audio track explicitly, populates the full `PlaybackDispatchQueue::Client` struct (`generator` / `sampleBufferRequest` / `cursor` / `audioRenderer`) — previously left nil, so the queue's audio branch crashed on first iteration — and `start` / `pause` / `reset` drive the `AVSampleBufferRenderSynchronizer` rate (without which the renderer just buffered samples instead of producing output). Destructor also unregisters the client.
>
> Only audio encode/decode on the AVF processor is still unaddressed — current `AVFAudioVideoProcessor` is video-only (VideoToolbox `VTCompressionSession` / `VTDecompressionSession`). Audio path through the processor remains a future addition; audio playback is now end-to-end functional via the dispatch queue.

### 3.1 `AudioVideoProcessor` — ✅ Implemented (video only)

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

### 3.2 `VideoPlaybackSession::Create` — ✅ Done

Currently returns `nullptr`. Wire it to `AVFVideoPlaybackSession` (which already exists but is disconnected):

```objc
SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(
        AudioVideoProcessorRef processor, PlaybackDispatchQueueRef dispatchQueue) {
    return SharedHandle<VideoPlaybackSession>(
        new AVFVideoPlaybackSession(processor, dispatchQueue));
}
```

### 3.3 `AVFVideoCaptureSession` — Fill Empty Methods — ✅ Done

The `start/stop/record/preview` methods are empty shells. Implement:
- `startPreview`: `[captureSession startRunning]`, set preview flag.
- `startRecord`: Start the `AVAssetWriter` with the file output already configured.
- `stopRecord` / `stopPreview`: Mirror the pattern from `AVFAudioCaptureSession`.

### 3.4 `AVFAudioCaptureDevice::createCaptureSession` — ✅ Done

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

## Phase 4: Windows (WMF) — Finish the Stubs — ✅ MOSTLY COMPLETE

> Landed 2026-05-17 via the **MFT-only with HW hint** architectural cut (Phase 4.1 + 4.4 collapsed into a single path; the half-built D3D11 video-decoder code in the constructor was deleted, not finished). Capture sample sink (4.3) and video playback start/pause/reset + topology (4.2) also wired today. Two open gaps documented below: color-format negotiation in 4.3, and the codec-pair API in 4.1 doesn't carry width/height/framerate.

### 4.1 `AudioVideoProcessor::encodeFrame` / `decodeFrame` — ✅ MFT-driven

> Done. `WMFAudioVideoProcessor.cpp` rewrite: shared `activateMFTForCodecPair` helper calls `MFTEnumEx` with `MFT_ENUM_FLAG_HARDWARE` (when `useHardwareAccel`) or `MFT_ENUM_FLAG_SYNCMFT`, plus `MFT_ENUM_FLAG_SORTANDFILTER`. First match is `ActivateObject`-ed into `encodeMFT` / `decodeMFT`, and the input/output media types are configured from the codec subtype pair. `encodeFrame` / `decodeFrame` drive `ProcessInput` then a single `ProcessOutput` with `MFT_OUTPUT_DATA_BUFFER::pSample = nullptr` so the MFT allocates the output sample. `MF_E_TRANSFORM_NEED_MORE_INPUT` is handled by returning `*output = nullptr`.
>
> **Open gap:** the public `setEncodeCodec(from, to)` / `setDecodeCodec(from, to)` API only carries codec subtypes. Width / height / framerate aren't part of the negotiation — the MFT's defaults are used. A richer API (taking `VideoStreamDesc`) is part of Phase 5.

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

### 4.2 `WMFVideoPlaybackSession::start` / `pause` / `reset` — ✅ Done

> Constructor now creates `sourceNode` + `videoOutputNode`, adds them to the topology, and connects `source[0] → video[0]`. `setAudioPlaybackDevice` lazily builds a second source-stream + audio-output node pair the first time it's called, finds the first audio stream descriptor on the presentation descriptor, and wires it through. `setVideoSource` picks the first video stream (falling back to stream 0) instead of blindly using index 0. `start` calls `SetTopology` once (gated by a `topologyDirty` flag) then `IMFMediaSession::Start`; `pause` calls `Pause`; `reset` clears the position propvariant and calls `Stop` + `ClearTopologies`.

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

### 4.3 `WMFVideoCaptureSampleSink::OnSample` — ✅ Done (with one open gap)

> `OnSample` now: `ConvertToContiguousBuffer` → `Lock` → wrap bytes as a non-owning `PixelStorage::view` on a `VideoFrame` → push to the sink → `Unlock` + `Release`. Sample timestamp is plumbed into `presentTime`. Sink constructor takes width/height; `QueryInterface` correctly returns the callback interface.
>
> `setVideoFrameSinkForPreview` was also fixed to actually wire the preview path: it now queries `IMFCaptureSource::GetCurrentDeviceMediaType(0, ...)` for the frame size, calls `IMFCapturePreviewSink::AddStream` (without which `SetSampleCallback`'s callback never fires), then `SetSampleCallback` on the returned sink-stream index.
>
> **Open gap (flagged in code):** the frame's `color_format` is hardcoded to RGBA. Devices producing NV12 / YUY2 / RGB32 will deliver bytes whose actual layout disagrees with the label. Proper handling needs either (a) MFT-based conversion in the preview pipeline or (b) reading the source subtype and translating to `OmegaCommon::Img::ColorFormat`. Tracked as future work.

(Original sketch retained below for reference.)

Currently returns `S_OK` without processing. Needs to:
1. Get the buffer from the sample via `pSample->GetBufferByIndex`.
2. Lock, extract pixel data.
3. Wrap in `VideoFrame` with dimensions from the capture format.
4. Push to the `VideoFrameSink`.

### 4.4 Complete the D3D11 decoder setup — ✅ Resolved (deleted, not completed)

> Per the **MFT-only with HW hint** architectural decision, the D3D11on12 device, `ID3D12CommandQueue` decode/encode queues, `D3D11_VIDEO_DECODER_DESC`, and related fields were removed from `WMFAudioVideoProcessor.h` and `WMFAudioVideoProcessor.cpp`. The MFT path delegates HW selection to Media Foundation's transform enumeration, so the half-built D3D11 video-decoder code is no longer needed. (Original sketch retained below for reference.)

The constructor creates the D3D11On12 device and queries the video device, but never finishes creating the decoder. After the `D3D11_VIDEO_DECODER_DESC`:
```cpp
D3D11_VIDEO_DECODER_CONFIG config {};
UINT configCount;
video_dev->GetVideoDecoderConfigCount(&desc, &configCount);
video_dev->GetVideoDecoderConfig(&desc, 0, &config);
video_dev->CreateVideoDecoder(&desc, &config, &decoder);
```

---

## Phase 5: AudioVideoProcessor Public API Alignment — ❌ NOT STARTED

> Deliberately deferred until all three backends exist. AVF currently exposes `SetEncodeMode` / `SetDecodeMode` / `Encode` / `Decode` taking native CM types; WMF and FFmpeg have no real methods yet. Unification still pending.

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

1. ~~**Phase 0**: Add types to `MediaIO.h`.~~ ✅ Done.
2. ~~**Phase 1**: Fix the semicolon.~~ ✅ Done.
3. ~~**Phase 1b**: Create `MediaIO.cpp` with factory method bodies.~~ ✅ Done 2026-05-17.
4. **Phase 2**: Linux/FFmpeg — the largest piece, no existing code to break. ❌ Not started.
5. ~~**Phase 3**: macOS — filling gaps in existing code.~~ ✅ Done (audio-through-processor path is the only remaining piece, see Phase 3 note).
6. ~~**Phase 4**: Windows — finishing partially-written code.~~ ✅ Done 2026-05-17 (4.1 + 4.4 collapsed via MFT-only cut; 4.2 + 4.3 wired). Two open gaps documented: capture color-format negotiation and codec-pair API width/height/framerate.
7. **Phase 5**: API unification — refactor after all three backends work. ❌ Deferred until Phase 2 lands.

### Remaining critical path
1. **Phase 2** (FFmpeg backend — largest remaining piece).
2. **Phase 5** once Phase 2 ships.
3. Address WMF open gaps (color format, richer codec API) opportunistically as part of Phase 5.

---

## Dependencies

| Platform | External Libraries |
|---|---|
| Linux | libavcodec, libavformat, libavutil, libswscale, libswresample, libavdevice, libva, libva-drm, libpulse-simple (or ALSA) |
| macOS | AVFoundation, AVFAudio, CoreVideo, VideoToolbox, CoreMedia (all system frameworks, already imported) |
| Windows | Media Foundation, D3D11, D3D12 (already linked via pragma comments) |

---

## Phase 6: Extract Media → OmegaVA (`<repo>/va`)

> **Scope:** lift the entire Media subsystem out of `wtk/` and stand it up as a sibling top-level module called **OmegaVA** (audio/video), so the dependency graph becomes `OmegaVA → OmegaCommon` and `OmegaWTK → OmegaVA` instead of `OmegaWTK_Media` being an internal WTK library. Goal: Media can be consumed by non-UI code (CLI tools, server-side transcoding, AUTOM build artifacts) without pulling in WTK's Composition / Native / Widgets stacks.
>
> **Non-goals:** changing the public API surface, rewriting any backend, or absorbing Composition/Native code into OmegaVA. The extraction is purely a relocation + decoupling exercise.

### 6.0 Coupling audit (today, pre-extraction)

`grep` across `wtk/src`, `wtk/include`, `wtk/tests` identifies every WTK ↔ Media touch point:

| Direction | File | What it pulls |
|---|---|---|
| WTK → Media (legitimate consumer) | `wtk/include/omegaWTK/UI/VideoView.h` | Includes `omegaWTK/Media/MediaIO.h`, `MediaPlaybackSession.h`; `VideoView` implements `VideoFrameSink` |
| WTK → Media (legitimate consumer) | `wtk/src/UI/VideoView.cpp` | Takes `MediaInputStream &` / `MediaOutputStream &`, owns playback session |
| WTK → Media (test) | `wtk/tests/VideoViewPlaybackTest/main.cpp` | `MediaInputStream::fromFile` |
| ⚠ **Media → WTK (layering violation)** | `wtk/src/Media/wmf/WMFAudioVideoCapture.cpp:160,163,628,736` | Uses `OmegaWTK::Composition::Rect` / `Composition::Point2D` to carry frame dimensions through `WMFVideoSampleGrabber` and `WMFVideoPlaybackSession::frameRect` |
| Cross-module shared | `Media/AudioVideoProcessorContext.h` | `VideoFrameSink` interface — used by Media internally, implemented by WTK |
| Mobile path | `wtk/src/Mobile/Media/android/AndroidAudioVideoCapture.cpp` | Android backend, currently nested under WTK Mobile |

The Composition::Rect leak is the only blocker to a clean extraction. Everything else is either a one-way `WTK depends on Media` arrow (which inverts cleanly once OmegaVA exists) or shared infrastructure (`OmegaCommon`, `OmegaCommon::Img::BitmapImage`) that both modules can keep depending on.

### 6.1 Module graph

**Before:**
```
OmegaCommon ───┬─→ OmegaWTK_Core ─→ OmegaWTK_Native ─→ OmegaWTK_Composition ─→ OmegaWTK_UI ─→ OmegaWTK_Widgets
               └─→ OmegaWTK_Media (internal to wtk/)            ▲
                                                                │ (Composition::Rect leak — to be removed)
                                                                │
OmegaGTE ─────────────────────────────────────────────→ OmegaWTK_Composition
```

**After:**
```
OmegaCommon ─→ OmegaVA  (new top-level module under va/)
                  │
                  ▼
OmegaCommon ─→ OmegaWTK_Core ─→ ... ─→ OmegaWTK_UI ─→ OmegaWTK_Widgets
                                          │
                                          └─→ links OmegaVA (consumes MediaInputStream / VideoPlaybackSession)
```

### 6.2 Open architectural decisions (defer to user before executing)

These shape downstream work — flag for explicit confirmation before any file is moved:

1. **Namespace.** Three plausible options:
   - **`OmegaVA::*`** (parallel to `OmegaWTK`, `OmegaGTE`, `OmegaCommon` — strongest "this is its own product" signal).
   - **`OmegaWTK::Media::*` retained** as the public surface, with OmegaVA being only the *build-system* module name (zero source churn outside CMakeLists; weakest decoupling signal).
   - **`OmegaMedia::*`** (matches the descriptive product name but breaks the `OmegaVA` naming the user picked).
2. **Public include path.** `va/include/omegaVA/Audio.h`, `va/include/omegaVA/Media/Audio.h`, or keep `omegaWTK/Media/Audio.h` paths intact via symlink/install rules?
3. **VideoFrameSink ownership.** Stays in OmegaVA as a pure-virtual interface that WTK implements (clean), or moves to WTK and OmegaVA references it via a forward decl (looser, but then OmegaVA's `VideoPlaybackSession` has no in-tree sink consumer)?
4. **Composition::Rect leak.** Replace WMF backend's use with a plain `OmegaVA::FrameSize {uint32_t w, h}` struct (Phase 6.3 below assumes this), or pull `Composition::Rect`/`Point2D` out into `OmegaCommon::Geometry` so both modules share a common type?
5. **Android mobile backend.** Move `wtk/src/Mobile/Media/android/` under `va/src/android/`, or leave the Android shim inside `wtk/Mobile/` and have it link OmegaVA? (Mobile glue arguably belongs with the rest of WTK Mobile, but the codec backend belongs with OmegaVA.)
6. **Two build systems.** CMake + AUTOM both need to be updated. Land both in lockstep, or stage CMake first and let AUTOM catch up?

Phase 6.3 onwards assumes a baseline of: **namespace `OmegaVA`, include path `omegaVA/Media/...`, VideoFrameSink stays in OmegaVA, Composition::Rect leak removed in favor of `OmegaVA::FrameSize`, Android stays under WTK Mobile but links OmegaVA, CMake + AUTOM updated together.** Any of those can be re-cut without changing the file moves.

### 6.3 Decoupling preflight (must land before any file moves)

- **Composition::Rect → OmegaVA::FrameSize.** Add a `struct FrameSize { uint32_t width; uint32_t height; };` to `Media/AudioVideoProcessorContext.h` (which already carries `VideoFrame` and `VideoFrameSink`). Replace all four use sites in `WMFAudioVideoCapture.cpp` (`WMFVideoSampleGrabber::frameRect`, `WMFVideoPlaybackSession::frameRect`, the assignment after `MFGetAttributeSize`, and the constructor parameter). This is a self-contained PR that should be verifiable on its own — no Media public surface changes.
- **Search for any other `OmegaWTK::*` references inside `wtk/src/Media/` and `wtk/include/omegaWTK/Media/`.** As of 2026-05-17 the only hits are `Composition::Rect` / `Point2D`; if Phase 2 / 4 follow-up work adds more, list them here and resolve before extraction.

### 6.4 Target directory layout

```
<repo>/
  va/
    AUTOM.build                  (mirrors common/AUTOM.build / wtk/AUTOM.build)
    AUTOMDEPS
    CMakeLists.txt
    README.md
    include/
      omegaVA/
        Media/                   (matches existing relative path inside the headers)
          Audio.h
          Video.h
          MediaIO.h
          MediaPlaybackSession.h
          AudioVideoProcessorContext.h
    src/
      Media/
        MediaIO.cpp
        avf/    AVFAudioVideoCapture.mm, AVFAudioVideoProcessor.{h,mm}
        wmf/    WMFAudioVideoCapture.cpp, WMFAudioVideoProcessor.{h,cpp}
        ffmpeg/ FFmpegAudioVideoCapture.cpp, FFmpegAudioVideoProcessor.{h,cpp}, FFmpegMediaPlaybackStubs.cpp
    docs/
      Media-API-Completion-Plan.md     (this file, moved out of wtk/docs/)
    tests/
      (TBD — split VideoViewPlaybackTest into a headless OmegaVA test
       that doesn't need VideoView, plus the existing WTK-side integration test)
```

### 6.5 File move table

| Current path | New path |
|---|---|
| `wtk/include/omegaWTK/Media/Audio.h` | `va/include/omegaVA/Media/Audio.h` |
| `wtk/include/omegaWTK/Media/Video.h` | `va/include/omegaVA/Media/Video.h` |
| `wtk/include/omegaWTK/Media/MediaIO.h` | `va/include/omegaVA/Media/MediaIO.h` |
| `wtk/include/omegaWTK/Media/MediaPlaybackSession.h` | `va/include/omegaVA/Media/MediaPlaybackSession.h` |
| `wtk/include/omegaWTK/Media/AudioVideoProcessorContext.h` | `va/include/omegaVA/Media/AudioVideoProcessorContext.h` |
| `wtk/src/Media/MediaIO.cpp` | `va/src/Media/MediaIO.cpp` |
| `wtk/src/Media/avf/*` | `va/src/Media/avf/*` |
| `wtk/src/Media/wmf/*` | `va/src/Media/wmf/*` |
| `wtk/src/Media/ffmpeg/*` | `va/src/Media/ffmpeg/*` |
| `wtk/docs/Media-API-Completion-Plan.md` | `va/docs/Media-API-Completion-Plan.md` |
| `wtk/src/Mobile/Media/android/AndroidAudioVideoCapture.cpp` | **Stays under WTK Mobile** (per 6.2 decision); links OmegaVA |

### 6.6 Namespace migration

- `namespace OmegaWTK::Media { ... }` → `namespace OmegaVA::Media { ... }` across all five headers and every backend `.cpp` / `.mm`.
- `#include "omegaWTK/Media/X.h"` → `#include "omegaVA/Media/X.h"` everywhere.
- WTK consumers (`UI/VideoView.h`, `UI/VideoView.cpp`, `tests/VideoViewPlaybackTest/main.cpp`) update both the include and the namespace qualifier.
- Optionally add `wtk/include/omegaWTK/Media/*.h` back-compat shims that `#include "omegaVA/Media/*.h"` and `using namespace OmegaVA::Media;` for one release cycle to give out-of-tree consumers a soft migration window. Default: skip the shims — the migration is internal.

### 6.7 CMake changes

1. **New file `va/CMakeLists.txt`** — modeled on the existing `wtk/CMakeLists.txt` Media section (lines ~186–296):
   - `omega_graphics_project(OmegaVA VERSION 0.1 LANGUAGES C CXX)` (define an OmegaVA version independent of OmegaWTK).
   - `file(GLOB MEDIA_SRCS "${OMEGAVA_SOURCE_DIR}/src/Media/*.cpp")` plus the per-platform globs (`avf/*.mm`, `wmf/*.cpp`, `ffmpeg/*.cpp`).
   - `add_library(OmegaVA STATIC ${MEDIA_SRCS})` — note: drops the `_Media` suffix; this is now the module's primary library.
   - `target_include_directories(OmegaVA PUBLIC ${OMEGAVA_PUBLIC_INCLUDE_DIR} $<TARGET_PROPERTY:OmegaCommon,INCLUDE_DIRECTORIES>)`.
   - `target_link_libraries(OmegaVA PUBLIC OmegaCommon)`. No link against OmegaWTK_* anything.
   - FFmpeg `pkg_check_modules` block moves here; AVFoundation/VideoToolbox frameworks linked via `target_link_libraries` on Apple; WMF link inputs stay as `#pragma comment(lib,...)` inside the backend `.cpp` files (already the case post Phase 4).
2. **Top-level `CMakeLists.txt`** — add `add_subdirectory(va)` *before* `add_subdirectory(wtk)` so WTK can reference the target.
3. **`wtk/CMakeLists.txt`** — remove the `OmegaWTK_Media` `add_library`, `target_include_directories`, and FFmpeg block (lines ~186–296 today). Drop `OmegaWTK_Media` from `OMEGAWTK_SUBMODULE_LIBS`. Add `target_link_libraries(OmegaWTK_UI PUBLIC OmegaVA)` (or wherever VideoView is built) so the WTK framework gets OmegaVA pulled into its `WHOLEARCHIVE` set.
4. **`cmake/OmegaGraphicsSuite.cmake`** — `omega_graphics_project()` already handles per-module setup; no changes needed unless it hardcodes a list of known module names.

### 6.8 AUTOM build changes

The repo also ships AUTOM (`AUTOM.build` / `AUTOMDEPS` at the top level plus per-module). Mirror the CMake changes:
- New `va/AUTOM.build` + `va/AUTOMDEPS` modeled on `common/`.
- Top-level `AUTOM.build` declares the new module dependency before `wtk`.
- `wtk/AUTOM.build` drops the Media-related build targets and adds a dep on `OmegaVA`.
- `OmegaGraphics.autom` (top-level metadata) lists `va/` alongside `common/`, `gte/`, `wtk/`, `autom/`.

If CMake-first staging is chosen (per 6.2 #6), keep `wtk/src/Media/` populated in the AUTOM build until the AUTOM follow-up lands.

### 6.9 Test split

`wtk/tests/VideoViewPlaybackTest` couples Media + WTK UI. After extraction, ideally split into:
- `va/tests/MediaIOSmokeTest` — headless, exercises `MediaInputStream::fromFile`, opens an asset, drives one decoded frame, no UI. Verifies OmegaVA is consumable standalone.
- `wtk/tests/VideoViewPlaybackTest` — stays in WTK, now imports OmegaVA. Same coverage as today.

### 6.10 Migration order

1. **Decoupling preflight** (6.3) — land Composition::Rect → OmegaVA::FrameSize first, verifiable on the current `OmegaWTK_Media` target before any move. Single PR.
2. **Open-decision confirmation** (6.2) — user signs off on namespace / include path / VideoFrameSink / mobile / single-vs-staged build-system.
3. **CMake + source move** — single mechanical PR: move files, rewrite namespace, add `va/CMakeLists.txt`, edit top-level CMakeLists, drop OmegaWTK_Media. Verify all three platform builds (build verification per user preference, before merging).
4. **AUTOM mirror** — if staged, follow up here.
5. **Documentation move** — relocate this plan to `va/docs/`, leave a one-line forwarder in `wtk/docs/` for one release.
6. **Test split** — opportunistic; can ship before or after the move.

### 6.11 Risk & rollback

- **Risk:** the `#pragma comment(lib, ...)` directives inside `WMFAudioVideoCapture.cpp` and `WMFAudioVideoProcessor.cpp` are static-library-local. Once they're inside `OmegaVA.lib`, the linker pulls them when OmegaWTK's framework DLL pass consumes `OmegaVA.lib` via `WHOLEARCHIVE`. The Phase 4 link error (`D3D11On12CreateDevice` undefined, root-caused to a use site free-riding on a pragma in a sibling file) is the cautionary tale: after the move, **scan every TU under WTK for free-riding pragma deps on Media** before declaring the migration done.
- **Rollback:** the entire move is recoverable via `git revert` of the single mechanical PR if integration breaks. The Phase 6.3 preflight is independently useful (it's a real layering fix) and should not be reverted.
- **Out-of-tree consumers:** none known. If discovered, the 6.6 back-compat shim option becomes mandatory rather than optional.
