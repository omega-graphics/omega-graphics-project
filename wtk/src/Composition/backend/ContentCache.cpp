// Phase G.0 — ContentCacheConfig env-var resolution.
//
// Reads the three `OMEGAWTK_*` env vars exactly once, the first time
// `ContentCacheConfig::inst()` is called. The resolved values are then
// stable for the rest of the process lifetime; later modifications to the
// environment do not retroactively change cache budgets. This matches the
// pattern `ResourceTrace::enabled()` uses for the GPU resource trace
// toggle.
//
// Values are parsed as unsigned decimal integers. A missing var, an empty
// value, or a parse failure leaves the default in place. The plan's locked
// defaults — 64 MB content-cache bytes, 1024 tessellation entries, 4096
// text-shaping entries — are the struct's in-class defaults, so the parse
// step only has to overwrite on success.

#include "ContentCache.h"

#include <cerrno>
#include <cstdlib>

#include <omega-common/utils.h>

namespace OmegaWTK::Composition {

    namespace {

        bool parseSizeT(const OmegaCommon::String & raw, std::size_t & out){
            if(raw.empty()){
                return false;
            }
            errno = 0;
            char * end = nullptr;
            const unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
            if(errno != 0){
                return false;
            }
            if(end == raw.c_str() || *end != '\0'){
                return false;
            }
            out = static_cast<std::size_t>(parsed);
            return true;
        }

        void readEnvIfPresent(const char * name, std::size_t & target){
            auto raw = OmegaCommon::getEnvVar(name);
            if(!raw.has_value()){
                return;
            }
            std::size_t parsed = 0;
            if(parseSizeT(*raw, parsed)){
                target = parsed;
            }
        }

        void readEnvIfPresent(const char * name, std::uint32_t & target){
            auto raw = OmegaCommon::getEnvVar(name);
            if(!raw.has_value()){
                return;
            }
            std::size_t parsed = 0;
            if(parseSizeT(*raw, parsed)){
                target = static_cast<std::uint32_t>(parsed);
            }
        }

        // Boolean toggle env var. Enabled when present and parsing to a
        // non-zero integer (so `=1` enables, `=0` / unset / garbage stays
        // at the default). Mirrors the numeric readers above rather than
        // matching free-form "true"/"yes" spellings — the documented
        // contract is `OMEGAWTK_CONTENT_CACHE_STATS=1`.
        void readEnvBoolIfPresent(const char * name, bool & target){
            auto raw = OmegaCommon::getEnvVar(name);
            if(!raw.has_value()){
                return;
            }
            std::size_t parsed = 0;
            if(parseSizeT(*raw, parsed)){
                target = (parsed != 0);
            }
        }

        ContentCacheConfig buildConfig(){
            ContentCacheConfig cfg;
            readEnvIfPresent("OMEGAWTK_CONTENT_CACHE_BYTES",        cfg.contentCacheBytes);
            readEnvIfPresent("OMEGAWTK_TESS_CACHE_ENTRIES",         cfg.tessellationCacheEntries);
            readEnvIfPresent("OMEGAWTK_TEXT_SHAPING_CACHE_ENTRIES", cfg.textShapingCacheEntries);
            readEnvIfPresent("OMEGAWTK_CONTENT_CACHE_MIN_SIZE_PX",  cfg.cacheMinSizePx);
            readEnvBoolIfPresent("OMEGAWTK_CONTENT_CACHE_STATS",    cfg.cacheStatsEnabled);
            readEnvBoolIfPresent("OMEGAWTK_RESIZE_STRETCH",         cfg.resizeStretchEnabled);
            return cfg;
        }

    }

    const ContentCacheConfig & ContentCacheConfig::inst(){
        static const ContentCacheConfig resolved = buildConfig();
        return resolved;
    }

}
