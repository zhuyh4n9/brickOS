#!/usr/bin/env bash
# brickie v0.1 端到端用例的公共骨架(run.sh 与 purity.sh 共用)。
#
# 纪律:
#   * 每个用例都在 `mktemp -d` 的临时目录里跑, 用例之间不共享状态;
#   * 断言只走两条路: **退出码** + **结构化 JSON**(tests/jsonq.py)或关键文本;
#   * 本文件不调用任何编译器(cc/g++/cargo/nm) —— V-9 的"零编译依赖"同样约束测试自身。
#
# 依赖注入(由调用方在 source 之后设置/覆盖):
#   TOOL_ROOT / REPO_ROOT / TESTS_DIR / TMPROOT / PY / CORE / GEN
set -u

# ------------------------------------------------------------------ 环境解析
# 二进制定位顺序(**与 python/brickie/native.py 一致**): 本机出树优先, 退到自举种子。
# 用例必须显式把 BRICKIE_CORE/BRICKIE_GEN 指到实际用的那份, 避免"前端自己找"带来的歧义。
pick_first() {
    local p
    for p in "$@"; do
        [ -n "$p" ] && [ -x "$p" ] && { printf '%s' "$p"; return 0; }
    done
    return 1
}

resolve_env() {
    TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
    TOOL_ROOT="${TOOL_ROOT:-$(cd "$TESTS_DIR/.." && pwd)}"
    REPO_ROOT="${REPO_ROOT:-$(cd "$TOOL_ROOT/../.." && pwd)}"
    PY="${PY:-$(command -v python3)}"
    HOST_ARCH="${BRICKIE_HOST_ARCH:-$(bash "$REPO_ROOT/tools/host-detect.sh" --arch)}"
    HOST_OS="${BRICKIE_HOST_OS:-$(bash "$REPO_ROOT/tools/host-detect.sh" --os)}"

    local build_bin="$REPO_ROOT/build/host/$HOST_ARCH/$HOST_OS/bin"
    local seed_bin="$REPO_ROOT/prebuilts/seed/brickie/$HOST_ARCH/$HOST_OS/bin"
    CORE="${BRICKIE_CORE:-$(pick_first "$build_bin/brickie-core" \
        "$REPO_ROOT"/build/host/*/*/bin/brickie-core "$seed_bin/brickie-core" \
        "$REPO_ROOT"/prebuilts/seed/brickie/*/*/bin/brickie-core)}"
    GEN="${BRICKIE_GEN:-$(pick_first "$build_bin/brickie-gen" \
        "$REPO_ROOT"/build/host/*/*/bin/brickie-gen "$seed_bin/brickie-gen" \
        "$REPO_ROOT"/prebuilts/seed/brickie/*/*/bin/brickie-gen)}"
    LAUNCHER="${BRICKIE_LAUNCHER:-$(pick_first "$build_bin/brickie" "$seed_bin/brickie")}"
    SEED_LAUNCHER="${SEED_LAUNCHER:-$(pick_first "$seed_bin/brickie")}"
    export TESTS_DIR TOOL_ROOT REPO_ROOT PY HOST_ARCH HOST_OS CORE GEN LAUNCHER SEED_LAUNCHER
}

# ------------------------------------------------------------------ 计数与报告
PASS=0
FAIL=0
FAILLOG=""

init_report() { FAILLOG="${1:-${TMPROOT:-/tmp}/failures.log}"; : >"$FAILLOG"; }

ok() { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }

bad() {
    FAIL=$((FAIL + 1))
    printf 'FAIL %s\n' "$1"
    [ -n "$FAILLOG" ] || FAILLOG="${TMPROOT:-/tmp}/failures.log"
    { printf 'FAIL %s\n' "$1"; shift || true; for line in "$@"; do printf '     %s\n' "$line"; done; } >>"$FAILLOG"
}

# 断言: 字符串相等
eq() { # eq <名称> <期望> <实得>
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "期望: $2" "实得: $3"; fi
}

# 断言: 期望(参数 2)包含于实得(参数 3)
contains() { # contains <名称> <片段> <全文>
    case "$3" in
        *"$2"*) ok "$1" ;;
        *) bad "$1" "期望包含: $2" "实得: $(printf '%s' "$3" | head -c 300)" ;;
    esac
}

# 断言: 某文件里出现某模式 >=1 次
grep_at_least() { # grep_at_least <名称> <模式> <文件>
    local n
    n="$(grep -cE -- "$2" "$3" 2>/dev/null || true)"
    if [ "${n:-0}" -ge 1 ]; then ok "$1"; else bad "$1" "在 $3 里找不到模式: $2"; fi
}

grep_none() { # grep_none <名称> <模式> <文件...>
    local label="$1" pat="$2"; shift 2
    local n
    n="$(grep -cE -- "$pat" "$@" 2>/dev/null | awk -F: '{s+=$NF} END{print s+0}')"
    if [ "${n:-0}" -eq 0 ]; then ok "$label"; else bad "$label" "不该出现的模式命中 $n 次: $pat"; fi
}

