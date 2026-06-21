# App Building Plan — Finalizing Cross-Platform Production Apps

This plan covers the gap between "the app compiles and runs from the build tree"
and "the app is a production artifact a user can install and that the OS treats
as a first-class application." It is the home for per-target packaging,
identity, signing, and OS-capability registration concerns that the
`Native-API-Completion-Proposal.md` work surfaced but deliberately left out of
the runtime API.

**Guiding principle — declare once, lower per-target.** WTK already has a single
app-declaration point: the `OmegaWTKApp()` CMake function
(`wtk/cmake/OmegaWTKApp.cmake`). Every production concern below should be
expressed there (or in a small companion manifest) **once, cross-platform**, and
*lowered* into the target-specific scaffolding that already exists under
`wtk/target/`. App authors should never hand-edit an `Info.plist`, a
`.exe.manifest`, an `AndroidManifest.xml`, or a `.desktop` file to get a
standard capability working — they declare the capability and the build
generates the right native incantation for each target.

---

## 1. Current State

### The single declaration point

```cmake
OmegaWTKApp(
    NAME        BasicAppTest
    BUNDLE_ID   "org.omegagraphics.BasicAppTest"
    SOURCES     ...
)
```

`OmegaWTKApp()` already branches per target (`wtk/cmake/OmegaWTKApp.cmake`):

| Target | Entry source | Identity / metadata scaffolding | Notes |
|--------|--------------|--------------------------------|-------|
| macOS | `target/macos/main.mm` | `target/macos/Info.plist.in` (→ `CFBundleIdentifier`), `MainMenu.nib`, ad-hoc codesign via `cmake/codesign.py` | builds a real `.app` bundle |
| Win32 | `target/win32/mmain.cpp` | `target/win32/app.exe.manifest` (DPI, supportedOS), `manifest.rc.in`, `resource_script.rc.in` (icon, string table) | bare `.exe`, no installer |
| Linux/GTK | `target/gtk/main.cpp` | *(none — no `.desktop`, no icon install)* | bare ELF |
| iOS | `target/ios/main.mm` | `target/ios/Info.plist.in` | via kreate |
| Android | `target/android/app/main_app.cpp` | `build.gradle` (`applicationId 'com.example.myapp'`), `AndroidManifest.xml` | gradle project |

The cross-platform bootstrap already exists: `target/AppEntryPoint.cpp` exposes
`OmegaWTKCreateApp` / `OmegaWTKRunApp` / `OmegaWTKDestroyApp`, and
`NativeAppLaunchArgs` carries argc/argv (see Native proposal §2.4).

### Key gaps

1. **App identity is fragmented.** `BUNDLE_ID` feeds only the Apple plists.
   Win32 has no AUMID, Linux has no `.desktop` app-id, Android hardcodes
   `com.example.myapp`. The same logical identity is spelled four different ways
   or missing.
2. **No capability registration.** OS features that require *install-time*
   declaration (notifications, file associations, URL schemes, background modes)
   have no cross-platform expression. The runtime APIs exist (e.g.
   `NotificationCenter`) but silently no-op when the install-time half is absent.
3. **Signing/distribution is dev-only.** macOS is ad-hoc signed
   (`CODE_SIGNATURE` defaults to `-`); no hardened runtime, no notarization,
   no entitlements file. Win32/Linux/Android have no signing path.
4. **Icons are inconsistent.** macOS plist references a (commented-out) icon;
   Win32 `.rc` references `app.ico`/`small.ico` that aren't generated; no icon
   pipeline produces the per-target formats from one source.

---

## 2. Proposed Cross-Platform Surface

### 2.1 Unified app identity + capability declaration

Extend `OmegaWTKApp()` so production metadata is declared once:

```cmake
OmegaWTKApp(
    NAME         MyApp
    IDENTITY     com.acme.myapp        # the ONE identity; lowered to all targets
    DISPLAY_NAME "My App"
    VERSION      1.2.0                 # → CFBundleShortVersionString / FILEVERSION / versionName
    ICON         assets/icon.png       # one high-res source; lowered to .icns/.ico/mipmaps
    CAPABILITIES Notifications UrlScheme=myapp
    SOURCES      ...
)
```

`IDENTITY` is the single source of truth, lowered to:

