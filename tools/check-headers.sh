#!/usr/bin/env bash
#
# brickOS prototype — 对外头文件的**自洽**门禁
#
# 为什么值得一条门禁:
#   注释里误写 `*/` 会**提前闭合注释**。最常见的来源是路径通配符 ——
#   `core/src/sched/*.c`、`platform/*/src/link.ld`、`core/src/sync/**` 里都含 `*/`。
#   这个坑在本仓已经踩过三次(TC-MEM-*/TC-MM-*、`core/src/sched/*.c`、`platform/*/src/...`)。
#   它"编得过"的假象来自: **没有 .c 包含的头文件根本不会被编译到** —— 于是错误要等到
#   第一个使用者出现才炸, 而那时现场已经离原因很远。
#
# 判据(两条, 都是机械的):
#   ① 每个 `core/include/br/core/*.h` 都能**单独** include 且 `-fsyntax-only` 通过
#      (单独 include 才能暴露"缺前置包含");
#   ② 全部头文件**一起** include 也能通过(暴露宏/类型冲突)。
#
# 用法: bash tools/check-headers.sh [<cc>]
# 退出码: 0 = 全过, 1 = 有头文件不自洽, 2 = 环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

CC="${1:-}"
if [ -z "$CC" ]; then
    for c in aarch64-linux-gnu-gcc aarch64-linux-gnu-gcc-16 aarch64-linux-gnu-gcc-15 cc gcc clang; do
        if command -v "$c" >/dev/null 2>&1; then CC="$c"; break; fi
    done
fi
if [ -z "$CC" ]; then
    echo "FAIL: 找不到任何 C 编译器(交叉或宿主)" >&2
    exit 2
fi

HDR_DIR="core/include/br/core"
[ -d "$HDR_DIR" ] || { echo "FAIL: 找不到 $HDR_DIR" >&2; exit 2; }

status=0
single=0
for h in "$HDR_DIR"/*.h; do
    rel="br/core/$(basename "$h")"
    if ! printf '#include <%s>\n' "$rel" | "$CC" -std=c11 -fsyntax-only -xc - -Icore/include 2>/tmp/brhdr.err; then
        echo "FAIL 单独 include 失败: $rel"
        sed 's/^/     | /' /tmp/brhdr.err | head -6
        status=1
    fi
    single=$((single + 1))
done

# ② 全体一起(两次 include 同一个头也要幂等 —— 头保护宏的用途)
{
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    for h in "$HDR_DIR"/*.h; do printf '#include <br/core/%s>\n' "$(basename "$h")"; done
    echo 'int br_check_headers_tu;'
} > /tmp/brhdr_all.c
if ! "$CC" -std=c11 -fsyntax-only -Icore/include /tmp/brhdr_all.c 2>/tmp/brhdr.err; then
    echo "FAIL 全体一起 include 失败(或二次 include 不幂等)"
    sed 's/^/     | /' /tmp/brhdr.err | head -10
    status=1
fi

if [ "$status" -ne 0 ]; then
    exit 1
fi
echo "ok   $single 个对外头: 单独 include + 全体一起 include + 二次 include 均通过($CC)"
exit 0
