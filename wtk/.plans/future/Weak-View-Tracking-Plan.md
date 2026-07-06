# Weak View Tracking Plan

**Status:** Future (proposed, not scheduled). Move to the top level of `wtk/.plans/` when implementation is greenlit.

**Scope:** Small, self-contained lifetime subsystem. One primitive plus the migration of a handful of existing raw-`View*` caches onto it.

---

## 1. Problem

Several long-lived structures cache a raw `View *` that outlives the single call it was captured in, and **none of them are notified when the pointed-at `View` is destroyed or detached.** A `View` that dies while one of these caches still points at it leaves a dangling pointer; the next access either reads freed memory or, worse, silently matches a *different* `View` later allocated at the same address (an ABA hit).

The concrete holders today, all introduced by the §2.3a Focus block of `Native-API-Completion-Proposal.md`, plus one older one:

| Holder | Introduced | Current mitigation |
|--------|-----------|--------------------|
| `FocusManager::currentlyFocused_` | F2 | **None** — relies on the "teardown clears focus" contract (a caller destroying the focused view must call `clearFocus()`/`setFocus(other)` first). A contract violation dereferences freed memory in `setFocus`'s previous-holder handling. |
| `FocusManager::root_` | F4 | Re-synced from `WidgetTreeHost::setRoot`; stale only between a root swap and the next `setRoot` (narrow, but unguarded). |
| `FocusManager::restorationStack_` | F5 | Tolerated: `viewInSubtree(root_, saved)` pointer-walks the live tree at pop time and skips a saved view that is gone. Correct but O(tree) per pop, and carries a residual ABA caveat. |
| `FocusManager::tabOrderNext_` | F6 | Inert: links are ignored unless both endpoints are live in the current tree. Safe, but stale links accumulate in the map forever (no `clearTabOrder`, no reclamation). |
| `WidgetTreeHost::hoveredView_` | (pre-Focus) | **None** obvious — a hovered view destroyed between hover-enter and the next hit-test is a latent dangling read. |
| Cursor-shape "active view" | **C1 (incoming)** | Will add another raw `View*` cache of the same shape unless this lands first. |

The common root cause: **there is no weak-reference mechanism anywhere in WTK/OmegaCommon — not for `View`, not for anything.** `ViewPtr` is `SharedHandle<View>` (`std::shared_ptr`, via `OMEGACOMMON_SHARED_CLASS`), so Views *are* shared-owned, but `View` does **not** derive from `std::enable_shared_from_this`, so a bare `View*` cannot currently produce even a `std::weak_ptr`. `OmegaCommon::ARC` / `ARCAny` are *strong* intrusive-refcount helpers (base `RuntimeObject`), not weak refs. Introducing any weak primitive is therefore a genuine architectural decision, not a wiring task.

Each holder has so far been patched with a bespoke, local discipline (a contract here, a tree-walk there, inert-link filtering elsewhere). That works per-site but does not compose, is easy to get wrong at the next call site, and re-pays the cost (the F5 tree-walk) or the risk (the F2 contract) every time. One shared weak-view primitive retires all of it.

---

## 2. Goals & non-goals

**Goals**

- A single, small `WeakView` primitive that becomes `nullptr` (observably) the moment its target `View` is destroyed.
- Migrate the six holders above onto it, deleting their bespoke mitigations (the F5 tree-walk, the F2 contract dependency, the F6 stale-link accumulation).
- Zero behavior change for callers that already do the right thing; strictly-safer behavior for the rest.
- Cheap: no per-frame cost, negligible per-`View` storage, no allocation on the hot path.

**Non-goals**

- A general-purpose weak-pointer ADT for the whole repo. This targets `View` specifically; generalizing later is possible but out of scope.
- Changing `View` ownership (it stays `SharedHandle`-owned).
- Detach-vs-destroy semantics beyond what the consumers need. (A *detached-but-alive* view is a separate question from a *destroyed* view — see §6 Open Questions.)
- Thread safety. WTK's view tree is main-thread-only; the primitive inherits that and does not synchronize.

---

## 3. Prior art

- **Chromium `views::View` + `ViewObserver` / `ViewTracker`.** `View` maintains an observer list; on destruction it fires `OnViewIsDeleting`. `ViewTracker` is a tiny helper that observes one view and nulls its pointer on that callback. This is the intrusive-observer model: the object being tracked owns the notification. It works regardless of how the view is owned (raw, unique, ref-counted).
- **Qt `QPointer<T>` + `QObject::destroyed`.** `QPointer` is a guarded pointer that auto-nulls when the `QObject` is destroyed, implemented via `QObject`'s internal per-object list of guarded pointers (`QObjectPrivate::declareGuard`). Same intrusive model as Chromium, packaged as a smart-pointer type. This is the ergonomic target: consumers hold a `QPointer`, deref returns null after destruction.
- **`std::weak_ptr` + `enable_shared_from_this`.** The standard-library answer *when the object is already `shared_ptr`-owned* — which `View` is. `weak_ptr::lock()` yields a temporary strong ref that is either valid (safe to deref) or empty. Zero extra per-object storage beyond the control block that already exists. The catch: requires `enable_shared_from_this<View>` and the invariant "every tracked View is owned by a `shared_ptr` before it is tracked."

