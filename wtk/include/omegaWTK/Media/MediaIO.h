#include "omegaWTK/Core/Core.h"

#ifndef OMEGAWTK_MEDIA_MEDIAIO_H
#define OMEGAWTK_MEDIA_MEDIAIO_H

namespace OmegaWTK::Media {

    struct MediaBuffer {
        void *data;
        size_t length;
    };

    struct MediaInputStream {
        bool bufferOrFile;
        OmegaCommon::String file;
        MediaBuffer buffer;
        static MediaInputStream fromFile(const OmegaCommon::FS::Path & path);
        static MediaInputStream fromBuffer(void *data,size_t length);
    };

    struct MediaOutputStream {
        bool bufferOrFile;
        OmegaCommon::String file;
        MediaBuffer buffer;
        static MediaOutputStream toFile(const OmegaCommon::FS::Path & path);
        static MediaOutputStream toBuffer(void *data,size_t length);
    };

    // ──────────────────────────────────────────────
    //  Audio / Video format descriptors
    // ──────────────────────────────────────────────

    /// @brief Layout of PCM audio samples in memory.
    enum class AudioSampleFormat : OPT_PARAM {
        S16,             ///< Signed 16-bit interleaved
        S32,             ///< Signed 32-bit interleaved
        Float32,         ///< 32-bit float interleaved
        Float64,         ///< 64-bit float interleaved
        PlanarS16,       ///< Signed 16-bit planar (one buffer per channel)
        PlanarFloat32,   ///< 32-bit float planar
        Unknown
    };

    /// @brief Describes the format of an audio stream.
    struct AudioStreamDesc {
        unsigned int sampleRate;       ///< e.g. 44100, 48000
        unsigned int channels;         ///< e.g. 1 (mono), 2 (stereo), 6 (5.1)
        unsigned int bitsPerSample;    ///< e.g. 16, 24, 32
        AudioSampleFormat sampleFormat;
    };

    /// @brief Pixel format for video frames (superset of image color formats).
    enum class PixelFormat : OPT_PARAM {
        RGBA,
        BGRA,
        RGB,
        NV12,       ///< Semi-planar YUV 4:2:0 (common HW decoder output)
        YUV420P,    ///< Planar YUV 4:2:0
        YUV422P,    ///< Planar YUV 4:2:2
        P010,       ///< 10-bit semi-planar YUV 4:2:0
        Unknown
    };

    /// @brief Identifies a codec for encode/decode operations.
    enum class MediaCodecID : OPT_PARAM {
        // Video codecs
        H264,
        HEVC,
        VP9,
        AV1,
        // Audio codecs
        AAC,
        MP3,
        FLAC,
        Opus,
        PCM,
        // Raw / passthrough
        RawVideo,
        RawAudio,
        Unknown
    };

    /// @brief Describes the format of a video stream.
    struct VideoStreamDesc {
        unsigned int width;
        unsigned int height;
        unsigned int frameRateNum;      ///< Numerator   (e.g. 30000)
        unsigned int frameRateDen;      ///< Denominator (e.g. 1001 for 29.97 fps)
        MediaCodecID codec;
        PixelFormat  pixelFormat;
        unsigned int bitDepth;          ///< e.g. 8, 10
    };

    /// @brief Container / mux format for file-level I/O.
    enum class ContainerFormat : OPT_PARAM {
        MP4,
        MKV,
        WebM,
        WAV,
        FlacContainer,
        OGG,
        Raw,
        Unknown
    };

    /// @brief Extended source descriptor combining a stream with format metadata.
    struct MediaSourceDesc {
        MediaInputStream stream;
        ContainerFormat  container;
        OmegaCommon::Vector<AudioStreamDesc> audioStreams;
        OmegaCommon::Vector<VideoStreamDesc> videoStreams;
    };
}
#endif