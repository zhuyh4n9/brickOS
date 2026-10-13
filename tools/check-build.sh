#!/usr/bin/env bash
# brickOS prototype — build wiring consistency check (tool section <-> image delegation).
#
# The image entry point moved from literal Makefile rules to the declaration surface + brickie; the
# failure mode is a silent regression, so six mechanical criteria: 1) default goal is tools;
# 2) no literal image sources / compile flags in the Makefile; 3) image targets delegate to
# $(BRICKIE); 4) no cross dependencies in the tool section (criterion = the cross prefix itself);
# 5) host artifacts out of tree in build/host/<host-arch>/<host-os>/bin (brickie-gen, brickie-core,
# the entry ELF with both native tools embedded via --embed); 6) the committed seed holds all three,
# with the entry ELF magic \x7fELF.
#
# Usage: bash tools/check-build.sh (or make check-build); exit 0 consistent, 1 inconsistent, 2
# usage/environment error. `make -n -B` only; sub-makes inherit -n, non-destructive targets only.

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

MK="make"
DRY="$MK -n -B"   # -B: ignore timestamps, so an already-built tree still prints every command

if [ ! -f Makefile ]; then
    echo "FAIL: Makefile not found" >&2
    exit 2
fi

status=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; status=1; }

# ---- 1) default goal is tools ----
if grep -qE '^\.DEFAULT_GOAL[[:space:]]*:?=[[:space:]]*tools[[:space:]]*$' Makefile; then
    pass "default goal is tools (.DEFAULT_GOAL := tools)"
else
    fail "default goal is not tools — check .DEFAULT_GOAL (during migration it should build tools only)"
fi
dry_default="$($DRY 2>&1)"
if printf '%s\n' "$dry_default" | grep -q -- 'tools/brickie'; then
    pass "the default goal really builds tools (tools/brickie appears in make -n -B)"
else
    fail "the default goal does **not** build tools: no tools/brickie in the dry run"
    printf '%s\n' "$dry_default" | sed 's/^/     | /' | head -5
fi
if printf '%s\n' "$dry_default" | grep -q -- 'brickie build'; then
    fail "the default goal also delegates images (brickie build) — during migration the default should build tools only"
else
    pass "the default goal does not delegate images (building images goes through the explicit target make all)"
fi

# ---- 2) no literal image sources/flags ----
src_hits="$(grep -nE '(core|platform|service)/[a-z0-9_/]*\.(c|S)' Makefile || true)"
if [ -z "$src_hits" ]; then
    pass "no literal image source paths in Makefile (pattern scan empty)"
else
    fail "Makefile still has literal image source paths — those belong in each plugin's [build].sources"
    printf '%s\n' "$src_hits" | sed 's/^/     | /' | head -5
fi
flag_hits="$(grep -nE '(-ffreestanding|-fno-builtin|-mstrict-align|-march=|-mgeneral-regs-only|-Wl,-T|-nostdlib|-nostartfiles|-fno-omit-frame-pointer|-fdata-sections|-ffunction-sections)' Makefile || true)"
if [ -z "$flag_hits" ]; then
    pass "no literal image compile flags in Makefile (pattern scan empty)"
else
    fail "Makefile still has image compile flags — those belong in product.toml [build] / plugin [build]"
    printf '%s\n' "$flag_hits" | sed 's/^/     | /' | head -5
fi

# ---- 3) image targets delegate ----
# Each image-side name must be a thin delegation and the target must still exist.
for t in all run smoke irq-test mem-test string-test dbg-test check-string check-headers size disasm clean-brickos; do
    if grep -qE "^$t:" Makefile; then
        pass "image alias exists ($t)"
    else
        fail "image alias lost: target $t is not in the Makefile"
    fi
done
wants=(
    '$(BRICKIE) build'
    '$(BRICKIE) run'
    '$(BRICKIE) test smoke'
    '$(BRICKIE) test irq-test'
    '$(BRICKIE) test mem-test'
    '$(BRICKIE) test string-test'
    '$(BRICKIE) test dbg-test'
    '$(BRICKIE) test check-string'
    '$(BRICKIE) test check-headers'
    '$(BRICKIE) size'
    '$(BRICKIE) disasm'
    '$(BRICKIE) clean'
)
for w in "${wants[@]}"; do
    if grep -qF -- "$w" Makefile; then
        pass "delegation point exists ($w)"
    else
        fail "missing delegation point: $w — image aliases must call brickie, not define their own rules"
    fi
done

# ---- 4) tool section has no cross deps ----
# Composition-phase commands must not need the cross toolchain. The criterion uses the cross prefix
# itself, not a bare 'aarch64': host artifact paths already contain the host arch, so a bare word
# would false-positive on an aarch64 host.
cross_prefix="$($MK -s print-cross-compile 2>/dev/null)"
dry_tools="$($DRY tools 2>&1)"
if [ -n "$cross_prefix" ] && printf '%s\n' "$dry_tools" | grep -qF "$cross_prefix"; then
    fail "the dry run of 'make tools' mentions cross toolchain '$cross_prefix' — the tool section is polluted"
    printf '%s\n' "$dry_tools" | grep -F "$cross_prefix" | sed 's/^/     | /' | head -3
