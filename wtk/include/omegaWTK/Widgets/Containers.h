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
    // Phase 4.6: bespoke flex algorithm + per-child cache deleted; the
    // public `LayoutManager`-family `FlexLayout` owns those now. The
    // widget keeps its public StackOptions / StackSlot API (existing
    // callers — tests, BasicAppTest, etc. — pass these by value), and
    // pushes them into `flexLayout_` as `FlexOptions` / `FlexChildSpec`
    // at every `addChild` / `setSlot` / `setOptions`. `childSlots`
    // stays purely so the public `getSlot` accessor keeps working.
    StackAxis                       axis;
    StackOptions                    stackOptions;
    OmegaCommon::Vector<StackSlot>  childSlots;
    FlexLayout                      flexLayout_ {};

protected:
    void onMount() override;
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

// ---------------------------------------------------------------------------
// Grid — Widget-Stub-Implementation-Plan Phase 3. A `Container` whose
// children are laid out in a fixed number of equal-width columns, filled
// row-major, with per-child column / row spans. It mirrors the
// `StackWidget` ↔ `FlexLayout` pattern exactly: the widget owns a
// `GridLayout` (the reusable layout built-in), sets it as the backing
// View's manager, and drives an arrange on mount / resize / child change.
// ---------------------------------------------------------------------------

struct OMEGAWTK_EXPORT GridOptions {
    /// Number of equal-width columns (clamped to `>= 1`).
    unsigned        columns       = 1;
    float           rowSpacing    = 0.f;
    float           columnSpacing = 0.f;
    /// How each child sits within its cell block. `Stretch` fills the cell;
    /// `Start` / `Center` / `End` keep the child's own size and position it.
    StackCrossAlign cellAlign     = StackCrossAlign::Start;
};

struct OMEGAWTK_EXPORT GridSlot {
    unsigned columnSpan = 1;
    unsigned rowSpan    = 1;
};

class OMEGAWTK_EXPORT Grid : public Container {
    GridOptions                    options_;
    GridLayout                     gridLayout_;
    OmegaCommon::Vector<GridSlot>  childSlots_;
protected:
    void onMount() override;
    void resize(Composition::Rect & newRect) override;
public:
    explicit Grid(Composition::Rect rect,const GridOptions & options = {});
    explicit Grid(ViewPtr view,const GridOptions & options = {});

    const GridOptions & getOptions() const;
    void setOptions(const GridOptions & options);

    WidgetPtr addChild(const WidgetPtr & child) override;
    WidgetPtr addChild(const WidgetPtr & child,const GridSlot & slot);
    bool removeChild(const WidgetPtr & child) override;

    void relayout();

    ~Grid() override;
};

}

#endif // OMEGAWTK_WIDGETS_CONTAINERS_H
