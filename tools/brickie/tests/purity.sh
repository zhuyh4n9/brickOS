#!/usr/bin/env bash
# V-18 glue-layer purity (no business rules in L5) — three **executable** criteria.
#
#   (a) independence: `brickie-core --selftest` is all-green in an environment with no PYTHONPATH
#       and not even python3 on PATH ⇒ domain decisions don't depend on Python packages.
#   (b) stub-substitution proof: replace `brickie-core` with a "canned-JSON stub"; `brickie check`'s
#       **diagnostic set (codes + count + order)** must match the real core exactly ⇒ L5 adds/filters/
#       reorders no business diagnostics (only "shape gate" ones, empty for shape-clean fixtures).
#   (c) static check: `python/brickie/**` must not contain **implementation vehicles** for business
#       rules (see the §denylist), which bans only decision-layer wording, not presentation/IO terms.
#
# Usage: bash tests/purity.sh        (normally pulled in via run.sh; also `make test-purity` alone)
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
. "$TESTS_DIR/lib.sh"
resolve_env

TMPROOT="$(mktemp -d)"
trap 'rm -rf "$TMPROOT"' EXIT
init_report "$TMPROOT/failures.log"

printf '\n=== V-18 glue-layer purity ===\n'

# ------------------------------------------------------------------ (a) selftest without Python
# `env -i` wipes the entire environment (including PYTHONPATH); `PATH=/nonexistent` hides even python3.
SELFTEST_OUT="$TMPROOT/selftest.txt"
env -i PATH=/nonexistent "$CORE" --selftest >"$SELFTEST_OUT" 2>&1
SELFTEST_RC=$?
eq "(a) --selftest empty-env exit code 0" "0" "$SELFTEST_RC"
eq "(a) --selftest all green (ok N cases)" "1" \
    "$(grep -cE '^ok [0-9]+ cases$' "$SELFTEST_OUT" || true)"
eq "(a) --selftest has no failure lines" "0" \
    "$(grep -ciE 'fail|error' "$SELFTEST_OUT" || true)"
eq "(a) python3 is indeed absent from the stripped env" "absent" \
    "$(PATH=/nonexistent command -v python3 >/dev/null 2>&1 && echo found || echo absent)"

# ------------------------------------------------------------------ (b) stub-substitution proof
# Stub: ignore stdin, echo back verbatim "the real core's response for the same fixture".
make_stub() { # make_stub <canned.json> <output path>
    cat >"$2" <<EOF
#!/bin/sh
# Stub: returns fixed JSON only, with no decision logic (never reads the input).
cat >/dev/null
cat "$1"
EOF
    chmod +x "$2"
}

stub_probe() { # stub_probe <fixture name> <label> <path for the real core's diagnostics>
    local fx="$1" label="$2" realout="$3"
    local w
    w="$(cp_fx "$fx")"
    run_json "$w" check
    cp "$JQ_RESP" "$realout"
    local n
    n="$("$PY" "$JSONQ" diagcodes "$realout")"
    [ -n "$n" ] || { bad "(b) $label fixture produced no diagnostics, so the proof is vacuous" "fixture $fx"; return; }

    local canned="$TMPROOT/stub-$label.json" stub="$TMPROOT/stub-$label"
    cp "$realout" "$canned"
    make_stub "$canned" "$stub"

    local saved="$CORE"
    CORE="$stub"
    run_json "$w" check
    CORE="$saved"
    local stubout="$TMPROOT/stub-resp-$label.json"
    cp "$JQ_RESP" "$stubout"

    # Diagnostic set: codes + count + order all byte-aligned
    eq "(b) $label stub substitution: diagnostic sequence matches" \
        "$("$PY" "$JSONQ" diags "$realout")" "$("$PY" "$JSONQ" diags "$stubout")"
    eq "(b) $label stub substitution: count matches" \
        "$("$PY" "$JSONQ" count "$realout" diagnostics)" \
        "$("$PY" "$JSONQ" count "$stubout" diagnostics)"
    eq "(b) $label stub substitution: code set matches" \
        "$("$PY" "$JSONQ" diagcodes "$realout")" "$("$PY" "$JSONQ" diagcodes "$stubout")"
    # Reverse check: the real core's diagnostics are indeed non-empty (else the three above hold trivially)
    eq "(b) $label real core diagnostics non-empty" "no" \
        "$([ "$("$PY" "$JSONQ" count "$realout" diagnostics)" -gt 0 ] && echo no || echo yes)"
}

