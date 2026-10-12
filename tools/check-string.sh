#!/usr/bin/env bash
#
# brickOS prototype — the **self-recursion** gate for compiler support routines (core/src/string.c)
#
# Why this gate is worth having: in `core/src/string.c`, the pair of fixed-8 `__builtin_memcpy`
#   calls was once lowered by GCC into a libcall to **memcpy itself** ⇒ infinite recursion ⇒ the
#   stack grew without bound, punched through the 64 KiB boot stack, and exception frames landed
#   in the .bss page tables ⇒ the symptom was "translation fault at boot, with the original fault
#   context lost". Such bugs "compile fine and even run fine on the host" (the host links libc);
#   **only the disassembly shows them** -- hence a mechanical criterion: none of the four routines
#   may contain a `bl` to itself. (The criterion bans self-recursion only, not legitimate calls
#   like memmove→memcpy.)
#
# Usage: bash tools/check-string.sh [<obj>] [<objdump>]
#   default <obj>     = build/obj/core/src/string.o
#                     (convention = product.toml [build].obj_dir + source-relative path + `.o`,
#                      same shape as brickie's `<obj_dir>/<src>.o`; a missing file ⇒ red)
#   default <objdump> = aarch64-linux-gnu-objdump → versioned variants → objdump
# Exit code: 0 = consistent, 1 = self-recursion present, 2 = usage/environment error

set -uo pipefail

cd "$(dirname "$0")/.." || exit 2

OBJ="${1:-build/obj/core/src/string.o}"

if [ ! -f "$OBJ" ]; then
    echo "FAIL: cannot find $OBJ (run `brickie build` first; see product.toml [build].obj_dir for the object location)" >&2
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
    echo "FAIL: cannot find objdump (aarch64 binutils)" >&2
    exit 2
fi

bad=0
for f in memcpy memmove memset memcmp; do
    n=$("$OBJDUMP" -d --no-show-raw-insn "$OBJ" \
        | awk -v f="<$f>:" 'index($0,f){inb=1;next} /^[0-9a-f]+ </{inb=0} inb' \
        | grep -c "bl.*<$f>")
    if [ "$n" != "0" ]; then
        echo "FAIL: $f contains $n bl instructions to itself (self-recursive libcall)"
        bad=1
    fi
done

if [ "$bad" -ne 0 ]; then
    echo "--- memcpy disassembly ---"
    "$OBJDUMP" -d --no-show-raw-insn "$OBJ" | sed -n '/<memcpy>:/,/^$/p'
    exit 1
fi

echo "ok   compiler support routines have no self-recursion (memcpy/memmove/memset/memcmp: 0 self-calls)"
exit 0
