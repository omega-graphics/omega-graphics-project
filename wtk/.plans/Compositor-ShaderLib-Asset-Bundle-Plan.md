# Compositor ShaderLib Asset Bundle Plan

Migrate loading of the compositor shader library from a loose
`compositor.omegasllib` file (resolved next to the executable) to an entry
inside the app's `default.pak` asset bundle. Bundle is unencrypted for now.

## Current state

- The compositor shader library is a precompiled `compositor.omegasllib`
  produced by `add_omegasl_lib(OmegaWTKCompositorShaderLib ...)` in
  [wtk/CMakeLists.txt:344-349](../CMakeLists.txt) and copied next to the
  executable (Resources/ on macOS, exe-dir on Win/Linux) by
  [wtk/cmake/OmegaWTKApp.cmake:15/51/62](../cmake/OmegaWTKApp.cmake).
- `PipelineRegistry::initialize()` resolves the path with platform-specific
  exe-dir probing in
  [Pipeline.cpp:24-63](../src/Composition/backend/Pipeline.cpp), then calls
  `gte.graphicsEngine->loadShaderLibrary(FS::Path)` at
  [Pipeline.cpp:96](../src/Composition/backend/Pipeline.cpp).
- `AppInst` already opens a separate app-level bundle at `./default.pak`
  (CWD-relative, partially broken on macOS) in
  [App.cpp:27-33](../src/UI/App.cpp) and stashes it as
  `Optional<AssetBundle> AppInst::assetBundle`. Nothing currently consumes
  `AppInst::getAssetBundle()`.
- `omega-assetc` defaults to `encrypt=true, sign=true`
  ([common/assetc/main.cpp:41-42](../../common/assetc/main.cpp)). The argv
  parser uses presence-true flags
  ([common/assetc/main.cpp:967-991](../../common/assetc/main.cpp)), so there
  is no `--no-encrypt` today — producing an unencrypted bundle needs an
  explicit negation flag.
- `AssetBundle::Impl` holds metadata only — path string, flags, key, entry
  table, name→index map
  ([common/src/assets.cpp:251-259](../../common/src/assets.cpp)). Every
  `load()` re-opens the bundle file via `std::ifstream`, seeks to
  `entry.fileOffset`, reads `entry.storedSize` bytes into a fresh
  `Vector<uint8_t>`, then decrypts/verifies
  ([common/src/assets.cpp:362-401](../../common/src/assets.cpp)). No buffered
  payload, no kept-open handle.
- `OmegaGraphicsEngine::loadShaderLibraryFromInputStream` consumes the
  stream fully incrementally — every read goes through `in.read()` directly
  into its destination buffer
  ([gte/src/GE.cpp:142](../../gte/src/GE.cpp), helpers at
  [gte/src/GE.cpp:40-57](../../gte/src/GE.cpp)). No `tellg`/`seekg`, no
  slurp-then-parse. A streaming `istream` will pay off directly.

## Decisions

1. Use the app's existing `default.pak` (not a separate `wtk-default.pak`).
   The compositor shader library is bundled into it as an entry named
   `compositor.omegasllib` with `AssetType::Shader`.
2. Bundle is unencrypted for now. Add an explicit `--no-encrypt` flag to
   `omega-assetc`; keep encryption default-on. Once WTK works end-to-end
   we'll flip back to encrypted-by-default and provision a key for the
   compositor bundle.
3. Add a streaming API to `AssetBundle` (`stream(name)` returning a
   `std::istream`-backed `UniqueHandle`) so the shader library is never
   fully buffered into memory. `loadShaderLibraryFromInputStream` already
   consumes incrementally, so the win is real.
4. `default.pak` ships in the same location as today's `compositor.omegasllib`
   (Resources/ on macOS, exe-dir on Win/Linux).
5. `OmegaWTKApp()` owns `default.pak` production: it globs the app's
   `ASSET_DIR`, prepends the compositor shaderlib, and invokes `omega-assetc`.
   Apps don't build the pak themselves.
