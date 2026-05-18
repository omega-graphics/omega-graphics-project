#include "omegaVA/Core.h"

#include "Audio.h"
#include "AudioVideoProcessorContext.h"

#ifndef OMEGAVA_VIDEO_H
#define OMEGAVA_VIDEO_H

namespace OmegaVA {

    INTERFACE VideoCaptureSession;

    /// @interface VideoDevice
    /// @brief A physical device for capturing video. (e.g. a USB webcam)
    class OMEGAVA_EXPORT VideoDevice {
    public:
        /** @brief Creates a Capture Session for this device.
           @param audioCaptureDevice The AudioCaptureDevice to capture the audio input.
           @returns A new VideoCaptureSession.
         */
        INTERFACE_METHOD UniqueHandle<VideoCaptureSession> createCaptureSession(SharedHandle<AudioCaptureDevice> & audioCaptureDevice) ABSTRACT;
    };

    /** @brief Enumerate all currently active and discoverable devices for video capture.
        @returns OmegaCommon::Vector<VideoDevice>
     */
    OMEGAVA_EXPORT OmegaCommon::Vector<SharedHandle<VideoDevice>> enumerateVideoDevices();

    /// @interface VideoCaptureSession
    /// @brief A temporary context for capturing video and audio input from a VideoDevice and an AudioCaptureDevice.
    class VideoCaptureSession {
    public:
        // Same rationale as AudioCaptureSession::~AudioCaptureSession —
        // sessions are owned via UniqueHandle<VideoCaptureSession>
        // (std::unique_ptr), and the base dtor must be virtual so the
        // backend subclass's teardown actually runs.
        virtual ~VideoCaptureSession() = default;

        /** @brief Sets the sink to receive the preview frame data.
           @param frameSink The VideoFrameSink to use.
         */
        INTERFACE_METHOD void setVideoFrameSinkForPreview(VideoFrameSink &frameSink) ABSTRACT;

        /** @brief Sets the AudioPlaybackDevice to receive preview audio samples.
           @param device The Playback Device to use.
         */
        INTERFACE_METHOD void setAudioPlaybackDeviceForPreview(SharedHandle<AudioPlaybackDevice> & device) ABSTRACT;

        /** @brief Sets the output stream.
          @param outputStream The MediaOutputStream to output video and audio samples to.
        */
        INTERFACE_METHOD void setVideoOutputStream(MediaOutputStream &outputStream) ABSTRACT;

        /// @brief Starts previewing media samples from the capture device (s). (Not capturing output).
        INTERFACE_METHOD void startPreview() ABSTRACT;

        /// @brief Starts capturing media samples from the capture device (s).
        INTERFACE_METHOD void startRecord() ABSTRACT;

        /// @brief Stops capturing media samples from the capture device (s).
        INTERFACE_METHOD void stopRecord() ABSTRACT;

        /// @brief Stops previewing media samples from the capture device (s).
        INTERFACE_METHOD void stopPreview() ABSTRACT;
    };
};

#endif
