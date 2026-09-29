#!/bin/sh
# Put WAMR where the build expects it (reference/wasm-micro-runtime, git-ignored): the pinned commit
# plus this project's patch (tools/patches/README.md). Idempotent: a tree already at the commit with
# the patch applied (a developer checkout) is left alone.
set -eu
WAMR_URL=https://github.com/bytecodealliance/wasm-micro-runtime.git
WAMR_COMMIT=5ee03eaf6b7bf93781a9e7717b66dc81d9eefa9f
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D="$ROOT/reference/wasm-micro-runtime"
PATCH="$ROOT/tools/patches/wamr-nucleov2.patch"

if [ ! -d "$D/.git" ]; then
    mkdir -p "$ROOT/reference"
    git clone --quiet --filter=blob:none --no-checkout "$WAMR_URL" "$D"
fi
if [ "$(git -C "$D" rev-parse HEAD 2>/dev/null || true)" != "$WAMR_COMMIT" ]; then
    git -C "$D" fetch --quiet --depth 1 origin "$WAMR_COMMIT" 2>/dev/null || true
    git -C "$D" checkout --quiet "$WAMR_COMMIT"
fi
# Compare diffs rather than `apply --reverse --check`: on a Windows checkout the work tree has CRLF
# and the reverse check fails even though the patch is in.
if git -C "$D" diff --quiet; then
    git -C "$D" apply "$PATCH"
    echo "WAMR $WAMR_COMMIT: patch applied"
elif [ "$(git -C "$D" diff | tr -d '\r' | grep -v '^index ')" = "$(tr -d '\r' < "$PATCH" | grep -v '^index ')" ]; then
    # (the `index` lines differ only in hash abbreviation length, which depends on the clone)
    echo "WAMR $WAMR_COMMIT: patch already applied"
else
    echo "WAMR tree has local changes that differ from $PATCH: refresh the patch or reset the tree" >&2
    exit 1
fi