6. Bundle is mandatory at runtime. `AppInst::assetBundle` becomes
   `AssetBundle` (value), not `Optional<AssetBundle>`. Missing/unreadable
   bundle is a hard error.

## Plan

### 1. `omega-assetc --no-encrypt`

[common/assetc/main.cpp:975](../../common/assetc/main.cpp)

- Add `bool noEncrypt = false` to `CompilerOptions`.
- `parser.addFlag(noEncrypt, "no-encrypt", {}, "Disable encryption (encryption is on by default).");`
- After parse: `if(noEncrypt) options.encrypt = false;`

One-liner. Lands first so a test pak can be produced by hand for step 2.

### 2. `AssetBundle::stream()` + slice streambuf

[common/include/omega-common/assets.h:58](../../common/include/omega-common/assets.h),
[common/src/assets.cpp](../../common/src/assets.cpp)

New API:

```cpp
Result<UniqueHandle<std::istream>, String> AssetBundle::stream(StrRef name) const;
```

Implementation:

- Look up the entry by name (same path as `load()` up to that point).
- If `entry.flags` has `Encrypted` or `Compressed` set, return
  `Err("streaming encrypted/compressed entries not yet supported; use load()")`.
  The current pak is neither, so this doesn't block.
- Construct a `SliceStreambuf` (new file-local class in `assets.cpp`):
  - Owns a `std::ifstream` opened on `impl.bundlePath`.
  - Tracks `[entry.fileOffset, entry.fileOffset + entry.storedSize)`.
  - Implements `underflow()` reading into a small internal buffer
    (e.g. 4 KB), capped at slice end.
  - Implements `seekoff`/`seekpos` rebased to the slice.
- Wrap in a `std::istream` subclass that owns the streambuf; return as
  `UniqueHandle<std::istream>`. Each stream is independent (its own
  `ifstream`); no shared state with `Impl`. Concurrent reads are fine; the
  stream can outlive the bundle.

Unit test: hand-built unencrypted pak, assert `stream("foo")` reads bytes
identical to `load("foo")`.

Follow-up (out of scope): once `stream()` exists, `load()` for
uncompressed/unencrypted entries can be reimplemented on top of it to
remove the duplicate read path in `readStoredBytes()`.

### 3. `OmegaWTKApp()` produces `default.pak`

[wtk/cmake/OmegaWTKApp.cmake](../cmake/OmegaWTKApp.cmake)

`OmegaWTKApp()` already takes `ASSET_DIR` as a single-value parameter; it
becomes the source-of-truth for the app's asset tree.

1. `file(GLOB_RECURSE … CONFIGURE_DEPENDS)` under `${_ARG_ASSET_DIR}` (if
   set).
2. Generate `${CMAKE_CURRENT_BINARY_DIR}/${_ARG_NAME}.pak.manifest` at
   configure time listing the globbed files plus
   `${OMEGAWTK_COMPOSITOR_SHADER_LIB} type=shader`.
3. `add_custom_command(OUTPUT default.pak …)` invoking:

   ```
   omega-assetc --no-encrypt \
       --manifest <manifest> \
       --strip-prefix ${ASSET_DIR}/ \
       -o <build>/default.pak
   ```

   Depends on `omega-assetc`, `OmegaWTKCompositorShaderLib`, the manifest,
   and the globbed files.
4. `add_custom_target(${_ARG_NAME}_DefaultPak DEPENDS <build>/default.pak)`;
   `add_dependencies(${_ARG_NAME} ${_ARG_NAME}_DefaultPak)`.
5. Swap `${OMEGAWTK_COMPOSITOR_SHADER_LIB}` → `<build>/default.pak` at
   [:15](../cmake/OmegaWTKApp.cmake), [:51](../cmake/OmegaWTKApp.cmake),
   [:62](../cmake/OmegaWTKApp.cmake). Filename is `default.pak`, lands in
   Resources/ on macOS or exe-dir on Win/Linux.

