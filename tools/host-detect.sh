#!/usr/bin/env bash
#
# brickOS prototype — host 平台三元组探测(唯一真值)
#
# 为什么单独一个脚本:
#   工具产物要**出树**放到 build/host/<host-arch>/<host-os>/bin(参考 Android 的
#   out/host/<os>-<arch>/bin)。这个路径由"宿主是谁"决定, 而宿主信息会被三处消费:
#   make(mk/host.mk)、构建门禁(tools/check-build.sh)、端到端用例(tests/run.sh)。
#   映射表放在三处必然漂移 ⇒ 收敛到这里一处; python 侧 python/brickie/hostinfo.py
#   是它的镜像(同名映射), 由 tests/run.sh 断言两者同口径。
#
# 输出(单行, 空格分隔, 方便 make 用 $(word 1/2) 取用):
#   <host-arch> <host-os>
# 例:
#   x86_64/Linux   →  x86-64 linux
#   aarch64/Darwin →  aarch64 darwin
#   x86_64/MINGW64 →  x86-64 win
#
# 覆盖: BRICKIE_HOST_ARCH / BRICKIE_HOST_OS(交叉/CI/用例构造异宿主时用)
#
# 用法: bash tools/host-detect.sh          # "x86-64 linux"
#       bash tools/host-detect.sh --arch   # "x86-64"
#       bash tools/host-detect.sh --os     # "linux"
#       bash tools/host-detect.sh --path   # "x86-64/linux"(即 build/host/ 下的一级路径)

set -u

raw_arch="${BRICKIE_HOST_ARCH:-$(uname -m 2>/dev/null || echo unknown)}"
raw_os="${BRICKIE_HOST_OS:-$(uname -s 2>/dev/null || echo unknown)}"

# ---------------------------------------------------------------- host-arch
# 统一到少数几个规范名(x86-64 带连字符, 与需求方给例一致)。
case "$raw_arch" in
    # 规范名直通(允许 env 覆盖已归一的值)
    x86-64|aarch64|x86|arm|riscv64|ppc64le|s390x) arch="$raw_arch" ;;
    x86_64|amd64)                arch="x86-64" ;;
    arm64)                       arch="aarch64" ;;
    i386|i486|i586|i686)         arch="x86" ;;
    armv6l|armv7l|armv7)         arch="arm" ;;
    # 未知: 小写透传(宁可不归一, 也不要归错)
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
