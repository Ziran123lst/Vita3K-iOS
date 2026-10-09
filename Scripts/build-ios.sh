#!/usr/bin/env bash
# Cross-compiles Vita3K for iOS using the macOS clang toolchain.
#
# Usage: bash Scripts/build-ios.sh [device|simulator]
#
# device     - ARM64 iPhone/iPad build (requires macOS + Xcode command line tools)
# simulator  - ARM64 iOS Simulator build (faster to iterate)
#
# Output goes to build-<target>/bin/Vita3K.app
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-device}"

case "$TARGET" in
    device)     SDK="iphoneos";        ARCH="arm64"; BUILD_DIR="$ROOT/build-device" ;;
    simulator)  SDK="iphonesimulator"; ARCH="arm64"; BUILD_DIR="$ROOT/build-simulator" ;;
    *) echo "Usage: $0 [device|simulator]" >&2; exit 1 ;;
esac

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cd "$ROOT/upstream"

echo "==> Configuring for iOS ($SDK / $ARCH) in $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_DISCORD_RICH_PRESENCE=OFF \
    -DARCHITECTURE="$ARCH"

echo "==> Building (jobs=$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "==> Build finished: $BUILD_DIR/bin/Vita3K.app"
