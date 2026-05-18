#include "omegaVA/AudioVideoProcessorContext.h"

#ifndef OMEGAVA_AVF_AVFAUDIOVIDEOPROCESSOR_H
#define OMEGAVA_AVF_AVFAUDIOVIDEOPROCESSOR_H

namespace OmegaVA {

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
