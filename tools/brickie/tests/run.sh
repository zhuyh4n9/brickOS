#!/usr/bin/env bash
# brickie v0.1 end-to-end acceptance regression baseline (V-1…V-21, negative N-1…N-4). Exit code
# 0 success / 1 validation red / 2 usage or environment error; --json serializes `data`, diagnostics
# sorted by (code,file,span,target). Every case copies its fixture into a fresh mktemp dir, no case
# calls a compiler, assertions are never loosened. Usage: bash tests/run.sh [-v] (via make test).
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
. "$TESTS_DIR/lib.sh"
resolve_env

TMPROOT="$(mktemp -d)"
trap 'rm -rf "$TMPROOT"' EXIT
init_report "$TMPROOT/failures.log"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

ENVELOPE_KEYS="command,data,diagnostics,exit_code,protocol,status,written"

printf '=== brickie v0.1 end-to-end acceptance (V-1…V-21 + N-1…N-4) ===\n'
printf 'host triple: %s/%s\n' "$HOST_ARCH" "$HOST_OS"
printf 'core: %s\n' "$CORE"
printf 'gen : %s\n' "$GEN"

section "0. environment and binaries"
eq "core is executable" "yes" "$([ -x "$CORE" ] && echo yes || echo no)"
eq "gen is executable" "yes" "$([ -x "$GEN" ] && echo yes || echo no)"
W="$(new_work)"
run_brickie "$W" --version
eq "--version exit code 0" "0" "$RC"
grep_at_least "--version prints the version" '^brickie 0\.1\.0$' "$OUT"
run_json "$W" --version
eqj "--version --json protocol version" "$JQ_RESP" "protocol" "1"
eqj "--version --json version" "$JQ_RESP" "data.version" "0.1.0"
eqj "--version --json language fingerprint includes rust" "$JQ_RESP" "data.languages.0.name" "rust"
run_json "$W" --version --deps
eqjc "--version --deps --json three direct deps" "$JQ_RESP" "data.deps" "3"
eq "core --selftest runs directly" "0" "$("$CORE" --selftest >/dev/null 2>&1; echo $?)"

# ---- V-1 skeleton generation
section "V-1 new for all five kinds × c, immediate check has 0 errors"
for pair in "app:app/hsm:" "interface:iface/posix:" "ability:provider/crypto:--subkind service" "runtime:runtime/qemu-aarch64:" "core:core/c_core:";
do
    ptype="${pair%%:*}"; rest="${pair#*:}"; name="${rest%%:*}"; extra="${rest#*:}"
    W="$(new_work)"
    # shellcheck disable=SC2086
    run_brickie "$W" new "$ptype" "$name" $extra
    eq "V-1 new $ptype exit code 0" "0" "$RC"
    short="${name##*/}"
    n=0
    for f in "$name/brick.toml" "$name/src/$short.c" "$name/include/$short/$short.h" \
             "$name/tests/smoke.toml" "$name/README.md" "build/gen/$name/plugin_desc.c"; do
        [ -f "$W/$f" ] && n=$((n + 1))
    done
    eq "V-1 new $ptype all six artifacts present" "6" "$n"
    grep_at_least "V-1 new $ptype descriptor carries the generated marker" 'brickie:generated' \
        "$W/build/gen/$name/plugin_desc.c"
    run_json "$W" check
    eq "V-1 new $ptype immediate check exit code 0" "0" "$RC"
    eqj "V-1 new $ptype immediate check errors=0" "$JQ_RESP" "data.summary.errors" "0"
    eqj "V-1 new $ptype hand-written files untouched (git semantics: nothing written)" "$JQ_RESP" "written" "[]"
done
W="$(new_work)"
run_brickie "$W" new app app/hsm
run_brickie "$W" new interface iface/posix
run_brickie "$W" new ability provider/crypto --subkind service
run_brickie "$W" new runtime runtime/qemu-aarch64
run_brickie "$W" new core core/c_core
run_json "$W" check
eq "V-1 five kinds one tree check exit code 0" "0" "$RC"
eqj "V-1 five kinds one tree closure has 5 plugins" "$JQ_RESP" "data.summary.closure.plugins" "5"

# ---- V-2 generated artifacts idempotent
section "V-2 gen idempotence / --check byte-identical"
W="$(new_work)"
run_brickie "$W" new ability provider/crypto --subkind service
H1="$(tree_hash "$W")"
run_brickie "$W" gen
eq "V-2 second gen exit code 0" "0" "$RC"
grep_at_least "V-2 second gen reports 'unchanged:'" 'unchanged:' "$OUT"
H2="$(tree_hash "$W")"
eq "V-2 two gen runs byte-identical" "$H1" "$H2"
run_brickie "$W" gen --check
eq "V-2 gen --check consistent exit code 0" "0" "$RC"
grep_at_least "V-2 gen --check text" 'byte-identical to recomputation' "$OUT"
printf '\n/* hand edit */\n' >>"$W/build/gen/provider/crypto/plugin_desc.c"
run_brickie "$W" gen --check
eq "V-2 tampered gen --check exit code 1" "1" "$RC"
run_json "$W" gen --check
eqj "V-2 tampered --json exit_code=1" "$JQ_RESP" "exit_code" "1"
eq "V-2 tampered --json code-less diagnostic (contract §2 allows it)" "null" \
    "$(jqget "$JQ_RESP" diagnostics.0.code)"
run_brickie "$W" gen
eq "V-2 gen repair exit code 0" "0" "$RC"
run_brickie "$W" gen --check
eq "V-2 after repair --check exit code 0" "0" "$RC"
# Stale descriptors after a plugin rename/delete: `gen --check` must go red first, and `gen` must
# delete them -- a leftover descriptor in the `[build].gen_sources` glob becomes a ghost plugin
# (after the uboot-mmc rename both descriptors pointed at one start symbol, so the second start
# returned -ENOSPC and boot halted under G6).
mkdir -p "$W/build/gen/third_party/uboot-mmc"
printf '/* brickie:generated */\nint ghost;\n' >"$W/build/gen/third_party/uboot-mmc/plugin_desc.c"
run_brickie "$W" gen --check
eq "V-2 stale descriptor: gen --check exit code 1" "1" "$RC"
grep_at_least "V-2 stale descriptor: --check names 'belongs to no plugin'" 'belongs to no plugin' "$ERR"
run_brickie "$W" gen
eq "V-2 stale descriptor: after gen cleanup exit code 0" "0" "$RC"
eq "V-2 stale descriptor: directory is gone" "gone" \
    "$([ -e "$W/build/gen/third_party" ] && echo here || echo gone)"
grep_at_least "V-2 stale descriptor: gen leaves a trace" 'stale generated files' "$OUT"
run_brickie "$W" gen --check
eq "V-2 stale descriptor: after cleanup --check exit code 0" "0" "$RC"

# ---- V-3 cycle detection
section "V-3 cycle ⇒ full cycle path + exit 1"
W="$(cp_fx cycle)"
run_json "$W" check
eq "V-3 check on a cycle exit code 1" "1" "$RC"
eq "V-3 cycle diagnostic code BRV-MF-0001 (R-9 gap stand-in)" "1" \
    "$(grep -c 'BRV-MF-0001' "$ERR" || true)"
grep_at_least "V-3 text reports the full cycle path" 'provider/a → provider/b → provider/c → provider/a' "$ERR"
eqj "V-3 check --json contains the cycle diagnostic" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
run_json "$W" dep closure
eqj "V-3 closure --json exit_code=1" "$JQ_RESP" "exit_code" "1"
eqjc "V-3 closure --json cycles count" "$JQ_RESP" "data.cycles" "1"
eqj "V-3 cycles[0].path complete (first == last)" "$JQ_RESP" "data.cycles.0.path" \
    '["provider/a", "provider/b", "provider/c", "provider/a"]'
eqjc "V-3 cycles[0].edges structured edge list has 3 entries" "$JQ_RESP" "data.cycles.0.edges" "3"
eqj "V-3 cycles[0].edges[0] shape {from,to}" "$JQ_RESP" "data.cycles.0.edges.0.from" "provider/a"
eqj "V-3 cycles[0].edges[0].to" "$JQ_RESP" "data.cycles.0.edges.0.to" "provider/b"

# ---- V-4 version bump table
section "V-4 event → segment bump table, row by row"
W="$(cp_fx ver-matrix)"
printf 'RFC: unfreeze decision record\n' >"$W/rfc.md"
segments_of() { # segments_of <work> <id> ⇒ "cg.major.minor.revise"
    run_json "$1" ver show "$2"
    printf '%s.%s.%s.%s' \
        "$(jqget "$JQ_RESP" data.segments.compat_gen)" \
        "$(jqget "$JQ_RESP" data.segments.major)" \
        "$(jqget "$JQ_RESP" data.segments.minor)" \
        "$(jqget "$JQ_RESP" data.segments.revise)"
}
run_brickie "$W" iface publish provider/crypto#crypto
eq "V-4 first publish = file creation, no segment bump (S-8)" "0.1.0.0" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" iface unfreeze provider/crypto#crypto --note rfc.md
eq "V-4(f) unfreeze enters the window" "0" "$RC"
run_brickie "$W" iface refreeze provider/crypto#crypto
eq "V-4(f) empty-unfreeze refreeze exit code 0" "0" "$RC"
eq "V-4(f) empty unfreeze ⇒ all four segments stay put" "0.1.0.0" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" iface unfreeze provider/crypto#crypto --note rfc.md
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/provider/crypto/brick.toml"
run_brickie "$W" iface refreeze provider/crypto#crypto --note rfc.md
eq "V-4(a) refreeze exit code 0" "0" "$RC"
eq "V-4(a) edited frozen entry ⇒ only COMPAT_GEN+1 (MAJOR not dragged in)" "1.1.0.0" \
    "$(segments_of "$W" provider/crypto#crypto)"
python3 - "$W/provider/crypto/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                        'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1)
p.write_text(s)
PY
run_brickie "$W" iface publish provider/crypto#crypto
eq "V-4(b) added entry publish exit code 0" "0" "$RC"
eq "V-4(b) addition ⇒ COMPAT_GEN stays put + MINOR+1" "1.1.1.0" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" ver bump provider/crypto#crypto --rule major
eq "V-4(c) ver bump major exit code 0" "0" "$RC"
eq "V-4(c) only MAJOR+1, COMPAT_GEN stays put" "1.2.0.0" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" ver bump provider/crypto#crypto --rule minor
eq "V-4(d) minor ⇒ MINOR+1" "1.2.1.0" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" ver bump provider/crypto#crypto --rule revise
eq "V-4(e) revise ⇒ REVISE+1" "1.2.1.1" "$(segments_of "$W" provider/crypto#crypto)"
run_brickie "$W" ver bump provider/crypto#crypto --rule compat_gen
eq "V-4 COMPAT_GEN is not a rule (usage error 2)" "2" "$RC"

# ---- V-5 interface publishing
section "V-5 publish output + idempotence + --check independent recompute"
W="$(cp_fx publish)"
run_json "$W" iface publish provider/a#a
eq "V-5 first publish exit code 0" "0" "$RC"
eqj "V-5 first publish noop=false" "$JQ_RESP" "data.noop" "false"
eqjc "V-5 3 files written (snapshot+CHANGELOG+lock)" "$JQ_RESP" "written" "3"
for f in "api/iface/provider/a/a.toml" "api/iface/CHANGELOG.md" "brickie.lock"; do
    eq "V-5 artifact in place $f" "yes" "$([ -f "$W/$f" ] && echo yes || echo no)"
done
grep_at_least "V-5 snapshot header carries NOT_ABI" 'NOT_ABI' "$W/api/iface/provider/a/a.toml"
grep_at_least "V-5 snapshot header carries hash_scope" 'hash_scope = "decl"' "$W/api/iface/provider/a/a.toml"
grep_at_least "V-5 snapshot header carries truth" 'truth = "decl"' "$W/api/iface/provider/a/a.toml"
grep_at_least "V-5 snapshot carries the brickie:generated marker" 'brickie:generated' "$W/api/iface/provider/a/a.toml"
grep_at_least "V-5 CHANGELOG keyed by version segment, no dates" '^## provider/a#a v0\.1\.0\.0' "$W/api/iface/CHANGELOG.md"
eq "V-5 CHANGELOG carries no dates (byte-reproducible)" "0" \
    "$(grep -cE '[0-9]{4}-[0-9]{2}-[0-9]{2}' "$W/api/iface/CHANGELOG.md" || true)"
