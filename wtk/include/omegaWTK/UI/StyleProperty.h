#ifndef OMEGAWTK_UI_STYLEPROPERTY_H
#define OMEGAWTK_UI_STYLEPROPERTY_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/Geometry.h"          // Point2D, Rect (for AnimatedValue)
#include "omegaWTK/Composition/Layer.h"             // LayerEffect::{DropShadow,Transformation}Params
#include "omegaWTK/Composition/TextLayoutEngine.h"  // TextLayoutDescriptor

#include <cstdint>
#include <variant>

namespace OmegaWTK {

/// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
/// the shared identity space the resolved-style cache, the
/// animation scheduler side-table, and the new style-sheet
/// vocabulary all key on. `NodeId` is the per-(View, element-tag)
/// identity allocated lazily through `allocateNodeId()`. Pre-D6
/// these lived inside `AnimationScheduler.h`; D6.1 lifts them to a
/// public header so `StyleSheet`s authored by app code can name
/// properties without reaching into private headers.
using NodeId = std::uint64_t;

/// Process-wide atomic counter behind `View::nodeId()` and the
/// per-`(UIView, UIElementTag)` NodeIds UIView allocates lazily.
/// Stable across the lifetime of the process; never reused.
NodeId allocateNodeId();

/// Animatable / styleable property channels. The `Layout*` keys are
/// layout-affecting (see `AnimationScheduler::isLayoutProperty`);
/// the rest are paint-only. Sheet authors and the runtime resolved-
/// style table both key on this enum.
enum class PropertyKey : std::uint16_t {
    // Visuals (read by Paint)
    BackgroundColor,
    BorderColor,
    BorderWidth,
    Opacity,
    FillBrush,
    ShadowOffsetX, ShadowOffsetY, ShadowBlur, ShadowColor,
    TransformX, TransformY,
    TransformScaleX, TransformScaleY,
    TransformRotation,
    /// Tier D / D5 (2026-06-03): aggregate drop-shadow cell.
    /// Animatable shadow channels still ride their own
    /// (ShadowOffsetX/Y, ShadowBlur, ShadowColor) scalar keys —
    /// `DropShadow` carries the resolved-style baseline the Shadow
    /// channel animations layer on top of during Paint.
    DropShadow,

    // Text
    TextColor, TextSize,
    /// Tier D / D5 (2026-06-03): text-style cells written by
    /// `resolveStyles()` and read during Paint via the `resolved<T>()`
    /// helper. `TextFont` holds a `SharedHandle<Composition::Font>`,
    /// `TextLayout` a `Composition::TextLayoutDescriptor`,
    /// `TextLineLimit` a uint32.
    TextFont, TextLayout, TextLineLimit,

    // Layout (read by the Layout phase — layout-affecting)
    LayoutWidth, LayoutHeight,
    LayoutX, LayoutY,

    // Sub-indexed (subIndex addresses the path node / gradient stop)
    PathNodeX, PathNodeY,

    // App-allocated keys start here
    UserDefined = 0x8000
};

/// Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
/// the per-property resolved-style cell. Sibling of the scheduler's
/// `AnimatedValue` — the StyleValue variant carries the non-animatable
/// handles (Font, TextLayoutDescriptor) that have no place in the
/// animation runtime's variant.
using StyleValue = std::variant<
    std::monostate,
    Composition::Color,
    SharedHandle<Composition::Brush>,
    Composition::LayerEffect::DropShadowParams,
    SharedHandle<Composition::Font>,
    Composition::TextLayoutDescriptor,
    std::uint32_t>;

/// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
/// animation-runtime cell value. Lifted out of `AnimationScheduler.h`
/// alongside `StyleValue` so the new public `StyleSheet` vocabulary
/// (keyframe-track erasure over this variant) can declare keyframe
/// animations without reaching into the private scheduler header.
/// Kept as a separate variant from `StyleValue` because the runtime
/// uses types (`Point2D`, `Rect`, `TransformationParams`) that the
/// resolved-style table never sees, and conversely `StyleValue` uses
/// types (`Font` handle, `TextLayoutDescriptor`) that aren't lerpable.
using AnimatedValue = std::variant<
    std::monostate,
    float, int, std::uint32_t,
    Composition::Color,
    Composition::Point2D,
    Composition::Rect,
    Core::SharedPtr<Composition::Brush>,
    Composition::LayerEffect::DropShadowParams,
    Composition::LayerEffect::TransformationParams>;

/// Side-table / active-property key. `subIndex` is 0 for the common
/// case and addresses path nodes / gradient stops for the `*At`
/// variants. Same shape across the animation side-table and the
/// D5 resolved-style table.
struct PropertyTableKey {
    NodeId        node     = 0;
    PropertyKey   key      = PropertyKey::Opacity;
    std::uint32_t subIndex = 0;

    bool operator==(const PropertyTableKey & other) const {
        return node == other.node && key == other.key && subIndex == other.subIndex;
    }
};

struct PropertyTableKeyHash {
    std::size_t operator()(const PropertyTableKey & k) const {
        // node dominates the entropy; fold key+subIndex into the low bits.
        std::size_t h = std::hash<std::uint64_t>{}(k.node);
        h ^= (static_cast<std::size_t>(k.key) + 0x9e3779b97f4a7c15ULL +
              (h << 6) + (h >> 2));
        h ^= (static_cast<std::size_t>(k.subIndex) + 0x9e3779b97f4a7c15ULL +
              (h << 6) + (h >> 2));
        return h;
    }
};

} // namespace OmegaWTK

#endif // OMEGAWTK_UI_STYLEPROPERTY_H
