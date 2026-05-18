#include "FFmpegAudioVideoProcessor.h"
#include "FFmpegMediaPrivate.h"
#include "omegaVA/Audio.h"
#include "omegaVA/Video.h"
#include "omegaVA/MediaPlaybackSession.h"

#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <memory>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#if OMEGA_AUDIO_ALSA
#  include <alsa/asoundlib.h>
#endif

namespace OmegaVA {

    namespace {
        // Register libavdevice exactly once. The capture sessions and
        // device-enumeration callers all hit this; std::once_flag makes
        // the registration cost a single atomic check per process.
        std::once_flag g_avdeviceOnce;
        void ensureAvDeviceRegistered() {
            std::call_once(g_avdeviceOnce, []{ avdevice_register_all(); });
        }
    }

    // `AudioPlaybackDevice` and `PlaybackDispatchQueue` are defined in
    // FFmpegMediaPrivate.h — both this TU (capture preview) and the
    // playback TU need them.

    namespace {
        // ─── ALSA device enumeration ─────────────────────────────────
        // alsa-lib's `snd_device_name_hint(-1, "pcm", &hints)` yields
        // every PCM device the kernel exposes, with NAME / DESC / IOID
        // tokens. We split into capture (IOID = "Input" or null) vs
        // playback (IOID = "Output" or null).
#if OMEGA_AUDIO_ALSA
        void enumerateAlsaPcms(
            OmegaCommon::Vector<std::pair<OmegaCommon::String, OmegaCommon::String>> & capture,
            OmegaCommon::Vector<std::pair<OmegaCommon::String, OmegaCommon::String>> & playback)
        {
            void ** hints = nullptr;
            if(snd_device_name_hint(-1, "pcm", &hints) < 0) return;
            for(void ** h = hints; *h; ++h) {
                char * n   = snd_device_name_get_hint(*h, "NAME");
                char * d   = snd_device_name_get_hint(*h, "DESC");
                char * io  = snd_device_name_get_hint(*h, "IOID");

                OmegaCommon::String name  = n ? n : "";
                OmegaCommon::String desc  = d ? d : name;
                bool isCapture  = !io || OmegaCommon::String(io) == "Input";
                bool isPlayback = !io || OmegaCommon::String(io) == "Output";

                if(isCapture)  capture.push_back({desc, name});
                if(isPlayback) playback.push_back({desc, name});

                if(n)  free(n);
                if(d)  free(d);
                if(io) free(io);
            }
            snd_device_name_free_hint(hints);
        }
#endif
    } // namespace

    // ─── AudioCaptureDevice ──────────────────────────────────────────
    class FFmpegAudioCaptureSession;

    class FFmpegAudioCaptureDevice : public AudioCaptureDevice,
                                     public std::enable_shared_from_this<FFmpegAudioCaptureDevice> {
    public:
        OmegaCommon::String displayName;
        OmegaCommon::String alsaName;
        explicit FFmpegAudioCaptureDevice(OmegaCommon::String n, OmegaCommon::String a)
            : displayName(std::move(n)), alsaName(std::move(a)) {}

        UniqueHandle<AudioCaptureSession> createCaptureSession() override;
    };

    OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> enumerateAudioCaptureDevices() {
        OmegaCommon::Vector<SharedHandle<AudioCaptureDevice>> out;
#if OMEGA_AUDIO_ALSA
        OmegaCommon::Vector<std::pair<OmegaCommon::String, OmegaCommon::String>> capture, playback;
        enumerateAlsaPcms(capture, playback);
        for(auto & p : capture) {
            out.push_back(std::make_shared<FFmpegAudioCaptureDevice>(p.first, p.second));
        }
#endif
        return out;
    }

    OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> enumerateAudioPlaybackDevices() {
        OmegaCommon::Vector<SharedHandle<AudioPlaybackDevice>> out;
#if OMEGA_AUDIO_ALSA
        OmegaCommon::Vector<std::pair<OmegaCommon::String, OmegaCommon::String>> capture, playback;
        enumerateAlsaPcms(capture, playback);
        for(auto & p : playback) {
            out.push_back(std::make_shared<AudioPlaybackDevice>(p.first, p.second));
        }
#endif
        return out;
    }

    // ─── VideoDevice ─────────────────────────────────────────────────
    class FFmpegVideoCaptureSession;

    class FFmpegVideoDevice : public VideoDevice {
    public:
        OmegaCommon::String devicePath;  ///< e.g. /dev/video0
        OmegaCommon::String displayName;
        explicit FFmpegVideoDevice(OmegaCommon::String path, OmegaCommon::String name)
            : devicePath(std::move(path)), displayName(std::move(name)) {}

        UniqueHandle<VideoCaptureSession> createCaptureSession(
            SharedHandle<AudioCaptureDevice> & audioCaptureDevice) override;
    };