grep_at_least "V-5 lock carries the brickie:generated marker" 'brickie:generated' "$W/brickie.lock"
H1="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
run_json "$W" iface publish provider/a#a
eq "V-5 surface unchanged ⇒ no-op exit code 0" "0" "$RC"
eqj "V-5 surface unchanged ⇒ noop=true" "$JQ_RESP" "data.noop" "true"
eqjc "V-5 surface unchanged ⇒ writes no files" "$JQ_RESP" "written" "0"
H2="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
eq "V-5 no-op is byte-identical" "$H1" "$H2"
run_json "$W" iface status provider/a#a --check
eq "V-5 status --check consistent exit code 0" "0" "$RC"
eqj "V-5 status --check consistent=true" "$JQ_RESP" "data.consistent" "true"
run_json "$W" iface publish provider/a#a --check
eq "V-5 publish --check exit code 0" "0" "$RC"
eqjc "V-5 publish --check writes nothing" "$JQ_RESP" "written" "0"
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000"/' \
    "$W/api/iface/provider/a/a.toml"
run_json "$W" iface status provider/a#a --check
eq "V-5 hand-edited snapshot ⇒ status --check exit code 1" "1" "$RC"
eqj "V-5 hand-edited snapshot ⇒ consistent=false" "$JQ_RESP" "data.consistent" "false"

# ---- V-6 impact report
section "V-6 impact report for a frozen-entry edit / --note hard gate"
W="$(cp_fx dependents)"
printf 'RFC: signature change\n' >"$W/rfc.md"
run_brickie "$W" iface publish provider/crypto#crypto
eq "V-6 file-creation publish exit code 0" "0" "$RC"
run_brickie "$W" iface unfreeze provider/crypto#crypto --note rfc.md
eq "V-6 unfreeze exit code 0" "0" "$RC"
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/provider/crypto/brick.toml"
run_json "$W" iface diff provider/crypto#crypto
eq "V-6 diff exit code 0" "0" "$RC"
eqj "V-6 changes[0].kind=CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-6 change set takes the strictest: compat_gen_changed=true" "$JQ_RESP" "data.compat_gen_changed" "true"
eqjc "V-6 2 direct dependents" "$JQ_RESP" "data.dependents.direct" "2"
eqjc "V-6 3 transitive dependents" "$JQ_RESP" "data.dependents.transitive" "3"
eqjc "V-6 1 unsatisfied dependent" "$JQ_RESP" "data.dependents.unsatisfied" "1"
eqj "V-6 unsatisfied dependent named + reason" "$JQ_RESP" "data.dependents.unsatisfied.0.plugin" "provider/consumer"
grep_at_least "V-6 mismatch reason explains the compat_gen change" 'compat_gen changed from 0 to 1' "$OUT"
run_json "$W" iface publish provider/crypto#crypto
eq "V-6 requires unfreeze but --note is missing ⇒ exit code 1" "1" "$RC"
grep_at_least "V-6 missing --note reports BRV-MF-0001" 'BRV-MF-0001' "$ERR"
run_json "$W" iface publish provider/crypto#crypto --note rfc.md
eq "V-6 publish with --note exit code 0" "0" "$RC"
eqj "V-6 COMPAT_GEN bumped after publish" "$JQ_RESP" "data.compat_gen" "1"
run_brickie "$W" iface refreeze provider/crypto#crypto
eq "V-6 publish in window consumed the baseline ⇒ refreeze is an empty unfreeze" "0" "$RC"
# Use unfreeze here: an unchanged-surface publish is a no-op and skips the --note gate.
run_brickie "$W" iface unfreeze provider/crypto#crypto --note no-such.md
eq "V-6 --note points at a missing path ⇒ usage error 2" "2" "$RC"

# ---- V-7 dependency solving
section "V-7 correct closure / VER-0001 / VER-0002 / budget totals"
W="$(cp_fx ver-conflict)"
run_json "$W" check
eq "V-7 compat_gen conflict ⇒ exit code 1" "1" "$RC"
eq "V-7 reports BRV-VER-0001" "1" "$(grep -c 'BRV-VER-0001' "$ERR" || true)"
grep_at_least "V-7 conflict reports who pinned which generation / which generation the provider is at" 'compat_gen=3, but the provider is currently at 4' "$ERR"
W="$(cp_fx ver-range)"
run_json "$W" check
eq "V-7 same-generation range out of bounds ⇒ exit code 1" "1" "$RC"
eq "V-7 reports BRV-VER-0002" "1" "$(grep -c 'BRV-VER-0002' "$ERR" || true)"
W="$(cp_fx showcase)"
run_json "$W" dep closure
eq "V-7 closure exit code 0" "0" "$RC"
eqj "V-7 closure plugin list" "$JQ_RESP" "data.selected" \
    '["app/hsm", "core/c_core", "iface/hsm-api", "provider/crypto", "runtime/qemu-aarch64", "sched/coop"]'
# Only the "provider first" invariant is required; nodes without init edges may sit anywhere.
init_order_ok() {
    python3 - "$1" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
pos = {n: i for i, n in enumerate(d.get("topo_order", []))}
bad = [(e.get("from"), e.get("to")) for e in d.get("init_edges", [])
       if not pos.get(e.get("to"), 10 ** 9) < pos.get(e.get("from"), -1)]
missing = [n for n in d.get("selected", []) if n not in pos]
print("yes" if not bad and not missing else f"no:{bad}{missing}")
PYX
}
eq "V-7 topological order puts the provider first for every init edge" "yes" "$(init_order_ok "$JQ_RESP")"
eqj "V-7 total RAM (runtime capacity not in Σ, S-14)" "$JQ_RESP" "data.totals.ram_kib" "48"
eqj "V-7 total stack" "$JQ_RESP" "data.totals.stack_kib" "4"
eqj "V-7 runtime capacity reported separately" "$JQ_RESP" "data.totals.runtime_capacity.ram_kib" "8192"
eqj "V-7 budget" "$JQ_RESP" "data.totals.budget.ram_kib" "512"

# ---- V-8 taxonomy and phase
section "V-8 three hard prohibitions, positive and negative cases / phase monotonicity"
for fx in tax-01-app-ability tax-02-native-adapter tax-03-iface-leaf; do
    W="$(cp_fx "$fx")"
    run_json "$W" check
    eq "V-8 $fx negative case exit code 1" "1" "$RC"
    eqj "V-8 $fx reports BRV-MF-0001 (prohibition has no dedicated code, R-9)" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
    grep_at_least "V-8 $fx message names both conflicting parties" 'consumer `' "$ERR"
done
W="$(cp_fx tax-ok)"
run_json "$W" check
eq "V-8 positive case app→interface→ability exit code 0" "0" "$RC"
eqj "V-8 positive case errors=0" "$JQ_RESP" "data.summary.errors" "0"
W="$(cp_fx phase-bad)"
run_json "$W" check
eq "V-8 R1 negative case exit code 1" "1" "$RC"
eq "V-8 R1 reports BRV-DEP-0009" "1" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
W="$(cp_fx phase-assert)"
run_json "$W" check
eq "V-8 R2 negative case exit code 1" "1" "$RC"
eq "V-8 R2 reports BRV-DEP-0010" "1" "$(grep -c 'BRV-DEP-0010' "$ERR" || true)"
eq "V-8 R2 does not report DEP-0009" "0" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
W="$(cp_fx phase-ok)"
run_json "$W" check
eq "V-8 sched-coop → runtime is not a false positive, exit code 0" "0" "$RC"
eq "V-8 no false positive: no DEP-0009" "0" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
eq "V-8 no false positive: no DEP-0010" "0" "$(grep -c 'BRV-DEP-0010' "$ERR" || true)"

# ---- V-9 zero build dependencies
section "V-9 zero build dependencies (minimal PATH + single-file bootstrap seed entry)"
MINBIN="$TMPROOT/minpath"
mkdir -p "$MINBIN"
for t in python3 sh env cat cp mkdir rm mv ls cmp sort head tail wc sed grep find \
         mktemp dirname basename; do
    src="$(command -v "$t" 2>/dev/null || true)"
    [ -n "$src" ] && ln -sf "$src" "$MINBIN/$t"
done
eq "V-9 minimal PATH has no cc/g++/cargo/nm" "0" \
    "$(ls "$MINBIN" | grep -cE '^(cc|gcc|g\+\+|c\+\+|cargo|nm|ld)$' || true)"
eq "V-9 python3 is visible in the minimal PATH" "1" \
    "$(PATH="$MINBIN" command -v python3 >/dev/null 2>&1 && echo 1 || echo 0)"
SEED_LAUNCHER_LOCAL="${SEED_LAUNCHER:-}"
eq "V-9 bootstrap seed entry exists" "yes" \
    "$([ -n "$SEED_LAUNCHER_LOCAL" ] && [ -x "$SEED_LAUNCHER_LOCAL" ] && echo yes || echo no)"
DIST="$TMPROOT/dist"
mkdir -p "$DIST"
cp "$SEED_LAUNCHER_LOCAL" "$DIST/brickie"
eq "V-9 distribution dir holds only the one brickie file" "1" "$(find "$DIST" -maxdepth 1 -type f | wc -l | tr -d ' ')"
W="$(new_work)"
mkdir -p "$W/home"
run_seed() { # run via the seed entry under a minimal PATH; CORE/GEN explicitly point at the real ones
    local work="$1"; shift
    ( cd "$work" && env -i PATH="$MINBIN" HOME="$work/home" \
        BRICKIE_CORE="$CORE" BRICKIE_GEN="$GEN" "$DIST/brickie" "$@" ) >"$OUT" 2>"$ERR"
    RC=$?
}
run_seed "$W" new ability provider/crypto --subkind service
eq "V-9 loop ① new exit code 0" "0" "$RC"
run_seed "$W" check
eq "V-9 loop ② check exit code 0" "0" "$RC"
run_seed "$W" dep closure
eq "V-9 loop ③ dep closure exit code 0" "0" "$RC"
run_seed "$W" iface publish provider/crypto#crypto
eq "V-9 loop ④ iface publish exit code 0" "0" "$RC"
run_seed "$W" iface status provider/crypto#crypto --check
eq "V-9 loop ⑤ iface status --check exit code 0" "0" "$RC"
run_seed "$W" gen --check
eq "V-9 loop ⑥ gen --check exit code 0" "0" "$RC"
eq "V-9 the whole loop is green with no compiler present" "yes" \
    "$([ -f "$W/api/iface/provider/crypto/crypto.toml" ] && echo yes || echo no)"
grep_none "V-9 suite scripts do not call cargo/g++" \
    '(^|[;&|(])[[:space:]]*(cargo|g\+\+)[[:space:]]' \
    "$TESTS_DIR/run.sh" "$TESTS_DIR/purity.sh" "$TESTS_DIR/lib.sh"

# ---- V-10 requires_iface
section "V-10 requires_iface: schema / not part of the closure / info only"
W="$(cp_fx req-iface)"
run_json "$W" check
eq "V-10 baseline check exit code 0 (info only)" "0" "$RC"
eqj "V-10 consuming an unfrozen interface under dev = info" "$JQ_RESP" "diagnostics.0.severity" "info"
W2="$(cp_fx req-iface-plain)"
run_brickie "$W" dep closure --json
A="$(cat "$OUT")"
run_brickie "$W2" dep closure --json
B="$(cat "$OUT")"
eq "V-10② closure byte-identical with/without requires_iface" "$A" "$B"
run_json "$W" iface show provider/consumer#consumer
eq "V-10③ iface show exit code 0" "0" "$RC"
eq "V-10③ registered as info (NOT_ABI + requires_iface, two entries)" "2" \
    "$(grep -c '"severity": "info"' "$JQ_RESP" || true)"
