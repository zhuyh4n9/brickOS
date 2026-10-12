# brickOS prototype v0.1.0 — top-level build entry (**tools only** + image-side transition aliases)
#
# ================================================================ Ownership (ADR-0003 S1/S4)
# **The image entry point is `brickie build`**, not this file.
#
#   ① Tools section (this file's only real thing): `make` / `make tools` build the compose-time
#      tools `{brickie, brickie-gen, brickie-core}` with the **host g++/cargo**, out of tree to
#      build/host/<host-arch>/<host-os>/bin. It **never touches the cross toolchain** — the
#      zero-compile-dependency discipline for compose-time commands (contract §9 R-13).
#   ② Image section: source set / compile flags / link rules **all** moved to the declaration
#      surface — product.toml [build] + each plugin brick.toml [build] + platform [build.target]
#      + the gates in tests/gates.toml; consumed by `brickie build`.
#
# Hence `all` / `run` / `smoke` / `irq-test` / `mem-test` / `string-test` / `dbg-test` /
# `check-string` / `check-headers` / `size` / `disasm` / `clean-brickos` in this file all
# degrade to **thin delegation aliases** (calling the matching $(BRICKIE) command) — the
# **interim form** of the ADR-0003 shrink phase. The real thing is `brickie build` and
# tests/gates.toml; the aliases only promise that "old muscle memory still works".
# Invariant ② of `make check-build` catches any "literal source / flag regression".
#
# Toolchain: **external gcc** (WORKAROUND br-wa-toolchain-001). Its selection (cross prefix /
# variant / linker script / QEMU model) is now expressed by product.toml + the platform plugin
# declaration surface, with **candidate order and resolution in core** (contract §9 R-14 / R-17).
# This file keeps only the single query point `print-cross-compile`.
#
# Usage:
#   make                    build tools only (= make tools; no image)
#   make all                build tools → delegate `brickie build` (image + [build].post gates)
#   make tools              build L5/L2 only (no cross compiler, no cargo needed)
#   make tools-core         build the L0/L1 Rust core only (brickie-core; needs cargo)
#   make tools-test         run the tools' own cases (generator selftest + end-to-end)
#   make brickie-check      validate the declaration surface with brickie (dev; no image)
#   make brickie-check-release  release-grade gate (--profile release)
#   make brickie-compose    regenerate build/gen/** artifacts (no compile)
#   source setup.sh         set up the dev environment (PATH + env vars; see ADR-0002 §7.5)
#   make env                print the same shell snippet (CI: eval "$(make -s env)")
#   make run                run on QEMU (Ctrl-A X to quit; delegates brickie run)
#   make smoke              3-second smoke (brickie test smoke)
#   make irq-test           interrupt subsystem per-case gate (brickie test irq-test)
#   make mem-test           host-side memory semantics gate (brickie test mem-test --no-build)
#   make string-test        host-side compiler support routine cases (brickie test string-test --no-build)
#   make dbg-test           memory map + debug plugin gate (brickie test dbg-test)
#   make check-string       compiler support routine self-recursion gate (brickie test check-string)
#   make check-headers      public header self-containment gate (brickie test check-headers --no-build)
#   make size / disasm      size / disassembly (brickie size / disasm)
#   make check-workarounds  WORKAROUND registry vs. source markers in sync
#   make check-build        build wiring gate (default=tools / no literal sources / delegation / out-of-tree / seed)
#   make tools-prebuilt     publish the tools as the bootstrap seed prebuilts/seed/brickie/<arch>/<os>/bin/
#   make tools-prebuilt-check  check whether the seed lags behind the source
#   make print-host-triple  print the host triple (host-arch/host-os)
#   make print-host-bin-dir print the host tools bin dir (build/host/<arch>/<os>/bin)
#   make print-cross-compile print the cross prefix (query / gate only; builds no longer read it)
#   make clean              remove artifacts of ① and ② (tools only: make tools-clean; prebuilts/ untouched)

# ---------------------------------------------------------------- prebuilt (optional)
# prebuilts/toolchain/ is a **derived directory** (not committed; recipe in prebuilts/toolchain.lock.toml), fetched by `make prebuilt`.
# This -include brings in the actual paths of each artifact; if the file is absent ⇒ probing below falls back to the host toolchain.
#
# **Trust no "in place" marker**: prebuilts/toolchain/ may be fetched only partially (`fetch-prebuilt.py <id>`), so below we
# check each binary **really exists** with $(wildcard). See ADR-0002.
-include prebuilts/toolchain/toolchain.mk

PREBUILT ?= prebuilts/toolchain

