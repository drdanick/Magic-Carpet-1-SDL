#!/usr/bin/env bash
# Convenience wrapper: build with the VS-bundled CMake + vcpkg (Windows, Git Bash).
# Usage: ./build.sh [build-preset]   (default: win-x86-debug)
set -e
VS="${VS_ROOT:-/c/Program Files/Microsoft Visual Studio/2022/Community}"
export VCPKG_ROOT="${VCPKG_ROOT:-$VS/VC/vcpkg}"
CMAKE="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
PRESET="${1:-win-x86-debug}"
CONFIGURE_PRESET="win-x86"
[[ "$PRESET" == win-x64* ]] && CONFIGURE_PRESET="win-x64-experimental"
[[ "$PRESET" == win-x86-asan ]] && CONFIGURE_PRESET="win-x86-asan"
[ -f "build/$CONFIGURE_PRESET/CMakeCache.txt" ] || "$CMAKE" --preset "$CONFIGURE_PRESET"
"$CMAKE" --build --preset "$PRESET"
