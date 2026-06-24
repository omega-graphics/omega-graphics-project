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
 §2.3a Focus "implementation order" table lands this as F2 — a skeleton
 of `setFocus` / `focusedView` / `lastFocusReason` / `clearFocus`. Tree
 traversal (`focusNext` / `focusPrevious`, F4), restoration points
 (`pushRestorationPoint` / `popAndRestore`, F5), and tab-order overrides
 (`setTabOrder`, F6) layer on in later phases.
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

    private:
        /// The view that currently owns keyboard focus. Non-owning.
        View * currentlyFocused_ = nullptr;
        /// The reason the current (or most-recently) focused view was
        /// selected. Gates focus-ring rendering downstream.
        FocusReason lastReason_ = FocusReason::Other;
    };

};

#endif
