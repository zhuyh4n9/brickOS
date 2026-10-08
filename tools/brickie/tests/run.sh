#!/usr/bin/env bash
# brickie v0.1 — 端到端验收回归基线(V-1…V-19 + 反向验收 N-1…N-4)。
#
# 施工图 = tests/ACCEPTANCE.md(验收矩阵); 契约 = docs/contract.md;
# 设计权威 = Design/docs/2-toolchain/brickie/brickie-v0.1.md §10。
#
# 口径(全部来自 BRV-D9 / contract):
#   退出码 0 成功 / 1 校验红 / 2 用法或环境错;
#   `--json` 是 `data` 的一个序列化, 诊断按 (code,file,span,target) 稳定排序。
#
# 三条硬纪律:
#   1. 每条用例 `cp -r` 夹具到 mktemp 目录再跑 —— 用例之间不污染;
#   2. 本套件**不调用**任何编译器(cc/g++/cargo/nm), 见 V-9;
#   3. 断言不为了变绿而放宽: 实得与期望不符就是 FAIL, 发现实现缺陷记进 ACCEPTANCE.md。
#
# 用法: bash tests/run.sh          (一般经 `make test` / `make tools-test`)
#       bash tests/run.sh -v       (打印每条用例的退出码/诊断, 便于定位)
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
. "$TESTS_DIR/lib.sh"
resolve_env

TMPROOT="$(mktemp -d)"
trap 'rm -rf "$TMPROOT"' EXIT
init_report "$TMPROOT/failures.log"
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

# 诊断数组键序(contract §3 + L5 信封)
ENVELOPE_KEYS="command,data,diagnostics,exit_code,protocol,status,written"

printf '=== brickie v0.1 端到端验收(V-1…V-19 + N-1…N-4) ===\n'
printf '宿主三元组: %s/%s\n' "$HOST_ARCH" "$HOST_OS"
printf 'core: %s\n' "$CORE"
printf 'gen : %s\n' "$GEN"

# ---------------------------------------------------------------- 0. 环境自证
section "0. 环境与二进制"
eq "core 可执行" "yes" "$([ -x "$CORE" ] && echo yes || echo no)"
eq "gen 可执行" "yes" "$([ -x "$GEN" ] && echo yes || echo no)"
W="$(new_work)"
run_brickie "$W" --version
eq "--version 退出码 0" "0" "$RC"
grep_at_least "--version 打印版本号" '^brickie 0\.1\.0$' "$OUT"
run_json "$W" --version
eqj "--version --json 协议版本" "$JQ_RESP" "protocol" "1"
eqj "--version --json 版本" "$JQ_RESP" "data.version" "0.1.0"
eqj "--version --json 语言指纹含 rust" "$JQ_RESP" "data.languages.0.name" "rust"
run_json "$W" --version --deps
eqjc "--version --deps --json 三个直接依赖" "$JQ_RESP" "data.deps" "3"
# 契约: 前端同 core 的协议号
eq "core --selftest 可直接跑" "0" "$("$CORE" --selftest >/dev/null 2>&1; echo $?)"

# ================================================================ V-1 骨架生成
section "V-1 new 四类 × c, 生成后立刻 check 0 错误"
for pair in "app:app/hsm:" "interface:iface/posix:" "ability:service/crypto:--subkind service" "platform:platform/qemu-aarch64:";
do
    ptype="${pair%%:*}"; rest="${pair#*:}"; name="${rest%%:*}"; extra="${rest#*:}"
    W="$(new_work)"
    # shellcheck disable=SC2086
    run_brickie "$W" new "$ptype" "$name" $extra
    eq "V-1 new $ptype 退出码 0" "0" "$RC"
    short="${name##*/}"
    n=0
    for f in "$name/plugin.toml" "$name/src/$short.c" "$name/include/$short/$short.h" \
             "$name/tests/smoke.toml" "$name/README.md" "build/gen/$name/plugin_desc.c"; do
        [ -f "$W/$f" ] && n=$((n + 1))
    done
    eq "V-1 new $ptype 六件产物齐备" "6" "$n"
    grep_at_least "V-1 new $ptype 描述符带生成物标记" 'brickie:generated' \
        "$W/build/gen/$name/plugin_desc.c"
    run_json "$W" check
    eq "V-1 new $ptype 立刻 check 退出码 0" "0" "$RC"
    eqj "V-1 new $ptype 立刻 check errors=0" "$JQ_RESP" "data.summary.errors" "0"
    eqj "V-1 new $ptype 人写文件未被改写(git 语义: 无写盘)" "$JQ_RESP" "written" "[]"
done
# 四类同树(ACCEPTANCE 的 fx/quad 形态, 由用例即时构造)
W="$(new_work)"
run_brickie "$W" new app app/hsm
run_brickie "$W" new interface iface/posix
run_brickie "$W" new ability service/crypto --subkind service
run_brickie "$W" new platform platform/qemu-aarch64
run_json "$W" check
eq "V-1 四类同树 check 退出码 0" "0" "$RC"
eqj "V-1 四类同树闭包含 4 个插件" "$JQ_RESP" "data.summary.closure.plugins" "4"

# ================================================================ V-2 生成物幂等
section "V-2 gen 幂等 / --check 逐字节一致"
W="$(new_work)"
run_brickie "$W" new ability service/crypto --subkind service
H1="$(tree_hash "$W")"
run_brickie "$W" gen
eq "V-2 第二次 gen 退出码 0" "0" "$RC"
grep_at_least "V-2 第二次 gen 报『已是最新』" '已是最新' "$OUT"
H2="$(tree_hash "$W")"
eq "V-2 两次 gen 逐字节一致" "$H1" "$H2"
run_brickie "$W" gen --check
eq "V-2 gen --check 一致退出码 0" "0" "$RC"
grep_at_least "V-2 gen --check 文本" '逐字节一致' "$OUT"
# 篡改生成物 ⇒ --check 必须红, 且 gen 能修回来
printf '\n/* 手改 */\n' >>"$W/build/gen/service/crypto/plugin_desc.c"
run_brickie "$W" gen --check
eq "V-2 篡改后 gen --check 退出码 1" "1" "$RC"
run_json "$W" gen --check
eqj "V-2 篡改后 --json exit_code=1" "$JQ_RESP" "exit_code" "1"
eq "V-2 篡改后 --json 无码诊断(contract §2 允许)" "null" \
    "$(jqget "$JQ_RESP" diagnostics.0.code)"
run_brickie "$W" gen
eq "V-2 gen 修复退出码 0" "0" "$RC"
run_brickie "$W" gen --check
eq "V-2 修复后 --check 退出码 0" "0" "$RC"

# ================================================================ V-3 环检测
section "V-3 环 ⇒ 完整环路径 + exit 1"
W="$(cp_fx cycle)"
run_json "$W" check
eq "V-3 check 造环退出码 1" "1" "$RC"
eq "V-3 环诊断码 BRV-MF-0001(R-9 缺口代用)" "1" \
    "$(grep -c 'BRV-MF-0001' "$ERR" || true)"
grep_at_least "V-3 文本报完整环路径" 'service/a → service/b → service/c → service/a' "$ERR"
eqj "V-3 check --json 含环诊断" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
run_json "$W" dep closure
eqj "V-3 closure --json exit_code=1" "$JQ_RESP" "exit_code" "1"
eqjc "V-3 closure --json cycles 数量" "$JQ_RESP" "data.cycles" "1"
eqj "V-3 cycles[0].path 完整(首尾同名)" "$JQ_RESP" "data.cycles.0.path" \
    '["service/a", "service/b", "service/c", "service/a"]'
eqjc "V-3 cycles[0].edges 结构化边列表 3 条" "$JQ_RESP" "data.cycles.0.edges" "3"
eqj "V-3 cycles[0].edges[0] 形状 {from,to}" "$JQ_RESP" "data.cycles.0.edges.0.from" "service/a"
eqj "V-3 cycles[0].edges[0].to" "$JQ_RESP" "data.cycles.0.edges.0.to" "service/b"

# ================================================================ V-4 版本推进表
section "V-4 事件 → 段 推进表逐行"
W="$(cp_fx ver-matrix)"
printf 'RFC: 解冻决策记录\n' >"$W/rfc.md"
segments_of() { # segments_of <work> <id> ⇒ "cg.major.minor.revise"
    run_json "$1" ver show "$2"
    printf '%s.%s.%s.%s' \
        "$(jqget "$JQ_RESP" data.segments.compat_gen)" \
        "$(jqget "$JQ_RESP" data.segments.major)" \
        "$(jqget "$JQ_RESP" data.segments.minor)" \
        "$(jqget "$JQ_RESP" data.segments.revise)"
}
run_brickie "$W" iface publish service/crypto#crypto
eq "V-4 首次 publish = 建档不推进段(S-8)" "0.1.0.0" "$(segments_of "$W" service/crypto#crypto)"
# (f) 空解冻 ⇒ 四段全不动
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-4(f) unfreeze 进入窗口" "0" "$RC"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-4(f) 空解冻 refreeze 退出码 0" "0" "$RC"
eq "V-4(f) 空解冻 ⇒ 四段全不动" "0.1.0.0" "$(segments_of "$W" service/crypto#crypto)"
# (a) 解冻 + 改已有冻结条目 + 重新冻结 ⇒ 仅 COMPAT_GEN+1
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/service/crypto/plugin.toml"
run_brickie "$W" iface refreeze service/crypto#crypto --note rfc.md
eq "V-4(a) refreeze 退出码 0" "0" "$RC"
eq "V-4(a) 改冻结条目 ⇒ 仅 COMPAT_GEN+1(MAJOR 不牵连)" "1.1.0.0" \
    "$(segments_of "$W" service/crypto#crypto)"
