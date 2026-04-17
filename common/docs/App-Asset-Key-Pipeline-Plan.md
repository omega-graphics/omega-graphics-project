# App Asset Key Pipeline Plan

## Goal

Move the asset-bundle decryption key out of the shipped sidecar `.pak.key` file and into the application build pipeline, so the app becomes the key carrier and `assets.pak` can ship by itself.

This plan uses a new `add_omega_graphics_app(...)` macro in `cmake/OmegaGraphicsSuite.cmake` as the canonical integration point, then layers `OmegaWTKApp(...)` and `add_aqua_game(...)` on top of it.

## Current State

Today:

- `omega-assetc` signs and encrypts bundles by default.
- When no explicit `--key-file` is provided, it creates `<output>.key`.
- `AssetBundle::open(path)` auto-loads the sibling key file.
- WTK opens `./assets.pak` directly in `wtk/src/UI/App.cpp`.

That is good enough for casual local testing, but not for shipping. If the key sits next to the bundle, anyone extracting the app payload gets both pieces.

## Constraints

- We want to keep `omega-assetc` as the bundle authoring tool.
- We want app builds to stay mostly declarative from CMake.
- We should not bake the raw key into the `.pak` file.
- We should not rely on a sidecar key file at runtime for shipping builds.
- We should preserve a local-dev path that is easy to use and debug.

## Proposed Direction

Introduce a build pipeline with three outputs:

1. `assets.pak`
2. a generated key source file compiled into the app
3. a generated header/API that runtime code uses to open the bundle with the embedded key

The app build macro becomes responsible for:

- packing assets with `omega-assetc`
- generating an app-private key source from the bundle key
- compiling that source into the app target
- staging only `assets.pak` into the final app layout

The key file becomes an intermediate build artifact, not a shipped runtime artifact.

## Security Model

This is still anti-casual-extraction hardening, not strong DRM.

Embedding the key in the executable is materially better than shipping it beside the bundle because:

- the extraction path is no longer “copy two files”
- the key can be split/encoded into generated code instead of stored verbatim
- platform-specific follow-up hardening becomes possible later

It does not stop a determined reverse engineer with a debugger.

## Phase 1: Build Primitive In OmegaGraphicsSuite

Add a new function in `cmake/OmegaGraphicsSuite.cmake`:

```cmake
function(add_omega_graphics_app)
    cmake_parse_arguments(
        "_ARG"
        ""
        "NAME;BUNDLE_ID;ASSET_DIR;ASSET_PAK_NAME;ASSET_NAMESPACE;BUNDLE_ICON"
        "SOURCES;DEPS;RESOURCES"
        ${ARGN})
endfunction()
```

Responsibilities:

- create the platform app target
- invoke a shared asset-packing helper when `ASSET_DIR` is present
- generate a key-source translation unit
- add that generated source to the app target
- stage `assets.pak` into the app output
- never stage `<output>.key` into the final app bundle or executable directory

This macro should sit above `add_app_bundle(...)` and plain `add_executable(...)`, not replace them internally.

## Phase 2: Asset Packaging Helper

Add a helper in `OmegaGraphicsSuite.cmake`, for example:

```cmake
function(omega_pack_app_assets)
    cmake_parse_arguments(
        "_ARG"
        ""
        "TARGET;ASSET_DIR;OUTPUT_PAK;OUTPUT_KEY;NAMESPACE"
        ""
        ${ARGN})
endfunction()
```

Behavior:

- collect files under `ASSET_DIR`
- invoke `omega-assetc`
- force a deterministic key path in the build tree, for example:
  - `${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_assets.pak`
  - `${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_assets.pak.key`
- produce both artifacts as build outputs

Important rule:

- the `.key` file lives in the build tree only
- the app target depends on it
- install/staging rules must ignore it

## Phase 3: Generate App-Owned Key Source

Add a small generator step after packing assets:

- input: `${TARGET}_assets.pak.key`
- outputs:
  - `${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_AssetKey.cpp`
  - optionally `${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_AssetKey.h`

The generated source should expose a narrow API, for example:

