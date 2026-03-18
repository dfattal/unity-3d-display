#!/bin/bash
# Cross-compile native plugin for Windows using MinGW on macOS.
# This is a COMPILE CHECK ONLY — the output DLL stays in build-win/.
# Use CI artifacts (MSVC-built) for the actual plugin binary.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

rm -rf build-win
mkdir build-win
cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

echo ""
echo "=== MinGW compile check PASSED ==="
echo "NOTE: This DLL is for compile verification only."
echo "      Use CI artifacts (MSVC) for the actual plugin binary."
