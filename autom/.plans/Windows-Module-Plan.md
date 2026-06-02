# Windows Module Plan (`windows.autom`)

## Current State

```
import "fs"

var rc_prog = find_program(cmd:"rc")

func WindowsResources(name, rc_file) { ... }   // compiles .rc -> .res via Script
func Win32App(name, sources) { ... }            // thin wrapper around Executable
func WindowsStoreApp(name, sources) { }         // empty stub
```

`Win32App` produces a bare `.exe` with no resource embedding, no manifest, no icon, no
subsystem flag, and no DLL staging. `WindowsStoreApp` is empty. Neither is
production-ready.

## What Production Windows Apps Need

### Win32 Desktop Apps

A shipping Win32 `.exe` requires:

1. **Application manifest** — `app.manifest` embedded via `/MANIFEST:EMBED` or via `.rc`.
   Controls UAC elevation, DPI awareness, Windows version compatibility, and COM
   registration. Without it, Windows applies compatibility shims.

2. **Resource file (`.rc`)** — compiled by `rc.exe` into a `.res` linked into the
   executable. Contains:
   - `VERSIONINFO` block (FileVersion, ProductVersion, CompanyName, etc.) — shows in
     file properties and is read by installers/updaters
   - `ICON` resource — the application icon shown in Explorer, taskbar, Alt+Tab
   - `MANIFEST` resource (type `RT_MANIFEST`, id 1) — embeds the app manifest
   - Optionally: string tables, dialogs, accelerators

3. **Subsystem selection** — `/SUBSYSTEM:WINDOWS` for GUI apps (no console window),
   `/SUBSYSTEM:CONSOLE` for CLI tools. Defaults to console if unspecified.

4. **DLL staging** — any dependent DLLs must be copied next to the `.exe` (or into a
   known search path). Applies to both project-built shared libraries and third-party
   DLLs.

5. **Icon** — a `.ico` file referenced from the `.rc` file. Without it, Windows shows
   a generic icon.

6. **Code signing** — `signtool.exe` signs the `.exe` with an Authenticode certificate.
   Unsigned apps trigger SmartScreen warnings and are blocked by enterprise policies.

### Windows Store / MSIX Apps

A UWP or packaged desktop app requires:

1. **MSIX/AppX package** — directory structure with `AppxManifest.xml`, assets, and the
   executable. Produced by `makeappx.exe`.

2. **AppxManifest.xml** — declares identity (name, publisher, version), capabilities
   (network, filesystem, etc.), visual assets, and entry points.

3. **Visual assets** — `Square44x44Logo.png`, `Square150x150Logo.png`,
   `Wide310x150Logo.png`, `StoreLogo.png` at required sizes.

4. **Package signing** — `signtool.exe` with a certificate whose subject matches the
   manifest's Publisher field. Required for sideloading.

5. **PRI resources** — `makepri.exe` compiles a resource index for localized strings
   and assets.

## Proposed API

### `Win32App(name, sources)`

Upgraded from bare `Executable` wrapper to production-ready Win32 app builder.

Parameters:
- `name` — target name
- `sources` — source files

Settable properties on the returned target (via the existing target property system):
- `.cflags`, `.libs`, `.lib_dirs`, `.include_dirs`, `.deps` — standard compiled target props

Additional targets created behind the scenes:
- Resource compilation (if `.rc` source detected in sources)
- DLL staging from resolved shared library deps

What it does:
1. Creates `Executable` target
2. Scans sources for `.rc` files — if found, compiles them via `WindowsResources`
   and links the `.res` as an additional object file
3. Adds `/SUBSYSTEM:WINDOWS` to ldflags by default (GUI app)

### `Win32Console(name, sources)`

Same as `Win32App` but with `/SUBSYSTEM:CONSOLE`. For CLI tools that need a console
window.

### `WindowsResources(name, rc_file)`

Already exists. Compiles a `.rc` file to `.res` via `rc.exe`. No changes needed.

### `WindowsManifest(name, app_name, version, description)`

New. Generates a standard `app.manifest` file via `config_file` from a template.

Returns the path to the generated manifest file, which can be referenced in a `.rc`:

```c
// app.rc
#include <winuser.h>
1 RT_MANIFEST "app.manifest"
1 ICON "app.ico"
```

Pipeline:
1. `config_file(in:"app.manifest.in", out:name + ".manifest")` — substitutes
   `@app_name@`, `@version@`, `@description@` into the manifest template

### `WindowsVersionInfo(name, version, company, description, icon)`

New. Generates a complete `.rc` file with `VERSIONINFO`, icon, and manifest references.

This is the convenience function that ties resources together. A Python helper script
generates the `.rc` because the `VERSIONINFO` block syntax is complex (it needs version
numbers split into comma-separated `MAJOR,MINOR,PATCH,0` format).

Pipeline:
1. `Script(win_resources.py)` — generates `{name}.rc` with VERSIONINFO block, ICON
   reference, and MANIFEST embed
