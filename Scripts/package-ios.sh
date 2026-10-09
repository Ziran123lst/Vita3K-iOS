#!/usr/bin/env bash
# Packages the built app. Device builds are intentionally unsigned and are
# meant to be re-signed locally (AltStore / Sideloadly / codesign) before
# sideloading — see Docs/INSTALLING.md.
#
# Usage: bash Scripts/package-ios.sh [device|simulator]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-device}"

case "$TARGET" in
    device)     APP_DIR="$ROOT/build-device/bin/Vita3K.app";    OUT="$ROOT/dist/Vita3K-iOS-device-unsigned.zip" ;;
    simulator)  APP_DIR="$ROOT/build-simulator/bin/Vita3K.app"; OUT="$ROOT/dist/Vita3K-iOS-simulator.zip" ;;
    *) echo "Usage: $0 [device|simulator]" >&2; exit 1 ;;
esac

if [[ ! -d "$APP_DIR" ]]; then
    echo "error: $APP_DIR not found — run Scripts/build-ios.sh $TARGET first" >&2
    exit 1
fi

mkdir -p "$ROOT/dist"
cd "$(dirname "$APP_DIR")"
rm -f "$OUT"
zip -r -q "$OUT" Vita3K.app
echo "==> Packaged: $OUT"
