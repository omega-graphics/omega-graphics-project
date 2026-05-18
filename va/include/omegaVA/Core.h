#include <omega-common/common.h>

#include <cassert>
#include <iomanip>
#include <memory>
#include <string>

#include "OmegaVAExport.h"

#ifndef OMEGAVA_CORE_H
#define OMEGAVA_CORE_H

// TODO: lift INTERFACE / INTERFACE_METHOD / ABSTRACT / FALLTHROUGH / DEFAULT
// and Core::Option / OPT_PARAM into omega-common so OmegaWTK and OmegaVA both
// pick them up from a single source (mirrors the duplication this file
// currently has with wtk/include/omegaWTK/Core/Core.h).

/// @brief OmegaVA
/// Audio / video subsystem extracted from OmegaWTK.
namespace OmegaVA {

    #ifdef INTERFACE
    #undef INTERFACE
    #endif
    #define INTERFACE class

    #define INTERFACE_METHOD virtual

    #define ABSTRACT = 0;

    #define FALLTHROUGH = default;

    #define DEFAULT {};

    namespace Core {
        typedef unsigned char Option;
    };

#define OPT_PARAM Core::Option

}

#endif
