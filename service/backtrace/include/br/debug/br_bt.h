/*
 * service/backtrace — stack backtrace capture service (public declaration surface)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §3 (mini ramdump "capture set: register set ... +
 * stack backtrace"; "host tool: offline analysis (thread timeline trace, fault decode, **stack backtrace**)").
 * So this plugin's **responsibility boundary** is the one drawn by 5-01:
 *   - target side: **capture** (walk the frame-pointer chain, get a pc/fp/lr triple sequence) — only what is provably doable;
 *   - host side: **symbolication** (addr2line/llvm-symbolizer turns pc into function+offset), offline.
 * The prototype does not embed a symbol table in the image (that needs two-pass linking, an optimization after v1.x "offline decoding").
 *
 * Dependency: the build must use `-fno-omit-frame-pointer` (else the x29 chain breaks) — already in
 * the Makefile CFLAGS and called out in the README (the plugin's only build-time precondition).
 *
 * ISR safety: all `br_bt_*` are **thread-only**; the fault-site capture entry takes fp/pc from the
 * exception frame (passed in by the platform fault dispatch path) and does not re-trigger an exception.
 */
#ifndef BR_DEBUG_BR_BT_H
#define BR_DEBUG_BR_BT_H

#include <br/core/br_types.h>

#define BR_BT_MAX_FRAMES   16u   /* max frames per capture (static buffer; no allocation) */
#define BR_BT_MAX_DEPTH    32u   /* absolute depth guard for the fp chain (cycle prevention) */

/* one frame: pc is the "next address to execute" (= return address of the call site) */
typedef struct {
    br_uintptr_t pc;
    br_uintptr_t fp;
    br_uintptr_t sp;      /* derived from fp (canonical slot in the frame); not derivable ⇒ 0 */
    br_u32       depth;
} br_bt_frame_t;

/* LATE-phase init: clears the capture snapshot and returns 0. **Does not touch stack bounds**
 * (bounds are set on demand by `br_bt_set_stack_bounds` after init; this prototype has no legal
 * caller, see below). (It also registers no cross-plugin trace event name: the declaration surface
 * has no backtrace → service/trace `[[dep]]` edge, see this plugin's README "declaration-surface debt".) */
int br_bt_init(void);

/* The selftest entry (TC-DBG-01x) moved to `src/backtrace_selftest.c` (ADR-0010): the generated
 * artifact emits `.selftest = backtrace_selftest` per symbol_prefix, driven by core after all starts.
 * ★ The test entry does **not** enter this plugin's golden interface surface — otherwise editing one
 *   test case would be a public interface change (the interface hash covers exactly the declaration
 *   surface), conflicting with the "selftests separate from production" ruling. */

/*
 * Declare the stack range (**optional**; lets the walk decide "fp is out of bounds" and stop when the
 * chain is corrupted instead of walking off). Unset ⇒ only the three guards `BR_BT_MAX_DEPTH`,
 * "fp must be 16-byte aligned", and "strictly increasing" (plus the conservative fallback
 * "lower bound = walker's SP").
 *
 * ★ **Caller and timing** (division of labor with `br_bt_init`, the two do not conflict):
 *   - `br_bt_init` (LATE phase) **does not touch stack bounds** — it only clears the capture
 *     snapshot; otherwise it would erase bounds set earlier;
 *   - this function may only be called **after** `br_bt_init` (calling before is meaningless: init clears state).
 *   - **No caller in this prototype**: stack bounds are a platform fact (`__stack_bottom/__stack_top`),
 *     and "platform → ability plugin" is an **illegal dependency edge** in design §7.3 (platform must
 *     not depend on ability) ⇒ to avoid an undeclared cross-layer call, bounds stay empty and the walk
 *     uses the conservative fallback. This interface is left to v1.x: the legal source of bound info is
 *     the "service/interface layer" or future core stack metadata (same place as stack guard vx.0).
 */
int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

/* Capture from the **current** context. Returns frames written (>=1); out==NULL/max==0 ⇒ 0. */
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max);

/*
 * Capture from a given site (exception frame / known fp). `pc` becomes frame 0's pc, `fp` the start.
 * Returns the frame count. Guards: fp == 0 / not 16-aligned / outside the stack range / non-monotonic / too deep ⇒ stop.
 */
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max);

/* Capture and print: `[BT] 0: pc=0x<hex> fp=0x<hex>` one line per frame + `[BT] frames=<n>`. Returns the frame count. */
br_u32 br_bt_print(void);
br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc);

/* Frame count of the last capture (observation; lets dump reuse the same snapshot without re-walking). */
br_u32 br_bt_last_count(void);
const br_bt_frame_t *br_bt_last(void);

#endif /* BR_DEBUG_BR_BT_H */
