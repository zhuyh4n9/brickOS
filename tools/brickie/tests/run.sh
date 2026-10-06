#!/usr/bin/env bash
# brickie v0.1 — 端到端用例(P0 批次: `new` / §10 的 V-1 / V-9 局部)
#
# 每个用例都在**临时目录**里跑: `brickie new` 的产物是仓库级路径(§8.3 顶层即 namespace),
# 不能污染原型树。退出码口径见 BRV-D9(0 成功 / 1 校验红 / 2 用法或环境错)。
#
# 用法: bash tests/run.sh        (一般经 `make test`)
set -u

TOOL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export BRICKIE_TOOL_ROOT="$TOOL_ROOT"
export PYTHONPATH="$TOOL_ROOT/python"
export PYTHONDONTWRITEBYTECODE=1   # 测试不往源码树里丢 __pycache__
PY="${PYTHON:-$(command -v python3)}"   # 绝对路径: V-9 用例要把 PATH 清空

PASS=0
FAIL=0
TMPROOT="$(mktemp -d)"
trap 'rm -rf "$TMPROOT"' EXIT

ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (期望 $3, 实得 $2)"; fi; }

# 在工作区 $1 里跑 brickie, 其余参数照传; stdout/stderr 落到 $WORK/out, $WORK/err
run() {
    local work="$1"; shift
    ( cd "$work" && "$PY" -m brickie "$@" >"$work/out" 2>"$work/err" )
    echo $?
}

new_work() { local d; d="$(mktemp -d "$TMPROOT/XXXXXX")"; echo "$d"; }

echo "=== brickie v0.1 端到端用例 ==="

# ---------------------------------------------------------------- 0. host 产物布局(Android 式 out)
# 工具是**宿主**程序 ⇒ 出树到 build/host/<host-arch>/<host-os>/bin(见 mk/host.mk)。
# 这条同时是"前端能不能找到刚编出来的生成器"的前置: 落点与查找口径必须一致。
REPO_ROOT="$(cd "$TOOL_ROOT/../.." && pwd)"
HOST_ARCH="$(bash "$TOOL_ROOT/../host-detect.sh" --arch)"
HOST_OS="$(bash "$TOOL_ROOT/../host-detect.sh" --os)"
HOST_BIN="$REPO_ROOT/build/host/$HOST_ARCH/$HOST_OS/bin"
check "生成器出树到 build/host/<host-arch>/<host-os>/bin" \
    "$([ -x "$HOST_BIN/brickie-gen" ] && echo yes || echo no)" "yes"
check "入口 ELF 出树(brickie)" \
    "$([ -x "$HOST_BIN/brickie" ] && echo yes || echo no)" "yes"
check "源码树不留中间产物(cxx/*.o)" \
    "$(find "$TOOL_ROOT/cxx" -maxdepth 1 -name '*.o' | wc -l | tr -d ' ')" "0"
check "源码树不留旧落点可执行(cxx/brickie-gen)" \
    "$([ -e "$TOOL_ROOT/cxx/brickie-gen" ] && echo yes || echo no)" "no"
# python 侧映射是 host-detect.sh 的镜像; 漂移会让前端找不到本树产物而退到 PATH
check "python hostinfo 与 host-detect.sh 同口径" \
    "$("$PY" -c 'import sys; sys.path.insert(0, sys.argv[1]); from brickie.hostinfo import host_triple_str; print(host_triple_str())' "$TOOL_ROOT/python")" \
    "$HOST_ARCH/$HOST_OS"