| Target | Lowered to |
|--------|-----------|
| macOS / iOS | `CFBundleIdentifier` (already wired — replaces `BUNDLE_ID`) |
| Win32 | AppUserModelID (AUMID) — set at runtime *and* stamped on the Start-Menu shortcut |
| Linux/GTK | `.desktop` filename + `StartupWMClass` + desktop-entry app id |
| Android | gradle `applicationId` + `package` |

> `BUNDLE_ID` stays as a deprecated alias for `IDENTITY` so existing call sites
> (the test apps) keep building.

### 2.2 Capability registry

A capability is declared cross-platform and *lowered* to whatever each target
needs at install time **and** wired to fire any required runtime registration
automatically (so app authors don't call platform setup by hand). The build
embeds the resolved capability set into the app (a small generated
`OmegaWTKAppManifest` the platform `NativeApp` reads at startup), so the runtime
half and the install-time half can never drift.

Each capability gets one section below with the same shape: **cross-platform
declaration → per-target lowering → runtime hook**.

---

## 3. Capability: Notifications  *(the driver for this plan)*

The `NotificationCenter` runtime API is complete across all three desktop
backends (Native proposal §2.11). But delivery depends on install-time identity
that WTK does not yet set up. Declaring `CAPABILITIES Notifications` should make
notifications *actually deliver* on a fresh install of each target.

| Platform | Install-time requirement | Runtime requirement | Status today |
|----------|--------------------------|---------------------|--------------|
| **macOS** | Bundled `.app` **+** `CFBundleIdentifier` **+** code signature (ad-hoc OK for dev). No plist *usage-description* key — `UNUserNotificationCenter` uses none. For **distribution**: hardened runtime + notarization; `aps-environment` entitlement **only** for *remote* push (local notes need none). | `requestAuthorization` prompt (already called) | ✅ bundle+id+ad-hoc-sign already satisfied; ⚠️ no entitlements file / hardened-runtime path for distribution |
| **Win32** | An **AUMID** *and* a **Start-Menu shortcut** carrying `System.AppUserModel.ID` = that AUMID. Without the shortcut, `ToastNotificationManager` silently drops toasts on unpackaged apps. (MSIX packaging supplies identity instead, if ever adopted.) | `SetCurrentProcessExplicitAppUserModelID` before first send (backend caches an AUMID) | ❌ no AUMID derived from identity, no shortcut creation |
| **Linux/GTK** | None required. A `.desktop` file with a matching app-id lets the notification daemon attribute the toast and show the app icon (production polish, not a blocker). | none (libnotify has no permission model; `requestPermission` returns Authorized) | ✅ works if libnotify + daemon present; ⚠️ no `.desktop` so no icon/attribution |
| **iOS** | Bundled app + id (same as macOS). Remote push needs `aps-environment`; local notes do not. | `requestAuthorization` prompt | n/a (mobile track) |
| **Android** | `POST_NOTIFICATIONS` permission in `AndroidManifest.xml` (runtime-granted on API 33+); a notification **channel** (API 26+). | request permission (API 33+) + create channel before posting | ❌ manifest/channel not wired |

### Lowering `CAPABILITIES Notifications`

- **macOS/iOS** — no plist key needed for local notifications. Generate an
  `.entitlements` file (empty/minimal for local; add `aps-environment` only if a
  future `Push` capability is declared) and pass it to the codesign step so a
  hardened-runtime/distribution build is one flag away. Document that running the
  **bundle** (`open MyApp.app`), not the inner Mach-O, is required for the
  permission prompt.
- **Win32** — derive the AUMID from `IDENTITY`, (a) ensure
  `SetCurrentProcessExplicitAppUserModelID` is called in the platform bootstrap
  from the embedded manifest, and (b) generate a Start-Menu shortcut (install
  step / first-run self-install) stamped with the AUMID. Keep MSIX as a future
  alternative.
- **Linux/GTK** — generate and install a `.desktop` file whose id matches
  `IDENTITY` and references the lowered icon, so the daemon attributes
  notifications correctly.
- **Android** — inject `<uses-permission android:name="POST_NOTIFICATIONS"/>`
  into the manifest and auto-create a default notification channel in the
  bootstrap; gate the runtime permission request on API level.

### Runtime hook

When the embedded manifest lists `Notifications`, the platform `NativeApp`
bootstrap performs the required setup automatically (Win32 AUMID; Android
channel) **before** `omegaWTKMain` runs, so `NotificationCenter::send` works
without app-author ceremony. This keeps the app code identical across targets —
exactly the `BasicAppTest` usage (`requestPermission` then `send`).

---

## 4. App Identity & Signing  *(cross-cutting, enables §3 and distribution)*

| Concern | macOS | Win32 | Linux | Android |
|---------|-------|-------|-------|---------|
| Identity | `CFBundleIdentifier` ✅ | AUMID (new) | `.desktop` id (new) | `applicationId` (de-hardcode) |
| Dev signing | ad-hoc ✅ | none | none | debug keystore |
| Dist signing | Developer ID + hardened runtime + **notarization** | Authenticode `signtool` | (optional) GPG / flatpak | release keystore |
| Entitlements | `.entitlements` (new) | n/a (manifest `requestedExecutionLevel` exists) | n/a | manifest permissions |

`OmegaWTKApp()` gains optional `SIGNING_IDENTITY` / `ENTITLEMENTS` /
`NOTARIZE` knobs; `CODE_SIGNATURE` (already present) remains the macOS dev
default of `-`.

---

## 5. Icons & Metadata

One `ICON` source lowered per target: `.icns` (macOS), `.ico` multi-res +
`resource_script.rc.in`'s `app.ico`/`small.ico` (Win32, currently referenced but
not generated), `.desktop` + hicolor PNGs (Linux), mipmap set (Android). Same for
`VERSION` → `CFBundleShortVersionString` / `.rc` `FILEVERSION` /
gradle `versionName`.

