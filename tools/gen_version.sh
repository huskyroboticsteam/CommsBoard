#!/bin/sh
# Writes App/config/version.h with the current git hash. CubeIDE runs this before each build.
cd "$(dirname "$0")/.." || exit 0
hash=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
git diff --quiet HEAD 2>/dev/null || hash="$hash-dirty"
new="#define FW_GIT_HASH \"$hash\""
[ "$(cat App/config/version.h 2>/dev/null)" = "$new" ] || echo "$new" > App/config/version.h
exit 0