grep_at_least "V-10③ message says v0.1 does not scan" 'does not scan' "$JQ_RESP"
grep_at_least "V-10③ span = compat.requires_iface[0]" 'compat.requires_iface\[0\]' "$JQ_RESP"
W3="$(cp_fx req-iface)"
sed -i '/^compat_gen = 0$/d' "$W3/provider/consumer/brick.toml"
run_json "$W3" check
eq "V-10① missing compat_gen ⇒ exit code 2" "2" "$RC"
eq "V-10① reports BRV-MF-0001" "1" "$(grep -c 'BRV-MF-0001' "$ERR" || true)"
grep_at_least "V-10① span names compat_gen" 'compat.requires_iface\[0\].compat_gen' "$ERR"
W3="$(cp_fx req-iface)"
sed -i '/^api_iface  = "native"$/d' "$W3/provider/consumer/brick.toml"
run_json "$W3" check
eq "V-10① missing api_iface ⇒ exit code 2" "2" "$RC"

# ---- V-11 hash semantics
section "V-11 hash_scope/truth paired / NOT_ABI / hash cross-check"
W="$(cp_fx truth)"
run_json "$W" iface show provider/x#x
eqj "V-11① --json carries hash_scope" "$JQ_RESP" "data.hash_scope" "decl"
eqj "V-11① --json carries truth" "$JQ_RESP" "data.truth" "decl"
eqj "V-11③ --json carries not_abi" "$JQ_RESP" "data.not_abi" "true"
grep_at_least "V-11③ text folds in the NOT_ABI hint" 'NOT_ABI' "$OUT"
Ws="$(cp_fx truth)"
sed -i 's/^hash_scope = "decl"$/hash_scope = "sym"/' "$Ws/provider/x/brick.toml"
run_json "$Ws" check
eq "V-11② hash_scope=sym ⇒ exit code 1" "1" "$RC"
eq "V-11② reports BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
Wt="$(cp_fx truth)"
sed -i 's/^truth      = "decl"$/truth      = "header"/' "$Wt/provider/x/brick.toml"
run_json "$Wt" check
eq "V-11② truth=header ⇒ exit code 1" "1" "$RC"
eq "V-11② reports BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
Wp="$(cp_fx publish)"
run_brickie "$Wp" iface publish provider/a#a
sed -i 's/^hash         = ""$/hash         = "sha256:deadbeef"/' "$Wp/provider/a/brick.toml"
run_json "$Wp" iface status provider/a#a
eq "V-11④ hash mismatch ⇒ status exit code 1" "1" "$RC"
eq "V-11④ reports BRV-IFACE-0012" "1" "$(grep -c 'BRV-IFACE-0012' "$ERR" || true)"
grep_at_least "V-11④ message names both recorded places" 'one truth recorded in two places' "$ERR"

# ---- V-12 export classification invariants
section "V-12 TAX-0016/17/18/19 + third-party pieces + skin three small items"
for pair in "export-tax16:BRV-TAX-0016" "export-tax17:BRV-TAX-0017" \
            "export-tax18:BRV-TAX-0018" "export-tax19:BRV-TAX-0019"; do
    fx="${pair%%:*}"; code="${pair##*:}"
    W="$(cp_fx "$fx")"
    run_json "$W" check
    eq "V-12 $fx exit code 1" "1" "$RC"
    eq "V-12 $fx reports $code" "1" "$(grep -c "$code" "$ERR" || true)"
done
# ADR-0033 (`BRV-TAX-0022`): a surface class needs a surface. `fx/thirdparty` is a plugin that
# declares `api_type = "third_party"` while exporting nothing ⇒ red. (It used to be the *positive*
# case "third_party has no export"; the ruling reversed it: a fully adapted port exports nothing and
# is `native`.) The positive counterpart -- third_party **with** a matching export -- is V-12 ③ below.
W="$(cp_fx thirdparty)"
run_json "$W" check
eq "V-12 ① third_party with no export ⇒ exit code 1" "1" "$RC"
eq "V-12 ① reports BRV-TAX-0022" "1" "$(grep -c 'BRV-TAX-0022' "$ERR" || true)"
W="$(cp_fx skin-one)"
run_json "$W" check
eq "V-12 positive ②: skin single provider ⇒ exit code 0" "0" "$RC"
W="$(cp_fx skin-multi)"
run_json "$W" check
eq "V-12 positive ②: skin multiple providers + equal classes ⇒ exit code 0" "0" "$RC"
grep_none "V-12 positive ② no TAX-0018" 'BRV-TAX-0018' "$ERR"
W="$(cp_fx sqlite)"
run_json "$W" check
eq "V-12 positive ③: app → third-party piece + adapter type edges only ⇒ exit code 0" "0" "$RC"
eqj "V-12 positive ③ errors=0" "$JQ_RESP" "data.summary.errors" "0"

# ---- V-13 version strings and ranges
section "V-13 version strings / range segment count / monotonic, no rollback"
W="$(cp_fx ver-badstr)"
run_json "$W" check
eq "V-13 5-segment version string ⇒ exit code 2" "2" "$RC"
eq "V-13 reports BRV-VER-0005" "1" "$(grep -c 'BRV-VER-0005' "$ERR" || true)"
W="$(cp_fx ver-badrange)"
run_json "$W" check
eq "V-13 4-segment range ⇒ exit code 2" "2" "$RC"
eq "V-13 reports BRV-VER-0006" "1" "$(grep -c 'BRV-VER-0006' "$ERR" || true)"
W="$(cp_fx ver-pad)"
run_json "$W" check
eq "V-13 short range right-padded with 0 (>=0.1 ≡ >=0.1.0) ⇒ exit code 0" "0" "$RC"
run_brickie "$W" dep tree
grep_at_least "V-13 canonical output right-pads with 0" '\[runtime >=0\.1\.0\]' "$OUT"
W="$(cp_fx publish)"
run_brickie "$W" iface publish provider/a#a
python3 - "$W/provider/a/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "g"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface publish provider/a#a --set 0.0.9.0
eq "V-13 --set below the current version ⇒ exit code 1" "1" "$RC"
eq "V-13 reports BRV-VER-0007" "1" "$(grep -c 'BRV-VER-0007' "$ERR" || true)"
run_json "$W" iface publish provider/a#a --set 0.1.9.0
eq "V-13 --set forward ⇒ exit code 0" "0" "$RC"

# ---- V-14 profile gating
section "V-14 same input, two profiles, two conclusions"
W="$(cp_fx req-iface)"
run_json "$W" check --profile dev
eq "V-14 dev ⇒ exit code 0" "0" "$RC"
eqj "V-14 under dev VER-0004 is info" "$JQ_RESP" "diagnostics.0.severity" "info"
grep_at_least "V-14 dev shows a VER-0004 hint" 'BRV-VER-0004' "$JQ_RESP"
run_json "$W" check --profile release
eq "V-14 release ⇒ exit code 1" "1" "$RC"
eq "V-14 release reports BRV-VER-0004" "1" "$(grep -c 'BRV-VER-0004' "$ERR" || true)"
grep_at_least "V-14 under release it is upgraded to error" 'error' "$ERR"
snapj "check-release-ver0004" "$JQ_RESP"

# ---- V-15 append vs modify
section "V-15 entry-level append vs modify (§5.4)"
W="$(cp_fx iface-enum)"
run_brickie "$W" iface publish provider/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["A", "B", "C"]/' "$W/provider/enums/brick.toml"
run_json "$W" iface diff provider/enums#enums
eqj "V-15 enum appended at the end ⇒ EXTENDED" "$JQ_RESP" "data.changes.0.kind" "EXTENDED"
eqj "V-15 EXTENDED needs no unfreeze ⇒ compat_gen unchanged" "$JQ_RESP" "data.compat_gen_changed" "false"
eqj "V-15 EXTENDED verdict=green" "$JQ_RESP" "data.verdict" "green"
run_json "$W" iface publish provider/enums#enums
eq "V-15 EXTENDED publish exit code 0" "0" "$RC"
eqj "V-15 EXTENDED ⇒ MINOR+1" "$JQ_RESP" "data.version" "0.1.1.0"
W="$(cp_fx iface-enum)"
run_brickie "$W" iface publish provider/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/provider/enums/brick.toml"
run_json "$W" iface diff provider/enums#enums
eqj "V-15 enum reordering ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 enum reordering ⇒ compat_gen_changed" "$JQ_RESP" "data.compat_gen_changed" "true"
eqj "V-15 not unfrozen ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish provider/enums#enums
eq "V-15 publish without unfreeze ⇒ exit code 1" "1" "$RC"
W="$(cp_fx iface-struct)"
run_brickie "$W" iface publish provider/st#st
sed -i 's/^layout = "{ void\* p; }"$/layout = "{ void* p; int n; }"/' "$W/provider/st/brick.toml"
run_json "$W" iface diff provider/st#st
eqj "V-15 struct field added ⇒ CHANGED(D22)" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 struct field added ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish provider/st#st
eq "V-15 struct field added without unfreeze ⇒ exit code 1" "1" "$RC"
W="$(cp_fx iface-ops)"
run_brickie "$W" iface publish provider/sv#sv
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/provider/sv/brick.toml"
run_json "$W" iface diff provider/sv#sv
eqj "V-15 ops slot added ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 ops slot added ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
W="$(cp_fx iface-macro)"
run_brickie "$W" iface publish provider/crypto#crypto
python3 - "$W/provider/crypto/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface diff provider/crypto#crypto
eqj "V-15 added entry ⇒ ADDED" "$JQ_RESP" "data.changes.0.kind" "ADDED"
eqj "V-15 ADDED needs no unfreeze ⇒ verdict=green" "$JQ_RESP" "data.verdict" "green"
eqj "V-15 ADDED ⇒ compat_gen unchanged" "$JQ_RESP" "data.compat_gen_changed" "false"

# ---- V-16 unfreeze window
section "V-16 unfreeze gate / release block / empty unfreeze leaves all four segments alone"
W="$(cp_fx unfreeze)"
printf 'RFC\n' >"$W/rfc.md"
run_brickie "$W" iface unfreeze provider/crypto#crypto
eq "V-16 unfreeze without --note ⇒ nonzero (argparse usage error 2)" "2" "$RC"
run_brickie "$W" iface publish provider/crypto#crypto
eq "V-16 file-creation publish exit code 0" "0" "$RC"
run_brickie "$W" iface unfreeze provider/crypto#crypto --note rfc.md
eq "V-16 with --note, entering the window, exit code 0" "0" "$RC"
run_json "$W" iface status provider/crypto#crypto
eqj "V-16 entering the window does not change the surface (snapshot_hash == recomputed_hash)" "$JQ_RESP" \
    "data.snapshot_hash" "$(jqget "$JQ_RESP" data.recomputed_hash)"
