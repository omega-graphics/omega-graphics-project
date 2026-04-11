#include "omegaWTK/Media/AudioVideoProcessorContext.h"

#ifndef OMEGAWTK_MEDIA_AVF_AVFAUDIOVIDEOPROCESSOR_H
#define OMEGAWTK_MEDIA_AVF_AVFAUDIOVIDEOPROCESSOR_H

namespace OmegaWTK::Media {

    class AudioVideoProcessor {
    public:
        void *compressionSession;      ///< VTCompressionSessionRef
        void *decompressionSession;    ///< VTDecompressionSessionRef
        void *lastEncodedBuffer;       ///< CMSampleBufferRef held between callback and return
        bool useHardwareAccel;
        void *gteDevice;               ///< id<MTLDevice> when hardware-accelerated
        unsigned int encodeCodecType;  ///< CMVideoCodecType stored as uint32_t
        unsigned int encodeWidth;
        unsigned int encodeHeight;

        explicit AudioVideoProcessor(bool useHardwareAccel, void *gteDevice);
        ~AudioVideoProcessor();
    };

};

#endif
