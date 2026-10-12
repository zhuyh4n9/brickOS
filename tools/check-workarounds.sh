#!/usr/bin/env bash
#
# brickOS prototype v0.1.0 - WORKAROUND registry consistency check
#
# Why this script:
#   A WORKAROUND is **debt**, and debt's worst case is "forgotten, and nobody knows it is owed".
#   This script binds the two records together - the `WORKAROUND(<id>)` markers in the sources
#   and the registry table in WORKAROUNDS.md. If either side gains or loses an entry, it is red.
#
# Usage: bash tools/check-workarounds.sh   (or make check-workarounds)
# Exit code: 0 = consistent, 1 = inconsistent, 2 = usage/environment error

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

REGISTRY="WORKAROUNDS.md"

if [ ! -f "$REGISTRY" ]; then
    echo "FAIL: cannot find $REGISTRY" >&2
    exit 2
fi

# Markers in the sources: WORKAROUND(<id>) - id looks like br-wa-xxx-000
# Scan the whole tree but exclude build artifacts / VCS metadata / the checker itself / the registry.
mapfile -t src_ids < <(
    grep -rhoE \
        --exclude-dir=build --exclude-dir=.git --exclude-dir=tools \
        --exclude="$REGISTRY" \
        'WORKAROUND\([a-z0-9-]+\)' . 2>/dev/null \
        | sed -E 's/^WORKAROUND\((.*)\)$/\1/' \
        | sort -u
)

# ids in the registry: table rows starting with `| br-wa-xxx-000 |`
mapfile -t reg_ids < <(
    grep -oE '^\|[[:space:]]*br-wa-[a-z0-9-]+' "$REGISTRY" \
        | sed -E 's/^\|[[:space:]]*//' \
        | sort -u
)

status=0

# 1) marker in the sources but not registered
for id in ${src_ids[@]+"${src_ids[@]}"}; do
    if ! printf '%s\n' ${reg_ids[@]+"${reg_ids[@]}"} | grep -qx "$id"; then
        echo "FAIL: source marks $id, but $REGISTRY has no such entry"
        status=1
    fi
done

# 2) registered but no marker in the sources
for id in ${reg_ids[@]+"${reg_ids[@]}"}; do
    if ! printf '%s\n' ${src_ids[@]+"${src_ids[@]}"} | grep -qx "$id"; then
        echo "FAIL: $REGISTRY registers $id, but the sources have no matching marker"
        status=1
    fi
done

echo "source markers ${#src_ids[@]} / registry entries ${#reg_ids[@]}"

if [ "$status" -eq 0 ]; then
    echo "PASS: WORKAROUND registry matches the source markers"
    for id in ${src_ids[@]+"${src_ids[@]}"}; do
        echo "  - $id"
    done
else
    echo "hint: remember to sync $REGISTRY after changing code (adds and removals both count)" >&2
fi

exit "$status"
