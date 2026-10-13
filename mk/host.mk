# mk/host.mk - host platform triple and host tool output directories.
# Layout (like Android's out/host/...):
#   build/host/<host-arch>/<host-os>/bin|lib|obj   host tools, static libs, intermediates
#   host-arch: x86-64 / aarch64 / ...   host-os: linux / darwin / win
#
# The committed bootstrap seed mirrors this at prebuilts/seed/brickie/<host-arch>/<host-os>/bin/
# ({brickie,brickie-gen,brickie-core}, shipped as a set; a fresh checkout without g++/cargo can
# run brickie). It is distinct from the derived download cache prebuilts/toolchain/.
#
# Single source of truth for the arch/os mapping is tools/host-detect.sh; this file only turns it
# into make variables. The repo root derives from this file's own location, so `include <rel>/mk/
# host.mk` yields the same directories from any cwd (out-of-tree builds).
#
# Overridable: HOST_BUILD_ROOT (default <repo>/build), PREBUILTS_ROOT (default <repo>/prebuilts),
# BRICKIE_HOST_ARCH / BRICKIE_HOST_OS (passed to host-detect.sh for CI foreign-host construction).

MK_HOST_MK := $(lastword $(MAKEFILE_LIST))
REPO_ROOT  := $(abspath $(dir $(abspath $(MK_HOST_MK)))/..)

HOST_DETECT ?= $(REPO_ROOT)/tools/host-detect.sh

# Detect once, split into two fields. Both guards (script missing / no output) fail immediately
# rather than degrading to an empty path segment like build/host//bin.
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

# Bootstrap seed (committed): prebuilts/seed/brickie/<host-arch>/<host-os>/bin/ (all three tools)
PREBUILTS_ROOT           ?= $(REPO_ROOT)/prebuilts
PREBUILT_BRICKIE_DIR     := $(PREBUILTS_ROOT)/seed/brickie/$(HOST_TRIPLE)
PREBUILT_BRICKIE_BIN_DIR := $(PREBUILT_BRICKIE_DIR)/bin
