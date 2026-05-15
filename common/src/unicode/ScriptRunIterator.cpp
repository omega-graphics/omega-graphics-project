// Forward iterator over consecutive same-script ranges. Pre-computes
// the runs in the constructor so iteration is just a vector walk.
//
// `USCRIPT_COMMON` and `USCRIPT_INHERITED` extend the *previous*
// strong-script run (so digits / punctuation / combining marks don't
// fragment otherwise-uniform text). The first run inherits whatever
// script its first non-common codepoint reports; if the entire range
// is `COMMON`, the run is reported as `USCRIPT_COMMON`.
//
// Used by WTK's text layout engine (Text-Layout-Engine-Plan.md
// Phase 3) to sub-segment each BiDi visual run into single-script
// shape calls.

#include "omega-common/unicode.h"

#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/utf16.h>
#include <unicode/utypes.h>

#include <vector>

namespace OmegaCommon {

    struct ScriptRunIterator::Impl {
        std::vector<Run> runs;
        std::size_t cursor = 0;
    };

    namespace {
        // Returns true when `script` should be treated as a
        // run-continuation rather than a run-boundary trigger.
        bool isCommonScript(UScriptCode script){
            return script == USCRIPT_COMMON || script == USCRIPT_INHERITED;
        }
    }

    ScriptRunIterator::ScriptRunIterator(const UniString & text,
                                         std::int32_t start,
                                         std::int32_t length)
        : impl_(std::make_unique<Impl>()) {
        const auto * buf = text.getBuffer();
        const auto totalLen = text.length();
        if(buf == nullptr || start < 0 || length <= 0 ||
           start + length > totalLen){
            return;
        }
        const std::int32_t end = start + length;

        std::int32_t cursor = start;
        std::int32_t runStart = start;
        UScriptCode currentScript = USCRIPT_COMMON;
        bool runHasStrongScript = false;

        while(cursor < end){
            UChar32 codepoint = 0;
            const std::int32_t prevCursor = cursor;
            U16_NEXT(buf, cursor, end, codepoint);
            UErrorCode err = U_ZERO_ERROR;
            UScriptCode cpScript = uscript_getScript(codepoint, &err);
            if(U_FAILURE(err)) cpScript = USCRIPT_COMMON;

            if(!runHasStrongScript){
                // First codepoint of a run, or all-common so far —
                // the first strong script defines the run's script.
                if(!isCommonScript(cpScript)){
                    currentScript = cpScript;
                    runHasStrongScript = true;
                }
                continue;
            }

            if(isCommonScript(cpScript) || cpScript == currentScript){
                // Continuation of the current run.
                continue;
            }

            // Script change: emit the run up to this codepoint and
            // start a new one beginning at `prevCursor`.
            Run r;
            r.start  = runStart;
            r.length = prevCursor - runStart;
            r.script = static_cast<std::int32_t>(currentScript);
            impl_->runs.push_back(r);

            runStart       = prevCursor;
            currentScript  = cpScript;
            runHasStrongScript = true;
        }

        // Tail: a leftover run covering everything from `runStart`
        // to `end`. Even an all-common range gets emitted (as
        // USCRIPT_COMMON) so callers don't lose those codepoints.
        if(runStart < end){
            Run r;
            r.start  = runStart;
            r.length = end - runStart;
            r.script = static_cast<std::int32_t>(currentScript);
            impl_->runs.push_back(r);
        }
    }

    ScriptRunIterator::~ScriptRunIterator() = default;

    bool ScriptRunIterator::next(Run & out) {
        if(impl_->cursor >= impl_->runs.size()) return false;
        out = impl_->runs[impl_->cursor++];
        return true;
    }

}
