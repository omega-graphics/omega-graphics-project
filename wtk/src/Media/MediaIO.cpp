#include "omegaWTK/Media/MediaIO.h"

namespace OmegaWTK::Media {

    MediaInputStream MediaInputStream::fromFile(OmegaCommon::FS::Path path) {
        MediaInputStream s;
        s.file = path.str();
        return s;
    }

    MediaOutputStream MediaOutputStream::toFile(OmegaCommon::FS::Path path) {
        MediaOutputStream s;
        s.file = path.str();
        return s;
    }

}
