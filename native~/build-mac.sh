#!/bin/bash
# Build native plugin for macOS (Universal: x86_64 + arm64)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

rm -rf build
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build . --config Release

echo ""
echo "=== Build complete ==="
ls -la "$SCRIPT_DIR/../Runtime/Plugins/macOS/displayxr_unity.bundle"
