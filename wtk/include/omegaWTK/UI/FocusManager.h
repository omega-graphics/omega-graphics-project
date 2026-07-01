/**
 @file FocusManager.h

 §2.3a (Focus, F2): the per-window virtual focus manager.

 There is exactly one OS focus per window (the root NativeItem is the
 first responder when the window is key). Per-view keyboard focus is a
 purely virtual concept: this class is the single authority that tracks
 *which* `View` in a `WidgetTreeHost`'s tree currently owns keyboard
 focus, and why it last changed (see `FocusReason` in `View.h`, which
 gates focus-ring rendering).

 One `FocusManager` is owned by each `WidgetTreeHost` (constructed in the
 host ctor, reached via `WidgetTreeHost::focusManager()`), mirroring the
 host's ownership of `OverlayHost`. The Native-API-Completion-Proposal
 §2.3a Focus "implementation order" table landed this as F2 — a skeleton
 of `setFocus` / `focusedView` / `lastFocusReason` / `clearFocus`. F4
 adds pre-order tree traversal (`focusNext` / `focusPrevious`, driven by
 Tab / Shift-Tab interception in `WidgetTreeHost::dispatchInputEvent`). F5
 adds the restoration stack (`pushRestorationPoint` / `popAndRestore`) for
 Modal / Popover / ContextMenu dismiss. Tab-order overrides (`setTabOrder`,
 F6) layer on in a later phase.
 */

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/UI/View.h"   // FocusReason (free enum) + View

#ifndef OMEGAWTK_UI_FOCUSMANAGER_H
#define OMEGAWTK_UI_FOCUSMANAGER_H

namespace OmegaWTK {

    /**
     @brief The single keyboard-focus authority for one `WidgetTreeHost`.

     Tracks the currently focused `View` and the `FocusReason` that
     selected it. The manager does not own the views it points at — the
     widget tree does — so a `View` that is destroyed or detached while
     focused leaves a dangling `currentlyFocused_`; callers that tear
     down a focused view must `clearFocus()` (or `setFocus(other)`) as
     part of that teardown. (F5's restoration stack is tolerant of
     detached views; the bare F2 pointer is not, by design — adding a
     tree-membership check on every access would duplicate the hit-test
     walk for no F2 consumer.)
     */
    class OMEGAWTK_EXPORT FocusManager {
    public:
        OMEGACOMMON_CLASS("OmegaWTK.UI.FocusManager")

        FocusManager();
        ~FocusManager();

        /**
         @brief Make `view` the focused view.

         If `view` already holds focus, only the recorded `FocusReason`
         is refreshed and no focus events are emitted (avoids event
         churn when, e.g., a click re-focuses the already-focused view).

         Otherwise the previously focused view (if any) has its focused
         flag cleared, `view` is marked focused with `reason`, and two
         events fire through the existing `NativeEventEmitter` machinery
         (§2.1): `NativeEvent::FocusGained` on `view` carrying the
         previous `View *` as its params, then `NativeEvent::FocusLost`
         on the previous view carrying the new `View *` as its params.
         The focus flags are written *before* either event is emitted so
         a handler observing `View::isFocused()` sees the settled state.

         The params are passed as the event's raw `void *` (a non-owned
         `View *`); `NativeEvent`'s destructor intentionally does not free
         them for these two event types, so no allocation or leak is
         involved. Pass `nullptr` for `view` to clear focus (equivalent
         to `clearFocus()`, but without the early-out guard).

         @note F2 does not gate on `View::focusPolicy()`. This is the
         low-level primitive; the policy checks live at the call sites
         (mouse-click focus in M1 filters on `ClickFocus`, Tab traversal
         in F4 on `TabFocus`).
        */
        void setFocus(View * view, FocusReason reason = FocusReason::Other);

        /// The currently focused view, or `nullptr` if none.
        View * focusedView() const;

        /// Why focus last changed. Read by a widget's `rebuildStyle()`
        /// hook to decide whether to paint a focus ring (keyboard
        /// reasons show it, `Mouse` / `Other` suppress it). Retains its
        /// value across `clearFocus()` so the prior holder's reason is
        /// still inspectable.
        FocusReason lastFocusReason() const;

        /// Drop focus from whatever currently holds it. No-op when
        /// nothing is focused. Clears the holder's focused flag and
        /// emits `NativeEvent::FocusLost` on it (with `nullptr` params,
        /// since no view gains focus). `lastFocusReason()` is left
        /// unchanged — it reflects why the cleared view had been focused.
        void clearFocus();

