#include "common.h"

#ifndef OMEGA_COMMON_REGEX_H
#define OMEGA_COMMON_REGEX_H

namespace OmegaCommon {

    enum class RegexOption : unsigned {
        None            = 0,
        CaseInsensitive = 1u << 0,
        Multiline       = 1u << 1,
        DotAll          = 1u << 2,
        Utf             = 1u << 3,
        Anchored        = 1u << 4
    };

    inline unsigned operator|(RegexOption a, RegexOption b) {
        return static_cast<unsigned>(a) | static_cast<unsigned>(b);
    }

    inline unsigned operator|(unsigned a, RegexOption b) {
        return a | static_cast<unsigned>(b);
    }

    struct RegexError {
        int code = 0;
        size_t offset = 0;
        String message;
    };

    struct RegexCapture {
        size_t start = 0;
        size_t end = 0;
        StrRef matched;
    };

    struct RegexMatch {
        RegexCapture fullMatch;
        Vector<RegexCapture> captures;

        const RegexCapture & group(size_t index) const;
    };

    struct RegexImpl;

    class OMEGACOMMON_EXPORT Regex {
        RegexImpl *impl = nullptr;

        explicit Regex(RegexImpl *p);
    public:
        Regex(const Regex &) = delete;
        Regex & operator=(const Regex &) = delete;
        Regex(Regex && other) noexcept;
        Regex & operator=(Regex && other) noexcept;
        ~Regex();

        static Result<Regex, RegexError> compile(StrRef pattern, unsigned options = 0);

        bool matches(StrRef input) const;
        Optional<RegexMatch> search(StrRef input) const;
        Optional<RegexMatch> searchFrom(StrRef input, size_t startOffset) const;
        Vector<RegexMatch> findAll(StrRef input) const;
        Result<String, RegexError> replace(StrRef input, StrRef replacement) const;
        Vector<String> split(StrRef input) const;
    };

    OMEGACOMMON_EXPORT String regexEscape(StrRef input);

}

#endif
