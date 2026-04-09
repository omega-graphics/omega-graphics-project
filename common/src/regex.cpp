#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "omega-common/regex.h"

namespace OmegaCommon {

    struct RegexImpl {
        pcre2_code *code;
        ~RegexImpl() {
            if (code)
                pcre2_code_free(code);
        }
    };

    // --- RegexMatch ---

    const RegexCapture & RegexMatch::group(size_t index) const {
        return captures.at(index);
    }

    // --- Regex lifecycle ---

    Regex::Regex(RegexImpl *p) : impl(p) {}

    Regex::Regex(Regex && other) noexcept : impl(other.impl) {
        other.impl = nullptr;
    }

    Regex & Regex::operator=(Regex && other) noexcept {
        if (this != &other) {
            delete impl;
            impl = other.impl;
            other.impl = nullptr;
        }
        return *this;
    }

    Regex::~Regex() {
        delete impl;
    }

    // --- Helpers ---

    static uint32_t translateOptions(unsigned options) {
        uint32_t flags = 0;
        if (options & static_cast<unsigned>(RegexOption::CaseInsensitive))
            flags |= PCRE2_CASELESS;
        if (options & static_cast<unsigned>(RegexOption::Multiline))
            flags |= PCRE2_MULTILINE;
        if (options & static_cast<unsigned>(RegexOption::DotAll))
            flags |= PCRE2_DOTALL;
        if (options & static_cast<unsigned>(RegexOption::Utf))
            flags |= PCRE2_UTF;
        if (options & static_cast<unsigned>(RegexOption::Anchored))
            flags |= PCRE2_ANCHORED;
        return flags;
    }

    static String pcre2ErrorMessage(int errorCode) {
        PCRE2_UCHAR buffer[256];
        int len = pcre2_get_error_message(errorCode, buffer, sizeof(buffer));
        if (len > 0)
            return String(reinterpret_cast<const char *>(buffer), (size_t)len);
        return "Unknown PCRE2 error";
    }

