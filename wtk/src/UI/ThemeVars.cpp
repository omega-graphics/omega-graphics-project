#include "omegaWTK/UI/ThemeVars.h"

#include <utility>

namespace OmegaWTK {

SharedHandle<ThemeVars> ThemeVars::Create(){
    return SharedHandle<ThemeVars>(new ThemeVars());
}

Core::Optional<StyleValue> ThemeVars::lookup(const OmegaCommon::String & name) const {
    auto it = values_.find(name);
    if(it == values_.end()){
        return {};
    }
    return it->second;
}

bool ThemeVars::empty() const {
    return values_.empty();
}

const OmegaCommon::Map<OmegaCommon::String, StyleValue> & ThemeVars::values() const {
    return values_;
}

// ---------------------------------------------------------------
// ThemeVars::Builder
// ---------------------------------------------------------------

ThemeVars::Builder & ThemeVars::Builder::set(
        const OmegaCommon::String & name, StyleValue value){
    // Last write wins — matches `StyleSheet::Builder::addKeyframeAnimation`
    // for name collisions.
    values_[name] = std::move(value);
    return *this;
}

SharedHandle<ThemeVars> ThemeVars::Builder::build() const {
    auto theme = SharedHandle<ThemeVars>(new ThemeVars());
    theme->values_ = values_;
    return theme;
}

} // namespace OmegaWTK
