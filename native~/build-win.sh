#!/bin/bash
# Cross-compile native plugin for Windows using MinGW on macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

rm -rf build-win
mkdir build-win
cd build-win
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

echo ""
echo "=== Build complete ==="
ls -la "$SCRIPT_DIR/../Runtime/Plugins/Windows/x64/displayxr_unity.dll"