# (b) 新增条目 ⇒ COMPAT_GEN 不动, MINOR+1
python3 - "$W/service/crypto/plugin.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
s = s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                        'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1)
p.write_text(s)
PY
run_brickie "$W" iface publish service/crypto#crypto
eq "V-4(b) 新增条目 publish 退出码 0" "0" "$RC"
eq "V-4(b) 新增 ⇒ COMPAT_GEN 不动 + MINOR+1" "1.1.1.0" "$(segments_of "$W" service/crypto#crypto)"
# (c) 重大产品版本 ⇒ 仅 MAJOR+1(COMPAT_GEN 不动)
run_brickie "$W" ver bump service/crypto#crypto --rule major
eq "V-4(c) ver bump major 退出码 0" "0" "$RC"
eq "V-4(c) 仅 MAJOR+1, COMPAT_GEN 不动" "1.2.0.0" "$(segments_of "$W" service/crypto#crypto)"
# (d)(e) minor / revise
run_brickie "$W" ver bump service/crypto#crypto --rule minor
eq "V-4(d) minor ⇒ MINOR+1" "1.2.1.0" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule revise
eq "V-4(e) revise ⇒ REVISE+1" "1.2.1.1" "$(segments_of "$W" service/crypto#crypto)"
run_brickie "$W" ver bump service/crypto#crypto --rule compat_gen
eq "V-4 COMPAT_GEN 不在 rule 里(用法错 2)" "2" "$RC"

# ================================================================ V-5 接口发布
section "V-5 publish 产出 + 幂等 + --check 独立重算"
W="$(cp_fx publish)"
run_json "$W" iface publish service/a#a
eq "V-5 首次 publish 退出码 0" "0" "$RC"
eqj "V-5 首次 publish noop=false" "$JQ_RESP" "data.noop" "false"
eqjc "V-5 落盘 3 件(快照+CHANGELOG+lock)" "$JQ_RESP" "written" "3"
for f in "api/iface/service/a/a.toml" "api/iface/CHANGELOG.md" "brickie.lock"; do
    eq "V-5 产物就位 $f" "yes" "$([ -f "$W/$f" ] && echo yes || echo no)"
done
grep_at_least "V-5 快照头带 NOT_ABI" 'NOT_ABI' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 快照头带 hash_scope" 'hash_scope = "decl"' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 快照头带 truth" 'truth = "decl"' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 快照带 brickie:generated 标记" 'brickie:generated' "$W/api/iface/service/a/a.toml"
grep_at_least "V-5 CHANGELOG 按版本段、无日期" '^## service/a#a v0\.1\.0\.0' "$W/api/iface/CHANGELOG.md"
eq "V-5 CHANGELOG 里没有日期(逐字节可复现)" "0" \
    "$(grep -cE '[0-9]{4}-[0-9]{2}-[0-9]{2}' "$W/api/iface/CHANGELOG.md" || true)"
grep_at_least "V-5 lock 带 brickie:generated 标记" 'brickie:generated' "$W/brickie.lock"
H1="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
run_json "$W" iface publish service/a#a
eq "V-5 面未变 ⇒ 空操作退出码 0" "0" "$RC"
eqj "V-5 面未变 ⇒ noop=true" "$JQ_RESP" "data.noop" "true"
eqjc "V-5 面未变 ⇒ 不写任何文件" "$JQ_RESP" "written" "0"
H2="$(tree_hash "$W/api/iface"; printf '-'; cat "$W/brickie.lock" | sha256sum)"
eq "V-5 空操作逐字节不变" "$H1" "$H2"
run_json "$W" iface status service/a#a --check
eq "V-5 status --check 一致退出码 0" "0" "$RC"
eqj "V-5 status --check consistent=true" "$JQ_RESP" "data.consistent" "true"
run_json "$W" iface publish service/a#a --check
eq "V-5 publish --check 退出码 0" "0" "$RC"
eqjc "V-5 publish --check 不写盘" "$JQ_RESP" "written" "0"
# 独立重算不一致 ⇒ 红(手改快照里的 hash 记录)
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000"/' \
    "$W/api/iface/service/a/a.toml"
run_json "$W" iface status service/a#a --check
eq "V-5 手改快照 ⇒ status --check 退出码 1" "1" "$RC"
eqj "V-5 手改快照 ⇒ consistent=false" "$JQ_RESP" "data.consistent" "false"

# ================================================================ V-6 影响报告
section "V-6 改冻结条目的影响报告 / --note 硬门"
W="$(cp_fx dependents)"
printf 'RFC: 签名变更\n' >"$W/rfc.md"
run_brickie "$W" iface publish service/crypto#crypto
eq "V-6 建档 publish 退出码 0" "0" "$RC"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-6 unfreeze 退出码 0" "0" "$RC"
sed -i 's/int(const uint8_t\*, size_t)/int(const uint8_t*, size_t, uint8_t*)/' \
    "$W/service/crypto/plugin.toml"
run_json "$W" iface diff service/crypto#crypto
eq "V-6 diff 退出码 0" "0" "$RC"
eqj "V-6 changes[0].kind=CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-6 变更集取最严: compat_gen_changed=true" "$JQ_RESP" "data.compat_gen_changed" "true"
eqjc "V-6 直接依赖者 2 个" "$JQ_RESP" "data.dependents.direct" "2"
eqjc "V-6 传递依赖者 3 个" "$JQ_RESP" "data.dependents.transitive" "3"
eqjc "V-6 失配者 1 个" "$JQ_RESP" "data.dependents.unsatisfied" "1"
eqj "V-6 失配者点名 + 原因" "$JQ_RESP" "data.dependents.unsatisfied.0.plugin" "service/consumer"
grep_at_least "V-6 失配原因说明 compat_gen 变化" 'compat_gen 已由 0 变 1' "$OUT"
run_json "$W" iface publish service/crypto#crypto
eq "V-6 命中须解冻但缺 --note ⇒ 退出码 1" "1" "$RC"
grep_at_least "V-6 缺 --note 报 BRV-MF-0001" 'BRV-MF-0001' "$ERR"
run_json "$W" iface publish service/crypto#crypto --note rfc.md
eq "V-6 带 --note 发布退出码 0" "0" "$RC"
eqj "V-6 发布后 COMPAT_GEN 已推进" "$JQ_RESP" "data.compat_gen" "1"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-6 窗口内 publish 已消费基线 ⇒ refreeze 是空解冻" "0" "$RC"
# --note 指向不存在的文件 ⇒ 用法错 2(前端门); 用 unfreeze(面未变时 publish 是空操作, 不过门)
run_brickie "$W" iface unfreeze service/crypto#crypto --note no-such.md
eq "V-6 --note 指向不存在路径 ⇒ 用法错 2" "2" "$RC"

# ================================================================ V-7 依赖求解
section "V-7 闭包正确 / VER-0001 / VER-0002 / 预算合计"
W="$(cp_fx ver-conflict)"
run_json "$W" check
eq "V-7 compat_gen 冲突 ⇒ 退出码 1" "1" "$RC"
eq "V-7 报 BRV-VER-0001" "1" "$(grep -c 'BRV-VER-0001' "$ERR" || true)"
grep_at_least "V-7 冲突报『谁钉了哪一代 / 提供方在哪一代』" 'compat_gen=3, 而提供方当前在 4' "$ERR"
W="$(cp_fx ver-range)"
run_json "$W" check
eq "V-7 同代 range 越界 ⇒ 退出码 1" "1" "$RC"
eq "V-7 报 BRV-VER-0002" "1" "$(grep -c 'BRV-VER-0002' "$ERR" || true)"
# 闭包正确性(showcase)
W="$(cp_fx showcase)"
run_json "$W" dep closure
eq "V-7 closure 退出码 0" "0" "$RC"
eqj "V-7 闭包插件数" "$JQ_RESP" "data.selected" \
    '["app/hsm", "iface/hsm-api", "platform/qemu-aarch64", "sched/coop", "service/crypto"]'