run_json "$W" check --profile release
eq "V-16 release inside the window ⇒ exit code 1" "1" "$RC"
eq "V-16 reports BRV-IFACE-0009" "1" "$(grep -c 'BRV-IFACE-0009' "$ERR" || true)"
run_json "$W" check --profile dev
eq "V-16 dev inside the window ⇒ exit code 0" "0" "$RC"
before_cg="$(jqget "$(run_json "$W" ver show provider/crypto#crypto; echo "$JQ_RESP")" data.version)"
run_brickie "$W" iface refreeze provider/crypto#crypto
eq "V-16 empty-unfreeze refreeze exit code 0" "0" "$RC"
run_json "$W" ver show provider/crypto#crypto
eq "V-16 empty unfreeze ⇒ all four segments stay put" "$before_cg" "$(jqget "$JQ_RESP" data.version)"
eqj "V-16 empty unfreeze leaves compat_gen alone" "$JQ_RESP" "data.segments.compat_gen" "0"

# ---- V-17 hash input completeness
section "V-17 macro values / service ops / enum order must all enter the hash"
hash_of() { # hash_of <work> <id>
    run_json "$1" iface show "$2"
    jqget "$JQ_RESP" data.declaration_hash
}
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" provider/crypto#crypto)"
sed -i 's/^value = "16"$/value = "4096"/' "$W/provider/crypto/brick.toml"
H1="$(hash_of "$W" provider/crypto#crypto)"
eq "V-17 BR_MAX 16→4096 yields a different hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
contains "V-17 hash is sha256:..." "sha256:" "$H0"
W="$(cp_fx iface-ops)"
H0="$(hash_of "$W" provider/sv#sv)"
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/provider/sv/brick.toml"
H1="$(hash_of "$W" provider/sv#sv)"
eq "V-17 adding a service ops slot yields a different hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
W="$(cp_fx iface-enum)"
H0="$(hash_of "$W" provider/enums#enums)"
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/provider/enums/brick.toml"
H1="$(hash_of "$W" provider/enums#enums)"
eq "V-17 enum reordering is seen by the hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" provider/crypto#crypto)"
python3 - "$W/provider/crypto/brick.toml" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
head, tail = s.split("[build]", 1)
blocks = re.findall(r"\[\[export\.entries\]\][^\[]*", head)
assert len(blocks) == 2, blocks
out = head.replace(blocks[0], "@@B0@@").replace(blocks[1], "@@B1@@")
out = out.replace("@@B0@@", blocks[1].rstrip() + "\n\n").replace("@@B1@@", blocks[0].rstrip() + "\n\n")
p.write_text(out + "[build]" + tail)
PY
run_json "$W" iface show provider/crypto#crypto
eq "V-17 declaration order does not affect the hash (IFACE-IR rule 1)" "$H0" "$(jqget "$JQ_RESP" data.declaration_hash)"
W="$(cp_fx publish)"
run_brickie "$W" iface publish provider/a#a
S1="$(sha256sum "$W/api/iface/provider/a/a.toml" "$W/brickie.lock" "$W/api/iface/CHANGELOG.md")"
W2="$(cp_fx publish)"
run_brickie "$W2" iface publish provider/a#a
S2="$(sha256sum "$W2/api/iface/provider/a/a.toml" "$W2/brickie.lock" "$W2/api/iface/CHANGELOG.md")"
eq "V-17 same input, two publish runs byte-identical" "${S1//$W/}" "${S2//$W2/}"

# ---- full command-surface coverage
section "command surface: 23 leaf commands + 2 global switches (human-readable + --json, two output modes)"
sweep() {
    MODE="$1"
    local W J
    W="$(cp_fx showcase)"
    printf 'RFC\n' >"$W/rfc.md"
    J="$TMPROOT/json-$MODE"
    mkdir -p "$J"

    S() { # S <label> <expected exit code> <text fragment> <json path> <json expected|present|absent> [args...]
        local label="$1" exp="$2" tgrep="$3" jpath="$4" jexp="$5"; shift 5
        if [ "$MODE" = json ]; then
            run_brickie "$W" "$@" --json
            cp "$OUT" "$J/$label.json"
            local r="$J/$label.json"
            eq "$label[--json] exit code" "$exp" "$RC"
            eq "$label[--json] envelope keys" "$ENVELOPE_KEYS" "$("$PY" "$JSONQ" envelope "$r")"
            eq "$label[--json] protocol=1" "1" "$("$PY" "$JSONQ" get "$r" protocol)"
            sortedj "$label[--json] diagnostics stably sorted" "$r"
            case "$jexp" in
                present) hasj "$label[--json] has $jpath" "$r" "$jpath" ;;
                absent)  eqjc "$label[--json] empty $jpath" "$r" "$jpath" "0" ;;
                *)       eqj  "$label[--json] $jpath" "$r" "$jpath" "$jexp" ;;
            esac
        else
            run_brickie "$W" "$@"
            eq "$label exit code" "$exp" "$RC"
            [ -n "$tgrep" ] && grep_at_least "$label text contains '$tgrep'" "$tgrep" "$OUT"
        fi
    }

    S check           0 'check passed'                data.summary.errors 0          check
    S closure         0 'product closure'                data.selected       present    dep closure
    S tree            0 'dependency tree'                  data.roots          present    dep tree
    S graph-dot       0 'digraph deps'            data.format         dot        dep graph --format dot
    S graph-mermaid   0 'graph LR'                data.format         mermaid    dep graph --format mermaid
    S graph-json      0 'format=json'             data.format         json       dep graph --format json
    S why-hit         0 'dependency path'                data.found          true       dep why app/hsm provider/crypto
    S why-miss        0 'dependency path not found'          data.found          false      dep why provider/crypto app/hsm
    S dep-index       0 'reverse dependency index'            data.dependents     present    dep index
    S gen             0 'generated-file rebuild plan'              data.count          6          gen
    S gen-check       0 'byte-identical to recomputation'              data.count          6          gen --check
    S ver-show        0 'interface unit provider/crypto#crypto' data.id     provider/crypto#crypto ver show provider/crypto
    S iface-list      0 'interface unit'                data.count          5          iface list
    S iface-show      0 'declaration hash'             data.not_abi        true       iface show provider/crypto#crypto
    S iface-diff0     0 'interface change set'              data.id             present    iface diff provider/crypto#crypto
    S iface-status0   1 'consistency inconsistent:'                  data.consistent     false      iface status provider/crypto#crypto
    S iface-status-c0 1 'consistency inconsistent:'                  data.consistent     false      iface status provider/crypto#crypto --check
    S iface-publish   0 'interface publish'                data.noop           false      iface publish provider/crypto#crypto
    S iface-status1   0 'consistency consistent:'                    data.consistent     true       iface status provider/crypto#crypto
    S iface-noop      0 'no-op'                  data.noop           true       iface publish provider/crypto#crypto
    S iface-pchk      0 'no-op'                  data.noop           true       iface publish provider/crypto#crypto --check
    S ver-bump        0 'version bump'                data.rule           revise     ver bump provider/crypto#crypto --rule revise
    S iface-freeze    0 'promotion proposal pending'              data.proposal       present    iface freeze provider/crypto#crypto --note rfc.md
    S iface-deprecate 0 'deprecate'                data.status         deprecated iface deprecate provider/crypto#crypto --note rfc.md
    S iface-undep     0 'undeprecate'            data.status         frozen     iface undeprecate provider/crypto#crypto --note rfc.md
    S iface-unfreeze  0 'enter window'                data.freeze_state   unfreezing iface unfreeze provider/crypto#crypto --note rfc.md
    S iface-refreeze  0 'leave window'                data.freeze_state   frozen     iface refreeze provider/crypto#crypto
    S dep-add         0 'dep add only prints a snippet'      data.found          true       dep add app/hsm iface/hsm-api --kind runtime
    S dep-rm          0 'dep rm only prints a snippet'       data.found          false      dep rm app/hsm provider/nowhere

    local W0="$W"
    W="$(new_work)"
    S new  0 'skeleton generated' data.count 6 new ability provider/svc --subkind service
    W="$(new_work)"
    S init 0 'skeleton generated' data.count 8 init hsm
    W="$W0"
}
sweep text
sweep json

# ---- V-18 glue-layer purity
section "V-18 glue-layer purity (three criteria in tests/purity.sh)"
PURITY_OUT="$TMPROOT/purity.out"
bash "$TESTS_DIR/purity.sh" >"$PURITY_OUT" 2>&1
PURITY_RC=$?
cat "$PURITY_OUT"
PURITY_N="$(grep -oE 'purity passed [0-9]+ / failed [0-9]+' "$PURITY_OUT" | head -1 || true)"
if [ -n "$PURITY_N" ]; then
    pn="$(printf '%s' "$PURITY_N" | sed -E 's/.*passed ([0-9]+) .*/\1/')"
    pf="$(printf '%s' "$PURITY_N" | sed -E 's/.*failed ([0-9]+).*/\1/')"
    PASS=$((PASS + pn)); FAIL=$((FAIL + pf))
    eq "V-18 purity exit code 0" "0" "$PURITY_RC"
else
    bad "V-18 purity.sh produced no summary line" "$(head -c 300 "$PURITY_OUT")"
fi
if [ "$PURITY_RC" -ne 0 ]; then
    grep -E '^FAIL' "$PURITY_OUT" >>"$FAILLOG" || true
fi

# ---- V-19 privilege and budget
section "V-19 privilege declarations / budget / resource exclusivity"
W="$(cp_fx priv-ability-p3)"
run_json "$W" check
eq "V-19① ability declares P3 ⇒ exit code 1" "1" "$RC"
eq "V-19① reports BRV-PRIV-0001" "1" "$(grep -c 'BRV-PRIV-0001' "$ERR" || true)"
grep_at_least "V-19① names the level gap" 'allows at most privilege level P2' "$ERR"
W="$(cp_fx priv-p2-map)"
run_json "$W" check
eq "V-19② level=P2 declares map ⇒ exit code 1" "1" "$RC"
eq "V-19② reports BRV-PRIV-0001" "1" "$(grep -c 'BRV-PRIV-0001' "$ERR" || true)"
grep_at_least "V-19② names the op and the minimum level" 'op `map` requires at least P4' "$ERR"
W="$(cp_fx budget)"
run_json "$W" check
eq "V-19③ budget overrun ⇒ exit code 1" "1" "$RC"
grep_at_least "V-19③ RAM delta" 'exceeds the product budget 512, delta 188 KiB' "$ERR"
grep_at_least "V-19③ stack delta" 'exceeds the product budget 16, delta 6 KiB' "$ERR"
eq "V-19③ two budget diagnostics (RAM + stack)" "2" "$(grep -c 'BRV-MF-0001' "$JQ_RESP" || true)"
W="$(cp_fx res-conflict)"
run_json "$W" check
eq "V-19④ IRQ/DMA exclusivity conflict ⇒ exit code 1" "1" "$RC"
eqjc "V-19④ two conflicts (irq + dma)" "$JQ_RESP" "diagnostics" "2"
grep_at_least "V-19④ reports both parties (irq)" 'irq `32` is declared by both `provider/irq-a`.*and `provider/irq-b`' "$ERR"
grep_at_least "V-19④ reports both parties (dma)" 'dma_channel `3` is declared by both `provider/irq-a`.*and `provider/irq-b`' "$ERR"
W="$(cp_fx priv-pool-ok)"
run_json "$W" check
eq "V-19 orthogonality table: pool lifecycle (alloc/free, granularity=pool) is P2 ⇒ legal, 0" "0" "$RC"
W="$(cp_fx priv-platform-p4)"
run_json "$W" check
eq "V-19 orthogonality table: map/protect are P4, the runtime plugin may declare them ⇒ legal, 0" "0" "$RC"

# ---- V-20 show family (ADR-0036): closure view + runtime init flow
section "V-20 show family (bricks / brick-init-flow)"
W="$(cp_fx showcase)"
run_brickie "$W" show bricks
eq "V-20① show bricks exit code 0" "0" "$RC"
grep_at_least "V-20① lists the closure" 'product bricks .*6 plugins, 2 roots' "$OUT"
grep_at_least "V-20① marks a product root" 'app/hsm .*root:app' "$OUT"
grep_at_least "V-20① marks a closure member" 'runtime/qemu-aarch64 .*dep' "$OUT"
grep_at_least "V-20① reports the budget it spends" 'product budget RAM 512 / stack 16' "$OUT"
run_json "$W" show bricks
eqj "V-20① roots counted" "$JQ_RESP" "data.totals.roots" "2"
eqj "V-20① plugins counted" "$JQ_RESP" "data.totals.plugins" "6"
eqj "V-20① root origin is recorded" "$JQ_RESP" "data.bricks.0.origin" "root"

