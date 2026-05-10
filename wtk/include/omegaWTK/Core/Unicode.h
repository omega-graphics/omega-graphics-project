#ifndef OMEGAWTK_CORE_UNICODE_H
#define OMEGAWTK_CORE_UNICODE_H

// This header has moved to <omega-common/unicode.h>. The OmegaWTK aliases
// below remain for source compatibility while WTK callers migrate. Per the
// Common-ImgCodec-Unicode-Refactor-Plan (Phase 2), the implementation now
// lives in OmegaCommon; only the namespace alias stays here.

#include "omega-common/unicode.h"
#include "OmegaWTKExport.h"

namespace OmegaWTK {

    using UnicodeChar   = OmegaCommon::UnicodeChar;
    using Unicode32Char = OmegaCommon::Unicode32Char;
    using UniString     = OmegaCommon::UniString;

}

#endif
