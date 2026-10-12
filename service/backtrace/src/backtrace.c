/*
 * service/backtrace — stack backtrace capture service (implementation)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §3:
 *   "capture set: register set, full TCB set + **per-thread stacks**, trace ring …" and
 *   "host tool: offline analysis (thread timing against trace, fault decode, **stack backtrace**)".
 * Responsibility boundary: **target only captures, host symbolizes**
 *   (addr2line/llvm-symbolizer turns pc into function+offset) — this file neither carries nor
 *   generates a symbol table (that needs two-pass linking, a v1.x "offline decode"
 *   optimization); see README for the ownership boundary.
 *
 * Build-time precondition: the whole image is compiled with `-fno-omit-frame-pointer`
 *   (Makefile CFLAGS) — otherwise -O2 drops x29 as an ordinary register and the
 *   chain breaks at frame 0. This is the plugin's only build-time precondition.
 *
 * aarch64 frame chain convention (AAPCS64):
 *   prologue `stp x29, x30, [sp, #-16]!; mov x29, sp` ⇒ x29 points at the save area,
 *   `[x29]` = caller's x29 (previous frame fp), `[x29 + 8]` = return address (next pc).
 *   Stack grows down ⇒ fp walking up the chain is **strictly increasing**
 *   (also the main loop guard).
 *
 * ISR safety: `br_bt_*` are all **thread-only**; the fault-site capture entry takes
 *   fp/pc from the exception frame (passed by the platform fault dispatch path) and
 *   never raises an exception itself.
 */
#include <br/debug/br_bt.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include "backtrace_internal.h"    /* internal view for self-test (see that header; unused in production) */

/* Conservative fallback window when bounds are unknown: lower bound = walker's current SP,
 * upper bound = SP + 1 MiB. The real boot stack is only tens of KiB (link.ld); 1 MiB covers
 * every caller frame while blocking the worst case of "fp smashed to a far wild value". */
#define BT_FALLBACK_STACK_BYTES  (1024u * 1024u)

/* =====================================================================
 * Internal state
 * ===================================================================== */

static br_uintptr_t s_stack_bottom;
static br_uintptr_t s_stack_top;     /* s_stack_top == 0 ⇒ bounds unset */

static br_bt_frame_t s_last[BR_BT_MAX_FRAMES];   /* last capture snapshot */
static br_u32        s_last_count;

/* =====================================================================
 * Helpers
 * ===================================================================== */

/* Stack range for this walk: prefer platform-set bounds; else fall back to
 * "current SP .. +1 MiB". The fallback lower bound is the walker's own SP — lower
 * than any walked caller frame (stack grows down), so it never kills a legit frame;
 * the cost is that the upper bound is only this fixed conservative window. */
static void bt_bounds(br_uintptr_t *lo, br_uintptr_t *hi)
{
    if (s_stack_top != 0u) {
        *lo = s_stack_bottom;
        *hi = s_stack_top;
        return;
    }

    br_uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    *lo = sp;
    *hi = sp + (br_uintptr_t)BT_FALLBACK_STACK_BYTES;
}

/* =====================================================================
 * Frame chain walk
 * ===================================================================== */

/*
 * Walk the fp chain from (fp, pc). Frame 0 = the passed (pc, fp).
 * Guards (all must pass before dereferencing fp; never read before check):
 *   ① fp != 0; ② fp 16-byte aligned (AAPCS64); ③ within the stack range;
 *   ④ strictly increasing (next_fp > fp; stop on loop/break); ⑤ depth < BR_BT_MAX_DEPTH;
 *   ⑥ frames < max.
 * Returns the number of frames written to out.
 */
static br_u32 bt_walk(br_uintptr_t fp, br_uintptr_t pc,
                      br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t lo;
    br_uintptr_t hi;
    bt_bounds(&lo, &hi);

    br_u32 depth = 0u;

    while ((depth < max) && (depth < BR_BT_MAX_DEPTH)) {
        /* ---- guards before dereference ---- */
        if (fp == 0u) {
            break;
        }
        if ((fp % 16u) != 0u) {
            break;
        }
        if ((fp < lo) || (fp >= hi)) {
            break;
        }
        if ((depth > 0u) && (fp <= out[depth - 1u].fp)) {
            break;   /* must increase strictly */
        }

        out[depth].pc    = pc;
        out[depth].fp    = fp;
        /* sp: fp + 16 is the "canonical position" in the stack frame (past the save
         * area), not the real SP — the real SP varies within the function body and
         * has no portable meaning. */
        out[depth].sp    = fp + 16u;
        out[depth].depth = depth;
        depth++;

        if ((depth >= max) || (depth >= BR_BT_MAX_DEPTH)) {
            break;
        }

        /* ---- read previous frame's fp and return address; volatile prevents compiler caching (stack may change) ---- */
        const volatile br_uintptr_t *frame = (const volatile br_uintptr_t *)fp;
        const br_uintptr_t next_fp = frame[0];
        const br_uintptr_t ret_pc  = frame[1];

        if (next_fp <= fp) {
            break;   /* chain must increase strictly, else it is a loop or smashed */
        }
        fp = next_fp;
        pc = ret_pc;
    }

    return depth;
}

