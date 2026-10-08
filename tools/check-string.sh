#!/usr/bin/env bash
#
# brickOS prototype — 编译器支持例程的**自递归**门禁(core/src/string.c)
#
# 为什么值得一条门禁:
#   `core/src/string.c` 里那对定长 8 的 `__builtin_memcpy` 曾被 GCC 降成对 **memcpy
#   自己**的 libcall ⇒ 无限递归 ⇒ 栈无界增长压穿 64 KiB 启动栈、异常帧落进 .bss 的
#   页表 ⇒ 表现为"开机即翻译 fault, 且原始 fault 现场丢失"。
#   这类错误"编得过、宿主也跑得对"(宿主链的是 libc), **只有反汇编看得见** ——
#   所以钉成机械判据: 四个例程内部**不得有对自身的 bl**。
#   (判据只禁自递归, 不禁 memmove→memcpy 这类正当调用。)
#
# 用法: bash tools/check-string.sh [<obj>] [<objdump>]
#   缺省 <obj>     = build/obj/core/src/string.o
#                    (口径 = product.toml 的 [build].obj_dir + 源相对路径 + `.o`,
#                     与 brickie 的 `<obj_dir>/<src>.o` 约定同构; 文件不在 ⇒ 报红)
#   缺省 <objdump> = aarch64-linux-gnu-objdump → 带版本号的变体 → objdump
# 退出码: 0 = 一致, 1 = 有自递归, 2 = 用法/环境错

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

OBJ="${1:-build/obj/core/src/string.o}"

if [ ! -f "$OBJ" ]; then
    echo "FAIL: 找不到 $OBJ(先 `brickie build`; 对象落点见 product.toml 的 [build].obj_dir)" >&2
    exit 2
fi

OBJDUMP="${2:-}"
if [ -z "$OBJDUMP" ]; then
    for c in aarch64-linux-gnu-objdump aarch64-linux-gnu-objdump-16 \
             aarch64-linux-gnu-objdump-15 aarch64-linux-gnu-objdump-14 objdump; do
        if command -v "$c" >/dev/null 2>&1; then OBJDUMP="$c"; break; fi
    done
fi
if [ -z "$OBJDUMP" ]; then
    echo "FAIL: 找不到 objdump(aarch64 binutils)" >&2
    exit 2
fi

bad=0
for f in memcpy memmove memset memcmp; do
    n=$("$OBJDUMP" -d --no-show-raw-insn "$OBJ" \
        | awk -v f="<$f>:" 'index($0,f){inb=1;next} /^[0-9a-f]+ </{inb=0} inb' \
        | grep -c "bl.*<$f>")
    if [ "$n" != "0" ]; then
        echo "FAIL: $f 内部有 $n 条对自身的 bl(自递归 libcall)"
        bad=1
    fi
done

if [ "$bad" -ne 0 ]; then
    echo "--- memcpy 反汇编 ---"
    "$OBJDUMP" -d --no-show-raw-insn "$OBJ" | sed -n '/<memcpy>:/,/^$/p'
    exit 1
fi

echo "ok   编译器支持例程无自递归(memcpy/memmove/memset/memcmp 内部 0 条自调用 bl)"
exit 0
