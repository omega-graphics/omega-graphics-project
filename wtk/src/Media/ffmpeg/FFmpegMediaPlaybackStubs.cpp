#include "omegaWTK/Media/MediaPlaybackSession.h"

namespace OmegaWTK::Media {

SharedHandle<PlaybackDispatchQueue> createPlaybackDispatchQueue() {
    return nullptr;
}

SharedHandle<AudioPlaybackSession> AudioPlaybackSession::Create(
    UniqueHandle<AudioVideoProcessor> &,
    SharedHandle<PlaybackDispatchQueue> &) {
    return nullptr;
}

SharedHandle<VideoPlaybackSession> VideoPlaybackSession::Create(
    UniqueHandle<AudioVideoProcessor> &,
    SharedHandle<PlaybackDispatchQueue> &) {
    return nullptr;
}

} // namespace OmegaWTK::Media
