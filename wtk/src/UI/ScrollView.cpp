#include "omegaWTK/UI/ScrollView.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/Brush.h"

#include <algorithm>

namespace OmegaWTK {

    namespace {
        constexpr float kScrollBarThickness = 6.f;
        constexpr float kScrollBarMargin = 2.f;
        constexpr float kScrollBarRadius = 3.f;
        constexpr float kMinThumbLength = 20.f;
    }

    void ScrollView::DefaultScrollHandler::onRecieveEvent(Native::NativeEventPtr event){
        if(event == nullptr || owner == nullptr){
            return;
        }
        if(event->type == Native::NativeEvent::ScrollWheel){
            auto *p = static_cast<Native::ScrollParams *>(event->params);
            if(p != nullptr){
                Core::Position newOffset = owner->scrollOffset;
                if(owner->hasHorizontalScrollBar){
                    newOffset.x -= p->deltaX;
                }
                if(owner->hasVerticalScrollBar){
                    newOffset.y -= p->deltaY;
                }
                newOffset.x = std::max(0.f, newOffset.x);
                newOffset.y = std::max(0.f, newOffset.y);
                owner->setScrollOffset(newOffset);
            }
        }
    }

    ScrollView::ScrollView(const Core::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, ViewPtr parent):
            View(rect, parent),
            child(child),
            hasVerticalScrollBar(hasVerticalScrollBar),
            hasHorizontalScrollBar(hasHorizontalScrollBar){
        defaultHandler.owner = this;
        setReciever(&defaultHandler);
        if(child != nullptr){
            addSubView(child.get());
        }
        // Create scroll bar overlay layers.
        if(hasVerticalScrollBar){
            Core::Rect vBarRect {
                Core::Position{rect.w - kScrollBarThickness - kScrollBarMargin, kScrollBarMargin},
                kScrollBarThickness,
                rect.h - 2.f * kScrollBarMargin
            };
            vScrollBarLayer = makeLayer(vBarRect);
            vScrollBarCanvas = makeCanvas(vScrollBarLayer);
        }
        if(hasHorizontalScrollBar){
            Core::Rect hBarRect {
                Core::Position{kScrollBarMargin, rect.h - kScrollBarThickness - kScrollBarMargin},
                rect.w - 2.f * kScrollBarMargin,
                kScrollBarThickness
            };
            hScrollBarLayer = makeLayer(hBarRect);
            hScrollBarCanvas = makeCanvas(hScrollBarLayer);
        }
    };

    void ScrollView::paintScrollBars(){
        auto & viewRect = getRect();
        if(child == nullptr){
            return;
        }
        auto & contentRect = child->getRect();

        if(hasVerticalScrollBar && vScrollBarCanvas != nullptr){
            float trackH = viewRect.h - 2.f * kScrollBarMargin;
            float contentH = contentRect.h;
            if(contentH <= viewRect.h){
                contentH = viewRect.h + 1.f;
            }
            float thumbRatio = viewRect.h / contentH;
            float thumbH = std::max(kMinThumbLength, trackH * thumbRatio);
            float scrollRatio = scrollOffset.y / (contentH - viewRect.h);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            float thumbY = scrollRatio * (trackH - thumbH);

            // Update layer rect.
            Core::Rect vBarRect {
                Core::Position{viewRect.w - kScrollBarThickness - kScrollBarMargin, kScrollBarMargin},
                kScrollBarThickness,
                trackH
            };
            vScrollBarLayer->resize(vBarRect);

            startCompositionSession();
            vScrollBarCanvas->clear();
            Core::RoundedRect thumbRect {
                {0.f, thumbY},
                kScrollBarThickness, thumbH,
                kScrollBarRadius, kScrollBarRadius
            };
            auto brush = Composition::ColorBrush(
                Composition::Color{0.5f, 0.5f, 0.5f, 0.6f});
            vScrollBarCanvas->drawRoundedRect(thumbRect, brush);
            vScrollBarCanvas->sendFrame();
            endCompositionSession();
        }

        if(hasHorizontalScrollBar && hScrollBarCanvas != nullptr){
            float trackW = viewRect.w - 2.f * kScrollBarMargin;
            float contentW = contentRect.w;
            if(contentW <= viewRect.w){
                contentW = viewRect.w + 1.f;
            }
            float thumbRatio = viewRect.w / contentW;
            float thumbW = std::max(kMinThumbLength, trackW * thumbRatio);
            float scrollRatio = scrollOffset.x / (contentW - viewRect.w);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            float thumbX = scrollRatio * (trackW - thumbW);

            Core::Rect hBarRect {
                Core::Position{kScrollBarMargin, viewRect.h - kScrollBarThickness - kScrollBarMargin},
                trackW,
                kScrollBarThickness
            };
            hScrollBarLayer->resize(hBarRect);

            startCompositionSession();
            hScrollBarCanvas->clear();
            Core::RoundedRect thumbRect {
                {thumbX, 0.f},
                thumbW, kScrollBarThickness,
                kScrollBarRadius, kScrollBarRadius
            };
            auto brush = Composition::ColorBrush(
                Composition::Color{0.5f, 0.5f, 0.5f, 0.6f});
            hScrollBarCanvas->drawRoundedRect(thumbRect, brush);
            hScrollBarCanvas->sendFrame();
            endCompositionSession();
        }
    }

    bool ScrollView::hasDelegate(){
        return delegate != nullptr;
    };

    void ScrollView::setDelegate(ScrollViewDelegate *_delegate){
        if(_delegate == nullptr){
            delegate = nullptr;
            setReciever(&defaultHandler);
            return;
        }
        delegate = _delegate;
        delegate->scrollView = this;
        setReciever(delegate);
    };

    void ScrollView::setScrollOffset(const Core::Position & offset){
        scrollOffset = offset;
        paintScrollBars();
    }

    Core::Position ScrollView::scrollOffsetContribution() const{
        return scrollOffset;
    }

    void ScrollViewDelegate::onRecieveEvent(Native::NativeEventPtr event){
        if(event == nullptr){
            return;
        }
        switch(event->type){
            case Native::NativeEvent::ScrollWheel: {
                auto *p = static_cast<Native::ScrollParams *>(event->params);
                if(p != nullptr){
                    onScrollWheel(p->deltaX, p->deltaY);
                }
                break;
            }
            default:
                break;
        }
    };

};
