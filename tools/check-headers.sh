#!/usr/bin/env bash
# brickOS prototype — public-header self-consistency gate.
#
# A stray `*/` in a comment (common in path globs like core/c_core/src/sched/*.c) closes the comment
# early; a header no .c includes is never compiled, so the error only shows at the first include.
# Criteria: ① each core/c_core/include/br/core/*.h passes -fsyntax-only standalone; ② including all of
# them together, twice (include guards must be idempotent), also passes.
# Usage: bash tools/check-headers.sh [<cc>]; exit 0 pass, 1 not self-consistent, 2 env error.

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

HDR_DIR="core/c_core/include/br/core"
[ -d "$HDR_DIR" ] || { echo "FAIL: cannot find $HDR_DIR" >&2; exit 2; }

status=0
single=0
for h in "$HDR_DIR"/*.h; do
    rel="br/core/$(basename "$h")"
    if ! printf '#include <%s>\n' "$rel" | "$CC" -std=c11 -fsyntax-only -xc - -Icore/c_core/include 2>/tmp/brhdr.err; then
        echo "FAIL standalone include failed: $rel"
        sed 's/^/     | /' /tmp/brhdr.err | head -6
        status=1
    fi
    single=$((single + 1))
done

# ② all at once; the double include must be idempotent (include guards)
{
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    echo 'int br_check_headers_tu;'
} > /tmp/brhdr_all.c
if ! "$CC" -std=c11 -fsyntax-only -Icore/c_core/include /tmp/brhdr_all.c 2>/tmp/brhdr.err; then
    echo "FAIL all-at-once include failed (or the double include is not idempotent)"
    sed 's/^/     | /' /tmp/brhdr.err | head -10
    status=1
fi

if [ "$status" -ne 0 ]; then
    exit 1
fi
echo "ok   $single public headers: standalone include + all-at-once include + double include all pass ($CC)"
exit 0
