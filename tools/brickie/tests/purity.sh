#!/usr/bin/env bash
# V-18 粘合层纯度(L5 里没有业务规则)—— 三条**可执行**判据。
#
#   (a) 独立性: `brickie-core --selftest` 在不设 PYTHONPATH、PATH 里连 python3 都没有的
#       环境里全绿 ⇒ 领域判定不依赖 Python 包。
#   (b) 桩替换反证: 把 `brickie-core` 换成一个"只回固定 JSON 的桩", `brickie check` 的
#       **诊断集合(码 + 数量 + 排序)** 必须与真实 core 逐一对齐 ⇒ L5 不添加/过滤/重排
#       业务诊断(L5 只允许加"形状门"诊断, 而夹具是形状干净的, 门为空)。
#   (c) 静态检查: `python/brickie/**` 不得出现业务规则的**实现载体**(见 §禁止清单)。
#       禁止清单窄而准: 只ban"只有判定层才会写的东西", 不ban呈现/参数/IO 的用词。
#
# 用法: bash tests/purity.sh        (一般经 run.sh 带进来; 也可 `make test-purity` 单跑)
set -u

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
. "$TESTS_DIR/lib.sh"
resolve_env

TMPROOT="$(mktemp -d)"
trap 'rm -rf "$TMPROOT"' EXIT
init_report "$TMPROOT/failures.log"

printf '\n=== V-18 粘合层纯度 ===\n'

# ------------------------------------------------------------------ (a) selftest 无 Python
# `env -i` 抹掉一切环境(含 PYTHONPATH); `PATH=/nonexistent` 让 core 连 python3 都找不到。
SELFTEST_OUT="$TMPROOT/selftest.txt"
env -i PATH=/nonexistent "$CORE" --selftest >"$SELFTEST_OUT" 2>&1
SELFTEST_RC=$?
eq "(a) --selftest 空环境退出码 0" "0" "$SELFTEST_RC"
eq "(a) --selftest 全绿(ok N cases)" "1" \
    "$(grep -cE '^ok [0-9]+ cases$' "$SELFTEST_OUT" || true)"
eq "(a) --selftest 无失败行" "0" \
    "$(grep -ciE 'fail|error' "$SELFTEST_OUT" || true)"
eq "(a) 精简环境里确实没有 python3" "absent" \
    "$(PATH=/nonexistent command -v python3 >/dev/null 2>&1 && echo found || echo absent)"

# ------------------------------------------------------------------ (b) 桩替换反证
# 桩: 忽略 stdin, 原样回吐"真实 core 对同一夹具的响应"。
make_stub() { # make_stub <canned.json> <输出路径>
    cat >"$2" <<EOF
#!/bin/sh
# 桩: 只回固定 JSON, 不含任何判定(不读输入内容)。
cat >/dev/null
cat "$1"
EOF
    chmod +x "$2"
}

stub_probe() { # stub_probe <夹具名> <标签> <真实 core 的诊断落盘路径>
    local fx="$1" label="$2" realout="$3"
    local w
    w="$(cp_fx "$fx")"
    run_json "$w" check
    cp "$JQ_RESP" "$realout"
    local n
    n="$("$PY" "$JSONQ" diagcodes "$realout")"
    [ -n "$n" ] || { bad "(b) $label 夹具未产生诊断, 反证无意义" "夹具 $fx"; return; }

    local canned="$TMPROOT/stub-$label.json" stub="$TMPROOT/stub-$label"
    cp "$realout" "$canned"
    make_stub "$canned" "$stub"

    local saved="$CORE"
    CORE="$stub"
    run_json "$w" check
    CORE="$saved"
    local stubout="$TMPROOT/stub-resp-$label.json"
    cp "$JQ_RESP" "$stubout"

    # 诊断集合: 码 + 数量 + 排序 三者逐字节对齐
    eq "(b) $label 桩替换: 诊断序列一致" \
        "$("$PY" "$JSONQ" diags "$realout")" "$("$PY" "$JSONQ" diags "$stubout")"
    eq "(b) $label 桩替换: 数量一致" \
        "$("$PY" "$JSONQ" count "$realout" diagnostics)" \
        "$("$PY" "$JSONQ" count "$stubout" diagnostics)"
    eq "(b) $label 桩替换: 码集合一致" \
        "$("$PY" "$JSONQ" diagcodes "$realout")" "$("$PY" "$JSONQ" diagcodes "$stubout")"
    # 反向: 真实 core 的诊断确实非空(否则上面三条恒真)
    eq "(b) $label 真实 core 诊断非空" "no" \
        "$([ "$("$PY" "$JSONQ" count "$realout" diagnostics)" -gt 0 ] && echo no || echo yes)"
}

