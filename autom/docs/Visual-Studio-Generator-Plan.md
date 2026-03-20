# Visual Studio Solution Generator Plan

## Background

The current `TargetVisualStudio.cpp` is a skeleton: it opens a `.sln` file, creates a
`.vcxproj` per target with an empty `<Project>` element, and closes. No actual project
content is emitted.

Visual Studio solutions consist of two file types that need full generation:

1. **`.sln`** — the solution file (plain text, not XML). Lists projects with GUIDs,
   declares build configurations, and maps per-project configs.
2. **`.vcxproj`** — MSBuild XML project files. One per compiled target. Contains source
   references, compiler/linker settings, configurations, and project-to-project references.

## Output Format Reference

### `.sln` structure

```
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "name", "name.vcxproj", "{PROJECT-GUID}"
EndProject
...
Global
  GlobalSection(SolutionConfigurationPlatforms) = preSolution
    Debug|x64 = Debug|x64
    Release|x64 = Release|x64
  EndGlobalSection
  GlobalSection(ProjectConfigurationPlatforms) = postSolution
    {PROJECT-GUID}.Debug|x64.ActiveCfg = Debug|x64
    {PROJECT-GUID}.Debug|x64.Build.0 = Debug|x64
    {PROJECT-GUID}.Release|x64.ActiveCfg = Release|x64
    {PROJECT-GUID}.Release|x64.Build.0 = Release|x64
  EndGlobalSection
EndGlobal
```

- The GUID `8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942` is the standard type GUID for C++ projects.
- Each project gets its own unique GUID (deterministic, derived from project name).

### `.vcxproj` structure

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="4.0"
         xmlns="http://schemas.microsoft.com/developer/msbuild/2003">

  <!-- 1. Configuration declarations -->
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64">
      <Configuration>Debug</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>

  <!-- 2. Global properties -->
  <PropertyGroup Label="Globals">
    <ProjectGuid>{GUID}</ProjectGuid>
    <RootNamespace>name</RootNamespace>
  </PropertyGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />

  <!-- 3. Per-config type/toolset -->
  <PropertyGroup Label="Configuration" Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ConfigurationType>Application|StaticLibrary|DynamicLibrary</ConfigurationType>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <!-- ...same for Release... -->

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />

  <!-- 4. Compiler settings (per-config) -->
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">
    <ClCompile>
      <AdditionalIncludeDirectories>dirs;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>defs;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <AdditionalOptions>cflags %(AdditionalOptions)</AdditionalOptions>
      <Optimization>Disabled</Optimization>
    </ClCompile>
    <Link>
      <AdditionalDependencies>libs;%(AdditionalDependencies)</AdditionalDependencies>
      <AdditionalLibraryDirectories>dirs;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalOptions>ldflags %(AdditionalOptions)</AdditionalOptions>
    </Link>
  </ItemDefinitionGroup>

  <!-- 5. Source files -->
  <ItemGroup>
    <ClCompile Include="relative/path/to/source.cpp" />
  </ItemGroup>

  <!-- 6. Project references (dependencies) -->
  <ItemGroup>
    <ProjectReference Include="dep.vcxproj">
      <Project>{DEP-GUID}</Project>
    </ProjectReference>
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

## Architecture

### GUID Generation

Reuse the existing `SHA256Hash` from `ADT.h` to produce deterministic GUIDs from target
names. Take the first 32 hex chars of `SHA256(target_name)` and format as
`XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`. This matches the Xcode generator's approach of
deterministic IDs from `generateObjectID`.

### Platform String

Map `TargetArch` to VS platform string:
- `x86` → `Win32`
- `x86_64` → `x64`
- `ARM` → `ARM`
- `AARCH64` → `ARM64`

### ConfigurationType

Map `TargetType` to VS config type:
- `EXECUTABLE` → `Application`
- `STATIC_LIBRARY` → `StaticLibrary`
- `SHARED_LIBRARY` → `DynamicLibrary`

### Target Types Handled

| AUTOM Target | VS Output |
|---|---|
| `CompiledTarget` (exe/lib/dll) | `.vcxproj` with full ClCompile/Link settings |
| `ScriptTarget` | `.vcxproj` with `CustomBuildStep` pre-build event |
| `GroupTarget` | No `.vcxproj`; referenced as solution folder or dependency only |
| `FSTarget` | `.vcxproj` with `CustomBuildStep` for copy/mkdir/symlink |

### Deferred Finish Pattern

