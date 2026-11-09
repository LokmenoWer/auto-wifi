#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TARGET="${1:-all}"
mkdir -p dist

build_native() {
    echo "==> Building native host binary..."
    cmake -B build-native -DCMAKE_BUILD_TYPE=Release
    cmake --build build-native -j4
    cp -f build-native/auto-wifi dist/auto-wifi
    echo "Native build complete: dist/auto-wifi"
    ls -lh dist/auto-wifi
}

build_windows() {
    if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        echo "Warning: x86_64-w64-mingw32-g++ not found, skipping Windows cross-compile."
        return 0
    fi
    echo "==> Cross-compiling Windows x64 binary..."
    cmake -B build-win \
        -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build build-win -j4
    cp -f build-win/auto-wifi.exe dist/auto-wifi.exe
    echo "Windows build complete: dist/auto-wifi.exe"
    ls -lh dist/auto-wifi.exe
}

case "$TARGET" in
    native)
        build_native
        ;;
    windows|win)
        build_windows
        ;;
    all|*)
        build_native
        build_windows
        ;;
esac
