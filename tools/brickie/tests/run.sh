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
for pair in "app:app/hsm:" "interface:iface/posix:" "ability:service/crypto:--subkind service" "platform:platform/qemu-aarch64:" "core:core/c_core:";
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
run_brickie "$W" new ability service/crypto --subkind service
run_brickie "$W" new platform platform/qemu-aarch64
run_brickie "$W" new core core/c_core
run_json "$W" check
eq "V-1 five kinds one tree check exit code 0" "0" "$RC"
eqj "V-1 five kinds one tree closure has 5 plugins" "$JQ_RESP" "data.summary.closure.plugins" "5"

# ---- V-2 generated artifacts idempotent
section "V-2 gen idempotence / --check byte-identical"
W="$(new_work)"
run_brickie "$W" new ability service/crypto --subkind service
H1="$(tree_hash "$W")"
run_brickie "$W" gen
eq "V-2 second gen exit code 0" "0" "$RC"
grep_at_least "V-2 second gen reports 'unchanged:'" 'unchanged:' "$OUT"
H2="$(tree_hash "$W")"
eq "V-2 two gen runs byte-identical" "$H1" "$H2"
run_brickie "$W" gen --check
eq "V-2 gen --check consistent exit code 0" "0" "$RC"
grep_at_least "V-2 gen --check text" 'byte-identical to recomputation' "$OUT"
printf '\n/* hand edit */\n' >>"$W/build/gen/service/crypto/plugin_desc.c"
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
grep_at_least "V-3 text reports the full cycle path" 'service/a → service/b → service/c → service/a' "$ERR"
eqj "V-3 check --json contains the cycle diagnostic" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
run_json "$W" dep closure
eqj "V-3 closure --json exit_code=1" "$JQ_RESP" "exit_code" "1"
eqjc "V-3 closure --json cycles count" "$JQ_RESP" "data.cycles" "1"
eqj "V-3 cycles[0].path complete (first == last)" "$JQ_RESP" "data.cycles.0.path" \
    '["service/a", "service/b", "service/c", "service/a"]'
eqjc "V-3 cycles[0].edges structured edge list has 3 entries" "$JQ_RESP" "data.cycles.0.edges" "3"
eqj "V-3 cycles[0].edges[0] shape {from,to}" "$JQ_RESP" "data.cycles.0.edges.0.from" "service/a"
eqj "V-3 cycles[0].edges[0].to" "$JQ_RESP" "data.cycles.0.edges.0.to" "service/b"

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
run_brickie "$W" iface publish service/crypto#crypto
eq "V-4 first publish = file creation, no segment bump (S-8)" "0.1.0.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-4(f) unfreeze enters the window" "0" "$RC"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-4(f) empty-unfreeze refreeze exit code 0" "0" "$RC"
eq "V-4(f) empty unfreeze ⇒ all four segments stay put" "0.1.0.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/service/crypto/brick.toml"
run_brickie "$W" iface refreeze service/crypto#crypto --note rfc.md
eq "V-4(a) refreeze exit code 0" "0" "$RC"
eq "V-4(a) edited frozen entry ⇒ only COMPAT_GEN+1 (MAJOR not dragged in)" "1.1.0.0" \
    "$(segments_of "$W" service/crypto#crypto)"
python3 - "$W/service/crypto/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                        'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1)
p.write_text(s)
PY
run_brickie "$W" iface publish service/crypto#crypto
eq "V-4(b) added entry publish exit code 0" "0" "$RC"
eq "V-4(b) addition ⇒ COMPAT_GEN stays put + MINOR+1" "1.1.1.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule major
eq "V-4(c) ver bump major exit code 0" "0" "$RC"
eq "V-4(c) only MAJOR+1, COMPAT_GEN stays put" "1.2.0.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule minor
eq "V-4(d) minor ⇒ MINOR+1" "1.2.1.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule revise
eq "V-4(e) revise ⇒ REVISE+1" "1.2.1.1" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule compat_gen
eq "V-4 COMPAT_GEN is not a rule (usage error 2)" "2" "$RC"

