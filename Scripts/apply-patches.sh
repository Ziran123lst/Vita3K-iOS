#!/usr/bin/env bash
# Applies the iOS port patches on top of the pristine upstream checkout.
# Patches are ordered and verified; each one must apply cleanly.
#
# Usage: bash Scripts/apply-patches.sh   (run after Scripts/bootstrap.sh)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT/upstream"
for patch in "$ROOT"/Patches/*.patch; do
    echo "==> Applying $(basename "$patch")"
    git apply --check "$patch"
    git apply "$patch"
done

echo "==> All patches applied"
