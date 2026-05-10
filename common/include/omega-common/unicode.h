#ifndef OMEGA_COMMON_UNICODE_H
#define OMEGA_COMMON_UNICODE_H

#include <cstdint>
#include <string>

#include "omega-common/utils.h"

namespace OmegaCommon {

    using UnicodeChar = char16_t;
    using Unicode32Char = char32_t;

    /**
     * A lightweight UTF-16 string wrapper. ICU is kept in the implementation
     * so including this header stays cheap and consumers don't need to link
     * ICU directly — OmegaCommon does that internally.
     */
    class OMEGACOMMON_EXPORT UniString {
        std::u16string data_;

        explicit UniString(std::u16string data);

    public:
        UniString() = default;
        explicit UniString(const char * utf8);

        static UniString fromUTF8(const char * utf8);
        static UniString fromUTF32(const Unicode32Char * utf32, std::int32_t length);

        std::int32_t length() const;
        const UnicodeChar * getBuffer() const;
    };

}

#endif
