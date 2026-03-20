#!/bin/bash
# Test: All three generators handle Script, FS, SourceGroup, and GroupTarget types
# Validates: Each generator correctly handles non-compiled target types
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AUTOM="$SCRIPT_DIR/../../bin/autom"
TOOLCHAINS="$SCRIPT_DIR/../../tools/default_toolchains.json"

FAIL=0

check_file() {
    if [ ! -f "$1" ]; then
        echo "  FAIL: Missing $1"
        FAIL=1
    else
        echo "  OK: $1"
    fi
}

check_dir() {
    if [ ! -d "$1" ]; then
        echo "  FAIL: Missing directory $1"
        FAIL=1
    else
        echo "  OK: $1/"
    fi
}

check_contains() {
    if ! grep -q "$2" "$1" 2>/dev/null; then
        echo "  FAIL: $1 missing pattern: $2"
        FAIL=1
    else
        echo "  OK: $1 contains '$2'"
    fi
}

# =====================================================================
# Ninja generator
# =====================================================================
echo "=== gen-complex [ninja]: Running autom --ninja ==="
cd "$SCRIPT_DIR"
rm -f build.ninja toolchain.ninja AUTOMINSTALL complexapp
rm -rf obj staging
"$AUTOM" --ninja --toolchains "$TOOLCHAINS" .

check_file "build.ninja"
check_file "toolchain.ninja"

# Compiled targets
check_contains "build.ninja" "complexapp"
check_contains "build.ninja" "app.cpp"
# SourceGroup inlined
check_contains "build.ninja" "helper.cpp"
# Script target
check_contains "build.ninja" "gen_version"
check_contains "build.ninja" "gen_version.py"
check_contains "build.ninja" "version.h"
# FS targets (Ninja uses dest path "staging", not target name "stage_dir")
check_contains "build.ninja" "staging"
check_contains "build.ninja" "copy_data"
check_contains "build.ninja" "readme.txt"
# GroupTarget
check_contains "build.ninja" "all"

# =====================================================================
# Visual Studio generator
# =====================================================================
echo ""
echo "=== gen-complex [sln]: Running autom --sln ==="
# Clean ninja outputs first
rm -f build.ninja toolchain.ninja AUTOMINSTALL
rm -rf obj

cd "$SCRIPT_DIR"
"$AUTOM" --sln --toolchains "$TOOLCHAINS" .

SLN="GenComplexTest.sln"
check_file "$SLN"
check_contains "$SLN" "Microsoft Visual Studio Solution File"
check_contains "$SLN" "complexapp"

# Compiled target vcxproj
check_file "complexapp.vcxproj"
check_contains "complexapp.vcxproj" "Application"
check_contains "complexapp.vcxproj" "app.cpp"
# SourceGroup sources inlined into compiled target
check_contains "complexapp.vcxproj" "helper.cpp"

# Script target vcxproj (Utility with CustomBuildStep)
check_file "gen_version.vcxproj"
check_contains "gen_version.vcxproj" "Utility"
check_contains "gen_version.vcxproj" "CustomBuildStep"
check_contains "gen_version.vcxproj" "gen_version.py"
check_contains "gen_version.vcxproj" "version.h"

# FS Mkdir vcxproj
check_file "stage_dir.vcxproj"
check_contains "stage_dir.vcxproj" "Utility"
check_contains "stage_dir.vcxproj" "CustomBuildStep"
check_contains "stage_dir.vcxproj" "staging"

# FS Copy vcxproj
check_file "copy_data.vcxproj"
check_contains "copy_data.vcxproj" "Utility"
check_contains "copy_data.vcxproj" "CustomBuildStep"
check_contains "copy_data.vcxproj" "readme.txt"

# Copy depends on Mkdir — verify ProjectReference
check_contains "copy_data.vcxproj" "<ProjectReference"
check_contains "copy_data.vcxproj" "stage_dir.vcxproj"

# GroupTarget should NOT produce a .vcxproj
if [ -f "all.vcxproj" ]; then
    echo "  FAIL: GroupTarget 'all' should not produce a .vcxproj"
    FAIL=1
else
    echo "  OK: GroupTarget 'all' correctly has no .vcxproj"
fi

# =====================================================================
# Xcode generator
# =====================================================================
echo ""
echo "=== gen-complex [xcode]: Running autom --xcode ==="
# Clean sln outputs
rm -f *.sln *.vcxproj AUTOMINSTALL

cd "$SCRIPT_DIR"
"$AUTOM" --xcode --toolchains "$TOOLCHAINS" .

XCODEPROJ="GenComplexTest.xcodeproj"
check_dir "$XCODEPROJ"
PBXPROJ="$XCODEPROJ/project.pbxproj"
check_file "$PBXPROJ"

check_contains "$PBXPROJ" "complexapp"
check_contains "$PBXPROJ" "app.cpp"
check_contains "$PBXPROJ" "helper.cpp"

# =====================================================================
# Summary
# =====================================================================
echo ""
if [ $FAIL -ne 0 ]; then
    echo "=== gen-complex: FAILED ==="
    exit 1
fi
echo "=== gen-complex: PASSED ==="
