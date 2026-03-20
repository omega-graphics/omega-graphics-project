# Distribution Module Plan (`dist.autom`)

## Purpose

`dist.autom` provides platform-specific packaging functions that turn built targets into
distributable installers and packages:

| Platform | Format | Tool |
|----------|--------|------|
| macOS | `.pkg` (flat installer) | `pkgbuild` + `productbuild` |
| macOS | `.dmg` (disk image) | `hdiutil` |
| Linux/Debian | `.deb` | `dpkg-deb` |
| Windows | `.msi` (Windows Installer) | WiX (`candle` + `light`) |
| Windows | `.exe` (NSIS installer) | `makensis` |

## Design

Each packaging format needs two things:
1. **Metadata generation** — control files, WiX XML, NSIS scripts, Info.plist fragments.
   These are templated from package metadata (name, version, maintainer, etc.)
2. **Tool invocation** — running the packaging tool on the staged content.

Both are handled by Python helper scripts invoked via `Script` targets. This matches the
existing pattern set by `codesign.py` in the apple module.

### Python Scripts

Three scripts under `autom/modules/dist/`:

```
modules/dist/
  dist_macos.py     # pkgbuild, productbuild, hdiutil
  dist_debian.py    # generates DEBIAN/control, runs dpkg-deb
  dist_windows.py   # generates .wxs, runs candle + light (MSI)
                    # or generates .nsi, runs makensis (EXE)
```

Each script accepts a JSON manifest on stdin (piped from a generated file) containing
all package metadata and file lists. This avoids shell quoting issues with complex
arguments and keeps the Script target args simple.

### Manifest Format

The `dist.autom` module generates a JSON manifest file via `config_file`, then passes
its path to the Python script. This is the contract between the AUTOM layer and Python:

```json
{
  "name": "myapp",
  "version": "1.0.0",
  "description": "My Application",
  "maintainer": "dev@example.com",
  "license": "MIT",
  "arch": "x86_64",
  "install_prefix": "/usr/local",
  "files": [
    {"src": "myapp", "dest": "bin/myapp"},
    {"src": "libfoo.so", "dest": "lib/libfoo.so"}
  ],
  "depends": ["libc6 (>= 2.31)"]
}
```

The manifest is written as a `.dist.json` template with `@variable@` substitution via
`config_file`, so package metadata flows from AUTOM variables into the Python scripts.

## Module API

### `DistPackage(name, version, targets, files, description, maintainer)`

Top-level function. Dispatches to the right platform packager based on `autom.target_os`.

Parameters:
- `name` — package/product name
- `version` — version string (e.g. `"1.0.0"`)
- `targets` — array of target names to include (must be already defined)
- `files` — array of extra files to include (e.g. config files, docs)
- `description` — one-line description
- `maintainer` — maintainer name/email

Returns: the final packaging Script target

### `MacOSPkg(name, version, targets, files, identifier, signing_id)`

macOS `.pkg` installer via `pkgbuild` + `productbuild`.

- `identifier` — reverse-DNS bundle identifier (e.g. `"com.example.myapp"`)
- `signing_id` — optional Developer ID Installer identity for signing

Pipeline:
1. `Mkdir` — create staging directory `_stage/{name}`
2. `Copy` — copy built targets and extra files into staging tree
3. `Script(dist_macos.py --format pkg)` — generates Distribution XML, runs
   `pkgbuild` on the staging root, then `productbuild` for the final signed `.pkg`

Output: `{name}-{version}.pkg`

### `MacOSDmg(name, version, targets, files, volume_name)`

macOS `.dmg` disk image via `hdiutil`.

- `volume_name` — name shown when the DMG is mounted

Pipeline:
1. `Mkdir` — create staging directory
2. `Copy` — copy targets and files into staging
3. `Script(dist_macos.py --format dmg)` — runs `hdiutil create` on staging dir

Output: `{name}-{version}.dmg`

### `DebianPackage(name, version, targets, files, description, maintainer, depends, section, priority)`

Debian `.deb` package via `dpkg-deb`.

- `depends` — array of package dependencies (e.g. `["libc6 (>= 2.31)"]`)
- `section` — Debian section (default `"utils"`)
- `priority` — package priority (default `"optional"`)

Pipeline:
1. `Mkdir` — create `{name}_deb/DEBIAN/` and `{name}_deb/usr/local/bin/` etc.
2. `Copy` — copy built targets and files into package tree
3. `Script(dist_debian.py)` — generates `DEBIAN/control` from manifest, runs
   `dpkg-deb --build`

Output: `{name}_{version}_{arch}.deb`

### `WindowsMSI(name, version, targets, files, manufacturer, upgrade_guid)`

Windows `.msi` installer via WiX Toolset.

- `manufacturer` — company/author name for the installer UI
- `upgrade_guid` — stable GUID for upgrade detection (must persist across versions)

Pipeline:
1. `Mkdir` — create staging directory
2. `Copy` — stage files
3. `Script(dist_windows.py --format msi)` — generates `.wxs` XML from manifest,
   runs `candle` to compile to `.wixobj`, runs `light` to link into `.msi`

Output: `{name}-{version}.msi`

### `WindowsNSIS(name, version, targets, files, manufacturer)`

Windows `.exe` installer via NSIS.

- `manufacturer` — company/author name

Pipeline:
1. `Mkdir` — create staging directory
2. `Copy` — stage files
3. `Script(dist_windows.py --format nsis)` — generates `.nsi` script from manifest,
   runs `makensis`

Output: `{name}-{version}-setup.exe`

## Target Dependency Chain

Each packaging function produces this chain:

