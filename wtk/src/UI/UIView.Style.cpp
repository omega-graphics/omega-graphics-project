#include "UIViewImpl.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/StyleResolver.h"
#include "FrameBuilder.h"

namespace OmegaWTK {

namespace UIViewInternal {

namespace {

bool matchesTag(const OmegaCommon::String & selector,const OmegaCommon::String & tag){
    return selector.empty() || selector == tag;
}

}

ResolvedViewStyle resolveViewStyle(const StylePtr & style,const UIViewTag & viewTag){
    ResolvedViewStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    for(const auto & entry : style->entries){
        if(!matchesTag(entry.viewTag,viewTag)){
            continue;
        }
        switch(entry.kind){
            case Style::Entry::Kind::BackgroundColor:
                if(entry.color){
                    resolved.backgroundColor = entry.color;
                }
                break;
            case Style::Entry::Kind::BorderEnabled:
                if(entry.boolValue){
                    resolved.useBorder = *entry.boolValue;
                }
                break;
            case Style::Entry::Kind::BorderColor:
                if(entry.color){
                    resolved.borderColor = entry.color;
                }
                break;
            case Style::Entry::Kind::BorderWidth:
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

SharedHandle<Composition::Brush> resolveElementBrush(const StylePtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag){
    auto brush = Composition::ColorBrush(
            Composition::Color::create8Bit(Composition::Color::White8));

    if(style == nullptr){
        return brush;
    }

    for(const auto & entry : style->entries){
        if(entry.kind != Style::Entry::Kind::ElementBrush){
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

ResolvedTextStyle resolveTextStyle(const StylePtr & style,
                                   const UIViewTag & viewTag,
                                   const UIElementTag & elementTag){
    ResolvedTextStyle resolved {};
    if(style == nullptr){
        return resolved;
    }

    auto entrySpecificity = [&](const Style::Entry & entry) -> int {
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
            case Style::Entry::Kind::TextFont:
                if(entry.font != nullptr &&
                   takeCandidate(specificity,idx,fontSpecificity,fontOrder)){
                    resolved.font = entry.font;
                    fontSpecificity = specificity;
                    fontOrder = idx;
                }
                break;
            case Style::Entry::Kind::TextColor:
                if(entry.color &&
                   takeCandidate(specificity,idx,colorSpecificity,colorOrder)){
                    resolved.color = *entry.color;
                    colorSpecificity = specificity;
                    colorOrder = idx;
                }
                break;
            case Style::Entry::Kind::TextAlignment:
                if(entry.textAlignment &&
                   takeCandidate(specificity,idx,alignmentSpecificity,alignmentOrder)){
                    resolved.layout.alignment = *entry.textAlignment;
                    alignmentSpecificity = specificity;
                    alignmentOrder = idx;
                }
                break;
            case Style::Entry::Kind::TextWrapping:
                if(entry.textWrapping &&
                   takeCandidate(specificity,idx,wrappingSpecificity,wrappingOrder)){
                    resolved.layout.wrapping = *entry.textWrapping;
                    wrappingSpecificity = specificity;
                    wrappingOrder = idx;
                }
                break;
            case Style::Entry::Kind::TextLineLimit:
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

ResolvedEffectStyle resolveElementEffectStyle(const StylePtr & style,
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
            case Style::Entry::Kind::DropShadowEffect:
                if(entry.dropShadowValue){
                    resolved.dropShadow = *entry.dropShadowValue;
                    resolved.dropShadowTransition.transition = entry.transition;
                    resolved.dropShadowTransition.duration = entry.duration;
                    resolved.dropShadowTransition.curve = entry.curve;
                }
                break;
            case Style::Entry::Kind::GaussianBlurEffect:
                if(entry.gaussianBlurValue){
                    resolved.gaussianBlur = *entry.gaussianBlurValue;
                    resolved.gaussianBlurTransition.transition = entry.transition;
                    resolved.gaussianBlurTransition.duration = entry.duration;
                    resolved.gaussianBlurTransition.curve = entry.curve;
                }
                break;
            case Style::Entry::Kind::DirectionalBlurEffect:
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

StyleScope collectStyleScope(const StylePtr & style,const UIViewTag & viewTag){
    StyleScope scope {};
    if(style == nullptr){
        return scope;
    }

    for(const auto & entry : style->entries){
        if(!entry.viewTag.empty() && !matchesTag(entry.viewTag,viewTag)){
            continue;
        }

        switch(entry.kind){
            case Style::Entry::Kind::BackgroundColor:
            case Style::Entry::Kind::BorderEnabled:
            case Style::Entry::Kind::BorderColor:
            case Style::Entry::Kind::BorderWidth:
                scope.touchesRoot = true;
                break;
            case Style::Entry::Kind::DropShadowEffect:
            case Style::Entry::Kind::GaussianBlurEffect:
            case Style::Entry::Kind::DirectionalBlurEffect:
                if(entry.elementTag.empty()){
                    scope.touchesRoot = true;
                }
                else {
                    addUniqueTag(scope.elementTags,entry.elementTag);
                }
                break;
            case Style::Entry::Kind::ElementBrush:
            case Style::Entry::Kind::ElementBrushAnimation:
            case Style::Entry::Kind::ElementAnimation:
            case Style::Entry::Kind::ElementPathAnimation:
            case Style::Entry::Kind::TextFont:
            case Style::Entry::Kind::TextColor:
            case Style::Entry::Kind::TextAlignment:
            case Style::Entry::Kind::TextWrapping:
            case Style::Entry::Kind::TextLineLimit:
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

AnimationScheduler * UIView::Impl::activeAnimationScheduler() const {
    auto * fb = AppWindow::activeFrameBuilder();
    return (fb != nullptr) ? fb->animationScheduler() : nullptr;
}

void UIView::resolveStyles(){
    // Tier B / B5: cell writes happen only in the Style phase.
    if(auto * fb = AppWindow::activeFrameBuilder(); fb != nullptr){
        fb->assertPhase(FramePhase::Style);
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // resolved-style writes now flow into the per-property
    // `styleTable_` keyed by `(NodeId, PropertyKey, subIndex)` —
    // same shape the AnimationScheduler side-table uses. The
    // aggregate `Resolved*Style` builders survive as transient
    // resolvers; this method splits their fields into one cell each
    // and lets Paint query through `resolved<T>(n, k, fallback)`.
    // The pre-D5 `resolvedViewStyle_` + `computedStyles_` aggregate
    // stores are deleted — nothing lives between resolveStyles() and
    // paint() except the cell table.
    impl_->styleTable_.clear();

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.3 (2026-06-03):
    // sheet cascade. The resolver walks the owning AppWindow's
    // style-sheet stack and writes one cell per resolved property
    // per matching node. The inline-`Style` writes below this call
    // overwrite cells on overlap — the layered shape decided
    // 2026-06-03 (inline wins).
    StyleSheets::StyleResolver::apply(*this);

    const auto viewNodeId = nodeId();
    const auto resolvedView =
        UIViewInternal::resolveViewStyle(impl_->currentStyle,impl_->tag);
    if(resolvedView.backgroundColor){
        impl_->styleTable_.set<Composition::Color>(
            viewNodeId, PropertyKey::BackgroundColor, *resolvedView.backgroundColor);
    }
    // useBorder / borderColor / borderWidth are resolved into
    // ResolvedViewStyle but never read in Paint today (the pre-D5
    // aggregate didn't surface them either). They stay unresolved
    // here — a future widget that wants a real border will write
    // BorderColor / BorderWidth cells through this same path.

    for(const auto & spec : impl_->currentLayoutV2_.elements()){
        const auto elementNodeId = impl_->ensureElementNodeId(spec.tag);

        auto brush = UIViewInternal::resolveElementBrush(
            impl_->currentStyle,impl_->tag,spec.tag);
        if(brush != nullptr){
            impl_->styleTable_.set<SharedHandle<Composition::Brush>>(
                elementNodeId, PropertyKey::FillBrush, brush);
        }

        const auto resolvedEffects = UIViewInternal::resolveElementEffectStyle(
            impl_->currentStyle,impl_->tag,spec.tag);
        if(resolvedEffects.dropShadow){
            impl_->styleTable_.set<Composition::LayerEffect::DropShadowParams>(
                elementNodeId, PropertyKey::DropShadow, *resolvedEffects.dropShadow);
        }
        // gaussianBlur / directionalBlur (and the three Transition
        // blocks) resolve but no Paint reader pulls them in the
        // current tree — same status as pre-D5 (the aggregate
        // carried them too, equally unread). D6 / D7 will wire the
        // readers as the Style cascade lights them up.

        // Text resolves against the element's text-style tag (which
        // may alias a shared style element), then writes cells under
        // the element's OWN NodeId so Paint reads by element identity.
        const UIElementTag textStyleTag = spec.textStyleTag.value_or(spec.tag);
        const auto resolvedText = UIViewInternal::resolveTextStyle(
            impl_->currentStyle,impl_->tag,textStyleTag);
        if(resolvedText.font != nullptr){
            impl_->styleTable_.set<SharedHandle<Composition::Font>>(
                elementNodeId, PropertyKey::TextFont, resolvedText.font);
        }
        impl_->styleTable_.set<Composition::Color>(
            elementNodeId, PropertyKey::TextColor, resolvedText.color);
        impl_->styleTable_.set<Composition::TextLayoutDescriptor>(
            elementNodeId, PropertyKey::TextLayout, resolvedText.layout);
        impl_->styleTable_.set<std::uint32_t>(
            elementNodeId, PropertyKey::TextLineLimit,
            static_cast<std::uint32_t>(resolvedText.lineLimit));
    }
}

void UIView::setStyle(const StylePtr &style){
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
