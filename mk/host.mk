# mk/host.mk - host platform triple and host tool output directories
#
# Layout (like Android's out/host/...):
#
#   build/host/<host-arch>/<host-os>/bin/   <- host executables (brickie-core + brickie-gen + the brickie launcher ELF)
#   build/host/<host-arch>/<host-os>/lib/   <- host static libs (e.g. libbrickie-gen.a)
#   build/host/<host-arch>/<host-os>/obj/   <- host intermediates (.o)
#
#   host-arch: x86-64 / aarch64 / ...   (CPU architecture)
#   host-os:   linux / darwin / win     (operating system)
#
# There is also a committed copy of the host artifacts (the bootstrap seed, like Android's
# prebuilts/):
#
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie       <- L5 launcher ELF (embeds the Python payload)
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie-gen   <- L2 renderer
#   prebuilts/seed/brickie/<host-arch>/<host-os>/bin/brickie-core  <- L0/L1 Rust core
#
#   `build/host/**` is this machine's build output (derived, not committed); `prebuilts/**`
#   is the seed committed with the sources, so a fresh checkout without g++/cargo can run
#   brickie right away - and it is the starting point for brickie eventually bootstrapping
#   its own build (see ADR 0004). The three ship as a set; the launcher ELF already embeds
#   both native tools (the seed directory is a redundant copy for direct dev use).
#
# Single source of truth: the arch/os mapping lives only in tools/host-detect.sh; this file
# merely turns it into make variables. Usage: `include <rel>/mk/host.mk` from a Makefile -
# the repo root is derived from **this file's own location**, so make yields the same
# directories from any cwd (key to out-of-tree builds).
#
# Overridable variables:
#   HOST_BUILD_ROOT  default <repo>/build (holds host/ and image artifacts)
#   PREBUILTS_ROOT   default <repo>/prebuilts (bootstrap seed; distinct from the download cache prebuilts/toolchain/)
#   BRICKIE_HOST_ARCH / BRICKIE_HOST_OS  passed through to host-detect.sh (CI foreign-host construction)
#
# Name note: `prebuilts/toolchain/` (singular: download cache + unpacked external toolchain;
# derived, large, not committed) and `prebuilts/` (plural: the bootstrap seed committed with
# the sources) are two different things; do not merge them.

MK_HOST_MK := $(lastword $(MAKEFILE_LIST))
REPO_ROOT  := $(abspath $(dir $(abspath $(MK_HOST_MK)))/..)

HOST_DETECT ?= $(REPO_ROOT)/tools/host-detect.sh

# Detect once, split into two fields. Two guards: script missing / script ran but printed
# nothing - both fail immediately as an environment error rather than silently degrading to
# an empty path segment (which would write artifacts to places like build/host//bin).
ifeq ($(wildcard $(HOST_DETECT)),)
$(error host detection script not found: $(HOST_DETECT) (required by mk/host.mk; check the checkout is complete))
endif
_HOST_DETECTED := $(shell bash $(HOST_DETECT))

HOST_ARCH   := $(word 1,$(_HOST_DETECTED))
HOST_OS     := $(word 2,$(_HOST_DETECTED))

ifeq ($(strip $(HOST_ARCH))$(strip $(HOST_OS)),)
$(error host detection script produced no output ($(HOST_DETECT)); run `bash $(HOST_DETECT)` alone to see the error)
endif

HOST_TRIPLE := $(HOST_ARCH)/$(HOST_OS)

HOST_BUILD_ROOT ?= $(REPO_ROOT)/build
HOST_OUT     := $(HOST_BUILD_ROOT)/host/$(HOST_TRIPLE)
HOST_BIN_DIR := $(HOST_OUT)/bin
HOST_LIB_DIR := $(HOST_OUT)/lib
HOST_OBJ_DIR := $(HOST_OUT)/obj

# Bootstrap seed (committed): prebuilts/seed/brickie/<host-arch>/<host-os>/bin/{brickie,brickie-gen,brickie-core}
PREBUILTS_ROOT           ?= $(REPO_ROOT)/prebuilts
PREBUILT_BRICKIE_DIR     := $(PREBUILTS_ROOT)/seed/brickie/$(HOST_TRIPLE)
PREBUILT_BRICKIE_BIN_DIR := $(PREBUILT_BRICKIE_DIR)/bin