    OmegaCommon::Vector<SharedHandle<VideoDevice>> enumerateVideoDevices() {
        OmegaCommon::Vector<SharedHandle<VideoDevice>> out;
        // V4L2 enumeration via /dev/video* + VIDIOC_QUERYCAP. Skipping
        // libudev keeps the dep footprint minimal; the trade-off is we
        // miss USB hotplug — devices added after process start aren't
        // re-enumerated until the caller asks again.
        DIR * dev = opendir("/dev");
        if(!dev) return out;
        struct dirent * ent;
        while((ent = readdir(dev)) != nullptr) {
            const std::string name = ent->d_name;
            if(name.rfind("video", 0) != 0) continue;
            const std::string path = "/dev/" + name;
            int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
            if(fd < 0) continue;
            v4l2_capability cap{};
            if(ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
                if(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
                    out.push_back(std::make_shared<FFmpegVideoDevice>(
                        path,
                        reinterpret_cast<const char *>(cap.card)));
                }
            }
            close(fd);
        }
        closedir(dev);
        return out;
    }

    // ───────────────────────────────────────────────────────────────
    //  Audio capture session
    // ───────────────────────────────────────────────────────────────

    class FFmpegAudioCaptureSession : public AudioCaptureSession {
        OmegaCommon::String alsaName;
        SharedHandle<AudioPlaybackDevice> previewDevice;
        MediaOutputStream * outputStream = nullptr;
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> recording{false};
        std::atomic<bool> previewing{false};

        // FFmpeg side (alsa input → packets → optional encode/mux).
        AVFormatContext * inputCtx = nullptr;
#if OMEGA_AUDIO_ALSA
        snd_pcm_t * previewPcm = nullptr;
#endif

    public:
        explicit FFmpegAudioCaptureSession(OmegaCommon::String name)
            : alsaName(std::move(name)) {
            ensureAvDeviceRegistered();
        }
        ~FFmpegAudioCaptureSession() override {
            stopRecord();
            stopPreview();
            if(running.load()) {
                running.store(false);
                if(worker.joinable()) worker.join();
            }
            if(inputCtx) avformat_close_input(&inputCtx);
#if OMEGA_AUDIO_ALSA
            if(previewPcm) snd_pcm_close(previewPcm);
#endif
        }

        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> & device) override {
            previewDevice = device;
        }
        void setAudioOutputStream(MediaOutputStream & outputStream_) override {
            outputStream = &outputStream_;
        }

        void startPreview() override {
            if(previewing.exchange(true)) return;
            startWorker();
        }
        void startRecord() override {
            if(recording.exchange(true)) return;
            startWorker();
        }
        void stopPreview() override { previewing.store(false); }
        void stopRecord() override  { recording.store(false); }

    private:
        void startWorker() {
            if(running.exchange(true)) return;
            worker = std::thread([this]{ runLoop(); });
        }