# ---------------------------------------------------------------- 0a. 入口 ELF(Python 前端 + 原生工具都在里面)
# 需求: "brickie 的 python 代码也需要编译为 elf" + "brickie-gen 等工具均需要编译到
# python 的 brickie elf 中" ⇒ 真 ELF(魔数 7f 45 4c 46), 且 Python 包、模板与
# **原生工具**(bin/brickie-gen)全嵌在二进制里(零依赖入口, 见 ADR-0004 §7)。
elf_magic() { head -c 4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
check "入口 ELF 是真 ELF(\\x7fELF)" "$(elf_magic "$HOST_BIN/brickie")" "7f454c46"
check "入口 ELF --version 退出码 0" \
    "$("$HOST_BIN/brickie" --version >/dev/null 2>&1; echo $?)" "0"
check "入口 ELF --version 打印版本" \
    "$("$HOST_BIN/brickie" --version 2>/dev/null | grep -c '^brickie 0.1.0')" "1"
# 嵌入的证据: 载荷里应有可执行的 bin/brickie-gen(而不是只靠同目录的独立件)。
check "载荷里嵌了可执行的原生工具(bin/brickie-gen)" \
    "$(tar tvf "$REPO_ROOT/build/host/$HOST_ARCH/$HOST_OS/share/brickie/payload.tar" 2>/dev/null | grep -c '^-rwx.*bin/brickie-gen$')" "1"

# **单文件自包含**: 只拷 `brickie` 一个文件到别处(没有 brickie-gen / 源码树 /
# PYTHONPATH / BRICKIE_TOOL_ROOT), 仍必须能跑 —— 原生工具就在载荷里。
W="$(new_work)"
DIST="$W/dist"; DISTWORK="$W/work"
mkdir -p "$DIST" "$DISTWORK"
cp "$HOST_BIN/brickie" "$DIST/"
check "独立分发目录里只有 brickie 一个文件" \
    "$(find "$DIST" -maxdepth 1 -type f | wc -l | tr -d ' ')" "1"
( cd "$DISTWORK" && env -i PATH=/usr/bin:/bin "$DIST/brickie" new ability service/crypto --subkind service \
    >"$DISTWORK/out" 2>"$DISTWORK/err" )
check "独立分发(仅一个 ELF, 空环境)new 成功" "$?" "0"
check "独立分发: 6 件产物齐全(证明内嵌 brickie-gen 被调用)" \
    "$([ -f "$DISTWORK/build/gen/service/crypto/plugin_desc.c" ] && [ -f "$DISTWORK/service/crypto/plugin.toml" ] && echo yes || echo no)" "yes"
check "独立分发: 不依赖源码树" \
    "$("$DIST/brickie" --version 2>/dev/null | grep -c '^brickie 0.1.0')" "1"

# ---------------------------------------------------------------- 0b. 自举种子(进版本库)
# prebuilts/seed/brickie/<host-arch>/<host-os>/bin 是与 build/host/... 同构的**种子**:
# 全新 checkout 没有 g++ 也能直接跑 brickie。种子落后于源码是本用例要抓的红。
SEED_BIN="$REPO_ROOT/prebuilts/seed/brickie/$HOST_ARCH/$HOST_OS/bin"
check "自举种子就位(prebuilts/seed/brickie/<host-arch>/<host-os>/bin)" \
    "$([ -x "$SEED_BIN/brickie-gen" ] && echo yes || echo no)" "yes"
check "自举种子含入口 ELF(brickie)" \
    "$([ -x "$SEED_BIN/brickie" ] && echo yes || echo no)" "yes"
check "种子 brickie-gen 与本次构建逐字节一致(未过期)" \
    "$(cmp -s "$HOST_BIN/brickie-gen" "$SEED_BIN/brickie-gen" && echo same || echo diff)" "same"
check "种子 brickie(入口 ELF)与本次构建逐字节一致(未过期)" \
    "$(cmp -s "$HOST_BIN/brickie" "$SEED_BIN/brickie" && echo same || echo diff)" "same"

# bootstrap: 把仓库换成一个只有 prebuilts/、没有 build/ 的"全新 checkout", 前端仍须能跑。
# （BRICKIE_REPO_ROOT 指到假仓库; 模板仍从真 TOOL_ROOT 取。）
W="$(new_work)"
FAKE_REPO="$W/fakerepo"
mkdir -p "$FAKE_REPO/prebuilts/seed/brickie/$HOST_ARCH/$HOST_OS/bin"
cp "$SEED_BIN/brickie-gen" "$FAKE_REPO/prebuilts/seed/brickie/$HOST_ARCH/$HOST_OS/bin/brickie-gen"
( cd "$W" && BRICKIE_REPO_ROOT="$FAKE_REPO" "$PY" -m brickie new ability service/crypto --subkind service \
    >"$W/out" 2>"$W/err" )
check "无 build/ 时用 prebuilts 种子(bootstrap 路径)" "$?" "0"
check "bootstrap 下产物齐全" \
    "$([ -f "$W/build/gen/service/crypto/plugin_desc.c" ] && echo yes || echo no)" "yes"
check "bootstrap 下没去找本机 build/(假仓库无 build/)" \
    "$([ -e "$FAKE_REPO/build" ] && echo yes || echo no)" "no"

# ---------------------------------------------------------------- 1. 四类骨架(V-1 一半)
for ptype in app interface ability platform; do
    W="$(new_work)"
    case "$ptype" in
        ability) NAME="service/crypto"; EXTRA="--subkind service" ;;
        app)     NAME="app/hsm";        EXTRA="" ;;
        interface) NAME="iface/posix";  EXTRA="" ;;
        platform) NAME="platform/qemu-aarch64"; EXTRA="" ;;
    esac
    RC="$(run "$W" new "$ptype" "$NAME" $EXTRA)"
    check "new $ptype: 退出码 0" "$RC" "0"
    check "new $ptype: plugin.toml 就位" "$([ -f "$W/$NAME/plugin.toml" ] && echo yes || echo no)" "yes"
    check "new $ptype: 头文件路径 = include/<short>/<short>.h" \
        "$([ -f "$W/$NAME/include/${NAME##*/}/${NAME##*/}.h" ] && echo yes || echo no)" "yes"
    check "new $ptype: 生成物落仓库级 build/gen/" \
        "$([ -f "$W/build/gen/$NAME/plugin_desc.c" ] && echo yes || echo no)" "yes"
    check "new $ptype: 报告写了 6 件" "$(grep -cE '^  (人写|生成) ' "$W/out")" "6"
done

# ---------------------------------------------------------------- 2. 生成物形态与假定
W="$(new_work)"
run "$W" new ability service/crypto --subkind service >/dev/null
GEN="$W/build/gen/service/crypto/plugin_desc.c"
check "生成物首行带 brickie:generated 标记" \
    "$(head -1 "$GEN" | grep -c 'brickie:generated')" "1"
check "生成物 ver 为四段" "$(grep -c '\.ver        = { 0, 1, 0, 0 }' "$GEN")" "1"
check "生成物明示未定稿假设(W-2/A-14/W-6)" \
    "$(grep -cE 'W-2|A-14|W-6' "$GEN")" "4"
check "生成物采用 static + .br_plugins(3-01 §13.3)" \
    "$(grep -c 'static const br_plugin_t BR_PLUGIN_SECTION' "$GEN")" "1"
check "人写文件**不带**生成物标记" \
    "$(grep -c 'brickie:generated' "$W/service/crypto/plugin.toml")" "0"
check "plugin.toml 声明 subkind(ability)" \
    "$(grep -c '^subkind     = "service"' "$W/service/crypto/plugin.toml")" "1"
check "plugin.toml 的 phase 由 subkind 推导 = late" \
    "$(grep -c '^phase       = "late"' "$W/service/crypto/plugin.toml")" "1"
check "plugin.toml 预置一条 [[export]] 且 api_iface = api_type" \
    "$(grep -c '^api_iface    = "native"' "$W/service/crypto/plugin.toml")" "1"
check "符号前缀不带 br_(core 独占, 4-02 §1)" \
    "$(grep -c 'int crypto_early_init' "$W/service/crypto/src/crypto.c")" "1"

# ---------------------------------------------------------------- 3. 相位推导(逐类)
W="$(new_work)"; run "$W" new platform platform/qemu-aarch64 >/dev/null
check "phase: platform → early" "$(grep -c '^phase       = "early"' "$W/platform/qemu-aarch64/plugin.toml")" "1"
W="$(new_work)"; run "$W" new app app/hsm >/dev/null
check "phase: app → app" "$(grep -c '^phase       = "app"' "$W/app/hsm/plugin.toml")" "1"
check "app 模板不写 subkind" "$(grep -c '^subkind' "$W/app/hsm/plugin.toml")" "0"
W="$(new_work)"; run "$W" new interface iface/posix >/dev/null
check "phase: interface → late" "$(grep -c '^phase       = "late"' "$W/iface/posix/plugin.toml")" "1"

# ---------------------------------------------------------------- 4. 已存在 ⇒ BRV-GEN-0002
W="$(new_work)"
run "$W" new ability service/crypto --subkind service >/dev/null
RC="$(run "$W" new ability service/crypto --subkind service)"
check "重复 new(人写文件已存在): 退出码 1" "$RC" "1"
check "重复 new: 报 BRV-GEN-0002" "$(grep -c 'BRV-GEN-0002' "$W/err")" "5"

# ---------------------------------------------------------------- 5. --force 只对生成物目录生效
W="$(new_work)"
mkdir -p "$W/build/gen/service/crypto"
printf '/* 人手写的, 不是生成物 */\n' >"$W/build/gen/service/crypto/plugin_desc.c"
RC="$(run "$W" new ability service/crypto --subkind service)"
check "生成物位置放了非生成物: 退出码 1" "$RC" "1"
check "报 BRV-GEN-0002(非生成物)" "$(grep -c 'BRV-GEN-0002' "$W/err")" "1"
RC="$(run "$W" new ability service/crypto --subkind service --force)"
check "--force 越过生成物目录冲突: 退出码 0" "$RC" "0"
check "--force 后确为生成物" \
    "$(head -1 "$W/build/gen/service/crypto/plugin_desc.c" | grep -c 'brickie:generated')" "1"
check "--force **不**越过人写文件(此时无人写文件)" \
    "$([ -f "$W/service/crypto/plugin.toml" ] && echo yes || echo no)" "yes"

# ---------------------------------------------------------------- 6. 用法错 ⇒ 退出码 2
W="$(new_work)"
check "未知 plugin_type: 退出码 2" "$(run "$W" new widget svc/x)" "2"
check "未知 api_type: 退出码 2" "$(run "$W" new ability service/crypto --api vm --subkind service)" "2"
check "未知 lang: 退出码 2" "$(run "$W" new ability service/crypto --lang rust --subkind service)" "2"
check "名字契约违例(大写): 退出码 2" "$(run "$W" new ability Service/Crypto)" "2"
check "名字契约违例(双斜杠): 退出码 2" "$(run "$W" new ability service//crypto)" "2"
check "ability 裸名缺 --subkind: 退出码 2" "$(run "$W" new ability sched-coop)" "2"
check "非 ability 给 --subkind: 退出码 2" "$(run "$W" new app app/hsm --subkind service)" "2"
check "缺位置参数: 退出码 2" "$(run "$W" new ability)" "2"
check "未知子命令: 退出码 2" "$(run "$W" frobnicate)" "2"

# ---------------------------------------------------------------- 7. 分类学诊断
W="$(new_work)"
check "api_type=third_party: 退出码 2" "$(run "$W" new ability service/crypto --api third_party)" "2"
check "api_type=third_party: 报 BRV-TAX-0014" "$(grep -c 'BRV-TAX-0014' "$W/err")" "1"
check "api_type=third_party: 不产出任何文件" "$(find "$W" -name '*.toml' -o -name '*.c' | wc -l | tr -d ' ')" "0"

W="$(new_work)"
check "裸名 + 显式 subkind: 退出码 0" "$(run "$W" new ability sched-coop --subkind scheduler)" "0"
check "裸名: 报推荐形态 warning(BRV-TAX-0013)" "$(grep -c 'BRV-TAX-0013' "$W/err")" "1"
check "裸名 + 显式 subkind: phase = early" "$(grep -c '^phase       = "early"' "$W/sched-coop/plugin.toml")" "1"

W="$(new_work)"
RC="$(run "$W" new ability service/crypto --subkind fs)"
check "subkind 推导与显式冲突: 退出码 1" "$RC" "1"
check "subkind 冲突: 报 BRV-TAX-0015" "$(grep -c 'BRV-TAX-0015' "$W/err")" "1"

# ---------------------------------------------------------------- 8. --json 契约
W="$(new_work)"
run "$W" new ability service/crypto --subkind service --json >/dev/null
check "--json 输出是合法 JSON 且 exit_code=0" \
    "$("$PY" -c 'import json,sys;print(json.load(open(sys.argv[1]))["exit_code"])' "$W/out")" "0"
check "--json 含 planned 6 条" \
    "$("$PY" -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["planned"]))' "$W/out")" "6"
check "--json 含 written 6 条" \
    "$("$PY" -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["written"]))' "$W/out")" "6"
check "--json 里 diagnostics 空(无警告/错误)" \
    "$("$PY" -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["diagnostics"]))' "$W/out")" "0"

# ---------------------------------------------------------------- 9. 确定性(V-2 的前置)
W1="$(new_work)"; W2="$(new_work)"
run "$W1" new ability service/crypto --subkind service >/dev/null
run "$W2" new ability service/crypto --subkind service >/dev/null
DIFF="$( (cd "$W1" && find . -type f | sort | xargs sha256sum) >"$W1/sums"; \
        (cd "$W2" && find . -type f | sort | xargs sha256sum) >"$W2/sums"; \
        diff <(sed 's|  ./|  |' "$W1/sums") <(sed 's|  ./|  |' "$W2/sums") | wc -l | tr -d ' ' )"
check "两次生成逐字节一致(不含 out/err)" "$DIFF" "0"

# ---------------------------------------------------------------- 10. --version / --help
W="$(new_work)"
check "--version 退出码 0" "$(run "$W" --version)" "0"
check "--version 打印版本" "$(grep -c '^brickie 0.1.0' "$W/out")" "1"
check "无子命令: 退出码 2" "$(run "$W")" "2"
run "$W" new --help >/dev/null
check "new --help 退出码 0" "$?" "0"
check "--help 不列出未交付命令(§5.5 反向验收)" \
    "$(grep -cE '^    (check|gen|dep|ver|iface|build|test|run|pack|dbg)\b' "$W/out")" "0"

# ---------------------------------------------------------------- 11. 零编译依赖(V-9 局部)
# PATH 里什么都找不到(连 cc/cargo/nm 都没有), 命令仍须成功。
W="$(new_work)"
( cd "$W" && env -i PATH=/nonexistent BRICKIE_TOOL_ROOT="$TOOL_ROOT" \
    PYTHONPATH="$TOOL_ROOT/python" "$PY" -m brickie new ability service/crypto --subkind service \
    >"$W/out" 2>"$W/err" )
check "无 cc/cargo/nm 环境下 new 仍成功(V-9 局部)" "$?" "0"
check "零依赖环境下产物齐全" \
    "$([ -f "$W/build/gen/service/crypto/plugin_desc.c" ] && echo yes || echo no)" "yes"

# ---------------------------------------------------------------- 12. 产物自身可被消费
# 骨架就是声明面的**起点真值**。如果它连 TOML 都解析不了, 后面所有命令都无从谈起。
W="$(new_work)"
run "$W" new ability service/crypto --subkind service >/dev/null
run "$W" new app app/hsm >/dev/null
run "$W" new interface iface/posix >/dev/null
run "$W" new platform platform/qemu-aarch64 >/dev/null
TOMLCHECK="$(cd "$W" && "$PY" - <<'PY'
import pathlib, tomllib
bad = 0
seen = 0
for f in sorted(pathlib.Path('.').rglob('*.toml')):
    seen += 1
    try:
        tomllib.loads(f.read_text())
    except Exception as exc:            # 测试脚本: 要打印失败原因
        bad += 1
        print(f'  {f}: {exc}')
print(f'{seen - bad}/{seen}')
PY
)"
check "全部生成的 TOML 可被 tomllib 解析(4×plugin.toml + 4×smoke.toml)" "$TOMLCHECK" "8/8"

W="$(new_work)"
run "$W" new ability service/crypto --subkind service >/dev/null
HDR="$W/service/crypto/include/crypto/crypto.h"
GEN="$W/build/gen/service/crypto/plugin_desc.c"
check "头文件守卫三处一致(#ifndef/#define/#endif)" \
    "$(grep -c '^#.*SERVICE_CRYPTO_H' "$HDR")" "3"
check "头文件声明三个生命周期回调" \
    "$(grep -c '^int crypto_\(early_init\|init\|start\)(void);' "$HDR")" "3"
check "描述符初始化三个回调" \
    "$(grep -c '^    \.\(early_init\|init\|start\) *=' "$GEN")" "3"
check "src 定义的回调与头文件声明一致" \
    "$(grep -c '^int crypto_\(early_init\|init\|start\)(void)$' "$W/service/crypto/src/crypto.c")" "3"

# ---------------------------------------------------------------- 汇总
echo
echo "=== 通过 $PASS / 失败 $FAIL ==="
[ "$FAIL" -eq 0 ] || exit 1
exit 0
