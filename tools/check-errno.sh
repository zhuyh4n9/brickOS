#!/usr/bin/env bash
#
# brickOS prototype — errno **单一真值**门禁(ADR-0023 → ADR-0027)
#
# 为什么需要这个脚本:
#   "内部错误码沿用 POSIX 编号"这条纪律有两个很容易悄悄坏掉的角:
#     ① **两份表**: 名字与数值分家(曾经: 名字在 br_errno.h 的别名表, 数值在 br_error.h);
#     ② **三方插件各自抄一份**: 移植件为了不改上游代码, 常写一串 `#define EIO BR_EIO`。
#        那既是第二处真值, 也是对上游代码的多余改动(ADR-0023 D-3)。
#
# 本刀(ADR-0027)把口径收敛到最简: **名字就是 POSIX 原名、且与数值同处一地** ——
# `br_error.h` 里 `#define EIO 5`, 没有 `BR_` 前缀。于是:
#   * 三方移植件里的裸 `-EIO`/`-EFSCORRUPTED` **原样成立**(不必写映射表);
#   * "本仓的 errno 名字"与"本项目自己的符号前缀"不再混在一起。
#
# 三条判据(前两条全机械, 不需要编译器在场):
#   ① **裸名**: `br_error.h` 的 errno 定义必须是 `#define E<名> <值>`(不许 `BR_E<名>`);
#   ② **一处真值**: 全树(排除 build/ third_party/)除 `br_error.h` 之外**没有第二处**
#      `#define E<名>`。三方插件不许自带 errno 表。
#   ③ **数值对拍**: 交给宿主用例 `ft-test` —— 它先捕获宿主 `<errno.h>` 的真值, 再比
#      我们的名字(见该文件里"为什么必须先包含 <errno.h>"的说明)。
#
# 用法: bash tools/check-errno.sh   (或 make check-errno)
# 退出码: 0 = 全过, 1 = 有违规, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

ERR_H="core/include/br/core/br_error.h"
[ -f "$ERR_H" ] || { echo "FAIL: 找不到 $ERR_H" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }

# ---- 真值表: `#define E<名> <值>`(排除 BR_ERR / BR_OK 等非 errno 名) ------------
mapfile -t names < <(
    grep -E '^#define[[:space:]]+E[A-Z0-9_]+[[:space:]]' "$ERR_H" \
        | awk '{print $2}' | sort -u
)
if [ "${#names[@]}" -lt 40 ]; then
    fail "$ERR_H 里的裸 errno 名太少(${#names[@]} 个) —— 是不是又退回 BR_E* 了?"
fi

# ---- ① 裸名: 不许再有 BR_E<名> 形式的 errno 定义 --------------------------------
br_defs="$(grep -cE '^#define[[:space:]]+BR_E[A-Z0-9_]+[[:space:]]+[0-9]' "$ERR_H" || true)"
if [ "${br_defs:-0}" -ne 0 ]; then
    fail "$ERR_H 里仍有 $br_defs 条 BR_E 前缀的 errno 定义 —— errno 名不许带 BR_ 前缀"
    grep -nE '^#define[[:space:]]+BR_E[A-Z0-9_]+[[:space:]]+[0-9]' "$ERR_H" | head -5 | sed 's/^/      /' >&2
fi

# ---- ② 一处真值(第三方插件 / 别处不许再定义同名) --------------------------------
alt="$(printf '%s\n' "${names[@]}" | paste -sd'|' -)"
hits="$(grep -rnE --include='*.c' --include='*.h' \
            "^#define[[:space:]]+(${alt})[[:space:]]" \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null \
        | grep -v "^\./$ERR_H:" || true)"
if [ -n "$hits" ]; then
    fail "除 $ERR_H 之外还有人定义 errno 名字(第二处真值)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
fi

if [ "$status" -eq 0 ]; then
    echo "ok   errno 表: ${#names[@]} 个裸 POSIX 名(无 BR_ 前缀 / 一处真值)"
    echo "PASS: errno 单一真值(裸名 + 一处定义); 数值对拍见宿主用例 ft-test"
fi
exit "$status"