run_brickie "$W" show brick-init-flow
eq "V-20② show brick-init-flow exit code 0" "0" "$RC"
grep_at_least "V-20② runtime early_init is the first step" '0  *early  *early_init  *runtime/qemu-aarch64' "$OUT"
grep_none "V-20② no APP early_init step" 'early  *early_init  *app/hsm' "$OUT"
grep_at_least "V-20② APP init comes after every LATE init" 'app/hsm  *APP init last' "$OUT"
grep_at_least "V-20② APP start is the last start step" 'app/hsm  *APP last: explicit rule' "$OUT"
grep_at_least "V-20② the IRQ hand-off is a step of its own" 'br_irq_cpu_enable\(\): after all init returned, before START' "$OUT"
grep_at_least "V-20② the order basis names the runtime sort" 'FIFO Kahn over kind=init edges' "$OUT"
run_json "$W" show brick-init-flow
eqj "V-20② four early steps (runtime + 3, no APP)" "$JQ_RESP" "data.counts.early" "4"
eqj "V-20② exactly one IRQ step" "$JQ_RESP" "data.counts.irq" "1"
eqj "V-20② five start steps" "$JQ_RESP" "data.counts.start" "5"
eqj "V-20② exactly one APP-phase init" "$JQ_RESP" "data.counts.app" "1"
hasj "V-20② the runtime order is reported" "$JQ_RESP" "data.order"
eqj "V-20② the first step is the runtime plugin's early_init" "$JQ_RESP" "data.steps.0.hook" "early_init"
eqj "V-20② the first step names the runtime plugin" "$JQ_RESP" "data.steps.0.plugin" "runtime/qemu-aarch64"
eqj "V-20② no phase divergence in a consistent tree" "$JQ_RESP" "data.counts.selftest" "0"

# The runtime order is not the name-tie-broken `dep closure` order: for this fixture they differ
# (section order puts the runtime plugin before sched/coop, the name order does not), which is exactly why
# the two views exist separately (ADR-0036 §2.1).
run_json "$W" dep closure
eqj "V-20② dep closure keeps its own topological order" "$JQ_RESP" "data.topo_order.0" "app/hsm"

# A declared phase that the runtime would not honour is red, not silently rerouted.
W="$(cp_fx showcase)"
sed -i 's/^phase *= *"app"/phase = "core"/' "$W/app/hsm/brick.toml"
run_json "$W" check
eq "V-20③ app declaring a phase it does not run in ⇒ exit code 2" "2" "$RC"
grep_at_least "V-20③ the phase gate says why" 'phase `core` is incompatible with' "$ERR"

# ---- V-21 APP ordering (ADR-0037): no early_init, and its init is the last init
section "V-21 APP ordering (ADR-0037)"
W="$(cp_fx showcase)"
run_json "$W" gen
eq "V-21① gen exit code 0" "0" "$RC"
grep_none "V-21① the APP descriptor has no early_init hook" '\.early_init  = hello_early_init' "$W/build/gen/app/hsm/plugin_desc.c"
grep_at_least "V-21① the APP descriptor writes NO_HOOK for it" '\.early_init  = BR_PLUGIN_NO_HOOK' "$W/build/gen/app/hsm/plugin_desc.c"
grep_none "V-21① no early_init prototype is emitted either" 'int hsm_early_init\(void\);' "$W/build/gen/app/hsm/plugin_desc.c"
# A non-APP plugin still gets one (the exception is the APP, not a global removal).
grep_at_least "V-21① a non-APP plugin still emits early_init" '\.early_init  = coop_early_init' "$W/build/gen/sched/coop/plugin_desc.c"

# The APP's init is the last init in the derived runtime flow, and it lands in the APP phase.
run_json "$W" show brick-init-flow
eqj "V-21② the APP init is in the APP phase" "$JQ_RESP" "data.counts.app" "1"
eqj "V-21② the APP phase holds exactly the APP" "$JQ_RESP" "data.steps.8.phase" "app"
eqj "V-21② and it is the APP" "$JQ_RESP" "data.steps.8.plugin" "app/hsm"
eqj "V-21② the IRQ hand-off follows the APP init" "$JQ_RESP" "data.steps.9.phase" "irq"

# V-B build family: declaration-surface and planning criteria only -- no compiler is called (V-9's
# discipline binds the tests too); the judge's log arrives as body text in context.log.
section "V-B build family (criteria that call no compiler)"
FX_BUILD="$(cp_fx build)"

JUDGE_RESP=""
core_judge() { # core_judge <work> <gate-name> <log-file> ⇒ JUDGE_RESP
    local w="$1" name="$2" logf="$3"
    "$PY" - "$w" "$name" "$logf" "$TMPROOT/.judge.req.json" <<'PYX'
import json, sys
w, name, logf, out = sys.argv[1:5]
json.dump({"protocol": 1, "command": "judge", "root": w,
           "args": {"name": name},
           "context": {"log": open(logf, encoding="utf-8").read()}},
          open(out, "w"))
PYX
    "$CORE" <"$TMPROOT/.judge.req.json" >"$TMPROOT/.judge.resp.json" 2>/dev/null
    JUDGE_RESP="$TMPROOT/.judge.resp.json"
}

# ---- ① plan shape: build --dry-run needs no compiler (core emits the plan, L5 prints it). ----
W="$(cp_fx build)"
run_json "$W" build --dry-run
eq "V-B① build --dry-run exit code 0" "0" "$RC"
eqj "V-B① dry_run flag" "$JQ_RESP" "data.dry_run" "true"
eqj "V-B① unit count = 6 (core + 5 plugins)" "$JQ_RESP" "data.unit_count" "6"
eqjc "V-B① data.units matches unit_count" "$JQ_RESP" "data.units" "6"
eqj "V-B① source file count" "$JQ_RESP" "data.source_count" "6"
eqj "V-B① steps_total = 14 (6 compiles + 5 archives + 1 symbol audit + link + objcopy)" "$JQ_RESP" "data.steps_total" "14"
eqjc "V-B① dry-run lists all steps" "$JQ_RESP" "data.steps" "14"
keysj "V-B① data.units[0] shape" "$JQ_RESP" "data.units.0" \
    "defines,dir,includes,name,plugin,sources,sources_list"
keysj "V-B① data.steps[0] shape (contract §5.4)" "$JQ_RESP" "data.steps.0" \
    "argv,consumes,expect_timeout,fail_code,group,judge,kind,label,log,outputs,stdout_lines,timeout_s,tool_code"
eqj "V-B① steps[0].fail_code = BRV-BLD-0011" "$JQ_RESP" "data.steps.0.fail_code" "BRV-BLD-0011"
eqj "V-B① steps[0].tool_code = BRV-BLD-0005" "$JQ_RESP" "data.steps.0.tool_code" "BRV-BLD-0005"
eqj "V-B① target comes from the runtime plugin" "$JQ_RESP" "data.target.owner" "runtime/qemu-aarch64"
eqj "V-B① target.arch" "$JQ_RESP" "data.target.arch" "aarch64"
eqj "V-B① target.cross" "$JQ_RESP" "data.target.cross" "aarch64-linux-gnu-"
eqj "V-B① target.linker_script (relative to the plugin root)" "$JQ_RESP" "data.target.linker_script" \
    "runtime/qemu-aarch64/src/link.ld"
STEPS_JSON="$(jqget "$JQ_RESP" data.steps)"
contains "V-B① arch flag (-mstrict-align) enters the compile argv" '"-mstrict-align"' "$STEPS_JSON"
contains "V-B① arch flag (-mgeneral-regs-only) enters the compile argv" '"-mgeneral-regs-only"' "$STEPS_JSON"
contains "V-B① linker script enters -Wl,-T" '"-Wl,-T,runtime/qemu-aarch64/src/link.ld"' "$STEPS_JSON"
contains "V-B① -Wl,-Map uses the artifact path" '"-Wl,-Map,build/brick.map"' "$STEPS_JSON"
contains "V-B① declared cflags enter argv" '"-DFX_BUILD=1"' "$STEPS_JSON"
contains "V-B① the assembly source (.S) is in the plan too (paths relative to root)" \
    'runtime/qemu-aarch64/src/start.S' "$STEPS_JSON"
# Every plan path is relative to root; the [build].sources glob is relative to the plugin root.
asm_ok() { # asm_ok <resp.json> ⇒ yes / no:…
    "$PY" - "$1" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
step = [s for s in d["steps"]
        if s["kind"] == "compile" and any(a.endswith(".S") for a in s["argv"])]
if not step:
    print("no-asm-step")
else:
    argv = step[0]["argv"]
    ok = ("-std=c11" not in argv and "-g3" in argv and "-mstrict-align" in argv)
    print("yes" if ok else "no:" + " ".join(argv))
PYX
}
eq "V-B① assembly steps compile via asflags (not mixed with C's -std=c11)" "yes" "$(asm_ok "$JQ_RESP")"
contains "V-B① link output is build/brick.elf" '"build/brick.elf"' "$STEPS_JSON"
# tool candidate order (ruling R-14): the order is the policy, and core reports the resolution.
eqjc "V-B① data.tools has nine entries (ar/cc/make/ninja/nm/objcopy/objdump/qemu/size)" "$JQ_RESP" "data.tools" "9"
eqj "V-B① tools[0].name = ar (BTreeMap order)" "$JQ_RESP" "data.tools.0.name" "ar"
eqj "V-B① tools[1].name = cc" "$JQ_RESP" "data.tools.1.name" "cc"
eqj "V-B① tools[4].name = nm" "$JQ_RESP" "data.tools.4.name" "nm"
keysj "V-B① data.tools[0] shape" "$JQ_RESP" "data.tools.0" "candidates,name,resolved,what"
eqj "V-B① cc first candidate = clang wrapper" "$JQ_RESP" "data.tools.1.candidates.0" "tools/toolchain/aarch64-none-elf-clang"
eqj "V-B① cc 2nd candidate = <cross>gcc" "$JQ_RESP" "data.tools.1.candidates.1" "aarch64-linux-gnu-gcc"
eqj "V-B① cc 3rd candidate = gcc-16" "$JQ_RESP" "data.tools.1.candidates.2" "aarch64-linux-gnu-gcc-16"
eqj "V-B① cc last candidate = host cc" "$JQ_RESP" "data.tools.1.candidates.6" "cc"
eqj "V-B① tools[5].name = objcopy" "$JQ_RESP" "data.tools.5.name" "objcopy"
eqj "V-B① objcopy first candidate = cross version" "$JQ_RESP" "data.tools.5.candidates.0" "aarch64-linux-gnu-objcopy"
eqj "V-B① objcopy last candidate = host version" "$JQ_RESP" "data.tools.5.candidates.1" "objcopy"
hasj "V-B① tools[1] (cc) carries a resolved key (resolution in core)" "$JQ_RESP" "data.tools.1.resolved"
eqj "V-B① QEMU machine comes from [build.target.qemu]" "$JQ_RESP" "data.target.qemu.machine" "virt,gic-version=3"

# ---------------------------------------------------------------- ② missing [build]
W="$(cp_fx build)"
"$PY" - "$W/product.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().split("[build]")[0].rstrip() + "\n")
PY
run_json "$W" build --dry-run
eq "V-B② product.toml missing [build] ⇒ exit code 2" "2" "$RC"
eq "V-B② reports BRV-BLD-0001" "1" "$(grep -c 'BRV-BLD-0001' "$ERR" || true)"

# ---------------------------------------------------------------- ③ missing [build.target]
W="$(cp_fx build)"
"$PY" - "$W/runtime/qemu-aarch64/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().split("[build.target]")[0].rstrip() + "\n")
PY
run_json "$W" build --dry-run
eq "V-B③ no plugin declares [build.target] ⇒ exit code 2" "2" "$RC"
eq "V-B③ reports BRV-BLD-0003" "1" "$(grep -c 'BRV-BLD-0003' "$ERR" || true)"

# ---------------------------------------------------------------- ④ literal source missing
W="$(cp_fx build)"
sed -i 's|sources  = \["src/\*.c"\]|sources  = ["src/*.c", "src/nope.c"]|' \
    "$W/app/hello/brick.toml"
run_json "$W" build --dry-run
eq "V-B④ [build].sources literal path missing ⇒ exit code 2" "2" "$RC"
eq "V-B④ reports BRV-BLD-0007" "1" "$(grep -c 'BRV-BLD-0007' "$ERR" || true)"
grep_at_least "V-B④ span names build.sources" 'build\.sources' "$ERR"