2. Returns a `WindowsResources` target wrapping the generated `.rc`

### `WindowsSign(name, target, cert_file, password, timestamp_url)`

New. Signs a built `.exe` or `.dll` with `signtool.exe`.

Pipeline:
1. `Script` invoking `signtool sign /f {cert_file} /p {password} /t {timestamp_url}
   /fd SHA256 {target_output}`

### `WindowsStageDlls(name, target, dlls)`

New. Copies dependent DLLs next to the executable output.

Pipeline:
1. `Copy(name:name, sources:dlls, dest:target_output_dir)`

### `WindowsStoreApp(name, sources, publisher, display_name, assets_dir)`

New. Packages a desktop app as MSIX for Store distribution or sideloading.

Parameters:
- `name` — package identity name (e.g. `"CompanyName.AppName"`)
- `sources` — source files for the executable
- `publisher` — certificate subject (e.g. `"CN=Company, O=Company, C=US"`)
- `display_name` — human-readable name shown in Start menu
- `assets_dir` — directory containing required visual assets (PNGs)

Pipeline:
1. `Win32App(name, sources)` — builds the core executable
2. `Mkdir` — create staging directory
3. `Copy` — stage executable, assets, and DLLs
4. `Script(win_store.py)` — generates `AppxManifest.xml` from parameters, runs
   `makepri.exe` for resource index, runs `makeappx.exe pack`
5. Optional: `WindowsSign` on the `.msix` output

## Python Helper Scripts

Two scripts under `autom/modules/win/`:

### `win_resources.py`

Generates a `.rc` file with VERSIONINFO, icon, and manifest.

```
Arguments:
  --name NAME
  --version VERSION  (dotted string, e.g. "1.2.3")
  --company COMPANY
  --description DESC
  --icon ICON_PATH   (optional, path to .ico)
  --manifest MANIFEST_PATH  (optional, path to .manifest)
  --output OUTPUT_RC

Steps:
  1. Parse version "1.2.3" into comma format "1,2,3,0"
  2. Write .rc file:
     - #include <winver.h>
     - VS_VERSION_INFO VERSIONINFO block
     - FILEVERSION / PRODUCTVERSION with comma format
     - StringFileInfo block with CompanyName, FileDescription,
       FileVersion, ProductName, ProductVersion
     - If icon: 1 ICON "{icon_path}"
     - If manifest: 1 RT_MANIFEST "{manifest_path}"
```

### `win_store.py`

Generates AppxManifest.xml and runs makeappx.

```
Arguments:
  --name IDENTITY_NAME
  --publisher PUBLISHER
  --display-name DISPLAY_NAME
  --version VERSION
  --executable EXE_NAME
  --assets-dir ASSETS
  --stage-dir DIR
  --output OUTPUT

Steps:
  1. Generate AppxManifest.xml in stage-dir:
     - <Identity Name="{name}" Publisher="{publisher}" Version="{version}"/>
     - <Properties> with DisplayName, Logo
     - <Application> with Executable, EntryPoint="Windows.FullTrustApplication"
     - <Capabilities> with rescap:runFullTrust
     - <VisualElements> with asset references
  2. Run: makepri createconfig /cf priconfig.xml /dq en-US
  3. Run: makepri new /pr {stage-dir} /cf priconfig.xml /of {stage-dir}/resources.pri
  4. Run: makeappx pack /d {stage-dir} /p {output}
```

## Manifest Template

A standard `app.manifest.in` shipped with the module at `modules/win/app.manifest.in`:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="@app_name@" version="@version@.0"/>
  <description>@description@</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
      <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
      <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
      <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
      <supportedOS Id="{e2011457-1546-43c5-a5fe-008deee3d3f0}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

This covers Windows 7 through 11 compatibility, Per-Monitor V2 DPI awareness, and
`asInvoker` UAC (no elevation). The supported OS GUIDs are the standard Microsoft
values for Vista, 7, 8, 8.1, and 10/11.

## Complete Module Structure

```
modules/
  windows.autom              # module entry point
  win/
    win_resources.py          # generates .rc with VERSIONINFO
    win_store.py              # generates AppxManifest.xml, runs makeappx
    app.manifest.in           # manifest template for config_file
```

## `windows.autom` Sketch

