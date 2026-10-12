# brickie — build entry for the tool itself
#
#   make            build the two host ELFs (out of tree): brickie-gen (L2) + brickie (L5 entry)
#                   (the Rust core brickie-core is embedded into the entry ELF if present; skipped otherwise)
#   make core       build the L0/L1 brickie-core with cargo → copy to build/host/<triple>/bin/
#                   (needs rust/ and cargo ≥ 1.83 — Cargo.lock is version 4; selection order below,
#                    a workspace-local .toolchain is adopted automatically; CARGO_HOME / CARGO_TARGET_DIR
#                    point at writable out-of-tree dirs)
#   make selftest   run the generator's built-in selftest (JSON round-trip / render / paths & modes / payload determinism)
#   make launcher-smoke  run the entry ELF's selftest (can the embedded payload start)
#   make test       run all cases (generator selftest + end-to-end tests/run.sh; run.sh already covers V-18 purity)
#   make test-purity    run only the three V-18 glue purity checks (selftest without Python / stub-replacement disproof / static check)
#   make test-v         verbose mode of the end-to-end acceptance run
#   make test-snap      regenerate the tests/snap/*.json protocol snapshots (only when updating the baseline on purpose)
#   make prebuilt   publish the **three** ELFs as the bootstrap seed prebuilts/seed/brickie/<arch>/<os>/bin/
#                   {brickie, brickie-gen, brickie-core}; missing core ⇒ FAIL (the seed must have all three)
#   make prebuilt-check  check whether the three seed pieces lag the source (no writes)
#   make clean
#
# Artifact locations are decided by mk/host.mk (cf. Android):
#   build/host/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}          (derived, not committed)
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core} (seed, committed)
#
# `brickie` is a **single-file self-contained entry ELF**: freeze.py embeds the Python front end and
# the templates into the binary; at run time it unpacks them, interprets with the system python3, and
# points BRICKIE_GEN / BRICKIE_CORE at the embedded native tools (see ADR-0004 §7 and contract §1).
#
# This file only does **delegation** and selection; it does not repeat cxx/'s compile rules. Split of
# labour with the image (repo-root Makefile): that one uses cross gcc for bare-metal aarch64; this one
# uses the **host g++/cargo** for compose-time tools — two compiler/option sets that never interfere.
#
# Language layering and the "three host executables" (contract §1):
#   brickie-core  Rust (L0 model + L1 solve/version/interface engine)  ← this file's `core:` delegates to cargo
#   brickie-gen   C++ (L2 generator, template rendering only)          ← cxx/Makefile
#   brickie        C++ launcher + embedded payload (L5 entry)          ← cxx/Makefile

include ../../mk/host.mk

CXX_DIR := cxx

INSTALL ?= install

# ---------------------------------------------------------------- host Rust toolchain
# Why "choosing cargo" is spelled out: `rust/Cargo.lock` is **version 4**, while cargo on PATH is
# likely the distro's old one (e.g. 1.75 on Ubuntu 24.04) — it cannot read v4 and only reports
# "lock file version 4 requires `-Znext-lockfile-bump`", an error that **following it does not fix**.
# So the selection order is explicit, and when too old we give the actionable fix on the spot.
#
# Selection order (first wins):
#   1. explicit override: env var `CARGO` / `make CARGO=/path/to/cargo …`;
#   2. workspace-local rustup install: `<repo>/.toolchain` or `<repo>/../.toolchain`
#      (GBs in size, **not committed**; location overridable via RUST_TOOLCHAIN_DIR);
#   3. `cargo` on PATH.
# Path 2 also sets RUSTUP_HOME and prepends its bin to PATH: rustup's shim finds the toolchain
# via RUSTUP_HOME, and `rustc --version` in build.rs (a fingerprint source) must be the same rustc
# cargo actually uses. (Comments must occupy their own line — trailing an assignment, make folds them in.)
# The Cargo.lock v4 threshold: cargo 1.83
RUST_MIN_CARGO_MINOR := 83

RUST_TOOLCHAIN_DIR ?= $(firstword $(wildcard $(REPO_ROOT)/.toolchain $(REPO_ROOT)/../.toolchain))

ifeq ($(origin CARGO),undefined)
  ifneq ($(wildcard $(RUST_TOOLCHAIN_DIR)/cargo/bin/cargo),)
    CARGO := $(RUST_TOOLCHAIN_DIR)/cargo/bin/cargo
  endif
endif
CARGO ?= cargo

# Whenever `$(CARGO)` looks like `<root>/cargo/bin/cargo` with `<root>/rustup` next to it, add the env:
# so neither the "explicit CARGO=…" nor the "auto-discovered" path forces the user to set RUSTUP_HOME.
# If the user already set RUSTUP_HOME, only prepend PATH and do not override it.
_RUST_TC_ROOT := $(patsubst %/cargo/bin/cargo,%,$(CARGO))
_RUST_TC_ENV  :=
ifneq ($(_RUST_TC_ROOT),$(CARGO))
  _RUST_TC_PATH := PATH="$(_RUST_TC_ROOT)/cargo/bin:$$PATH"
  ifeq ($(origin RUSTUP_HOME),undefined)
    _RUST_TC_ENV := RUSTUP_HOME="$(_RUST_TC_ROOT)/rustup" $(_RUST_TC_PATH)
  else
    _RUST_TC_ENV := $(_RUST_TC_PATH)
  endif