# ---------------------------------------------------------------- ⑤ test --list
W="$(cp_fx build)"
run_json "$W" test --list
eq "V-B⑤ test --list exit code 0" "0" "$RC"
eqj "V-B⑤ declaration-surface file" "$JQ_RESP" "data.file" "tests/gates.toml"
eqj "V-B⑤ present" "$JQ_RESP" "data.present" "true"
eqjc "V-B⑤ 4 QEMU gates" "$JQ_RESP" "data.gates" "4"
eqj "V-B⑤ gates[0]" "$JQ_RESP" "data.gates.0" "g-route"
eqj "V-B⑤ gates[3]" "$JQ_RESP" "data.gates.3" "g-ok"
eqjc "V-B⑤ 2 host tests" "$JQ_RESP" "data.hosttests" "2"
eqj "V-B⑤ hosttests[1] (with depends)" "$JQ_RESP" "data.hosttests.1" "ht-dep"
eqjc "V-B⑤ 1 script gate" "$JQ_RESP" "data.scripts" "1"
eqj "V-B⑤ scripts[0]" "$JQ_RESP" "data.scripts.0" "s-echo"
eqjc "V-B⑤ one build_post" "$JQ_RESP" "data.build_post" "1"
eqj "V-B⑤ build_post[0] = g-ok" "$JQ_RESP" "data.build_post.0" "g-ok"

# ---------------------------------------------------------------- ⑥ judge (criteria in core)
printf 'nothing here\n' >"$TMPROOT/judge-empty.log"
printf 'BAD thing\n' >"$TMPROOT/judge-forbid.log"
printf 'OK\nPASS TC-2 case\n' >"$TMPROOT/judge-ok.log"
core_judge "$FX_BUILD" g-route "$TMPROOT/judge-empty.log"
"$PY" "$JSONQ" text "$JUDGE_RESP" >"$TMPROOT/.judge.txt"
eqj "V-B⑥ require missing one ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ require missing one ⇒ failed 1" "$JUDGE_RESP" "data.failed" "1"
eq "V-B⑥ require missing one ⇒ BRV-BLD-0009" "1" \
    "$(grep -c 'BRV-BLD-0009' "$TMPROOT/.judge.txt" || true)"
eqj "V-B⑥ require's span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].require"
core_judge "$FX_BUILD" g-forbid "$TMPROOT/judge-forbid.log"
eqj "V-B⑥ forbid hit ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ forbid hit ⇒ span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].forbid"
core_judge "$FX_BUILD" g-tag "$TMPROOT/judge-empty.log"
eqj "V-B⑥ missing PASS tag ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ missing PASS tag ⇒ span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].require_tags"
core_judge "$FX_BUILD" g-ok "$TMPROOT/judge-ok.log"
eqj "V-B⑥ all green ⇒ exit_code 0" "$JUDGE_RESP" "exit_code" "0"
eqj "V-B⑥ all green ⇒ failed 0" "$JUDGE_RESP" "data.failed" "0"
eqj "V-B⑥ all green ⇒ code-less info (ruling R-16)" "$JUDGE_RESP" "diagnostics.0.code" "null"
eqj "V-B⑥ all green ⇒ severity=info" "$JUDGE_RESP" "diagnostics.0.severity" "info"
eqj "V-B⑥ all green ⇒ three checks" "$JUDGE_RESP" "data.checks.2.ok" "true"
core_judge "$FX_BUILD" no-such-gate "$TMPROOT/judge-empty.log"
"$PY" "$JSONQ" text "$JUDGE_RESP" >"$TMPROOT/.judge.txt"
eqj "V-B⑥ unknown gate ⇒ exit_code 2" "$JUDGE_RESP" "exit_code" "2"
eq "V-B⑥ unknown gate ⇒ BRV-BLD-0008" "1" \
    "$(grep -c 'BRV-BLD-0008' "$TMPROOT/.judge.txt" || true)"

# ---------------------------------------------------------------- ⑦ regex super-subset
W="$(cp_fx gates-regex)"
run_json "$W" test --list
eq "V-B⑦ regex uses a ( group ⇒ exit code 2" "2" "$RC"
eq "V-B⑦ reports BRV-BLD-0012 (reject, not silently)" "1" "$(grep -c 'BRV-BLD-0012' "$ERR" || true)"
grep_at_least "V-B⑦ span names gate.require[0]" 'gate\.require\[0\]' "$ERR"

# ---------------------------------------------------------------- ⑧ clean paths table
W="$(cp_fx build)"
run_json "$W" clean --all
eq "V-B⑧ clean --all exit code 0" "0" "$RC"
eqjc "V-B⑧ 13 paths" "$JQ_RESP" "data.paths" "13"
PATHS_JSON="$(jqget "$JQ_RESP" data.paths)"
contains "V-B⑧ paths include build/obj" '"build/obj"' "$PATHS_JSON"
contains "V-B⑧ paths include build/brick.elf" '"build/brick.elf"' "$PATHS_JSON"
contains "V-B⑧ paths include build/brick.bin" '"build/brick.bin"' "$PATHS_JSON"
contains "V-B⑧ paths include build/gen (--all)" '"build/gen"' "$PATHS_JSON"
contains "V-B⑧ paths include the host-test drop point build/hosttest" '"build/hosttest"' "$PATHS_JSON"
eqj "V-B⑧ paths sorted (lexicographically first entry)" "$JQ_RESP" "data.paths.0" "build/brick.bin"
eqjc "V-B⑧ the fixture had no artifacts ⇒ nothing deleted" "$JQ_RESP" "written" "0"

# ---------------------------------------------------------------- ⑨ --emit-backends
W="$(cp_fx build)"
run_json "$W" build --dry-run --emit-backends
eq "V-B⑨ --emit-backends exit code 0" "0" "$RC"
eqj "V-B⑨ emit_backends flag" "$JQ_RESP" "data.emit_backends" "true"
eqjc "V-B⑨ two backend files written" "$JQ_RESP" "written" "2"
eq "V-B⑨ build/gen/build.mk in place" "yes" "$([ -f "$W/build/gen/build.mk" ] && echo yes || echo no)"
eq "V-B⑨ build/gen/build.ninja in place" "yes" "$([ -f "$W/build/gen/build.ninja" ] && echo yes || echo no)"
grep_at_least "V-B⑨ build.mk carries the generated marker" 'brickie:generated' "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.mk is BR-D4 backend A" '\$\(ELF\): \$\(OBJS\)' "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.mk writes the linker script into the rule" '-Wl,-T,runtime/qemu-aarch64/src/link.ld' \
    "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.ninja carries the generated marker" 'brickie:generated' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ build.ninja is BR-D4 backend B" '^rule link$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ build.ninja bakes in the resolved compiler" '^cc = ' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ ninja has a cc rule (C sources)" '^rule cc$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ ninja has an asm rule (.S sources)" '^rule asm$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ .S goes through the asm rule" ': asm .*src/start\.S' "$W/build/gen/build.ninja"
# Ninja's .ninja_log/.ninja_deps must not land in the repo root; builddir is pinned under build/gen.
grep_at_least "V-B⑨ ninja builddir is pinned under build/gen" '^builddir = build/gen/ninja$' \
    "$W/build/gen/build.ninja"
eq "V-B⑨ dry-run writes no incremental state (it is derived)" "no" \
    "$([ -f "$W/build/gen/build-state.json" ] && echo yes || echo no)"

# ---- V-22 source disciplines (ADR-0038): self-contained .c / globals stay inside a plugin /
# declared dependencies only. Pure text + declaration surface: no compiler, no archive (the symbol
# half is the `.a` audit inside `brickie build`, exercised by V-B⑩ below).
section "V-22 source disciplines (R1/R2/R3, ADR-0038)"
mk_discipline_tree() { # mk_discipline_tree <work>: two ability plugins, one provider, one consumer
    local w="$1"
    mkdir -p "$w/provider/prov/src" "$w/provider/prov/include" "$w/provider/cons/src" "$w/provider/cons/include"
    cat >"$w/provider/prov/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "provider/prov"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[[export]]
name = "prov"
api_iface = "native"
form = "api"
version = "0.1.0.0"
compat_gen = 0
freeze_state = "unfrozen"
status = "experimental"
[[export.entries]]
kind = "func"
name = "prov_ping"
sig = "int (void)"
[build]
sources = ["src/*.c"]
includes = ["include"]
TOML
    cat >"$w/provider/prov/include/prov.h" <<'H'
#ifndef PROV_H
#define PROV_H
int prov_ping(void);
#endif
H
    cat >"$w/provider/prov/src/prov.c" <<'C'
#include "prov.h"
int prov_ping(void) { return 7; }
C
    cat >"$w/provider/cons/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "provider/cons"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[[dep]]
name = "provider/prov"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
includes = ["include"]
TOML
    cat >"$w/provider/cons/src/cons.c" <<'C'
#include "prov.h"
int cons_run(void) { return prov_ping(); }
C
}

# ① R1: a prototype with no definition in the same .c
W="$(new_work)"; mk_discipline_tree "$W"
printf 'int lonely(void);\n' >>"$W/provider/cons/src/cons.c"
run_json "$W" check --src
eq "V-22① prototype without definition ⇒ exit code 1" "1" "$RC"
eqj "V-22① code is BRV-SRC-0001" "$JQ_RESP" "diagnostics.0.code" "BRV-SRC-0001"
contains "V-22① message names the symbol" 'lonely' "$(cat "$ERR")"

# ② R1: an `extern` declaration with no definition in the same .c
W="$(new_work)"; mk_discipline_tree "$W"
printf 'extern int elsewhere;\n' >>"$W/provider/cons/src/cons.c"
run_json "$W" check --src
eq "V-22② extern without definition ⇒ exit code 1" "1" "$RC"
eqj "V-22② code is BRV-SRC-0001" "$JQ_RESP" "diagnostics.0.code" "BRV-SRC-0001"

# ③ R2: a file-scope object without `static`
W="$(new_work)"; mk_discipline_tree "$W"
printf 'int leaked_counter;\n' >>"$W/provider/cons/src/cons.c"
run_json "$W" check --src
eq "V-22③ non-static global ⇒ exit code 1" "1" "$RC"
eqj "V-22③ code is BRV-SRC-0002" "$JQ_RESP" "diagnostics.0.code" "BRV-SRC-0002"
# …and `static` is accepted (the positive half, so the rule is not "no globals")
W="$(new_work)"; mk_discipline_tree "$W"
printf 'static int s_counter;\n' >>"$W/provider/cons/src/cons.c"
run_json "$W" check --src
eq "V-22③ static global ⇒ exit code 0" "0" "$RC"

# ④ R2: a public header exporting an object
W="$(new_work)"; mk_discipline_tree "$W"
printf 'extern int prov_state;\n' >>"$W/provider/prov/include/prov.h"
run_json "$W" check --src
eq "V-22④ header exports an object ⇒ exit code 1" "1" "$RC"
eqj "V-22④ code is BRV-SRC-0002" "$JQ_RESP" "diagnostics.0.code" "BRV-SRC-0002"

# ⑤ R2: `kind = "var"` in the export surface is refused
W="$(new_work)"; mk_discipline_tree "$W"
"$PY" - "$W/provider/prov/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text() + '[[export.entries]]\nkind = "var"\nname = "prov_state"\n')
PYX
run_json "$W" check
eq "V-22⑤ kind=var export ⇒ exit code 1" "1" "$RC"
eqj "V-22⑤ code is BRV-SRC-0002" "$JQ_RESP" "diagnostics.0.code" "BRV-SRC-0002"

# ⑥ R3: an include of a plugin that is only a *transitive* dependency
W="$(new_work)"; mk_discipline_tree "$W"
mkdir -p "$W/provider/mid/src" "$W/provider/mid/include"
"$PY" - "$W" <<'PYX'
import pathlib, sys
w = pathlib.Path(sys.argv[1])
(w / "provider/mid/brick.toml").write_text(
    'schema = 1\n[plugin]\nname = "provider/mid"\nplugin_type = "ability"\n'
    'api_type = "native"\nsubkind = "service"\nlang = "c"\nphase = "late"\nversion = "0.1.0.0"\n'
    '[[dep]]\nname = "provider/prov"\nrange = ">=0.1.0"\nkind = "runtime"\n'
    '[build]\nsources = ["src/*.c"]\nincludes = ["include"]\n')
