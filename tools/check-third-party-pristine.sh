#!/usr/bin/env bash
#
# brickOS prototype — gate for third_party being a **read-only vendor tree** (ADR-0025)
#
# Why it is needed:
#   From this cut on, `third_party/` is a vendor tree of **byte-for-byte upstream
#   originals** (pinned to a u-boot commit), and **every change** we make to
#   third-party code (porting / compat / glue / self-checks) lives under `adapter/`.
#   These two rules used to exist only in documents, so "just tweak one line of
#   third-party code to make it build" was never caught — and that is exactly the
#   mistake made repeatedly in the previous round (adding comments/SPDX inside
#   third-party code, changing includes, deleting trims).
#
# Criteria (five, all mechanical, no compiler/network needed):
#   (1) the lock file is present with complete metadata (repo / branch / commit / snapshot_sha256);
#   (2) the sha256 of **every file** under `third_party/uboot-imx/**` matches the lock
#       <- core: "do not modify third-party code"
#   (3) **every** entry in the lock exists on disk (nothing silently lost; a missing file is red too);
#   (4) there is no `brick.toml` under `third_party/` — third-party code is not a
#       brickOS plugin; our plugins live in adapter/;
#   (5) **no** code outside `third_party/` includes third-party headers — concretely,
#       no `#include <third_party/...>` / `#include "third_party/..."` (absolute-path
#       form) may appear.
#       * Since ADR-0026 the vendored `.c` files **do take part in compilation**
#         (`sources` in each `brick.toml` points directly at them), so the old wording
#         "reference only, not compiled" no longer holds; what this item now guards is
#         "**headers** must not be taken directly by absolute path" — when a vendor
#         header is needed, `adapter/*/compat/` provides a **relative-path forwarding
#         shim** (see `adapter/uboot-erofs/compat/u-boot/lz4.h` and
#         `adapter/uboot-fat/compat/{fat,fs,rtc_def}.h`), so "who references third-party
#         code" is countable at a glance in adapter/, instead of being scattered across
#         the build's `-I` flags.
#       => the other half that goes with it is `tools/check-adapter-copies.py`
#          (no vendor source copies inside adapter).
#
# Usage: bash tools/check-third-party-pristine.sh
# Exit code: 0 = all pass, 1 = violations found, 2 = usage/environment error

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

LOCK="adapter/third-party-upstream.lock"
VENDOR="third_party/uboot-imx"

[ -f "$LOCK" ] || { echo "FAIL: lock file not found: $LOCK" >&2; exit 2; }
[ -d "$VENDOR" ] || { echo "FAIL: vendor tree not found: $VENDOR" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }
pass() { echo "ok   $*"; }

# ---- (1) metadata -----------------------------------------------------------
meta_ok=1
for k in repo branch commit snapshot_sha256; do
    grep -qE "^$k[[:space:]]*=" "$LOCK" || { fail "$LOCK is missing metadata field: $k"; meta_ok=0; }
done
if [ "$meta_ok" = 1 ]; then
    commit="$(grep -E '^commit[[:space:]]*=' "$LOCK" | head -1 | sed 's/.*"\(.*\)".*/\1/')"
    pass "lock metadata complete (commit ${commit:0:12}...)"
fi

# ---- parse [[file]] entries --------------------------------------------------
mapfile -t want < <(
    awk '
        function val(s) { sub(/^[^"]*"/, "", s); sub(/"[^"]*$/, "", s); return s }
        /^\[\[file\]\]/        { path=""; sha=""; next }
        /^path[[:space:]]*=/   { path=val($0); next }
        /^sha256[[:space:]]*=/ { sha=val($0);
                                 if (path != "" && sha != "") print path " " sha; next }
    ' "$LOCK"
)
[ "${#want[@]}" -gt 0 ] || { fail "no [[file]] entries in $LOCK"; exit 1; }

# ---- (2)(3) per-file verification --------------------------------------------
n_ok=0
: > /tmp/.tp_want.$$
for row in "${want[@]}"; do
    p="${row%% *}"; h="${row##* }"
    printf '%s\n' "$p" >> /tmp/.tp_want.$$
    f="$VENDOR/$p"
    if [ ! -f "$f" ]; then
        fail "in the lock but missing on disk: $f"
        continue
    fi
    got="$(sha256sum "$f" | cut -d' ' -f1)"
    if [ "$got" = "$h" ]; then
        n_ok=$((n_ok + 1))
    else
        fail "third-party file was modified: $f"
        echo "       lock $h" >&2
        echo "       got  $got" >&2
        echo "       => third_party/ is a **read-only** vendor tree; to adapt, change the port files under adapter/" >&2
    fi
done
if [ "$status" -eq 0 ]; then
    pass "vendor tree is byte-identical to the lock ($n_ok files)"
fi

# (2b) reverse: no files in the vendor tree that are "not in the lock" (an extra file = something stuffed in)
extra=""
while IFS= read -r f; do
    rel="${f#"$VENDOR"/}"
    grep -qxF "$rel" /tmp/.tp_want.$$ || extra="$extra$rel"$'\n'
done < <(find "$VENDOR" -type f | sort)
rm -f /tmp/.tp_want.$$
if [ -n "$extra" ]; then
    fail "$VENDOR has files that are not in the lock (something was stuffed into the read-only vendor tree)"
    printf '%s' "$extra" | sed 's/^/      /' >&2
else
    pass "vendor tree has no extra files (only the upstream originals registered in the lock)"
fi

# ---- (4) no plugins under third_party ----------------------------------------
if find third_party -name brick.toml -print -quit | grep -q .; then
    fail "brick.toml found under third_party/ — third-party code is not a brickOS plugin (our plugins live in adapter/)"
else
    pass "no brick.toml under third_party/ (all plugins are in adapter/)"
fi

# ---- (5) third-party headers must not be taken by absolute path --------------
hits="$(grep -rn --include='*.c' --include='*.h' -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]third_party/' \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null || true)"
if [ -n "$hits" ]; then
    fail "code outside third_party/ includes third-party headers by absolute path (to use a vendor header, add a relative-path forwarding shim under adapter/*/compat/)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
else
    pass "nobody includes third-party headers by absolute path (vendor headers only via adapter/*/compat/ shims)"
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: third_party is a read-only vendor tree (byte-identical to upstream / no extra files / no plugins / headers only via adapter shims)"
fi
exit "$status"
