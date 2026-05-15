// pImpl wrapper around ICU's BreakIterator. Defined here so consumers
// of `omega-common/unicode.h` don't pull in `unicode/brkiter.h` /
// transitively link ICU themselves.
//
// Used by WTK's text layout engine (Text-Layout-Engine-Plan.md,
// Phase 2). Phase 2 only honors mandatory breaks (\n, U+2028, U+2029),
// but the iterator exposes all break opportunities so Phase 3's wrap
// pass can reuse the same object.

#include "omega-common/unicode.h"

#include <unicode/brkiter.h>
#include <unicode/locid.h>
#include <unicode/unistr.h>

namespace OmegaCommon {

    struct BreakIterator::Impl {
        std::unique_ptr<icu::BreakIterator> iter;
        icu::UnicodeString text;
        Type type;
    };

    BreakIterator::BreakIterator(Type type, const UniString & text)
        : impl_(std::make_unique<Impl>()) {
        impl_->type = type;
        const auto * buf = text.getBuffer();
        const auto len = text.length();
        if(buf != nullptr && len > 0){
            impl_->text = icu::UnicodeString(buf, len);
        }

        UErrorCode err = U_ZERO_ERROR;
        // Default locale is fine for Phase 2 — line break behavior
        // doesn't vary across locales for the mandatory-break case we
        // care about. The wrap pass in Phase 3 may want to thread a
        // locale through.
        icu::BreakIterator * raw = nullptr;
        switch(type){
            case Type::Word:
                raw = icu::BreakIterator::createWordInstance(icu::Locale::getDefault(), err);
                break;
            case Type::Line:
            default:
                raw = icu::BreakIterator::createLineInstance(icu::Locale::getDefault(), err);
                break;
        }
        if(U_FAILURE(err) || raw == nullptr){
            // Leave impl_->iter null; first() / next() will return
            // DONE so the caller sees an iterator that produces no
            // boundaries — the layout engine then treats the whole
            // string as a single segment.
            return;
        }
        raw->setText(impl_->text);
        impl_->iter.reset(raw);
    }

    BreakIterator::~BreakIterator() = default;

    std::int32_t BreakIterator::first() {
        if(impl_->iter == nullptr) return DONE;
        const std::int32_t pos = impl_->iter->first();
        return (pos == icu::BreakIterator::DONE) ? DONE : pos;
    }

    std::int32_t BreakIterator::next() {
        if(impl_->iter == nullptr) return DONE;
        const std::int32_t pos = impl_->iter->next();
        return (pos == icu::BreakIterator::DONE) ? DONE : pos;
    }

    bool BreakIterator::isMandatoryBreakAt(std::int32_t position) const {
        if(impl_->iter == nullptr || impl_->type != Type::Line){
            return false;
        }
        // ICU's line iterator tags each boundary with a "rule status"
        // range. UBRK_LINE_HARD..UBRK_LINE_HARD_LIMIT identifies
        // mandatory breaks (newline-driven); UBRK_LINE_SOFT covers
        // optional break opportunities. The iterator must already be
        // positioned at `position`; we check the *current* status
        // rather than re-positioning so callers don't pay an extra
        // ICU traversal per query.
        const std::int32_t status = impl_->iter->getRuleStatus();
        if(impl_->iter->current() != position){
            return false;
        }
        return status >= UBRK_LINE_HARD && status < UBRK_LINE_HARD_LIMIT;
    }

}
