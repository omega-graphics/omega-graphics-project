#ifndef OMEGA_COMMON_IMG_CODEC_PRIV_H
#define OMEGA_COMMON_IMG_CODEC_PRIV_H

#include "omega-common/img.h"

#include <istream>

namespace OmegaCommon::Img {

#ifdef TARGET_WIN32
#define DEFAULT_SCREEN_GAMMA 2.2
#endif

#ifdef TARGET_MACOS
#define DEFAULT_SCREEN_GAMMA 2.2
#endif

#ifndef DEFAULT_SCREEN_GAMMA
#define DEFAULT_SCREEN_GAMMA 2.2
#endif

    class ImgCodec {
    protected:
        std::istream & in;
        BitmapImage * storage;
    public:
        virtual void readToStorage() = 0;
        ImgCodec(std::istream & _in, BitmapImage * res) : in(_in), storage(res) {};
        virtual ~ImgCodec() {};
    };

}

#endif