```
import "fs"

var rc_prog = find_program(cmd:"rc")
var signtool_prog = find_program(cmd:"signtool")
var win_resources_script = fs_abspath(path:"./win/win_resources.py")
var win_store_script = fs_abspath(path:"./win/win_store.py")
var manifest_template = fs_abspath(path:"./win/app.manifest.in")

func WindowsResources(name, rc_file) {
    var res_target = Script(
        name:name,
        cmd:"rc",
        args:["/fo", name + ".res", rc_file],
        outputs:[name + ".res"]
    )
    return res_target
}

func WindowsManifest(name, app_name, version, description) {
    var manifest_out = name + ".manifest"
    config_file(in:manifest_template, out:manifest_out)
    return manifest_out
}

func WindowsVersionInfo(name, version, company, description, icon) {
    var rc_out = name + ".rc"
    var gen = Script(
        name:name + "_rcgen",
        cmd:"python3",
        args:[win_resources_script,
              "--name", name,
              "--version", version,
              "--company", company,
              "--description", description,
              "--icon", icon,
              "--output", rc_out],
        outputs:[rc_out]
    )
    var res = WindowsResources(name:name, rc_file:rc_out)
    res.deps = [name + "_rcgen"]
    return res
}

func Win32App(name, sources) {
    var exec = Executable(name:name, sources:sources)
    exec.cflags = ["/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE"]
    return exec
}

func Win32Console(name, sources) {
    var exec = Executable(name:name, sources:sources)
    exec.cflags = ["/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE"]
    return exec
}

func WindowsSign(name, target_file, cert_file, password, timestamp_url) {
    var signed = Script(
        name:name,
        cmd:"signtool",
        args:["sign", "/f", cert_file, "/p", password,
              "/t", timestamp_url, "/fd", "SHA256", target_file],
        outputs:[target_file + ".signed"]
    )
    return signed
}

func WindowsStageDlls(name, dlls, dest_dir) {
    var staged = Copy(name:name, sources:dlls, dest:dest_dir)
    return staged
}

func WindowsStoreApp(name, sources, publisher, display_name, assets_dir) {
    var exec = Win32App(name:name, sources:sources)
    var stage_dir = name + "_msix_stage"
    var stage = Mkdir(name:stage_dir, dest:stage_dir)
    var copy_exe = Copy(name:name + "_msix_copy", sources:[name + ".exe"], dest:stage_dir)
    copy_exe.deps = [name, stage_dir]
    var copy_assets = Copy(
        name:name + "_msix_assets",
        sources:fs_glob(path:assets_dir + "/*.png"),
        dest:stage_dir
    )
    copy_assets.deps = [stage_dir]
    var output = name + ".msix"
    var package = Script(
        name:output,
        cmd:"python3",
        args:[win_store_script,
              "--name", name,
              "--publisher", publisher,
              "--display-name", display_name,
              "--version", "1.0.0.0",
              "--executable", name + ".exe",
              "--assets-dir", assets_dir,
              "--stage-dir", stage_dir,
              "--output", output],
        outputs:[output]
    )
    package.deps = [name + "_msix_copy", name + "_msix_assets"]
    GroupTarget(name:name + "_store", deps:[output])
    return exec
}
```

## Dependency Chain — `Win32App` with Full Resources

```
WindowsVersionInfo("myapp", ...)
  └─ Script(win_resources.py) → myapp.rc
       └─ WindowsResources("myapp", "myapp.rc")
            └─ Script(rc /fo myapp.res myapp.rc) → myapp.res

Win32App("myapp", sources)
  └─ Executable("myapp", sources)

GroupTarget("myapp_final", deps:["myapp", "myapp_res"])

// Optional signing:
WindowsSign("myapp_signed", "myapp.exe", cert, pw, ts_url)
  └─ Script(signtool sign ...) → myapp.exe.signed
```

The `.res` from `WindowsVersionInfo` is linked into the executable as an additional
object file. This requires the user to add the `.res` output to the target's link
inputs — or alternatively, we can have `Win32App` accept an optional resources parameter.

## Open Question: Linking `.res` into Executables

The current engine has `CompiledTarget.other_objs` (a `std::vector<std::string>`) which
is designed for exactly this — extra object files to link. But this field is not exposed
as a settable property in the AUTOM scripting language.

**Option A (module-only, no engine change):** Pass the `.res` as an ldflags entry.
MSVC's `link.exe` accepts `.res` files directly on the command line, so adding
`myapp.res` to ldflags works.

**Option B (small engine change):** Expose `other_objs` as a target property in
`Execution.cpp`'s member access handler. This is cleaner but requires an engine patch.

Recommendation: Start with Option A. It works today with no engine changes. Move to
Option B when the engine's property system gets a broader pass.

## No Engine Changes Required

Everything is built from existing primitives:
- `Executable`, `Shared` for compiled targets
- `Script` for `rc.exe`, `signtool.exe`, `win_resources.py`, `win_store.py`, `makeappx`
- `Mkdir` + `Copy` for staging
- `GroupTarget` for dependency chaining
- `config_file` for manifest template substitution
- `find_program` for tool discovery

## Files to Create

| File | Purpose |
|------|---------|
| `modules/windows.autom` | Updated module with all functions |
| `modules/win/win_resources.py` | Generates `.rc` with VERSIONINFO, icon, manifest |
| `modules/win/win_store.py` | Generates AppxManifest.xml, runs makeappx |
| `modules/win/app.manifest.in` | Template for `config_file` substitution |
