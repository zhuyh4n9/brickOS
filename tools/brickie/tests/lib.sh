#!/usr/bin/env bash
# Shared skeleton for brickie end-to-end cases (run.sh, purity.sh). Each case runs in its own
# mktemp -d; assertions use only exit codes + structured JSON (jsonq.py) or key text; no compiler
# is called. After source the caller sets TOOL_ROOT REPO_ROOT TESTS_DIR TMPROOT PY CORE GEN.
set -u

# ------------------------------------------------------------------ environment resolution
# Binary lookup order (same as python/brickie/native.py): in-tree build first, then the seed.
pick_first() {
    local p
    for p in "$@"; do
        [ -n "$p" ] && [ -x "$p" ] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

resolve_env() {
    TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
    TOOL_ROOT="${TOOL_ROOT:-$(cd "$TESTS_DIR/.." && pwd)}"
    REPO_ROOT="${REPO_ROOT:-$(cd "$TOOL_ROOT/../.." && pwd)}"
    PY="${PY:-$(command -v python3)}"
    HOST_ARCH="${BRICKIE_HOST_ARCH:-$(bash "$REPO_ROOT/tools/host-detect.sh" --arch)}"
    HOST_OS="${BRICKIE_HOST_OS:-$(bash "$REPO_ROOT/tools/host-detect.sh" --os)}"

    local build_bin="$REPO_ROOT/build/host/$HOST_ARCH/$HOST_OS/bin"
    local seed_bin="$REPO_ROOT/prebuilts/seed/brickie/$HOST_ARCH/$HOST_OS/bin"
    CORE="${BRICKIE_CORE:-$(pick_first "$build_bin/brickie-core" \
        "$REPO_ROOT"/build/host/*/*/bin/brickie-core "$seed_bin/brickie-core" \
        "$REPO_ROOT"/prebuilts/seed/brickie/*/*/bin/brickie-core)}"
    GEN="${BRICKIE_GEN:-$(pick_first "$build_bin/brickie-gen" \
        "$REPO_ROOT"/build/host/*/*/bin/brickie-gen "$seed_bin/brickie-gen" \
        "$REPO_ROOT"/prebuilts/seed/brickie/*/*/bin/brickie-gen)}"
    LAUNCHER="${BRICKIE_LAUNCHER:-$(pick_first "$build_bin/brickie" "$seed_bin/brickie")}"
    SEED_LAUNCHER="${SEED_LAUNCHER:-$(pick_first "$seed_bin/brickie")}"
    export TESTS_DIR TOOL_ROOT REPO_ROOT PY HOST_ARCH HOST_OS CORE GEN LAUNCHER SEED_LAUNCHER
}

PASS=0
FAIL=0
FAILLOG=""

init_report() { FAILLOG="${1:-${TMPROOT:-/tmp}/failures.log}"; : >"$FAILLOG"; }

ok() { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }

bad() {
    FAIL=$((FAIL + 1))
    printf 'FAIL %s\n' "$1"
    [ -n "$FAILLOG" ] || FAILLOG="${TMPROOT:-/tmp}/failures.log"
    { printf 'FAIL %s\n' "$1"; shift || true; for line in "$@"; do printf '     %s\n' "$line"; done; } >>"$FAILLOG"
}

eq() { # eq <name> <expected> <actual>
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected: $2" "actual: $3"; fi
}

# assertion: expected (arg 2) is contained in actual (arg 3)
contains() { # contains <name> <fragment> <full text>
    case "$3" in
        *"$2"*) ok "$1" ;;
        *) bad "$1" "expected to contain: $2" "actual: $(printf '%s' "$3" | head -c 300)" ;;
    esac
}

grep_at_least() { # grep_at_least <name> <pattern> <file>
    local n
    n="$(grep -cE -- "$2" "$3" 2>/dev/null || true)"
    if [ "${n:-0}" -ge 1 ]; then ok "$1"; else bad "$1" "pattern not found in $3: $2"; fi
}

grep_none() { # grep_none <name> <pattern> <file...>
    local label="$1" pat="$2"; shift 2
    local n
    n="$(grep -cE -- "$pat" "$@" 2>/dev/null | awk -F: '{s+=$NF} END{print s+0}')"
    if [ "${n:-0}" -eq 0 ]; then ok "$label"; else bad "$label" "forbidden pattern matched $n times: $pat"; fi
}

