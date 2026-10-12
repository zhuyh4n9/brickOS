#!/usr/bin/env bash
#
# brickOS prototype - host platform triple detection (single source of truth).
#
# Why a separate script:
#   Tool artifacts go out-of-tree to build/host/<host-arch>/<host-os>/bin (like Android's
#   out/host/<os>-<arch>/bin). That path depends on who the host is, and host info is
#   consumed in three places: make (mk/host.mk), the build gate (tools/check-build.sh) and
#   the e2e tests (tests/run.sh). A mapping table kept in three places is bound to drift, so
#   it is converged here; python/brickie/hostinfo.py mirrors the same mapping and
#   tests/run.sh asserts the two agree.
#
# Output (single line, space separated, so make can use $(word 1/2)):
#   <host-arch> <host-os>
# Examples:
#   x86_64/Linux   ->  x86-64 linux
#   aarch64/Darwin ->  aarch64 darwin
#   x86_64/MINGW64 ->  x86-64 win
#
# Override: BRICKIE_HOST_ARCH / BRICKIE_HOST_OS (for cross / CI / foreign-host construction)
#
# Usage: bash tools/host-detect.sh          # "x86-64 linux"
#        bash tools/host-detect.sh --arch   # "x86-64"
#        bash tools/host-detect.sh --os     # "linux"
#        bash tools/host-detect.sh --path   # "x86-64/linux" (first level under build/host/)

set -u

raw_arch="${BRICKIE_HOST_ARCH:-$(uname -m 2>/dev/null || echo unknown)}"
raw_os="${BRICKIE_HOST_OS:-$(uname -s 2>/dev/null || echo unknown)}"

# ---------------------------------------------------------------- host-arch
# Normalize to a small set of canonical names (x86-64 with a hyphen, as requested).
case "$raw_arch" in
    # canonical names pass through (env may override with an already-normalized value)
    x86-64|aarch64|x86|arm|riscv64|ppc64le|s390x) arch="$raw_arch" ;;
    x86_64|amd64)                arch="x86-64" ;;
    arm64)                       arch="aarch64" ;;
    i386|i486|i586|i686)         arch="x86" ;;
    armv6l|armv7l|armv7)         arch="arm" ;;
    # unknown: pass through lowercased (better not to normalize than to normalize wrong)
    *) arch="$(printf '%s' "$raw_arch" | tr '[:upper:]' '[:lower:]')" ;;
esac

# ---------------------------------------------------------------- host-os
case "$raw_os" in
    linux|darwin|win|freebsd)    os="$raw_os" ;;
    Linux*)                      os="linux" ;;
    Darwin)                      os="darwin" ;;
    MINGW*|MSYS*|CYGWIN*|Windows*|windows*) os="win" ;;
    FreeBSD)                     os="freebsd" ;;
    *) os="$(printf '%s' "$raw_os" | tr '[:upper:]' '[:lower:]')" ;;
esac

case "${1:-}" in
    "")      printf '%s %s\n' "$arch" "$os" ;;
    --arch)  printf '%s\n' "$arch" ;;
    --os)    printf '%s\n' "$os" ;;
    --path)  printf '%s/%s\n' "$arch" "$os" ;;
    *)       printf 'usage: %s [--arch|--os|--path]\n' "$0" >&2; exit 2 ;;
esac