        /// §2.3a F4: the root of the View subtree that `focusNext` /
        /// `focusPrevious` traverse. Set by `WidgetTreeHost::setRoot`
        /// alongside its own `root` assignment (the single point where the
        /// host's root widget changes), so the traversal root never
        /// desyncs from the tree the host is actually hosting. `nullptr`
        /// when the host has no root — traversal then finds nothing and
        /// the `focusNext` / `focusPrevious` early-out to `false`.
        void setRoot(View * root);

        /// §2.3a F4: advance keyboard focus to the next tab-focusable view
        /// in the tree, selecting it with `FocusReason::Tab`.
        ///
        /// Traversal order is a pre-order walk of the View tree under the
        /// root set by `setRoot` (parent, then children, then siblings) —
        /// the DOM tab-order users intuit. Views whose `focusPolicy()`
        /// lacks the `TabFocus` bit are skipped. Starting from the
        /// currently focused view, focus moves to the next tab-focusable
        /// view, wrapping past the end back to the first. When nothing is
        /// focused (or the focused view is not in the traversal set, e.g.
        /// an overlay), focus lands on the first tab-focusable view.
        ///
        /// @returns `false` (and changes nothing) when no tab-focusable
        /// view exists under the root; `true` otherwise.
        bool focusNext();

        /// §2.3a F4: the `focusPrevious` counterpart — moves focus to the
        /// previous tab-focusable view with `FocusReason::Backtab`,
        /// wrapping past the start back to the last. When nothing is
        /// focused, focus lands on the *last* tab-focusable view (so
        /// Shift-Tab from a fresh window enters at the end, matching Qt /
        /// browsers). Same `false`-when-empty contract as `focusNext`.
        bool focusPrevious();

        /// §2.3a F5: capture the currently focused view onto the
        /// restoration stack.
        ///
        /// Called by a Modal / Popover / ContextMenu just before it takes
        /// focus, so the view that had focus when the popup opened can be
        /// handed it back on dismiss. The capture is the raw
        /// `focusedView()` pointer (possibly `nullptr` when nothing is
        /// focused — a valid "restore to no-focus" record). The stack is
        /// LIFO so nested popups restore in reverse open order.
        void pushRestorationPoint();

        /// §2.3a F5: pop the top restoration point and re-focus the view
        /// it captured, with `FocusReason::Popup` (a keyboard reason, so a
        /// focus ring re-appears if the restored view was tab-focused
        /// before the popup covered it).
        ///
        /// Tolerant of detached / destroyed captures: the saved pointer is
        /// only ever *compared* against the live tree (never dereferenced)
        /// to decide whether it is still present. A non-null capture that
        /// is no longer in the tree — its owning subtree was torn down
        /// while the popup was up — is skipped, leaving focus as the popup
        /// left it. A `nullptr` capture restores the no-focus state
        /// (`setFocus(nullptr, …)` clears focus). No-op on an empty stack.
        ///
        /// @note This tolerance covers the *saved* pointer only. The
        /// current holder (`focusedView()`) is still governed by the F2
        /// teardown contract: a caller that destroys the focused view must
        /// `clearFocus()` first, or `setFocus`'s handling of the previous
        /// holder dereferences freed memory. F5 does not lift that.
        void popAndRestore();

    private:
        /// §2.3a F4: root of the traversal subtree (non-owning; the widget
        /// tree owns the views). Null until `setRoot` is called.
        View * root_ = nullptr;
        /// §2.3a F5: LIFO stack of captured focus holders (non-owning View
        /// pointers, `nullptr` entries allowed). `pushRestorationPoint`
        /// appends `currentlyFocused_`; `popAndRestore` consumes the back.
        /// Entries are validated against the live tree at pop time rather
        /// than eagerly pruned on view destruction, so a stale entry deep
        /// in the stack is simply skipped when it surfaces.
        OmegaCommon::Vector<View *> restorationStack_;

        /// The view that currently owns keyboard focus. Non-owning.
        View * currentlyFocused_ = nullptr;
        /// The reason the current (or most-recently) focused view was
        /// selected. Gates focus-ring rendering downstream.
        FocusReason lastReason_ = FocusReason::Other;
    };

};

#endif