The two viable shapes for WTK are the **intrusive observer** (Chromium/Qt) and the **`weak_ptr`** (stdlib). They are compared in §4.

---

## 4. Design options

### Option A — Intrusive `WeakView` (Chromium/Qt model) — *recommended*

`View` gains a small, intrusive registry of back-references. A `WeakView` value type registers itself with a `View` on assignment and unregisters on destruction/reassignment; `View::~View()` walks its registry and nulls every live `WeakView` pointing at it.

```cpp
// New: WeakView.h (UI)
class View;

/// A non-owning handle to a View that becomes null the instant the View
/// is destroyed. Main-thread only. Copyable; each copy self-registers.
class OMEGAWTK_EXPORT WeakView {
public:
    WeakView() = default;
    WeakView(View * v);              // registers with v (no-op if null)
    WeakView(const WeakView & o);
    WeakView & operator=(View * v);
    WeakView & operator=(const WeakView & o);
    ~WeakView();                     // unregisters

    View * get() const { return target_; }   // null after target dies
    explicit operator bool() const { return target_ != nullptr; }
    View * operator->() const { return target_; }
    View & operator*()  const { return *target_; }
private:
    friend class View;
    void detach();                   // called by View::~View to null us
    View * target_ = nullptr;
    // intrusive-list linkage into View::Impl::weakRefs_
};
```

`View::Impl` grows one small member — a list of the `WeakView`s currently observing it (e.g. `OmegaCommon::Vector<WeakView*> weakRefs_`, or an intrusive doubly-linked list to make unregister O(1)). `View::~View()` (which today only prints a debug line) additionally does:

```cpp
for (WeakView * w : impl_->weakRefs_) w->detach();   // sets w->target_ = nullptr
```

**Pros**

- Does not depend on *how* a View is owned — works even for a View held only by a raw pointer, so it is robust against future ownership changes and against a View tracked before its `shared_ptr` exists.
- Deref is a plain non-null pointer read; no `.lock()` / temporary strong ref ceremony at call sites.
- Self-contained: one header + a dozen lines in `View::Impl` and `~View()`. No change to `View`'s base classes.
- Matches the "no weak handles anywhere" starting point — introduces exactly one, minimal, and named for what it is.

**Cons**

- Intrusive: every `View` carries the registry member (one vector/pointer, empty in the common case). New idiom for the repo.
- Registration/unregistration cost on `WeakView` copy/move (negligible; these are not hot-path objects).

### Option B — `std::weak_ptr<View>` via `enable_shared_from_this`

Make `View : public std::enable_shared_from_this<View>`; consumers store `std::weak_ptr<View>` obtained from `view->weak_from_this()` (C++17) and validate with `.lock()`.

**Pros**

- Standard library; no new type to write or maintain.
- Zero extra per-View storage — reuses the `shared_ptr` control block Views already have.
- `.lock()` hands back a temporary *strong* ref, so a validated deref is guaranteed alive for the duration of use (a genuine safety edge over Option A's bare pointer if a consumer ever deref'd across a possible-destroy boundary).

**Cons**

- Depends on the invariant **"every tracked View is owned by a `shared_ptr` before it is tracked."** True today (all Views come from `SharedHandle(new View(...))` and are retained by their Widget), but *unenforced* — a View tracked during construction, or one whose only owner is a raw pointer in a parent's `subviews` vector with the `SharedHandle` dropped, would have an empty `weak_from_this()` and silently never validate. The bug is invisible until it bites.
- Introduces `weak_ptr`/`shared_from_this` as a net-new idiom (per the developer, no weak handles exist anywhere), and `enable_shared_from_this` interacts subtly with `View`'s existing base (`Native::NativeEventEmitter`) and with any code that constructs a `View` on the stack or via a non-`shared_ptr` path.
- Call sites must hold the `.lock()` result to keep the guarantee, which is easy to forget and then no better than Option A.

### Why A over B

Both are small. The deciding factor is robustness against an **unenforced invariant**: Option B is silently wrong for any View that is tracked while not (yet) `shared_ptr`-owned, and that failure mode is invisible until production. Option A has no such precondition — it is correct for any View however it is owned. Given the developer's emphasis that this area has *no* weak infrastructure (so we are choosing the idiom, not inheriting one) and the shepherd preference for what survives over what is lightest-now, Option A is the recommended default. **This is a decision point — see §7.**

---

