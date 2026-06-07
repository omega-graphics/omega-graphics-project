#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Composition/Layer.h"
#include <cstdint>

#ifndef OMEGAWTK_UI_OVERLAYHOST_H
#define OMEGAWTK_UI_OVERLAYHOST_H

namespace OmegaWTK {

class Widget;
OMEGACOMMON_SHARED_CLASS(Widget);
class WidgetTreeHost;

/// Z-tier classification for an overlay. Higher numeric value paints
/// later (on top) and is consulted first by hit-test top-first
/// traversal. See Overlay-Z-Order-Plan §2.
enum class OverlayTier : std::uint8_t {
    Floating  = 1,
    Modal     = 2,
    Tooltip   = 3,
    DragGhost = 4
};

/// Placement contract supplied by the caller of `OverlayHost::present`.
/// The host turns this into a concrete window-space rect using the
/// overlay widget's own rect as the *desired size* and edge-clamping
/// the result against the window content bounds. Plan §3 / §4.2.
struct OMEGAWTK_EXPORT OverlayAnchor {
    enum class Mode : std::uint8_t {
        AtWidget,        ///< Relative to a Widget's window-space rect + edge.
        AtPoint,         ///< Absolute window-space point (e.g. cursor).
        CenterInWindow   ///< For Modal / Sheet.
    };

    enum class Edge : std::uint8_t {
        Top,
        Bottom,
        Left,
        Right
    };

    Mode mode = Mode::AtWidget;
    /// The anchor widget when `mode == AtWidget`. Window-space origin
    /// is read via `View::offsetFromRoot()` at present time. Ignored
    /// for the other two modes.
    Widget * widget = nullptr;
    /// Window-space point when `mode == AtPoint`. Ignored otherwise.
    Composition::Point2D point {0.f, 0.f};
    /// Which edge of `widget` the overlay sits against, plus the gap
    /// (in window-space pixels) between the widget edge and the
    /// overlay edge. Used only when `mode == AtWidget`.
    Edge edge = Edge::Bottom;
    float gap = 4.f;
};

/// What dismisses this overlay (besides explicit `OverlayHost::dismiss`).
/// O1 stores these verbatim — O3 consumes them on hit / Escape /
/// anchor-destroy paths.
struct OMEGAWTK_EXPORT OverlayDismissPolicy {
    bool clickOutside     = true;
    bool escapeKey        = true;
    bool windowDeactivate = true;
    bool anchorDestroyed  = true;
    bool absorbsHits      = true;
};

/// Visual chrome the host applies on top of the overlay widget's own
/// style. O1 stores the configuration verbatim; O2.1 reads it during
/// the overlay paint pass. Plan §4.3.
struct OMEGAWTK_EXPORT OverlayOrnamentation {
    bool dropShadow = true;
    Composition::LayerEffect::DropShadowParams shadowParams {
        /* x_offset   */ 0.f,
        /* y_offset   */ 2.f,
        /* radius     */ 4.f,
        /* blurAmount */ 4.f,
        /* opacity    */ 0.25f,
        /* color      */ {0.f, 0.f, 0.f, 1.f}
    };
    /// Corner radius for the shadow silhouette. Callers should set
    /// this to match the overlay widget's own background corner
    /// rounding so the shadow tracks the visible edge. `0.f` =
    /// rectangular shadow.
    float cornerRadius = 0.f;
};

/// Opaque ticket returned by `OverlayHost::present`. Zero means
/// "no overlay" — call `valid()` before storing.
struct OMEGAWTK_EXPORT OverlayHandle {
    std::uint64_t id = 0;
    bool valid() const { return id != 0; }
};

/// Paint-time view of a presented overlay. O2.1 consumes this in
/// `WidgetTreeHost::paintDirty` to emit per-overlay chrome (drop
/// shadow) before walking the overlay subtree. Carries the
/// already-resolved window-space rect and ornament so the paint
/// loop does not have to walk back through the host for each.
struct OMEGAWTK_EXPORT PresentedOverlay {
    Widget * widget = nullptr;
    Composition::Rect rect {{0.f, 0.f}, 0.f, 0.f};
    OverlayOrnamentation ornament {};
};

/// In-window overlay layer. One instance per `WidgetTreeHost`; reach
/// it via `WidgetTreeHost::overlayHost()`. O1 implements present /
/// dismiss / iteration / anchor math; the paint walk (O2), hit-test
/// and dismissal triggers (O3), focus restoration (O4), and modal
/// trap (O5) plug in via the iteration accessors below without
/// changing this surface.
class OMEGAWTK_EXPORT OverlayHost {
    struct Impl;
    Core::UniquePtr<Impl> impl_;
public:
    explicit OverlayHost(WidgetTreeHost & host);
    ~OverlayHost();

