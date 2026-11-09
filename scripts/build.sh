#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j4

mkdir -p dist
cp -f build/auto-wifi.exe dist/auto-wifi.exe
echo "Build complete: dist/auto-wifi.exe"
ls -lh dist/auto-wifi.exe
