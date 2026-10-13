#!/usr/bin/env bash
# brickOS prototype — errno single source of truth gate.
#
# The name IS the POSIX name and lives with its value: br_error.h has `#define EIO 5` with no BR_
# prefix, so bare -EIO/-EFSCORRUPTED in third-party ports work as-is. Criteria: ① definitions in
# br_error.h are `#define E<name> <value>` (no BR_E*); ② no second `#define E<name>` elsewhere in
# the tree (excluding build/, third_party/, tools/); ③ value comparison in the host test ft-test.
# Usage: bash tools/check-errno.sh (or make check-errno); exit 0 pass, 1 violation, 2 env error.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

ERR_H="core/include/br/core/br_error.h"
[ -f "$ERR_H" ] || { echo "FAIL: cannot find $ERR_H" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }

# ---- truth table: `#define E<name> <value>` (excludes non-errno names like BR_ERR / BR_OK) ----
mapfile -t names < <(
    grep -E '^#define[[:space:]]+E[A-Z0-9_]+[[:space:]]' "$ERR_H" \
        | awk '{print $2}' | sort -u
)
if [ "${#names[@]}" -lt 40 ]; then
    fail "too few bare errno names in $ERR_H (${#names[@]}) -- did it fall back to BR_E* again?"
fi

# ---- ① bare names: no BR_E<name> errno definition may remain ----
br_defs="$(grep -cE '^#define[[:space:]]+BR_E[A-Z0-9_]+[[:space:]]+[0-9]' "$ERR_H" || true)"
if [ "${br_defs:-0}" -ne 0 ]; then
    fail "$ERR_H still has $br_defs BR_E-prefixed errno definitions -- errno names may not carry a BR_ prefix"
    grep -nE '^#define[[:space:]]+BR_E[A-Z0-9_]+[[:space:]]+[0-9]' "$ERR_H" | head -5 | sed 's/^/      /' >&2
fi

# ---- ② one source of truth (third-party addons / anywhere else may not define the same name) --
alt="$(printf '%s\n' "${names[@]}" | paste -sd'|' -)"
hits="$(grep -rnE --include='*.c' --include='*.h' \
            "^#define[[:space:]]+(${alt})[[:space:]]" \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null \
        | grep -v "^\./$ERR_H:" || true)"
if [ -n "$hits" ]; then
    fail "errno names are defined somewhere other than $ERR_H (second source of truth)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
fi

if [ "$status" -eq 0 ]; then
    echo "ok   errno table: ${#names[@]} bare POSIX names (no BR_ prefix / single source of truth)"
    echo "PASS: errno single source of truth (bare names + one definition); value comparison in host test ft-test"
fi
exit "$status"
