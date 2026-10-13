#!/usr/bin/env bash
# brickOS prototype — self-recursion gate for compiler support routines (core/src/string.c).
# A `bl` to itself means a libcall lowered to the routine itself: unbounded recursion, a blown
# boot stack and lost fault context. Host builds hide it (libc), so check the disassembly: no
# memcpy/memmove/memset/memcmp self-call (legitimate calls like memmove->memcpy are allowed).
# Usage: bash tools/check-string.sh [<obj>] [<objdump>]; <obj> defaults to
#   build/obj/core/src/string.o (product.toml [build].obj_dir + source path + .o; absent => red);
#   <objdump> defaults to cross objdump -> versioned variants -> objdump.
#   Exit 0 consistent, 1 self-recursion present, 2 usage/environment error.

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
