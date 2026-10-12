#!/usr/bin/env bash
#
# brickOS prototype — the errno **single source of truth** gate (ADR-0023 → ADR-0027)
#
# Why this script is needed:
#   The discipline "internal error codes reuse POSIX numbers" has two corners that break quietly:
#     ① **two tables**: names and values split apart (once: names in the br_errno.h alias table, values in br_error.h);
#     ② **each third-party addon copies its own**: to avoid touching upstream code, ports often write
#        a run of `#define EIO BR_EIO` -- a second source of truth and needless upstream edits (ADR-0023 D-3).
#
# This cut (ADR-0027) converges the convention to the simplest form: **the name IS the original
# POSIX name, and it lives together with its value** -- `br_error.h` has `#define EIO 5` with no
# `BR_` prefix. Hence: bare `-EIO`/`-EFSCORRUPTED` in third-party ports **work as-is** (no mapping
# table needed), and "this repo's errno names" no longer mixes with "this project's symbol prefix".
#
# Three criteria (the first two are fully mechanical and need no compiler present):
#   ① **bare names**: errno definitions in `br_error.h` must be `#define E<name> <value>` (no `BR_E<name>`);
#   ② **one source of truth**: across the whole tree (excluding build/ third_party/) there is **no second**
#      `#define E<name>` besides `br_error.h`. Third-party addons may not ship their own errno table.
#   ③ **value comparison**: delegated to the host test case `ft-test` -- it first captures the host's
#      `<errno.h>` truth, then compares our names (see the "why <errno.h> must be included first" note there).
#
# Usage: bash tools/check-errno.sh   (or make check-errno)
# Exit code: 0 = all pass, 1 = violation present, 2 = usage/environment error

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

ERR_H="core/include/br/core/br_error.h"
[ -f "$ERR_H" ] || { echo "FAIL: cannot find $ERR_H" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }

# ---- truth table: `#define E<name> <value>` (excluding non-errno names like BR_ERR / BR_OK) ----
mapfile -t names < <(
    grep -E '^#define[[:space:]]+E[A-Z0-9_]+[[:space:]]' "$ERR_H" \
        | awk '{print $2}' | sort -u
)
if [ "${#names[@]}" -lt 40 ]; then
    fail "too few bare errno names in $ERR_H (${#names[@]}) -- did it fall back to BR_E* again?"
fi

# ---- ① bare names: no errno definition of the form BR_E<name> may remain ----------------------
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
