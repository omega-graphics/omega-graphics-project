#!/bin/bash
# Test: Xcode project generator produces valid .xcodeproj/project.pbxproj
# Validates: Executable + Archive with dependency, source references, target configs
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AUTOM="$SCRIPT_DIR/../../bin/autom"
TOOLCHAINS="$SCRIPT_DIR/../../tools/default_toolchains.json"

cd "$SCRIPT_DIR"

# Clean previous outputs
rm -rf *.xcodeproj AUTOMINSTALL

echo "=== gen-xcode: Running autom --xcode ==="
"$AUTOM" --xcode --toolchains "$TOOLCHAINS" .

FAIL=0

check_file() {
    if [ ! -f "$1" ]; then
        echo "FAIL: Missing $1"
        FAIL=1
    else
        echo "OK: $1"
    fi
}

check_dir() {
    if [ ! -d "$1" ]; then
        echo "FAIL: Missing directory $1"
        FAIL=1
    else
        echo "OK: $1/"
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

# --- .xcodeproj bundle ---
XCODEPROJ="GenXcodeTest.xcodeproj"
check_dir "$XCODEPROJ"

PBXPROJ="$XCODEPROJ/project.pbxproj"
check_file "$PBXPROJ"
check_not_empty "$PBXPROJ"

# Validate pbxproj structure
check_contains "$PBXPROJ" "archiveVersion = 1"
check_contains "$PBXPROJ" "objectVersion"
check_contains "$PBXPROJ" "PBXProject"
check_contains "$PBXPROJ" "PBXNativeTarget"
check_contains "$PBXPROJ" "PBXGroup"

# Validate targets
check_contains "$PBXPROJ" "CodecLib"
check_contains "$PBXPROJ" "xcodeapp"

# Validate source file references
check_contains "$PBXPROJ" "codec.cpp"
check_contains "$PBXPROJ" "main.cpp"

# Validate build configurations exist
check_contains "$PBXPROJ" "XCBuildConfiguration"
check_contains "$PBXPROJ" "XCConfigurationList"

# Validate build phases
check_contains "$PBXPROJ" "PBXSourcesBuildPhase"
check_contains "$PBXPROJ" "PBXFrameworksBuildPhase"

if [ $FAIL -ne 0 ]; then
    echo "=== gen-xcode: FAILED ==="
    exit 1
fi
echo "=== gen-xcode: PASSED ==="
