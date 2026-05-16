#include "omegaWTK/Core/Core.h"
#include "omega-common/img.h"
#include "omega-common/unicode.h"
#include "Geometry.h"
#include "GTEForward.h"
#include "Path.h"
#include "FontEngine.h"
#include "Layer.h"
#include "Canvas.h"

#include <cstdint>

#ifndef OMEGAWTK_COMPOSITION_DISPLAYLIST_H
#define OMEGAWTK_COMPOSITION_DISPLAYLIST_H

namespace OmegaWTK::Composition {

    /// UIView-Render-Redesign-Plan Phase 2.0: a frame-scoped, flat
    /// recording of paint intent emitted by a `SceneNode::paint` pass
    /// (today: by `UIView::update()`). Per Tier 2 §2.0 cross-cutting
    /// decisions, `DrawOp` mirrors `VisualCommand` 1:1 — same ten
    /// variants, same field names, same field types — so the eventual
    /// Tier 4 retirement of `VisualCommand` is a mechanical
    /// search/replace rather than a semantic migration. The text
    /// variant is the post-shaping `TextRun` (matching
    /// `VisualCommand::TextRun`); high-level `drawText` callers shape
    /// first and then emit a `TextRun` op.
    struct OMEGAWTK_EXPORT DrawOp {
        typedef enum : OPT_PARAM {
            Rect,
            RoundedRect,
            Ellipse,
            VectorPath,
            TextRun,
            Bitmap,
            Shadow,
            /// 3D-effect transform (perspective, full 3D rotation, etc.)
            /// applied to subsequent draws within the same frame. The
            /// payload is a `Matrix4x4`, *not* a 2D translation. Nodes
            /// that just need to move on the 2D plane update their rect
            /// (`Composition::Rect::pos`) — they do not push a transform.
            SetTransform,
            SetOpacity,
            /// Phase 2.5: reserves a rectangular region for a native
            /// surface to draw into (video, GPU view, web view, etc.).
            /// Carries no pixel data; the compositor turns this into a
            /// platform-specific carve-out (clear / clip / blend-discard)
            /// and forwards the `hostId` so the platform compositor
            /// knows which native layer owns the region. Tier-2 replay
            /// is a no-op — the carve-out lands in Tier 3 alongside
            /// FrameBuilder.
            NativeContent,
            /// Phase 2.4 state ops. No Tier-2 producer; replay is a
            /// no-op (the Canvas API has no clip / scoped-transform
            /// surface). ScrollView (Tier 3) is the first planned
            /// `PushClip` producer. `PushTransform` carries the same
            /// `Matrix4x4` 3D-effect transform as `SetTransform` —
            /// scope-pushed instead of canvas-state-replaced. 2D
            /// positional moves never go through these ops; they
            /// update rects directly.
            PushClip,
            PopClip,
            PushTransform,
            PopTransform
        } Type;
        Type type;

        struct OMEGAWTK_EXPORT Params {
            struct {
                Composition::Rect rect;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } rectParams;

            struct {
                Composition::RoundedRect rect;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } roundedRectParams;

            struct {
                Composition::Ellipse ellipse;
                Core::SharedPtr<Brush> brush;
                Core::Optional<Border> border;
            } ellipseParams;

            /// `VectorPath` mirrors `VisualCommand::pathParams` but
            /// captures the canvas-level `Path` handle rather than the
            /// already-segmented `GVectorPath2D` list — Canvas's
            /// `drawPath` decomposition into per-segment commands
            /// stays inside the replay step. Shared ownership: the
            /// op holds a `SharedPtr<Path>` so producers that build
            /// Paths at parse time (SVGView, Phase 2.3) can hand off
            /// ownership without a sidecar storage vector. UIView
            /// producers (Phase 2.1) already own `Shape::path` as a
            /// `SharedPtr<Path>` and pass it through unchanged.
            struct {
                Core::SharedPtr<Composition::Path> path;
                Core::Optional<Border> border;
            } pathParams;

            /// Mirrors `VisualCommand::textRunParams`: a pre-shaped
            /// `TextRun`. High-level `drawText` producers do shaping
            /// upstream (via the WTK text layout engine) and emit one
            /// `TextRun` op per resolved sub-run group.
            struct {
                OmegaCommon::Vector<TextSubRun> subRuns;
                Composition::Rect rect;
                Composition::Color color;
            } textRunParams;

