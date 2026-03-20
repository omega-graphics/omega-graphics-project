#!/bin/bash
# Test: Ninja generator produces valid build.ninja and toolchain.ninja
# Validates: Executable + Archive + SourceGroup dependency chain
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AUTOM="$SCRIPT_DIR/../../bin/autom"
TOOLCHAINS="$SCRIPT_DIR/../../tools/default_toolchains.json"

cd "$SCRIPT_DIR"

# Clean previous outputs
rm -f build.ninja toolchain.ninja AUTOMINSTALL testapp MathLib.a
rm -rf obj

echo "=== gen-ninja: Running autom --ninja ==="
"$AUTOM" --ninja --toolchains "$TOOLCHAINS" .

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

check_file "build.ninja"
check_file "toolchain.ninja"

# Validate build.ninja content
check_contains "build.ninja" "MathLib"
check_contains "build.ninja" "testapp"
check_contains "build.ninja" "mathlib.cpp"
check_contains "build.ninja" "main.cpp"
check_contains "build.ninja" "utils.cpp"
check_contains "build.ninja" "MathLib.a"
check_contains "build.ninja" "exe"

# Try to actually build if ninja is available
if command -v ninja &>/dev/null; then
    echo "=== gen-ninja: Building with ninja ==="
    ninja
    if [ -f "testapp" ]; then
        echo "OK: testapp binary produced"
        echo "=== gen-ninja: Running testapp ==="
        ./testapp
        echo "OK: testapp exited $?"
    else
        echo "FAIL: testapp binary not produced"
        FAIL=1
    fi
else
    echo "SKIP: ninja not found, skipping build step"
fi

if [ $FAIL -ne 0 ]; then
    echo "=== gen-ninja: FAILED ==="
    exit 1
fi
echo "=== gen-ninja: PASSED ==="