# ---------------------------------------------------------------- toolchain probing (query only)
# Prefer prebuilt (self-contained, reproducible), else fall back to the host external cross toolchain (br-wa-toolchain-001 interim form).
ifneq ($(strip $(PREBUILT_CROSS)),)
  CROSS_COMPILE ?= $(PREBUILT_CROSS)
  TOOLCHAIN_SOURCE := prebuilts/toolchain/  ($(PREBUILT_CROSS))
else
  CROSS_COMPILE ?= aarch64-linux-gnu-
  TOOLCHAIN_SOURCE := host external toolchain  (br-wa-toolchain-001)
endif

# Recursive make also uses the prebuilt one (pinned version); otherwise the make that called this file.
ifneq ($(wildcard $(PREBUILT_MAKE)),)
  MAKE := $(PREBUILT_MAKE)
endif

# ------------------------------------------------------------------ dirs / artifacts
BUILD_DIR := build

# Host tool artifacts (out of tree): build/host/<host-arch>/<host-os>/{bin,lib,obj}
# The host is detected by mk/host.mk via tools/host-detect.sh; these sit on the opposite side
# of build/ from the image artifacts (build/obj, build/brick.*) and never overwrite each other
# — like Android's split between out/host and out/target.
#
# There is also `prebuilts/` (committed): the bootstrap seed prebuilts/seed/brickie/<host-arch>/<host-os>/bin/,
# published by `make tools-prebuilt` — so a fresh checkout without g++ can run brickie directly, and
# the starting point for future brickie self-hosting of its own build (see docs/decisions/0004). Do not
# confuse it with the download cache `prebuilts/toolchain/` (derived side, not committed).
HOST_BUILD_ROOT := $(abspath $(BUILD_DIR))
include mk/host.mk

# ------------------------------------------------------------------- targets
# ⚠ Default target = **tools** (not the image): during migration `make` no longer builds the image too,
#   sparing "tools only" setups from the cross toolchain; "build image" goes via `make all` (`brickie build`).
.DEFAULT_GOAL := tools

.PHONY: all prebuilt prebuilt-check prebuilt-clean \
        tools tools-core tools-test tools-clean tools-prebuilt tools-prebuilt-check \
        brickie-check brickie-check-release brickie-compose \
        run smoke irq-test mem-test string-test dbg-test check-string check-headers \
        size disasm check-workarounds check-build \
        clean clean-brickos help print-host-triple print-host-bin-dir \
        print-prebuilt-bin-dir print-cross-compile env

# ============================================================== ⓪ prebuilt toolset
# **Download cache** for the external toolchain (pinned versions + SHA256 verification; recipe = prebuilts/toolchain.lock.toml), see ADR-0002.
#
# ⚠ Target name `prebuilt` and directory `prebuilts/toolchain/` **share a name** — held apart only by the .PHONY above.
#   Miss it and make thinks "the directory already exists ⇒ nothing to do", making the fetch a **silent no-op** (been there).
#
# ⚠ Not the same thing as prebuilts/ (**committed side**, the bootstrap seed): that one is the `tools-prebuilt` target.
#   The two directory names differ by one "s" and mean the opposite — see the naming note at the top of ADR-0004.
prebuilt:
	@echo "== ⓪ prebuilt: fetch (pinned versions + SHA256 verification) =="
	@python3 tools/fetch-prebuilt.py

# In-place check. **Not fetched does not fail by default**: falling back to the host toolchain is a legal interim form (br-wa-toolchain-001).
# Add PREBUILT_STRICT=1 where hard self-containment is required (CI reproducibility jobs).
prebuilt-check:
	@if [ "$(PREBUILT_STRICT)" = "1" ]; then \
	    python3 tools/fetch-prebuilt.py --check || exit 1; \
	else \
	    python3 tools/fetch-prebuilt.py --check || true; \
	fi
	@echo "   cross toolchain source: $(TOOLCHAIN_SOURCE)"

# Deletes the download cache only. Guard: accept only paths ending in prebuilt — so a PREBUILT pointing elsewhere is not wiped by mistake.
prebuilt-clean:
	@case "$(PREBUILT)" in prebuilts/toolchain|*/toolchain) ;; \
	  *) echo "refused: PREBUILT=$(PREBUILT) is not the prebuilts/toolchain dir, not deleting"; exit 2;; esac
	rm -rf $(PREBUILT)

# ============================================================== ① Tools section (this file's only real thing)
# brickie itself (the compose-time tool). It has its own build entry and test set — here we only **delegate**
# and never repeat its rules (single source of truth for mk: tools/brickie/Makefile).
#
# Why recursive make instead of listing objects: tools and image use **different compilers and options**
# (host g++ vs. cross gcc), and the image already moved over to brickie, so mixing them in one Makefile is pointless.
TOOLS_DIR := tools/brickie

