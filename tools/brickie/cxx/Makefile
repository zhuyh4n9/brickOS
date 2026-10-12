# brickie's L2 generator + L5 entry ELF. Build: plain make + g++ + python3, no CMake needed.
#
# Artifacts go **out of tree** (mirroring Android's out/host/... layout, see mk/host.mk):
#
#   build/host/<host-arch>/<host-os>/bin/brickie-gen      ← L2 generator (ELF, template rendering only)
#   build/host/<host-arch>/<host-os>/bin/brickie          ← L5 front-end entry (ELF, embeds Python payload)
#   build/host/<host-arch>/<host-os>/bin/brickie-core     ← L0/L1 Rust core (**not built by this Makefile**;
#                                                            produced by `make core` in tools/brickie)
#   build/host/<host-arch>/<host-os>/lib/libbrickie-gen.a ← static library
#   build/host/<host-arch>/<host-os>/obj/cxx/*.o          ← intermediate objects
#   build/host/<host-arch>/<host-os>/share/brickie/payload.tar ← payload embedded in brickie (for inspection)
#
# All three executables go into the bootstrap seed (committed, see ADR-0004 / contract §1):
#
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
#
# `brickie` is a **single self-contained entry ELF**: the Python package, templates and **native tools**
# (brickie-gen; brickie-core too if present) are packed by freeze.py into an uncompressed tar embedded
# in it, unpacked at runtime to a temp dir and interpreted by the system python3; the launcher points
# BRICKIE_GEN / BRICKIE_CORE at the unpacked copies ⇒ the seed directory is self-contained (see the
# header comment in cxx/launcher.cpp).
#
# **brickie-core degradation**: when the Rust side (`rust/`, another agent) has not produced it yet,
# `make cxx` / `make` must still succeed — this file uses `CORE_EMBED := $(if $(wildcard ...))` for
# conditional embedding: embed if the file exists, otherwise skip and print a notice. But
# `make prebuilt` **requires all three** (the seed is what lets you start without a compiler), so it
# FAILs when core is missing.
#
# The source tree keeps **no** .o/.a/executables — generated-artifact discipline (C6) and state-dir
# discipline (BRV-D5): the whole output dir is git-ignored (repo root .gitignore: build/); only
# prebuilts/ is committed.
#
# Divergence from §9.2 (to be fed back): the design says `cxx/ # CMake: libbrickie-gen + brickie-gen
# executable`, but this machine has no cmake; the first cut uses an equivalent minimal Makefile keeping
# the same library/executable split, only the **output location** moves to the host out-tree and adds
# an L5 entry ELF (see README D-1/D-6/D-7/D-8).

include ../../../mk/host.mk

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
AR       ?= ar
INSTALL  ?= install
PYTHON   ?= python3

# The L2 generator only renders templates: json/diag/text are infrastructure, render is the only command.
# (The old rules/gen_new — name contract / taxonomy / phase / path layout / `new` — is deleted:
#  decisions belong to brickie-core (Rust), plans come from plan-new / plan-init.)
SRCS := json.cpp diag.cpp text.cpp render.cpp
OBJS := $(addprefix $(HOST_OBJ_DIR)/cxx/,$(SRCS:.cpp=.o))
HDRS := $(wildcard *.h)

MAIN_OBJ := $(HOST_OBJ_DIR)/cxx/main.o
LIB := $(HOST_LIB_DIR)/libbrickie-gen.a
BIN := $(HOST_BIN_DIR)/brickie-gen

# ------------------------------------------------ L5 entry ELF (self-contained payload)
# Payload contents = everything needed at runtime: L5 front-end + templates + schema + **native tools**
# (see PAYLOAD_EMBEDS below). ⚠ This list **must stay in sync with freeze.py's PAYLOAD_ROOTS**
# (python/templates/schema): a narrower glob causes **silent inconsistency where source edits do not
# repack the payload** (seen before: listing only python/brickie/*.py + templates meant edits to
# python/brickie/README.md or schema/*.json did not trigger a repack).
TOOL_ROOT    := ..
FREEZE_PY    := $(TOOL_ROOT)/freeze.py
PAYLOAD_ROOTS := python templates schema
PAYLOAD_SRCS := $(shell find $(addprefix $(TOOL_ROOT)/,$(PAYLOAD_ROOTS)) -type f \
                    ! -path '*/__pycache__/*' ! -name '*.pyc' 2>/dev/null)
PAYLOAD_TAR  := $(HOST_OUT)/share/brickie/payload.tar
PAYLOAD_C    := $(HOST_OBJ_DIR)/cxx/brickie_payload.cpp
PAYLOAD_OBJ  := $(HOST_OBJ_DIR)/cxx/brickie_payload.o
LAUNCHER_OBJ := $(HOST_OBJ_DIR)/cxx/launcher.o
LAUNCHER_BIN := $(HOST_BIN_DIR)/brickie

# L0/L1 Rust core: installed here by `make core` (cargo) in `tools/brickie`.
CORE_BIN := $(HOST_BIN_DIR)/brickie-core

# **Native tools** embedded into `brickie` (requirement: "tools like brickie-gen must be compiled into
# the brickie elf"):
#   * brickie-gen  always embedded (built by this Makefile);
#   * brickie-core conditionally embedded — when the Rust agent has not produced it, the build must not
#     fail; just skip it and print a notice.
# Left side is the artifact in this tree, right side is the path inside the payload; the payload path
# is executable and the launcher uses it to set BRICKIE_GEN / BRICKIE_CORE. To add e.g.
# `brickie-verify` later, append one line here.
CORE_EMBED    := $(if $(wildcard $(CORE_BIN)),$(CORE_BIN)=bin/brickie-core,)
PAYLOAD_EMBEDS := $(BIN)=bin/brickie-gen $(CORE_EMBED)

