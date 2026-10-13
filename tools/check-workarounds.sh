#!/usr/bin/env bash
# brickOS prototype — WORKAROUND registry consistency check: source WORKAROUND markers and the
# registry table in WORKAROUNDS.md record the same debt, so a missing entry on either side is red.
# Usage: bash tools/check-workarounds.sh (or make check-workarounds); exit 0 ok, 1 mismatch.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

REGISTRY="WORKAROUNDS.md"

if [ ! -f "$REGISTRY" ]; then
    echo "FAIL: cannot find $REGISTRY" >&2
    exit 2
fi

# Source markers WORKAROUND(<id>) (build/, .git/, tools/ and the registry excluded) vs. registry
# table rows `| br-wa-xxx-000 |`.
mapfile -t src_ids < <(
    grep -rhoE \
        --exclude-dir=build --exclude-dir=.git --exclude-dir=tools \
        --exclude="$REGISTRY" \
        'WORKAROUND\([a-z0-9-]+\)' . 2>/dev/null \
        | sed -E 's/^WORKAROUND\((.*)\)$/\1/' \
        | sort -u
)

mapfile -t reg_ids < <(
    grep -oE '^\|[[:space:]]*br-wa-[a-z0-9-]+' "$REGISTRY" \
        | sed -E 's/^\|[[:space:]]*//' \
        | sort -u
)

status=0

# 1) source marker without a registry entry;  2) registry entry without a source marker
for id in ${src_ids[@]+"${src_ids[@]}"}; do
    if ! printf '%s\n' ${reg_ids[@]+"${reg_ids[@]}"} | grep -qx "$id"; then
        echo "FAIL: source marks $id, but $REGISTRY has no such entry"
        status=1
    fi
done

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
