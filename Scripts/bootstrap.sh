#!/usr/bin/env bash
# Syncs the latest upstream Vita3K master (with submodules) and overlays the
# iOS port files on top of it. Every build follows upstream master at the
# time it runs — if an upstream change breaks one of Patches/*.patch, the
# apply step (Scripts/apply-patches.sh) fails loudly so the port can be
# re-based.
#
# Usage: bash Scripts/bootstrap.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UPSTREAM_DIR="$ROOT/upstream"

if [[ ! -d "$UPSTREAM_DIR/.git" ]]; then
    echo "==> Cloning Vita3K master with submodules..."
    git clone --recursive https://github.com/Vita3K/Vita3K.git "$UPSTREAM_DIR"
fi

cd "$UPSTREAM_DIR"
echo "==> Fetching latest upstream master"
git fetch --depth 1 origin master
git checkout -q FETCH_HEAD || git checkout -q -B ios-port FETCH_HEAD
git submodule update --init --recursive --depth 1
git log -1 --oneline

echo "==> Overlaying iOS port files"
cp -R "$ROOT/overlay/." "$UPSTREAM_DIR/"

echo "==> Bootstrap complete"