    OverlayHost(const OverlayHost &) = delete;
    OverlayHost & operator=(const OverlayHost &) = delete;

    /// Mount `overlay` into the overlay slot at `tier`. The overlay's
    /// own rect width/height is the desired size; its pos is recomputed
    /// from `anchor` and edge-clamped against the window's content
    /// bounds, then committed via `Widget::setRect`. Returns a handle
    /// suitable for later `dismiss()`. Invalid handle if `overlay`
    /// is null.
    OverlayHandle present(WidgetPtr overlay,
                          OverlayTier tier,
                          const OverlayAnchor & anchor,
                          const OverlayDismissPolicy & policy = {},
                          const OverlayOrnamentation & ornament = {});

    /// Remove the overlay identified by `handle`. No-op on a stale
    /// or zero handle.
    void dismiss(OverlayHandle handle);

    /// Remove every entry that hosts `overlay`. No-op if not present.
    void dismiss(Widget * overlay);

    /// Remove every entry in `tier`.
    void dismissAll(OverlayTier tier);

    /// Remove every entry across all tiers.
    void dismissAll();

    /// True iff at least one overlay in `tier` is currently presented.
    bool isPresenting(OverlayTier tier) const;

    /// True iff at least one overlay is currently presented in any
    /// tier. O2's `WidgetTreeHost::paintDirty` consults this to
    /// decide whether to force-paint the main tree alongside the
    /// overlay subtree walks — the deposited `CompositeFrame` must
    /// carry the main slice or the rest of the window blanks out.
    bool isPresentingAny() const;

    /// Topmost-first iteration across all tiers: reverse tier order
    /// (DragGhost → Tooltip → Modal → Floating), then reverse
    /// insertion order within tier. Used by O3 hit-test and Escape
    /// dispatch. The returned range is valid until the next
    /// mutating call on this host.
    OmegaCommon::ArrayRef<Widget *> overlaysTopFirst() const;

    /// Bottom-up iteration scoped to a single tier (insertion order).
    /// Used by O2's paint walk. The returned range is valid until
    /// the next mutating call on this host.
    OmegaCommon::ArrayRef<Widget *> overlaysIn(OverlayTier tier) const;

    /// Bottom-up iteration scoped to a single tier (insertion order),
    /// returning the resolved rect and ornament alongside the widget
    /// pointer. O2.1 uses this in `WidgetTreeHost::paintDirty` so the
    /// per-overlay chrome (drop shadow) can be emitted without a
    /// second walk through the host. The returned range is valid
    /// until the next mutating call on this host.
    OmegaCommon::ArrayRef<PresentedOverlay> overlaysForPaintIn(
        OverlayTier tier) const;

    /// Window-space rect committed for `handle` at present time
    /// (already edge-clamped). Used by O2's paint walk to position
    /// the overlay subtree. Zero rect on a stale or zero handle.
    Composition::Rect rectFor(OverlayHandle handle) const;

    /// Tier the overlay was presented into. Defaults to `Floating`
    /// on a stale or zero handle.
    OverlayTier tierFor(OverlayHandle handle) const;

    /// Recompute window-space rects for every presented overlay
    /// (anchor → rect math re-applied + edge-clamp). Called by
    /// `WidgetTreeHost` on window resize; also safe to call from
    /// app code when an anchor widget repositions and the overlay
    /// should follow.
    void relayoutAll();
};

}

#endif