```
built targets (already exist)
  └─ _stage mkdir
       └─ _stage_copy (Copy targets + files into staging)
            └─ _manifest (config_file generates .dist.json)
                 └─ _package (Script: python3 dist_*.py --manifest manifest.json)
                      └─ GroupTarget(name) wraps the final output
```

All intermediate targets are namespaced with the package name to avoid collisions when
multiple packages are defined in a single project.

## `dist.autom` Module Structure

```
import "fs"

var dist_macos_script = fs_abspath(path:"./dist/dist_macos.py")
var dist_debian_script = fs_abspath(path:"./dist/dist_debian.py")
var dist_windows_script = fs_abspath(path:"./dist/dist_windows.py")

func MacOSPkg(name, version, targets, files, identifier, signing_id) {
    var stage_dir = name + "_pkg_stage"
    var stage_mkdir = Mkdir(name:stage_dir, dest:stage_dir)
    var stage_copy = Copy(name:name + "_pkg_copy", sources:targets + files, dest:stage_dir)
    stage_copy.deps = [stage_dir]
    var output = name + "-" + version + ".pkg"
    var pkg = Script(
        name:name + ".pkg",
        cmd:"python3",
        args:[dist_macos_script, "--format", "pkg",
              "--name", name, "--version", version,
              "--identifier", identifier,
              "--signing-id", signing_id,
              "--stage-dir", stage_dir,
              "--output", output],
        outputs:[output]
    )
    pkg.deps = [name + "_pkg_copy"]
    return pkg
}

func MacOSDmg(name, version, targets, files, volume_name) { ... }
func DebianPackage(name, version, targets, files, ...) { ... }
func WindowsMSI(name, version, targets, files, ...) { ... }
func WindowsNSIS(name, version, targets, files, ...) { ... }

func DistPackage(name, version, targets, files, description, maintainer) {
    if autom.target_os == "Darwin" {
        return MacOSPkg(name:name, version:version, ...)
    }
    elif autom.target_os == "Linux" {
        return DebianPackage(name:name, version:version, ...)
    }
    elif autom.target_os == "Windows" {
        return WindowsMSI(name:name, version:version, ...)
    }
}
```

## Python Script Designs

### `dist_macos.py`

```
Arguments:
  --format {pkg|dmg}
  --name NAME
  --version VERSION
  --identifier ID        (pkg only)
  --signing-id ID        (pkg only, optional)
  --volume-name NAME     (dmg only)
  --stage-dir DIR
  --output OUTPUT

pkg mode:
  1. pkgbuild --root {stage-dir} --identifier {id} --version {version} {name}.pkg
  2. If signing-id provided:
     productbuild --sign {signing-id} --package {name}.pkg {output}
  3. Else: rename to {output}

dmg mode:
  1. hdiutil create -volname {volume-name} -srcfolder {stage-dir}
     -ov -format UDZO {output}
```

### `dist_debian.py`

```
Arguments:
  --name NAME
  --version VERSION
  --arch ARCH
  --description DESC
  --maintainer MAINTAINER
  --depends DEP1,DEP2,...
  --section SECTION
  --priority PRIORITY
  --stage-dir DIR
  --output OUTPUT

Steps:
  1. Create DEBIAN/control in stage-dir:
       Package: {name}
       Version: {version}
       Architecture: {deb_arch}
       Maintainer: {maintainer}
       Description: {description}
       Depends: {depends}
       Section: {section}
       Priority: {priority}
  2. Map AUTOM arch to Debian arch (x86_64 -> amd64, AARCH64 -> arm64, etc.)
  3. dpkg-deb --build {stage-dir} {output}
```

### `dist_windows.py`

```
Arguments:
  --format {msi|nsis}
  --name NAME
  --version VERSION
  --manufacturer MANUFACTURER
  --upgrade-guid GUID    (msi only)
  --stage-dir DIR
  --output OUTPUT

msi mode:
  1. Generate {name}.wxs:
     - <Product> with Name, Version, Manufacturer, UpgradeCode
     - <Directory> tree mapping staged files to install locations
     - <Component> per file, <Feature> referencing all components
  2. candle {name}.wxs -o {name}.wixobj
  3. light {name}.wixobj -o {output}

nsis mode:
  1. Generate {name}.nsi:
     - !define PRODUCT_NAME, PRODUCT_VERSION
     - InstallDir "$PROGRAMFILES\{name}"
     - Section with File entries for each staged file
     - Uninstaller section
  2. makensis {name}.nsi
```

## Files to Create

| File | Purpose |
|------|---------|
| `modules/dist.autom` | Module entry point with all packaging functions |
| `modules/dist/dist_macos.py` | macOS .pkg and .dmg generation |
| `modules/dist/dist_debian.py` | Debian .deb generation |
| `modules/dist/dist_windows.py` | Windows .msi and .exe installer generation |

## No Engine Changes Required

Everything is built from existing primitives:
- `Mkdir` + `Copy` for staging (wired up in Layer 1)
- `Script` for invoking Python helpers
- `GroupTarget` for dependency chaining
- `config_file` if manifest templating is needed
- `autom.target_os` for platform dispatch

## Example Usage

```
import "dist"

project(name:"MyApp", version:"1.0.0")

var app = Executable(name:"myapp", sources:["src/main.cpp"])
var lib = Shared(name:"libfoo", sources:["src/foo.cpp"])

var pkg = DistPackage(
    name:"myapp",
    version:"1.0.0",
    targets:["myapp", "libfoo"],
    files:["README.md", "LICENSE"],
    description:"My Application",
    maintainer:"dev@example.com"
)
```

On macOS this produces `myapp-1.0.0.pkg`, on Debian `myapp_1.0.0_amd64.deb`,
on Windows `myapp-1.0.0.msi`.
