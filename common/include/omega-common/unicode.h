#ifndef OMEGA_COMMON_UNICODE_H
#define OMEGA_COMMON_UNICODE_H

#include <cstdint>
#include <memory>
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

        /// Encode the contents as a UTF-8 byte string. Round-trips with
        /// fromUTF8. Returns an empty string on an encoding error.
        String toUTF8() const;
        /// Encode the contents as a UTF-32 code-point string. Round-trips
        /// with fromUTF32. Returns an empty string on an encoding error.
        UString toUTF32() const;

        std::int32_t length() const;
        const UnicodeChar * getBuffer() const;
    };

    /**
     * A pImpl wrapper around ICU's BreakIterator. Hides
     * `unicode/brkiter.h` from public headers; consumers keep ICU as
     * a private OmegaCommon dependency. Used by the WTK text layout
     * engine (Text-Layout-Engine-Plan.md, Phase 2) to find line-break
     * opportunities; Phase 2 only honors *mandatory* breaks (\n,
     * U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR), but the
     * iterator exposes optional breaks too for the wrap pass that
     * lands in Phase 3.
     *
     * Not thread-safe — each thread should own its iterator. The
     * underlying ICU object internalizes the text on construction
     * (no shared reference to the source `UniString`).
     */
    class OMEGACOMMON_EXPORT BreakIterator {
    public:
        enum class Type {
            /// Line break opportunities (mandatory + optional).
            Line,
            /// Word boundaries.
            Word
        };

        /// Sentinel returned by `next()` after the last boundary,
        /// matching ICU's `UBRK_DONE`.
        static constexpr std::int32_t DONE = -1;

        BreakIterator(Type type, const UniString & text);
        ~BreakIterator();

        BreakIterator(const BreakIterator &) = delete;
        BreakIterator & operator=(const BreakIterator &) = delete;

        /// Reset to the first boundary (always 0 for non-empty text).
        std::int32_t first();
        /// Advance to the next boundary or return `DONE`.
        std::int32_t next();

        /// True when the boundary at `position` was a mandatory line
        /// break (newline, U+2028, U+2029, paragraph separator).
        /// Phase 2's layout engine uses this to gate line creation
        /// against the no-wrap contract. Only meaningful for
        /// `Type::Line` iterators; returns false for `Type::Word`.
        bool isMandatoryBreakAt(std::int32_t position) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /**
     * A pImpl wrapper around ICU's `UBiDi`. Carries one analyzed
     * paragraph (the input text) and exposes its *visual* runs — the
     * subsequence of logical positions plus their resolved direction.
     * Used by the WTK text layout engine (Text-Layout-Engine-Plan.md,
     * Phase 3) to feed HarfBuzz one direction-correct sub-run at a
     * time.
     *
     * The default paragraph level is auto-detected (`UBIDI_DEFAULT_LTR`)
     * — the analyzer scans for the first strong character and uses
     * its direction. Phase 3's layout engine wraps one analyzer per
     * logical line; the BiDi algorithm does not cross line boundaries
     * itself, so each line is its own paragraph.
     *
     * Not thread-safe. Internalizes the text on construction (no
     * shared reference to the source `UniString`).
     */
    class OMEGACOMMON_EXPORT BidiParagraph {
    public:
        /// One visual run produced by `getVisualRun`. `logicalStart`
        /// is the offset into the *original* text where this run
        /// begins (so the caller can substring with `[logicalStart,
        /// logicalStart + length)`); `rightToLeft` reflects the run's
        /// resolved embedding level (odd levels are RTL).
        struct VisualRun {
            std::int32_t logicalStart = 0;
            std::int32_t length       = 0;
            bool         rightToLeft  = false;
        };

        explicit BidiParagraph(const UniString & text);
        ~BidiParagraph();

        BidiParagraph(const BidiParagraph &) = delete;
        BidiParagraph & operator=(const BidiParagraph &) = delete;

        /// Number of visual runs the analyzer produced. Always 1 for
        /// pure-LTR or pure-RTL text; larger for mixed-direction
        /// paragraphs.
        std::int32_t runCount() const;

        /// Get the run at the given visual index (`0 ≤ index <
        /// runCount()`). Runs are returned in *visual* order: index 0
        /// is the leftmost run on screen, regardless of its logical
        /// position. Out-of-range indices return a zero-length run.
        VisualRun getVisualRun(std::int32_t index) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /**
     * Forward iterator over consecutive same-script ranges in a
     * `UniString`. Phase-3 layout uses this *after* `BidiParagraph`
     * to split each visual run into single-script segments before
     * shaping, so HarfBuzz sees one (text, font, direction, script)
     * tuple per shape call.
     *
     * "Same script" means `uscript_getScript` returns the same
     * `UScriptCode` for every codepoint in the range, with the usual
     * exception that `USCRIPT_COMMON` and `USCRIPT_INHERITED` (e.g.,
     * digits, combining marks, punctuation) extend the *previous*
     * run rather than starting a new one.
     */
    class OMEGACOMMON_EXPORT ScriptRunIterator {
    public:
        /// One run produced by `next`. `script` is an ICU
        /// `UScriptCode` value stored as `int32_t` so callers don't
        /// need to include `unicode/uscript.h`.
        struct Run {
            std::int32_t start  = 0;
            std::int32_t length = 0;
            std::int32_t script = 0; // UScriptCode (USCRIPT_COMMON = 0)
        };

        /// Iterate over the substring `[start, start + length)` of
        /// `text`. The text reference is held only for the
        /// constructor's duration — runs are pre-computed.
        ScriptRunIterator(const UniString & text,
                          std::int32_t start,
                          std::int32_t length);
        ~ScriptRunIterator();

        ScriptRunIterator(const ScriptRunIterator &) = delete;
        ScriptRunIterator & operator=(const ScriptRunIterator &) = delete;

        /// Get the next run. Returns false when the iterator is
        /// exhausted; the output `Run` is undefined in that case.
        bool next(Run & out);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}

#endif