# 拓扑序的**不变量**是"每条 init 边的提供方在前"(无 init 边的节点可任意位次)
init_order_ok() {
    python3 - "$1" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
pos = {n: i for i, n in enumerate(d.get("topo_order", []))}
bad = [(e.get("from"), e.get("to")) for e in d.get("init_edges", [])
       if not pos.get(e.get("to"), 10 ** 9) < pos.get(e.get("from"), -1)]
missing = [n for n in d.get("selected", []) if n not in pos]
print("yes" if not bad and not missing else f"no:{bad}{missing}")
PYX
}
eq "V-7 拓扑序满足每条 init 边提供方在前" "yes" "$(init_order_ok "$JQ_RESP")"
eqj "V-7 合计 RAM(platform 容量不进 Σ, S-14)" "$JQ_RESP" "data.totals.ram_kib" "48"
eqj "V-7 合计栈" "$JQ_RESP" "data.totals.stack_kib" "4"
eqj "V-7 平台容量单独给" "$JQ_RESP" "data.totals.platform_capacity.ram_kib" "8192"
eqj "V-7 预算" "$JQ_RESP" "data.totals.budget.ram_kib" "512"

# ================================================================ V-8 分类学与相位
section "V-8 三条硬禁则正反例 / 相位单调"
for fx in tax-01-app-ability tax-02-native-adapter tax-03-iface-leaf; do
    W="$(cp_fx "$fx")"
    run_json "$W" check
    eq "V-8 $fx 反例退出码 1" "1" "$RC"
    eqj "V-8 $fx 报 BRV-MF-0001(禁则无专属码, R-9)" "$JQ_RESP" "diagnostics.0.code" "BRV-MF-0001"
    grep_at_least "V-8 $fx 消息点名冲突双方" '消费者 `' "$ERR"
done
W="$(cp_fx tax-ok)"
run_json "$W" check
eq "V-8 正例 app→interface→ability 退出码 0" "0" "$RC"
eqj "V-8 正例 errors=0" "$JQ_RESP" "data.summary.errors" "0"
W="$(cp_fx phase-bad)"
run_json "$W" check
eq "V-8 R1 反例退出码 1" "1" "$RC"
eq "V-8 R1 报 BRV-DEP-0009" "1" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
W="$(cp_fx phase-assert)"
run_json "$W" check
eq "V-8 R2 反例退出码 1" "1" "$RC"
eq "V-8 R2 报 BRV-DEP-0010" "1" "$(grep -c 'BRV-DEP-0010' "$ERR" || true)"
eq "V-8 R2 反例不同时报 DEP-0009" "0" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
W="$(cp_fx phase-ok)"
run_json "$W" check
eq "V-8 sched-coop → platform 不误杀退出码 0" "0" "$RC"
eq "V-8 不误杀: 无 DEP-0009" "0" "$(grep -c 'BRV-DEP-0009' "$ERR" || true)"
eq "V-8 不误杀: 无 DEP-0010" "0" "$(grep -c 'BRV-DEP-0010' "$ERR" || true)"

# ================================================================ V-9 零编译依赖
section "V-9 零编译依赖(精简 PATH + 自举种子单文件入口)"
# 1. 精简 PATH: 只有 python3 / sh / coreutils 的 symlink —— 没有任何编译器
MINBIN="$TMPROOT/minpath"
mkdir -p "$MINBIN"
for t in python3 sh env cat cp mkdir rm mv ls cmp sort head tail wc sed grep find \
         mktemp dirname basename; do
    src="$(command -v "$t" 2>/dev/null || true)"
    [ -n "$src" ] && ln -sf "$src" "$MINBIN/$t"
done
eq "V-9 精简 PATH 里没有 cc/g++/cargo/nm" "0" \
    "$(ls "$MINBIN" | grep -cE '^(cc|gcc|g\+\+|c\+\+|cargo|nm|ld)$' || true)"
eq "V-9 精简 PATH 里 python3 可见" "1" \
    "$(PATH="$MINBIN" command -v python3 >/dev/null 2>&1 && echo 1 || echo 0)"
# 自举种子入口: 单文件自包含(只拷这一个文件), 原生件用**实际的那份**(显式注入)
SEED_LAUNCHER_LOCAL="${SEED_LAUNCHER:-}"
eq "V-9 自举种子入口存在" "yes" \
    "$([ -n "$SEED_LAUNCHER_LOCAL" ] && [ -x "$SEED_LAUNCHER_LOCAL" ] && echo yes || echo no)"
DIST="$TMPROOT/dist"
mkdir -p "$DIST"
cp "$SEED_LAUNCHER_LOCAL" "$DIST/brickie"
eq "V-9 分发目录里只有 brickie 一个文件" "1" "$(find "$DIST" -maxdepth 1 -type f | wc -l | tr -d ' ')"
W="$(new_work)"
mkdir -p "$W/home"
run_seed() { # 精简 PATH 下用种子入口跑; CORE/GEN 显式指到实际用的那份
    local work="$1"; shift
    ( cd "$work" && env -i PATH="$MINBIN" HOME="$work/home" \
        BRICKIE_CORE="$CORE" BRICKIE_GEN="$GEN" "$DIST/brickie" "$@" ) >"$OUT" 2>"$ERR"
    RC=$?
}
run_seed "$W" new ability service/crypto --subkind service
eq "V-9 闭环① new 退出码 0" "0" "$RC"
run_seed "$W" check
eq "V-9 闭环② check 退出码 0" "0" "$RC"
run_seed "$W" dep closure
eq "V-9 闭环③ dep closure 退出码 0" "0" "$RC"
run_seed "$W" iface publish service/crypto#crypto
eq "V-9 闭环④ iface publish 退出码 0" "0" "$RC"
run_seed "$W" iface status service/crypto#crypto --check
eq "V-9 闭环⑤ iface status --check 退出码 0" "0" "$RC"
run_seed "$W" gen --check
eq "V-9 闭环⑥ gen --check 退出码 0" "0" "$RC"
eq "V-9 闭环无编译器在场也能全绿" "yes" \
    "$([ -f "$W/api/iface/service/crypto/crypto.toml" ] && echo yes || echo no)"
# 2. 套件自身不得调用编译器(扫脚本里的调用形态, 只查 cargo/g++, 避免误伤散文)
grep_none "V-9 套件脚本不调用 cargo/g++" \
    '(^|[;&|(])[[:space:]]*(cargo|g\+\+)[[:space:]]' \
    "$TESTS_DIR/run.sh" "$TESTS_DIR/purity.sh" "$TESTS_DIR/lib.sh"

# ================================================================ V-10 requires_iface
section "V-10 requires_iface: schema / 不参与闭包 / 只报 info"
W="$(cp_fx req-iface)"
run_json "$W" check
eq "V-10 基线 check 退出码 0(只报 info)" "0" "$RC"
eqj "V-10 dev 下未冻结接口消费 = info" "$JQ_RESP" "diagnostics.0.severity" "info"
# ② 加/去 requires_iface, 闭包逐字节相同
W2="$(cp_fx req-iface-plain)"
run_brickie "$W" dep closure --json
A="$(cat "$OUT")"
run_brickie "$W2" dep closure --json
B="$(cat "$OUT")"
eq "V-10② 有/无 requires_iface 闭包逐字节相同" "$A" "$B"
# ③ iface show 对该字段只报 info
run_json "$W" iface show service/consumer#consumer
eq "V-10③ iface show 退出码 0" "0" "$RC"
eq "V-10③ 登记为 info(NOT_ABI + requires_iface 两条)" "2" \
    "$(grep -c '"severity": "info"' "$JQ_RESP" || true)"
grep_at_least "V-10③ 消息说明 v0.1 不扫描" '不扫描' "$JQ_RESP"
grep_at_least "V-10③ span = compat.requires_iface[0]" 'compat.requires_iface\[0\]' "$JQ_RESP"
# ① 缺字段 ⇒ 形状类错误 + 退出码 2
W3="$(cp_fx req-iface)"
sed -i '/^compat_gen = 0$/d' "$W3/service/consumer/plugin.toml"
run_json "$W3" check
eq "V-10① 缺 compat_gen ⇒ 退出码 2" "2" "$RC"
eq "V-10① 报 BRV-MF-0001" "1" "$(grep -c 'BRV-MF-0001' "$ERR" || true)"
grep_at_least "V-10① span 点名 compat_gen" 'compat.requires_iface\[0\].compat_gen' "$ERR"
W3="$(cp_fx req-iface)"
sed -i '/^api_iface  = "native"$/d' "$W3/service/consumer/plugin.toml"
run_json "$W3" check
eq "V-10① 缺 api_iface ⇒ 退出码 2" "2" "$RC"