```cpp
namespace MyApp::Assets {
    OmegaCommon::ArrayRef<std::uint8_t> bundleKey();
}
```

Recommended first implementation:

- parse the 32-byte hex key file
- emit a `static constexpr std::uint8_t kBundleKey[32]`
- return an `ArrayRef` over that array

Recommended second-step hardening:

- emit multiple fragments instead of one contiguous array
- reconstruct the key at runtime before calling `AssetBundle::open`
- optionally XOR or permute fragments in generated code

That will not make the key secret against deep analysis, but it will remove the lowest-effort extraction path.

## Phase 4: Runtime Open Path

Add a new convenience overload in `OmegaCommon`:

```cpp
static Result<AssetBundle, String> open(
    FS::Path path,
    ArrayRef<std::uint8_t> key);
```

This already exists.

So the runtime-side plan is mostly about changing app code to use it.

For WTK:

- update `wtk/src/UI/App.cpp`
- if generated key API is available for the current app target, call:
  - `AssetBundle::open(assets_path, MyApp::Assets::bundleKey())`
- keep the sidecar fallback only for local/dev compatibility if explicitly enabled

For AQUA:

- do the same in its app startup path once asset bundles are integrated there

## Phase 5: Canonical Macro Adoption

Once `add_omega_graphics_app(...)` exists, migrate wrappers:

- `OmegaWTKApp(...)` becomes a thin adapter over `add_omega_graphics_app(...)`
- `add_aqua_game(...)` becomes a thin adapter over `add_omega_graphics_app(...)`

That keeps asset/key behavior consistent across app types.

Suggested layering:

1. `add_omega_graphics_app(...)`
2. `OmegaWTKApp(...)` forwards framework/runtime specifics
3. `add_aqua_game(...)` forwards AQUA-specific entry points and platform assets

## Phase 6: Dev vs Shipping Modes

Add an explicit mode switch:

- `OMEGA_GRAPHICS_EMBED_ASSET_KEY=ON` for shipping/default app builds
- `OMEGA_GRAPHICS_EMBED_ASSET_KEY=OFF` for local debugging if desired

When `ON`:

- compile generated key source into the app
- stage only `assets.pak`
- do not stage `.pak.key`

When `OFF`:

- stage both `assets.pak` and `assets.pak.key`
- skip generated key source
- allow `AssetBundle::open(path)` sidecar resolution exactly as it works today

This gives us a clean migration path and a convenient debugging escape hatch.

## Phase 6B: Publisher Key Signing And DRM

This phase upgrades the plan from embedded-key hardening to a real publisher-controlled content-protection pipeline.

### Core Principle

We should not use the publisher private key to encrypt large asset payloads directly.

Instead:

- `omega-assetc` still encrypts asset data with a random symmetric content key
- the user-provided private key signs the bundle manifest, bundle metadata, and DRM license artifacts
- the content key is protected by wrapping or remote delivery, not by direct private-key encryption

That distinction matters:

- asymmetric signing gives authenticity and publisher control
- symmetric encryption gives practical bulk-data protection
- DRM protects the symmetric content key from trivial extraction

### Proposed Bundle / DRM Model

For a shipping DRM profile:

1. `omega-assetc` generates a random 32-byte content key for the bundle.
2. Assets are encrypted with that content key using AES-256-GCM.
3. `omega-assetc` emits a signed manifest that includes:
   - bundle hash
   - asset table hash
   - content-key identifier
   - DRM profile identifier
   - publisher / product metadata
4. The manifest is signed with the user-provided publisher private key.
5. The raw content key is not staged into the app output.
6. The content key is either:
   - wrapped for a target runtime trust anchor, or
   - omitted entirely from the shipped build and delivered by a license server at runtime.

### New `omega-assetc` Inputs

Add publisher/DRM-oriented options such as:

```text
--signing-key <pem>          Publisher private signing key (required for DRM builds)
--signing-cert <pem>         Optional publisher certificate / chain
--drm-profile <name>         none | embedded | wrapped | license-server
--key-wrap-pubkey <pem>      Public key used to wrap the content key for runtime delivery
--license-id <id>            Product / content identifier used by the DRM system
--license-endpoint <url>     Runtime license service endpoint metadata
```