---

## 6. Implementation Priority

| Priority | Item | Rationale |
|----------|------|-----------|
| **P1** | `IDENTITY` unification in `OmegaWTKApp()` (alias `BUNDLE_ID`) | Prerequisite for every capability; low risk |
| **P1** | Capability registry + embedded `OmegaWTKAppManifest` + bootstrap read | The mechanism §3 rides on |
| **P1** | Notifications lowering — Win32 AUMID+shortcut, macOS `.entitlements`, GTK `.desktop` | Closes the gap this plan was spun off from |
| **P2** | Icon pipeline (one source → per-target formats) | Visible polish; unblocks Win32 `.rc` icon refs |
| **P2** | Distribution signing (notarization, signtool, release keystore) | Needed before shipping, not before dogfooding |
| **P3** | Additional capabilities (UrlScheme, FileAssociations, BackgroundModes) | Same registry pattern, add as needed |

---

## 7. File Change Summary

### New
- `wtk/docs/App-Building-Plan.md` — this document
- `wtk/target/macos/App.entitlements.in` — generated per-app entitlements
- `wtk/target/linux/app.desktop.in` — `.desktop` template (new `target/linux/`)
- `wtk/target/win32/install_shortcut.{cpp,ps1}` — AUMID-stamped Start-Menu shortcut
- A generated `OmegaWTKAppManifest` header/resource embedded into each app

### Modified
- `wtk/cmake/OmegaWTKApp.cmake` — `IDENTITY`/`VERSION`/`ICON`/`CAPABILITIES`/`SIGNING_*` args; per-target lowering
- `wtk/target/macos/Info.plist.in` — `VERSION` wiring; entitlements hookup at codesign
- `wtk/target/win32/resource_script.rc.in` — generated icon + `FILEVERSION`
- `wtk/target/android/app/build.gradle` + `AndroidManifest.xml` — `applicationId` from `IDENTITY`, `POST_NOTIFICATIONS`
- Platform `NativeApp` bootstraps — read embedded manifest, run capability registration (Win32 AUMID, Android channel) before `omegaWTKMain`

### Unchanged (by design)
- App author code — the whole point: `NotificationCenter` usage (and other
  runtime APIs) stays identical across targets; only the one-line capability
  declaration is added.

---

## 8. Open Questions

1. **Self-install vs. installer for the Win32 shortcut.** First-run
   self-registration (create the Start-Menu shortcut on launch if missing) keeps
   the dev loop simple but writes to the user profile; a real installer (MSI/MSIX)
   is cleaner for distribution. Which is the default?
2. **Where does the embedded manifest live** — a generated C++ TU linked in, or a
   platform resource (Win32 `.rc` string, macOS bundle resource)? A linked TU is
   the most uniform and avoids per-target resource plumbing.
3. **kreate overlap.** iOS/Android identity already partly flows through kreate;
   confirm `OmegaWTKApp()` is the right owner for mobile identity or whether it
   delegates to kreate. (No kREATE.)
4. **Capability granularity** — is `Notifications` one capability, or split into
   `LocalNotifications` / `RemotePush` (the latter being what pulls in
   `aps-environment` on Apple and FCM on Android)?
