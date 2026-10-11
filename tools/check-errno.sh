#!/usr/bin/env bash
#
# brickOS prototype — errno **单一真值**门禁(ADR-0023)
#
# 为什么需要这个脚本:
#   "内部错误码沿用 POSIX 编号"这条纪律有两个很容易悄悄坏掉的角:
#     ① **两份表**: 数值在 `br_error.h`, POSIX 名字在别处(从前在 `runtime/posix`)——
#        谁都能改错一个, 而且"产品不选 POSIX 运行时就一个名字都没有";
#     ② **三方插件各自抄一份**: 移植件为了不改上游代码, 往往写一串 `#define EIO BR_EIO`。
#        那既是第三份真值, 也是对上游代码的多余改动(ADR-0023 D-3)。
#
# 三条判据(都是机械的, 不需要编译器在场):
#   ① **名值配对**: `br_errno.h` 的每个 `#define E<名> ...` 都必须**恰好**写成 `BR_E<名>`
#      —— 于是它的值由构造等于 `br_error.h` 的数值(值本身由宿主 `ft-test` 拿 <errno.h> 对拍);
#   ② **全集覆盖**: `br_error.h` 的每个 `BR_E*` 都在 `br_errno.h` 里有一个 `E*` 名字
#      (没有"这个码没名字"的角落);
#   ③ **一处真值**: 全树(排除 build/ .git/ tools/)除 `br_errno.h` 之外**没有第二处**
#      `#define E<名>` —— 三方插件也不许自带 errno 表, 要新码就按 ADR-0023 D-4 从内核表补。
#
# 用法: bash tools/check-errno.sh   (或 make check-errno)
# 退出码: 0 = 全过, 1 = 有违规, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

ERR_H="core/include/br/core/br_error.h"
ERRNO_H="core/include/br/core/br_errno.h"

for f in "$ERR_H" "$ERRNO_H"; do
    [ -f "$f" ] || { echo "FAIL: 找不到 $f" >&2; exit 2; }
done

status=0
fail() { echo "FAIL: $*" >&2; status=1; }

# ---- 收集两侧的条目 ----------------------------------------------------------
# br_error.h: `#define BR_EPERM 1` / `#define BR_EWOULDBLOCK BR_EAGAIN`(别名)
mapfile -t br_defs < <(
    grep -E '^#define[[:space:]]+BR_E[A-Z0-9_]+[[:space:]]' "$ERR_H" | awk '{print $2}' | sort -u
)
# br_errno.h: `#define EPERM BR_EPERM`
mapfile -t e_defs < <(
    grep -E '^#define[[:space:]]+E[A-Z0-9_]+[[:space:]]+BR_E' "$ERRNO_H" | awk '{print $2, $3}' | sort -u
)

if [ "${#br_defs[@]}" -eq 0 ] || [ "${#e_defs[@]}" -eq 0 ]; then
    echo "FAIL: 有一侧一个条目都没解析出来(正则或文件结构变了?)" >&2
    exit 2
fi

# ---- ① 名值配对 --------------------------------------------------------------
names=()
for pair in "${e_defs[@]}"; do
    n="${pair%% *}"
    v="${pair##* }"
    if [ "$v" != "BR_$n" ]; then
        fail "$ERRNO_H: \`$n\` 绑到了 \`$v\`(必须是 \`BR_$n\` —— 值的真值只在 $ERR_H)"
    fi
    names+=("$n")
done

# ---- ② 全集覆盖 --------------------------------------------------------------
has_name() {  # $1 = POSIX 名字(本就带前导 E, 例如 EIO)
    local want="$1" x
    for x in "${names[@]}"; do
        [ "$x" = "$want" ] && return 0
    done
    return 1
}
for d in "${br_defs[@]}"; do
    nm="${d#BR_}"          # BR_EIO → EIO(POSIX 名字本就带前导 E)
    if ! has_name "$nm"; then
        fail "$ERR_H 有 $d, 但 $ERRNO_H 里没有对应的 POSIX 名字 \`$nm\`"
    fi
done

# ---- ③ 一处真值(第三方插件不许自带 errno 表) ---------------------------------
alt=""
for n in "${names[@]}"; do
    if [ -z "$alt" ]; then alt="$n"; else alt="$alt|$n"; fi
done

hits="$(grep -rnE --include='*.c' --include='*.h' \
            "^#define[[:space:]]+(${alt})\b" \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null \
        | grep -v "^\./$ERRNO_H:" || true)"
if [ -n "$hits" ]; then
    fail "除 $ERRNO_H 之外还有人定义 errno 名字(第二处真值)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
fi

echo "ok   errno 表: ${#names[@]} 个 POSIX 名字 / ${#br_defs[@]} 个 BR_E*(一处真值)"
if [ "$status" -eq 0 ]; then
    echo "PASS: errno 单一真值(名值配对 + 全集覆盖 + 无第二处定义)"
fi
exit "$status"