# ---- V-5 interface publishing
section "V-5 publish output + idempotence + --check independent recompute"
W="$(cp_fx publish)"
run_json "$W" iface publish service/a#a
eq "V-5 first publish exit code 0" "0" "$RC"
eqj "V-5 first publish noop=false" "$JQ_RESP" "data.noop" "false"
eqjc "V-5 3 files written (snapshot+CHANGELOG+lock)" "$JQ_RESP" "written" "3"
for f in "api/iface/service/a/a.toml" "api/iface/CHANGELOG.md" "brickie.lock"; do
    eq "V-5 artifact in place $f" "yes" "$([ -f "$W/$f" ] && echo yes || echo no)"
done
grep_at_least "V-5 snapshot header carries NOT_ABI" 'NOT_ABI' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 snapshot header carries hash_scope" 'hash_scope = "decl"' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 snapshot header carries truth" 'truth = "decl"' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 snapshot carries the brickie:generated marker" 'brickie:generated' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 CHANGELOG keyed by version segment, no dates" '^## service/a#a v0\.1\.0\.0' "$W/api/iface/CHANGELOG.md"
eq "V-5 CHANGELOG carries no dates (byte-reproducible)" "0" \
    "$(grep -cE '[0-9]{4}-[0-9]{2}-[0-9]{2}' "$W/api/iface/CHANGELOG.md" || true)"
grep_at_least "V-5 lock carries the brickie:generated marker" 'brickie:generated' "$W/brickie.lock"
H1="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
run_json "$W" iface publish service/a#a
eq "V-5 surface unchanged ⇒ no-op exit code 0" "0" "$RC"
eqj "V-5 surface unchanged ⇒ noop=true" "$JQ_RESP" "data.noop" "true"
eqjc "V-5 surface unchanged ⇒ writes no files" "$JQ_RESP" "written" "0"
H2="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
eq "V-5 no-op is byte-identical" "$H1" "$H2"
run_json "$W" iface status service/a#a --check
eq "V-5 status --check consistent exit code 0" "0" "$RC"
eqj "V-5 status --check consistent=true" "$JQ_RESP" "data.consistent" "true"
run_json "$W" iface publish service/a#a --check
eq "V-5 publish --check exit code 0" "0" "$RC"
eqjc "V-5 publish --check writes nothing" "$JQ_RESP" "written" "0"
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000"/' \
    "$W/api/iface/service/a/a.toml"
run_json "$W" iface status service/a#a --check
eq "V-5 hand-edited snapshot ⇒ status --check exit code 1" "1" "$RC"
eqj "V-5 hand-edited snapshot ⇒ consistent=false" "$JQ_RESP" "data.consistent" "false"