cons = w / "provider/cons/brick.toml"
cons.write_text(cons.read_text().replace('name = "provider/prov"', 'name = "provider/mid"'))
PYX
run_json "$W" check --deps
eq "V-22⑥ transitive-only include ⇒ exit code 1" "1" "$RC"
eqj "V-22⑥ code is BRV-DEP-0012" "$JQ_RESP" "diagnostics.0.code" "BRV-DEP-0012"
contains "V-22⑥ message says 'direct'" 'no **direct** dependency' "$(cat "$ERR")"
# …and declaring the dependency directly makes it green again
"$PY" - "$W/provider/cons/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace(
    "[build]",
    '[[dep]]\nname = "provider/prov"\nrange = ">=0.1.0"\nkind = "runtime"\n\n[build]'))
PYX
run_json "$W" check --deps
eq "V-22⑥ declaring it directly ⇒ exit code 0" "0" "$RC"

# ⑧ the symbol half: the `.a` audit judge (the exact criteria `brickie build` runs)
W="$(new_work)"; mk_discipline_tree "$W"
cat >"$TMPROOT/nm-declared.log" <<'NM'
build/lib/provider/cons.a:cons.o:                 U prov_ping
build/lib/provider/prov.a:prov.o:0000000000000000 T prov_ping
NM
core_judge "$W" symbol-audit "$TMPROOT/nm-declared.log"
eqj "V-22⑧ audit: declared dependency ⇒ failed=0" "$JUDGE_RESP" "data.failed" "0"
"$PY" - "$W/provider/cons/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('[[dep]]\nname = "provider/prov"\nrange = ">=0.1.0"\nkind = "runtime"\n', ''))
PYX
core_judge "$W" symbol-audit "$TMPROOT/nm-declared.log"
eqj "V-22⑧ audit: undeclared dependency ⇒ failed=1" "$JUDGE_RESP" "data.failed" "1"
eqj "V-22⑧ audit: code is BRV-DEP-0012" "$JUDGE_RESP" "diagnostics.0.code" "BRV-DEP-0012"
contains "V-22⑧ audit names the symbol" 'prov_ping' "$(cat "$JUDGE_RESP")"

# ⑨ the symbol half of R2: a data symbol defined by a `native` plugin never crosses a plugin edge,
# even when the dependency is declared.
W="$(new_work)"; mk_discipline_tree "$W"
cat >"$TMPROOT/nm-data.log" <<'NM'
build/lib/provider/cons.a:cons.o:                 U prov_state
build/lib/provider/prov.a:prov.o:0000000000000008 D prov_state
NM
core_judge "$W" symbol-audit "$TMPROOT/nm-data.log"
eqj "V-22⑨ audit: data symbol across plugins ⇒ failed=1" "$JUDGE_RESP" "data.failed" "1"
eqj "V-22⑨ audit: code is BRV-SRC-0002" "$JUDGE_RESP" "diagnostics.0.code" "BRV-SRC-0002"
# A weak symbol is not evidence: every descriptor defines `br_plugin_gen_total` weak.
cat >"$TMPROOT/nm-weak.log" <<'NM'
build/lib/provider/cons.a:cons.o:                 w br_plugin_gen_total
build/lib/provider/prov.a:prov.o:0000000000000000 V br_plugin_gen_total
NM
core_judge "$W" symbol-audit "$TMPROOT/nm-weak.log"
eqj "V-22⑨ audit: weak symbols are ignored ⇒ failed=0" "$JUDGE_RESP" "data.failed" "0"

# ⑩ the pipeline: `brickie build` always plans the archives + the audit, and the link consumes the
# audit verdict (a red audit must stop the image).
W="$(cp_fx build)"
run_json "$W" build --dry-run
eq "V-22⑩ build dry-run exit code 0" "0" "$RC"
"$PY" - "$JQ_RESP" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
steps = d["steps"]
archives = [s for s in steps if s["kind"] == "archive"]
audits = [s for s in steps if s["kind"] == "audit"]
link = [s for s in steps if s["kind"] == "link"]
assert archives, "no archive steps"
assert all(a["argv"][1] == "rcsD" and a["argv"][2].startswith("build/lib/") for a in archives), archives
assert len(audits) == 1, audits
a = audits[0]
assert a["judge"] == "symbol-audit", a
assert "-A" in a["argv"] and "-g" in a["argv"], a
assert "build/gen/symbol-audit.txt" in link[0]["consumes"], link[0]["consumes"]
for s in archives:
    assert s["group"] < a["group"] < link[0]["group"], (s["group"], a["group"], link[0]["group"])
print("plan ok: %d archives + 1 audit before link" % len(archives))
PYX
eq "V-22⑩ plan has archives + audit before link" "0" "$?"

# ⑪ A-2 survives the change: an APP may not include `<br/core/**>` directly, whatever it declares
# (the APP's only door is the Interface layer). Needs a core plugin in the tree so the header
# resolves -- the APP rule is about the consumer's class, not about the declaration graph.
W="$(new_work)"; mk_discipline_tree "$W"
mkdir -p "$W/core/c_core/src" "$W/core/c_core/include/br/core" "$W/app/hsm/src"
cat >"$W/core/c_core/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "core/c_core"
plugin_type = "core"
api_type = "native"
lang = "c"
phase = "early"
version = "0.2.0.0"
[build]
sources = ["src/*.c"]
includes = ["include"]
TOML
printf '#define BR_NULL ((void *)0)\nvoid br_log_write(int level, const char *fmt);\n' \
    >"$W/core/c_core/include/br/core/br_log.h"
printf '#include <br/core/br_log.h>\nvoid br_log_write(int level, const char *fmt) { (void)level; (void)fmt; }\n' \
    >"$W/core/c_core/src/log.c"
cat >"$W/app/hsm/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "app/hsm"
plugin_type = "app"
api_type = "native"
lang = "c"
phase = "app"
version = "0.1.0.0"
[build]
sources = ["src/*.c"]
includes = ["include"]
TOML
printf '#include <br/core/br_log.h>\nint hsm_start(void) { return 0; }\n' >"$W/app/hsm/src/main.c"
run_json "$W" check --deps
eq "V-22⑪ APP includes a core header ⇒ exit code 1" "1" "$RC"
eqj "V-22⑪ code is BRV-MF-0001 (A-2)" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
contains "V-22⑪ message says the APP must not go below the iface layer" 'iface layer' "$(cat "$ERR")"

# ⑦ the real tree is clean under the new scope (the discipline is enforced, not aspirational)
run_json "$REPO_ROOT" check --src
eq "V-22⑦ brickOS tree: src scope exit code 0" "0" "$RC"
eqj "V-22⑦ brickOS tree: src scope errors=0" "$JQ_RESP" "data.summary.errors" "0"

# ---- V-23 HAL layer (ADR-0039): `hal` is a peer of `ability`, `ability` selects it
section "V-23 hal layer (BRV-TAX-0025 + direction table)"
mk_layers() { # mk_layers <work>: core + runtime + one ability + one hal brick
    local w="$1"
    mkdir -p "$w"/{core/c_core,runtime/soc,provider/impl,hal/iface}/src
    cat >"$w/core/c_core/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "core/c_core"
plugin_type = "core"
api_type = "native"
lang = "c"
phase = "early"
version = "0.1.0.0"
[build]
sources = ["src/*.c"]
TOML
    printf 'int core_probe(void);\nint core_probe(void){return 0;}\n' >"$w/core/c_core/src/a.c"
    cat >"$w/runtime/soc/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "runtime/soc"
plugin_type = "runtime"
api_type = "native"
lang = "c"
phase = "core"
version = "0.1.0.0"
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[[dep]]
name = "hal/iface"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
TOML
    printf 'int plat_probe(void);\nint plat_probe(void){return 0;}\n' >"$w/runtime/soc/src/a.c"
    cat >"$w/hal/iface/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "hal/iface"
plugin_type = "hal"
api_type = "native"
lang = "c"
phase = "early"
version = "0.1.0.0"
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
TOML
    printf 'int hal_probe(void);\nint hal_probe(void){return 0;}\n' >"$w/hal/iface/src/a.c"
    cat >"$w/provider/impl/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "provider/impl"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[[dep]]
name = "hal/iface"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
TOML
    printf 'int impl_probe(void);\nint impl_probe(void){return 0;}\n' >"$w/provider/impl/src/a.c"
    cat >"$w/product.toml" <<'TOML'
schema = 1
[product]
name = "t"
version = "0.1.0.0"
stage = "dev"
app = { brick = "", requires = {} }
runtime = { brick = "runtime/soc", requires = {} }
core = { brick = "core/c_core", requires = {} }
[[product.ability]]
a = { brick = "provider/impl", requires = {} }
TOML
}

# ① the happy path: runtime -> hal and ability -> hal are both legal
W="$(new_work)"; mk_layers "$W"
run_json "$W" check
eq "V-23① runtime→hal and ability→hal ⇒ exit code 0" "0" "$RC"
eqj "V-23① summary errors=0" "$JQ_RESP" "data.summary.errors" "0"

# ② `hal -> ability` is forbidden (the infrastructure must not depend on an implementation)
W="$(new_work)"; mk_layers "$W"
"$PY" - "$W/hal/iface/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace("[build]",
    '[[dep]]\nname = "provider/impl"\nrange = ">=0.1.0"\nkind = "runtime"\n\n[build]'))
PYX
run_json "$W" check --deps
eq "V-23② hal→ability ⇒ exit code 1" "1" "$RC"
contains "V-23② message names the hal row" 'plugin_type = hal' "$(cat "$ERR")"

# ③ namespace and plugin_type must agree (BRV-TAX-0025): `hal/` cannot be an ability
W="$(new_work)"; mk_layers "$W"
"$PY" - "$W/hal/iface/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('plugin_type = "hal"', 'plugin_type = "ability"')
                              .replace('phase = "early"', 'subkind = "service"\nphase = "late"'))
PYX
run_json "$W" check
eq "V-23③ hal/ declared as ability ⇒ exit code 1" "1" "$RC"
grep_at_least "V-23③ code is BRV-TAX-0025" 'BRV-TAX-0025' "$ERR"
# …and the reverse: a `provider/` name declaring plugin_type = "hal"
W="$(new_work)"; mk_layers "$W"
"$PY" - "$W/provider/impl/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('plugin_type = "ability"', 'plugin_type = "hal"')
                              .replace('subkind = "service"\n', ''))
PYX
run_json "$W" check
# A shape error outranks a red verdict (BRV-D9: exit 2 > 1): the phase/subkind combination is
# shape-invalid for the new type, so the aggregate exit code is 2 even though BRV-TAX-0025 is also
# reported (the grep below proves the semantic code is there, not just the shape one).
eq "V-23③ provider/ declared as hal ⇒ exit code 2 (shape outranks red)" "2" "$RC"
grep_at_least "V-23③ code is BRV-TAX-0025 (reverse)" 'BRV-TAX-0025' "$ERR"

# ④ the `framework` subkind is retired: it is a shape error now
W="$(new_work)"; mk_layers "$W"
"$PY" - "$W/provider/impl/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('subkind = "service"', 'subkind = "framework"'))
PYX
run_json "$W" check
eq "V-23④ subkind=framework ⇒ exit code 2 (shape)" "2" "$RC"
contains "V-23④ message lists the legal subkinds" "scheduler', 'io', 'fs', 'service'" "$(cat "$ERR")"
grep_none "V-23④ 'framework' is no longer in the legal subkind list" "'scheduler', 'framework'" "$ERR"