## 5. Phased implementation (Option A)

Each phase is independently landable and leaves the tree building.

- **W1 — The primitive.** Add `WeakView.h`/`.cpp` under `wtk/{include/omegaWTK,src}/UI`. Add `weakRefs_` to `View::Impl` and the detach loop to `View::~View()`. Unit-testable in isolation: construct a `ViewPtr`, point a `WeakView` at it, destroy the `ViewPtr`, assert the `WeakView` is null. **Blocks nothing; consumers migrate incrementally after.**
- **W2 — FocusManager migration.** Replace `currentlyFocused_`, `root_` with `WeakView`; replace `restorationStack_` element type and `tabOrderNext_` key/value with `WeakView`. Delete `viewInSubtree` (F5's tolerance) — `popAndRestore` becomes "if the saved `WeakView` is non-null, restore it." `buildTraversalOrder` drops dead links naturally (a null `WeakView` is skipped). The F2 class-doc caveat about "callers must clearFocus() before destroying the focused view" is **removed** — the manager now self-heals.
- **W3 — WidgetTreeHost::hoveredView_.** Migrate to `WeakView`; the hover dispatcher's "hovered view changed" comparison and the `CursorExit` emit both null-check through the handle.
- **W4 — C1 cursor cache (if C1 lands first, fold in here; if this plan lands first, C1 uses `WeakView` from day one).**
- **W5 — Sweep.** `grep` for other persisted raw `View*` caches (e.g. `ScrollView::owner`) and migrate any that can outlive their target. Document `WeakView` as the house pattern for cross-call View caching.

Estimated size: W1 ~120 lines incl. test; W2–W4 are mechanical type swaps that *delete* more than they add.

---

## 6. Consumers & what gets deleted

| Consumer | Before | After |
|----------|--------|-------|
| `FocusManager::currentlyFocused_` / `root_` | raw `View*` + teardown contract | `WeakView`; contract dependency deleted |
| `FocusManager::restorationStack_` | `Vector<View*>` + `viewInSubtree` walk at pop | `Vector<WeakView>`; **`viewInSubtree` deleted** |
| `FocusManager::tabOrderNext_` | `MapVec<View*,View*>` + inert-stale-link filtering + unbounded growth | `MapVec` keyed on stable id with `WeakView` values, or periodic prune; stale links self-clear |
| `WidgetTreeHost::hoveredView_` | raw `View*` (latent dangle) | `WeakView` |
| C1 cursor active-view | (incoming raw `View*`) | `WeakView` |

Net line count is expected to be **negative** after W2 — the tolerance machinery removed exceeds the primitive added.

> **`tabOrderNext_` note:** a `WeakView` as a `MapVec` *key* needs a hash/equality on the stable target (pointer identity is fine while alive, but a dead key hashing on a null target collides). Simplest: key the map on `View::nodeId()` (already a stable `uint64_t`) and store `WeakView` values, or keep raw-pointer keys and prune entries whose `WeakView` value went null during `buildTraversalOrder`. Decide at W2.

---

## 7. Open questions / decision points

1. **Option A vs B.** Recommended A (no unenforced invariant). Developer decides. If B, W1 becomes "add `enable_shared_from_this` + audit all View construction paths for shared-ownership-before-tracking" instead of writing the primitive.
2. **Detach vs destroy.** Consumers care about *destroy* (freed memory). Do any care about *detach-but-alive* (a View removed from the tree but still held by a `SharedHandle` somewhere)? F5's current tree-walk conflates them (a detached-but-alive view is treated as "gone"). If the new primitive only nulls on *destroy*, F5's behavior changes slightly: a detached-but-alive saved view would now be restored, where today it is skipped. Confirm that is acceptable (it is arguably *more* correct) or add an `isAttached()` gate.
3. **`ScrollView::owner` and other pre-existing raw `View*`/`Widget*` back-pointers.** In scope for the W5 sweep, or leave untouched until independently implicated?
4. **Debug-line cleanup.** `View::~View()` currently `std::cout << "View will destruct"`. Fold its removal into W1 or leave it.

---

## 8. Testing

- **W1 unit test** (new, under the UI test target): point-then-destroy, copy-then-destroy, reassign, self-null-after-death, vector-of-`WeakView` survives element destruction. No graphics needed — pure lifetime.
- **W2 regression:** the existing Focus behavior verified for F4/F5/F6 must be unchanged when no view dies mid-sequence (the common path). Add a targeted test: focus a view, destroy it out from under the manager *without* calling `clearFocus()`, assert `focusedView()` is null and a subsequent `focusNext()` does not crash — the exact scenario the F2 contract currently forbids.
- **Build/link + BasicAppTest** on macOS as with the Focus phases; visual verification is not required (no rendered output changes), but a manual tab-through-then-destroy smoke check is worthwhile once TextInput/Modal consumers exist.