# ---- V-6 impact report
section "V-6 impact report for a frozen-entry edit / --note hard gate"
W="$(cp_fx dependents)"
printf 'RFC: signature change\n' >"$W/rfc.md"
run_brickie "$W" iface publish service/crypto#crypto
eq "V-6 file-creation publish exit code 0" "0" "$RC"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-6 unfreeze exit code 0" "0" "$RC"
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/service/crypto/brick.toml"
run_json "$W" iface diff service/crypto#crypto
eq "V-6 diff exit code 0" "0" "$RC"
eqj "V-6 changes[0].kind=CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-6 change set takes the strictest: compat_gen_changed=true" "$JQ_RESP" "data.compat_gen_changed" "true"
eqjc "V-6 2 direct dependents" "$JQ_RESP" "data.dependents.direct" "2"
eqjc "V-6 3 transitive dependents" "$JQ_RESP" "data.dependents.transitive" "3"
eqjc "V-6 1 unsatisfied dependent" "$JQ_RESP" "data.dependents.unsatisfied" "1"
eqj "V-6 unsatisfied dependent named + reason" "$JQ_RESP" "data.dependents.unsatisfied.0.plugin" "service/consumer"
grep_at_least "V-6 mismatch reason explains the compat_gen change" 'compat_gen changed from 0 to 1' "$OUT"
run_json "$W" iface publish service/crypto#crypto
eq "V-6 requires unfreeze but --note is missing ⇒ exit code 1" "1" "$RC"
grep_at_least "V-6 missing --note reports BRV-MF-0001" 'BRV-MF-0001' "$ERR"
run_json "$W" iface publish service/crypto#crypto --note rfc.md
eq "V-6 publish with --note exit code 0" "0" "$RC"
eqj "V-6 COMPAT_GEN bumped after publish" "$JQ_RESP" "data.compat_gen" "1"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-6 publish in window consumed the baseline ⇒ refreeze is an empty unfreeze" "0" "$RC"
# Use unfreeze here: an unchanged-surface publish is a no-op and skips the --note gate.
run_brickie "$W" iface unfreeze service/crypto#crypto --note no-such.md
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
    '["app/hsm", "core/c_core", "iface/hsm-api", "platform/qemu-aarch64", "sched/coop", "service/crypto"]'
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
eqj "V-7 total RAM (platform capacity not in Σ, S-14)" "$JQ_RESP" "data.totals.ram_kib" "48"
eqj "V-7 total stack" "$JQ_RESP" "data.totals.stack_kib" "4"
eqj "V-7 platform capacity reported separately" "$JQ_RESP" "data.totals.platform_capacity.ram_kib" "8192"
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
eq "V-8 sched-coop → platform is not a false positive, exit code 0" "0" "$RC"
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
run_seed "$W" new ability service/crypto --subkind service
eq "V-9 loop ① new exit code 0" "0" "$RC"
run_seed "$W" check
eq "V-9 loop ② check exit code 0" "0" "$RC"
run_seed "$W" dep closure
eq "V-9 loop ③ dep closure exit code 0" "0" "$RC"
run_seed "$W" iface publish service/crypto#crypto
eq "V-9 loop ④ iface publish exit code 0" "0" "$RC"
run_seed "$W" iface status service/crypto#crypto --check
eq "V-9 loop ⑤ iface status --check exit code 0" "0" "$RC"
run_seed "$W" gen --check
eq "V-9 loop ⑥ gen --check exit code 0" "0" "$RC"
eq "V-9 the whole loop is green with no compiler present" "yes" \
    "$([ -f "$W/api/iface/service/crypto/crypto.toml" ] && echo yes || echo no)"
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
run_json "$W" iface show service/consumer#consumer
eq "V-10③ iface show exit code 0" "0" "$RC"
eq "V-10③ registered as info (NOT_ABI + requires_iface, two entries)" "2" \
    "$(grep -c '"severity": "info"' "$JQ_RESP" || true)"
grep_at_least "V-10③ message says v0.1 does not scan" 'does not scan' "$JQ_RESP"
grep_at_least "V-10③ span = compat.requires_iface[0]" 'compat.requires_iface\[0\]' "$JQ_RESP"
W3="$(cp_fx req-iface)"
sed -i '/^compat_gen = 0$/d' "$W3/service/consumer/brick.toml"
run_json "$W3" check
eq "V-10① missing compat_gen ⇒ exit code 2" "2" "$RC"
eq "V-10① reports BRV-MF-0001" "1" "$(grep -c 'BRV-MF-0001' "$ERR" || true)"
grep_at_least "V-10① span names compat_gen" 'compat.requires_iface\[0\].compat_gen' "$ERR"
W3="$(cp_fx req-iface)"
sed -i '/^api_iface  = "native"$/d' "$W3/service/consumer/brick.toml"
run_json "$W3" check
eq "V-10① missing api_iface ⇒ exit code 2" "2" "$RC"