/* Walk + store the static snapshot (reused by br_bt_last/dump to avoid re-walking) */
static br_u32 bt_snapshot(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    const br_u32 n = bt_walk(fp, pc, out, max);

    if (out != s_last) {
        const br_u32 k = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
        for (br_u32 i = 0u; i < k; i++) {
            s_last[i] = out[i];
        }
    }
    s_last_count = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
    return n;
}

/* =====================================================================
 * Public capture surface
 * ===================================================================== */

/* noinline is a **semantic requirement**, not an optimization hint: frame 0's fp must
 * be this function's own frame, else `mov x29` reads the caller's frame and the chain
 * loses a level (probe criterion of TC-DBG-010).
 * The header declaration does not carry the attribute, so it is only on the definition
 * (ABI unchanged). */
__attribute__((noinline))
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t fp;
    br_uintptr_t lr;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("mov %0, x30" : "=r"(lr));

    /* Frame 0's pc is lr = "next instruction after returning to the caller" — the same
     * convention as the other chain frames (which store return addresses), so no +/- 4
     * correction is needed. */
    return bt_snapshot(fp, lr, out, max);
}

__attribute__((noinline))
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }
    return bt_snapshot(fp, pc, out, max);
}

int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    if ((bottom == 0u) || (top <= bottom)) {
        return BR_ERR(EINVAL);
    }
    s_stack_bottom = bottom;
    s_stack_top    = top;
    return 0;
}

/* =====================================================================
 * Presentation
 * ===================================================================== */

static void bt_emit(const br_bt_frame_t *fr, br_u32 n)
{
    for (br_u32 i = 0u; i < n; i++) {
        br_log_info("[BT] %u: pc=0x%lx fp=0x%lx sp=0x%lx",
                    fr[i].depth, fr[i].pc, fr[i].fp, fr[i].sp);
    }
    br_log_info("[BT] frames=%u", n);
}

br_u32 br_bt_print(void)
{
    /* Write the static snapshot directly: capture already stores one, saving a 512 B intermediate buffer */
    const br_u32 n = br_bt_capture(s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc)
{
    const br_u32 n = br_bt_capture_from(fp, pc, s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_last_count(void)
{
    return s_last_count;
}

const br_bt_frame_t *br_bt_last(void)
{
    return s_last;
}

int br_bt_init(void)
{
    /* Clear the "last capture" snapshot. ★ Do not touch the stack bounds: the platform
     * sets them in early_init from linker-script symbols, and init is the LATE phase —
     * clearing them here would wipe that setup. */
    for (br_u32 i = 0u; i < BR_BT_MAX_FRAMES; i++) {
        s_last[i].pc    = 0u;
        s_last[i].fp    = 0u;
        s_last[i].sp    = 0u;
        s_last[i].depth = 0u;
    }
    s_last_count = 0u;

    /* No cross-plugin trace event name registration: the declaration surface has no
     * service/backtrace → service/trace [[dep]] edge (already noted in br_bt.h's init
     * comment); a cross-plugin call would introduce an undeclared dependency.
     *
     * ⚠ Stack bounds stay **unchanged**: br_bt.h's init comment says "clear state and
     *    reset the stack range", but set_stack_bounds' comment in the same header says
     *    platform calls it in **early_init**, and init is LATE — clearing here would wipe
     *    that setup. This implementation does not clear bounds (the self-consistent
     *    reading of the two comments); the wording conflict was reported upstream. */
    return 0;
}

/* =====================================================================
 * Internal view for self-test
 *
 * See `src/backtrace_internal.h`: these two exist **only** for `backtrace_selftest.c` —
 * a case narrows the stack bounds temporarily and **restores them exactly**, and
 * "restore to unset (0/0)" cannot be expressed via the public
 * `br_bt_set_stack_bounds` (it rejects invalid values). ★ They read/write the
 * **production state itself**, with no copy kept elsewhere, else the case would be
 * changing a copy's bounds.
 * ===================================================================== */

void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top)
{
    if (bottom != BR_NULL) {
        *bottom = s_stack_bottom;
    }
    if (top != BR_NULL) {
        *top = s_stack_top;
    }
}

void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    s_stack_bottom = bottom;
    s_stack_top    = top;
}


/* =====================================================================
 * Plugin lifecycle hooks (name = symbol_prefix(short) + phase; referenced by the
 * generated `build/gen/service/backtrace/plugin_desc.c`; design 1-01 §9 / 3-05 §2)
 * Self-declared prototypes satisfy -Wmissing-prototypes (hook names come from the
 * generator, not the public header or [[export]]).
 * ===================================================================== */
int backtrace_early_init(void);
int backtrace_init(void);
int backtrace_start(void);

/* EARLY phase: no action (the platform sets stack bounds in its early_init; this plugin does not touch them). */
int backtrace_early_init(void)
{
    return 0;
}

/* LATE phase (Service category ⇒ ② completion point): delegating to the existing init. */
int backtrace_init(void)
{
    return br_bt_init();
}

/* START phase: this service has nothing to do once interrupts/threads are available. */
int backtrace_start(void)
{
    return 0;
}