Follow the Xcode generator's pattern: `consumeTarget` accumulates targets into a
`pendingTargets` vector. `finish()` does all the work — resolves dependencies, generates
GUIDs, writes `.vcxproj` files per-target, then writes the `.sln` referencing all of them.
This is necessary because the `.sln` file needs to know all projects and their GUIDs before
it can be written.

### Toolchain Handling

`supportsCustomToolchainRules()` should return `false`. Visual Studio manages its own
toolchain via `<PlatformToolset>` — AUTOM doesn't need to emit toolchain rules. The
toolchain info from AUTOM is only used to inform the `PlatformToolset` value (e.g. `v143`
for VS 2022, `v142` for VS 2019).

### Dependency Resolution

For `CompiledTarget` dependencies that are libraries:
- Add a `<ProjectReference>` element pointing to the dependency's `.vcxproj`
- Include the dependency's GUID

For `SourceGroup` dependencies:
- Inline the source files into the parent target's `<ClCompile>` list (same as Ninja/Xcode)

### Output Directory

If `CompiledTarget.output_dir` is set:
- Set `<OutDir>` to the specified directory
- Set `<TargetName>` and `<TargetExt>` from name/output_ext

## Implementation Steps

### Step 1: Internal data structures

```cpp
struct VsProjectEntry {
    std::shared_ptr<Target> target;
    std::string guid;           // deterministic from name
    std::string name;
    std::string vcxprojPath;    // relative to output dir
    bool isCompiled;
};
```

### Step 2: GUID generation helper

```cpp
std::string generateGUID(const std::string &seed) {
    SHA256Hash hash;
    hash.addData((void*)seed.data(), seed.size());
    std::string hex;
    hash.getResultAsHex(hex);
    // Format: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
    return "{" + hex.substr(0,8) + "-" + hex.substr(8,4) + "-"
         + hex.substr(12,4) + "-" + hex.substr(16,4) + "-"
         + hex.substr(20,12) + "}";
}
```

### Step 3: XML writing helpers

Simple stream-based XML emission (no external XML library needed — matching the existing
pattern of direct `ofstream` writes in every other generator):

```cpp
void xmlLine(std::ostream &out, int indent, const std::string &line);
void xmlOpen(std::ostream &out, int indent, const std::string &tag, const std::string &attrs = "");
void xmlClose(std::ostream &out, int indent, const std::string &tag);
void xmlEmpty(std::ostream &out, int indent, const std::string &tag, const std::string &attrs);
```

### Step 4: `writeVcxproj` for CompiledTarget

Writes a complete `.vcxproj` file for a single compiled target:
1. Project configurations (Debug/Release x platform)
2. Globals (GUID, namespace)
3. Import default props
4. Per-config property groups (ConfigurationType, PlatformToolset, OutDir)
5. Import cpp props
6. Per-config ItemDefinitionGroup (ClCompile settings, Link settings)
7. Source ItemGroup (ClCompile includes)
8. ProjectReference ItemGroup (resolved deps that are compiled targets)
9. Import cpp targets

### Step 5: `writeVcxproj` for ScriptTarget

Writes a `.vcxproj` with:
1. Same config/globals/imports skeleton
2. ConfigurationType = `Utility`
3. CustomBuildStep with Command = script + args, Outputs = declared outputs

### Step 6: `writeSolution`

Writes the `.sln` file:
1. Header with format version
2. One `Project` block per entry (with type GUID, name, path, project GUID)
3. `Global` section with solution/project config mappings

### Step 7: `finish()` orchestration

```
1. Generate GUIDs for all pending targets
2. For each CompiledTarget: writeVcxproj(...)
3. For each ScriptTarget: writeVcxproj(...)
4. writeSolution(all entries)
5. Close solution stream
```

## Files Changed

Only `autom/src/gen/TargetVisualStudio.cpp` — the class already exists, it just needs a
complete implementation replacing the current skeleton.

## Design Decisions

**No external XML library.** The Xcode generator writes a complex nested format with raw
`ofstream` writes. The Ninja generator does the same. The vcxproj XML is simpler than
pbxproj, so direct stream writing is sufficient and consistent with the codebase.

**Deterministic GUIDs.** Using SHA256 of the target name means re-running AUTOM produces
identical `.sln`/`.vcxproj` files (no random UUIDs that churn source control).

**Debug + Release only.** Matching what the Xcode generator does. The Ninja generator also
only handles a single configuration. Custom configs can be added later.

**Deferred write pattern.** The `.sln` needs all project GUIDs upfront. Accumulating in
`consumeTarget` and writing in `finish()` is the same pattern Xcode uses and avoids
two-pass generation.