# ================================================================ V-11 hash 语义
section "V-11 hash_scope/truth 成对 / NOT_ABI / hash 交叉检查"
W="$(cp_fx truth)"
run_json "$W" iface show service/x#x
eqj "V-11① --json 带 hash_scope" "$JQ_RESP" "data.hash_scope" "decl"
eqj "V-11① --json 带 truth" "$JQ_RESP" "data.truth" "decl"
eqj "V-11③ --json 带 not_abi" "$JQ_RESP" "data.not_abi" "true"
grep_at_least "V-11③ 文本折叠 NOT_ABI 提示" 'NOT_ABI' "$OUT"
# ② sym/header 不成对 ⇒ 红
Ws="$(cp_fx truth)"
sed -i 's/^hash_scope = "decl"$/hash_scope = "sym"/' "$Ws/service/x/plugin.toml"
run_json "$Ws" check
eq "V-11② hash_scope=sym ⇒ 退出码 1" "1" "$RC"
eq "V-11② 报 BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
Wt="$(cp_fx truth)"
sed -i 's/^truth      = "decl"$/truth      = "header"/' "$Wt/service/x/plugin.toml"
run_json "$Wt" check
eq "V-11② truth=header ⇒ 退出码 1" "1" "$RC"
eq "V-11② 报 BRV-IFACE-0011" "1" "$(grep -c 'BRV-IFACE-0011' "$ERR" || true)"
# ④ plugin.toml [[export]].hash 与快照不等 ⇒ 红
Wp="$(cp_fx publish)"
run_brickie "$Wp" iface publish service/a#a
sed -i 's/^hash         = ""$/hash         = "sha256:deadbeef"/' "$Wp/service/a/plugin.toml"
run_json "$Wp" iface status service/a#a
eq "V-11④ hash 不等 ⇒ status 退出码 1" "1" "$RC"
eq "V-11④ 报 BRV-IFACE-0012" "1" "$(grep -c 'BRV-IFACE-0012' "$ERR" || true)"
grep_at_least "V-11④ 消息点名两处记录" '同一真值的两处记录' "$ERR"

# ================================================================ V-12 导出分类不变量
section "V-12 TAX-0016/17/18/19 + 三方件 + skin 三小项"
for pair in "export-tax16:BRV-TAX-0016" "export-tax17:BRV-TAX-0017" \
            "export-tax18:BRV-TAX-0018" "export-tax19:BRV-TAX-0019"; do
    fx="${pair%%:*}"; code="${pair##*:}"
    W="$(cp_fx "$fx")"
    run_json "$W" check
    eq "V-12 $fx 退出码 1" "1" "$RC"
    eq "V-12 $fx 报 $code" "1" "$(grep -c "$code" "$ERR" || true)"
done
W="$(cp_fx thirdparty)"
run_json "$W" check
eq "V-12 正例①: third_party 无 export ⇒ 退出码 0" "0" "$RC"
eqj "V-12 正例① errors=0" "$JQ_RESP" "data.summary.errors" "0"
W="$(cp_fx skin-one)"
run_json "$W" check
eq "V-12 正例②: skin 单提供者 ⇒ 退出码 0" "0" "$RC"
W="$(cp_fx skin-multi)"
run_json "$W" check
eq "V-12 正例②: skin 多提供者 + 分类相等 ⇒ 退出码 0" "0" "$RC"
grep_none "V-12 正例② 无 TAX-0018" 'BRV-TAX-0018' "$ERR"
W="$(cp_fx sqlite)"
run_json "$W" check
eq "V-12 正例③: app → 三方件 + adapter 仅 type 边 ⇒ 退出码 0" "0" "$RC"
eqj "V-12 正例③ errors=0" "$JQ_RESP" "data.summary.errors" "0"

# ================================================================ V-13 版本串与 range
section "V-13 版本串 / range 段数 / 单调不回退"
W="$(cp_fx ver-badstr)"
run_json "$W" check
eq "V-13 5 段版本串 ⇒ 退出码 2" "2" "$RC"
eq "V-13 报 BRV-VER-0005" "1" "$(grep -c 'BRV-VER-0005' "$ERR" || true)"
W="$(cp_fx ver-badrange)"
run_json "$W" check
eq "V-13 4 段 range ⇒ 退出码 2" "2" "$RC"
eq "V-13 报 BRV-VER-0006" "1" "$(grep -c 'BRV-VER-0006' "$ERR" || true)"
W="$(cp_fx ver-pad)"
run_json "$W" check
eq "V-13 range 缺段右补 0(>=0.1 ≡ >=0.1.0)⇒ 退出码 0" "0" "$RC"
run_brickie "$W" dep tree
grep_at_least "V-13 规范化输出右补 0" '\[runtime >=0\.1\.0\]' "$OUT"
# compat_gen 缺失: 见 V-10①(形状门 BRV-MF-0001 / 退出码 2; BRV-VER-0003 无构造点, 记 ACCEPTANCE)
# --set 回退 ⇒ BRV-VER-0007
W="$(cp_fx publish)"
run_brickie "$W" iface publish service/a#a
python3 - "$W/service/a/plugin.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "g"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface publish service/a#a --set 0.0.9.0
eq "V-13 --set 低于当前版本 ⇒ 退出码 1" "1" "$RC"
eq "V-13 报 BRV-VER-0007" "1" "$(grep -c 'BRV-VER-0007' "$ERR" || true)"
run_json "$W" iface publish service/a#a --set 0.1.9.0
eq "V-13 --set 前进 ⇒ 退出码 0" "0" "$RC"

# ================================================================ V-14 profile 门禁
section "V-14 同一输入两种 profile 两种结论"
W="$(cp_fx req-iface)"
run_json "$W" check --profile dev
eq "V-14 dev ⇒ 退出码 0" "0" "$RC"
eqj "V-14 dev 下 VER-0004 是 info" "$JQ_RESP" "diagnostics.0.severity" "info"
grep_at_least "V-14 dev 有 VER-0004 提示" 'BRV-VER-0004' "$JQ_RESP"
run_json "$W" check --profile release
eq "V-14 release ⇒ 退出码 1" "1" "$RC"
eq "V-14 release 报 BRV-VER-0004" "1" "$(grep -c 'BRV-VER-0004' "$ERR" || true)"
grep_at_least "V-14 release 下升级为 error" 'error' "$ERR"
snapj "check-release-ver0004" "$JQ_RESP"

# ================================================================ V-15 append vs modify
section "V-15 条目级 append vs modify(§5.4)"
W="$(cp_fx iface-enum)"
run_brickie "$W" iface publish service/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["A", "B", "C"]/' "$W/service/enums/plugin.toml"
run_json "$W" iface diff service/enums#enums
eqj "V-15 枚举末尾追加 ⇒ EXTENDED" "$JQ_RESP" "data.changes.0.kind" "EXTENDED"
eqj "V-15 EXTENDED 免解冻 ⇒ compat_gen 不变" "$JQ_RESP" "data.compat_gen_changed" "false"
eqj "V-15 EXTENDED verdict=green" "$JQ_RESP" "data.verdict" "green"
run_json "$W" iface publish service/enums#enums
eq "V-15 EXTENDED publish 退出码 0" "0" "$RC"
eqj "V-15 EXTENDED ⇒ MINOR+1" "$JQ_RESP" "data.version" "0.1.1.0"
# 反例: 枚举重排
W="$(cp_fx iface-enum)"
run_brickie "$W" iface publish service/enums#enums
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/service/enums/plugin.toml"
run_json "$W" iface diff service/enums#enums
eqj "V-15 枚举重排 ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 枚举重排 ⇒ compat_gen_changed" "$JQ_RESP" "data.compat_gen_changed" "true"
eqj "V-15 未解冻 ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish service/enums#enums
eq "V-15 未解冻 publish ⇒ 退出码 1" "1" "$RC"
# 反例: 给已冻结结构体加字段
W="$(cp_fx iface-struct)"
run_brickie "$W" iface publish service/st#st
sed -i 's/^layout = "{ void\* p; }"$/layout = "{ void* p; int n; }"/' "$W/service/st/plugin.toml"
run_json "$W" iface diff service/st#st
eqj "V-15 结构体加字段 ⇒ CHANGED(D22)" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 结构体加字段 ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
run_json "$W" iface publish service/st#st
eq "V-15 结构体加字段未解冻 ⇒ 退出码 1" "1" "$RC"
# 反例: service ops 加槽
W="$(cp_fx iface-ops)"
run_brickie "$W" iface publish service/sv#sv
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/service/sv/plugin.toml"
run_json "$W" iface diff service/sv#sv
eqj "V-15 ops 加槽 ⇒ CHANGED" "$JQ_RESP" "data.changes.0.kind" "CHANGED"
eqj "V-15 ops 加槽 ⇒ verdict=red" "$JQ_RESP" "data.verdict" "red"
# 正例: 新增独立条目 ⇒ ADDED 免解冻
W="$(cp_fx iface-macro)"
run_brickie "$W" iface publish service/crypto#crypto
python3 - "$W/service/crypto/plugin.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
p.write_text(s.replace("[build]", '[[export.entries]]\nkind   = "func"\nname   = "crypto_new"\n'
                                'sig    = "int(void)"\nstatus = "experimental"\n\n[build]', 1))
PY
run_json "$W" iface diff service/crypto#crypto
eqj "V-15 新增条目 ⇒ ADDED" "$JQ_RESP" "data.changes.0.kind" "ADDED"
eqj "V-15 ADDED 免解冻 ⇒ verdict=green" "$JQ_RESP" "data.verdict" "green"
eqj "V-15 ADDED ⇒ compat_gen 不动" "$JQ_RESP" "data.compat_gen_changed" "false"