# ---- V-11 hash semantics
section "V-11 hash_scope/truth paired / NOT_ABI / hash cross-check"
W="$(cp_fx truth)"
run_json "$W" iface show service/x#x
eqj "V-11① --json carries hash_scope" "$JQ_RESP" "data.hash_scope" "decl"
eqj "V-11① --json carries truth" "$JQ_RESP" "data.truth" "decl"
eqj "V-11③ --json carries not_abi" "$JQ_RESP" "data.not_abi" "true"
grep_at_least "V-11③ text folds in the NOT_ABI hint" 'NOT_ABI' "$OUT"
Ws="$(cp_fx truth)"
sed -i 's/^hash_scope = "decl"$/hash_scope = "sym"/' "$Ws/service/x/brick.toml"
run_json "$Ws" check
eq "V-11② hash_scope=sym ⇒ exit code 1" "1" "$RC"
eq "V-11② reports BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
Wt="$(cp_fx truth)"
sed -i 's/^truth      = "decl"$/truth      = "header"/' "$Wt/service/x/brick.toml"
run_json "$Wt" check
eq "V-11② truth=header ⇒ exit code 1" "1" "$RC"
eq "V-11② reports BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
Wp="$(cp_fx publish)"
run_brickie "$Wp" iface publish service/a#a
sed -i 's/^hash         = ""$/hash         = "sha256:deadbeef"/' "$Wp/service/a/brick.toml"
run_json "$Wp" iface status service/a#a
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
run_brickie "$W" iface publish service/a#a
python3 - "$W/service/a/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "g"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface publish service/a#a --set 0.0.9.0
eq "V-13 --set below the current version ⇒ exit code 1" "1" "$RC"
eq "V-13 reports BRV-VER-0007" "1" "$(grep -c 'BRV-VER-0007' "$ERR" || true)"
run_json "$W" iface publish service/a#a --set 0.1.9.0
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
run_brickie "$W" iface publish service/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["A", "B", "C"]/' "$W/service/enums/brick.toml"
run_json "$W" iface diff service/enums#enums
eqj "V-15 enum appended at the end ⇒ EXTENDED" "$JQ_RESP" "data.changes.0.kind" "EXTENDED"
eqj "V-15 EXTENDED needs no unfreeze ⇒ compat_gen unchanged" "$JQ_RESP" "data.compat_gen_changed" "false"
eqj "V-15 EXTENDED verdict=green" "$JQ_RESP" "data.verdict" "green"
run_json "$W" iface publish service/enums#enums
eq "V-15 EXTENDED publish exit code 0" "0" "$RC"
eqj "V-15 EXTENDED ⇒ MINOR+1" "$JQ_RESP" "data.version" "0.1.1.0"
W="$(cp_fx iface-enum)"
run_brickie "$W" iface publish service/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/service/enums/brick.toml"
run_json "$W" iface diff service/enums#enums
eqj "V-15 enum reordering ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 enum reordering ⇒ compat_gen_changed" "$JQ_RESP" "data.compat_gen_changed" "true"
eqj "V-15 not unfrozen ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish service/enums#enums
eq "V-15 publish without unfreeze ⇒ exit code 1" "1" "$RC"
W="$(cp_fx iface-struct)"
run_brickie "$W" iface publish service/st#st
sed -i 's/^layout = "{ void\* p; }"$/layout = "{ void* p; int n; }"/' "$W/service/st/brick.toml"
run_json "$W" iface diff service/st#st
eqj "V-15 struct field added ⇒ CHANGED(D22)" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 struct field added ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish service/st#st
eq "V-15 struct field added without unfreeze ⇒ exit code 1" "1" "$RC"
W="$(cp_fx iface-ops)"
run_brickie "$W" iface publish service/sv#sv
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/service/sv/brick.toml"
run_json "$W" iface diff service/sv#sv
eqj "V-15 ops slot added ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 ops slot added ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
W="$(cp_fx iface-macro)"
run_brickie "$W" iface publish service/crypto#crypto
python3 - "$W/service/crypto/brick.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface diff service/crypto#crypto
eqj "V-15 added entry ⇒ ADDED" "$JQ_RESP" "data.changes.0.kind" "ADDED"
eqj "V-15 ADDED needs no unfreeze ⇒ verdict=green" "$JQ_RESP" "data.verdict" "green"
eqj "V-15 ADDED ⇒ compat_gen unchanged" "$JQ_RESP" "data.compat_gen_changed" "false"

