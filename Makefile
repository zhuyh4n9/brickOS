# brickOS prototype v0.1.0 — top-level make entry: tools only, plus image-side transition aliases.
# Ownership: the image entry point is `brickie build`.
# ① Tools section (this file's only real rules): `make` / `make tools` build the compose-time
#   tools {brickie, brickie-gen, brickie-core} with host g++/cargo, out of tree to
#   build/host/<host-arch>/<host-os>/bin; the cross toolchain is never used here.
# ② Image section: sources / flags / link rules live on the declaration surface (product.toml
#   [build] + plugin brick.toml [build] + platform [build.target] + tests/gates.toml); consumed
#   by `brickie build`, and every image target here is a thin $(BRICKIE) delegation alias.
# Toolchain: external gcc (WORKAROUND br-wa-toolchain-001); selection resolves in core, so this
#   file keeps only the query point `print-cross-compile`.
# Usage: make [tools] (L5/L2) | all (delegate brickie build) | tools-core (L0/L1 Rust) |
#   run | smoke | irq-test | mem-test | string-test | dbg-test | check-string | check-headers |
#   size | disasm | brickie-check | brickie-check-release | brickie-compose | tools-test |
#   check-workarounds | check-build | tools-prebuilt | tools-prebuilt-check | env | clean |
#   print-host-triple | print-host-bin-dir | print-prebuilt-bin-dir | print-cross-compile;
#   source setup.sh sets the dev environment (make env prints the same snippet for CI).

# ---------------------------------------------------------------- prebuilt (optional)
# prebuilts/toolchain/ is derived (not committed; recipe in prebuilts/toolchain.lock.toml); this
# -include supplies each artifact's path. Absent or partial fetch => the probing below falls back
# to the host toolchain ($(wildcard) checks each binary really exists, never a marker file).
-include prebuilts/toolchain/toolchain.mk

PREBUILT ?= prebuilts/toolchain

# ---------------------------------------------------------------- toolchain probing (query only)
# Prefer prebuilt (reproducible), else the host external cross toolchain (br-wa-toolchain-001).
ifneq ($(strip $(PREBUILT_CROSS)),)
  CROSS_COMPILE ?= $(PREBUILT_CROSS)
  TOOLCHAIN_SOURCE := prebuilts/toolchain/  ($(PREBUILT_CROSS))
else
  CROSS_COMPILE ?= aarch64-linux-gnu-
  TOOLCHAIN_SOURCE := host external toolchain  (br-wa-toolchain-001)
endif

# Recursive make uses the pinned prebuilt make when present.
ifneq ($(wildcard $(PREBUILT_MAKE)),)
  MAKE := $(PREBUILT_MAKE)
endif

# ------------------------------------------------------------------ dirs / artifacts
BUILD_DIR := build

# Host tools land out of tree in build/host/<host-arch>/<host-os>/{bin,lib,obj}, opposite the image
# artifacts (build/obj, build/brick.*). prebuilts/seed/brickie/<host-arch>/<host-os>/bin/ is the
# committed seed from `make tools-prebuilt`; the download cache prebuilts/toolchain/ is derived.
HOST_BUILD_ROOT := $(abspath $(BUILD_DIR))
include mk/host.mk

# ------------------------------------------------------------------- targets
# Default target = tools, not the image: tools-only checkouts need no cross toolchain.
.DEFAULT_GOAL := tools

.PHONY: all prebuilt prebuilt-check prebuilt-clean \
        tools tools-core tools-test tools-clean tools-prebuilt tools-prebuilt-check \
        brickie-check brickie-check-release brickie-compose \
        run smoke irq-test mem-test string-test dbg-test check-string check-headers \
        size disasm check-workarounds check-build \
        clean clean-brickos help print-host-triple print-host-bin-dir \
        print-prebuilt-bin-dir print-cross-compile env

# ============================================================== ⓪ prebuilt toolset
# Download cache for the external toolchain (pinned versions + SHA256; recipe in
# prebuilts/toolchain.lock.toml). The target name collides with the directory, held apart only by
# .PHONY above — without it make treats the fetch as a silent no-op. Not the seed (prebuilts/).
prebuilt:
	@echo "== ⓪ prebuilt: fetch (pinned versions + SHA256 verification) =="
	@python3 tools/fetch-prebuilt.py

