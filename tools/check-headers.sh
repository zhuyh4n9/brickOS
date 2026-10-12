#!/usr/bin/env bash
#
# brickOS prototype — the **self-consistency** gate for public headers
#
# Why this gate is worth having:
#   A stray `*/` in a comment **closes the comment early**. The most common source is path globs --
#   `core/src/sched/*.c`, `platform/*/src/link.ld`, `core/src/sync/**` all contain `*/`.
#   This trap has been hit three times in this repo (TC-MEM-*/TC-MM-*, `core/src/sched/*.c`, `platform/*/src/...`).
#   The illusion that it "compiles" comes from: **a header no .c includes is never compiled at all** --
#   so the error only explodes when the first user appears, and by then the scene is far from the cause.
#
# Criteria (two, both mechanical):
#   ① every `core/include/br/core/*.h` can be included **standalone** and pass `-fsyntax-only`
#      (only a standalone include exposes "missing prerequisite include");
#   ② including **all** headers **together** also passes (exposes macro/type clashes).
#
# Usage: bash tools/check-headers.sh [<cc>]
# Exit code: 0 = all pass, 1 = some header is not self-consistent, 2 = environment error

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

CC="${1:-}"
if [ -z "$CC" ]; then
    for c in aarch64-linux-gnu-gcc aarch64-linux-gnu-gcc-16 aarch64-linux-gnu-gcc-15 cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC="$c"; break; fi
    done
fi
if [ -z "$CC" ]; then
    echo "FAIL: cannot find any C compiler (cross or host)" >&2
    exit 2
fi

HDR_DIR="core/include/br/core"
[ -d "$HDR_DIR" ] || { echo "FAIL: cannot find $HDR_DIR" >&2; exit 2; }

status=0
single=0
for h in "$HDR_DIR"/*.h; do
    rel="br/core/$(basename "$h")"
    if ! printf '#include <%s>\n' "$rel" | "$CC" -std=c11 -fsyntax-only -xc - -Icore/include 2>/tmp/brhdr.err; then
        echo "FAIL standalone include failed: $rel"
        sed 's/^/     | /' /tmp/brhdr.err | head -6
        status=1
    fi
    single=$((single + 1))
done

# ② all at once (including the same header twice must be idempotent -- the point of include guards)
{
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    echo 'int br_check_headers_tu;'
} > /tmp/brhdr_all.c
if ! "$CC" -std=c11 -fsyntax-only -Icore/include /tmp/brhdr_all.c 2>/tmp/brhdr.err; then
    echo "FAIL all-at-once include failed (or the double include is not idempotent)"
    sed 's/^/     | /' /tmp/brhdr.err | head -10
    status=1
fi

if [ "$status" -ne 0 ]; then
    exit 1
fi
echo "ok   $single public headers: standalone include + all-at-once include + double include all pass ($CC)"
exit 0
