#include "FFmpegAudioVideoProcessor.h"

#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace OmegaVA {

    namespace {
        // ─── MediaCodecID ↔ AVCodecID ──────────────────────────────
        AVCodecID toAVCodecID(MediaCodecID id) {
            switch(id){
                case MediaCodecID::H264:     return AV_CODEC_ID_H264;
                case MediaCodecID::HEVC:     return AV_CODEC_ID_HEVC;
                case MediaCodecID::VP9:      return AV_CODEC_ID_VP9;
                case MediaCodecID::AV1:      return AV_CODEC_ID_AV1;
                case MediaCodecID::AAC:      return AV_CODEC_ID_AAC;
                case MediaCodecID::MP3:      return AV_CODEC_ID_MP3;
                case MediaCodecID::FLAC:     return AV_CODEC_ID_FLAC;
                case MediaCodecID::Opus:     return AV_CODEC_ID_OPUS;
                case MediaCodecID::PCM:      return AV_CODEC_ID_PCM_S16LE;
                case MediaCodecID::RawVideo: return AV_CODEC_ID_RAWVIDEO;
                case MediaCodecID::RawAudio: return AV_CODEC_ID_PCM_S16LE;
                case MediaCodecID::Unknown:  break;
            }
            return AV_CODEC_ID_NONE;
        }

        AVSampleFormat toAVSampleFormat(AudioSampleFormat fmt) {
            switch(fmt){
                case AudioSampleFormat::S16:           return AV_SAMPLE_FMT_S16;
                case AudioSampleFormat::S32:           return AV_SAMPLE_FMT_S32;
                case AudioSampleFormat::Float32:       return AV_SAMPLE_FMT_FLT;
                case AudioSampleFormat::Float64:       return AV_SAMPLE_FMT_DBL;
                case AudioSampleFormat::PlanarS16:     return AV_SAMPLE_FMT_S16P;
                case AudioSampleFormat::PlanarFloat32: return AV_SAMPLE_FMT_FLTP;
                case AudioSampleFormat::Unknown:       break;
            }
            return AV_SAMPLE_FMT_S16;
        }

        AudioSampleFormat fromAVSampleFormat(AVSampleFormat fmt) {
            switch(fmt){
                case AV_SAMPLE_FMT_S16:  return AudioSampleFormat::S16;
                case AV_SAMPLE_FMT_S32:  return AudioSampleFormat::S32;
                case AV_SAMPLE_FMT_FLT:  return AudioSampleFormat::Float32;
                case AV_SAMPLE_FMT_DBL:  return AudioSampleFormat::Float64;
                case AV_SAMPLE_FMT_S16P: return AudioSampleFormat::PlanarS16;
                case AV_SAMPLE_FMT_FLTP: return AudioSampleFormat::PlanarFloat32;
                default:                 return AudioSampleFormat::Unknown;
            }
        }
    } // namespace

    AudioVideoProcessor::AudioVideoProcessor(bool useHardwareAccel_, void *gteDevice_)
        : useHardwareAccel(useHardwareAccel_), gteDevice(gteDevice_) {
        // libav stays quiet unless something actually breaks. Tests
        // running headless on CI already produce enough noise without
        // every probed-and-rejected codec announcing itself.
        av_log_set_level(AV_LOG_WARNING);

        // Vulkan-Video HW accel is gated on the OmegaGTE-side
        // enablement plan landing first. Until then any HW request
        // degrades to software with a one-time warning (per
        // Media-API-Completion-Plan §2.6.7). We *do* keep the gteDevice
        // pointer around so the future HW path doesn't need an API
        // change at the call site.
        if(useHardwareAccel && !warnedHardwareAccel) {
            std::cerr << "[FFmpegAudioVideoProcessor] useHardwareAccel=true, but Vulkan "
                         "Video HW accel is not yet wired up (depends on "
                         "gte/docs/Vulkan-Video-Encode-Decode-Plan landing). "
                         "Falling back to software decode/encode." << std::endl;
            warnedHardwareAccel = true;
        }

        scratchFrame  = av_frame_alloc();
        scratchPacket = av_packet_alloc();
    }

    AudioVideoProcessor::~AudioVideoProcessor() {
        // Order: codec contexts before sws/swr (they reference frame
        // formats that came from the codec contexts).
        if(encodeVideoCtx) avcodec_free_context(&encodeVideoCtx);
        if(decodeVideoCtx) avcodec_free_context(&decodeVideoCtx);
        if(encodeAudioCtx) avcodec_free_context(&encodeAudioCtx);
        if(decodeAudioCtx) avcodec_free_context(&decodeAudioCtx);

        if(decodeVideoSws) { sws_freeContext(decodeVideoSws); decodeVideoSws = nullptr; }
        if(encodeVideoSws) { sws_freeContext(encodeVideoSws); encodeVideoSws = nullptr; }
        if(decodeAudioSwr) { swr_free(&decodeAudioSwr); }
        if(encodeAudioSwr) { swr_free(&encodeAudioSwr); }

        if(scratchFrame)  av_frame_free(&scratchFrame);
        if(scratchPacket) av_packet_free(&scratchPacket);

        if(pcmBuffer) {
            av_free(pcmBuffer);
            pcmBuffer = nullptr;
            pcmBufferSize = 0;
        }
    }

    // ───────────────────────────────────────────────────────────────
    //  Video
    // ───────────────────────────────────────────────────────────────

    void AudioVideoProcessor::setVideoEncodeCodec(MediaCodecID codec, const VideoStreamDesc & desc) {
        if(encodeVideoCtx) avcodec_free_context(&encodeVideoCtx);
        const AVCodec * c = avcodec_find_encoder(toAVCodecID(codec));
        if(!c) {
            std::cerr << "[FFmpegAudioVideoProcessor] no encoder for codec id "
                      << int(codec) << std::endl;
            return;
        }
        encodeVideoCtx = avcodec_alloc_context3(c);
        encodeVideoCtx->width     = static_cast<int>(desc.width);
        encodeVideoCtx->height    = static_cast<int>(desc.height);
        encodeVideoCtx->time_base = AVRational{
            static_cast<int>(desc.frameRateDen > 0 ? desc.frameRateDen : 1),
            static_cast<int>(desc.frameRateNum > 0 ? desc.frameRateNum : 30) };
        encodeVideoCtx->framerate = AVRational{
            static_cast<int>(desc.frameRateNum > 0 ? desc.frameRateNum : 30),
            static_cast<int>(desc.frameRateDen > 0 ? desc.frameRateDen : 1) };
        // Most encoders only accept YUV420P as their primary input;
        // libswscale handles RGBA→YUV420P on encodeVideoFrame.
        encodeVideoCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        // Modest defaults — callers that care about bitrate / GOP / etc
        // can extend the public API in Phase 5; today the WMF backend
        // similarly leaves these at MFT defaults.
        encodeVideoCtx->bit_rate = 2'000'000;
        encodeVideoCtx->gop_size = 12;

        int err = avcodec_open2(encodeVideoCtx, c, nullptr);
        if(err < 0) {
            char msg[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(err, msg, sizeof(msg));
            std::cerr << "[FFmpegAudioVideoProcessor] avcodec_open2 (video encode) failed: "
                      << msg << std::endl;
            avcodec_free_context(&encodeVideoCtx);
        }
    }

    void AudioVideoProcessor::setVideoDecodeCodec(MediaCodecID codec, const VideoStreamDesc & desc) {
        if(decodeVideoCtx) avcodec_free_context(&decodeVideoCtx);
        const AVCodec * c = avcodec_find_decoder(toAVCodecID(codec));
        if(!c) {
            std::cerr << "[FFmpegAudioVideoProcessor] no decoder for codec id "
                      << int(codec) << std::endl;
            return;
        }
        decodeVideoCtx = avcodec_alloc_context3(c);
        // Width/height/pix_fmt are mostly informational here — the
        // decoder fills them from the bitstream on first packet. We
        // seed them anyway so callers that query before any decode
        // see something sane.
        decodeVideoCtx->width   = static_cast<int>(desc.width);
        decodeVideoCtx->height  = static_cast<int>(desc.height);
        int err = avcodec_open2(decodeVideoCtx, c, nullptr);
        if(err < 0) {
            char msg[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(err, msg, sizeof(msg));
            std::cerr << "[FFmpegAudioVideoProcessor] avcodec_open2 (video decode) failed: "
                      << msg << std::endl;
            avcodec_free_context(&decodeVideoCtx);
        }
    }

    bool AudioVideoProcessor::openVideoDecoderFromParams(AVCodecParameters * codecpar) {
        if(!codecpar) return false;
        if(decodeVideoCtx) avcodec_free_context(&decodeVideoCtx);
        const AVCodec * c = avcodec_find_decoder(codecpar->codec_id);
        if(!c) {
            std::cerr << "[FFmpegAudioVideoProcessor] no decoder for AVCodecID "
                      << int(codecpar->codec_id) << std::endl;
            return false;
        }
        decodeVideoCtx = avcodec_alloc_context3(c);
        if(avcodec_parameters_to_context(decodeVideoCtx, codecpar) < 0) {
            avcodec_free_context(&decodeVideoCtx);
            return false;
        }
        if(avcodec_open2(decodeVideoCtx, c, nullptr) < 0) {
            avcodec_free_context(&decodeVideoCtx);
            return false;
        }
        return true;
    }

    bool AudioVideoProcessor::decodeVideoPacket(AVPacket * packet, VideoFrame & output) {
        if(!decodeVideoCtx) return false;
        int err = avcodec_send_packet(decodeVideoCtx, packet);
        if(err < 0 && err != AVERROR(EAGAIN)) {
            return false;
        }
        err = avcodec_receive_frame(decodeVideoCtx, scratchFrame);
        if(err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Decoder needs more input or stream's done. Caller loops
            // until they hit AVERROR_EOF; surface as "no frame" so the
            // caller can advance to the next packet.
            return false;
        }
        if(err < 0) return false;

        const int w = scratchFrame->width;
        const int h = scratchFrame->height;
        if(w <= 0 || h <= 0) {
            av_frame_unref(scratchFrame);
            return false;
        }

        // Rebuild the sws context if the input dims/pixfmt changed.
        AVPixelFormat srcFmt = static_cast<AVPixelFormat>(scratchFrame->format);
        decodeVideoSws = sws_getCachedContext(decodeVideoSws,
            w, h, srcFmt,
            w, h, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if(!decodeVideoSws) {
            av_frame_unref(scratchFrame);
            return false;
        }

        // Allocate RGBA destination on the BitmapImage's PixelStorage.
        const std::size_t stride = static_cast<std::size_t>(w) * 4;
        const std::size_t byteCount = stride * static_cast<std::size_t>(h);
        output.videoFrame.pixels = OmegaCommon::Img::PixelStorage::allocate(byteCount);

        std::uint8_t * dstPlanes[1] = { output.videoFrame.pixels.data() };
        int dstStrides[1] = { static_cast<int>(stride) };
        sws_scale(decodeVideoSws,
            scratchFrame->data, scratchFrame->linesize, 0, h,
            dstPlanes, dstStrides);

        output.videoFrame.header.width  = static_cast<std::uint32_t>(w);
        output.videoFrame.header.height = static_cast<std::uint32_t>(h);
        output.videoFrame.header.channels = 4;
        output.videoFrame.header.bitDepth = 8;
        output.videoFrame.header.stride = stride;
        output.videoFrame.header.color_format = OmegaCommon::Img::ColorFormat::RGBA;
        output.videoFrame.header.alpha_format = OmegaCommon::Img::AlphaFormat::Straight;
        output.decodeFinishTime = std::chrono::high_resolution_clock::now();

        av_frame_unref(scratchFrame);
        return true;
    }

    bool AudioVideoProcessor::decodeVideoFrame(const MediaBuffer & input, VideoFrame & output) {
        // Wrap caller's bytes in a non-owning AVPacket; av_packet_from_data
        // would take ownership, which would force a copy. The bytes outlive
        // the call so a borrow is correct here.
        AVPacket * pkt = scratchPacket;
        av_packet_unref(pkt);
        pkt->data = static_cast<std::uint8_t *>(input.data);
        pkt->size = static_cast<int>(input.length);
        return decodeVideoPacket(pkt, output);
    }

    bool AudioVideoProcessor::encodeVideoAVFrame(AVFrame * frame, MediaBuffer & output) {
        if(!encodeVideoCtx) return false;
        int err = avcodec_send_frame(encodeVideoCtx, frame);
        if(err < 0) return false;
        AVPacket * pkt = scratchPacket;
        av_packet_unref(pkt);
        err = avcodec_receive_packet(encodeVideoCtx, pkt);
        if(err < 0) return false;

        // MediaBuffer is a borrowed-pointer struct; the caller owns the
        // backing storage. The simplest contract is "we allocate, you
        // free with av_free". Anything more elaborate (a Vec-shaped
        // MediaBuffer with a deleter) belongs in Phase 5.
        output.data   = av_malloc(pkt->size);
        output.length = pkt->size;
        if(output.data) {
            std::memcpy(output.data, pkt->data, pkt->size);
        }
        av_packet_unref(pkt);
        return output.data != nullptr;
    }

    bool AudioVideoProcessor::encodeVideoFrame(const VideoFrame & input, MediaBuffer & output) {
        if(!encodeVideoCtx) return false;
        const std::uint32_t w = input.videoFrame.header.width;
        const std::uint32_t h = input.videoFrame.header.height;
        if(w == 0 || h == 0 || input.videoFrame.pixels.empty()) return false;

        encodeVideoSws = sws_getCachedContext(encodeVideoSws,
            static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_RGBA,
            encodeVideoCtx->width, encodeVideoCtx->height, encodeVideoCtx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if(!encodeVideoSws) return false;

        AVFrame * f = scratchFrame;
        av_frame_unref(f);
        f->format = encodeVideoCtx->pix_fmt;
        f->width  = encodeVideoCtx->width;
        f->height = encodeVideoCtx->height;
        if(av_frame_get_buffer(f, 32) < 0) return false;

        const std::uint8_t * src[1] = { input.videoFrame.pixels.data() };
        int srcStride[1] = { static_cast<int>(input.videoFrame.header.stride) };
        sws_scale(encodeVideoSws,
            src, srcStride, 0, static_cast<int>(h),
            f->data, f->linesize);

        // Monotonic PTS — callers will need a richer encoder API to
        // supply explicit timestamps; for now PTS counts encoded
        // frames.
        static std::int64_t encodePts = 0;
        f->pts = encodePts++;

        return encodeVideoAVFrame(f, output);
    }

    // ───────────────────────────────────────────────────────────────
    //  Audio
    // ───────────────────────────────────────────────────────────────

    void AudioVideoProcessor::setAudioDecodeCodec(MediaCodecID codec, const AudioStreamDesc & desc) {
        if(decodeAudioCtx) avcodec_free_context(&decodeAudioCtx);
        const AVCodec * c = avcodec_find_decoder(toAVCodecID(codec));
        if(!c) return;
        decodeAudioCtx = avcodec_alloc_context3(c);
        decodeAudioCtx->sample_rate = static_cast<int>(desc.sampleRate);
        // FFmpeg ≥ 5.1 (libavcodec ≥ 59) — vendored n7.1.1 here — uses
        // AVChannelLayout. av_channel_layout_default fills a sensible
        // default mapping (mono / stereo / 5.1) from a channel count.
        av_channel_layout_default(&decodeAudioCtx->ch_layout, static_cast<int>(desc.channels));
        decodeAudioCtx->sample_fmt = toAVSampleFormat(desc.sampleFormat);
        if(avcodec_open2(decodeAudioCtx, c, nullptr) < 0) {
            avcodec_free_context(&decodeAudioCtx);
        }
    }

    void AudioVideoProcessor::setAudioEncodeCodec(MediaCodecID codec, const AudioStreamDesc & desc) {
        if(encodeAudioCtx) avcodec_free_context(&encodeAudioCtx);
        const AVCodec * c = avcodec_find_encoder(toAVCodecID(codec));
        if(!c) return;
        encodeAudioCtx = avcodec_alloc_context3(c);
        encodeAudioCtx->sample_rate = static_cast<int>(desc.sampleRate);
        av_channel_layout_default(&encodeAudioCtx->ch_layout, static_cast<int>(desc.channels));
        // Pick the first sample format the encoder supports; encoders
        // are picky here (AAC wants FLTP, MP3 wants S16P / FLT, etc.).
        encodeAudioCtx->sample_fmt = c->sample_fmts ? c->sample_fmts[0] : toAVSampleFormat(desc.sampleFormat);
        encodeAudioCtx->bit_rate = 128'000;
        if(avcodec_open2(encodeAudioCtx, c, nullptr) < 0) {
            avcodec_free_context(&encodeAudioCtx);
        }
    }

    bool AudioVideoProcessor::openAudioDecoderFromParams(AVCodecParameters * codecpar) {
        if(!codecpar) return false;
        if(decodeAudioCtx) avcodec_free_context(&decodeAudioCtx);
        const AVCodec * c = avcodec_find_decoder(codecpar->codec_id);
        if(!c) return false;
        decodeAudioCtx = avcodec_alloc_context3(c);
        if(avcodec_parameters_to_context(decodeAudioCtx, codecpar) < 0) {
            avcodec_free_context(&decodeAudioCtx);
            return false;
        }
        if(avcodec_open2(decodeAudioCtx, c, nullptr) < 0) {
            avcodec_free_context(&decodeAudioCtx);
            return false;
        }
        return true;
    }

    bool AudioVideoProcessor::decodeAudioPacket(AVPacket * packet, AudioSample & output) {
        if(!decodeAudioCtx) return false;
        int err = avcodec_send_packet(decodeAudioCtx, packet);
        if(err < 0 && err != AVERROR(EAGAIN)) return false;
        err = avcodec_receive_frame(decodeAudioCtx, scratchFrame);
        if(err < 0) return false;

        // Resample planar / non-S16 to interleaved S16 — the
        // lowest-common-denominator format every audio sink
        // (`snd_pcm_writei`, Pulse `pa_simple_write`) accepts without
        // further negotiation. Future audio work can widen this.
        const int channels = scratchFrame->ch_layout.nb_channels;

        if(!decodeAudioSwr) {
            // Modern (FFmpeg ≥ 5.1) channel-layout-aware swr setup.
            // Output layout is the same as input so we only flip the
            // sample format / packing; resampling rate stays identity.
            AVChannelLayout outLayout{};
            av_channel_layout_copy(&outLayout, &scratchFrame->ch_layout);
            swr_alloc_set_opts2(&decodeAudioSwr,
                &outLayout, AV_SAMPLE_FMT_S16, scratchFrame->sample_rate,
                &scratchFrame->ch_layout,
                static_cast<AVSampleFormat>(scratchFrame->format),
                scratchFrame->sample_rate,
                0, nullptr);
            av_channel_layout_uninit(&outLayout);
            if(!decodeAudioSwr || swr_init(decodeAudioSwr) < 0) {
                av_frame_unref(scratchFrame);
                return false;
            }
        }

        const int outSamples = swr_get_out_samples(decodeAudioSwr, scratchFrame->nb_samples);
        const std::size_t needed = static_cast<std::size_t>(outSamples)
                                 * static_cast<std::size_t>(channels)
                                 * sizeof(std::int16_t);
        if(needed > pcmBufferSize) {
            if(pcmBuffer) av_free(pcmBuffer);
            pcmBuffer = static_cast<std::uint8_t *>(av_malloc(needed));
            pcmBufferSize = needed;
        }
        std::uint8_t * outBuf[1] = { pcmBuffer };
        int produced = swr_convert(decodeAudioSwr,
            outBuf, outSamples,
            const_cast<const std::uint8_t **>(scratchFrame->extended_data),
            scratchFrame->nb_samples);

        output.data   = pcmBuffer;
        output.length = produced > 0
            ? static_cast<std::size_t>(produced) * channels * sizeof(std::int16_t)
            : 0;
        output.format.sampleRate    = scratchFrame->sample_rate;
        output.format.channels      = channels;
        output.format.bitsPerSample = 16;
        output.format.sampleFormat  = AudioSampleFormat::S16;
        output.decodeTime = std::chrono::high_resolution_clock::now();

        av_frame_unref(scratchFrame);
        return output.length > 0;
    }

    bool AudioVideoProcessor::decodeAudio(const MediaBuffer & input, AudioSample & output) {
        AVPacket * pkt = scratchPacket;
        av_packet_unref(pkt);
        pkt->data = static_cast<std::uint8_t *>(input.data);
        pkt->size = static_cast<int>(input.length);
        return decodeAudioPacket(pkt, output);
    }

    bool AudioVideoProcessor::encodeAudio(const AudioSample & input, MediaBuffer & output) {
        if(!encodeAudioCtx) return false;
        // Build an AVFrame around the caller's PCM. swr adapts to the
        // encoder's required sample_fmt / layout.
        if(!encodeAudioSwr) {
            AVChannelLayout inLayout{};
            av_channel_layout_default(&inLayout, static_cast<int>(input.format.channels));
            swr_alloc_set_opts2(&encodeAudioSwr,
                &encodeAudioCtx->ch_layout, encodeAudioCtx->sample_fmt, encodeAudioCtx->sample_rate,
                &inLayout, toAVSampleFormat(input.format.sampleFormat), static_cast<int>(input.format.sampleRate),
                0, nullptr);
            av_channel_layout_uninit(&inLayout);
            if(!encodeAudioSwr || swr_init(encodeAudioSwr) < 0) return false;
        }

        AVFrame * f = scratchFrame;
        av_frame_unref(f);
        f->format         = encodeAudioCtx->sample_fmt;
        av_channel_layout_copy(&f->ch_layout, &encodeAudioCtx->ch_layout);
        f->sample_rate    = encodeAudioCtx->sample_rate;
        // nb_samples must match the encoder's frame_size for fixed-frame
        // codecs (MP3, AAC). encodeAudio is best-effort here — full
        // FIFO/buffer handling is a Phase 5 concern.
        f->nb_samples = encodeAudioCtx->frame_size > 0 ? encodeAudioCtx->frame_size : 1024;
        if(av_frame_get_buffer(f, 0) < 0) return false;

        const std::uint8_t * srcPlanes[1] = { static_cast<const std::uint8_t *>(input.data) };
        swr_convert(encodeAudioSwr,
            f->data, f->nb_samples,
            srcPlanes,
            static_cast<int>(input.length / (input.format.channels * (input.format.bitsPerSample / 8))));

        int err = avcodec_send_frame(encodeAudioCtx, f);
        if(err < 0) return false;
        AVPacket * pkt = scratchPacket;
        av_packet_unref(pkt);
        err = avcodec_receive_packet(encodeAudioCtx, pkt);
        if(err < 0) return false;

        output.data   = av_malloc(pkt->size);
        output.length = pkt->size;
        if(output.data) std::memcpy(output.data, pkt->data, pkt->size);
        av_packet_unref(pkt);
        return output.data != nullptr;
    }

    // ───────────────────────────────────────────────────────────────
    //  Public factory + helper bridges used by sessions
    // ───────────────────────────────────────────────────────────────

    SharedHandle<AudioVideoProcessor> createAudioVideoProcessor(bool useHardwareAccel, void *gteDevice) {
        return std::make_shared<AudioVideoProcessor>(useHardwareAccel, gteDevice);
    }

} // namespace OmegaVA
