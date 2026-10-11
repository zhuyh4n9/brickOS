#!/usr/bin/env bash
# brickOS prototype — eMMC 后备镜像生成器(ADR-0021)
#
# 为什么需要它: platform 的 QEMU 参数给 `-device emmc` 挂了
#   `-drive if=none,id=emmc0,format=raw,file=build/emmc0.img`
# 而 QEMU **不会**自己创建块设备后备文件;`build/` 原本只放派生物 ⇒ 让这个脚本把空 raw
# 镜像造出来(稀疏), 由 `tests/gates.toml` 的 `[build].post` 在每次 `brickie build` 之后
# 跑一次 —— 于是任何 QEMU 门禁(以及 `brickie test` 的自动构建)都拿得到它。
#
# 为什么是"空镜像": v1 的 eMMC 端到端用例是**写特征模式再读回校验**(不依赖外部文件系统
# 镜像), 所以只要一块容量确定、内容全零的介质即可。大小 8 MiB = 16384 个 512 B 扇区,
# 足以覆盖单块/多块/跨 ADMA 边界的组合。
#
# 用法: mk-emmc-image.sh [<path>] [<size_mib>]
set -euo pipefail

out="${1:-build/emmc0.img}"
mib="${2:-8}"
bytes=$(( mib * 1024 * 1024 ))

mkdir -p "$(dirname "$out")"

cur=0
if [ -f "$out" ]; then
    cur=$(wc -c < "$out" | tr -d ' ')
fi

if [ "$cur" = "$bytes" ]; then
    echo "ok   eMMC 镜像 ${out} (${mib} MiB, 已存在)"
    exit 0
fi

if command -v truncate >/dev/null 2>&1; then
    truncate -s "$bytes" "$out"
else
    dd if=/dev/zero of="$out" bs=1M count="$mib" status=none
fi

echo "ok   eMMC 镜像 ${out} (${mib} MiB, 新建)"