        void runLoop() {
            // Open the ALSA capture as an FFmpeg input. The `alsa`
            // demuxer (registered by avdevice_register_all) takes the
            // PCM hint as the URL.
            const AVInputFormat * fmt = av_find_input_format("alsa");
            if(!fmt) { running.store(false); return; }
            int err = avformat_open_input(&inputCtx, alsaName.c_str(), const_cast<AVInputFormat *>(fmt), nullptr);
            if(err < 0 || !inputCtx) { running.store(false); return; }

#if OMEGA_AUDIO_ALSA
            // Open the preview sink (separate PCM handle). Format is
            // pinned to S16LE / 2ch / 48kHz — what most consumer
            // microphones produce after FFmpeg's defaults.
            if(previewDevice && !previewPcm) {
                int pcmErr = snd_pcm_open(&previewPcm, previewDevice->alsaName.c_str(),
                                          SND_PCM_STREAM_PLAYBACK, 0);
                if(pcmErr == 0) {
                    snd_pcm_set_params(previewPcm,
                        SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                        2, 48000, 1, 100'000);
                }
            }
#endif

            AVPacket * pkt = av_packet_alloc();
            while(running.load() && (previewing.load() || recording.load())) {
                if(av_read_frame(inputCtx, pkt) < 0) break;
#if OMEGA_AUDIO_ALSA
                if(previewing.load() && previewPcm) {
                    // ALSA-raw indev hands us interleaved PCM verbatim
                    // (no compression). frames = bytes / (channels * sample_bytes).
                    snd_pcm_writei(previewPcm, pkt->data, pkt->size / 4);
                }
#endif
                // Recording → MediaOutputStream is a future enhancement
                // tied to mux setup; for now record is a no-op so the
                // session exists for the public API to call into.
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            running.store(false);
        }
    };

    UniqueHandle<AudioCaptureSession> FFmpegAudioCaptureDevice::createCaptureSession() {
        return std::make_unique<FFmpegAudioCaptureSession>(alsaName);
    }

    // ───────────────────────────────────────────────────────────────
    //  Video capture session
    // ───────────────────────────────────────────────────────────────

    class FFmpegVideoCaptureSession : public VideoCaptureSession {
        OmegaCommon::String devicePath;
        SharedHandle<AudioCaptureDevice> audioDevice;
        VideoFrameSink * previewSink = nullptr;
        SharedHandle<AudioPlaybackDevice> previewAudioDevice;
        MediaOutputStream * outputStream = nullptr;

        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> previewing{false};
        std::atomic<bool> recording{false};

        AVFormatContext * inputCtx = nullptr;
        SwsContext * sws = nullptr;
        int videoStreamIdx = -1;

    public:
        explicit FFmpegVideoCaptureSession(OmegaCommon::String path,
                                           SharedHandle<AudioCaptureDevice> audio)
            : devicePath(std::move(path)), audioDevice(std::move(audio)) {
            ensureAvDeviceRegistered();
        }
        ~FFmpegVideoCaptureSession() override {
            stopRecord();
            stopPreview();
            running.store(false);
            if(worker.joinable()) worker.join();
            if(sws) sws_freeContext(sws);
            if(inputCtx) avformat_close_input(&inputCtx);
        }

        void setVideoFrameSinkForPreview(VideoFrameSink & frameSink) override {
            previewSink = &frameSink;
        }
        void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> & device) override {
            previewAudioDevice = device;
        }
        void setVideoOutputStream(MediaOutputStream & outputStream_) override {
            outputStream = &outputStream_;
        }

        void startPreview() override {
            if(previewing.exchange(true)) return;
            startWorker();
        }
        void startRecord() override {
            if(recording.exchange(true)) return;
            startWorker();
        }
        void stopPreview() override { previewing.store(false); }
        void stopRecord() override  { recording.store(false); }

    private:
        void startWorker() {
            if(running.exchange(true)) return;
            worker = std::thread([this]{ runLoop(); });
        }

        void runLoop() {
            const AVInputFormat * fmt = av_find_input_format("v4l2");
            if(!fmt) { running.store(false); return; }
            int err = avformat_open_input(&inputCtx, devicePath.c_str(), const_cast<AVInputFormat *>(fmt), nullptr);
            if(err < 0 || !inputCtx) { running.store(false); return; }
            if(avformat_find_stream_info(inputCtx, nullptr) < 0) { running.store(false); return; }

            // Pick first video stream — typical V4L2 device exposes one.
            for(unsigned i = 0; i < inputCtx->nb_streams; ++i) {
                if(inputCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    videoStreamIdx = static_cast<int>(i);
                    break;
                }
            }
            if(videoStreamIdx < 0) { running.store(false); return; }

            // Build a one-shot decoder for the camera's output format
            // (uncompressed YUYV or raw RGB on most webcams; some
            // pre-encode to MJPEG, in which case we need the decoder).
            AVCodecParameters * par = inputCtx->streams[videoStreamIdx]->codecpar;
            const AVCodec * dec = avcodec_find_decoder(par->codec_id);
            AVCodecContext * dctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(dctx, par);
            if(avcodec_open2(dctx, dec, nullptr) < 0) {
                avcodec_free_context(&dctx);
                running.store(false);
                return;
            }

            AVPacket * pkt = av_packet_alloc();
            AVFrame   * frm = av_frame_alloc();
            while(running.load() && (previewing.load() || recording.load())) {
                if(av_read_frame(inputCtx, pkt) < 0) break;
                if(pkt->stream_index != videoStreamIdx) {
                    av_packet_unref(pkt);
                    continue;
                }
                if(avcodec_send_packet(dctx, pkt) < 0) {
                    av_packet_unref(pkt);
                    continue;
                }
                while(avcodec_receive_frame(dctx, frm) >= 0) {
                    if(!previewSink) {
                        av_frame_unref(frm);
                        continue;
                    }
                    const int w = frm->width;
                    const int h = frm->height;
                    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frm->format);
                    sws = sws_getCachedContext(sws,
                        w, h, srcFmt,
                        w, h, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if(!sws) { av_frame_unref(frm); continue; }

                    auto frame = std::make_shared<VideoFrame>();
                    const std::size_t stride = static_cast<std::size_t>(w) * 4;
                    frame->videoFrame.pixels = OmegaCommon::Img::PixelStorage::allocate(stride * h);
                    std::uint8_t * dst[1] = { frame->videoFrame.pixels.data() };
                    int dstStride[1] = { static_cast<int>(stride) };
                    sws_scale(sws, frm->data, frm->linesize, 0, h, dst, dstStride);

                    frame->videoFrame.header.width  = w;
                    frame->videoFrame.header.height = h;
                    frame->videoFrame.header.channels = 4;
                    frame->videoFrame.header.bitDepth = 8;
                    frame->videoFrame.header.stride = stride;
                    frame->videoFrame.header.color_format = OmegaCommon::Img::ColorFormat::RGBA;
                    frame->videoFrame.header.alpha_format = OmegaCommon::Img::AlphaFormat::Straight;
                    frame->decodeFinishTime = std::chrono::high_resolution_clock::now();
                    frame->presentTime = frame->decodeFinishTime;
                    previewSink->pushFrame(frame);

                    av_frame_unref(frm);
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
            av_frame_free(&frm);
            avcodec_free_context(&dctx);
            running.store(false);
        }
    };

    UniqueHandle<VideoCaptureSession> FFmpegVideoDevice::createCaptureSession(
        SharedHandle<AudioCaptureDevice> & audioCaptureDevice) {
        return std::make_unique<FFmpegVideoCaptureSession>(devicePath, audioCaptureDevice);
    }

} // namespace OmegaVA