# In-place check; not fetched is not a failure by default (host fallback is legal,
# br-wa-toolchain-001). Set PREBUILT_STRICT=1 where hard self-containment is required.
prebuilt-check:
	@if [ "$(PREBUILT_STRICT)" = "1" ]; then \
	    python3 tools/fetch-prebuilt.py --check || exit 1; \
	else \
	    python3 tools/fetch-prebuilt.py --check || true; \
	fi
	@echo "   cross toolchain source: $(TOOLCHAIN_SOURCE)"

# Deletes the download cache only; refuses a PREBUILT path not ending in toolchain.
prebuilt-clean:
	@case "$(PREBUILT)" in prebuilts/toolchain|*/toolchain) ;; \
	  *) echo "refused: PREBUILT=$(PREBUILT) is not the prebuilts/toolchain dir, not deleting"; exit 2;; esac
	rm -rf $(PREBUILT)

# ============================================================== ① Tools section (only real rules)
# brickie's own build entry is tools/brickie/Makefile (single source of truth); this only delegates.
# Recursive make because tools use host g++/cargo and the image uses cross gcc.
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

# L0/L1 Rust core; separate because `make tools` must work in a checkout without cargo.
tools-core:
	@echo "== ① tools: brickie-core (L0/L1 Rust; cargo + out of tree to $(HOST_BIN_DIR)) =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) core

# Bootstrap seed (committed) -> prebuilts/seed/brickie/<host-arch>/<host-os>/bin/.
tools-prebuilt: tools
	@echo "== ① tools bootstrap seed → prebuilts/seed/brickie/$(HOST_TRIPLE)/bin/ =="
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt

# Gate (on demand): a seed lagging the source turns red; never auto-writes.
tools-prebuilt-check: tools
	@$(MAKE) --no-print-directory -C $(TOOLS_DIR) prebuilt-check

# ------------------------------------------------ compose-time surface (managed by brickie)
# BRICKIE is the entry ELF (Python front end + embedded brickie-core/brickie-gen), so no
# PYTHONPATH/source tree is needed; a missing core falls back to the bootstrap seed (native.py).
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

# ============================================================== ② Image section (aliases)
# Thin aliases to brickie: each ensures the tools exist (order-only), calls the matching command
# and writes no rules. Order-only because the image rebuild criterion is brickie's argv fingerprint.
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

# Host-side gates use --no-build: they need only a host C compiler, no cross toolchain/QEMU.
mem-test: | tools
	@echo "== ② host memory semantics gate: delegate brickie test mem-test --no-build =="
	@$(BRICKIE) test mem-test --no-build --root $(COMPOSE_ROOT)

string-test: | tools
	@echo "== ② host support routine cases: delegate brickie test string-test --no-build =="
	@$(BRICKIE) test string-test --no-build --root $(COMPOSE_ROOT)

dbg-test: | tools
	@echo "== ② memory map + debug plugin gate: delegate brickie test dbg-test =="
	@$(BRICKIE) test dbg-test --root $(COMPOSE_ROOT)

# check-string inspects the compiled artifact (the image must be built) => no --no-build.
check-string: | tools
	@echo "== ② support routine self-recursion gate: delegate brickie test check-string =="
	@$(BRICKIE) test check-string --root $(COMPOSE_ROOT)

# check-headers only runs -fsyntax-only (it finds its own C compiler) => --no-build.
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

# check-build: default goal=tools / no literal sources or flags / image targets delegate /
# tools section free of the cross toolchain / out-of-tree artifacts and seed.
check-build:
	@bash tools/check-build.sh

# ------------------------------------------------------------------- queries
# Host triple and artifact locations; single source of truth is tools/host-detect.sh -> mk/host.mk.
print-host-triple:
	@echo $(HOST_TRIPLE)

print-host-bin-dir:
	@echo $(HOST_BIN_DIR)

print-prebuilt-bin-dir:
	@echo $(PREBUILT_BRICKIE_BIN_DIR)

# Read-only query point for the cross prefix (gates/setup.sh); image builds do not read it, target
# facts live in the platform plugin's [build.target].
print-cross-compile:
	@echo $(CROSS_COMPILE)

# Dev environment shell snippet (toolchain bin dirs + CROSS_COMPILE + brickie root); prints only.
env:
	@bash setup.sh --print

# ------------------------------------------------------------------- cleaning
# clean covers both sides; tools-clean is tools only. Host tools (build/host) are separate from the
# image side cleaned by `brickie clean`, so cleaning the image never wipes the tools.
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
