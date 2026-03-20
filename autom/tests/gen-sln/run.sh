#!/bin/bash
# Test: Visual Studio solution generator produces valid .sln and .vcxproj files
# Validates: Executable + Archive + Shared library with project references
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AUTOM="$SCRIPT_DIR/../../bin/autom"
TOOLCHAINS="$SCRIPT_DIR/../../tools/default_toolchains.json"

cd "$SCRIPT_DIR"

# Clean previous outputs
rm -f *.sln *.vcxproj AUTOMINSTALL

echo "=== gen-sln: Running autom --sln ==="
"$AUTOM" --sln --toolchains "$TOOLCHAINS" .

FAIL=0

check_file() {
    if [ ! -f "$1" ]; then
        echo "FAIL: Missing $1"
        FAIL=1
    else
        echo "OK: $1"
    fi
}

check_contains() {
    if ! grep -q "$2" "$1" 2>/dev/null; then
        echo "FAIL: $1 missing pattern: $2"
        FAIL=1
    else
        echo "OK: $1 contains '$2'"
    fi
}

check_not_empty() {
    if [ ! -s "$1" ]; then
        echo "FAIL: $1 is empty"
        FAIL=1
    else
        echo "OK: $1 is non-empty ($(wc -c < "$1") bytes)"
    fi
}

# --- .sln file ---
SLN="GenSlnTest.sln"
check_file "$SLN"
check_not_empty "$SLN"
check_contains "$SLN" "Microsoft Visual Studio Solution File"
check_contains "$SLN" "Format Version 12.00"
check_contains "$SLN" "EngineLib"
check_contains "$SLN" "RendererLib"
check_contains "$SLN" "slnapp"
check_contains "$SLN" "8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942"
check_contains "$SLN" "GlobalSection(SolutionConfigurationPlatforms)"
check_contains "$SLN" "GlobalSection(ProjectConfigurationPlatforms)"
check_contains "$SLN" "Debug|"
check_contains "$SLN" "Release|"

# --- .vcxproj files ---
for TARGET in EngineLib RendererLib slnapp; do
    VCXPROJ="$TARGET.vcxproj"
    check_file "$VCXPROJ"
    check_not_empty "$VCXPROJ"
    check_contains "$VCXPROJ" '<?xml version="1.0"'
    check_contains "$VCXPROJ" "Microsoft.Cpp.Default.props"
    check_contains "$VCXPROJ" "Microsoft.Cpp.props"
    check_contains "$VCXPROJ" "Microsoft.Cpp.targets"
    check_contains "$VCXPROJ" "<ProjectGuid>"
    check_contains "$VCXPROJ" "<RootNamespace>$TARGET</RootNamespace>"
    check_contains "$VCXPROJ" "PlatformToolset"
done

# Validate ConfigurationType per target type
check_contains "EngineLib.vcxproj" "<ConfigurationType>StaticLibrary</ConfigurationType>"
check_contains "RendererLib.vcxproj" "<ConfigurationType>DynamicLibrary</ConfigurationType>"
check_contains "slnapp.vcxproj" "<ConfigurationType>Application</ConfigurationType>"

# Validate source file references
check_contains "EngineLib.vcxproj" "engine.cpp"
check_contains "RendererLib.vcxproj" "renderer.cpp"
check_contains "slnapp.vcxproj" "main.cpp"

# Validate project references (slnapp depends on EngineLib and RendererLib)
check_contains "slnapp.vcxproj" "<ProjectReference"
check_contains "slnapp.vcxproj" "EngineLib.vcxproj"
check_contains "slnapp.vcxproj" "RendererLib.vcxproj"

# Validate compiler/linker settings sections exist
check_contains "slnapp.vcxproj" "<ClCompile>"
check_contains "slnapp.vcxproj" "<Link>"
check_contains "slnapp.vcxproj" "Optimization"

# Validate EngineLib (static) uses Lib, not Link
check_contains "EngineLib.vcxproj" "<Lib>"

# Validate Debug/Release configs present in vcxproj
check_contains "slnapp.vcxproj" "Debug|"
check_contains "slnapp.vcxproj" "Release|"
check_contains "slnapp.vcxproj" "MultiThreadedDebugDLL"
check_contains "slnapp.vcxproj" "MultiThreadedDLL"

# Validate GUID consistency: project GUID in .sln matches .vcxproj
for TARGET in EngineLib RendererLib slnapp; do
    VCXPROJ_GUID=$(grep -o '<ProjectGuid>{[^}]*}</ProjectGuid>' "$TARGET.vcxproj" | sed 's/<[^>]*>//g')
    if grep -q "$VCXPROJ_GUID" "$SLN"; then
        echo "OK: $TARGET GUID $VCXPROJ_GUID matches in .sln"
    else
        echo "FAIL: $TARGET GUID $VCXPROJ_GUID not found in .sln"
        FAIL=1
    fi
done

if [ $FAIL -ne 0 ]; then
    echo "=== gen-sln: FAILED ==="
    exit 1
fi
echo "=== gen-sln: PASSED ==="