# ================================================================ V-16 解冻窗口
section "V-16 unfreeze 门槛 / release 阻断 / 空解冻四段不动"
W="$(cp_fx unfreeze)"
printf 'RFC\n' >"$W/rfc.md"
run_brickie "$W" iface unfreeze service/crypto#crypto
eq "V-16 unfreeze 缺 --note ⇒ 非 0(argparse 用法错 2)" "2" "$RC"
run_brickie "$W" iface publish service/crypto#crypto
eq "V-16 建档 publish 退出码 0" "0" "$RC"
run_brickie "$W" iface unfreeze service/crypto#crypto --note rfc.md
eq "V-16 带 --note 进入窗口退出码 0" "0" "$RC"
run_json "$W" iface status service/crypto#crypto
eqj "V-16 进入窗口不改面(snapshot_hash == recomputed_hash)" "$JQ_RESP" \
    "data.snapshot_hash" "$(jqget "$JQ_RESP" data.recomputed_hash)"
run_json "$W" check --profile release
eq "V-16 窗口内 release ⇒ 退出码 1" "1" "$RC"
eq "V-16 报 BRV-IFACE-0009" "1" "$(grep -c 'BRV-IFACE-0009' "$ERR" || true)"
run_json "$W" check --profile dev
eq "V-16 窗口内 dev ⇒ 退出码 0" "0" "$RC"
before_cg="$(jqget "$(run_json "$W" ver show service/crypto#crypto; echo "$JQ_RESP")" data.version)"
run_brickie "$W" iface refreeze service/crypto#crypto
eq "V-16 空解冻 refreeze 退出码 0" "0" "$RC"
run_json "$W" ver show service/crypto#crypto
eq "V-16 空解冻 ⇒ 四段全不动" "$before_cg" "$(jqget "$JQ_RESP" data.version)"
eqj "V-16 空解冻 compat_gen 不动" "$JQ_RESP" "data.segments.compat_gen" "0"

# ================================================================ V-17 hash 输入完整性
section "V-17 macro 值 / service ops / 枚举序 必须进 hash"
hash_of() { # hash_of <work> <id>
    run_json "$1" iface show "$2"
    jqget "$JQ_RESP" data.declaration_hash
}
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" service/crypto#crypto)"
sed -i 's/^value = "16"$/value = "4096"/' "$W/service/crypto/plugin.toml"
H1="$(hash_of "$W" service/crypto#crypto)"
eq "V-17 BR_MAX 16→4096 产生不同 hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
contains "V-17 hash 是 sha256:..." "sha256:" "$H0"
W="$(cp_fx iface-ops)"
H0="$(hash_of "$W" service/sv#sv)"
sed -i 's/^ops = \["open", "close"\]$/ops = ["open", "close", "seek"]/' "$W/service/sv/plugin.toml"
H1="$(hash_of "$W" service/sv#sv)"
eq "V-17 service ops 加槽产生不同 hash" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
W="$(cp_fx iface-enum)"
H0="$(hash_of "$W" service/enums#enums)"
sed -i 's/^value = \["A", "B"\]$/value = ["B", "A"]/' "$W/service/enums/plugin.toml"
H1="$(hash_of "$W" service/enums#enums)"
eq "V-17 枚举重排被 hash 感知" "different" \
    "$([ "$H0" != "$H1" ] && echo different || echo same)"
# 声明顺序不影响 hash(IFACE-IR 规则 1)
W="$(cp_fx iface-macro)"
H0="$(hash_of "$W" service/crypto#crypto)"
python3 - "$W/service/crypto/plugin.toml" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
head, tail = s.split("[build]", 1)
blocks = re.findall(r"\[\[export\.entries\]\][^\[]*", head)
assert len(blocks) == 2, blocks
out = head.replace(blocks[0], "@@B0@@").replace(blocks[1], "@@B1@@")
out = out.replace("@@B0@@", blocks[1].rstrip() + "\n\n").replace("@@B1@@", blocks[0].rstrip() + "\n\n")
p.write_text(out + "[build]" + tail)
PY
run_json "$W" iface show service/crypto#crypto
eq "V-17 声明顺序不影响 hash(IFACE-IR 规则 1)" "$H0" "$(jqget "$JQ_RESP" data.declaration_hash)"
# 两次 publish 逐字节可复现
W="$(cp_fx publish)"
run_brickie "$W" iface publish service/a#a
S1="$(sha256sum "$W/api/iface/service/a/a.toml" "$W/brickie.lock" "$W/api/iface/CHANGELOG.md")"
W2="$(cp_fx publish)"
run_brickie "$W2" iface publish service/a#a
S2="$(sha256sum "$W2/api/iface/service/a/a.toml" "$W2/brickie.lock" "$W2/api/iface/CHANGELOG.md")"
eq "V-17 同输入两次 publish 逐字节相同" "${S1//$W/}" "${S2//$W2/}"

# ================================================================ 命令面全覆盖
section "命令面: 23 叶子命令 + 2 全局开关(人读 + --json 两种输出)"
# 同一条序列在两条独立工作区各跑一遍: MODE=text / MODE=json。
# 每条都断言退出码; text 断言关键文本, json 断言信封键 + protocol + 诊断排序 + data 关键字段。
sweep() {
    MODE="$1"
    local W J
    W="$(cp_fx showcase)"
    printf 'RFC\n' >"$W/rfc.md"
    J="$TMPROOT/json-$MODE"
    mkdir -p "$J"

    S() { # S <标签> <期望退出码> <文本片段> <json 路径> <json 期望|present|absent> [args...]
        local label="$1" exp="$2" tgrep="$3" jpath="$4" jexp="$5"; shift 5
        if [ "$MODE" = json ]; then
            run_brickie "$W" "$@" --json
            cp "$OUT" "$J/$label.json"
            local r="$J/$label.json"
            eq "$label[--json] 退出码" "$exp" "$RC"
            eq "$label[--json] 信封键" "$ENVELOPE_KEYS" "$("$PY" "$JSONQ" envelope "$r")"
            eq "$label[--json] protocol=1" "1" "$("$PY" "$JSONQ" get "$r" protocol)"
            sortedj "$label[--json] 诊断稳定排序" "$r"
            case "$jexp" in
                present) hasj "$label[--json] 有 $jpath" "$r" "$jpath" ;;
                absent)  eqjc "$label[--json] 空 $jpath" "$r" "$jpath" "0" ;;
                *)       eqj  "$label[--json] $jpath" "$r" "$jpath" "$jexp" ;;
            esac
        else
            run_brickie "$W" "$@"
            eq "$label 退出码" "$exp" "$RC"
            [ -n "$tgrep" ] && grep_at_least "$label 文本含「$tgrep」" "$tgrep" "$OUT"
        fi
    }

    S check           0 '检查通过'                data.summary.errors 0          check
    S closure         0 '产品闭包'                data.selected       present    dep closure
    S tree            0 '依赖树'                  data.roots          present    dep tree
    S graph-dot       0 'digraph deps'            data.format         dot        dep graph --format dot
    S graph-mermaid   0 'graph LR'                data.format         mermaid    dep graph --format mermaid
    S graph-json      0 'format=json'             data.format         json       dep graph --format json
    S why-hit         0 '依赖路径'                data.found          true       dep why app/hsm service/crypto
    S why-miss        0 '未找到依赖路径'          data.found          false      dep why service/crypto app/hsm
    S dep-index       0 '反向依赖索引'            data.dependents     present    dep index
    S gen             0 '生成物重建'              data.count          5          gen
    S gen-check       0 '逐字节一致'              data.count          5          gen --check
    S ver-show        0 '接口单元 service/crypto#crypto' data.id     service/crypto#crypto ver show service/crypto
    S iface-list      0 '接口单元'                data.count          5          iface list
    S iface-show      0 '声明面 hash'             data.not_abi        true       iface show service/crypto#crypto
    S iface-diff0     0 '接口变更集'              data.id             present    iface diff service/crypto#crypto
    S iface-status0   1 '不一致'                  data.consistent     false      iface status service/crypto#crypto
    S iface-status-c0 1 '不一致'                  data.consistent     false      iface status service/crypto#crypto --check
    S iface-publish   0 '接口发布'                data.noop           false      iface publish service/crypto#crypto
    S iface-status1   0 '一致'                    data.consistent     true       iface status service/crypto#crypto
    S iface-noop      0 '空操作'                  data.noop           true       iface publish service/crypto#crypto
    S iface-pchk      0 '空操作'                  data.noop           true       iface publish service/crypto#crypto --check
    S ver-bump        0 '版本推进'                data.rule           revise     ver bump service/crypto#crypto --rule revise
    S iface-freeze    0 '待升格提案'              data.proposal       present    iface freeze service/crypto#crypto --note rfc.md
    S iface-deprecate 0 '接口弃用'                data.status         deprecated iface deprecate service/crypto#crypto --note rfc.md
    S iface-undep     0 '接口取消弃用'            data.status         frozen     iface undeprecate service/crypto#crypto --note rfc.md
    S iface-unfreeze  0 '进入窗口'                data.freeze_state   unfreezing iface unfreeze service/crypto#crypto --note rfc.md
    S iface-refreeze  0 '退出窗口'                data.freeze_state   frozen     iface refreeze service/crypto#crypto
    S dep-add         0 'dep add 只打印片段'      data.found          true       dep add app/hsm iface/hsm-api --kind runtime
    S dep-rm          0 'dep rm 只打印片段'       data.found          false      dep rm app/hsm service/nowhere

    # new / init 在各自空工作区里跑(避免污染上面的状态)
    local W0="$W"
    W="$(new_work)"
    S new  0 '骨架已生成' data.count 6 new ability service/svc --subkind service
    W="$(new_work)"
    S init 0 '骨架已生成' data.count 8 init hsm
    W="$W0"
}
sweep text
sweep json

