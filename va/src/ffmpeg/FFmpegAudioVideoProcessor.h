#ifndef OMEGAVA_FFMPEG_FFMPEGAUDIOVIDEOPROCESSOR_H
#define OMEGAVA_FFMPEG_FFMPEGAUDIOVIDEOPROCESSOR_H

#include "omegaVA/AudioVideoProcessorContext.h"
#include "omegaVA/MediaIO.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace OmegaVA {

    // Media-API-Completion-Plan §2.2 / Phase 5 deferred.
    // Concrete FFmpeg-side processor. The public surface in
    // AudioVideoProcessorContext.h is currently un-virtualized (Phase 5
    // unifies it across backends); each backend defines its own concrete
    // OmegaVA::AudioVideoProcessor, and the sessions that live
    // alongside the implementation call its methods directly.
    //
    // Hardware acceleration: the plan's preferred path is Vulkan Video
    // via OmegaGTE's shared VkDevice (§2.6). That depends on the
    // GTE-side enablement work in Vulkan-Video-Encode-Decode-Plan and
    // is intentionally NOT implemented here yet. `useHardwareAccel =
    // true` is honoured by logging a one-time warning and falling
    // through to software, matching §2.6.7.
    class AudioVideoProcessor {
    public:
        explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice);
        ~AudioVideoProcessor();

        AudioVideoProcessor(const AudioVideoProcessor &) = delete;
        AudioVideoProcessor & operator=(const AudioVideoProcessor &) = delete;

        // ─── Video ────────────────────────────────────────────────
        void setVideoEncodeCodec(MediaCodecID codec, const VideoStreamDesc & desc);
        void setVideoDecodeCodec(MediaCodecID codec, const VideoStreamDesc & desc);
        bool encodeVideoFrame(const VideoFrame & input, MediaBuffer & output);
        bool decodeVideoFrame(const MediaBuffer & input, VideoFrame & output);

        // Decode an already-demuxed AVPacket directly into a VideoFrame.
        // The playback session has the AVFormatContext and hands us
        // packets without round-tripping through MediaBuffer.
        bool decodeVideoPacket(AVPacket * packet, VideoFrame & output);
        // Inverse — submit a pre-built AVFrame straight to the encoder.
        // Used by the capture-record path.
        bool encodeVideoAVFrame(AVFrame * frame, MediaBuffer & output);

        // ─── Audio ────────────────────────────────────────────────
        void setAudioEncodeCodec(MediaCodecID codec, const AudioStreamDesc & desc);
        void setAudioDecodeCodec(MediaCodecID codec, const AudioStreamDesc & desc);
        bool encodeAudio(const AudioSample & input, MediaBuffer & output);
        bool decodeAudio(const MediaBuffer & input, AudioSample & output);

        // Decode an AVPacket of audio into PCM. Output `data` is owned by
        // the processor and remains valid until the next decodeAudio*
        // call (same lifetime contract macOS uses for its CMBlockBuffer
        // pool).
        bool decodeAudioPacket(AVPacket * packet, AudioSample & output);

        // Direct access for sessions in the same TU.
        AVCodecContext * decodeVideoContext()  { return decodeVideoCtx; }
        AVCodecContext * decodeAudioContext()  { return decodeAudioCtx; }
        AVCodecContext * encodeVideoContext()  { return encodeVideoCtx; }
        AVCodecContext * encodeAudioContext()  { return encodeAudioCtx; }

        // Configure the decoders from an FFmpeg-side AVCodecParameters
        // block (e.g. AVStream::codecpar). Used by the playback sessions
        // where the demuxer already knows the codec/profile — avoids the
        // lossy MediaCodecID round-trip.
        bool openVideoDecoderFromParams(AVCodecParameters * codecpar);
        bool openAudioDecoderFromParams(AVCodecParameters * codecpar);

    private:
        bool useHardwareAccel = false;
        void *gteDevice = nullptr;

        AVCodecContext * encodeVideoCtx = nullptr;
        AVCodecContext * decodeVideoCtx = nullptr;
        AVCodecContext * encodeAudioCtx = nullptr;
        AVCodecContext * decodeAudioCtx = nullptr;

        // Lazy — built on first use to match output dimensions/format.
        SwsContext * decodeVideoSws = nullptr;
        SwsContext * encodeVideoSws = nullptr;
        SwrContext * decodeAudioSwr = nullptr;
        SwrContext * encodeAudioSwr = nullptr;

        // Reused per call to avoid alloc churn.
        AVFrame * scratchFrame = nullptr;
        AVPacket * scratchPacket = nullptr;

        // PCM buffer that decodeAudio* returns into. Grown as needed;
        // freed in the destructor.
        std::uint8_t * pcmBuffer = nullptr;
        std::size_t pcmBufferSize = 0;

        bool warnedHardwareAccel = false;
    };

} // namespace OmegaVA

#endif