# Bootstrap seed (committed): **three files** — brickie (self-contained) + brickie-gen + brickie-core.
PREBUILT_BIN          := $(PREBUILT_BRICKIE_BIN_DIR)/brickie-gen
PREBUILT_LAUNCHER_BIN := $(PREBUILT_BRICKIE_BIN_DIR)/brickie
PREBUILT_CORE_BIN     := $(PREBUILT_BRICKIE_BIN_DIR)/brickie-core

.PHONY: all clean selftest launcher-smoke prebuilt prebuilt-check \
        print-host-triple print-host-bin-dir print-prebuilt-bin-dir print-launcher-bin

all: $(BIN) $(LAUNCHER_BIN)

$(BIN): $(MAIN_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(HOST_OBJ_DIR)/cxx/%.o: %.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Entry ELF: launcher + generated payload array
$(LAUNCHER_BIN): $(LAUNCHER_OBJ) $(PAYLOAD_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(LAUNCHER_OBJ): launcher.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(PAYLOAD_OBJ): $(PAYLOAD_C)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Payload: repack when sources, the packer or **any embedded tool** changes (tools live in the
# payload, so changing one changes the ELF). freeze.py guarantees byte-for-byte reproducibility
# (a prerequisite for the bootstrap seed).
_EMBED_SRCS := $(foreach pair,$(PAYLOAD_EMBEDS),$(firstword $(subst =, ,$(pair))))
$(PAYLOAD_C): $(PAYLOAD_SRCS) $(FREEZE_PY) $(_EMBED_SRCS)
	@mkdir -p $(dir $@) $(dir $(PAYLOAD_TAR))
	@if [ ! -f "$(CORE_BIN)" ]; then \
	    echo "note: $(CORE_BIN) not found -- payload will not contain brickie-core"; \
	    echo "      (run \`make core\` first, then rebuild the entry ELF; embedding brickie-gen only for now)"; \
	fi
	$(PYTHON) $(FREEZE_PY) --c $@ --tar $(PAYLOAD_TAR) \
	    $(foreach pair,$(PAYLOAD_EMBEDS),--embed $(pair))

selftest: $(BIN)
	$(BIN) --selftest

# Smoke: can the entry ELF start up (it carries its own Python payload, independent of the source tree)
launcher-smoke: $(LAUNCHER_BIN)
	$(LAUNCHER_BIN) --version

# core is produced by `make core` (cargo) in `tools/brickie`; this Makefile does not build Rust.
# This empty rule exists only to emit a **readable FAIL** when core is missing, instead of make's
# "No rule to make target".
$(CORE_BIN):
	@echo "FAIL: missing $(CORE_BIN) -- the bootstrap seed requires all three of {brickie,brickie-gen,brickie-core}" >&2
	@echo "      run first: make -C .. core   (needs rust/ and cargo)" >&2
	@false

# Publish the bootstrap seed: only writes when "seed != this build" (republishing produces no diff).
prebuilt: $(BIN) $(LAUNCHER_BIN) $(CORE_BIN)
	@mkdir -p $(dir $(PREBUILT_BIN))
	@for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)" \
	             "$(CORE_BIN):$(PREBUILT_CORE_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ -f "$$dst" ] && cmp -s "$$src" "$$dst"; then \
	        echo "prebuilt already up to date: $$dst"; \
	    else \
	        $(INSTALL) -m 0755 "$$src" "$$dst"; \
	        echo "prebuilt published: $$dst"; \
	    fi; \
	done

# Gate: are the three seed files behind the sources (content only, no writes). Missing/stale ⇒ exit 1.
prebuilt-check: $(BIN) $(LAUNCHER_BIN)
	@status=0; \
	for pair in "$(BIN):$(PREBUILT_BIN)" "$(LAUNCHER_BIN):$(PREBUILT_LAUNCHER_BIN)" \
	             "$(CORE_BIN):$(PREBUILT_CORE_BIN)"; do \
	    src="$${pair%%:*}"; dst="$${pair##*:}"; \
	    if [ ! -f "$$src" ]; then \
	        echo "FAIL: missing build artifact: $$src (for core run: make -C .. core)" >&2; status=1; \
	    elif [ ! -f "$$dst" ]; then \
	        echo "FAIL: missing bootstrap seed: $$dst (run make prebuilt)" >&2; status=1; \
	    elif cmp -s "$$src" "$$dst"; then \
	        echo "ok   bootstrap seed matches this build: $$dst"; \
	    else \
	        echo "FAIL: bootstrap seed behind sources: $$dst (run make prebuilt to republish)" >&2; status=1; \
	    fi; \
	done; \
	exit $$status

# Gives build gates/cases "who is the current host and where are the artifacts" (no re-deriving the
# mapping in shell).
print-host-triple:
	@echo $(HOST_TRIPLE)
print-host-bin-dir:
	@echo $(HOST_BIN_DIR)
print-prebuilt-bin-dir:
	@echo $(PREBUILT_BRICKIE_BIN_DIR)
print-launcher-bin:
	@echo $(LAUNCHER_BIN)

clean:
	rm -rf $(HOST_OUT)
	@# migration fallback: remove old artifacts left in the source tree by the first cut (current rules no longer create them)
	rm -f $(wildcard *.o) libbrickie-gen.a brickie-gen
	@# note: clean does **not** touch prebuilts/ -- the bootstrap seed is an asset committed with the sources, not a derived file.
