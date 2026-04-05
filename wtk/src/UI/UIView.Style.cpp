#include "UIViewImpl.h"

namespace OmegaWTK {

namespace UIViewInternal {

namespace {

bool matchesTag(const OmegaCommon::String & selector,const OmegaCommon::String & tag){
    return selector.empty() || selector == tag;
}

}

ResolvedViewStyle resolveViewStyle(const StyleSheetPtr & style,const UIViewTag & viewTag){
    ResolvedViewStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        switch(entry.kind){
            case StyleSheet::Entry::Kind::BackgroundColor:
                if(entry.color){
                    resolved.backgroundColor = entry.color;
                }
                break;
            case StyleSheet::Entry::Kind::BorderEnabled:
                if(entry.boolValue){
                    resolved.useBorder = *entry.boolValue;
                }
                break;
            case StyleSheet::Entry::Kind::BorderColor:
                if(entry.color){
                    resolved.borderColor = entry.color;
                }
                break;
            case StyleSheet::Entry::Kind::BorderWidth:
                if(entry.floatValue){
                    resolved.borderWidth = *entry.floatValue;
                }
                break;
            default:
                break;
        }
    }
    return resolved;
}

SharedHandle<Composition::Brush> resolveElementBrush(const StyleSheetPtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag){
    auto brush = Composition::ColorBrush(
            Composition::Color::create8Bit(Composition::Color::White8));

    if(style == nullptr){
        return brush;
    }

    for(const auto & entry : style->entries){
        if(entry.kind != StyleSheet::Entry::Kind::ElementBrush){
            continue;
        }
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(!matchesTag(entry.elementTag,elementTag)){
            continue;
        }
        if(entry.brush != nullptr){
            brush = entry.brush;
        }
    }
    return brush;
}

ResolvedTextStyle resolveTextStyle(const StyleSheetPtr & style,
                                   const UIViewTag & viewTag,
                                   const UIElementTag & elementTag){
    ResolvedTextStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    auto entrySpecificity = [&](const StyleSheet::Entry & entry) -> int {
        int specificity = 0;
        if(!entry.viewTag.empty()){
            if(!matchesTag(entry.viewTag,viewTag)){
                return -1;
            }
            specificity += 1;
        }
        if(!entry.elementTag.empty()){
            if(!matchesTag(entry.elementTag,elementTag)){
                return -1;
            }
            specificity += 2;
        }
        return specificity;
    };

    auto takeCandidate = [](int candidateSpecificity,
                            std::size_t candidateOrder,
                            int & currentSpecificity,
                            std::size_t & currentOrder) -> bool {
        if(candidateSpecificity < 0){
            return false;
        }
        if(candidateSpecificity > currentSpecificity){
            return true;
        }
        return candidateSpecificity == currentSpecificity && candidateOrder >= currentOrder;
    };

    int fontSpecificity = -1;
    int colorSpecificity = -1;
    int alignmentSpecificity = -1;
    int wrappingSpecificity = -1;
    int lineLimitSpecificity = -1;
    std::size_t fontOrder = 0;
    std::size_t colorOrder = 0;
    std::size_t alignmentOrder = 0;
    std::size_t wrappingOrder = 0;
    std::size_t lineLimitOrder = 0;

    for(std::size_t idx = 0; idx < style->entries.size(); ++idx){
        const auto & entry = style->entries[idx];
        const int specificity = entrySpecificity(entry);
        if(specificity < 0){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::TextFont:
                if(entry.font != nullptr &&
                   takeCandidate(specificity,idx,fontSpecificity,fontOrder)){
                    resolved.font = entry.font;
                    fontSpecificity = specificity;
                    fontOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextColor:
                if(entry.color &&
                   takeCandidate(specificity,idx,colorSpecificity,colorOrder)){
                    resolved.color = *entry.color;
                    colorSpecificity = specificity;
                    colorOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextAlignment:
                if(entry.textAlignment &&
                   takeCandidate(specificity,idx,alignmentSpecificity,alignmentOrder)){
                    resolved.layout.alignment = *entry.textAlignment;
                    alignmentSpecificity = specificity;
                    alignmentOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextWrapping:
                if(entry.textWrapping &&
                   takeCandidate(specificity,idx,wrappingSpecificity,wrappingOrder)){
                    resolved.layout.wrapping = *entry.textWrapping;
                    wrappingSpecificity = specificity;
                    wrappingOrder = idx;
                }
                break;
            case StyleSheet::Entry::Kind::TextLineLimit:
                if(entry.uintValue &&
                   takeCandidate(specificity,idx,lineLimitSpecificity,lineLimitOrder)){
                    resolved.lineLimit = *entry.uintValue;
                    lineLimitSpecificity = specificity;
                    lineLimitOrder = idx;
                }
                break;
            default:
                break;
        }
    }

    return resolved;
}

ResolvedEffectStyle resolveElementEffectStyle(const StyleSheetPtr & style,
                                              const UIViewTag & viewTag,
                                              const UIElementTag & elementTag){
    ResolvedEffectStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        if(entry.elementTag.empty() || !matchesTag(entry.elementTag,elementTag)){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::DropShadowEffect:
                if(entry.dropShadowValue){
                    resolved.dropShadow = *entry.dropShadowValue;
                    resolved.dropShadowTransition.transition = entry.transition;
                    resolved.dropShadowTransition.duration = entry.duration;
                    resolved.dropShadowTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::GaussianBlurEffect:
                if(entry.gaussianBlurValue){
                    resolved.gaussianBlur = *entry.gaussianBlurValue;
                    resolved.gaussianBlurTransition.transition = entry.transition;
                    resolved.gaussianBlurTransition.duration = entry.duration;
                    resolved.gaussianBlurTransition.curve = entry.curve;
                }
                break;
            case StyleSheet::Entry::Kind::DirectionalBlurEffect:
                if(entry.directionalBlurValue){
                    resolved.directionalBlur = *entry.directionalBlurValue;
                    resolved.directionalBlurTransition.transition = entry.transition;
                    resolved.directionalBlurTransition.duration = entry.duration;
                    resolved.directionalBlurTransition.curve = entry.curve;
                }
                break;
            default:
                break;
        }
    }
    return resolved;
}

bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag){
    return std::find(tags.begin(),tags.end(),tag) != tags.end();
}

void addUniqueTag(OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag){
    if(tag.empty()){
        return;
    }
    if(!containsTag(tags,tag)){
        tags.push_back(tag);
    }
}

StyleScope collectStyleScope(const StyleSheetPtr & style,const UIViewTag & viewTag){
    StyleScope scope {};
    if(style == nullptr){
        return scope;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }

        switch(entry.kind){
            case StyleSheet::Entry::Kind::BackgroundColor:
            case StyleSheet::Entry::Kind::BorderEnabled:
            case StyleSheet::Entry::Kind::BorderColor:
            case StyleSheet::Entry::Kind::BorderWidth:
                scope.touchesRoot = true;
                break;
            case StyleSheet::Entry::Kind::DropShadowEffect:
            case StyleSheet::Entry::Kind::GaussianBlurEffect:
            case StyleSheet::Entry::Kind::DirectionalBlurEffect:
                if(entry.elementTag.empty()){
                    scope.touchesRoot = true;
                }
                else {
                    addUniqueTag(scope.elementTags,entry.elementTag);
                }
                break;
            case StyleSheet::Entry::Kind::ElementBrush:
            case StyleSheet::Entry::Kind::ElementBrushAnimation:
            case StyleSheet::Entry::Kind::ElementAnimation:
            case StyleSheet::Entry::Kind::ElementPathAnimation:
            case StyleSheet::Entry::Kind::TextFont:
            case StyleSheet::Entry::Kind::TextColor:
            case StyleSheet::Entry::Kind::TextAlignment:
            case StyleSheet::Entry::Kind::TextWrapping:
            case StyleSheet::Entry::Kind::TextLineLimit:
            case StyleSheet::Entry::Kind::LayoutWidth:
            case StyleSheet::Entry::Kind::LayoutHeight:
            case StyleSheet::Entry::Kind::LayoutMargin:
            case StyleSheet::Entry::Kind::LayoutPadding:
            case StyleSheet::Entry::Kind::LayoutClamp:
            case StyleSheet::Entry::Kind::LayoutTransition:
                if(entry.elementTag.empty()){
                    scope.touchesAllElements = true;
                }
                else {
                    addUniqueTag(scope.elementTags,entry.elementTag);
                }
                break;
            default:
                break;
        }
    }
    return scope;
}

}

void UIView::setStyleSheet(const StyleSheetPtr &style){
    const auto previousScope = UIViewInternal::collectStyleScope(impl_->currentStyle,impl_->tag);
    const auto nextScope = UIViewInternal::collectStyleScope(style,impl_->tag);

    impl_->currentStyle = style;
    impl_->styleDirty = true;
    impl_->styleDirtyGlobal = previousScope.touchesAllElements || nextScope.touchesAllElements;
    impl_->styleChangeRequiresCoherentFrame = impl_->styleDirtyGlobal;

    if(impl_->styleDirtyGlobal){
        impl_->markAllElementsDirty();
        impl_->firstFrameCoherentSubmit = true;
        return;
    }

    OmegaCommon::Vector<UIElementTag> affectedTags = previousScope.elementTags;
    for(const auto & nextTag : nextScope.elementTags){
        UIViewInternal::addUniqueTag(affectedTags,nextTag);
    }
    OmegaCommon::Vector<UIElementTag> expandedTags = affectedTags;
    for(const auto & spec : impl_->currentLayoutV2_.elements()){
        if(!spec.text || !spec.textStyleTag){
            continue;
        }
        if(UIViewInternal::containsTag(affectedTags,*spec.textStyleTag)){
            UIViewInternal::addUniqueTag(expandedTags,spec.tag);
        }
    }
    for(const auto & affectedTag : expandedTags){
        impl_->markElementDirty(affectedTag,false,true,true,false,false);
    }
}

}
