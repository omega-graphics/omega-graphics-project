# Win32 DirectManipulation Scroll-Phase Plan (future)

Forward-looking plan for **true precision-touchpad scroll phase + OS inertia on
Windows**. Parked in `.plans/future/` — greenlight it when the Win32 backend is
next actively exercised. It is the Windows counterpart to the macOS
`NSEvent.momentumPhase` path and the GTK `is_stop` path already shipped under
[ScrollView-Interaction-Enhancements-Plan §E5](../ScrollView-Interaction-Enhancements-Plan.md).

## 1. Why this is deferred (the problem)

`ScrollParams::phase` (see `NativeEvent.h`) lets a `ScrollView` distinguish a
discrete mouse wheel from a trackpad gesture and its momentum stream, so it does
not layer app-side momentum on top of an OS fling. Populating it needs the
backend to *know* the gesture lifecycle:

- **macOS** reads `NSEvent.phase` / `momentumPhase` directly — done.
- **GTK4** reads `GDK_SCROLL_SMOOTH` deltas + `gdk_scroll_event_is_stop()`. GTK
  streams **no** OS inertia, so the app synthesizes the fling on the `Ended`
  (is_stop) event — done, gated by `ScrollParams::providesOSMomentum == false`.
- **Win32** delivers scrolling as `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL`. These
  messages carry **no phase and no source flag** — a precision-touchpad pan, its
  post-lift inertia, and a discrete wheel notch are indistinguishable at the
  message level. So today the Win32 backend sends `phase == None` for
  everything, and the `ScrollView` treats a precision touchpad as a discrete
  wheel: it runs the app-side per-tick fling. Because the Precision Touchpad
  driver *also* streams its own inertial `WM_MOUSEWHEEL` messages after lift,
  the result is **doubled momentum** on a precision touchpad.

Getting real phase on Windows requires **DirectManipulation** (the COM
compositor-input subsystem that backs UWP/modern scrolling). There is no
lightweight `WM_MOUSEWHEEL`-only path that reliably separates user pan from
inertia. That is a subsystem, not a small native follow-up, which is why it is
its own plan.

## 2. Scope

Populate `ScrollParams::phase` on Win32 from DirectManipulation so a precision
touchpad reports `Began` / `Changed` / `Ended` for the user pan and
`MomentumBegan` / `Momentum` / `MomentumEnded` for the inertia, and set
`ScrollParams::providesOSMomentum = true` (Windows, like macOS, streams its own
inertia). The existing `ScrollView` E5 consumer then needs **no change** — it
already suppresses the app fling for any real phase on an OS-momentum platform.

Out of scope: pen/touch panning, zoom (pinch), rails/snap-points — DM supports
them but they are separate features.

## 3. Approach (prior art → design)

DirectManipulation is the API Microsoft ships for exactly this. The shape:

1. **Manager + viewport.** Create an `IDirectManipulationManager`, an
   `IDirectManipulationViewport` bound to the content `HWND`, and enable
   `DIRECTMANIPULATION_TRANSLATE_X | TRANSLATE_Y | TRANSLATE_INERTIA`.
2. **Feed input.** Route `WM_POINTERDOWN` / pointer messages (after
   `EnableMouseInPointer`) — or the DM message hook — into
   `IDirectManipulationViewport::SetContact` so DM owns the manipulation.
3. **Read transforms via the update manager.** Drive
   `IDirectManipulationUpdateManager` from the compositor/frame tick; on each
   `IDirectManipulationViewportEventHandler::OnViewportStatusChanged` map the DM
   status to a `ScrollPhase`:
   - `RUNNING` (contact down) → `Changed` (first after `READY`/`ENABLED` →
     `Began`).
   - `INERTIA` → `Momentum` (first tick → `MomentumBegan`).
   - back to `READY` → `MomentumEnded` (or `Ended` if no inertia ran).
   `OnContentUpdated` yields the per-frame translation delta → `deltaX/deltaY`.
4. **Emit** a `ScrollWheel` `NativeEvent` per content update, phase-tagged, with
   `providesOSMomentum = true`.

### Phasing (fill in when greenlit)
- **DM-1** Manager/viewport lifecycle bound to the window HWND; no event
  emission yet (verify DM initializes, no crash, wheel still works via the
  existing path).
- **DM-2** Status → `ScrollPhase` mapping + content-update deltas → emit
  phase-tagged `ScrollWheel`; set `providesOSMomentum`.
- **DM-3** Reconcile with the legacy `WM_MOUSEWHEEL` path (discrete wheel must
  still send `phase == None`); ensure a real mouse wheel and a precision
  touchpad coexist without double events.

## 4. Verification
- Precision touchpad: two-finger scroll glides once (no double momentum);
  flick-and-release coasts and eases to a stop; a wheel notch mid-glide cancels.
- Discrete wheel unchanged (still `None` → app fling).
- Blind-build caveat: Windows builds run through WSL and the agent cannot drive
  them — iterate on compiler output the developer pastes back, and hand off the
  interaction/screenshot verification.

## 5. Interim state (shipped)
Win32 sends `phase == None` for all scroll input. A precision touchpad there
gets the discrete-wheel app-momentum path — acceptable and functional, with the
known doubled-momentum caveat above — until this plan lands.