# ================================================================ V-18 粘合层纯度
section "V-18 粘合层纯度(tests/purity.sh 三条判据)"
PURITY_OUT="$TMPROOT/purity.out"
bash "$TESTS_DIR/purity.sh" >"$PURITY_OUT" 2>&1
PURITY_RC=$?
cat "$PURITY_OUT"
PURITY_N="$(grep -oE 'purity 通过 [0-9]+ / 失败 [0-9]+' "$PURITY_OUT" | head -1 || true)"
if [ -n "$PURITY_N" ]; then
    pn="$(printf '%s' "$PURITY_N" | sed -E 's/.*通过 ([0-9]+) .*/\1/')"
    pf="$(printf '%s' "$PURITY_N" | sed -E 's/.*失败 ([0-9]+).*/\1/')"
    PASS=$((PASS + pn)); FAIL=$((FAIL + pf))
    eq "V-18 purity 退出码 0" "0" "$PURITY_RC"
else
    bad "V-18 purity.sh 未产出汇总行" "$(head -c 300 "$PURITY_OUT")"
fi
if [ "$PURITY_RC" -ne 0 ]; then
    grep -E '^FAIL' "$PURITY_OUT" >>"$FAILLOG" || true
fi

# ================================================================ V-19 特权与预算
section "V-19 特权声明 / 预算 / 资源独占"
W="$(cp_fx priv-ability-p3)"
run_json "$W" check
eq "V-19① ability 声明 P3 ⇒ 退出码 1" "1" "$RC"
eq "V-19① 报 BRV-PRIV-0001" "1" "$(grep -c 'BRV-PRIV-0001' "$ERR" || true)"
grep_at_least "V-19① 点名级差" '最多允许特权级 P2' "$ERR"
W="$(cp_fx priv-p2-map)"
run_json "$W" check
eq "V-19② level=P2 声明 map ⇒ 退出码 1" "1" "$RC"
eq "V-19② 报 BRV-PRIV-0001" "1" "$(grep -c 'BRV-PRIV-0001' "$ERR" || true)"
grep_at_least "V-19② 点名 op 与最低级别" 'op `map` 最低要求 P4' "$ERR"
W="$(cp_fx budget)"
run_json "$W" check
eq "V-19③ 预算超限 ⇒ 退出码 1" "1" "$RC"
grep_at_least "V-19③ RAM 差值" '超出产品预算 512, 差值 188 KiB' "$ERR"
grep_at_least "V-19③ 栈差值" '超出产品预算 16, 差值 6 KiB' "$ERR"
eq "V-19③ 预算两条诊断(RAM + 栈)" "2" "$(grep -c 'BRV-MF-0001' "$JQ_RESP" || true)"
W="$(cp_fx res-conflict)"
run_json "$W" check
eq "V-19④ IRQ/DMA 独占冲突 ⇒ 退出码 1" "1" "$RC"
eqjc "V-19④ 两条冲突(irq + dma)" "$JQ_RESP" "diagnostics" "2"
grep_at_least "V-19④ 报冲突双方(irq)" 'irq `32` 被 `service/irq-a`.*与 `service/irq-b`' "$ERR"
grep_at_least "V-19④ 报冲突双方(dma)" 'dma_channel `3` 被 `service/irq-a`.*与 `service/irq-b`' "$ERR"
W="$(cp_fx priv-pool-ok)"
run_json "$W" check
eq "V-19 正交表: 池生命周期(alloc/free, granularity=pool)属 P2 ⇒ 合法 0" "0" "$RC"
W="$(cp_fx priv-platform-p4)"
run_json "$W" check
eq "V-19 正交表: map/protect 属 P4, platform 可声明 ⇒ 合法 0" "0" "$RC"

# ================================================================ V-B 构建族
# 施工图 = ACCEPTANCE.md §V-B; 契约 = docs/contract.md §5.4/§7.1/§7.2/§7.4 + §9 R-13…R-16。
#
# ★ 本节**不调用任何编译器**(V-9 的纪律同样约束测试自身): 只用 `build --dry-run` /
#   `--emit-backends`(core 出计划、L5 干跑)+ 直接调 core 的 `judge` 信封。
#   判据都在**声明面与规划数据**上: argv 里有没有 arch 标志/链接脚本、候选序对不对、
#   码对不对 —— 一个字都不"看它编出来没有"(那需要交叉编译器, 属真构建的判据)。
section "V-B 构建族(不调编译器的判据)"
FX_BUILD="$(cp_fx build)"

# 直接调 core 的信封(judge 的日志是**正文**, 经 context.log 传入; 判据在 core)。
JUDGE_RESP=""
core_judge() { # core_judge <work> <gate-name> <log-file> ⇒ JUDGE_RESP
    local w="$1" name="$2" logf="$3"
    "$PY" - "$w" "$name" "$logf" "$TMPROOT/.judge.req.json" <<'PYX'
import json, sys
w, name, logf, out = sys.argv[1:5]
json.dump({"protocol": 1, "command": "judge", "root": w,
           "args": {"name": name},
           "context": {"log": open(logf, encoding="utf-8").read()}},
          open(out, "w"))
PYX
    "$CORE" <"$TMPROOT/.judge.req.json" >"$TMPROOT/.judge.resp.json" 2>/dev/null
    JUDGE_RESP="$TMPROOT/.judge.resp.json"
}

# ---------------------------------------------------------------- ① 计划形状
# `build --dry-run` 不需要编译器: core 给计划, L5 只打印。
W="$(cp_fx build)"
run_json "$W" build --dry-run
eq "V-B① build --dry-run 退出码 0" "0" "$RC"
eqj "V-B① dry_run 标记" "$JQ_RESP" "data.dry_run" "true"
eqj "V-B① 组件数 = 6(核心 + 5 插件)" "$JQ_RESP" "data.unit_count" "6"
eqjc "V-B① data.units 与 unit_count 一致" "$JQ_RESP" "data.units" "6"
eqj "V-B① 源文件数" "$JQ_RESP" "data.source_count" "7"
eqj "V-B① steps_total = 9(7 编译 + 链接 + objcopy)" "$JQ_RESP" "data.steps_total" "9"
eqjc "V-B① dry-run 下列出全部步骤" "$JQ_RESP" "data.steps" "9"
keysj "V-B① data.units[0] 形状" "$JQ_RESP" "data.units.0" \
    "defines,dir,includes,name,plugin,sources,sources_list"
keysj "V-B① data.steps[0] 形状(contract §5.4)" "$JQ_RESP" "data.steps.0" \
    "argv,consumes,expect_timeout,fail_code,group,judge,kind,label,log,outputs,stdout_lines,timeout_s,tool_code"
eqj "V-B① steps[0].fail_code = BRV-BLD-0011" "$JQ_RESP" "data.steps.0.fail_code" "BRV-BLD-0011"
eqj "V-B① steps[0].tool_code = BRV-BLD-0005" "$JQ_RESP" "data.steps.0.tool_code" "BRV-BLD-0005"
eqj "V-B① target 来自 platform 插件" "$JQ_RESP" "data.target.owner" "platform/qemu-aarch64"
eqj "V-B① target.arch" "$JQ_RESP" "data.target.arch" "aarch64"
eqj "V-B① target.cross" "$JQ_RESP" "data.target.cross" "aarch64-linux-gnu-"
eqj "V-B① target.linker_script(相对插件根)" "$JQ_RESP" "data.target.linker_script" \
    "platform/qemu-aarch64/src/link.ld"
STEPS_JSON="$(jqget "$JQ_RESP" data.steps)"
contains "V-B① arch 标志(-mstrict-align)进了编译 argv" '"-mstrict-align"' "$STEPS_JSON"
contains "V-B① arch 标志(-mgeneral-regs-only)进了编译 argv" '"-mgeneral-regs-only"' "$STEPS_JSON"
contains "V-B① 链接脚本进了 -Wl,-T" '"-Wl,-T,platform/qemu-aarch64/src/link.ld"' "$STEPS_JSON"
contains "V-B① -Wl,-Map 用产物落点" '"-Wl,-Map,build/brick.map"' "$STEPS_JSON"
contains "V-B① 声明面的 cflags 进了 argv" '"-DFX_BUILD=1"' "$STEPS_JSON"
contains "V-B① 汇编源(.S)也在计划里(路径相对 root)" \
    'platform/qemu-aarch64/src/start.S' "$STEPS_JSON"