tools:
	@echo "== ① tools: $(TOOLS_DIR) (host g++, no cross toolchain) =="
	@echo "   artifacts: $(HOST_BIN_DIR)/{brickie,brickie-gen}  [$(HOST_TRIPLE)]"
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) cxx

tools-test:
	@echo "== ① tools cases =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) test

tools-clean:
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) clean

# L0/L1 Rust core (separate target: `make tools` builds only L5/L2, so a checkout without
# cargo can still start; the bootstrap seed needs all three ⇒ `make tools-prebuilt` pulls it in too).
tools-core:
	@echo "== ① tools: brickie-core (L0/L1 Rust; cargo + out of tree to $(HOST_BIN_DIR)) =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) core

# Bootstrap seed (committed): published to prebuilts/seed/brickie/<host-arch>/<host-os>/bin/.
# When brickie later self-hosts its own build, this seed is the "first brick".
tools-prebuilt: tools
	@echo "== ① tools bootstrap seed → prebuilts/seed/brickie/$(HOST_TRIPLE)/bin/ =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt

# Gate (on demand): a seed lagging the source turns red — a reminder to republish, never auto-writes.
tools-prebuilt-check: tools
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt-check

# ------------------------------------------------ compose-time declaration surface (managed by brickie)
# The declaration surface (schema/dependencies/export surface/privileges/budgets) is validated and governed
# by brickie; generated artifacts are rebuilt by brickie gen (`build/gen/**`).
#
# The entry is the **entry ELF**: it carries the Python front end and two native tools
# (brickie-core/brickie-gen), so no PYTHONPATH/source tree is needed here; if core is missing it falls back
# to the bootstrap seed (see native.py lookup order); invariant ⑥ of `make check-build` verifies all three.
BRICKIE      := $(HOST_BIN_DIR)/brickie
COMPOSE_ROOT := $(abspath .)

.PHONY: brickie-check brickie-check-release brickie-compose
brickie-check: tools
	@echo "== compose-time check: brickie check (declaration surface completeness, dev) =="
	@$(BRICKIE) check --root $(COMPOSE_ROOT)

brickie-check-release: tools
	@echo "== compose-time check: brickie check --profile release (release-grade gate) =="
	@$(BRICKIE) check --root $(COMPOSE_ROOT) --profile release

brickie-compose: tools
	@echo "== regenerate artifacts: brickie gen → build/gen/** (no compile) =="
	@$(BRICKIE) gen --root $(COMPOSE_ROOT)

# ============================================================== ② Image section (transition aliases)
# Below are the ADR-0003 **shrink-phase transition aliases**: the real thing is brickie (build.rs) + tests/gates.toml.
# Each alias does only three things: ensure the tools are present (order-only) → call the matching command → write no rules itself.
# Once CI and habits call `brickie` directly, these aliases can be deleted wholesale (the S4 end state of ADR-0003).
#
# Why order-only (`| tools`): newer tools must not trigger the illusion of "rebuilding the image" — the
# criterion is brickie's argv fingerprint and timestamps (contract §9 R-15), not here.
all: | tools
	@echo "== ② image: delegate brickie build (ADR-0003 shrink-phase transition alias) =="
	@$(BRICKIE) build --root $(COMPOSE_ROOT)

run: | tools
	@echo "== ② QEMU run: delegate brickie run (Ctrl-A X to quit) =="
	@$(BRICKIE) run --root $(COMPOSE_ROOT)

smoke: | tools
	@echo "== ② smoke gate: delegate brickie test smoke =="
	@$(BRICKIE) test smoke --root $(COMPOSE_ROOT)

irq-test: | tools
	@echo "== ② interrupt per-case gate: delegate brickie test irq-test =="
	@$(BRICKIE) test irq-test --root $(COMPOSE_ROOT)

# Host-side gates use --no-build: they need only a host C compiler, independent of the cross toolchain/QEMU.
mem-test: | tools
	@echo "== ② host memory semantics gate: delegate brickie test mem-test --no-build =="
	@$(BRICKIE) test mem-test --no-build --root $(COMPOSE_ROOT)

string-test: | tools
	@echo "== ② host support routine cases: delegate brickie test string-test --no-build =="
	@$(BRICKIE) test string-test --no-build --root $(COMPOSE_ROOT)

dbg-test: | tools
	@echo "== ② memory map + debug plugin gate: delegate brickie test dbg-test =="
	@$(BRICKIE) test dbg-test --root $(COMPOSE_ROOT)

