#!/usr/bin/env bash
#
# brickOS prototype — 第三方模块**双向隔离**门禁(ADR-0021 D-3 / ADR-0022)
#
# 为什么需要这个脚本:
#   `third_party/**` 是 GPL-2.0+ 的移植件, 而 `framework/**`、`platform/**`、`core/**`
#   保持 WTFPL。隔离政策一直是"目录级隔离", 但**判据只写在文档里** —— 没有任何机械检查。
#   于是"移植时顺手把 brickOS 的调用写进上游派生文件"这种漂移无人拦:
#   它让"哪些代码是上游的、哪些是我们的"变得不可判定, 也让将来换上游版本时无法估代价。
#
#   本门禁把政策变成四条**机械判据**。它不判"代码写得好不好", 只判**边界在不在**:
#
#   ① **目录齐备**: 每个 third_party 插件都有 `src/`(上游派生) 与 `adapter/`(本项目适配);
#   ② **单向依赖**: `src/**` 里的文件**不许**直接 `#include <br/...>`
#      —— 运行环境只能经 `adapter/` 的适配头取(如 `mmc_compat.h`/`fat_compat.h`/
#      `erofs_compat.h`)。反向(`adapter/**` include `<br/...>`)是**允许且必须**的;
#   ③ **不许反向依赖**: 全树(排除 third_party/ 自己、build/、.git/)**不许** include
#      `third_party/**` 的头 —— 框架与平台不得反向依赖三方件(与规则 `api_type_edge_reason`
#      的 `native → third_party` 禁则同旨, 但那条只管**插件依赖**, 管不到裸 `#include`);
#   ④ **署名在场**: `src/**` 每个文件都带 `SPDX-License-Identifier` —— 上游每个文件都有,
#      派生件丢了它就是署名/许可回归(本门禁是 ADR-0024 的判据)。
#
# 用法: bash tools/check-third-party-isolation.sh   (或 make check-third-party-isolation)
# 退出码: 0 = 全过, 1 = 有违规, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

TP_ROOT="third_party"
[ -d "$TP_ROOT" ] || { echo "FAIL: 找不到 $TP_ROOT" >&2; exit 2; }

status=0
fail() { echo "FAIL: $*" >&2; status=1; }
pass() { echo "ok   $*"; }

# 收集所有第三方插件目录(顶层目录 = namespace, 见 4-02)
mapfile -t plugins < <(find "$TP_ROOT" -mindepth 1 -maxdepth 1 -type d | sort)
if [ "${#plugins[@]}" -eq 0 ]; then
    echo "FAIL: $TP_ROOT 下没有插件" >&2
    exit 2
fi

for p in "${plugins[@]}"; do
    name="${p#"$TP_ROOT"/}"

    # ---- ① 目录齐备 --------------------------------------------------------
    if [ ! -d "$p/src" ]; then
        fail "$name: 缺 src/(上游派生件目录)"
        continue
    fi
    if [ ! -d "$p/adapter" ]; then
        fail "$name: 缺 adapter/(本项目适配层目录, 见 $p/adapter/README.md)"
        continue
    fi
    n_src=$(find "$p/src" -type f \( -name '*.c' -o -name '*.h' \) | wc -l)
    n_adp=$(find "$p/adapter" -type f \( -name '*.c' -o -name '*.h' \) | wc -l)
    if [ "$n_src" -eq 0 ] || [ "$n_adp" -eq 0 ]; then
        fail "$name: src/ 或 adapter/ 是空的(src=$n_src adapter=$n_adp)"
        continue
    fi
    pass "$name: src/ $n_src 件(上游派生) + adapter/ $n_adp 件(本项目)"

    # ---- ② 单向依赖: src/ 不许直接 include <br/...> -------------------------
    hits="$(grep -rn --include='*.c' --include='*.h' -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*<br/' "$p/src" 2>/dev/null || true)"
    if [ -n "$hits" ]; then
        fail "$name: src/ 直接 include 了 brickOS 头(运行环境只能经 adapter/)"
        printf '%s\n' "$hits" | sed 's/^/      /' >&2
    else
        pass "$name: src/ 不直接 include <br/...>(运行环境经 adapter/)"
    fi

    # ---- ④ 署名在场: src/ 每个文件都要有 SPDX ------------------------------
    missing=""
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if ! grep -q 'SPDX-License-Identifier' "$f"; then
            missing="$missing$f"$'\n'
        fi
    done < <(find "$p/src" -type f \( -name '*.c' -o -name '*.h' \) | sort)
    if [ -n "$missing" ]; then
        fail "$name: src/ 有文件缺 SPDX-License-Identifier(署名回归)"
        printf '%s' "$missing" | sed 's/^/      /' >&2
    else
        pass "$name: src/ 全部文件带 SPDX 标识"
    fi
done

# ---- ③ 全树不许反向 include third_party/** ----------------------------------
# 三方件是**叶子**: 框架/平台/core 依赖它们就把 GPL 代码拖进了 WTFPL 侧的包含面。
# (豁免: third_party/ 内部互相 include 是正常的分层。)
hits="$(grep -rn --include='*.c' --include='*.h' -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]third_party/' \
            --exclude-dir=build --exclude-dir=.git --exclude-dir=tools . 2>/dev/null || true)"
if [ -n "$hits" ]; then
    fail "third_party/ 之外有人 include 三方件的头(框架/平台不得反向依赖三方件)"
    printf '%s\n' "$hits" | sed 's/^/      /' >&2
else
    pass "third_party/ 之外无人 include 三方件的头(单向: 三方件是叶子)"
fi

if [ "$status" -eq 0 ]; then
    echo "PASS: 第三方模块双向隔离(目录齐备 / src 不碰 brickOS 头 / 无反向依赖 / 署名在场)"
fi
exit "$status"