            struct {
                Core::SharedPtr<OmegaCommon::Img::BitmapImage> img;
                Core::SharedPtr<OmegaGTE::GETexture> texture;
                Core::SharedPtr<OmegaGTE::GEFence> textureFence;
                Composition::Rect rect;
                Core::Optional<Composition::Rect> sourceRect;
                Core::Optional<Composition::Color> tintColor;
            } bitmapParams;

            struct {
                LayerEffect::DropShadowParams shadow {};
                Composition::Rect shapeRect {};
                float cornerRadius = 0.f;
                bool isEllipse = false;
            } shadowParams;

            /// 3D-effect transform. Identity at frame start. Updated
            /// by `DrawOp::SetTransform`. Mirrors
            /// `VisualCommand::Data::transformMatrix`. Note: this is
            /// *not* the path 2D-positional changes take — those
            /// update `Composition::Rect::pos` directly.
            Matrix4x4 transformMatrix = Matrix4x4::Identity();
            float opacityValue = 1.f;

            /// Phase 2.5 NativeContent payload. `destRect` is in
            /// view-local coordinates (same frame as other DrawOp
            /// rects); `hostId` identifies the platform native layer
            /// that will draw here; `zOrderHint` lets the compositor
            /// pick a stable ordering on platforms with multiple
            /// native layer ordering modes (CA addSublayer order vs.
            /// DirectComposition visual tree, per §9.7 Q3).
            struct {
                Composition::Rect destRect {};
                std::uint64_t hostId = 0;
                int zOrderHint = 0;
            } nativeContentParams;

            /// Phase 2.4 `PushClip` payload. The rect is in the
            /// current drawing space (post-`PushTransform`, if any).
            /// `PopClip` carries no payload. The Tier-2 replay does
            /// not consult this; ScrollView's Tier-3 paint pushes
            /// the clip and FrameBuilder threads it through the
            /// compositor backend.
            struct {
                Composition::Rect rect {};
            } pushClipParams;

            /// Phase 2.4 `PushTransform` reuses `transformMatrix`
            /// (same 3D-effect `Matrix4x4` shape as `SetTransform`).
            /// `PopTransform` carries no payload. Separate variant
            /// from `SetTransform` because the semantics differ —
            /// scope-pushed vs. canvas-state-replaced — even though
            /// the payload is bit-identical.
        } params;

        DrawOp() = delete;

        // Sentinel constructors per variant. Field shapes match
        // `VisualCommand` constructors at `Canvas.h:206` so Phase 2.1
        // call-site conversions are mechanical.

        DrawOp(const Composition::Rect & rect,
               Core::SharedPtr<Brush> brush,
               Core::Optional<Border> border = std::nullopt)
            : type(Rect) {
            params.rectParams = {rect, std::move(brush), std::move(border)};
        }

        DrawOp(const Composition::RoundedRect & rect,
               Core::SharedPtr<Brush> brush,
               Core::Optional<Border> border = std::nullopt)
            : type(RoundedRect) {
            params.roundedRectParams = {rect, std::move(brush), std::move(border)};
        }

        DrawOp(const Composition::Ellipse & ellipse,
               Core::SharedPtr<Brush> brush,
               Core::Optional<Border> border = std::nullopt)
            : type(Ellipse) {
            params.ellipseParams = {ellipse, std::move(brush), std::move(border)};
        }

        DrawOp(Core::SharedPtr<Composition::Path> path,
               Core::Optional<Border> border = std::nullopt)
            : type(VectorPath) {
            params.pathParams = {std::move(path), std::move(border)};
        }

        DrawOp(OmegaCommon::Vector<TextSubRun> subRuns,
               const Composition::Rect & rect,
               const Composition::Color & color)
            : type(TextRun) {
            params.textRunParams = {std::move(subRuns), rect, color};
        }

        DrawOp(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,
               const Composition::Rect & rect)
            : type(Bitmap) {
            params.bitmapParams = {std::move(img), nullptr, nullptr, rect,
                                   std::nullopt, std::nullopt};
        }

