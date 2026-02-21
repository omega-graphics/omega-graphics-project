#include "omegaWTK/UI/Widget.h"

#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_WIDGETS_CONTAINERS_H
#define OMEGAWTK_WIDGETS_CONTAINERS_H

namespace OmegaWTK {

struct OMEGAWTK_EXPORT StackInsets {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

enum class StackAxis : uint8_t {
    Horizontal,
    Vertical
};

enum class StackMainAlign : uint8_t {
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly
};

enum class StackCrossAlign : uint8_t {
    Start,
    Center,
    End,
    Stretch
};

struct OMEGAWTK_EXPORT StackOptions {
    float spacing = 0.f;
    StackInsets padding {};
    StackMainAlign mainAlign = StackMainAlign::Start;
    StackCrossAlign crossAlign = StackCrossAlign::Start;
    bool clipOverflow = false;
};

struct OMEGAWTK_EXPORT StackSlot {
    float flexGrow = 0.f;
    float flexShrink = 1.f;
    Core::Optional<float> basis {};
    Core::Optional<float> minMain {};
    Core::Optional<float> maxMain {};
    Core::Optional<float> minCross {};
    Core::Optional<float> maxCross {};
    StackInsets margin {};
    Core::Optional<StackCrossAlign> alignSelf {};
};

class OMEGAWTK_EXPORT StackWidget : public Widget {
public:
    struct ChildEntry {
        WidgetPtr widget;
        StackSlot slot;
    };
private:
    StackAxis axis;
    StackOptions options;
    OmegaCommon::Vector<ChildEntry> stackChildren;
    bool needsLayout = true;
    bool inLayout = false;

    void layoutChildren();
protected:
    void onMount() override;
    void onPaint(PaintContext & context,PaintReason reason) override;
    void resize(Core::Rect & newRect) override;
public:
    StackWidget(StackAxis axis,const Core::Rect & rect,WidgetPtr parent,const StackOptions & options = {});

    StackAxis getAxis() const;
    const StackOptions & getOptions() const;
    void setOptions(const StackOptions & options);

    std::size_t childCount() const;
    WidgetPtr childAt(std::size_t idx) const;

    WidgetPtr addChild(const WidgetPtr & child,const StackSlot & slot = {});
    bool removeChild(const WidgetPtr & child);
    bool setSlot(const WidgetPtr & child,const StackSlot & slot);
    bool setSlot(std::size_t idx,const StackSlot & slot);
    Core::Optional<StackSlot> getSlot(const WidgetPtr & child) const;

    void relayout();

    ~StackWidget() override;
};

class OMEGAWTK_EXPORT HStack : public StackWidget {
public:
    explicit HStack(const Core::Rect & rect,WidgetPtr parent,const StackOptions & options = {});
};

class OMEGAWTK_EXPORT VStack : public StackWidget {
public:
    explicit VStack(const Core::Rect & rect,WidgetPtr parent,const StackOptions & options = {});
};

}

#endif // OMEGAWTK_WIDGETS_CONTAINERS_H
