// Media-API-Completion-Plan §2.5 — FFmpeg playback.
//
// PlaybackDispatchQueue is the same shape as the AVF implementation in
// wtk/src/Media/avf/AVFAudioVideoCapture.mm: one std::thread per queue
// with a std::condition_variable, and a vector of `Client` entries the
// worker iterates over. Each client owns its own AVFormatContext +
// AudioVideoProcessor and is responsible for producing frames /
// samples in its `tick()` callback. The queue thread sleeps between
// ticks so a paused client doesn't burn the CPU.
//
// This file replaces the previous null-stub of the same name. The
// `Stubs` suffix is retained so the CMake glob still picks it up
// without retouching the build files.

#include "FFmpegAudioVideoProcessor.h"
#include "FFmpegMediaPrivate.h"
#include "omegaVA/MediaPlaybackSession.h"

#include <iostream>

#if OMEGA_AUDIO_ALSA
#  include <alsa/asoundlib.h>
#endif

namespace OmegaVA {

    SharedHandle<PlaybackDispatchQueue> createPlaybackDispatchQueue() {
        return std::make_shared<PlaybackDispatchQueue>();
    }

    typedef SharedHandle<AudioVideoProcessor> & AudioVideoProcessorRef;
    typedef SharedHandle<PlaybackDispatchQueue> & PlaybackDispatchQueueRef;

    // ───────────────────────────────────────────────────────────────
    //  Audio playback session
    // ───────────────────────────────────────────────────────────────

    class FFmpegAudioPlaybackSession : public AudioPlaybackSession {
        AudioVideoProcessor * proc = nullptr;
        SharedHandle<PlaybackDispatchQueue> queue;
        SharedHandle<AudioPlaybackDevice> playbackDevice;
        std::shared_ptr<PlaybackDispatchQueue::Client> client;

        AVFormatContext * formatCtx = nullptr;
        int audioStreamIdx = -1;
        AVPacket * pkt = nullptr;
        std::mutex stateMtx;

#if OMEGA_AUDIO_ALSA
        snd_pcm_t * pcm = nullptr;
#endif

    public:
        FFmpegAudioPlaybackSession(AudioVideoProcessorRef p, PlaybackDispatchQueueRef q)
            : AudioPlaybackSession(p), queue(q) {
            proc = p.get();
            pkt = av_packet_alloc();
        }
        ~FFmpegAudioPlaybackSession() override {
            if(queue && client) queue->unregisterClient(client);
            if(pkt) av_packet_free(&pkt);
            if(formatCtx) avformat_close_input(&formatCtx);
#if OMEGA_AUDIO_ALSA
            if(pcm) snd_pcm_close(pcm);
#endif
        }

        void setAudioSource(MediaInputStream & inputStream) override {
            std::lock_guard<std::mutex> g(stateMtx);
            if(formatCtx) avformat_close_input(&formatCtx);
            if(avformat_open_input(&formatCtx, inputStream.file.c_str(), nullptr, nullptr) < 0) {
                formatCtx = nullptr;
                return;
            }
            if(avformat_find_stream_info(formatCtx, nullptr) < 0) return;
            audioStreamIdx = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if(audioStreamIdx < 0) return;
            proc->openAudioDecoderFromParams(formatCtx->streams[audioStreamIdx]->codecpar);
        }

        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> & device) override {
            playbackDevice = device;
        }

        void start() override {
            std::lock_guard<std::mutex> g(stateMtx);
            if(!formatCtx || audioStreamIdx < 0) return;
#if OMEGA_AUDIO_ALSA
            // Open the ALSA PCM lazily, format pinned to S16LE / 2ch /
            // 48 kHz — the lowest common denominator that every desktop
            // sound card supports without resampler negotiation. The
            // processor's swr resamples the source to match on every
            // decode (see decodeAudioPacket).
            if(!pcm && playbackDevice) {
                snd_pcm_open(&pcm, playbackDevice->alsaName.c_str(),
                             SND_PCM_STREAM_PLAYBACK, 0);
                if(pcm) {
                    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                       SND_PCM_ACCESS_RW_INTERLEAVED,
                                       2, 48000, 1, 200'000);
                }
            }
#endif
            if(!client) {
                client = queue->registerClient([this]{ tick(); });
            }
            client->active.store(true);
        }

