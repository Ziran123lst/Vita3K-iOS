#!/usr/bin/env bash
# Clones the pinned upstream Vita3K (with submodules) and overlays the iOS
# port files on top of it.
#
# Usage: bash Scripts/bootstrap.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PINNED_UPSTREAM="b852ad97236a4c217b6c961f9569c4314f77083d"
UPSTREAM_DIR="$ROOT/upstream"

if [[ ! -d "$UPSTREAM_DIR/.git" ]]; then
    echo "==> Cloning Vita3K (pinned at $PINNED_UPSTREAM) with submodules..."
    git clone --recursive https://github.com/Vita3K/Vita3K.git "$UPSTREAM_DIR"
fi

cd "$UPSTREAM_DIR"
echo "==> Checking out pinned upstream commit $PINNED_UPSTREAM"
git fetch --depth 1 origin "$PINNED_UPSTREAM"
git checkout -q "$PINNED_UPSTREAM" || git checkout -q -B ios-port "$PINNED_UPSTREAM"
git submodule update --init --recursive --depth 1

echo "==> Overlaying iOS port files"
cp -R "$ROOT/overlay/." "$UPSTREAM_DIR/"

echo "==> Bootstrap complete"