# ---- V-16 unfreeze window
section "V-16 unfreeze gate / release block / empty unfreeze leaves all four segments alone"
W="$(cp_fx unfreeze)"
printf 'RFC\n' >"$W/rfc.md"
run_brickie "$W" iface unfreeze service/crypto#crypto
eq "V-16 unfreeze without --note ⇒ nonzero (argparse usage error 2)" "2" "$RC"
run_brickie "$W" iface publish service/crypto#crypto
eq "V-16 file-creation publish exit code 0" "0" "$RC"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-16 with --note, entering the window, exit code 0" "0" "$RC"
run_json "$W" iface status service/crypto#crypto
eqj "V-16 entering the window does not change the surface (snapshot_hash == recomputed_hash)" "$JQ_RESP" \
    "data.snapshot_hash" "$(jqget "$JQ_RESP" data.recomputed_hash)"
run_json "$W" check --profile release
eq "V-16 release inside the window ⇒ exit code 1" "1" "$RC"
eq "V-16 reports BRV-IFACE-0009" "1" "$(grep -c 'BRV-IFACE-0009' "$ERR" || true)"
run_json "$W" check --profile dev
eq "V-16 dev inside the window ⇒ exit code 0" "0" "$RC"
before_cg="$(jqget "$(run_json "$W" ver show service/crypto#crypto; echo "$JQ_RESP")" data.version)"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-16 empty-unfreeze refreeze exit code 0" "0" "$RC"
run_json "$W" ver show service/crypto#crypto
eq "V-16 empty unfreeze ⇒ all four segments stay put" "$before_cg" "$(jqget "$JQ_RESP" data.version)"
eqj "V-16 empty unfreeze leaves compat_gen alone" "$JQ_RESP" "data.segments.compat_gen" "0"

# ---- V-17 hash input completeness
section "V-17 macro values / service ops / enum order must all enter the hash"
hash_of() { # hash_of <work> <id>
    run_json "$1" iface show "$2"
    jqget "$JQ_RESP" data.declaration_hash
}
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" service/crypto#crypto)"
sed -i 's/^value = "16"$/value = "4096"/' "$W/service/crypto/brick.toml"
H1="$(hash_of "$W" service/crypto#crypto)"
eq "V-17 BR_MAX 16→4096 yields a different hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
contains "V-17 hash is sha256:..." "sha256:" "$H0"
W="$(cp_fx iface-ops)"
H0="$(hash_of "$W" service/sv#sv)"
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/service/sv/brick.toml"
H1="$(hash_of "$W" service/sv#sv)"
eq "V-17 adding a service ops slot yields a different hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
W="$(cp_fx iface-enum)"
H0="$(hash_of "$W" service/enums#enums)"
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/service/enums/brick.toml"
H1="$(hash_of "$W" service/enums#enums)"
eq "V-17 enum reordering is seen by the hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" service/crypto#crypto)"
python3 - "$W/service/crypto/brick.toml" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
head, tail = s.split("[build]", 1)
blocks = re.findall(r"\[\[export\.entries\]\][^\[]*", head)
assert len(blocks) == 2, blocks
out = head.replace(blocks[0], "@@B0@@").replace(blocks[1], "@@B1@@")
out = out.replace("@@B0@@", blocks[1].rstrip() + "\n\n").replace("@@B1@@", blocks[0].rstrip() + "\n\n")
p.write_text(out + "[build]" + tail)
PY
run_json "$W" iface show service/crypto#crypto
eq "V-17 declaration order does not affect the hash (IFACE-IR rule 1)" "$H0" "$(jqget "$JQ_RESP" data.declaration_hash)"
W="$(cp_fx publish)"
run_brickie "$W" iface publish service/a#a
S1="$(sha256sum "$W/api/iface/service/a/a.toml" "$W/brickie.lock" "$W/api/iface/CHANGELOG.md")"
W2="$(cp_fx publish)"
run_brickie "$W2" iface publish service/a#a
S2="$(sha256sum "$W2/api/iface/service/a/a.toml" "$W2/brickie.lock" "$W2/api/iface/CHANGELOG.md")"
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
    S why-hit         0 'dependency path'                data.found          true       dep why app/hsm service/crypto
    S why-miss        0 'dependency path not found'          data.found          false      dep why service/crypto app/hsm
    S dep-index       0 'reverse dependency index'            data.dependents     present    dep index
    S gen             0 'generated-file rebuild plan'              data.count          6          gen
    S gen-check       0 'byte-identical to recomputation'              data.count          6          gen --check
    S ver-show        0 'interface unit service/crypto#crypto' data.id     service/crypto#crypto ver show service/crypto
    S iface-list      0 'interface unit'                data.count          5          iface list
    S iface-show      0 'declaration hash'             data.not_abi        true       iface show service/crypto#crypto
    S iface-diff0     0 'interface change set'              data.id             present    iface diff service/crypto#crypto
    S iface-status0   1 'consistency inconsistent:'                  data.consistent     false      iface status service/crypto#crypto
    S iface-status-c0 1 'consistency inconsistent:'                  data.consistent     false      iface status service/crypto#crypto --check
    S iface-publish   0 'interface publish'                data.noop           false      iface publish service/crypto#crypto
    S iface-status1   0 'consistency consistent:'                    data.consistent     true       iface status service/crypto#crypto
    S iface-noop      0 'no-op'                  data.noop           true       iface publish service/crypto#crypto
    S iface-pchk      0 'no-op'                  data.noop           true       iface publish service/crypto#crypto --check
    S ver-bump        0 'version bump'                data.rule           revise     ver bump service/crypto#crypto --rule revise
    S iface-freeze    0 'promotion proposal pending'              data.proposal       present    iface freeze service/crypto#crypto --note rfc.md
    S iface-deprecate 0 'deprecate'                data.status         deprecated iface deprecate service/crypto#crypto --note rfc.md
    S iface-undep     0 'undeprecate'            data.status         frozen     iface undeprecate service/crypto#crypto --note rfc.md
    S iface-unfreeze  0 'enter window'                data.freeze_state   unfreezing iface unfreeze service/crypto#crypto --note rfc.md
    S iface-refreeze  0 'leave window'                data.freeze_state   frozen     iface refreeze service/crypto#crypto
    S dep-add         0 'dep add only prints a snippet'      data.found          true       dep add app/hsm iface/hsm-api --kind runtime
    S dep-rm          0 'dep rm only prints a snippet'       data.found          false      dep rm app/hsm service/nowhere

    local W0="$W"
    W="$(new_work)"
    S new  0 'skeleton generated' data.count 6 new ability service/svc --subkind service
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
grep_at_least "V-19④ reports both parties (irq)" 'irq `32` is declared by both `service/irq-a`.*and `service/irq-b`' "$ERR"
grep_at_least "V-19④ reports both parties (dma)" 'dma_channel `3` is declared by both `service/irq-a`.*and `service/irq-b`' "$ERR"
W="$(cp_fx priv-pool-ok)"
run_json "$W" check
eq "V-19 orthogonality table: pool lifecycle (alloc/free, granularity=pool) is P2 ⇒ legal, 0" "0" "$RC"
W="$(cp_fx priv-platform-p4)"
run_json "$W" check
eq "V-19 orthogonality table: map/protect are P4, platform may declare them ⇒ legal, 0" "0" "$RC"

# ---- V-20 show family (ADR-0036): closure view + runtime init flow
section "V-20 show family (bricks / brick-init-flow)"
W="$(cp_fx showcase)"
run_brickie "$W" show bricks
eq "V-20① show bricks exit code 0" "0" "$RC"
grep_at_least "V-20① lists the closure" 'product bricks .*6 plugins, 2 roots' "$OUT"
grep_at_least "V-20① marks a product root" 'app/hsm .*root:app' "$OUT"
grep_at_least "V-20① marks a closure member" 'platform/qemu-aarch64 .*dep' "$OUT"
grep_at_least "V-20① reports the budget it spends" 'product budget RAM 512 / stack 16' "$OUT"
run_json "$W" show bricks
eqj "V-20① roots counted" "$JQ_RESP" "data.totals.roots" "2"
eqj "V-20① plugins counted" "$JQ_RESP" "data.totals.plugins" "6"
eqj "V-20① root origin is recorded" "$JQ_RESP" "data.bricks.0.origin" "root"

run_brickie "$W" show brick-init-flow
eq "V-20② show brick-init-flow exit code 0" "0" "$RC"
grep_at_least "V-20② platform early_init is the first step" '0  *early  *early_init  *platform/qemu-aarch64' "$OUT"
grep_none "V-20② no APP early_init step" 'early  *early_init  *app/hsm' "$OUT"
grep_at_least "V-20② APP init comes after every LATE init" 'app/hsm  *APP init last' "$OUT"
grep_at_least "V-20② APP start is the last start step" 'app/hsm  *APP last: explicit rule' "$OUT"
grep_at_least "V-20② the IRQ hand-off is a step of its own" 'br_irq_cpu_enable\(\): after all init returned, before START' "$OUT"
grep_at_least "V-20② the order basis names the runtime sort" 'FIFO Kahn over kind=init edges' "$OUT"
run_json "$W" show brick-init-flow
eqj "V-20② four early steps (platform + 3, no APP)" "$JQ_RESP" "data.counts.early" "4"
eqj "V-20② exactly one IRQ step" "$JQ_RESP" "data.counts.irq" "1"
eqj "V-20② five start steps" "$JQ_RESP" "data.counts.start" "5"
eqj "V-20② exactly one APP-phase init" "$JQ_RESP" "data.counts.app" "1"
hasj "V-20② the runtime order is reported" "$JQ_RESP" "data.order"
eqj "V-20② the first step is the platform's early_init" "$JQ_RESP" "data.steps.0.hook" "early_init"
eqj "V-20② the first step names the platform" "$JQ_RESP" "data.steps.0.plugin" "platform/qemu-aarch64"
eqj "V-20② no phase divergence in a consistent tree" "$JQ_RESP" "data.counts.selftest" "0"

# The runtime order is not the name-tie-broken `dep closure` order: for this fixture they differ
# (section order puts platform before sched/coop, the name order does not), which is exactly why
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
eqj "V-B① source file count" "$JQ_RESP" "data.source_count" "7"
eqj "V-B① steps_total = 9 (7 compiles + link + objcopy)" "$JQ_RESP" "data.steps_total" "9"
eqjc "V-B① dry-run lists all steps" "$JQ_RESP" "data.steps" "9"
keysj "V-B① data.units[0] shape" "$JQ_RESP" "data.units.0" \
    "defines,dir,includes,name,plugin,sources,sources_list"
keysj "V-B① data.steps[0] shape (contract §5.4)" "$JQ_RESP" "data.steps.0" \
    "argv,consumes,expect_timeout,fail_code,group,judge,kind,label,log,outputs,stdout_lines,timeout_s,tool_code"
eqj "V-B① steps[0].fail_code = BRV-BLD-0011" "$JQ_RESP" "data.steps.0.fail_code" "BRV-BLD-0011"
eqj "V-B① steps[0].tool_code = BRV-BLD-0005" "$JQ_RESP" "data.steps.0.tool_code" "BRV-BLD-0005"
eqj "V-B① target comes from the platform plugin" "$JQ_RESP" "data.target.owner" "platform/qemu-aarch64"
eqj "V-B① target.arch" "$JQ_RESP" "data.target.arch" "aarch64"
eqj "V-B① target.cross" "$JQ_RESP" "data.target.cross" "aarch64-linux-gnu-"
eqj "V-B① target.linker_script (relative to the plugin root)" "$JQ_RESP" "data.target.linker_script" \
    "platform/qemu-aarch64/src/link.ld"
STEPS_JSON="$(jqget "$JQ_RESP" data.steps)"
contains "V-B① arch flag (-mstrict-align) enters the compile argv" '"-mstrict-align"' "$STEPS_JSON"
contains "V-B① arch flag (-mgeneral-regs-only) enters the compile argv" '"-mgeneral-regs-only"' "$STEPS_JSON"
contains "V-B① linker script enters -Wl,-T" '"-Wl,-T,platform/qemu-aarch64/src/link.ld"' "$STEPS_JSON"
contains "V-B① -Wl,-Map uses the artifact path" '"-Wl,-Map,build/brick.map"' "$STEPS_JSON"
contains "V-B① declared cflags enter argv" '"-DFX_BUILD=1"' "$STEPS_JSON"
contains "V-B① the assembly source (.S) is in the plan too (paths relative to root)" \
    'platform/qemu-aarch64/src/start.S' "$STEPS_JSON"
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
eqjc "V-B① data.tools has seven entries" "$JQ_RESP" "data.tools" "7"
eqj "V-B① tools[0].name = cc (BTreeMap order)" "$JQ_RESP" "data.tools.0.name" "cc"
keysj "V-B① data.tools[0] shape" "$JQ_RESP" "data.tools.0" "candidates,name,resolved,what"
eqj "V-B① cc first candidate = <cross>gcc" "$JQ_RESP" "data.tools.0.candidates.0" "aarch64-linux-gnu-gcc"
eqj "V-B① cc 2nd candidate = gcc-16" "$JQ_RESP" "data.tools.0.candidates.1" "aarch64-linux-gnu-gcc-16"
eqj "V-B① cc last candidate = host cc" "$JQ_RESP" "data.tools.0.candidates.5" "cc"
eqj "V-B① tools[3].name = objcopy" "$JQ_RESP" "data.tools.3.name" "objcopy"
eqj "V-B① objcopy first candidate = cross version" "$JQ_RESP" "data.tools.3.candidates.0" "aarch64-linux-gnu-objcopy"
eqj "V-B① objcopy last candidate = host version" "$JQ_RESP" "data.tools.3.candidates.1" "objcopy"
hasj "V-B① tools[0] carries a resolved key (resolution in core)" "$JQ_RESP" "data.tools.0.resolved"
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
"$PY" - "$W/platform/qemu-aarch64/brick.toml" <<'PY'
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
grep_at_least "V-B⑨ build.mk writes the linker script into the rule" '-Wl,-T,platform/qemu-aarch64/src/link.ld' \
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
run_brickie "$W" iface check service/x#x
eq "N-2 brickie iface check ⇒ exit code 2" "2" "$RC"
run_brickie "$W" frobnicate
eq "N-2 unknown subcommand ⇒ exit code 2" "2" "$RC"
run_brickie "$W"
eq "N-2 no subcommand ⇒ exit code 2" "2" "$RC"

# ---- N-3 machine-file ownership
section "N-3 machine files are written only by machines / hand edits go red"
W="$(cp_fx publish)"
run_brickie "$W" iface publish service/a#a
PLUGIN_BEFORE="$(sha256sum "$W/service/a/brick.toml")"
run_brickie "$W" iface publish service/a#a
eq "N-3 publish does not rewrite brick.toml (§9.1.1 rule 1)" "$PLUGIN_BEFORE" \
    "$(sha256sum "$W/service/a/brick.toml")"
grep_at_least "N-3 the snapshot is a machine file (carries the generated marker)" 'brickie:generated' "$W/api/iface/service/a/a.toml"
grep_at_least "N-3 the lock is a machine file" 'brickie:generated' "$W/brickie.lock"
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:1111111111111111111111111111111111111111111111111111111111111111"/' \
    "$W/api/iface/service/a/a.toml"
run_json "$W" iface status service/a#a
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
run_json "$W" iface show service/crypto#crypto
snapj "iface-show" "$JQ_RESP"
W="$(cp_fx cycle)"
run_json "$W" check
snapj "check-cycle" "$JQ_RESP"
W="$(new_work)"
run_json "$W" new ability service/crypto --subkind service
snapj "new-json" "$JQ_RESP"
W="$(cp_fx publish)"
run_json "$W" iface publish service/a#a
snapj "publish" "$JQ_RESP"
run_json "$W" iface status service/a#a
snapj "iface-status" "$JQ_RESP"

# ---- summary
printf '\n=== passed %s / failed %s ===\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '\nFailure details:\n'
    cat "$FAILLOG"
    exit 1
fi
exit 0