stub_probe "export-tax16" "tax16" "$TMPROOT/real-tax16.json"
stub_probe "cycle" "cycle" "$TMPROOT/real-cycle.json"
stub_probe "budget" "budget" "$TMPROOT/real-budget.json"

# ------------------------------------------------------------------ (c) 静态检查
# 禁止清单(**窄而准**: 只ban"判定层才会写的东西"):
#   1. 业务 BRV 码 —— L5 只允许形状类 `MF`(BRV-D2 的 schema 门)与协议握手 `PROTO`;
#      出现 DEP/VER/IFACE/TAX/PRIV/GEN 的码 ⇒ 前端在自己造判定结论。
#   2. 计算库 —— hashlib/hmac(IFACE-IR hash)、networkx/graphlib(拓扑/环)、z3(区间求解)。
#   3. 规则形状的函数名 —— solve/topo/cycle 检测/区间交集/版本推进/规范化 hash/分类学/特权/预算。
#      (故意不ban `def ...closure`: 呈现函数 `_render_closure` 是允许的。)
#   4. 版本段算术 —— 对 compat_gen/major/minor/revise 的 +-*/ 。
#   5. range/版本解析原语 —— L5 只把 `range` 字符串当参数透传, 不得解析。
#   6. 状态机转移字面量 —— 三态/冻结态之间的 `->`/`=>` 迁移表。
#   7. 机器文件正文归属 —— 生成物标记与快照/lock 的渲染函数名;
#      `api/iface/**` 的路径拼接也归 core(files[] 是 core 给的)。
PY_DIR="$TOOL_ROOT/python/brickie"
purity_pattern_check() { # <名称> <ERE> <理由>
    local label="$1" pat="$2" why="$3"
    local hits
    # 只扫代码(`*.py`): README 里引用码名/术语是文档, 不是判定实现。
    hits="$(grep -rInE --include='*.py' -- "$pat" "$PY_DIR" 2>/dev/null || true)"
    if [ -z "$hits" ]; then
        ok "(c) $label"
    else
        bad "(c) $label —— $why" "$(printf '%s' "$hits" | head -c 400)"
    fi
}

purity_pattern_check "无业务 BRV 码(只允许 MF/PROTO)" \
    'BRV-(DEP|VER|IFACE|TAX|PRIV|GEN)-[0-9]{4}' \
    "业务码必须由 brickie-core 报(L5 只有形状门 MF 与协议握手 PROTO)"
purity_pattern_check "无 hash/图论/求解库" \
    '\b(hashlib|hmac|networkx|graphlib|z3|numpy|scipy)\b' \
    "hash 计算 / 拓扑排序 / 区间求解是判定层职责"
purity_pattern_check "无规则形状的函数名" \
    'def[[:space:]]+[a-zA-Z_]*(solve|topo|detect_cycle|find_cycle|intersect|satisf|bump_version|advance_version|canonical_surface|surface_hash|compute_hash|content_hash|classify|privilege_level|budget_sum|change_set|verdict)' \
    "这些名字对应闭包/环/区间/推进/规范化/分类/特权/预算判定"
purity_pattern_check "无版本段算术" \
    '(compat_gen|major|minor|revise)[[:space:]]*[-+*/]|[-+*/][[:space:]]*(compat_gen|major|minor|revise)' \
    "四段推进只能由 core 计算(§5.2)"
purity_pattern_check "无 range/版本解析原语" \
    '(parse_range|range_matches|range_intersect|parse_version|parse_triple|interval_)' \
    "range 只作为参数字符串透传, 解析归 core(§7.4)"
purity_pattern_check "无状态机转移字面量" \
    '(experimental|frozen|deprecated|unfreezing)[[:space:]]*(->|=>|→)' \
    "三态/冻结态状态机在 core(§6.4)"
purity_pattern_check "无机器文件正文归属" \
    '(brickie:generated|render_snapshot|render_lock|api/iface/)' \
    "lock/快照/CHANGELOG 的正文必须由 core 给(files[]), L5 只落盘"

# 正向对照: 允许的六件事必须有实现载体(否则上面的"空禁止"可能只是文件为空)
grep_at_least "(c) 正向对照: 有 argparse 参数解析" 'import argparse' "$PY_DIR/cli.py"
grep_at_least "(c) 正向对照: 有 schema 形状校验"   'import json'      "$PY_DIR/schema.py"
grep_at_least "(c) 正向对照: 有子进程调用"        'subprocess'       "$PY_DIR/native.py"
grep_at_least "(c) 正向对照: 有幂等落盘"          'def write_files'  "$PY_DIR/writer.py"

# ------------------------------------------------------------------ 汇总
printf '\n--- purity 通过 %s / 失败 %s ---\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    printf 'purity 失败明细:\n'
    cat "$FAILLOG"
    exit 1
fi
exit 0