### DRM Profiles

#### `embedded`

- current hardening model
- app gets generated key source
- no sidecar key shipped

#### `wrapped`

- bundle content key is wrapped to an app/runtime public key
- app unwraps at runtime using a platform-protected private key
- best fit for:
  - macOS Keychain / Secure Enclave
  - Windows DPAPI / CNG key storage
  - Linux TPM or desktop keyring where available

#### `license-server`

- no content key ships in plaintext or recoverable wrapped form inside the build artifacts
- app starts, proves entitlement, and requests a license
- license service returns a signed license blob carrying either:
  - the content key encrypted for the app/device session, or
  - a short-lived session key used to decrypt a wrapped content key in the bundle

This is the strongest proposed model in this plan.

### `add_omega_graphics_app(...)` Integration

Extend the app macro with DRM-facing arguments:

```cmake
add_omega_graphics_app(
    NAME MyApp
    BUNDLE_ID com.example.myapp
    ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Assets
    ASSET_DRM_PROFILE license-server
    ASSET_SIGNING_KEY ${CMAKE_SOURCE_DIR}/keys/publisher_signing.pem
    ASSET_SIGNING_CERT ${CMAKE_SOURCE_DIR}/keys/publisher_cert.pem
    ASSET_KEY_WRAP_PUBKEY ${CMAKE_SOURCE_DIR}/keys/runtime_wrap_pub.pem
    ASSET_LICENSE_ID myapp-main-assets
    ASSET_LICENSE_ENDPOINT https://license.example.com/v1/assets
    SOURCES ...
)
```

The build macro should:

- pass the signing private key into `omega-assetc`
- select the DRM profile
- generate runtime config or source for the license/product identifiers
- avoid staging any plaintext bundle key in shipping DRM modes

### Runtime DRM Flow

For `license-server`:

1. App loads `assets.pak`.
2. App verifies publisher metadata with the embedded public verification key.
3. App requests a license using:
   - app identity
   - bundle key identifier
   - app version / build id
   - optional user entitlement token
   - optional device attestation
4. License service validates entitlement and returns a signed license payload.
5. App verifies the license signature using the embedded publisher public key or a dedicated DRM verification key.
6. App derives or unwraps the content key.
7. `AssetBundle::open(...)` uses that runtime key to decrypt assets.

For `wrapped`:

1. App loads `assets.pak`.
2. App retrieves a platform-protected unwrap key.
3. App unwraps the bundle content key locally.
4. App opens the bundle.

### Protecting The Key

This phase should move protection to one of these trust anchors:

- platform secure storage
- hardware-backed device key
- remote entitlement / license server

Recommended order of strength:

1. `license-server`
2. `wrapped`
3. `embedded`
4. sidecar `.key` file

### What The Private Key Is Actually For

The user-provided private key should be used to:

- sign bundle manifests
- sign license blobs
- sign content-key wrapping metadata
- establish publisher identity

It should not be used to directly encrypt the full asset payload.

If we need asymmetric encryption in the pipeline, use a public key for wrapping and the corresponding private key for unwrapping, not the publisher signing key.

### Concrete Deliverables For This Phase

- extend `omega-assetc` with publisher signing inputs
- add signed manifest generation to the bundle format or a sidecar authenticated metadata block
- add DRM profile selection to `add_omega_graphics_app(...)`
- add runtime verification of publisher signatures
- add either:
  - local wrapped-key support, or
  - a license client abstraction in OmegaCommon / WTK

### Recommended First Slice For DRM

The first DRM-focused implementation should be:

1. add `--signing-key`
2. add signed bundle-manifest output
3. add `ASSET_DRM_PROFILE embedded|wrapped|license-server`
4. implement `wrapped` first for offline-friendly shipping builds
5. add `license-server` after the manifest and verification path are stable

## Phase 7: Platform-Specific Staging

### macOS

