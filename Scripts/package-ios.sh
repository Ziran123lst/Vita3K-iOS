#!/usr/bin/env bash
# Packages the built app. Device builds are intentionally unsigned and are
# meant to be re-signed locally (AltStore / Sideloadly / codesign) before
# sideloading — see Docs/INSTALLING.md.
#
# Device builds additionally produce a .ipa (Payload/Vita3K.app) so the build
# can be published directly to a GitHub Release.
#
# Usage: bash Scripts/package-ios.sh [device|simulator]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-device}"

case "$TARGET" in
    device)     APP_DIR="$ROOT/build-device/bin/Vita3K.app"; OUT_ZIP="$ROOT/dist/Vita3K-iOS-device-unsigned.zip"; OUT_IPA="$ROOT/dist/Vita3K-iOS-device.ipa" ;;
    simulator)  APP_DIR="$ROOT/build-simulator/bin/Vita3K.app"; OUT_ZIP="$ROOT/dist/Vita3K-iOS-simulator.zip"; OUT_IPA="" ;;
    *) echo "Usage: $0 [device|simulator]" >&2; exit 1 ;;
esac

if [[ ! -d "$APP_DIR" ]]; then
    echo "error: $APP_DIR not found — run Scripts/build-ios.sh $TARGET first" >&2
    exit 1
fi

mkdir -p "$ROOT/dist"
cd "$(dirname "$APP_DIR")"
rm -f "$OUT_ZIP"
zip -r -q "$OUT_ZIP" Vita3K.app
echo "==> Packaged: $OUT_ZIP"

if [[ -n "$OUT_IPA" ]]; then
    IPA_STAGE="$ROOT/dist/ipa-stage"
    rm -rf "$IPA_STAGE"
    mkdir -p "$IPA_STAGE/Payload"
    cp -R "$APP_DIR" "$IPA_STAGE/Payload/"
    rm -f "$OUT_IPA"
    (cd "$IPA_STAGE" && zip -r -q "$OUT_IPA" Payload)
    rm -rf "$IPA_STAGE"
    echo "==> Packaged IPA: $OUT_IPA"
fi