# ★ 路径口径: 计划里的一切路径**相对 root**; 而声明面 `[build].sources` 的通配**相对插件根**。
#   这条断言同时钉住"汇编不被当 C 编"(它必须走 asflags, 不许混进 C 的 -std=c11)。
asm_ok() { # asm_ok <resp.json> ⇒ yes / no:…
    "$PY" - "$1" <<'PYX'
import json, sys
d = json.load(open(sys.argv[1]))["data"]
step = [s for s in d["steps"]
        if s["kind"] == "compile" and any(a.endswith(".S") for a in s["argv"])]
if not step:
    print("no-asm-step")
else:
    argv = step[0]["argv"]
    ok = ("-std=c11" not in argv and "-g3" in argv and "-mstrict-align" in argv)
    print("yes" if ok else "no:" + " ".join(argv))
PYX
}
eq "V-B① 汇编步骤按 asflags 编(不混 C 的 -std=c11)" "yes" "$(asm_ok "$JQ_RESP")"
contains "V-B① 链接产物是 build/brick.elf" '"build/brick.elf"' "$STEPS_JSON"
# 工具候选序(裁定 R-14): 顺序即策略, 解析结果由 core 给。
eqjc "V-B① data.tools 七件" "$JQ_RESP" "data.tools" "7"
eqj "V-B① tools[0].name = cc(BTreeMap 序)" "$JQ_RESP" "data.tools.0.name" "cc"
keysj "V-B① data.tools[0] 形状" "$JQ_RESP" "data.tools.0" "candidates,name,resolved,what"
eqj "V-B① cc 候选首位 = <cross>gcc" "$JQ_RESP" "data.tools.0.candidates.0" "aarch64-linux-gnu-gcc"
eqj "V-B① cc 候选第 2 位 = gcc-16" "$JQ_RESP" "data.tools.0.candidates.1" "aarch64-linux-gnu-gcc-16"
eqj "V-B① cc 候选末位 = 宿主 cc" "$JQ_RESP" "data.tools.0.candidates.5" "cc"
eqj "V-B① tools[3].name = objcopy" "$JQ_RESP" "data.tools.3.name" "objcopy"
eqj "V-B① objcopy 候选首位 = 交叉版" "$JQ_RESP" "data.tools.3.candidates.0" "aarch64-linux-gnu-objcopy"
eqj "V-B① objcopy 候选末位 = 宿主版" "$JQ_RESP" "data.tools.3.candidates.1" "objcopy"
hasj "V-B① tools[0] 带 resolved 键(解析在 core)" "$JQ_RESP" "data.tools.0.resolved"
eqj "V-B① QEMU 型号来自 [build.target.qemu]" "$JQ_RESP" "data.target.qemu.machine" "virt,gic-version=3"

# ---------------------------------------------------------------- ② 缺 [build]
W="$(cp_fx build)"
"$PY" - "$W/product.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().split("[build]")[0].rstrip() + "\n")
PY
run_json "$W" build --dry-run
eq "V-B② product.toml 缺 [build] ⇒ 退出码 2" "2" "$RC"
eq "V-B② 报 BRV-BLD-0001" "1" "$(grep -c 'BRV-BLD-0001' "$ERR" || true)"

# ---------------------------------------------------------------- ③ 缺 [build.target]
W="$(cp_fx build)"
"$PY" - "$W/platform/qemu-aarch64/plugin.toml" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
p.write_text(p.read_text().split("[build.target]")[0].rstrip() + "\n")
PY
run_json "$W" build --dry-run
eq "V-B③ 没有插件声明 [build.target] ⇒ 退出码 2" "2" "$RC"
eq "V-B③ 报 BRV-BLD-0003" "1" "$(grep -c 'BRV-BLD-0003' "$ERR" || true)"

# ---------------------------------------------------------------- ④ 字面源不存在
W="$(cp_fx build)"
sed -i 's|sources  = \["src/\*.c"\]|sources  = ["src/*.c", "src/nope.c"]|' \
    "$W/app/hello/plugin.toml"
run_json "$W" build --dry-run
eq "V-B④ [build].sources 字面路径不存在 ⇒ 退出码 2" "2" "$RC"
eq "V-B④ 报 BRV-BLD-0007" "1" "$(grep -c 'BRV-BLD-0007' "$ERR" || true)"
grep_at_least "V-B④ span 点名 build.sources" 'build\.sources' "$ERR"

# ---------------------------------------------------------------- ⑤ test --list
W="$(cp_fx build)"
run_json "$W" test --list
eq "V-B⑤ test --list 退出码 0" "0" "$RC"
eqj "V-B⑤ 声明面文件" "$JQ_RESP" "data.file" "tests/gates.toml"
eqj "V-B⑤ present" "$JQ_RESP" "data.present" "true"
eqjc "V-B⑤ QEMU 门禁 4 条" "$JQ_RESP" "data.gates" "4"
eqj "V-B⑤ gates[0]" "$JQ_RESP" "data.gates.0" "g-route"
eqj "V-B⑤ gates[3]" "$JQ_RESP" "data.gates.3" "g-ok"
eqjc "V-B⑤ 宿主用例 2 条" "$JQ_RESP" "data.hosttests" "2"
eqj "V-B⑤ hosttests[1](带 depends)" "$JQ_RESP" "data.hosttests.1" "ht-dep"
eqjc "V-B⑤ 脚本门禁 1 条" "$JQ_RESP" "data.scripts" "1"
eqj "V-B⑤ scripts[0]" "$JQ_RESP" "data.scripts.0" "s-echo"
eqjc "V-B⑤ build_post 一条" "$JQ_RESP" "data.build_post" "1"
eqj "V-B⑤ build_post[0] = g-ok" "$JQ_RESP" "data.build_post.0" "g-ok"

# ---------------------------------------------------------------- ⑥ judge(判据在 core)
printf 'nothing here\n' >"$TMPROOT/judge-empty.log"
printf 'BAD thing\n' >"$TMPROOT/judge-forbid.log"
printf 'OK\nPASS TC-2 case\n' >"$TMPROOT/judge-ok.log"
core_judge "$FX_BUILD" g-route "$TMPROOT/judge-empty.log"
"$PY" "$JSONQ" text "$JUDGE_RESP" >"$TMPROOT/.judge.txt"
eqj "V-B⑥ require 缺一条 ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ require 缺一条 ⇒ failed 1" "$JUDGE_RESP" "data.failed" "1"
eq "V-B⑥ require 缺一条 ⇒ BRV-BLD-0009" "1" \
    "$(grep -c 'BRV-BLD-0009' "$TMPROOT/.judge.txt" || true)"
eqj "V-B⑥ require 的 span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].require"
core_judge "$FX_BUILD" g-forbid "$TMPROOT/judge-forbid.log"
eqj "V-B⑥ forbid 命中 ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ forbid 命中 ⇒ span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].forbid"
core_judge "$FX_BUILD" g-tag "$TMPROOT/judge-empty.log"
eqj "V-B⑥ 缺 PASS tag ⇒ exit_code 1" "$JUDGE_RESP" "exit_code" "1"
eqj "V-B⑥ 缺 PASS tag ⇒ span" "$JUDGE_RESP" "diagnostics.0.span" "gate[0].require_tags"
core_judge "$FX_BUILD" g-ok "$TMPROOT/judge-ok.log"
eqj "V-B⑥ 全绿 ⇒ exit_code 0" "$JUDGE_RESP" "exit_code" "0"
eqj "V-B⑥ 全绿 ⇒ failed 0" "$JUDGE_RESP" "data.failed" "0"
eqj "V-B⑥ 全绿 ⇒ 无码 info(裁定 R-16)" "$JUDGE_RESP" "diagnostics.0.code" "null"
eqj "V-B⑥ 全绿 ⇒ severity=info" "$JUDGE_RESP" "diagnostics.0.severity" "info"
eqj "V-B⑥ 全绿 ⇒ checks 三条" "$JUDGE_RESP" "data.checks.2.ok" "true"
core_judge "$FX_BUILD" no-such-gate "$TMPROOT/judge-empty.log"
"$PY" "$JSONQ" text "$JUDGE_RESP" >"$TMPROOT/.judge.txt"
eqj "V-B⑥ 未知门禁 ⇒ exit_code 2" "$JUDGE_RESP" "exit_code" "2"
eq "V-B⑥ 未知门禁 ⇒ BRV-BLD-0008" "1" \
    "$(grep -c 'BRV-BLD-0008' "$TMPROOT/.judge.txt" || true)"