new_work() { mktemp -d "$TMPROOT/work.XXXXXX"; }

cp_fx() { # cp_fx <fixture name> [target parent dir]
    local name="$1" parent="${2:-$TMPROOT}"
    local w
    w="$(mktemp -d "$parent/run.XXXXXX")"
    cp -r "$TESTS_DIR/fx/$name/." "$w/"
    printf '%s' "$w"
}

# ------------------------------------------------------------------ running brickie
OUT=""
ERR=""
RC=0

# run_brickie <workdir> [args...]: run the frontend in the workdir; RC/OUT/ERR hold the result.
# Default is the source-tree frontend (python3 -m brickie); BRICKIE_LAUNCHER uses the entry ELF.
run_brickie() {
    local work="$1"; shift
    OUT="$TMPROOT/.last.out"
    ERR="$TMPROOT/.last.err"
    local launcher="${BRICKIE_LAUNCHER:-}"
    if [ -n "$launcher" ]; then
        ( cd "$work" && BRICKIE_CORE="$CORE" BRICKIE_GEN="$GEN" \
            "$launcher" "$@" ) >"$OUT" 2>"$ERR"
    else
        ( cd "$work" && PYTHONPATH="$TOOL_ROOT/python" BRICKIE_CORE="$CORE" \
            BRICKIE_GEN="$GEN" BRICKIE_TOOL_ROOT="$TOOL_ROOT" \
            PYTHONDONTWRITEBYTECODE=1 "$PY" -m brickie "$@" ) >"$OUT" 2>"$ERR"
    fi
    RC=$?
    [ "${VERBOSE:-0}" = "1" ] && printf '  $ brickie %s (rc=%s)\n' "$*" "$RC" >&2
    return 0
}


JSONQ="$TESTS_DIR/jsonq.py"
JQ_RESP=""

# run_json <workdir> [args...]: run once with --json and leave the response in JQ_RESP, so a case
# can use the same grep for both modes; diagnostics are projected to text into ERR.
run_json() {
    local work="$1"; shift
    run_brickie "$work" "$@" --json
    JQ_RESP="$TMPROOT/.last.json"
    cp "$OUT" "$JQ_RESP"
    "$PY" "$JSONQ" text "$JQ_RESP" >"$ERR"
}

jqget() { "$PY" "$JSONQ" get "$@"; }

eqj() { # eqj <name> <response file> <path> <expected>
    eq "$1" "$4" "$("$PY" "$JSONQ" get "$2" "$3")"
}

eqjc() { # eqjc <name> <response file> <path> <count>
    eq "$1" "$4" "$("$PY" "$JSONQ" count "$2" "$3")"
}

hasj() { # hasj <name> <response file> <path>
    eq "$1" "yes" "$("$PY" "$JSONQ" has "$2" "$3")"
}

keysj() { # keysj <name> <response file> <path> <key set (comma separated, sorted)>
    eq "$1" "$4" "$("$PY" "$JSONQ" keys "$2" "$3")"
}

sortedj() { # the diagnostics array is ordered by (code,file,span,target)
    eq "$1" "yes" "$("$PY" "$JSONQ" sorted "$2")"
}

snapj() { # snapj <snapshot name> <response file> — structured snapshot (N-4)
    # BRICKIE_SNAP_EMIT=1 regenerates the baseline instead (only when updating it on purpose).
    local name="$1" resp="$2"
    local detail
    if [ "${BRICKIE_SNAP_EMIT:-0}" = "1" ]; then
        if detail="$("$PY" "$JSONQ" snapshot --emit "$name" "$resp" 2>&1)"; then
            ok "snapshot $name (regenerated)"
        else
            bad "snapshot $name (regeneration failed)" "$detail"
        fi
        return
    fi
    if detail="$("$PY" "$JSONQ" snapshot "$name" "$resp")"; then
        ok "snapshot $name"
    else
        bad "snapshot $name" "$detail"
    fi
}

tree_hash() { # directory content fingerprint (path + sha256), used to assert byte-for-byte idempotence
    ( cd "$1" && find . -type f -not -name out -not -name err | LC_ALL=C sort \
        | xargs -r sha256sum )
}

section() { printf '\n=== %s ===\n' "$1"; }
