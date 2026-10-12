#!/usr/bin/env bash
#
# brickOS prototype — third_party **只读 vendor 树**门禁(ADR-0025)
#
# 为什么需要它:
#   `third_party/` 从本刀起是**逐字节上游原件**的 vendor 树(从 u-boot 钉死 commit 引入),
#   我们对三方件的**一切改动(移植/兼容/对接/自检)**都在 `adapter/` 下。这两条纪律
#   以前只写在文档里, 于是"顺手改一行三方件让它编过"永远不会被发现 —— 而那正是上一轮
#   反复犯的错(往三方件里加注释/SPDX、改 include、删裁剪)。
#
# 判据(五条, 全机械, 不需要编译器/网络):
#   ① 锁文件在场且元数据齐(repo / branch / commit / snapshot_sha256);
#   ② `third_party/uboot-imx/**` 的**每个文件**的 sha256 与锁一致  ← 核心: "不改三方代码"
#   ③ 锁里**每条**在磁盘上都存在(没有静默丢失; 少了文件也是红);
#   ④ `third_party/` 下没有 `plugin.toml` —— 三方件不是 brickOS 插件, 我们的插件在 adapter/;
#   ⑤ `third_party/` 之外**没有**任何代码 include 三方件的头 —— vendor 树只是参照物,
#      不参与编译(参与编译的是 adapter/ 里的移植件)。这条同时钉住"三方件不污染构建"。
#
# 用法: bash tools/check-third-party-pristine.sh
# 退出码: 0 = 全过, 1 = 有违规, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

LOCK="adapter/third-party-upstream.lock"
VENDOR="third_party/uboot-imx"

[ -f "$LOCK" ] || { echo "FAIL: 找不到锁文件 $LOCK" >&2; exit 2; }
[ -d "$VENDOR" ] || { echo "FAIL: 找不到 vendor 树 $VENDOR" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }
pass() { echo "ok   $*"; }

# ---- ① 元数据 ---------------------------------------------------------------
meta_ok=1
for k in repo branch commit snapshot_sha256; do
    grep -qE "^$k[[:space:]]*=" "$LOCK" || { fail "$LOCK 缺元数据字段: $k"; meta_ok=0; }
done
if [ "$meta_ok" = 1 ]; then
    commit="$(grep -E '^commit[[:space:]]*=' "$LOCK" | head -1 | sed 's/.*"\(.*\)".*/\1/')"
    pass "锁元数据齐(commit ${commit:0:12}…)"
fi

# ---- 解析 [[file]] 条目 ------------------------------------------------------
mapfile -t want < <(
    awk '
        function val(s) { sub(/^[^"]*"/, "", s); sub(/"[^"]*$/, "", s); return s }
        /^\[\[file\]\]/        { path=""; sha=""; next }
        /^path[[:space:]]*=/   { path=val($0); next }
        /^sha256[[:space:]]*=/ { sha=val($0);
                                 if (path != "" && sha != "") print path " " sha; next }
    ' "$LOCK"
)
[ "${#want[@]}" -gt 0 ] || { fail "$LOCK 里没有 [[file]] 条目"; exit 1; }

# ---- ②③ 逐文件核对 -----------------------------------------------------------
n_ok=0
: > /tmp/.tp_want.$$
for row in "${want[@]}"; do
    p="${row%% *}"; h="${row##* }"
    printf '%s\n' "$p" >> /tmp/.tp_want.$$
    f="$VENDOR/$p"
    if [ ! -f "$f" ]; then
        fail "锁里有但磁盘上没有: $f"
        continue
    fi
    got="$(sha256sum "$f" | cut -d' ' -f1)"
    if [ "$got" = "$h" ]; then
        n_ok=$((n_ok + 1))
    else
        fail "三方件被改动过: $f"
        echo "       锁  $h" >&2
        echo "       实得 $got" >&2
        echo "       ⇒ third_party/ 是**只读** vendor 树; 要适配请改 adapter/ 下的移植件" >&2
    fi
done
if [ "$status" -eq 0 ]; then
    pass "vendor 树与锁逐字节一致($n_ok 个文件)"
fi

# ②b 反向: vendor 树里不许有"锁里没有"的文件(多出来的文件 = 有人往三方件里塞东西)
extra=""
while IFS= read -r f; do
    rel="${f#"$VENDOR"/}"
    grep -qxF "$rel" /tmp/.tp_want.$$ || extra="$extra$rel"$'\n'
done < <(find "$VENDOR" -type f | sort)
rm -f /tmp/.tp_want.$$
if [ -n "$extra" ]; then
    fail "$VENDOR 里有锁里没有的文件(往只读 vendor 树里塞了东西)"
    printf '%s' "$extra" | sed 's/^/      /' >&2
else
    pass "vendor 树没有多余文件(只有锁里登记的上游原件)"
fi

# ---- ④ third_party 下不许有插件 ---------------------------------------------
if find third_party -name plugin.toml -print -quit | grep -q .; then
    fail "third_party/ 下有 plugin.toml —— 三方件不是 brickOS 插件(我们的插件在 adapter/)"
else
    pass "third_party/ 下没有 plugin.toml(插件都在 adapter/)"
fi

# ---- ⑤ 三方件的头不参与编译 ---------------------------------------------------
hits="$(grep -rn --include='*.c' --include='*.h' -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]third_party/' \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null || true)"
if [ -n "$hits" ]; then
    fail "third_party/ 之外有人 include 三方件的头(vendor 树只作参照, 不参与编译)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
else
    pass "无人 include 三方件的头(vendor 树只作参照, 不参与编译)"
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: third_party 只读 vendor 树(逐字节=上游 / 无多余文件 / 无插件 / 不参与编译)"
fi
exit "$status"
