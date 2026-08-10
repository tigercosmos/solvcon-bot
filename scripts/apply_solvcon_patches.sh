#!/usr/bin/env bash
#
# Apply local solvcon JSON-parser patches to the third_party/solvcon
# submodule's working tree. See issue.md for what each patch fixes.
#
# Idempotent: skips a patch that's already applied. Exits non-zero on
# a real apply failure.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUBMODULE_DIR="$REPO_ROOT/third_party/solvcon"
PATCH_DIR="$REPO_ROOT/patches"

if [[ ! -d "$SUBMODULE_DIR" ]]; then
    echo "fatal: $SUBMODULE_DIR not found" >&2
    echo "  run: git submodule update --init" >&2
    exit 1
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
if [[ ${#patches[@]} -eq 0 ]]; then
    echo "no patches in $PATCH_DIR — nothing to do"
    exit 0
fi

for p in "${patches[@]}"; do
    base=$(basename "$p")
    # If the patch is already applied (--reverse --check succeeds),
    # skip it. Otherwise apply.
    if (cd "$SUBMODULE_DIR" && git apply --reverse --check "$p" >/dev/null 2>&1); then
        echo "==> $base already applied, skipping"
        continue
    fi
    echo "==> applying $base"
    (cd "$SUBMODULE_DIR" && git apply "$p")
done

echo "==> done."
