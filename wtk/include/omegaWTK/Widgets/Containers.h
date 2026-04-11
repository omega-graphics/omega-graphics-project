#include "omegaWTK/Widgets/BasicWidgets.h"

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

class OMEGAWTK_EXPORT StackWidget : public Container {
    struct StackChildCache {
        float preferredMainSize = 0.f;
        float preferredCrossSize = 0.f;
        bool hasPreferredSize = false;
    };

    StackAxis axis;
    StackOptions stackOptions;
    OmegaCommon::Vector<StackSlot> childSlots;
    OmegaCommon::Vector<StackChildCache> childSizeCache;
    bool needsLayout = true;
    bool hasLastStableFrame = false;
    Composition::Rect lastStableFrame {Composition::Point2D{0.f,0.f},1.f,1.f};

protected:
    void layoutChildren() override;
    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Composition::Rect & newRect) override;
public:
    StackWidget(StackAxis axis,Composition::Rect rect,const StackOptions & options = {});
    StackWidget(StackAxis axis,ViewPtr view,const StackOptions & options = {});

    StackAxis getAxis() const;
    const StackOptions & getOptions() const;
    void setOptions(const StackOptions & options);

    WidgetPtr addChild(const WidgetPtr & child) override;
    WidgetPtr addChild(const WidgetPtr & child,const StackSlot & slot);
    bool removeChild(const WidgetPtr & child) override;
    bool setSlot(const WidgetPtr & child,const StackSlot & slot);
    bool setSlot(std::size_t idx,const StackSlot & slot);
    Core::Optional<StackSlot> getSlot(const WidgetPtr & child) const;

    void relayout();

    ~StackWidget() override;
};

class OMEGAWTK_EXPORT HStack : public StackWidget {
public:
    explicit HStack(Composition::Rect rect,const StackOptions & options = {});
    explicit HStack(ViewPtr view,const StackOptions & options = {});
};

class OMEGAWTK_EXPORT VStack : public StackWidget {
public:
    explicit VStack(Composition::Rect rect,const StackOptions & options = {});
    explicit VStack(ViewPtr view,const StackOptions & options = {});
};

}

#endif // OMEGAWTK_WIDGETS_CONTAINERS_H
