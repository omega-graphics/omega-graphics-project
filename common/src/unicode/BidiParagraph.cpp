// pImpl wrapper around ICU's `UBiDi`. Defined here so consumers of
// `omega-common/unicode.h` don't pull in `unicode/ubidi.h` /
// transitively link ICU themselves.
//
// Used by WTK's text layout engine (Text-Layout-Engine-Plan.md
// Phase 3) to split a logical-order line of text into visual runs
// — one per resolved embedding level — before script segmentation
// and shaping.

#include "omega-common/unicode.h"

#include <unicode/ubidi.h>
#include <unicode/utypes.h>

#include <vector>

namespace OmegaCommon {

    struct BidiParagraph::Impl {
        UBiDi * bidi = nullptr;
        // ICU keeps a pointer to the source UTF-16 buffer for the
        // lifetime of `ubidi_setPara`; copy it so we don't depend on
        // the caller's `UniString` outliving us.
        std::vector<UChar> textCopy;
    };

    BidiParagraph::BidiParagraph(const UniString & text)
        : impl_(std::make_unique<Impl>()) {
        const auto * buf = text.getBuffer();
        const auto len = text.length();
        if(buf == nullptr || len <= 0){
            return;
        }
        impl_->textCopy.resize(static_cast<std::size_t>(len));
        for(std::int32_t i = 0; i < len; ++i){
            impl_->textCopy[static_cast<std::size_t>(i)] = static_cast<UChar>(buf[i]);
        }

        UErrorCode err = U_ZERO_ERROR;
        impl_->bidi = ubidi_open();
        if(impl_->bidi == nullptr){
            return;
        }
        // `UBIDI_DEFAULT_LTR` makes the analyzer auto-detect the
        // paragraph direction from the first strong character,
        // falling back to LTR if none. Matches how editors and
        // browsers handle mixed-direction text.
        ubidi_setPara(impl_->bidi,
                      impl_->textCopy.data(),
                      len,
                      UBIDI_DEFAULT_LTR,
                      nullptr,
                      &err);
        if(U_FAILURE(err)){
            ubidi_close(impl_->bidi);
            impl_->bidi = nullptr;
        }
    }

    BidiParagraph::~BidiParagraph(){
        if(impl_ && impl_->bidi != nullptr){
            ubidi_close(impl_->bidi);
        }
    }

    std::int32_t BidiParagraph::runCount() const {
        if(impl_->bidi == nullptr) return 0;
        UErrorCode err = U_ZERO_ERROR;
        const std::int32_t n = ubidi_countRuns(impl_->bidi, &err);
        if(U_FAILURE(err)) return 0;
        return n;
    }

    BidiParagraph::VisualRun
    BidiParagraph::getVisualRun(std::int32_t index) const {
        VisualRun out;
        if(impl_->bidi == nullptr || index < 0) return out;
        std::int32_t logicalStart = 0;
        std::int32_t length = 0;
        const UBiDiDirection dir = ubidi_getVisualRun(impl_->bidi, index,
                                                     &logicalStart, &length);
        if(length <= 0) return out;
        out.logicalStart = logicalStart;
        out.length       = length;
        out.rightToLeft  = (dir == UBIDI_RTL);
        return out;
    }

}