# ---------------------------------------------------------------- ⑦ 正则超子集
W="$(cp_fx gates-regex)"
run_json "$W" test --list
eq "V-B⑦ 正则用 ( 分组 ⇒ 退出码 2" "2" "$RC"
eq "V-B⑦ 报 BRV-BLD-0012(拒绝而不是静默)" "1" "$(grep -c 'BRV-BLD-0012' "$ERR" || true)"
grep_at_least "V-B⑦ span 点名 gate.require[0]" 'gate\.require\[0\]' "$ERR"

# ---------------------------------------------------------------- ⑧ clean 的 paths 表
W="$(cp_fx build)"
run_json "$W" clean --all
eq "V-B⑧ clean --all 退出码 0" "0" "$RC"
eqjc "V-B⑧ paths 13 条" "$JQ_RESP" "data.paths" "13"
PATHS_JSON="$(jqget "$JQ_RESP" data.paths)"
contains "V-B⑧ paths 含 build/obj" '"build/obj"' "$PATHS_JSON"
contains "V-B⑧ paths 含 build/brick.elf" '"build/brick.elf"' "$PATHS_JSON"
contains "V-B⑧ paths 含 build/brick.bin" '"build/brick.bin"' "$PATHS_JSON"
contains "V-B⑧ paths 含 build/gen(--all)" '"build/gen"' "$PATHS_JSON"
contains "V-B⑧ paths 含宿主用例落点 build/hosttest" '"build/hosttest"' "$PATHS_JSON"
eqj "V-B⑧ paths 有序(字典序首项)" "$JQ_RESP" "data.paths.0" "build/brick.bin"
eqjc "V-B⑧ 夹具里本来没有产物 ⇒ 一件都没删" "$JQ_RESP" "written" "0"

# ---------------------------------------------------------------- ⑨ --emit-backends
W="$(cp_fx build)"
run_json "$W" build --dry-run --emit-backends
eq "V-B⑨ --emit-backends 退出码 0" "0" "$RC"
eqj "V-B⑨ emit_backends 标记" "$JQ_RESP" "data.emit_backends" "true"
eqjc "V-B⑨ 落盘两件后端文件" "$JQ_RESP" "written" "2"
eq "V-B⑨ build/gen/build.mk 就位" "yes" "$([ -f "$W/build/gen/build.mk" ] && echo yes || echo no)"
eq "V-B⑨ build/gen/build.ninja 就位" "yes" "$([ -f "$W/build/gen/build.ninja" ] && echo yes || echo no)"
grep_at_least "V-B⑨ build.mk 带生成物标记" 'brickie:generated' "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.mk 是 BR-D4 后端 A" '\$\(ELF\): \$\(OBJS\)' "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.mk 把链接脚本写进规则" '-Wl,-T,platform/qemu-aarch64/src/link.ld' \
    "$W/build/gen/build.mk"
grep_at_least "V-B⑨ build.ninja 带生成物标记" 'brickie:generated' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ build.ninja 是 BR-D4 后端 B" '^rule link$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ build.ninja 烧进解析出的编译器" '^cc = ' "$W/build/gen/build.ninja"
# C 源与汇编源**各一条 rule**(把汇编当 C 编只是"碰巧也能过", 不是判据)。
grep_at_least "V-B⑨ ninja 有 cc rule(C 源)" '^rule cc$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ ninja 有 asm rule(.S 源)" '^rule asm$' "$W/build/gen/build.ninja"
grep_at_least "V-B⑨ .S 走 asm rule" ': asm .*src/start\.S' "$W/build/gen/build.ninja"
# ninja 的 .ninja_log/.ninja_deps 不许落到仓库根(踩过): builddir 钉在 build/gen 下。
grep_at_least "V-B⑨ ninja 的 builddir 钉在 build/gen 下" '^builddir = build/gen/ninja$' \
    "$W/build/gen/build.ninja"
eq "V-B⑨ dry-run 不落增量状态(它是派生物)" "no" \
    "$([ -f "$W/build/gen/build-state.json" ] && echo yes || echo no)"

# ================================================================ N-1 反向验收: help 面
section "N-1 §5.4 未交付命令一条都不在 --help 里"
help_commands() { # <help 输出文件> ⇒ 逗号分隔的子命令名(已排序去重)
    awk '/^positional arguments:/{f=1;next} /^options:/{f=0} f' "$1" \
        | grep -oE '^    [a-z][a-z0-9-]*' | tr -d ' ' | sort -u | paste -sd, -
}
W="$(new_work)"
run_brickie "$W" --help
eq "N-1 顶层 help 退出码 0" "0" "$RC"
eq "N-1 顶层命令面 = 13 组(组合期 7 + 构建族 6)" \
    "build,check,clean,dep,disasm,gen,iface,init,new,run,size,test,ver" "$(help_commands "$OUT")"
# build/test/run 原在 §5.4 的禁名名单里, 现由 ADR-0004 交付(第二族)⇒ 只反查**仍不做**的。
for c in api-dump verify dbg pack add show env; do
    eq "N-1 顶层 help 不含 $c" "0" \
        "$(help_commands "$OUT" | tr ',' '\n' | grep -cx "$c" || true)"
done
run_brickie "$W" iface --help
eq "N-1 iface 子命令面" \
    "deprecate,diff,freeze,list,publish,refreeze,show,status,undeprecate,unfreeze" \
    "$(help_commands "$OUT")"
eq "N-1 iface 下无 check(v0.2 才做)" "0" \
    "$(help_commands "$OUT" | tr ',' '\n' | grep -cx 'check' || true)"
grep_none "N-1 iface help 全文无 'iface check'" 'iface check' "$OUT"
run_brickie "$W" dep --help
eq "N-1 dep 子命令面" "add,closure,graph,index,rm,tree,why" "$(help_commands "$OUT")"
run_brickie "$W" ver --help
eq "N-1 ver 子命令面" "bump,show" "$(help_commands "$OUT")"

# ================================================================ N-2 反向验收: 未交付命令
section "N-2 §5.4 未交付命令直接调用 ⇒ 退出码 2"
W="$(new_work)"
# build / test / run 已交付(见 N-1 注), 不再属于"未交付"名单。
for c in api-dump verify dbg pack add show env; do
    run_brickie "$W" "$c"
    eq "N-2 brickie $c ⇒ 退出码 2" "2" "$RC"
done
run_brickie "$W" iface check service/x#x
eq "N-2 brickie iface check ⇒ 退出码 2" "2" "$RC"
run_brickie "$W" frobnicate
eq "N-2 未知子命令 ⇒ 退出码 2" "2" "$RC"
run_brickie "$W"
eq "N-2 无子命令 ⇒ 退出码 2" "2" "$RC"

# ================================================================ N-3 机器文件归属
section "N-3 机器文件只由机器写 / 手改即红"
W="$(cp_fx publish)"
run_brickie "$W" iface publish service/a#a
PLUGIN_BEFORE="$(sha256sum "$W/service/a/plugin.toml")"
run_brickie "$W" iface publish service/a#a
eq "N-3 publish 不改写 plugin.toml(§9.1.1 规则 1)" "$PLUGIN_BEFORE" \
    "$(sha256sum "$W/service/a/plugin.toml")"
grep_at_least "N-3 快照是机器文件(带生成物标记)" 'brickie:generated' "$W/api/iface/service/a/a.toml"
grep_at_least "N-3 lock 是机器文件" 'brickie:generated' "$W/brickie.lock"
sed -i 's/^hash = "sha256:[0-9a-f]*"$/hash = "sha256:1111111111111111111111111111111111111111111111111111111111111111"/' \
    "$W/api/iface/service/a/a.toml"
run_json "$W" iface status service/a#a
eq "N-3 手改快照 ⇒ status 退出码 1" "1" "$RC"
eqj "N-3 手改快照 ⇒ consistent=false" "$JQ_RESP" "data.consistent" "false"

# ================================================================ N-4 协议快照
section "N-4 --json 结构化快照(协议版本锁定)"
W="$(cp_fx showcase)"
run_brickie "$W" --version --json
cp "$OUT" "$TMPROOT/s-version.json"
snapj "version" "$TMPROOT/s-version.json"
run_json "$W" check
snapj "check-clean" "$JQ_RESP"
run_json "$W" dep tree
snapj "dep-tree" "$JQ_RESP"
run_json "$W" dep closure
snapj "dep-closure" "$JQ_RESP"
run_json "$W" iface list
snapj "iface-list" "$JQ_RESP"
run_json "$W" iface show service/crypto#crypto
snapj "iface-show" "$JQ_RESP"
W="$(cp_fx cycle)"
run_json "$W" check
snapj "check-cycle" "$JQ_RESP"
W="$(new_work)"
run_json "$W" new ability service/crypto --subkind service
snapj "new-json" "$JQ_RESP"
W="$(cp_fx publish)"
run_json "$W" iface publish service/a#a
snapj "publish" "$JQ_RESP"
run_json "$W" iface status service/a#a
snapj "iface-status" "$JQ_RESP"

# ================================================================ 汇总
printf '\n=== 通过 %s / 失败 %s ===\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf '\n失败明细:\n'
    cat "$FAILLOG"
    exit 1
fi
exit 0