Stage `assets.pak` as an app resource:

- `MyApp.app/Contents/Resources/assets.pak`

Generated key source is compiled into the app binary.

No `.key` file is copied into `Contents/Resources`.

### Windows

Stage `assets.pak` next to the executable for now:

- `Apps/MyApp/assets.pak` or `Apps/MyApp.exe` sibling

Generated key source is compiled into the `.exe`.

No `.key` file is copied to the output directory.

Possible later upgrade:

- embed the bundle itself as a Windows resource and load from memory

### Linux

Stage `assets.pak` next to the executable or under a known resource dir.

Generated key source is compiled into the ELF.

No `.key` file is staged.

## Phase 8: Failure Behavior

The runtime path should fail loudly and specifically:

- if the app was built for embedded-key mode but the generated key API is missing, fail at compile time
- if `assets.pak` is missing, fail at startup with a path-specific error
- if bundle authentication fails, fail with the existing tamper/authentication message
- if a dev build expects a sidecar key and it is missing, keep the current explicit key error

## Concrete File Plan

### New / Expanded Build Logic

- `cmake/OmegaGraphicsSuite.cmake`
  - add `add_omega_graphics_app(...)`
  - add `omega_pack_app_assets(...)`
  - add generated-key source step
  - add DRM-oriented asset arguments and staging rules

### Wrapper Migration

- `wtk/cmake/OmegaWTKApp.cmake`
- `aqua/cmake/AquaGame.cmake`

### Runtime Wiring

- `wtk/src/UI/App.cpp`
- later AQUA app startup path(s)
- DRM verification / license acquisition runtime path

### Optional Generator Script

If the generated C++ becomes non-trivial, add:

- `common/assetc/generate_asset_key_source.py`

That is preferable to embedding large codegen logic directly in CMake.

### Likely New DRM Files

- `common/include/omega-common/drm.h`
- `common/src/drm.cpp`
- `common/assetc/generate_asset_manifest.py` or equivalent helper
- optional license client integration files under `common/src/`

## Recommended Incremental Rollout

1. Add `omega_pack_app_assets(...)` and generated-key source emission.
2. Add `add_omega_graphics_app(...)` in `OmegaGraphicsSuite.cmake`.
3. Migrate `OmegaWTKApp(...)` to use it.
4. Update `wtk/src/UI/App.cpp` to open with the embedded key API.
5. Keep sidecar-key fallback behind a dev toggle.
6. Migrate `add_aqua_game(...)`.
7. Remove sidecar-key staging from shipping app flows.
8. Add publisher-signed manifest support to `omega-assetc`.
9. Add DRM profile support to `add_omega_graphics_app(...)`.
10. Implement wrapped-key or license-server runtime resolution.

## Testing Plan

### CMake / Packaging

- verify app builds emit `assets.pak`
- verify shipping builds do not emit `.pak.key` into final app output
- verify generated key source is added to the target

### Runtime

- app opens `assets.pak` successfully with embedded key
- tampered bundle still fails authentication
- shipping build fails if bundle is copied without the app binary
- publisher signature verification fails on modified manifest metadata
- DRM build fails closed when no valid wrapped key or license is available
- wrapped-key builds can open assets offline on authorized targets
- license-server builds can acquire a valid license and reject expired or forged licenses

### Regression

- local dev mode with sidecar key still works when explicitly enabled
- existing `omega-assetc` integration tests remain green

## Recommended First Implementation Scope

The first patch should stop here:

- add `omega_pack_app_assets(...)`
- add generated key source from `.pak.key`
- add `add_omega_graphics_app(...)`
- migrate `OmegaWTKApp(...)`
- update `wtk/src/UI/App.cpp`

That gives us a full vertical slice without forcing AQUA migration in the same change.

The first DRM patch after that should stop here:

- add publisher-signed manifest support to `omega-assetc`
- add `ASSET_SIGNING_KEY` and `ASSET_DRM_PROFILE` to `add_omega_graphics_app(...)`
- verify publisher signatures at runtime
- implement `wrapped` DRM before `license-server`