Empty `ASSET_DIR` still produces a valid `default.pak` containing just the
shader lib.

If the project already has a manifest convention for an example app I'd
follow it instead of `GLOB_RECURSE`; to be confirmed when I open the first
example.

### 4. Mandatory bundle + exe-dir resolution in `AppInst`

[wtk/include/omegaWTK/UI/App.h:19](../include/omegaWTK/UI/App.h),
[wtk/src/UI/App.cpp:27-33](../src/UI/App.cpp)

- Lift the platform exe-dir resolver out of
  [Pipeline.cpp:24-63](../src/Composition/backend/Pipeline.cpp) into a
  shared helper (likely a private header under `wtk/src/Core/`):
  `resolveExeRelativePath(const char *filename) -> OmegaCommon::String`.
- `AppInst::assetBundle` becomes `AssetBundle` (value), not
  `Optional<AssetBundle>`. `getAssetBundle()` returns `AssetBundle&` /
  `const AssetBundle&`.
- In the `AppInst` constructor, resolve `default.pak` next to the exe and
  call `AssetBundle::open(path)`. On error, throw — let the platform `main`
  shim catch and report. Bundle is mandatory; missing bundle is a hard
  error.
- Nothing currently consumes `getAssetBundle()`, so the signature change is
  free.

### 5. `PipelineRegistry::initialize()` consumes the stream

[wtk/src/Composition/backend/Pipeline.cpp:80-115](../src/Composition/backend/Pipeline.cpp)

- Delete `getCompositorShaderLibPath()` and the surrounding
  `#if defined(TARGET_*)` block at
  [:18-63](../src/Composition/backend/Pipeline.cpp) (helper got lifted in
  step 4).
- `auto &bundle = AppInst::inst()->getAssetBundle();`
- `auto streamResult = bundle.stream("compositor.omegasllib");` — error out
  cleanly on failure (missing entry = packaging bug).
- `shaderLibrary_ = gte.graphicsEngine->loadShaderLibraryFromInputStream(*streamResult.value());`

If the `omegaWTK/UI/App.h` → Composition include cycle bites, fall back to
`Composition::SetDefaultBundle(AssetBundle*)` called from `AppInst` before
`InitializeEngine()`. Decide when we hit it.

## Order of work

1. `omega-assetc --no-encrypt` — one-liner; standalone commit.
2. `AssetBundle::stream()` + slice streambuf + unit test — standalone
   commit.
3. `OmegaWTKApp.cmake` produces `default.pak` via assetc and ships it.
4. `AppInst` switches to exe-dir resolution, makes the bundle mandatory,
   drops the `Optional`.
5. `PipelineRegistry::initialize()` consumes
   `bundle.stream("compositor.omegasllib")`; old platform path code deleted.

Steps 3–5 land together (they jointly flip the loading mechanism). Steps 1
and 2 are independent and ship first.

## Risks

- `file(GLOB_RECURSE CONFIGURE_DEPENDS)` reliability across CMake
  generators. If WTK has an existing manifest convention in an example app,
  prefer that.
- `std::ifstream` per `stream()` call holds an FD for the stream's
  lifetime. Cheap at 1 FD per loaded shaderlib. If we ever stream many
  entries concurrently we may want a shared memory-mapped backing — out of
  scope here.
- Replacing `Optional<AssetBundle>` with `AssetBundle` is an ABI break for
  external callers of `AppInst::getAssetBundle()`. Confirmed no in-tree
  consumers; flag in the commit message.

## Future work (out of scope)

- Re-enable encryption (drop the `--no-encrypt` from
  `OmegaWTKApp.cmake`, provision a per-app key).
- Decrypting/decompressing streambuf so `stream()` works for any entry,
  not just raw ones.
- Reimplement `AssetBundle::load()` on top of `stream()` to remove the
  duplicate read path.