        void pause() override {
            if(client) client->active.store(false);
        }
        void reset() override {
            if(client) client->active.store(false);
            std::lock_guard<std::mutex> g(stateMtx);
            if(formatCtx) {
                av_seek_frame(formatCtx, audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
            }
        }

    private:
        void tick() {
            std::lock_guard<std::mutex> g(stateMtx);
            if(!formatCtx) return;
            if(av_read_frame(formatCtx, pkt) < 0) {
                // EOF — stop ticking. Caller can call start() again to
                // loop, after which they'd reset() first.
                if(client) client->active.store(false);
                return;
            }
            if(pkt->stream_index != audioStreamIdx) {
                av_packet_unref(pkt);
                return;
            }
            AudioSample sample{};
            if(proc->decodeAudioPacket(pkt, sample)) {
#if OMEGA_AUDIO_ALSA
                if(pcm && sample.data && sample.length > 0) {
                    const snd_pcm_uframes_t frames =
                        sample.length / (sample.format.channels * (sample.format.bitsPerSample / 8));
                    snd_pcm_writei(pcm, sample.data, frames);
                }
#endif
            }
            av_packet_unref(pkt);
        }
    };

    SharedHandle<AudioPlaybackSession> AudioPlaybackSession::Create(
            AudioVideoProcessorRef processor, PlaybackDispatchQueueRef dispatchQueue) {
        return SharedHandle<AudioPlaybackSession>(
            new FFmpegAudioPlaybackSession(processor, dispatchQueue));
    }

    // ───────────────────────────────────────────────────────────────
    //  Video playback session
    // ───────────────────────────────────────────────────────────────

    class FFmpegVideoPlaybackSession : public VideoPlaybackSession {
        AudioVideoProcessor * proc = nullptr;
        SharedHandle<PlaybackDispatchQueue> queue;
        VideoFrameSink * sink = nullptr;
        SharedHandle<AudioPlaybackDevice> playbackDevice;
        std::shared_ptr<PlaybackDispatchQueue::Client> client;

        AVFormatContext * formatCtx = nullptr;
        int videoStreamIdx = -1;
        int audioStreamIdx = -1;
        AVPacket * pkt = nullptr;
        std::mutex stateMtx;

        // Wall-clock anchor for presentation-time synchronisation.
        // Computed lazily on the first decoded frame; resets on reset().
        std::chrono::high_resolution_clock::time_point clockEpoch{};
        std::int64_t firstPts = AV_NOPTS_VALUE;
        AVRational  videoTimeBase{1, 1};

#if OMEGA_AUDIO_ALSA
        snd_pcm_t * pcm = nullptr;
#endif

    public:
        FFmpegVideoPlaybackSession(AudioVideoProcessorRef p, PlaybackDispatchQueueRef q)
            : VideoPlaybackSession(p), queue(q) {
            proc = p.get();
            pkt = av_packet_alloc();
        }
        ~FFmpegVideoPlaybackSession() override {
            if(queue && client) queue->unregisterClient(client);
            if(pkt) av_packet_free(&pkt);
            if(formatCtx) avformat_close_input(&formatCtx);
#if OMEGA_AUDIO_ALSA
            if(pcm) snd_pcm_close(pcm);
#endif
        }