        DrawOp(Core::SharedPtr<OmegaCommon::Img::BitmapImage> img,
               const Composition::Rect & rect,
               Core::Optional<Composition::Rect> sourceRect,
               Core::Optional<Composition::Color> tintColor)
            : type(Bitmap) {
            params.bitmapParams = {std::move(img), nullptr, nullptr, rect,
                                   std::move(sourceRect), std::move(tintColor)};
        }

        DrawOp(Core::SharedPtr<OmegaGTE::GETexture> texture,
               Core::SharedPtr<OmegaGTE::GEFence> fence,
               const Composition::Rect & rect)
            : type(Bitmap) {
            params.bitmapParams = {nullptr, std::move(texture), std::move(fence),
                                   rect, std::nullopt, std::nullopt};
        }

        DrawOp(const LayerEffect::DropShadowParams & shadow,
               const Composition::Rect & shapeRect,
               float cornerRadius,
               bool isEllipse)
            : type(Shadow) {
            params.shadowParams = {shadow, shapeRect, cornerRadius, isEllipse};
        }

        /// Push a 3D-effect transform onto the canvas state. Carries a
        /// `Matrix4x4`; intended for perspective / 3D rotation / scale
        /// effects, not for 2D positional translation (move the rect
        /// instead).
        explicit DrawOp(const Matrix4x4 & matrix)
            : type(SetTransform) {
            params.transformMatrix = matrix;
        }

        explicit DrawOp(float opacity)
            : type(SetOpacity) {
            params.opacityValue = opacity;
        }

        /// Factory for `NativeContent`. Named distinctly from the enum
        /// value `NativeContent` (C++ enums and member functions share
        /// the same scope) and used in lieu of a ctor so the
        /// `(Rect, ...)` signature doesn't collide with `DrawOp::Rect`.
        /// See the Phase 2.5 enum-doc above for the carve-out
        /// semantics.
        static DrawOp makeNativeContent(const Composition::Rect & destRect,
                                        std::uint64_t hostId,
                                        int zOrderHint = 0) {
            DrawOp op(StateOpTag{Type::NativeContent});
            op.params.nativeContentParams = {destRect, hostId, zOrderHint};
            return op;
        }

        /// Phase 2.4 state-op factories. Use factories (not ctors) for
        /// the same enum-vs-method-name reason as `makeNativeContent`,
        /// and so `PushTransform`'s `(Matrix4x4)` signature doesn't
        /// shadow the `SetTransform` ctor.
        static DrawOp makePushClip(const Composition::Rect & rect) {
            DrawOp op(StateOpTag{Type::PushClip});
            op.params.pushClipParams = {rect};
            return op;
        }
        static DrawOp makePopClip() {
            return DrawOp(StateOpTag{Type::PopClip});
        }
        static DrawOp makePushTransform(const Matrix4x4 & matrix) {
            DrawOp op(StateOpTag{Type::PushTransform});
            op.params.transformMatrix = matrix;
            return op;
        }
        static DrawOp makePopTransform() {
            return DrawOp(StateOpTag{Type::PopTransform});
        }

    private:
        struct StateOpTag { Type t; };
        explicit DrawOp(StateOpTag tag) : type(tag.t) {}
    };

    /// Flat, append-only recording of `DrawOp`s for one paint pass.
    /// Phase 2.0 keeps the type intentionally thin — the eventual
    /// per-window display list (Tier 3) will add a transform stack and
    /// dirty-region helpers, but neither is needed for the
    /// single-branch-replay slice this phase ships.
    class OMEGAWTK_EXPORT DisplayList {
        OmegaCommon::Vector<DrawOp> ops_;
    public:
        OMEGACOMMON_CLASS("OmegaWTK.Composition.DisplayList")

        DisplayList() = default;

        void append(DrawOp op) {
            ops_.push_back(std::move(op));
        }

        void clear() {
            ops_.clear();
        }

        std::size_t size() const {
            return ops_.size();
        }

        const OmegaCommon::Vector<DrawOp> & ops() const {
            return ops_;
        }
    };

    /// Tier 2 scaffolding: replays a `DisplayList` into a `Canvas`,
    /// preserving the existing GPU path. Removed in Tier 4 when the
    /// compositor backend's `renderToTarget` switch is rewritten to
    /// dispatch on `DrawOp` directly.
    class OMEGAWTK_EXPORT DisplayListReplay {
    public:
        static void replay(const DisplayList & list, Canvas & canvas);
    };

}

#endif