# ------------------------------------------------------------------ 工作区
new_work() { mktemp -d "$TMPROOT/work.XXXXXX"; }

# 把夹具复制成一条用例的私有工作区
cp_fx() { # cp_fx <夹具名> [目标父目录]
    local name="$1" parent="${2:-$TMPROOT}"
    local w
    w="$(mktemp -d "$parent/run.XXXXXX")"
    cp -r "$TESTS_DIR/fx/$name/." "$w/"
    printf '%s' "$w"
}

# ------------------------------------------------------------------ 跑 brickie
OUT=""
ERR=""
RC=0

# run_brickie <工作区> [args...]: 在工作区里跑前端; RC/OUT/ERR 为结果。
# 默认走源码树前端(python3 -m brickie); BRICKIE_LAUNCHER 非空时改走入口 ELF。
run_brickie() {
    local work="$1"; shift
    OUT="$TMPROOT/.last.out"
    ERR="$TMPROOT/.last.err"
    local launcher="${BRICKIE_LAUNCHER:-}"
    if [ -n "$launcher" ]; then
        ( cd "$work" && BRICKIE_CORE="$CORE" BRICKIE_GEN="$GEN" \
            "$launcher" "$@" ) >"$OUT" 2>"$ERR"
    else
        ( cd "$work" && PYTHONPATH="$TOOL_ROOT/python" BRICKIE_CORE="$CORE" \
            BRICKIE_GEN="$GEN" BRICKIE_TOOL_ROOT="$TOOL_ROOT" \
            PYTHONDONTWRITEBYTECODE=1 "$PY" -m brickie "$@" ) >"$OUT" 2>"$ERR"
    fi
    RC=$?
    [ "${VERBOSE:-0}" = "1" ] && printf '  $ brickie %s (rc=%s)\n' "$*" "$RC" >&2
    return 0
}


# ------------------------------------------------------------------ JSON 断言
JSONQ="$TESTS_DIR/jsonq.py"
JQ_RESP=""

# run_json <工作区> [args...]: 跑一次 --json 并把响应留在 JQ_RESP。
# 为了让用例能用**同一条** grep 走两种模式, 这里把 JSON 里的诊断投影成文本写进 ERR
# (JSON 模式下进程的 stderr 本来就是空的; 诊断在信封里)。
run_json() {
    local work="$1"; shift
    run_brickie "$work" "$@" --json
    JQ_RESP="$TMPROOT/.last.json"
    cp "$OUT" "$JQ_RESP"
    "$PY" "$JSONQ" text "$JQ_RESP" >"$ERR"
}

jqget() { "$PY" "$JSONQ" get "$@"; }

eqj() { # eqj <名称> <响应文件> <路径> <期望>
    eq "$1" "$4" "$("$PY" "$JSONQ" get "$2" "$3")"
}

eqjc() { # eqjc <名称> <响应文件> <路径> <数量>
    eq "$1" "$4" "$("$PY" "$JSONQ" count "$2" "$3")"
}

hasj() { # hasj <名称> <响应文件> <路径>
    eq "$1" "yes" "$("$PY" "$JSONQ" has "$2" "$3")"
}

keysj() { # keysj <名称> <响应文件> <路径> <键集合(逗号分隔, 已排序)>
    eq "$1" "$4" "$("$PY" "$JSONQ" keys "$2" "$3")"
}

sortedj() { # 诊断数组按 (code,file,span,target) 有序
    eq "$1" "yes" "$("$PY" "$JSONQ" sorted "$2")"
}

snapj() { # snapj <快照名> <响应文件> —— 结构化快照(N-4)
    # BRICKIE_SNAP_EMIT=1 时改为**重生成**基线(只在明确要更新协议快照时用)。
    local name="$1" resp="$2"
    local detail
    if [ "${BRICKIE_SNAP_EMIT:-0}" = "1" ]; then
        if detail="$("$PY" "$JSONQ" snapshot --emit "$name" "$resp" 2>&1)"; then
            ok "快照 $name(重生成)"
        else
            bad "快照 $name(重生成失败)" "$detail"
        fi
        return
    fi
    if detail="$("$PY" "$JSONQ" snapshot "$name" "$resp")"; then
        ok "快照 $name"
    else
        bad "快照 $name" "$detail"
    fi
}

# ------------------------------------------------------------------ 文件级工具
tree_hash() { # 目录内容指纹(路径 + sha256), 用来断言逐字节幂等
    ( cd "$1" && find . -type f -not -name out -not -name err | LC_ALL=C sort \
        | xargs -r sha256sum )
}

# ------------------------------------------------------------------ 段落标题
section() { printf '\n=== %s ===\n' "$1"; }
