#!/usr/bin/env bash
# brickOS prototype - host platform triple detection (single source of truth).
#
# Output is one space-separated line, "<host-arch> <host-os>", so make can use $(word 1/2):
# x86_64/Linux -> "x86-64 linux". Consumed by mk/host.mk, tools/check-build.sh, tests/run.sh and
# python/brickie/hostinfo.py (which mirrors it; tests/run.sh asserts the two agree).
#
# Override: BRICKIE_HOST_ARCH / BRICKIE_HOST_OS (cross / CI / foreign-host construction).
# Usage: bash tools/host-detect.sh [--arch|--os|--path] (--path = first level under build/host/).

set -u

raw_arch="${BRICKIE_HOST_ARCH:-$(uname -m 2>/dev/null || echo unknown)}"
raw_os="${BRICKIE_HOST_OS:-$(uname -s 2>/dev/null || echo unknown)}"

# ---------------------------------------------------------------- host-arch
# Normalize to canonical names (x86-64 with a hyphen).
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