    static RegexMatch buildMatch(StrRef input, pcre2_match_data *matchData, uint32_t captureCount) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);

        RegexCapture full{
            ovector[0],
            ovector[1],
            StrRef(input.data() + ovector[0], (StrRef::size_type)(ovector[1] - ovector[0]))
        };

        Vector<RegexCapture> caps;
        for (uint32_t i = 1; i < captureCount; ++i) {
            if (ovector[2 * i] != PCRE2_UNSET) {
                size_t s = ovector[2 * i];
                size_t e = ovector[2 * i + 1];
                caps.push_back({s, e, StrRef(input.data() + s, (StrRef::size_type)(e - s))});
            } else {
                caps.push_back({0, 0, StrRef()});
            }
        }
        return RegexMatch{std::move(full), std::move(caps)};
    }

    // --- Regex API ---

    Result<Regex, RegexError> Regex::compile(StrRef pattern, unsigned options) {
        int errorCode;
        PCRE2_SIZE errorOffset;
        uint32_t pcre2Opts = translateOptions(options);

        pcre2_code *code = pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>(pattern.data()),
            pattern.size(),
            pcre2Opts,
            &errorCode,
            &errorOffset,
            nullptr);

        if (!code) {
            RegexError err;
            err.code = errorCode;
            err.offset = (size_t)errorOffset;
            err.message = pcre2ErrorMessage(errorCode);
            return Result<Regex, RegexError>::err(std::move(err));
        }

        return Result<Regex, RegexError>::ok(Regex(new RegexImpl{code}));
    }

    bool Regex::matches(StrRef input) const {
        pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(impl->code, nullptr);
        int rc = pcre2_match(
            impl->code,
            reinterpret_cast<PCRE2_SPTR>(input.data()),
            input.size(),
            0,
            PCRE2_ANCHORED,
            matchData,
            nullptr);

        bool result = false;
        if (rc >= 0) {
            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
            result = (ovector[0] == 0 && ovector[1] == input.size());
        }
        pcre2_match_data_free(matchData);
        return result;
    }

    Optional<RegexMatch> Regex::search(StrRef input) const {
        return searchFrom(input, 0);
    }

    Optional<RegexMatch> Regex::searchFrom(StrRef input, size_t startOffset) const {
        pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(impl->code, nullptr);
        int rc = pcre2_match(
            impl->code,
            reinterpret_cast<PCRE2_SPTR>(input.data()),
            input.size(),
            startOffset,
            0,
            matchData,
            nullptr);

        if (rc < 0) {
            pcre2_match_data_free(matchData);
            return {};
        }

        uint32_t captureCount = (rc == 0)
            ? pcre2_get_ovector_count(matchData)
            : (uint32_t)rc;

        RegexMatch match = buildMatch(input, matchData, captureCount);
        pcre2_match_data_free(matchData);
        return match;
    }

    Vector<RegexMatch> Regex::findAll(StrRef input) const {
        Vector<RegexMatch> results;
        size_t offset = 0;

        while (offset <= input.size()) {
            pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(impl->code, nullptr);
            int rc = pcre2_match(
                impl->code,
                reinterpret_cast<PCRE2_SPTR>(input.data()),
                input.size(),
                offset,
                0,
                matchData,
                nullptr);

            if (rc < 0) {
                pcre2_match_data_free(matchData);
                break;
            }

            uint32_t captureCount = (rc == 0)
                ? pcre2_get_ovector_count(matchData)
                : (uint32_t)rc;

            RegexMatch match = buildMatch(input, matchData, captureCount);
            pcre2_match_data_free(matchData);

            size_t matchEnd = match.fullMatch.end;
            results.push_back(std::move(match));

            // Advance past empty matches to avoid infinite loop
            if (matchEnd == offset)
                offset++;
            else
                offset = matchEnd;
        }
        return results;
    }

    Result<String, RegexError> Regex::replace(StrRef input, StrRef replacement) const {
        // First call to determine output length
        PCRE2_SIZE outLen = 0;
        int rc = pcre2_substitute(
            impl->code,
            reinterpret_cast<PCRE2_SPTR>(input.data()),
            input.size(),
            0,
            PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
            nullptr,
            nullptr,
            reinterpret_cast<PCRE2_SPTR>(replacement.data()),
            replacement.size(),
            nullptr,
            &outLen);

        if (rc != PCRE2_ERROR_NOMEMORY && rc < 0) {
            RegexError err;
            err.code = rc;
            err.offset = 0;
            err.message = pcre2ErrorMessage(rc);
            return Result<String, RegexError>::err(std::move(err));
        }

        // Allocate output buffer and perform substitution
        Vector<PCRE2_UCHAR> outBuf(outLen);
        PCRE2_SIZE finalLen = outLen;
        rc = pcre2_substitute(
            impl->code,
            reinterpret_cast<PCRE2_SPTR>(input.data()),
            input.size(),
            0,
            PCRE2_SUBSTITUTE_GLOBAL,
            nullptr,
            nullptr,
            reinterpret_cast<PCRE2_SPTR>(replacement.data()),
            replacement.size(),
            outBuf.data(),
            &finalLen);

        if (rc < 0) {
            RegexError err;
            err.code = rc;
            err.offset = 0;
            err.message = pcre2ErrorMessage(rc);
            return Result<String, RegexError>::err(std::move(err));
        }

        return Result<String, RegexError>::ok(
            String(reinterpret_cast<const char *>(outBuf.data()), finalLen));
    }

    Vector<String> Regex::split(StrRef input) const {
        Vector<String> parts;
        size_t lastEnd = 0;

        pcre2_match_data *matchData = pcre2_match_data_create_from_pattern(impl->code, nullptr);
        size_t offset = 0;

        while (offset <= input.size()) {
            int rc = pcre2_match(
                impl->code,
                reinterpret_cast<PCRE2_SPTR>(input.data()),
                input.size(),
                offset,
                0,
                matchData,
                nullptr);

            if (rc < 0)
                break;

            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
            size_t matchStart = ovector[0];
            size_t matchEnd = ovector[1];

            parts.push_back(String(input.data() + lastEnd, matchStart - lastEnd));
            lastEnd = matchEnd;

            if (matchEnd == offset)
                offset++;
            else
                offset = matchEnd;
        }

        pcre2_match_data_free(matchData);
        parts.push_back(String(input.data() + lastEnd, input.size() - lastEnd));
        return parts;
    }

    // --- Free helpers ---

    String regexEscape(StrRef input) {
        static const char metacharacters[] = "\\.^$|?*+()[]{}";
        String result;
        result.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            char c = input.data()[i];
            for (const char *m = metacharacters; *m; ++m) {
                if (c == *m) {
                    result.push_back('\\');
                    break;
                }
            }
            result.push_back(c);
        }
        return result;
    }

}
