#!/usr/bin/env bash
#
# TangramOS prototype v0.1.0 — WORKAROUND 登记一致性检查
#
# 为什么需要这个脚本:
#   WORKAROUND 是**欠债**, 欠债最怕的是"忘了还、也没人知道欠着"。
#   本脚本把两处绑死 —— 源码里的 `WORKAROUND(<id>)` 标记 与
#   WORKAROUNDS.md 的登记表。任一侧多/少了, 直接报红。
#
# 用法: bash tools/check-workarounds.sh   (或 make check-workarounds)
# 退出码: 0 = 一致, 1 = 不一致, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

REGISTRY="WORKAROUNDS.md"

if [ ! -f "$REGISTRY" ]; then
    echo "FAIL: 找不到 $REGISTRY" >&2
    exit 2
fi

# 源码里的标记: WORKAROUND(<id>) —— id 形如 tg-wa-xxx-000
# 扫描全树, 但排除构建产物/版本库元数据/检查器自身/登记表本身。
mapfile -t src_ids < <(
    grep -rhoE \
        --exclude-dir=build --exclude-dir=.git --exclude-dir=tools \
        --exclude="$REGISTRY" \
        'WORKAROUND\([a-z0-9-]+\)' . 2>/dev/null \
        | sed -E 's/^WORKAROUND\((.*)\)$/\1/' \
        | sort -u
)

# 登记表里的 id: 表格行首 `| tg-wa-xxx-000 |`
mapfile -t reg_ids < <(
    grep -oE '^\|[[:space:]]*tg-wa-[a-z0-9-]+' "$REGISTRY" \
        | sed -E 's/^\|[[:space:]]*//' \
        | sort -u
)

status=0

# 1) 源码有标记但没登记
for id in ${src_ids[@]+"${src_ids[@]}"}; do
    if ! printf '%s\n' ${reg_ids[@]+"${reg_ids[@]}"} | grep -qx "$id"; then
        echo "FAIL: 源码标记了 $id, 但 $REGISTRY 里没有登记"
        status=1
    fi
done

# 2) 登记了但源码里找不到标记
for id in ${reg_ids[@]+"${reg_ids[@]}"}; do
    if ! printf '%s\n' ${src_ids[@]+"${src_ids[@]}"} | grep -qx "$id"; then
        echo "FAIL: $REGISTRY 登记了 $id, 但源码里没有对应标记"
        status=1
    fi
done

echo "源码标记 ${#src_ids[@]} 个 / 登记表 ${#reg_ids[@]} 条"

if [ "$status" -eq 0 ]; then
    echo "PASS: WORKAROUND 登记与源码标记一致"
    for id in ${src_ids[@]+"${src_ids[@]}"}; do
        echo "  - $id"
    done
else
    echo "提示: 改完代码记得同步 $REGISTRY(增删条目都算)" >&2
fi

exit "$status"