endif

# `$HOME/.cargo` may be read-only in managed environments ⇒ put CARGO_HOME / target in writable
# out-of-tree dirs (build/ is not committed as a whole). Both can be overridden via env vars.
CARGO_HOME       ?= $(abspath ../../build/cargo-home)
CARGO_TARGET_DIR ?= $(abspath ../../build/cargo-target)
CORE_MANIFEST    ?= rust/Cargo.toml
CORE_PACKAGE     ?= brickie-core
CORE_BIN         := $(HOST_BIN_DIR)/brickie-core

.PHONY: all cxx core selftest launcher-smoke test test-purity test-v test-snap prebuilt prebuilt-check clean \
        print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-launcher-bin

all: cxx

cxx:
	$(MAKE) -C $(CXX_DIR)

# L0/L1 Rust core: produced from Rust sources, copied to the same out-of-tree spot as brickie-gen.
# When rust/ is absent: if a brickie-core artifact already exists (given by someone) skip it, else FAIL —
# because `make prebuilt` requires all three; plain `make` / `make cxx` are unaffected.
#
# Note: the whole block must be **one shell** (line-continued with `\`) — each make recipe line gets
# its own shell, so `exit 0` would end only that line and the cargo below would still run.
core:
	@if [ ! -f "$(CORE_MANIFEST)" ]; then \
	    if [ -x "$(CORE_BIN)" ]; then \
	        echo "core: $(CORE_BIN) already exists, skipping (Rust source $(CORE_MANIFEST) is not in this checkout)"; \
	        exit 0; \
	    fi; \
	    echo "FAIL: missing $(CORE_MANIFEST) (the L0/L1 Rust core is not in place yet)" >&2; \
	    echo "      the bootstrap seed requires all three of {brickie,brickie-gen,brickie-core}" >&2; \
	    exit 1; \
	fi; \
	_ver=$$($(_RUST_TC_ENV) "$(CARGO)" --version 2>/dev/null); \
	_rest=$${_ver#cargo }; _rest=$${_rest%% *}; \
	_maj=$${_rest%%.*}; _min=$${_rest#*.}; _min=$${_min%%.*}; \
	_num=$$(( _maj * 1000 + _min )) 2>/dev/null || _num=0; \
	if [ "$$_num" -lt $$(( 1000 + $(RUST_MIN_CARGO_MINOR) )) ]; then \
	    echo "FAIL: cargo too old or unavailable: '$$_ver' (from $(CARGO))" >&2; \
	    echo "      $(CORE_MANIFEST)'s Cargo.lock is version 4, needs cargo ≥ 1.$(RUST_MIN_CARGO_MINOR);" >&2; \
	    echo "      the old cargo's raw error is \"lock file version 4 requires -Znext-lockfile-bump\" — following it does not help." >&2; \
	    echo "      fix: install a newer cargo, or pass one explicitly (e.g. make -C tools/brickie core CARGO=/path/to/cargo)," >&2; \
	    echo "            or place a .toolchain in the workspace ($(REPO_ROOT)/.toolchain or $(REPO_ROOT)/../.toolchain)." >&2; \
	    exit 1; \
	fi; \
	mkdir -p "$(CARGO_HOME)" "$(CARGO_TARGET_DIR)" "$(HOST_BIN_DIR)"; \
	echo "core: $$_ver ($(CARGO)) → $(CORE_BIN)"; \
	CARGO_HOME="$(CARGO_HOME)" CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" $(_RUST_TC_ENV) \
	    "$(CARGO)" build --release --manifest-path "$(CORE_MANIFEST)" -p $(CORE_PACKAGE) || exit $$?; \
	$(INSTALL) -m 0755 "$(CARGO_TARGET_DIR)/release/$(CORE_PACKAGE)" "$(CORE_BIN)" || exit $$?; \
	echo "core: $(CORE_BIN)"

selftest: cxx
	$(MAKE) -C $(CXX_DIR) selftest

launcher-smoke: cxx
	$(MAKE) -C $(CXX_DIR) launcher-smoke

test: selftest
	bash tests/run.sh

# The three V-18 criteria can be run on their own (run.sh already pulls them into `make test`)
test-purity:
	bash tests/purity.sh

# Verbose mode of the end-to-end acceptance run (prints exit code/diagnostics per case)
test-v:
	bash tests/run.sh -v

# Only when deliberately updating the protocol snapshot baseline: regenerate tests/snap/*.json
test-snap:
	BRICKIE_SNAP_EMIT=1 bash tests/run.sh
	@echo "tests/snap/ regenerated — review the diff before committing"

# core before cxx: when the sub-make re-parses, $(wildcard $(CORE_BIN)) is already true ⇒ the payload embeds core.
prebuilt: core
	$(MAKE) -C $(CXX_DIR) prebuilt

prebuilt-check: cxx
	$(MAKE) -C $(CXX_DIR) prebuilt-check

clean:
	$(MAKE) -C $(CXX_DIR) clean

# Pass-through: host triple / host bin dir / seed dir (used by gates and cases, avoiding recomputed mappings in shell)
print-host-triple:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-host-triple

print-host-bin-dir:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-host-bin-dir

print-prebuilt-bin-dir:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-prebuilt-bin-dir

print-launcher-bin:
	@$(MAKE) --no-print-directory -C $(CXX_DIR) print-launcher-bin