# check-string inspects self-recursion in the **compiled artifact** (the image must be built) ⇒ no --no-build.
check-string: | tools
	@echo "== ② support routine self-recursion gate: delegate brickie test check-string =="
	@$(BRICKIE) test check-string --root $(COMPOSE_ROOT)

# check-headers only does `-fsyntax-only` (the script finds a cross or host C compiler itself) ⇒ --no-build.
check-headers: | tools
	@echo "== ② header self-containment gate: delegate brickie test check-headers --no-build =="
	@$(BRICKIE) test check-headers --no-build --root $(COMPOSE_ROOT)

size: | tools
	@echo "== ② size: delegate brickie size =="
	@$(BRICKIE) size --root $(COMPOSE_ROOT)

disasm: | tools
	@echo "== ② disassembly: delegate brickie disasm =="
	@$(BRICKIE) disasm --root $(COMPOSE_ROOT)

# ------------------------------------------------------- WORKAROUND / build gates
check-workarounds:
	@bash tools/check-workarounds.sh

# Build wiring gate: is the default target tools / does the Makefile still hold literal sources and flags /
# are the image targets delegating / is the tools section polluted by the cross toolchain / out-of-tree and seed.
# Rationale at the top of the script — broken wiring is usually **silent**.
check-build:
	@bash tools/check-build.sh

# ------------------------------------------------------------------- queries
# Host triple and artifact locations: gates/CI/cases take them from here instead of recomputing the mapping
# (single source of truth = tools/host-detect.sh → mk/host.mk).
print-host-triple:
	@echo $(HOST_TRIPLE)

print-host-bin-dir:
	@echo $(HOST_BIN_DIR)

print-prebuilt-bin-dir:
	@echo $(PREBUILT_BRICKIE_BIN_DIR)

# **Read-only** query point for the cross prefix (used by gates and setup.sh). Image builds no longer read it —
# target facts live in the platform plugin's [build.target] (contract §7.1).
print-cross-compile:
	@echo $(CROSS_COMPILE)

# Dev environment (shell snippet): toolchain bin dirs + CROSS_COMPILE + brickie's root path.
#   eval "$(make -s env)"   — enters the current shell directly (equivalent to source setup.sh)
#   source setup.sh         — the usual way; also --unset / --with-make / missing-file hints
# This only **prints**: a Makefile should not modify its caller's environment (it cannot, and must not pretend to).
env:
	@bash setup.sh --print

# ------------------------------------------------------------------- cleaning
# `clean` removes derived artifacts on **both** sides, tools and image; use tools-clean for tools only.
#
# They are cleaned separately: host tools live in build/host/ (out-of-tree output), the image side is
# cleaned by `brickie clean` per the declaration surface (it does **not** touch build/host — so "clean image" never wipes tools).
clean: clean-brickos tools-clean

clean-brickos: | tools
	@echo "== ② clean image artifacts: delegate brickie clean =="
	@$(BRICKIE) clean --root $(COMPOSE_ROOT)

help:
	@echo "targets: tools (default) all (delegates brickie build) prebuilt prebuilt-check prebuilt-clean"
	@echo "      tools-core tools-test tools-prebuilt tools-prebuilt-check tools-clean"
	@echo "      brickie-check brickie-check-release brickie-compose"
	@echo "      run smoke irq-test mem-test string-test dbg-test check-string check-headers"
	@echo "      size disasm check-workarounds check-build clean"
	@echo "      print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-cross-compile env"
	@echo "vars: CROSS_COMPILE=$(CROSS_COMPILE) (query point only)"
	@echo "      HOST_TRIPLE=$(HOST_TRIPLE)  HOST_BIN_DIR=$(HOST_BIN_DIR)"
	@echo "      PREBUILT_BRICKIE_BIN_DIR=$(PREBUILT_BRICKIE_BIN_DIR)"
	@echo "ownership: image entry = brickie build (declaration surface: product.toml [build] + plugin [build] + [build.target]);"
	@echo "          this file builds tools only; the image targets are all **transition aliases** (ADR-0003 S1/S4)"
	@echo "layout: tools → build/host/<host-arch>/<host-os>/bin (cf. Android out/host/...); image → build/"
	@echo "seed: tools-prebuilt → prebuilts/seed/brickie/<host-arch>/<host-os>/bin (committed, for bootstrap)"
	@echo "toolchain source: $(TOOLCHAIN_SOURCE)"
	@echo "prebuilt (fetch target → prebuilts/toolchain/): $(if $(filter 1,$(PREBUILT_READY)),in place,not fetched — run make prebuilt (ADR-0002))"