        void setVideoSource(MediaInputStream & inputStream) override {
            std::lock_guard<std::mutex> g(stateMtx);
            if(formatCtx) avformat_close_input(&formatCtx);
            if(avformat_open_input(&formatCtx, inputStream.file.c_str(), nullptr, nullptr) < 0) {
                formatCtx = nullptr;
                return;
            }
            if(avformat_find_stream_info(formatCtx, nullptr) < 0) return;
            videoStreamIdx = av_find_best_stream(formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            audioStreamIdx = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if(videoStreamIdx >= 0) {
                proc->openVideoDecoderFromParams(formatCtx->streams[videoStreamIdx]->codecpar);
                videoTimeBase = formatCtx->streams[videoStreamIdx]->time_base;
            }
            if(audioStreamIdx >= 0) {
                proc->openAudioDecoderFromParams(formatCtx->streams[audioStreamIdx]->codecpar);
            }
        }
        void setVideoFrameSink(VideoFrameSink & sink_) override { sink = &sink_; }
        void setAudioPlaybackDevice(SharedHandle<AudioPlaybackDevice> & device) override {
            playbackDevice = device;
        }

        void start() override {
            std::lock_guard<std::mutex> g(stateMtx);
            if(!formatCtx) return;
#if OMEGA_AUDIO_ALSA
            if(!pcm && playbackDevice && audioStreamIdx >= 0) {
                snd_pcm_open(&pcm, playbackDevice->alsaName.c_str(),
                             SND_PCM_STREAM_PLAYBACK, 0);
                if(pcm) {
                    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                                       SND_PCM_ACCESS_RW_INTERLEAVED,
                                       2, 48000, 1, 200'000);
                }
            }
#endif
            if(!client) {
                client = queue->registerClient([this]{ tick(); });
            }
            clockEpoch = std::chrono::high_resolution_clock::time_point{};
            firstPts = AV_NOPTS_VALUE;
            client->active.store(true);
        }

        void pause() override {
            if(client) client->active.store(false);
        }
        void reset() override {
            if(client) client->active.store(false);
            std::lock_guard<std::mutex> g(stateMtx);
            if(formatCtx) {
                av_seek_frame(formatCtx, videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
            }
            clockEpoch = std::chrono::high_resolution_clock::time_point{};
            firstPts = AV_NOPTS_VALUE;
        }

    private:
        void tick() {
            std::lock_guard<std::mutex> g(stateMtx);
            if(!formatCtx) return;
            if(av_read_frame(formatCtx, pkt) < 0) {
                if(client) client->active.store(false);
                return;
            }
            if(pkt->stream_index == videoStreamIdx && sink) {
                auto frame = std::make_shared<VideoFrame>();
                if(proc->decodeVideoPacket(pkt, *frame)) {
                    // PTS-driven sleep keeps playback at source rate
                    // rather than "as fast as we can decode." The first
                    // decoded frame anchors the wall clock; every
                    // subsequent frame computes its target wall time as
                    // anchor + (pts - firstPts) * time_base.
                    if(firstPts == AV_NOPTS_VALUE) {
                        firstPts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : 0;
                        clockEpoch = std::chrono::high_resolution_clock::now();
                    }
                    const std::int64_t deltaPts =
                        (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : 0) - firstPts;
                    const std::int64_t deltaNs =
                        deltaPts * 1'000'000'000LL * videoTimeBase.num / videoTimeBase.den;
                    frame->presentTime = clockEpoch + std::chrono::nanoseconds(deltaNs);

                    auto now = std::chrono::high_resolution_clock::now();
                    if(frame->presentTime > now) {
                        std::this_thread::sleep_for(frame->presentTime - now);
                    }
                    sink->pushFrame(frame);
                }
            } else if(pkt->stream_index == audioStreamIdx) {
                AudioSample sample{};
                if(proc->decodeAudioPacket(pkt, sample)) {
#if OMEGA_AUDIO_ALSA
                    if(pcm && sample.data && sample.length > 0) {
                        const snd_pcm_uframes_t frames =
                            sample.length / (sample.format.channels * (sample.format.bitsPerSample / 8));
                        snd_pcm_writei(pcm, sample.data, frames);
                    }
#endif
                }
            }
            av_packet_unref(pkt);
        }
    };

    SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(
            AudioVideoProcessorRef processor, PlaybackDispatchQueueRef dispatchQueue) {
        return SharedHandle<VideoPlaybackSession>(
            new FFmpegVideoPlaybackSession(processor, dispatchQueue));
    }

} // namespace OmegaVA