else
    pass "'make tools' does not touch the cross toolchain (pure host; prefix '$cross_prefix')"
fi

# ---- 5) host artifacts out of tree ----
host_bin="$($MK -s print-host-bin-dir 2>/dev/null)"
host_triple="$($MK -s print-host-triple 2>/dev/null)"
if printf '%s\n' "$host_bin" | grep -qE '/build/host/[^/]+/[^/]+/bin$'; then
    pass "host bin dir matches build/host/<host-arch>/<host-os>/bin ($host_triple)"
else
    fail "host bin dir is not build/host/<host-arch>/<host-os>/bin: '$host_bin'"
fi
if [ -n "$host_bin" ] && printf '%s\n' "$dry_default" | grep -qF -- "-o $host_bin/brickie-gen"; then
    pass "generator artifact lands in the host bin dir (-o $host_bin/brickie-gen)"
else
    fail "generator artifact does **not** land in the host bin dir ($host_bin/brickie-gen)"
    printf '%s\n' "$dry_default" | grep -E ' -o ' | sed 's/^/     | /' | head -3
fi
# The entry ELF must also be out of tree (regex excludes the brickie-gen prefix).
if [ -n "$host_bin" ] && printf '%s\n' "$dry_default" | grep -qE -- "-o $host_bin/brickie( |\$|\))"; then
    pass "entry ELF lands in the host bin dir (-o $host_bin/brickie)"
else
    fail "entry ELF (Python front end) does **not** land in the host bin dir ($host_bin/brickie)"
fi
# Both native tools must be embedded into the entry ELF (--embed <artifact>=bin/<name>), else it is
# just a shell, not self-contained.
for tool in brickie-gen brickie-core; do
    if printf '%s\n' "$dry_default" | grep -qE -- "--embed .*${tool}=bin/${tool}"; then
        pass "packing command embeds the native tool into the entry ELF (--embed ...=bin/${tool})"
    else
        fail "packing command does **not** embed ${tool} into the entry ELF (missing --embed ...=bin/${tool})"
        printf '%s\n' "$dry_default" | grep -F -- 'freeze.py' | sed 's/^/     | /' | head -3
    fi
done
# The L0/L1 Rust core is a target in tools/brickie/Makefile (not in `tools`, since L5/L2 must build
# without cargo), so this dry-runs that layer directly.
dry_core="$($DRY -C tools/brickie core 2>&1)"
if [ -n "$host_bin" ] && printf '%s\n' "$dry_core" | grep -qF -- "$host_bin/brickie-core"; then
    pass "Rust core artifact lands in the host bin dir ($host_bin/brickie-core)"
else
    fail "Rust core (brickie-core) does **not** land in the host bin dir ($host_bin/brickie-core)"
    printf '%s\n' "$dry_core" | grep -F -- 'brickie-core' | sed 's/^/     | /' | head -3
fi
if printf '%s\n' "$dry_default" | grep -qE ' -o (main|json|diag|text|rules|gen_new|render)\.o'; then
    fail "the tool section still drops intermediates in the source tree (-o <name>.o)"
    printf '%s\n' "$dry_default" | grep -E ' -o [a-z_]+\.o' | sed 's/^/     | /' | head -3
else
    pass "the tool section drops no intermediates in the source tree (out-of-tree build)"
fi

# ---- 6) bootstrap seed committed ----
# build/host/** is derived; this checks the committed seed's directory convention and presence only
# (it does not dry-run tools-prebuilt, whose recursive line would really write to disk).
seed_bin="$($MK -s print-prebuilt-bin-dir 2>/dev/null)"
if printf '%s\n' "$seed_bin" | grep -qE '/prebuilts/seed/brickie/[^/]+/[^/]+/bin$'; then
    pass "bootstrap seed dir matches prebuilts/seed/brickie/<host-arch>/<host-os>/bin"
else
    fail "bootstrap seed dir does not match the convention: '$seed_bin'"
fi
for name in brickie brickie-gen brickie-core; do
    if [ -x "$seed_bin/$name" ]; then
        pass "bootstrap seed in place ($seed_bin/$name)"
    else
        fail "bootstrap seed missing: $seed_bin/$name — run 'make tools-prebuilt' to publish one"
    fi
done
# The seeded entry ELF must be a real ELF (magic 7f 45 4c 46), never a script.
if [ -f "$seed_bin/brickie" ] && [ "$(head -c 4 "$seed_bin/brickie" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
    pass "the seeded brickie is a real ELF (\\x7fELF)"
else
    fail "the seeded brickie is not an ELF (bad magic)"
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: build wiring is consistent (default goal=tools / no literal sources / image delegation / tool section has no cross deps / host artifacts out of tree / bootstrap seed committed)"
else
    echo "hint: rerun this check after changing the Makefile's tools section or the image aliases" >&2
fi

exit "$status"