stub_probe "export-tax16" "tax16" "$TMPROOT/real-tax16.json"
stub_probe "cycle" "cycle" "$TMPROOT/real-cycle.json"
stub_probe "budget" "budget" "$TMPROOT/real-budget.json"

# ------------------------------------------------------------------ (c) static check
# Denylist (**narrow and exact**: bans only what "only the decision layer would write"):
#   1. business BRV codes — L5 may only use shape-class `MF` (BRV-D2's schema gate) and handshake `PROTO`;
#      a DEP/VER/IFACE/TAX/PRIV/GEN code ⇒ the frontend is inventing decision conclusions itself.
#   2. computation libraries — hashlib/hmac (IFACE-IR hash), networkx/graphlib (topology/cycles), z3 (interval solving).
#   3. rule-shaped function names — solve/topo/cycle detection/interval intersection/version advance/canonical hash/taxonomy/privilege/budget.
#      (deliberately not banning `def ...closure`: the presentation function `_render_closure` is allowed.)
#   4. version-segment arithmetic — +-*/ on compat_gen/major/minor/revise.
#   5. range/version parsing primitives — L5 only passes `range` strings through as arguments, never parses them.
#   6. state-machine transition literals — `->`/`=>` transition tables between three/frozen states.
#   7. ownership of machine-file bodies — generated markers and snapshot/lock render function names;
#      path joining for `api/iface/**` also belongs to core (files[] is provided by core).
PY_DIR="$TOOL_ROOT/python/brickie"
purity_pattern_check() { # <name> <ERE> <reason>
    local label="$1" pat="$2" why="$3"
    local hits
    # Scan code only (`*.py`): code names/terms quoted in a README are docs, not decision implementations.
    hits="$(grep -rInE --include='*.py' -- "$pat" "$PY_DIR" 2>/dev/null || true)"
    if [ -z "$hits" ]; then
        ok "(c) $label"
    else
        bad "(c) $label —— $why" "$(printf '%s' "$hits" | head -c 400)"
    fi
}

purity_pattern_check "no business BRV codes (only MF/PROTO allowed)" \
    'BRV-(DEP|VER|IFACE|TAX|PRIV|GEN)-[0-9]{4}' \
    "business codes must be reported by brickie-core (L5 has only shape gate MF and handshake PROTO)"
purity_pattern_check "no hash/graph/solver libraries" \
    '\b(hashlib|hmac|networkx|graphlib|z3|numpy|scipy)\b' \
    "hash computation / topological sort / interval solving are the decision layer's job"
purity_pattern_check "no rule-shaped function names" \
    'def[[:space:]]+[a-zA-Z_]*\b(solve|topo|detect_cycle|find_cycle|intersect|satisf|bump_version|advance_version|canonical_surface|surface_hash|compute_hash|content_hash|classify|privilege_level|budget_sum|change_set|verdict)' \
    "these names correspond to closure/cycle/interval/advance/canonical/classify/privilege/budget decisions"
purity_pattern_check "no version-segment arithmetic" \
    '(compat_gen|major|minor|revise)[[:space:]]*[-+*/]|[-+*/][[:space:]]*(compat_gen|major|minor|revise)' \
    "four-segment advancement may only be computed by core (§5.2)"
purity_pattern_check "no range/version parsing primitives" \
    '(parse_range|range_matches|range_intersect|parse_version|parse_triple|interval_)' \
    "range is passed through as an argument string only; parsing belongs to core (§7.4)"
purity_pattern_check "no state-machine transition literals" \
    '(experimental|frozen|deprecated|unfreezing)[[:space:]]*(->|=>|→)' \
    "the three-state/frozen state machine lives in core (§6.4)"
purity_pattern_check "no ownership of machine-file bodies" \
    '(brickie:generated|render_snapshot|render_lock|api/iface/)' \
    "lock/snapshot/CHANGELOG bodies must come from core (files[]); L5 only writes them to disk"

# Positive control: the six allowed things need implementation vehicles (else "empty denylist" = empty files)
grep_at_least "(c) positive: argparse parsing"     'import argparse' "$PY_DIR/cli.py"
grep_at_least "(c) positive: schema shape check"   'import json'     "$PY_DIR/schema.py"
grep_at_least "(c) positive: subprocess used"      'subprocess'      "$PY_DIR/native.py"
grep_at_least "(c) positive: idempotent write"     'def write_files' "$PY_DIR/writer.py"

# ------------------------------------------------------------------ summary
printf '\n--- purity passed %s / failed %s ---\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf 'purity failure details:\n'
    cat "$FAILLOG"
    exit 1
fi
exit 0