# ⑤ the real tree: the HAL layer exists and every brick declares the type its namespace implies
run_json "$REPO_ROOT" show bricks
eq "V-23⑤ brickOS closure exit code 0" "0" "$RC"
EQ_HAL="$(jqget "$JQ_RESP" 'data.totals.by_type.hal')"
eq "V-23⑤ the closure has HAL bricks" "9" "$EQ_HAL"
EQ_CORE_UNITS="$(grep -c '^\[\[export\]\]' "$REPO_ROOT/core/c_core/brick.toml")"
eq "V-23⑤ core keeps 11 units after the split" "11" "$EQ_CORE_UNITS"
eq "V-23⑤ hal/int exports the interrupt units" "4" "$(grep -c '^\[\[export\]\]' "$REPO_ROOT/hal/int/brick.toml")"
eq "V-23⑤ hal/mm exports the memory units" "2" "$(grep -c '^\[\[export\]\]' "$REPO_ROOT/hal/mm/brick.toml")"
eq "V-23⑤ hal/sched exports the scheduling units" "3" "$(grep -c '^\[\[export\]\]' "$REPO_ROOT/hal/sched/brick.toml")"

# ---- V-24 naming convergence (ADR-0040): the target runtime vs. the provider namespace
section "V-24 runtime/provider naming"
mk_names() { # mk_names <work>: core + runtime + one hal + one provider
    local w="$1"
    mkdir -p "$w"/{core/c_core,runtime/soc,hal/iface,provider/impl}/src
    cat >"$w/core/c_core/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "core/c_core"
plugin_type = "core"
api_type = "native"
lang = "c"
phase = "early"
version = "0.1.0.0"
[build]
sources = ["src/*.c"]
TOML
    printf 'int core_probe(void)\n{\n    return 0;\n}\n' >"$w/core/c_core/src/a.c"
    cat >"$w/runtime/soc/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "runtime/soc"
plugin_type = "runtime"
api_type = "native"
lang = "c"
phase = "core"
version = "0.1.0.0"
[[res]]
kind = "ram"
used_kib = 4096
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[[dep]]
name = "hal/iface"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
[build.target]
arch = "aarch64"
cross = "aarch64-linux-gnu-"
linker_script = "src/link.ld"
TOML
    printf 'int rt_probe(void)\n{\n    return 0;\n}\n' >"$w/runtime/soc/src/a.c"
    printf 'SECTIONS { }\n' >"$w/runtime/soc/src/link.ld"
    cat >"$w/hal/iface/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "hal/iface"
plugin_type = "hal"
api_type = "native"
lang = "c"
phase = "early"
version = "0.1.0.0"
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
TOML
    printf 'int hal_probe(void)\n{\n    return 0;\n}\n' >"$w/hal/iface/src/a.c"
    cat >"$w/provider/impl/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "provider/impl"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[[dep]]
name = "core/c_core"
range = ">=0.1.0"
kind = "runtime"
[[dep]]
name = "hal/iface"
range = ">=0.1.0"
kind = "runtime"
[build]
sources = ["src/*.c"]
TOML
    printf 'int impl_probe(void)\n{\n    return 0;\n}\n' >"$w/provider/impl/src/a.c"
    cat >"$w/product.toml" <<'TOML'
schema = 1
[product]
name = "t"
version = "0.1.0.0"
stage = "dev"
app = { brick = "", requires = {} }
runtime = { brick = "runtime/soc", requires = {} }
core = { brick = "core/c_core", requires = {} }
[[product.ability]]
p = { brick = "provider/impl", requires = {} }
TOML
}

# ① the new names are accepted: runtime type + runtime/ namespace + provider/ namespace
W="$(new_work)"; mk_names "$W"
run_json "$W" check
eq "V-24① runtime + provider names ⇒ exit code 0" "0" "$RC"
eqj "V-24① summary errors=0" "$JQ_RESP" "data.summary.errors" "0"
# …and `provider/` derives subkind=service, so its phase is LATE
run_json "$W" check
eq "V-24① provider/ derives subkind service (phase late accepted)" "0" "$RC"

# ② the old names are gone: `plugin_type = "platform"` is a shape error
W="$(new_work)"; mk_names "$W"
"$PY" - "$W/runtime/soc/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('plugin_type = "runtime"', 'plugin_type = "platform"'))
PYX
run_json "$W" check
eq "V-24② plugin_type=platform ⇒ exit code 2 (shape)" "2" "$RC"
contains "V-24② message lists the legal types" "runtime" "$(cat "$ERR")"

# ③ the old namespaces are gone: `service/` no longer means an ability
W="$(new_work)"; mk_names "$W"
mkdir -p "$W/service/old/src"
cat >"$W/service/old/brick.toml" <<'TOML'
schema = 1
[plugin]
name = "service/old"
plugin_type = "ability"
api_type = "native"
subkind = "service"
lang = "c"
phase = "late"
version = "0.1.0.0"
[build]
sources = ["src/*.c"]
TOML
printf 'int old_probe(void)\n{\n    return 0;\n}\n' >"$W/service/old/src/a.c"
"$PY" - "$W/product.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().replace('p = { brick = "provider/impl"', 'p = { brick = "service/old"'))
PYX
run_json "$W" check
eq "V-24③ service/ namespace ⇒ red (BRV-TAX-0013 warns, shape stays ok)" "0" "$RC"
contains "V-24③ warning names the recommended namespaces" "provider" "$(cat "$ERR")"

# ④ the runtime plugin owns [build.target]; an ability may not declare it
W="$(new_work)"; mk_names "$W"
"$PY" - "$W/provider/impl/brick.toml" <<'PYX'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text() + '\n[build.target]\narch = "aarch64"\n')
PYX
run_json "$W" check
# Shape error, not a red verdict: which plugin may own `[build.target]` is a declaration-surface
# shape rule (exit 2), unlike the semantic codes that return 1.
eq "V-24④ [build.target] on an ability ⇒ exit code 2 (shape)" "2" "$RC"
contains "V-24④ message names the runtime type" "runtime" "$(cat "$ERR")"

# ⑤ the real tree: the two namespaces exist, the old ones do not
run_json "$REPO_ROOT" show bricks
eq "V-24⑤ brickOS closure exit code 0" "0" "$RC"
eqj "V-24⑤ the closure has exactly one runtime plugin" "$JQ_RESP" "data.totals.by_type.runtime" "1"
eq "V-24⑤ the runtime plugin is runtime/qemu-aarch64" "runtime/qemu-aarch64" \
    "$(find "$REPO_ROOT/runtime" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -1 | sed 's|^|runtime/|')"
eq "V-24⑤ the old platform/ directory is gone" "gone" \
    "$([ -d "$REPO_ROOT/platform" ] && echo here || echo gone)"
eq "V-24⑤ the old service/ directory is gone" "gone" \
    "$([ -d "$REPO_ROOT/service" ] && echo here || echo gone)"
eq "V-24⑤ provider/ holds the seven concrete implementations" "7" \
    "$(find "$REPO_ROOT/provider" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')"
eq "V-24⑤ the contract headers moved to br/runtime/" "4" \
    "$(find "$REPO_ROOT/runtime/qemu-aarch64/include/br/runtime" -name '*.h' | wc -l | tr -d ' ')"

# ---- N-1 negative acceptance: help surface
section "N-1 no undelivered §5.4 command appears in --help"
help_commands() { # <help output file> ⇒ comma-separated subcommand names (sorted, deduped)
    awk '/^positional arguments:/{f=1;next} /^options:/{f=0} f' "$1" \
        | grep -oE '^    [a-z][a-z0-9-]*' | tr -d ' ' | sort -u | paste -sd, -
}
W="$(new_work)"
run_brickie "$W" --help
eq "N-1 top-level help exit code 0" "0" "$RC"
eq "N-1 top-level command surface = 14 groups (8 composition + 6 build family)" \
    "build,check,clean,dep,disasm,gen,iface,init,new,run,show,size,test,ver" "$(help_commands "$OUT")"
for c in api-dump verify dbg pack add env; do
    eq "N-1 top-level help excludes $c" "0" \
        "$(help_commands "$OUT" | tr ',' '\n' | grep -cx "$c" || true)"
done
run_brickie "$W" iface --help
eq "N-1 iface subcommand surface" \
    "deprecate,diff,freeze,list,publish,refreeze,show,status,undeprecate,unfreeze" \
    "$(help_commands "$OUT")"
eq "N-1 no check under iface (v0.2 work)" "0" \
    "$(help_commands "$OUT" | tr ',' '\n' | grep -cx 'check' || true)"
grep_none "N-1 iface help never says 'iface check'" 'iface check' "$OUT"
run_brickie "$W" dep --help
eq "N-1 dep subcommand surface" "add,closure,graph,index,rm,tree,why" "$(help_commands "$OUT")"
run_brickie "$W" ver --help
eq "N-1 ver subcommand surface" "bump,show" "$(help_commands "$OUT")"
run_brickie "$W" show --help
eq "N-1 show subcommand surface" "brick-init-flow,bricks" "$(help_commands "$OUT")"

# ---- N-2 negative acceptance: undelivered commands
section "N-2 undelivered §5.4 commands called directly ⇒ exit code 2"
W="$(new_work)"
for c in api-dump verify dbg pack add env; do
    run_brickie "$W" "$c"
    eq "N-2 brickie $c ⇒ exit code 2" "2" "$RC"
done
run_brickie "$W" show nope
eq "N-2 brickie show <unknown> ⇒ exit code 2" "2" "$RC"
run_brickie "$W" iface check provider/x#x
eq "N-2 brickie iface check ⇒ exit code 2" "2" "$RC"
run_brickie "$W" frobnicate
eq "N-2 unknown subcommand ⇒ exit code 2" "2" "$RC"
run_brickie "$W"
eq "N-2 no subcommand ⇒ exit code 2" "2" "$RC"

# ---- N-3 machine-file ownership
section "N-3 machine files are written only by machines / hand edits go red"
W="$(cp_fx publish)"
run_brickie "$W" iface publish provider/a#a
PLUGIN_BEFORE="$(sha256sum "$W/provider/a/brick.toml")"
run_brickie "$W" iface publish provider/a#a
eq "N-3 publish does not rewrite brick.toml (§9.1.1 rule 1)" "$PLUGIN_BEFORE" \
    "$(sha256sum "$W/provider/a/brick.toml")"
grep_at_least "N-3 the snapshot is a machine file (carries the generated marker)" 'brickie:generated' "$W/api/iface/provider/a/a.toml"
grep_at_least "N-3 the lock is a machine file" 'brickie:generated' "$W/brickie.lock"
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:1111111111111111111111111111111111111111111111111111111111111111"/' \
    "$W/api/iface/provider/a/a.toml"
run_json "$W" iface status provider/a#a
eq "N-3 hand-edited snapshot ⇒ status exit code 1" "1" "$RC"
eqj "N-3 hand-edited snapshot ⇒ consistent=false" "$JQ_RESP" "data.consistent" "false"

# ---- N-4 protocol snapshots
section "N-4 --json structured snapshots (protocol version locked)"
W="$(cp_fx showcase)"
run_brickie "$W" --version --json
cp "$OUT" "$TMPROOT/s-version.json"
snapj "version" "$TMPROOT/s-version.json"
run_json "$W" check
snapj "check-clean" "$JQ_RESP"
run_json "$W" dep tree
snapj "dep-tree" "$JQ_RESP"
run_json "$W" dep closure
snapj "dep-closure" "$JQ_RESP"
run_json "$W" show bricks
snapj "show-bricks" "$JQ_RESP"
run_json "$W" show brick-init-flow
snapj "show-init-flow" "$JQ_RESP"
run_json "$W" iface list
snapj "iface-list" "$JQ_RESP"
run_json "$W" iface show provider/crypto#crypto
snapj "iface-show" "$JQ_RESP"
W="$(cp_fx cycle)"
run_json "$W" check
snapj "check-cycle" "$JQ_RESP"
W="$(new_work)"
run_json "$W" new ability provider/crypto --subkind service
snapj "new-json" "$JQ_RESP"
W="$(cp_fx publish)"
run_json "$W" iface publish provider/a#a
snapj "publish" "$JQ_RESP"
run_json "$W" iface status provider/a#a
snapj "iface-status" "$JQ_RESP"

# ---- summary
printf '\n=== passed %s / failed %s ===\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '\nFailure details:\n'
    cat "$FAILLOG"
    exit 1
fi
exit 0
